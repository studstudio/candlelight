#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <GxEPD2_BW.h>
#include <vector>
#include <WiFiManager.h>
#include <esp_wifi.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include <qrcode.h>
#include "epd_png.h"
#include "ota_guard.h"

const char* BASE_URL = "https://candlelight.daniloinfinite.workers.dev";
// stamped in by CI (-DFW_VERSION="<git sha>"); an unstamped local build is "dev"
#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif

const char* LAMP_ID = "test-lamp-1";  // must match the lampId the sender page uploads to

// panel wiring (GDEY042T81, 400x300). SCK/MOSI are the board's default SPI pins
// (SCK = 18, MO = 23), which GxEPD2 uses automatically
const int EPD_BUSY = A3;  // GPIO35
const int EPD_RST = D12;  // GPIO4
const int EPD_DC = D7;    // GPIO13
const int EPD_CS = D6;    // GPIO14

GxEPD2_BW<GxEPD2_420_GDEY042T81, GxEPD2_420_GDEY042T81::HEIGHT> display(
    GxEPD2_420_GDEY042T81(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

const uint32_t STILL_HOLD_MS = 1000;   // how long each image stays up during the automatic pass
const int ANIM_LOOPS = 1;              // times an animation plays through before moving on

// ---- sleep schedule ----
const uint32_t SYNC_INTERVAL_S = 600;                        // deep-sleep time between worker syncs (10 min while prototyping)
const uint32_t WIFI_TIMEOUT_MS = 20000;                      // give up on WiFi after this and go back to sleep
const uint32_t AWAKE_IDLE_MS = 15000;                        // stay up this long after the last button press or pass
const uint32_t FW_CHECK_EVERY_WAKES = 86400 / SYNC_INTERVAL_S;  // firmware update check about once a day
const uint32_t OTA_BUDGET_MS = 90000;                        // a firmware download that isn't finished by now is abandoned (retried next check)
const uint32_t OTA_STALL_MS = 15000;                         // ...and so is one that stops delivering data for this long
const bool FULL_REFRESH_ON_WAKE = false;                     // set true if partial refreshes after deep sleep ghost or glitch

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

// streams one queue item straight into flash; true only if the whole file arrived.
// every attempt gets its own fresh TLS connection (a connection left over from
// an earlier request can hand back an empty body), and a short or failed
// transfer is retried
bool downloadItem(const char* itemId, size_t expectedSize) {
  for (int attempt = 1; attempt <= 3; attempt++) {
    otaguard::tick();
    WiFiClientSecure client;
    client.setInsecure();  // prototype only
    HTTPClient http;
    http.setReuse(false);
    http.setTimeout(20000);
    http.begin(client, lampUrl(String("/items/") + itemId));
    int code = http.GET();
    if (code != 200) {
      Serial.printf("  attempt %d: HTTP %d\n", attempt, code);
      http.end();
      continue;
    }
    int contentLen = http.getSize();  // -1 when the server streams it chunked
    File f = LittleFS.open(TMP_PATH, "w");
    if (!f) { http.end(); return false; }
    int written = http.writeToStream(&f);  // bytes written, or a negative HTTPClient error
    f.close();
    http.end();

    File check = LittleFS.open(TMP_PATH, "r");  // measure the closed file, not the open handle
    size_t got = check ? check.size() : 0;
    if (check) check.close();

    if (!expectedSize || got == expectedSize) return true;
    Serial.printf("  attempt %d: got %u of %u bytes (content-length %d, writeToStream %d)\n",
                  attempt, (unsigned)got, (unsigned)expectedSize, contentLen, written);
    LittleFS.remove(TMP_PATH);
    delay(500);
  }
  return false;
}

// tells the worker the item is safely on the device so it frees the queue slot
bool ackItem(const char* itemId) {
  WiFiClientSecure client;
  client.setInsecure();  // prototype only
  HTTPClient http;
  http.setReuse(false);
  http.setTimeout(10000);
  http.begin(client, lampUrl(String("/items/") + itemId));
  int code = http.sendRequest("DELETE");
  http.end();
  return code == 200;
}

// prints an item's sentAt (epoch ms, shown as UTC too) and every sentGeo field
void printSentInfo(JsonVariantConst sentAt, JsonVariantConst geo) {
  if (sentAt.isNull()) {
    Serial.println("    sentAt:  (none, queued before sentAt was recorded)");
  } else {
    long long ms = sentAt.as<long long>();
    time_t secs = (time_t)(ms / 1000);
    struct tm tm;
    gmtime_r(&secs, &tm);
    char when[24];
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);
    Serial.printf("    sentAt:  %lld  (%s UTC)\n", ms, when);
  }
  if (geo.isNull()) {
    Serial.println("    sentGeo: (none)");
    return;
  }
  Serial.printf("    sentGeo: city=%s region=%s country=%s postal=%s\n",
                geo["city"] | "-", geo["region"] | "-", geo["country"] | "-", geo["postalCode"] | "-");
  if (geo["lat"].isNull() || geo["lon"].isNull()) {
    Serial.printf("             lat/lon=- tz=%s colo=%s\n", geo["timezone"] | "-", geo["colo"] | "-");
  } else {
    Serial.printf("             lat=%.4f lon=%.4f tz=%s colo=%s\n", geo["lat"].as<double>(),
                  geo["lon"].as<double>(), geo["timezone"] | "-", geo["colo"] | "-");
  }
}

// streams the firmware into the next OTA slot, giving up after OTA_BUDGET_MS in
// total or OTA_STALL_MS without data. A stalled connection must never keep the
// radio on for minutes or trip the watchdog (which would count as a crash).
// true only if the whole image arrived, verified and the new slot is selected
bool downloadFirmware(WiFiClientSecure& client, const String& url) {
  HTTPClient http;
  http.setReuse(false);
  http.setTimeout(10000);
  http.begin(client, url);
  int code = http.GET();
  int len = http.getSize();
  if (code != 200 || len <= 0) {
    Serial.printf("  firmware download: HTTP %d, length %d\n", code, len);
    http.end();
    return false;
  }
  if (!Update.begin(len)) {
    Serial.printf("  firmware download: no room for %d bytes\n", len);
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  uint8_t buf[1460];
  int written = 0;
  uint32_t t0 = millis(), lastData = millis();
  const char* failure = nullptr;
  while (written < len && !failure) {
    otaguard::tick();
    if (millis() - t0 > OTA_BUDGET_MS) {
      failure = "took too long";
    } else if (millis() - lastData > OTA_STALL_MS) {
      failure = "stalled";
    } else if (int avail = stream->available()) {
      int n = stream->readBytes(buf, min(avail, (int)sizeof(buf)));
      if (n > 0) {
        if (Update.write(buf, n) != (size_t)n) failure = "flash write failed";
        written += n;
        lastData = millis();
      }
    } else if (!stream->connected()) {
      failure = "connection closed early";
    } else {
      delay(5);
    }
  }
  http.end();
  if (!failure && !Update.end(true)) failure = "image rejected";  // checks the image and selects the new slot
  if (failure) {
    Serial.printf("  firmware download %s after %u of %d bytes (%u s)\n", failure, (unsigned)written, len,
                  (unsigned)((millis() - t0) / 1000));
    Update.abort();
    return false;
  }
  Serial.printf("  firmware downloaded in %u s\n", (unsigned)((millis() - t0) / 1000));
  return true;
}

// asks the worker which firmware is current; if it isn't the one running, pulls
// it into the spare OTA slot and reboots into it (never returns on success)
void checkForUpdate() {
  Serial.printf("Firmware: %s\n", FW_VERSION);

  WiFiClientSecure client;
  client.setInsecure();  // prototype only
  HTTPClient http;
  http.setReuse(false);
  http.setTimeout(10000);
  http.begin(client, String(BASE_URL) + "/firmware/version");
  int code = http.GET();
  if (code != 200) {
    Serial.printf("Update check: HTTP %d\n", code);
    http.end();
    return;
  }
  String body = http.getString();
  http.end();
  client.stop();

  JsonDocument doc;
  if (deserializeJson(doc, body)) return;
  String latest = doc["version"] | "";
  if (latest.isEmpty() || latest == FW_VERSION) {
    Serial.println("Firmware is up to date");
    return;
  }

  if (latest == otaguard::badVersion()) {
    Serial.printf("Firmware %s failed on this lamp before, skipping (hold NEXT 10s to retry it)\n", latest.c_str());
    return;
  }

  Serial.printf("Updating firmware %s -> %s (%u bytes)\n", FW_VERSION, latest.c_str(), (unsigned)(doc["size"] | 0));
  WiFiClientSecure updateClient;
  updateClient.setInsecure();  // prototype only
  otaguard::tick();
  otaguard::setTrying(latest);  // lets the next boot blocklist it if the bootloader rolls it back
  if (downloadFirmware(updateClient, String(BASE_URL) + "/firmware/latest.bin")) {
    Serial.println("Update installed, rebooting into it");
    Serial.flush();
    delay(200);
    ESP.restart();
  }
  otaguard::clearTrying();  // nothing was installed
}

// returns how many new images were stored
int syncQueue() {
  // 1. what's waiting, oldest first, with sentAt/sentGeo for each
  WiFiClientSecure queueClient;
  queueClient.setInsecure();  // prototype only
  HTTPClient http;
  http.setReuse(false);
  http.setTimeout(10000);
  http.begin(queueClient, lampUrl("/queue"));
  int code = http.GET();
  if (code != 200) {
    Serial.printf("Queue request failed: %d %s\n", code, http.errorToString(code).c_str());
    http.end();
    return 0;
  }
  String body = http.getString();
  http.end();
  queueClient.stop();  // free its TLS session before the per-item downloads open their own

  JsonDocument queueDoc;
  if (deserializeJson(queueDoc, body)) {
    Serial.println("Queue JSON parse failed");
    return 0;
  }
  JsonArray queue = queueDoc["items"].as<JsonArray>();
  Serial.printf("Queue: %u item(s) waiting\n", (unsigned)queue.size());

  JsonDocument manifestDoc;
  loadManifest(manifestDoc);
  JsonArray stored = manifestDoc.as<JsonArray>();
  Serial.printf("Device: %u image(s) stored\n", (unsigned)stored.size());

  LittleFS.remove(TMP_PATH);  // leftover from an interrupted sync
  int added = 0;

  // 2. pull each one in order. every item is committed on its own (download ->
  // evict oldest if full -> manifest -> ack), so a dropped connection mid-sync
  // leaves a consistent device and the rest simply stay queued for next time
  for (JsonObject item : queue) {
    otaguard::tick();
    const char* itemId = item["itemId"] | "";
    if (!*itemId) continue;
    Serial.printf("- %s\n", itemId);
    printSentInfo(item["sentAt"], item["sentGeo"]);

    // an earlier ack may have failed after the image was already saved
    if (inManifest(stored, itemId)) {
      Serial.println("  already stored, acking only");
      ackItem(itemId);
      continue;
    }

    if (!downloadItem(itemId, item["size"] | 0)) break;

    // make sure the e-ink chunks inside the PNG are intact before it takes a slot
    EpdImage img;
    File tf = LittleFS.open(TMP_PATH, "r");
    bool valid = tf && epdParse(tf, img);
    if (tf) tf.close();
    if (!valid) {
      // can never be shown, and leaving it queued would block everything behind it
      Serial.println("  no valid epRb/epRa payload, discarding");
      LittleFS.remove(TMP_PATH);
      ackItem(itemId);
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

    added++;
    if (!ackItem(itemId)) Serial.println("  ack failed (will dedupe next sync)");
  }

  // 3. what the device ended up with, oldest -> newest
  Serial.printf("Stored now (%u/%d):\n", (unsigned)stored.size(), MAX_IMAGES);
  for (JsonObject e : stored) {
    Serial.printf("  %s\n", e["id"].as<const char*>());
    printSentInfo(e["sentAt"], e["geo"]);
  }
  Serial.printf("Downloaded %d new image(s)\n", added);
  return added;
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

// one small dot per unseen image, stacked in a column up the bottom-left corner.
// Each dot has a white ring so it reads over any image
void drawBadge(int n) {
  const int r = 3, ring = 2, step = 14, margin = 8;
  int x = margin + r + ring;
  for (int i = 0; i < n; i++) {
    int y = display.height() - margin - r - ring - i * step;
    if (y < r + ring) break;  // off the top of the panel
    display.fillCircle(x, y, r + ring, GxEPD_WHITE);
    display.fillCircle(x, y, r, GxEPD_BLACK);
  }
}

// draws one packed frame; partial = fast refresh without the full-screen flash.
// badge > 0 also draws that many dots in the bottom-left corner
bool wokeFromSleep = false;   // set in setup(): the panel was hibernated, not freshly powered
bool drewThisWake = false;
void drawFrame(const EpdImage& img, bool partial, int badge = 0) {
  if (FULL_REFRESH_ON_WAKE && wokeFromSleep && !drewThisWake) partial = false;
  drewThisWake = true;
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
    if (badge > 0) drawBadge(badge);
  } while (display.nextPage());
}

// set by buttonTask on each press of the NEXT button, cleared when consumed
volatile bool nextPressed = false;
// set by buttonTask when the NEXT button is held for SYNC_HOLD_MS, cleared when handled
volatile bool syncRequested = false;
// set by buttonTask when NEXT is held for REPLAY_HOLD_MS..SYNC_HOLD_MS and released
volatile bool replayRequested = false;

// full = flashing full-screen refresh (clears ghosting); otherwise a fast partial one
// badge > 0 draws that many dots in the bottom-left corner (on an animation, on its resting first frame)
void showImage(const String& itemId, size_t num, size_t total, bool full, int badge = 0) {
  File f = LittleFS.open("/" + itemId, "r");
  EpdImage img;
  if (!f || !epdParse(f, img)) {
    Serial.printf("Can't open %s\n", itemId.c_str());
    if (f) f.close();
    return;
  }
  Serial.printf("Image %u/%u: %s (%u frame(s), %.1f KB)\n", (unsigned)num, (unsigned)total, itemId.c_str(),
                img.frames, f.size() / 1024.0);

  if (img.frames == 1) {
    if (epdReadFrame(f, img, 0, frameBuf)) drawFrame(img, !full, badge);
    f.close();
    return;
  }

  // animation: only the first frame can be a full refresh, the rest are partial
  bool first = full;
  for (int loop = 0; loop < ANIM_LOOPS; loop++) {
    for (uint8_t i = 0; i < img.frames; i++) {
      if (nextPressed || replayRequested || syncRequested) { f.close(); return; }  // button skips the rest of the animation
      uint32_t t0 = millis();
      if (!epdReadFrame(f, img, i, frameBuf)) break;
      drawFrame(img, !first);
      first = false;
      uint32_t spent = millis() - t0;
      if (spent < img.intervalMs) delay(img.intervalMs - spent);
    }
  }
  // come to rest on the first frame (with the badge, if any)
  if (epdReadFrame(f, img, 0, frameBuf)) drawFrame(img, true, badge);
  f.close();
}

std::vector<String> storedIds;  // newest first (display order); the manifest itself is oldest first

std::vector<bool> storedAnimated;  // same order as storedIds

void loadStoredIds() {
  storedIds.clear();
  storedAnimated.clear();
  JsonDocument doc;
  loadManifest(doc);
  for (JsonObject e : doc.as<JsonArray>()) {
    storedIds.insert(storedIds.begin(), e["id"].as<String>());
    storedAnimated.insert(storedAnimated.begin(), (e["frames"] | 1) > 1);
  }
}

// ---------- WiFi setup (captive portal) ----------

const int RESET_BUTTON = D5;                 // GPIO0, the BOOT button
const int NEXT_BUTTON = 25;                  // GPIO25 (labelled D2): momentary switch to GND
const uint32_t REPLAY_HOLD_MS = 2000;        // hold NEXT 2 s or more (then let go) to replay an animation
const uint32_t SYNC_HOLD_MS = 10000;         // hold NEXT this long to re-check the worker for new images/firmware
const uint32_t RESET_HOLD_MS = 8000;         // hold this long to forget the saved WiFi
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
    display.setCursor(50, 275);
    display.print("No popup? Open 192.168.4.1");
  } while (display.nextPage());
}

// long-press BOOT at any time (after the board has started: holding it
// during power-up would put the chip in flash-download mode instead) to
// forget the saved WiFi and restart into setup mode
bool buttonWakePress = false;  // set in setup() when the button that woke the lamp is still down

void buttonTask(void*) {
  pinMode(RESET_BUTTON, INPUT_PULLUP);
  pinMode(NEXT_BUTTON, INPUT_PULLUP);
  uint32_t heldSince = 0;
  // woken by the button: the press began at power-up, so count the hold from there
  uint32_t nextHeldSince = buttonWakePress ? 1 : 0;
  bool syncFired = false;
  for (;;) {
    // polled every 50 ms, which also debounces. On release, a short press is
    // "next" and a longer one (REPLAY_HOLD_MS+) is "replay"; holding on to
    // SYNC_HOLD_MS asks for a sync instead
    if (digitalRead(NEXT_BUTTON) == LOW) {
      if (!nextHeldSince) nextHeldSince = millis();
      if (!syncFired && millis() - nextHeldSince >= SYNC_HOLD_MS) {
        syncFired = true;
        syncRequested = true;
      }
    } else {
      if (nextHeldSince && !syncFired) {
        if (millis() - nextHeldSince >= REPLAY_HOLD_MS) replayRequested = true;
        else nextPressed = true;
      }
      nextHeldSince = 0;
      syncFired = false;
    }

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
    otaguard::tick();
    Serial.println("No WiFi yet, retrying");
  }
  Serial.println("Connected: " + WiFi.localIP().toString());
}

// ---------- sleep / wake cycle ----------
//
// The lamp spends almost all its time in deep sleep (the e-ink panel keeps its
// image with no power) and wakes for one of three reasons:
//  - power-up / reset / crash / firmware update: full sync, then the quick pass
//  - the sleep timer, every SYNC_INTERVAL_S: sync; the pass only if new images came
//  - the NEXT button: no WiFi at all, just step to the next image
// State that has to survive sleep lives in RTC memory.

RTC_DATA_ATTR uint32_t rtcWakes = 0;  // timer wakes since the last firmware check
RTC_DATA_ATTR int rtcViewIdx = 0;     // image on screen, in newest-first order
RTC_DATA_ATTR int rtcUnseen = 0;      // downloaded images not stepped through yet: the badge number

bool displayReady = false;  // display.init() has run, so it's safe to hibernate
bool cycleOk = false;  // this wake reached the worker or drew something, i.e. the firmware did its job

// waits up to ms (0 = forever) for a press of the NEXT button; true if pressed.
// Returns false early if a sync was requested
bool waitForNext(uint32_t ms) {
  uint32_t t0 = millis();
  while (!nextPressed && !replayRequested) {  // a replay request counts as a press; the caller decides what it means
    otaguard::tick();
    if (syncRequested) return false;
    if (ms && millis() - t0 >= ms) return false;
    delay(20);
  }
  nextPressed = false;
  return true;
}

bool hasSavedWiFi() {
  wifi_config_t conf;
  return esp_wifi_get_config(WIFI_IF_STA, &conf) == ESP_OK && conf.sta.ssid[0] != 0;
}

// joins the saved network, giving up after WIFI_TIMEOUT_MS. The setup portal
// (which keeps the lamp awake for minutes) opens only when asked: for a lamp
// with no WiFi configured at all, or on a manual sync that can't connect
bool connectForSync(bool portalIfUnconfigured, bool portalIfFailed) {
  WiFi.mode(WIFI_STA);  // also loads the saved credentials
  if (!hasSavedWiFi()) {
    if (!portalIfUnconfigured) {
      Serial.println("No saved WiFi, skipping sync");
      return false;
    }
    connectWiFi();
    return true;
  }
  WiFi.begin();
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_TIMEOUT_MS) {
    otaguard::tick();
    delay(100);
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Connected: " + WiFi.localIP().toString());
    return true;
  }
  if (portalIfFailed) {
    connectWiFi();
    return true;
  }
  Serial.println("WiFi didn't connect, skipping sync");
  return false;
}

// connects, optionally checks for new firmware, downloads new images, then
// turns WiFi off. False if it couldn't get online
bool doSync(bool checkFirmware, bool portalIfUnconfigured, bool portalIfFailed, int& added) {
  added = 0;
  if (!connectForSync(portalIfUnconfigured, portalIfFailed)) return false;
  cycleOk = true;
  if (checkFirmware) checkForUpdate();  // reboots into new firmware if there is one
  added = syncQueue();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  return true;
}

// draws image idx (newest-first). The first image gets the full refresh, and
// only it carries the badge
void drawIndex(int idx, int badge) {
  showImage(storedIds[idx], idx + 1, storedIds.size(), idx == 0, idx == 0 ? badge : 0);
  rtcViewIdx = idx;
  cycleOk = true;
}

// quick pass through everything, newest to oldest, then rest on the newest with
// the badge. A button press steps on and ends the pass; a sync request ends it
// too and is left for the caller
void quickPass(int badge) {
  int total = storedIds.size();
  for (int i = 0; i < total; i++) {
    drawIndex(i, 0);
    if (waitForNext(STILL_HOLD_MS)) {
      replayRequested = false;  // during the pass any press just steps on
      rtcUnseen = 0;  // they're looking through them now
      drawIndex((i + 1) % total, 0);
      return;
    }
    if (syncRequested) return;
  }
  Serial.println("Pass complete, resting on the newest image (press the button to step)");
  rtcUnseen = badge;
  drawIndex(0, badge);
}

// plays the animation on screen again (it rests on its first frame afterwards)
void replayCurrent() {
  showImage(storedIds[rtcViewIdx], rtcViewIdx + 1, storedIds.size(), false, rtcViewIdx == 0 ? rtcUnseen : 0);
  cycleOk = true;
}

// 10 s hold: re-check the worker (firmware too), fetch new images, replay the pass
void manualSync() {
  Serial.println("NEXT held: syncing with the worker");
  otaguard::clearBad();  // a manual sync retries a blocklisted firmware version too
  int added = 0;
  doSync(true, true, true, added);
  loadStoredIds();
  nextPressed = false;
  syncRequested = false;
  rtcUnseen += added;
  if (!storedIds.empty()) quickPass(rtcUnseen);
}

// stays awake until AWAKE_IDLE_MS after the last activity so the NEXT button
// can step through the images; a 2 s hold replays an animation, a 10 s hold syncs
void idleWindow() {
  uint32_t last = millis();
  while (millis() - last < AWAKE_IDLE_MS) {
    bool pressed = waitForNext(AWAKE_IDLE_MS - (millis() - last));
    if (rtcViewIdx >= (int)storedIds.size()) rtcViewIdx = 0;
    if (syncRequested) {
      manualSync();
      last = millis();
    } else if (replayRequested) {
      replayRequested = false;
      if (!storedIds.empty() && storedAnimated[rtcViewIdx]) {
        replayCurrent();
      } else if (!storedIds.empty()) {  // nothing to replay on a still: it's just a step
        rtcUnseen = 0;
        drawIndex((rtcViewIdx + 1) % storedIds.size(), 0);
      }
      last = millis();
    } else if (pressed) {
      rtcUnseen = 0;
      if (!storedIds.empty()) drawIndex((rtcViewIdx + 1) % storedIds.size(), 0);
      last = millis();
    }
  }
}

[[noreturn]] void goToSleep() {
  if (cycleOk) otaguard::markValid();  // the cycle did its job: keep this firmware
  else otaguard::beforeSleep();        // it didn't (say, no WiFi): a rollback proves nothing about it

  // a button still held would wake us again at once
  uint32_t t0 = millis();
  while (digitalRead(NEXT_BUTTON) == LOW && millis() - t0 < 10000) delay(20);

  if (displayReady) display.hibernate();
  WiFi.mode(WIFI_OFF);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)NEXT_BUTTON, 0);
  rtc_gpio_pullup_en((gpio_num_t)NEXT_BUTTON);
  rtc_gpio_pulldown_dis((gpio_num_t)NEXT_BUTTON);
  esp_sleep_enable_timer_wakeup((uint64_t)SYNC_INTERVAL_S * 1000000ULL);
  Serial.printf("Sleeping (next sync in %us, or press the button)\n", (unsigned)SYNC_INTERVAL_S);
  Serial.flush();
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  otaguard::begin(FW_VERSION);  // before anything that could fail
#ifdef FW_TEST_CRASH  // test builds only: proves the rollback works
  Serial.println("FW_TEST_CRASH: crashing on purpose");
  delay(200);
  abort();
#endif
  delay(200);

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  bool buttonWake = cause == ESP_SLEEP_WAKEUP_EXT0;
  bool timerWake = cause == ESP_SLEEP_WAKEUP_TIMER;
  wokeFromSleep = buttonWake || timerWake;
  if (esp_reset_reason() == ESP_RST_POWERON) {  // RTC memory also survives soft resets (e.g. after an update)
    rtcWakes = 0;
    rtcViewIdx = 0;
    rtcUnseen = 0;
  }
  Serial.printf("Wake: %s\n", buttonWake ? "button" : timerWake ? "timer" : "power-up/reset");
  if (buttonWake) rtc_gpio_deinit((gpio_num_t)NEXT_BUTTON);

  if (!LittleFS.begin(true)) {  // true = format on first boot
    Serial.println("LittleFS mount failed");
    goToSleep();
  }
  // after deep sleep the panel still holds its image, so partial refreshes keep working
  display.init(115200, !wokeFromSleep);
  displayReady = true;

  pinMode(NEXT_BUTTON, INPUT_PULLUP);
  if (buttonWake) {
    if (digitalRead(NEXT_BUTTON) == HIGH) nextPressed = true;  // tapped and released before we were up: still a step
    else buttonWakePress = true;                               // still down: buttonTask times the hold from power-up
  }
  xTaskCreate(buttonTask, "button", 4096, nullptr, 1, nullptr);

  loadStoredIds();
  if (rtcViewIdx >= (int)storedIds.size()) rtcViewIdx = 0;

  if (buttonWake) {  // no WiFi, just step through what's stored
    idleWindow();
    goToSleep();
  }

  // power-up/reset: sync and replay the pass. Timer: sync, and pass only if there's news
  bool checkFirmware;
  if (!timerWake) {
    checkFirmware = true;
    rtcWakes = 0;
  } else {
    checkFirmware = ++rtcWakes >= FW_CHECK_EVERY_WAKES;
    if (checkFirmware) rtcWakes = 0;
  }
  int added = 0;
  doSync(checkFirmware, !timerWake, false, added);
  loadStoredIds();
  rtcUnseen += added;

  if (!storedIds.empty() && (!timerWake || added > 0)) {
    quickPass(rtcUnseen);
    idleWindow();
  } else if (otaguard::updatePending() && !storedIds.empty()) {
    // first run of a new firmware with nothing new to show: still prove the display path
    Serial.println("First run of a new firmware: proving the display path before keeping it");
    drawIndex(rtcViewIdx, rtcViewIdx == 0 ? rtcUnseen : 0);
  }
  goToSleep();
}

void loop() {
  goToSleep();  // not reached; setup() always ends in deep sleep
}
