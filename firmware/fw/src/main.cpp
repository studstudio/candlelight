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

const uint32_t STILL_HOLD_MS = 500;    // [PLACEHOLDER] how long each image stays up during the automatic pass
const int ANIM_LOOPS = 1;              // times an animation plays through before moving on

// ---- sleep schedule ----
const uint32_t SYNC_INTERVAL_S = 600;                        // [PLACEHOLDER] deep-sleep time between worker syncs (10 min while prototyping)
const uint32_t WIFI_TIMEOUT_MS = 20000;                      // [PLACEHOLDER] give up on WiFi after this and go back to sleep
const uint32_t AWAKE_IDLE_MS = 30000;                        // [PLACEHOLDER] stay up this long after the last button press or pass
const uint32_t OTA_BUDGET_MS = 150000;                       // [PLACEHOLDER] a firmware download that isn't finished by now is abandoned (retried next check)
const uint32_t OTA_STALL_MS = 15000;                         // [PLACEHOLDER] ...and so is one that stops delivering data for this long
const uint32_t FAST_WIFI_MS = 6000;                          // [PLACEHOLDER] fast connect (cached channel/BSSID/IP) gives up after this and does a full connect
const uint32_t DHCP_REUSE_S = 1800;                          // [PLACEHOLDER] reuse the cached IP address this long before asking DHCP again
const uint32_t BUNDLE_STALL_MS = 15000;                      // [PLACEHOLDER] a sync response that stops delivering data for this long is abandoned
const uint32_t BUNDLE_BUDGET_MS = 120000;                    // [PLACEHOLDER] total time allowed for the sync response
const bool FAST_DRAW = true;                                 // send frames to the panel directly (skips the per-pixel library loop); set false if the display glitches
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

enum CommitResult { COMMIT_OK, COMMIT_INVALID, COMMIT_FAIL };

// the image just written to TMP_PATH becomes a stored image: checks it, makes
// room if the device is full, renames it into place and adds it to the manifest
CommitResult commitTmpItem(JsonDocument& manifestDoc, JsonArray stored, const char* itemId, JsonVariantConst sentAt,
                           JsonVariantConst geo) {
  // make sure the e-ink chunks inside the PNG are intact before it takes a slot
  EpdImage img;
  File tf = LittleFS.open(TMP_PATH, "r");
  bool valid = tf && epdParse(tf, img);
  if (tf) tf.close();
  if (!valid) {
    // can never be shown, and leaving it queued would block everything behind it
    Serial.println("  no valid epRb/epRa payload, discarding");
    LittleFS.remove(TMP_PATH);
    return COMMIT_INVALID;
  }
  Serial.printf("  %ux%u, %u-bpp, %u frame(s)\n", img.w, img.h, img.bpp, img.frames);

  // device holds MAX_IMAGES at most: the oldest image the user has already
  // viewed makes room for this one (an unviewed one only if all are unviewed)
  if ((int)stored.size() >= MAX_IMAGES) {
    size_t victim = 0;
    for (size_t i = 0; i < stored.size(); i++) {
      if (stored[i]["seen"] | true) { victim = i; break; }
    }
    String oldId = stored[victim]["id"] | "";
    LittleFS.remove("/" + oldId);
    stored.remove(victim);
    Serial.printf("  evicted: %s\n", oldId.c_str());
  }

  LittleFS.rename(TMP_PATH, "/" + String(itemId));
  JsonObject entry = stored.add<JsonObject>();
  entry["id"] = itemId;
  entry["sentAt"] = sentAt;
  entry["geo"] = geo;
  entry["frames"] = img.frames;
  entry["intervalMs"] = img.intervalMs;
  entry["seen"] = false;  // not viewed with the button yet
  if (!saveManifest(manifestDoc)) {
    Serial.println("  manifest write failed");
    return COMMIT_FAIL;
  }
  return COMMIT_OK;
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

// the worker reports the current firmware inside the queue response, so
// learning about an update costs no extra request. syncQueue() fills these in
String workerFwVersion;
uint32_t workerFwSize = 0;

// if the worker's firmware isn't the one running, pulls it into the spare OTA
// slot and reboots into it (never returns on success)
void updateFirmwareIfNeeded() {
  if (workerFwVersion.isEmpty() || workerFwVersion == FW_VERSION) {
    Serial.println("Firmware is up to date");
    return;
  }
  if (otaguard::isBad(workerFwVersion)) {
    Serial.printf("Firmware %s failed on this lamp before, skipping (hold NEXT 10s to retry it)\n", workerFwVersion.c_str());
    return;
  }

  Serial.printf("Updating firmware %s -> %s (%u bytes)\n", FW_VERSION, workerFwVersion.c_str(), (unsigned)workerFwSize);
  WiFiClientSecure updateClient;
  updateClient.setInsecure();  // prototype only
  otaguard::tick();
  otaguard::setTrying(workerFwVersion);  // lets the next boot blocklist it if the bootloader rolls it back
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
  uint32_t tQueue = millis();
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
  Serial.printf("Queue list took %u ms\n", (unsigned)(millis() - tQueue));
  workerFwVersion = queueDoc["firmware"]["version"] | "";
  workerFwSize = queueDoc["firmware"]["size"] | 0;
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

    uint32_t tItem = millis();
    if (!downloadItem(itemId, item["size"] | 0)) break;
    uint32_t tDownloaded = millis();

    CommitResult cr = commitTmpItem(manifestDoc, stored, itemId, item["sentAt"], item["sentGeo"]);
    if (cr == COMMIT_INVALID) {
      ackItem(itemId);
      continue;
    }
    if (cr == COMMIT_FAIL) break;

    added++;
    uint32_t tStored = millis();
    if (!ackItem(itemId)) Serial.println("  ack failed (will dedupe next sync)");
    uint32_t tAcked = millis();
    uint32_t dl = tDownloaded - tItem;
    double kb = (item["size"] | 0) / 1024.0;
    Serial.printf("  timing: download %u ms (%.1f KB, %.0f KB/s), validate+store %u ms, ack %u ms, total %u ms\n",
                  (unsigned)dl, kb, dl ? kb * 1000.0 / dl : 0.0, (unsigned)(tStored - tDownloaded),
                  (unsigned)(tAcked - tStored), (unsigned)(tAcked - tItem));
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

// ---------- one-connection sync ----------
//
// GET /lamp/<id>/sync returns the queue AND the images in one response (see the
// worker): one secure connection instead of one per image, then a single POST
// /ack for everything stored. If anything about it fails the caller falls back
// to the older per-image sync above, which still works against any worker.

enum SyncResult { SYNC_OK, SYNC_FALLBACK, SYNC_PARTIAL };

// reads one '\n'-terminated line of the response
bool readLineFrom(WiFiClient* s, String& out) {
  out = "";
  uint32_t last = millis();
  while (millis() - last < BUNDLE_STALL_MS) {
    otaguard::tick();
    if (s->available()) {
      char c = s->read();
      last = millis();
      if (c == '\n') return true;
      if (c != '\r') out += c;
      if (out.length() > 2048) return false;
    } else if (!s->connected()) {
      return false;
    } else {
      delay(2);
    }
  }
  return false;
}

// copies the next n bytes of the response into f, or throws them away if f is null
bool copyBytes(WiFiClient* s, File* f, size_t n, uint32_t deadline) {
  uint8_t buf[1460];
  uint32_t last = millis();
  while (n > 0) {
    otaguard::tick();
    if (millis() > deadline || millis() - last > BUNDLE_STALL_MS) return false;
    int avail = s->available();
    if (!avail) {
      if (!s->connected()) return false;
      delay(2);
      continue;
    }
    int got = s->readBytes(buf, min((size_t)avail, min(n, sizeof(buf))));
    if (got <= 0) continue;
    if (f && f->write(buf, got) != (size_t)got) return false;
    n -= got;
    last = millis();
  }
  return true;
}

// confirms a batch of stored items to the worker in one request
bool ackItems(const std::vector<String>& ids) {
  if (ids.empty()) return true;
  JsonDocument doc;
  JsonArray arr = doc["ids"].to<JsonArray>();
  for (const String& id : ids) arr.add(id);
  String body;
  serializeJson(doc, body);
  WiFiClientSecure client;
  client.setInsecure();  // prototype only
  HTTPClient http;
  http.setReuse(false);
  http.setTimeout(10000);
  http.begin(client, lampUrl("/ack"));
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);
  http.end();
  return code == 200;
}

// downloads everything waiting in a single response. added = images stored
SyncResult syncBundle(int& added) {
  added = 0;
  WiFiClientSecure client;
  client.setInsecure();  // prototype only
  HTTPClient http;
  http.setReuse(false);
  http.setTimeout(10000);
  uint32_t tStart = millis();
  http.begin(client, lampUrl("/sync"));
  int code = http.GET();
  if (code != 200) {
    Serial.printf("Bundle sync: HTTP %d\n", code);
    http.end();
    return SYNC_FALLBACK;
  }
  WiFiClient* stream = http.getStreamPtr();
  uint32_t deadline = millis() + BUNDLE_BUDGET_MS;

  String line;
  JsonDocument head;
  if (!readLineFrom(stream, line) || deserializeJson(head, line) || (head["v"] | 0) != 1) {
    Serial.println("Bundle sync: unreadable header");
    http.end();
    return SYNC_FALLBACK;
  }
  workerFwVersion = head["firmware"]["version"] | "";
  workerFwSize = head["firmware"]["size"] | 0;
  int count = head["count"] | 0;
  Serial.printf("Bundle: %d item(s) waiting (header after %u ms)\n", count, (unsigned)(millis() - tStart));

  JsonDocument manifestDoc;
  loadManifest(manifestDoc);
  JsonArray stored = manifestDoc.as<JsonArray>();
  Serial.printf("Device: %u image(s) stored\n", (unsigned)stored.size());
  LittleFS.remove(TMP_PATH);  // leftover from an interrupted sync

  std::vector<String> toAck;  // everything we're finished with, stored or not
  SyncResult result = SYNC_OK;
  for (int k = 0; k < count; k++) {
    if (!readLineFrom(stream, line)) { result = SYNC_PARTIAL; break; }
    JsonDocument item;
    if (deserializeJson(item, line)) { result = SYNC_PARTIAL; break; }  // can't find the next item mid-stream
    String itemId = item["itemId"] | "";
    size_t size = item["size"] | 0;
    Serial.printf("- %s\n", itemId.c_str());
    printSentInfo(item["sentAt"], item["sentGeo"]);

    // an earlier ack may have failed after the image was already saved
    if (itemId.isEmpty() || inManifest(stored, itemId.c_str())) {
      Serial.println("  already stored, skipping");
      if (!copyBytes(stream, nullptr, size, deadline)) { result = SYNC_PARTIAL; break; }
      if (itemId.length()) toAck.push_back(itemId);
      continue;
    }

    uint32_t tItem = millis();
    File f = LittleFS.open(TMP_PATH, "w");
    if (!f) { result = SYNC_PARTIAL; break; }
    bool copied = copyBytes(stream, &f, size, deadline);
    f.close();
    uint32_t tDownloaded = millis();
    if (!copied) {
      Serial.println("  download incomplete");
      LittleFS.remove(TMP_PATH);
      result = SYNC_PARTIAL;
      break;
    }
    CommitResult cr = commitTmpItem(manifestDoc, stored, itemId.c_str(), item["sentAt"], item["sentGeo"]);
    if (cr == COMMIT_FAIL) { result = SYNC_PARTIAL; break; }
    if (cr == COMMIT_OK) added++;
    toAck.push_back(itemId);  // stored, or unusable: either way it's done with
    uint32_t dl = tDownloaded - tItem;
    double kb = size / 1024.0;
    Serial.printf("  timing: download %u ms (%.1f KB, %.0f KB/s), validate+store %u ms\n", (unsigned)dl, kb,
                  dl ? kb * 1000.0 / dl : 0.0, (unsigned)(millis() - tDownloaded));
  }
  http.end();

  if (!toAck.empty()) {
    uint32_t tAck = millis();
    bool acked = ackItems(toAck);
    Serial.printf("Ack of %u item(s) %s in %u ms\n", (unsigned)toAck.size(), acked ? "done" : "FAILED (will dedupe next sync)",
                  (unsigned)(millis() - tAck));
  }
  Serial.printf("Stored now %u/%d, downloaded %d new image(s)%s\n", (unsigned)stored.size(), MAX_IMAGES, added,
                result == SYNC_OK ? "" : " (sync broke off)");
  return result;
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

// boxed text on a white background so it reads over any image
void drawBoxedText(const char* txt, int x, int y) {
  display.setTextSize(2);
  display.setTextColor(GxEPD_BLACK);
  int16_t bx, by;
  uint16_t bw, bh;
  display.getTextBounds(txt, 0, 0, &bx, &by, &bw, &bh);
  const int pad = 5;
  int w = bw + 2 * pad, h = bh + 2 * pad;
  if (x < 0) x = display.width() + x - w;   // negative = measured from the right edge
  if (y < 0) y = display.height() + y - h;  // negative = measured from the bottom edge
  display.fillRect(x, y, w, h, GxEPD_WHITE);
  display.drawRect(x, y, w, h, GxEPD_BLACK);
  display.drawRect(x + 1, y + 1, w - 2, h - 2, GxEPD_BLACK);
  display.setCursor(x + pad - bx, y + pad - by);
  display.print(txt);
}

// the number of unseen images other than the one on screen, bottom-left corner
void drawBadge(int n) {
  char txt[8];
  snprintf(txt, sizeof(txt), "%d", n);
  drawBoxedText(txt, 6, -6);
}

// top-left corner: feedback that the 10 s sync hold registered
void drawSyncBadge() {
  drawBoxedText("Syncing...", 6, 6);
}

// draws one packed frame; partial = fast refresh without the full-screen flash.
// badge > 0 also draws that number bottom-left; syncBadge adds the top-left \"Syncing...\" badge
bool wokeFromSleep = false;   // set in setup(): the panel was hibernated, not freshly powered
bool drewThisWake = false;
uint32_t lastBuildMs = 0, lastPanelMs = 0;  // of the last drawFrame: making the picture vs sending it and refreshing

uint8_t monoBuf[(GxEPD2_420_GDEY042T81::WIDTH / 8) * GxEPD2_420_GDEY042T81::HEIGHT];  // 1 bit per pixel, 1 = white

// 2-bit stills to the 1-bit picture the panel takes, with the same halftone as
// pixelIsBlack() but without a library call per pixel
void ditherToMono(const EpdImage& img, const uint8_t* src, uint8_t* dst) {
  static const uint8_t BAYER[2][2] = {{0, 2}, {3, 1}};
  int srcRow = (img.w * img.bpp + 7) / 8, dstRow = (img.w + 7) / 8;
  for (int y = 0; y < img.h; y++) {
    const uint8_t* srow = src + y * srcRow;
    uint8_t* drow = dst + y * dstRow;
    const uint8_t* m = BAYER[y & 1];
    for (int bx = 0; bx < dstRow; bx++) {  // 8 pixels at a time
      uint8_t out = 0xFF;
      for (int k = 0; k < 8; k++) {
        int x = bx * 8 + k;
        uint8_t code = (srow[x >> 2] >> (6 - 2 * (x & 3))) & 3;
        uint8_t t = m[x & 1];
        if (code == 0 || (code == 1 && t >= 1) || (code == 2 && t >= 3)) out &= ~(0x80 >> k);
      }
      drow[bx] = out;
    }
  }
}

// Sends a full-size landscape frame straight to the panel with the same
// controller commands the library's own paging uses (see GxEPD2_BW::nextPage),
// skipping the buffer and the per-pixel drawing. The frames are already packed
// the way the panel wants them (MSB first, 1 = white). False if it can't (other
// size, or FAST_DRAW off), and the caller draws the normal way
bool drawDirect(const EpdImage& img, bool partial) {
  const int W = GxEPD2_420_GDEY042T81::WIDTH, H = GxEPD2_420_GDEY042T81::HEIGHT;
  if (!FAST_DRAW || img.w != W || img.h != H) return false;
  uint32_t t0 = millis();
  const uint8_t* bitmap;
  if (img.bpp == 1) {
    bitmap = frameBuf;
  } else if (img.bpp == 2) {
    ditherToMono(img, frameBuf, monoBuf);
    bitmap = monoBuf;
  } else {
    return false;
  }
  lastBuildMs = millis() - t0;

  uint32_t t1 = millis();
  auto& e = display.epd2;
  if (partial) {
    e.writeImage(bitmap, 0, 0, W, H);
    e.refresh(0, 0, W, H);
    e.writeImageAgain(bitmap, 0, 0, W, H);  // so the next partial update knows what is on screen
  } else {
    e.writeImageForFullRefresh(bitmap, 0, 0, W, H);
    e.refresh(false);
    e.writeImageAgain(bitmap, 0, 0, W, H);
    e.powerOff();
  }
  lastPanelMs = millis() - t1;
  return true;
}

void drawFrame(const EpdImage& img, bool partial, int badge = 0, bool syncBadge = false) {
  if (FULL_REFRESH_ON_WAKE && wokeFromSleep && !drewThisWake) partial = false;
  drewThisWake = true;
  display.setRotation(img.w < img.h ? 1 : 0);  // portrait images (300x400) rotate the panel
  if (display.width() != img.w || display.height() != img.h) {
    Serial.printf("Image is %ux%u but panel is %dx%d, skipping\n", img.w, img.h, display.width(), display.height());
    return;
  }
  // frames with a badge on them need the library's drawing; everything else can go straight to the panel
  if (badge <= 0 && !syncBadge && drawDirect(img, partial)) return;

  if (partial) display.setPartialWindow(0, 0, img.w, img.h);
  else display.setFullWindow();
  uint32_t tBuild = 0, tPanel = 0;
  bool more;
  display.firstPage();
  do {
    uint32_t t0 = millis();
    display.fillScreen(GxEPD_WHITE);
    for (uint16_t y = 0; y < img.h; y++) {
      for (uint16_t x = 0; x < img.w; x++) {
        if (pixelIsBlack(epdPixel(frameBuf, img, x, y), img.bpp, x, y)) display.drawPixel(x, y, GxEPD_BLACK);
      }
    }
    if (badge > 0) drawBadge(badge);
    if (syncBadge) drawSyncBadge();
    tBuild += millis() - t0;
    uint32_t t1 = millis();
    more = display.nextPage();
    tPanel += millis() - t1;
  } while (more);
  lastBuildMs = tBuild;
  lastPanelMs = tPanel;
}

// set by buttonTask on each press of the NEXT button, cleared when consumed
volatile bool nextPressed = false;
// set by buttonTask when the NEXT button is held for SYNC_HOLD_MS, cleared when handled
volatile bool syncRequested = false;
// set by buttonTask when NEXT is held for REPLAY_HOLD_MS..SYNC_HOLD_MS and released
volatile bool replayRequested = false;

// full = flashing full-screen refresh (clears ghosting); otherwise a fast partial one
// prints how long each animation frame took to draw, how long the code then
// waited to reach the target interval, and the real time from one frame start to
// the next. If draw time exceeds the target interval the panel can't keep up
struct FrameTiming { uint32_t startMs, drawMs, waitMs, buildMs, panelMs; };
void printAnimTiming(const std::vector<FrameTiming>& t, uint16_t targetMs, bool interrupted) {
  if (t.empty()) return;
  Serial.printf("  animation timing: target %u ms/frame, %u frame(s) played%s\n", targetMs, (unsigned)t.size(),
                interrupted ? " (stopped by a button press)" : "");
  uint32_t minP = 0xFFFFFFFF, maxP = 0, sumP = 0, maxDraw = 0;
  for (size_t k = 0; k < t.size(); k++) {
    if (t[k].drawMs > maxDraw) maxDraw = t[k].drawMs;
    if (k == 0) {
      Serial.printf("    frame %2u: draw %4u ms (build %u + panel %u), wait %4u ms\n", (unsigned)(k + 1), (unsigned)t[k].drawMs,
                    (unsigned)t[k].buildMs, (unsigned)t[k].panelMs, (unsigned)t[k].waitMs);
      continue;
    }
    uint32_t period = t[k].startMs - t[k - 1].startMs;  // frame start to frame start: what you actually see
    if (period < minP) minP = period;
    if (period > maxP) maxP = period;
    sumP += period;
    Serial.printf("    frame %2u: draw %4u ms (build %u + panel %u), wait %4u ms, %4u ms after the previous frame%s\n", (unsigned)(k + 1),
                  (unsigned)t[k].drawMs, (unsigned)t[k].buildMs, (unsigned)t[k].panelMs, (unsigned)t[k].waitMs, (unsigned)period,
                  t[k].drawMs > targetMs ? "  <- draw slower than target" : "");
  }
  if (t.size() > 1) {
    Serial.printf("    frame-to-frame: avg %u ms, min %u, max %u (target %u); slowest draw %u ms\n",
                  (unsigned)(sumP / (t.size() - 1)), (unsigned)minP, (unsigned)maxP, targetMs, (unsigned)maxDraw);
  }
}

// badge > 0 draws that number bottom-left (on an animation, on its resting first frame)
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

  // animation: only the first frame can be a full refresh, the rest are partial.
  // Timing is recorded per frame and printed afterwards (printing in between would skew it)
  std::vector<FrameTiming> timing;
  timing.reserve(img.frames * ANIM_LOOPS);
  bool first = full;
  bool interrupted = false;
  for (int loop = 0; loop < ANIM_LOOPS && !interrupted; loop++) {
    for (uint8_t i = 0; i < img.frames; i++) {
      if (nextPressed || replayRequested || syncRequested) {  // button skips the rest of the animation
        interrupted = true;
        break;
      }
      uint32_t t0 = millis();
      if (!epdReadFrame(f, img, i, frameBuf)) break;
      drawFrame(img, !first);
      first = false;
      uint32_t spent = millis() - t0;
      uint32_t wait = spent < img.intervalMs ? img.intervalMs - spent : 0;
      if (wait) delay(wait);
      timing.push_back({t0, spent, wait, lastBuildMs, lastPanelMs});
    }
  }
  printAnimTiming(timing, img.intervalMs, interrupted);
  if (interrupted) {
    f.close();
    return;
  }
  // come to rest on the first frame (with the badge, if any)
  uint32_t restStart = millis();
  if (epdReadFrame(f, img, 0, frameBuf)) drawFrame(img, true, badge);
  Serial.printf("  rest frame drawn in %u ms (build %u + panel %u)\n", (unsigned)(millis() - restStart), (unsigned)lastBuildMs,
                (unsigned)lastPanelMs);
  f.close();
}

std::vector<String> storedIds;  // newest first (display order); the manifest itself is oldest first

std::vector<bool> storedAnimated;  // same order as storedIds
std::vector<bool> storedSeen;      // viewed with the button (an image only drawn by the quick pass doesn't count)
bool seenDirty = false;            // storedSeen changed and isn't in the manifest yet

void loadStoredIds() {
  storedIds.clear();
  storedAnimated.clear();
  storedSeen.clear();
  JsonDocument doc;
  loadManifest(doc);
  for (JsonObject e : doc.as<JsonArray>()) {
    storedIds.insert(storedIds.begin(), e["id"].as<String>());
    storedAnimated.insert(storedAnimated.begin(), (e["frames"] | 1) > 1);
    storedSeen.insert(storedSeen.begin(), e["seen"] | true);  // images stored before this existed count as seen
  }
}

int countUnseen() {
  int n = 0;
  for (bool seen : storedSeen) n += !seen;
  return n;
}

// the badge number: unseen images other than the newest, which is the one the
// badge sits on (so 4 new images show a 3 next to image 1)
int badgeCount() {
  int n = 0;
  for (size_t i = 1; i < storedSeen.size(); i++) n += !storedSeen[i];
  return n;
}

void markSeen(int idx) {
  if (idx >= 0 && idx < (int)storedSeen.size() && !storedSeen[idx]) {
    storedSeen[idx] = true;
    seenDirty = true;
  }
}

// writes the seen flags into the manifest (called before sleeping and before a
// sync, which rewrites the manifest)
void saveSeenFlags() {
  if (!seenDirty) return;
  JsonDocument doc;
  loadManifest(doc);
  for (JsonObject e : doc.as<JsonArray>()) {
    String id = e["id"].as<String>();
    for (size_t i = 0; i < storedIds.size(); i++) {
      if (storedIds[i] == id) {
        e["seen"] = (bool)storedSeen[i];
        break;
      }
    }
  }
  if (saveManifest(doc)) seenDirty = false;
}

// ---------- WiFi setup (captive portal) ----------

const int RESET_BUTTON = D5;                 // GPIO0, the BOOT button
const int NEXT_BUTTON = 25;                  // GPIO25 (labelled D2): momentary switch to GND
const uint32_t REPLAY_HOLD_MS = 2000;        // [PLACEHOLDER] hold NEXT 2 s or more (then let go) to replay an animation
const uint32_t SYNC_HOLD_MS = 10000;         // [PLACEHOLDER] hold NEXT this long to re-check the worker for new images/firmware
const uint32_t RESET_HOLD_MS = 8000;         // [PLACEHOLDER] hold this long to forget the saved WiFi
const uint32_t PORTAL_TIMEOUT_S = 180;       // [PLACEHOLDER] setup mode stays open this long, then retries the saved network

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

RTC_DATA_ATTR int rtcViewIdx = 0;     // image on screen, in newest-first order
// what the last successful connect looked like, so the next wake can skip the channel scan and DHCP
RTC_DATA_ATTR uint8_t rtcWifiChannel = 0;  // 0 = nothing cached
RTC_DATA_ATTR uint8_t rtcBssid[6];
RTC_DATA_ATTR uint32_t rtcIp = 0, rtcGw = 0, rtcMask = 0, rtcDns = 0;
RTC_DATA_ATTR uint32_t rtcDhcpAgeS = 0;    // seconds since the cached address came from DHCP
RTC_DATA_ATTR int rtcManualFails = 0; // 10 s syncs in a row that couldn't connect (the 2nd opens the setup portal)

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

// waits for the WiFi connection, up to ms
bool waitForWiFi(uint32_t ms) {
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < ms) {
    otaguard::tick();
    delay(20);
  }
  return WiFi.status() == WL_CONNECTED;
}

bool hasSavedWiFi() {
  wifi_config_t conf;
  return esp_wifi_get_config(WIFI_IF_STA, &conf) == ESP_OK && conf.sta.ssid[0] != 0;
}

// joins the saved network, giving up after WIFI_TIMEOUT_MS. The setup portal
// (which keeps the lamp awake for minutes) opens only for a lamp with no WiFi
// configured at all. A saved network that won't connect never opens it: skip
// the sync and sleep (hold BOOT 8 s to forget the network and set up again)
bool connectForSync(bool portalIfUnconfigured) {
  WiFi.mode(WIFI_STA);  // also loads the saved credentials
  if (!hasSavedWiFi()) {
    if (!portalIfUnconfigured) {
      Serial.println("No saved WiFi, skipping sync");
      return false;
    }
    connectWiFi();
    rtcManualFails = 0;
    return true;
  }
  wifi_config_t conf;
  esp_wifi_get_config(WIFI_IF_STA, &conf);
  uint32_t t0 = millis();
  bool connected = false, usedStatic = false, fastTried = false;

  // fast path: the same access point and channel as last time (no scan), and the
  // same IP address while it is still fresh (no DHCP). Falls back to a full connect
  if (rtcWifiChannel) {
    if (rtcIp && rtcDhcpAgeS < DHCP_REUSE_S) {
      WiFi.config(IPAddress(rtcIp), IPAddress(rtcGw), IPAddress(rtcMask), IPAddress(rtcDns));
      usedStatic = true;
    }
    fastTried = true;
    WiFi.begin((const char*)conf.sta.ssid, (const char*)conf.sta.password, rtcWifiChannel, rtcBssid);
    connected = waitForWiFi(FAST_WIFI_MS);
    if (!connected) {
      Serial.println("Fast WiFi connect failed, doing a full connect");
      WiFi.disconnect();
      if (usedStatic) WiFi.config(IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0));  // back to DHCP
      usedStatic = false;
      rtcWifiChannel = 0;
      rtcIp = 0;
    }
  }
  if (!connected) {
    WiFi.begin();
    uint32_t spent = millis() - t0;
    connected = waitForWiFi(spent < WIFI_TIMEOUT_MS ? WIFI_TIMEOUT_MS - spent : 0);
  }

  if (connected) {
    WiFi.setSleep(false);  // no modem power-save while syncing: it adds delay to every packet, and we're only on for seconds
    // remember how we got on, for the next wake
    rtcWifiChannel = WiFi.channel();
    memcpy(rtcBssid, WiFi.BSSID(), 6);
    if (usedStatic) {
      rtcDhcpAgeS += SYNC_INTERVAL_S;
    } else {
      rtcIp = (uint32_t)WiFi.localIP();
      rtcGw = (uint32_t)WiFi.gatewayIP();
      rtcMask = (uint32_t)WiFi.subnetMask();
      rtcDns = (uint32_t)WiFi.dnsIP();
      rtcDhcpAgeS = 0;
    }
    Serial.printf("Connected: %s (%s, WiFi took %u ms, %u ms since wake)\n", WiFi.localIP().toString().c_str(),
                  usedStatic ? "cached channel + IP" : (fastTried && connected && rtcWifiChannel) ? "cached channel, DHCP" : "full connect",
                  (unsigned)(millis() - t0), (unsigned)millis());
    rtcManualFails = 0;  // WiFi works, whatever happened on an earlier hold
    return true;
  }
  Serial.println("WiFi didn't connect, skipping sync");
  return false;
}

// connects, downloads new images, installs a newer firmware if the worker
// reported one, then turns WiFi off. False if it couldn't get online
bool doSync(bool portalIfUnconfigured, int& added) {
  added = 0;
  if (!connectForSync(portalIfUnconfigured)) return false;
  cycleOk = true;
  saveSeenFlags();  // the sync rewrites the manifest from flash, so save our changes first
  uint32_t tSync = millis();
  SyncResult sr = syncBundle(added);
  if (sr != SYNC_OK) {
    Serial.println(sr == SYNC_FALLBACK ? "Falling back to the per-image sync" : "Finishing with the per-image sync");
    added += syncQueue();
  }
  Serial.printf("Sync took %u ms (%u ms since wake)\n", (unsigned)(millis() - tSync), (unsigned)millis());
  updateFirmwareIfNeeded();  // reboots into the new firmware if there is one
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  return true;
}

// draws image idx (newest-first). The first image gets the full refresh, and
// only it carries the badge
void drawIndex(int idx, int badge) {
  static bool logged = false;
  if (!logged) {
    logged = true;
    Serial.printf("First draw starts %u ms after wake\n", (unsigned)millis());
  }
  showImage(storedIds[idx], idx + 1, storedIds.size(), idx == 0, idx == 0 ? badge : 0);
  rtcViewIdx = idx;
  cycleOk = true;
}

// quick pass through the UNSEEN images only (never viewed with the button),
// newest to oldest, then rest on the newest with the unseen-count badge. Images
// already viewed are skipped but stay reachable with the button; unviewed ones
// from earlier syncs are included, so nothing gets buried while nobody's home.
// A button press steps on and ends the pass; a sync request ends it too and is
// left for the caller. With nothing unseen it just draws the newest image once
void quickPass() {
  int total = storedIds.size();
  std::vector<int> pass;
  for (int i = 0; i < total; i++) {
    if (!storedSeen[i]) pass.push_back(i);
  }
  int badge = badgeCount();
  Serial.printf("%d unseen image(s), badge %d\n", (int)pass.size(), badge);

  if (pass.size() > 1 || (pass.size() == 1 && pass[0] != 0)) {
    for (size_t k = 0; k < pass.size(); k++) {
      drawIndex(pass[k], 0);
      if (waitForNext(STILL_HOLD_MS)) {
        replayRequested = false;  // during the pass any press just steps on
        markSeen(pass[k]);        // pressing means the user is here
        int next = (pass[k] + 1) % total;
        markSeen(next);
        drawIndex(next, next == 0 ? badgeCount() : 0);
        return;
      }
      if (syncRequested) return;
    }
    Serial.println("Pass complete, resting on the newest image (press the button to step)");
  }
  drawIndex(0, badge);
}

// redraws the image on screen as a still (first frame) with or without the
// "Syncing..." badge, as a fast partial refresh. The panel can't be updated around one
// spot without the picture, so the picture is re-read and drawn again
void redrawCurrent(bool syncBadge) {
  if (storedIds.empty()) return;
  File f = LittleFS.open("/" + storedIds[rtcViewIdx], "r");
  EpdImage img;
  bool ok = f && epdParse(f, img) && epdReadFrame(f, img, 0, frameBuf);
  if (f) f.close();
  if (ok) drawFrame(img, true, rtcViewIdx == 0 ? badgeCount() : 0, syncBadge);
}

// pressing NEXT means the user is here: what's on screen counts as viewed, and
// so does the next image, which is drawn. The badge reappears when you wrap
// back to the newest, showing how many images are still unviewed
void stepNext() {
  int total = storedIds.size();
  markSeen(rtcViewIdx);
  int next = (rtcViewIdx + 1) % total;
  markSeen(next);
  drawIndex(next, next == 0 ? badgeCount() : 0);
}

// plays the animation on screen again (it rests on its first frame afterwards)
void replayCurrent() {
  showImage(storedIds[rtcViewIdx], rtcViewIdx + 1, storedIds.size(), false, rtcViewIdx == 0 ? badgeCount() : 0);
  cycleOk = true;
}

// the setup network with the QR screen, one attempt of PORTAL_TIMEOUT_S.
// True if someone joined it and the lamp is now online
bool openSetupPortal() {
  String apName = setupApName();
  WiFiManager wm;
  wm.setConfigPortalTimeout(PORTAL_TIMEOUT_S);
  wm.setTitle("Candlelight");
  Serial.println("Setup mode: join " + apName);
  drawSetupScreen(apName);
  bool online = wm.startConfigPortal(apName.c_str());
  // if nobody came, manualSync() puts the image back when it finishes
  return online;
}

// 10 s hold: "sync now" for the impatient. Same sync a timer wake does, plus a
// retry of a blocklisted firmware. The setup portal opens only if WiFi fails
// to connect on two of these in a row, so a lamp with working WiFi never sees it
void manualSync() {
  Serial.println("NEXT held 10s: syncing with the worker");
  if (rtcViewIdx >= (int)storedIds.size()) rtcViewIdx = 0;
  redrawCurrent(true);  // "Syncing..." top-left: the hold registered
  otaguard::clearBad();
  int added = 0;
  if (!doSync(false, added)) {
    rtcManualFails++;
    Serial.printf("Manual sync couldn't connect (%d in a row)\n", rtcManualFails);
    if (rtcManualFails >= 2) {
      rtcManualFails = 0;
      if (openSetupPortal()) doSync(false, added);  // new network saved: sync over it
    }
  }
  loadStoredIds();
  nextPressed = false;
  syncRequested = false;
  if (rtcViewIdx >= (int)storedIds.size()) rtcViewIdx = 0;
  if (!storedIds.empty() && countUnseen() > 0) quickPass();  // redraws everything, so the badge goes with it
  else redrawCurrent(false);                                 // nothing new: just take the badge away
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
      if (!storedIds.empty()) {
        if (storedAnimated[rtcViewIdx]) {
          markSeen(rtcViewIdx);
          replayCurrent();
        } else {
          stepNext();  // nothing to replay on a still: it's just a step
        }
      }
      last = millis();
    } else if (pressed) {
      if (!storedIds.empty()) stepNext();
      last = millis();
    }
  }
}

[[noreturn]] void goToSleep() {
  if (cycleOk) otaguard::markValid();  // the cycle did its job: keep this firmware
  else otaguard::beforeSleep();        // it didn't (say, no WiFi): a rollback proves nothing about it

  saveSeenFlags();

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
    rtcViewIdx = 0;
    rtcManualFails = 0;
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

  // power-up/reset: sync, then the quick pass. Timer: sync, and the pass only if there's news
  Serial.printf("Firmware: %s\n", FW_VERSION);
  int added = 0;
  doSync(!timerWake, added);
  loadStoredIds();

  // power-up always shows something (the panel's state is unknown); a timer wake only when images arrived
  if (!storedIds.empty() && (!timerWake || added > 0)) {
    quickPass();
    idleWindow();
  } else if (otaguard::updatePending() && !storedIds.empty()) {
    // first run of a new firmware with nothing new to show: still prove the display path
    Serial.println("First run of a new firmware: proving the display path before keeping it");
    drawIndex(rtcViewIdx, rtcViewIdx == 0 ? badgeCount() : 0);
  }
  goToSleep();
}

void loop() {
  goToSleep();  // not reached; setup() always ends in deep sleep
}
