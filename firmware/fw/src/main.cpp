#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <GxEPD2_BW.h>
#include <vector>
#include <WiFiManager.h>
#include <qrcode.h>
#include "epd_png.h"

const char* BASE_URL = "https://candlelight.daniloinfinite.workers.dev";
const char* LAMP_ID = "test-lamp-1";  // must match the lampId the sender page uploads to

// panel wiring (GDEY042T81, 400x300). SCK/MOSI are the board's default SPI pins
// (SCK = 18, MO = 23), which GxEPD2 uses automatically
const int EPD_BUSY = A3;  // GPIO35
const int EPD_RST = D12;  // GPIO4
const int EPD_DC = D7;    // GPIO13
const int EPD_CS = D6;    // GPIO14

GxEPD2_BW<GxEPD2_420_GDEY042T81, GxEPD2_420_GDEY042T81::HEIGHT> display(
    GxEPD2_420_GDEY042T81(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

const uint32_t STILL_HOLD_MS = 30000;  // how long each still stays up before the next image
const int ANIM_LOOPS = 3;              // times an animation plays through before moving on

const int MAX_IMAGES = 12;                // most images the device keeps at once
const char* MANIFEST_PATH = "/manifest.json";
const char* TMP_PATH = "/tmp.png";

// manifest.json is an array, oldest first, one entry per stored image:
//   { "id": itemId, "sentAt": ms epoch, "geo": { city, region, country, lat, lon, ... },
//     "frames": 1 for a still, "intervalMs": per-frame delay for animations }
// the PNG itself lives at "/" + itemId

String lampUrl(const String& rest) {
  return String(BASE_URL) + "/lamp/" + LAMP_ID + rest;
}

bool loadManifest(JsonDocument& doc) {
  doc.to<JsonArray>();
  File f = LittleFS.open(MANIFEST_PATH, "r");
  if (!f) return true;  // first boot, nothing stored yet
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err || !doc.is<JsonArray>()) {
    Serial.printf("Manifest unreadable (%s), starting empty\n", err.c_str());
    doc.to<JsonArray>();
  }
  return true;
}

bool saveManifest(JsonDocument& doc) {
  File f = LittleFS.open(MANIFEST_PATH, "w");
  if (!f) return false;
  serializeJson(doc, f);
  f.close();
  return true;
}

bool inManifest(JsonArray arr, const char* id) {
  for (JsonObject e : arr) {
    if (strcmp(e["id"] | "", id) == 0) return true;
  }
  return false;
}

// streams one queue item straight into flash; true only if the whole file arrived
bool downloadItem(WiFiClientSecure& client, const char* itemId, size_t expectedSize) {
  HTTPClient http;
  http.setTimeout(20000);
  http.begin(client, lampUrl(String("/items/") + itemId));
  int code = http.GET();
  if (code != 200) {
    Serial.printf("  download HTTP %d\n", code);
    http.end();
    return false;
  }
  File f = LittleFS.open(TMP_PATH, "w");
  if (!f) { http.end(); return false; }
  http.writeToStream(&f);
  size_t got = f.size();
  f.close();
  http.end();
  if (expectedSize && got != expectedSize) {
    Serial.printf("  size mismatch: got %u, expected %u\n", (unsigned)got, (unsigned)expectedSize);
    LittleFS.remove(TMP_PATH);
    return false;
  }
  return true;
}

// tells the worker the item is safely on the device so it frees the queue slot
bool ackItem(WiFiClientSecure& client, const char* itemId) {
  HTTPClient http;
  http.setTimeout(10000);
  http.begin(client, lampUrl(String("/items/") + itemId));
  int code = http.sendRequest("DELETE");
  http.end();
  return code == 200;
}

void syncQueue() {
  WiFiClientSecure client;
  client.setInsecure();  // prototype only

  // 1. what's waiting, oldest first, with sentAt/sentGeo for each
  HTTPClient http;
  http.setTimeout(10000);
  http.begin(client, lampUrl("/queue"));
  int code = http.GET();
  if (code != 200) {
    Serial.printf("Queue request failed: %d %s\n", code, http.errorToString(code).c_str());
    http.end();
    return;
  }
  String body = http.getString();
  http.end();

  JsonDocument queueDoc;
  if (deserializeJson(queueDoc, body)) {
    Serial.println("Queue JSON parse failed");
    return;
  }
  JsonArray queue = queueDoc["items"].as<JsonArray>();
  Serial.printf("Queue: %u item(s) waiting\n", (unsigned)queue.size());

  JsonDocument manifestDoc;
  loadManifest(manifestDoc);
  JsonArray stored = manifestDoc.as<JsonArray>();
  Serial.printf("Device: %u image(s) stored\n", (unsigned)stored.size());

  LittleFS.remove(TMP_PATH);  // leftover from an interrupted sync

  // 2. pull each one in order. every item is committed on its own (download ->
  // evict oldest if full -> manifest -> ack), so a dropped connection mid-sync
  // leaves a consistent device and the rest simply stay queued for next time
  for (JsonObject item : queue) {
    const char* itemId = item["itemId"] | "";
    if (!*itemId) continue;
    Serial.printf("- %s\n", itemId);

    // an earlier ack may have failed after the image was already saved
    if (inManifest(stored, itemId)) {
      Serial.println("  already stored, acking only");
      ackItem(client, itemId);
      continue;
    }

    if (!downloadItem(client, itemId, item["size"] | 0)) break;

    // make sure the e-ink chunks inside the PNG are intact before it takes a slot
    EpdImage img;
    File tf = LittleFS.open(TMP_PATH, "r");
    bool valid = tf && epdParse(tf, img);
    if (tf) tf.close();
    if (!valid) {
      // can never be shown, and leaving it queued would block everything behind it
      Serial.println("  no valid epRb/epRa payload, discarding");
      LittleFS.remove(TMP_PATH);
      ackItem(client, itemId);
      continue;
    }
    Serial.printf("  %ux%u, %u-bpp, %u frame(s)\n", img.w, img.h, img.bpp, img.frames);

    // device holds MAX_IMAGES at most: the oldest image makes room for this one
    if ((int)stored.size() >= MAX_IMAGES) {
      String oldId = stored[0]["id"] | "";
      LittleFS.remove("/" + oldId);
      stored.remove(0);
      Serial.printf("  evicted oldest: %s\n", oldId.c_str());
    }

    LittleFS.rename(TMP_PATH, "/" + String(itemId));
    JsonObject entry = stored.add<JsonObject>();
    entry["id"] = itemId;
    entry["sentAt"] = item["sentAt"];
    entry["geo"] = item["sentGeo"];
    entry["frames"] = img.frames;
    entry["intervalMs"] = img.intervalMs;
    if (!saveManifest(manifestDoc)) {
      Serial.println("  manifest write failed");
      break;
    }

    if (!ackItem(client, itemId)) Serial.println("  ack failed (will dedupe next sync)");
  }

  // 3. what the device ended up with, oldest -> newest
  Serial.printf("Stored now (%u/%d):\n", (unsigned)stored.size(), MAX_IMAGES);
  for (JsonObject e : stored) {
    Serial.printf("  %s  sentAt=%lld  %s, %s\n", e["id"].as<const char*>(),
                  e["sentAt"].as<long long>(),
                  e["geo"]["city"] | "?", e["geo"]["country"] | "?");
  }
}

// ---------- drawing ----------

uint8_t frameBuf[30000];  // one packed frame: 400x300 at 2 bpp is the largest (30000 bytes)

// stills carry 4 gray levels but this first pass drives the panel black/white
// only, so the two middle grays are halftoned with a 2x2 ordered pattern
bool pixelIsBlack(uint8_t code, uint8_t bpp, int x, int y) {
  if (bpp == 1) return code == 0;
  static const uint8_t BAYER[2][2] = {{0, 2}, {3, 1}};
  switch (code) {
    case 0: return true;
    case 1: return BAYER[y & 1][x & 1] >= 1;  // ~25% white
    case 2: return BAYER[y & 1][x & 1] >= 3;  // ~75% white
    default: return false;
  }
}

// draws one packed frame; partial = fast refresh without the full-screen flash
void drawFrame(const EpdImage& img, bool partial) {
  display.setRotation(img.w < img.h ? 1 : 0);  // portrait images (300x400) rotate the panel
  if (display.width() != img.w || display.height() != img.h) {
    Serial.printf("Image is %ux%u but panel is %dx%d, skipping\n", img.w, img.h, display.width(), display.height());
    return;
  }
  if (partial) display.setPartialWindow(0, 0, img.w, img.h);
  else display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    for (uint16_t y = 0; y < img.h; y++) {
      for (uint16_t x = 0; x < img.w; x++) {
        if (pixelIsBlack(epdPixel(frameBuf, img, x, y), img.bpp, x, y)) display.drawPixel(x, y, GxEPD_BLACK);
      }
    }
  } while (display.nextPage());
}

void showImage(const String& itemId) {
  File f = LittleFS.open("/" + itemId, "r");
  EpdImage img;
  if (!f || !epdParse(f, img)) {
    Serial.printf("Can't open %s\n", itemId.c_str());
    if (f) f.close();
    return;
  }
  Serial.printf("Showing %s (%u frame(s))\n", itemId.c_str(), img.frames);

  if (img.frames == 1) {
    if (epdReadFrame(f, img, 0, frameBuf)) drawFrame(img, false);
    f.close();
    delay(STILL_HOLD_MS);
    return;
  }

  // animation: first frame does a full refresh to clear ghosting, the rest are partial
  bool first = true;
  for (int loop = 0; loop < ANIM_LOOPS; loop++) {
    for (uint8_t i = 0; i < img.frames; i++) {
      uint32_t t0 = millis();
      if (!epdReadFrame(f, img, i, frameBuf)) break;
      drawFrame(img, !first);
      first = false;
      uint32_t spent = millis() - t0;
      if (spent < img.intervalMs) delay(img.intervalMs - spent);
    }
  }
  f.close();
}

std::vector<String> storedIds;  // oldest first, same order as the manifest

void loadStoredIds() {
  storedIds.clear();
  JsonDocument doc;
  loadManifest(doc);
  for (JsonObject e : doc.as<JsonArray>()) storedIds.push_back(e["id"].as<String>());
}

// ---------- WiFi setup (captive portal) ----------

const int RESET_BUTTON = D5;                 // GPIO0, the BOOT button
const uint32_t RESET_HOLD_MS = 5000;         // hold this long to forget the saved WiFi
const uint32_t PORTAL_TIMEOUT_S = 180;       // setup mode stays open this long, then retries the saved network

String setupApName() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char name[24];
  snprintf(name, sizeof(name), "Candlelight-%02X%02X", mac[4], mac[5]);
  return String(name);
}

// instructions + a QR code that joins the setup network when scanned
void drawSetupScreen(const String& apName) {
  QRCode qr;
  uint8_t qrData[qrcode_getBufferSize(3)];
  String payload = "WIFI:S:" + apName + ";T:nopass;;";
  qrcode_initText(&qr, qrData, 3, ECC_LOW, payload.c_str());
  const int scale = 6, qx = 20, qy = 50;

  display.setRotation(0);
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);

    display.setTextSize(3);
    display.setCursor(20, 10);
    display.print("Candlelight setup");

    for (int y = 0; y < qr.size; y++)
      for (int x = 0; x < qr.size; x++)
        if (qrcode_getModule(&qr, x, y)) display.fillRect(qx + x * scale, qy + y * scale, scale, scale, GxEPD_BLACK);

    display.setTextSize(2);
    static const char* LINES[][2] = {
      {"60", "1. Scan this"}, {"80", "   code with"}, {"100", "   your phone"},
      {"140", "2. Pick your"}, {"160", "   WiFi (2.4"}, {"180", "   GHz) and"},
      {"200", "   enter its"}, {"220", "   password"},
    };
    for (auto& l : LINES) {
      display.setCursor(215, atoi(l[0]));
      display.print(l[1]);
    }
    display.setCursor(50, 250);
    display.print("Network: " + apName);
  } while (display.nextPage());
}

// long-press BOOT at any time (after the board has started: holding it
// during power-up would put the chip in flash-download mode instead) to
// forget the saved WiFi and restart into setup mode
void buttonTask(void*) {
  pinMode(RESET_BUTTON, INPUT_PULLUP);
  uint32_t heldSince = 0;
  for (;;) {
    if (digitalRead(RESET_BUTTON) == LOW) {
      if (!heldSince) heldSince = millis();
      if (millis() - heldSince >= RESET_HOLD_MS) {
        Serial.println("BOOT held: forgetting saved WiFi");
        WiFi.mode(WIFI_STA);
        WiFi.disconnect(true, true);  // erase stored credentials
        delay(200);
        ESP.restart();
      }
    } else {
      heldSince = 0;
    }
    delay(50);
  }
}

// blocks until the board is on WiFi. With saved credentials it just joins;
// with none, or if they stop working, it opens the Candlelight-XXXX setup
// network (and keeps the saved credentials, so a router that is only
// temporarily down gets rejoined after the portal times out and retries)
void connectWiFi() {
  String apName = setupApName();
  WiFiManager wm;
  wm.setConnectTimeout(20);
  wm.setConfigPortalTimeout(PORTAL_TIMEOUT_S);
  wm.setTitle("Candlelight");
  bool screenDrawn = false;
  wm.setAPCallback([&](WiFiManager* w) {
    Serial.println("Setup mode: join " + apName);
    // only take over the screen on a fresh lamp; one that merely lost its
    // network keeps showing its last image while it retries
    if (!screenDrawn && !w->getWiFiIsSaved()) {
      drawSetupScreen(apName);
      screenDrawn = true;
    }
  });
  while (!wm.autoConnect(apName.c_str())) {
    Serial.println("No WiFi yet, retrying");
  }
  Serial.println("Connected: " + WiFi.localIP().toString());
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  if (!LittleFS.begin(true)) {  // true = format on first boot
    Serial.println("LittleFS mount failed");
    return;
  }
  display.init(115200);
  xTaskCreate(buttonTask, "button", 4096, nullptr, 1, nullptr);

  connectWiFi();
  syncQueue();
  loadStoredIds();
}

void loop() {
  // cycles through everything stored, oldest to newest
  if (storedIds.empty()) {
    Serial.println("Nothing stored to show");
    delay(5000);
    return;
  }
  for (const String& id : storedIds) showImage(id);
}
