#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <GxEPD2_BW.h>
#include <vector>
#include <memory>
#include <algorithm>
#include <WiFiManager.h>
#include <esp_wifi.h>
#include <ping/ping_sock.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include <qrcode.h>
#include "epd_png.h"
#include "ota_guard.h"
#include "tls_resume.h"

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

const uint32_t STILL_HOLD_MS = 0;      // [PLACEHOLDER] extra time each image stays up during the automatic pass (0: the next goes up as soon as it is in)
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

String workerHost() {
  return String(BASE_URL).substring(8);  // after "https://"
}

// the TLS session of the last connection to the worker (see tls_resume.h).
// RTC memory: kept through deep sleep, cleared by power loss (then the next
// handshake is a full one)
RTC_DATA_ATTR uint8_t rtcTlsSession[1024];
RTC_DATA_ATTR uint32_t rtcTlsSessionLen = 0;

// connects to the worker, resuming the last TLS session when the server still
// accepts it, and logs how long the handshake took
bool connectWorker(ResumableTLS& c, const char* what) {
  size_t len = rtcTlsSessionLen;
  bool offered = len > 0;
  uint32_t t0 = millis();
  bool ok = c.connectResumable(workerHost().c_str(), 443, rtcTlsSession, len, sizeof(rtcTlsSession));
  rtcTlsSessionLen = ok ? len : 0;  // a failed connect forgets the session: the next one starts clean
  Serial.printf("%s: TLS handshake %u ms (%s)%s\n", what, (unsigned)(millis() - t0),
                offered ? "saved session offered" : "full, no saved session", ok ? "" : " FAILED");
  return ok;
}

// Our own minimal HTTP/1.1 request on an open connection: sends the request in
// one write, then reads the status line and headers. Returns the status code
// (0 on failure); contentLength gets the body length (-1 if not given). The
// body is left on the connection for the caller. The Arduino HTTPClient took
// ~1.5 s per request (5 s for an ack) on this lamp, where a raw request gets
// its first byte back in ~50 ms (measured by the link test)
int rawRequest(Client& c, const char* method, const String& path, const String& body, long& contentLength,
               uint32_t timeoutMs = 10000) {
  String req = String(method) + " " + path + " HTTP/1.1\r\nHost: " + workerHost() + "\r\nConnection: keep-alive\r\n";
  if (body.length()) req += "Content-Type: application/json\r\nContent-Length: " + String(body.length()) + "\r\n";
  req += "\r\n" + body;
  if (c.write((const uint8_t*)req.c_str(), req.length()) != req.length()) return 0;
  int code = 0;
  bool statusLine = true;
  contentLength = -1;
  String line;
  uint32_t last = millis();
  while (millis() - last < timeoutMs) {
    int ch = c.read();
    if (ch < 0) {
      if (!c.connected()) return 0;
      delay(1);
      continue;
    }
    last = millis();
    if (ch != '\n') {
      if (ch != '\r') line += (char)ch;
      continue;
    }
    if (statusLine) {  // "HTTP/1.1 200 OK"
      int sp = line.indexOf(' ');
      code = sp > 0 ? line.substring(sp + 1).toInt() : 0;
      statusLine = false;
    } else if (!line.length()) {
      return code;  // blank line: the headers are done
    } else {
      line.toLowerCase();
      if (line.startsWith("content-length:")) contentLength = line.substring(15).toInt();
    }
    line = "";
  }
  return 0;
}

// Images received this wake stay in PSRAM until sleep, so they can be drawn at
// once while the background writer (see syncBundle) is still putting them on flash
struct MemImage { String id; uint8_t* data; size_t size; };
std::vector<MemImage> memImages;

const MemImage* findMemImage(const String& id) {
  for (const MemImage& m : memImages) {
    if (m.id == id) return &m;
  }
  return nullptr;
}

// reads an image held in memory through the same calls as a File, for epdParse/epdReadFrame
struct MemFile {
  const uint8_t* p;
  size_t n;
  size_t pos = 0;
  MemFile(const uint8_t* data, size_t len) : p(data), n(len) {}
  size_t size() const { return n; }
  bool seek(uint32_t to) {
    if (to > n) return false;
    pos = to;
    return true;
  }
  size_t read(uint8_t* buf, size_t len) {
    len = min(len, n - pos);
    memcpy(buf, p + pos, len);
    pos += len;
    return len;
  }
  void close() {}
  explicit operator bool() const { return p != nullptr; }
};

// while the background writer runs, the manifest on flash is behind: the
// up-to-date one is this copy in memory, and loadManifest() hands it out instead
JsonDocument memManifest;
volatile bool memManifestLive = false;

bool loadManifest(JsonDocument& doc) {
  if (memManifestLive) {
    doc.set(memManifest);
    return true;
  }
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

// the manifest is kept oldest first. Ids start with the send time in ms (same
// number of digits), so sorting by id sorts by time, whatever order a sync
// delivered the images in. out gets the sorted copy; doc itself is left alone
// (callers hold JsonArray references into it)
void sortedManifest(JsonDocument& doc, JsonDocument& out) {
  JsonArray arr = doc.as<JsonArray>();
  std::vector<std::pair<String, JsonObject>> order;
  for (JsonObject e : arr) order.push_back({e["id"].as<String>(), e});
  std::sort(order.begin(), order.end(), [](const std::pair<String, JsonObject>& a, const std::pair<String, JsonObject>& b) {
    return a.first < b.first;
  });
  JsonArray o = out.to<JsonArray>();
  for (auto& p : order) o.add(p.second);
}

bool saveManifest(JsonDocument& doc) {
  JsonDocument sorted;
  sortedManifest(doc, sorted);
  File f = LittleFS.open(MANIFEST_PATH, "w");
  if (!f) return false;
  serializeJson(sorted, f);
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

// device holds MAX_IMAGES at most: the oldest image the user has already viewed
// makes room for a new one (an unviewed one only if all are unviewed). Takes it
// out of the manifest and returns its id ("" if there was room); the caller
// removes the file
String makeRoom(JsonArray stored) {
  if ((int)stored.size() < MAX_IMAGES) return "";
  size_t victim = 0;
  for (size_t i = 0; i < stored.size(); i++) {
    if (stored[i]["seen"] | true) { victim = i; break; }
  }
  String oldId = stored[victim]["id"] | "";
  stored.remove(victim);
  Serial.printf("  evicted: %s\n", oldId.c_str());
  return oldId;
}

void addEntry(JsonArray stored, const char* itemId, JsonVariantConst sentAt, JsonVariantConst geo, const EpdImage& img) {
  JsonObject entry = stored.add<JsonObject>();
  entry["id"] = itemId;
  entry["sentAt"] = sentAt;
  entry["geo"] = geo;
  entry["frames"] = img.frames;
  entry["intervalMs"] = img.intervalMs;
  entry["seen"] = false;  // not viewed with the button yet
}

// the image just written to TMP_PATH becomes a stored image: checks it, makes
// room if the device is full, renames it into place and adds it to the manifest.
// With `evicted` the caller saves the manifest once for a whole batch: evicted
// files are only listed there, to be removed after that save, so a power cut
// in between never leaves the manifest pointing at a missing file
CommitResult commitTmpItem(JsonDocument& manifestDoc, JsonArray stored, const char* itemId, JsonVariantConst sentAt,
                           JsonVariantConst geo, std::vector<String>* evicted = nullptr) {
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

  String oldId = makeRoom(stored);
  if (oldId.length()) {
    if (evicted) evicted->push_back(oldId);
    else LittleFS.remove("/" + oldId);
  }
  LittleFS.rename(TMP_PATH, "/" + String(itemId));
  addEntry(stored, itemId, sentAt, geo, img);
  if (evicted) return COMMIT_OK;  // the caller saves the manifest
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
      int n = stream->read(buf, min(avail, (int)sizeof(buf)));  // one bulk read (readBytes goes byte by byte)
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
    Serial.printf("Firmware %s failed on this lamp before, skipping (hold NEXT 5s to retry it)\n", workerFwVersion.c_str());
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

std::vector<String> storedIds;  // newest first (display order); the manifest itself is oldest first

std::vector<bool> storedAnimated;  // same order as storedIds
std::vector<bool> storedSeen;      // viewed with the button (an image only drawn by the quick pass doesn't count)
bool seenDirty = false;            // storedSeen changed and isn't in the manifest yet

// ---------- the quick pass, played while the sync downloads ----------
// After a sync the lamp shows the images that sync brought, OLDEST first,
// ending on the newest (image 1), each as a still (an animation's first frame;
// it plays when the user steps to it). They arrive oldest first, so each one is
// drawn as soon as it is in: the pass plays during the download instead of
// after it. passQueue holds the ids still to draw; passTick() (called from the
// download loop) draws the next once the one on screen has been up
// STILL_HOLD_MS. The last one waiting when nothing more is coming is held back:
// quickPass() puts the newest up as the resting image (with the badge)
bool speedTestPending = false;  // [DIAGNOSTIC] run the link test at the end of this wake (see speedTest)
std::vector<String> passQueue;
bool passRunning = false;     // a pass is playing (a press stops it)
bool passMoreComing = false;  // the sync still has images to deliver
int passDrawn = 0;
uint32_t passLastDraw = 0;
void passTick();

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
    passQueue.push_back(itemId);
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


uint32_t copyFlashUs = 0;  // time spent inside File::write by copyBytes (flash erase + program), to tell flash from network

// copies the next n bytes of the response into mem (if given) or f, or throws them away if both are null
bool copyBytes(WiFiClient* s, File* f, size_t n, uint32_t deadline, uint8_t* mem = nullptr) {
  uint8_t buf[1460];
  uint32_t last = millis();
  while (n > 0) {
    otaguard::tick();
    passTick();  // the next image of the pass goes up while this one downloads
    if (millis() > deadline || millis() - last > BUNDLE_STALL_MS) return false;
    int avail = s->available();
    if (!avail) {
      if (!s->connected()) return false;
      delay(2);
      continue;
    }
    size_t want = min((size_t)avail, n);
    if (!mem) want = min(want, sizeof(buf));
    // one bulk read: Stream::readBytes would fetch byte by byte, each byte two
    // calls into the TLS library (~30,000 per image)
    int got = s->read(mem ? mem : buf, want);
    if (got <= 0) continue;
    if (mem) {
      mem += got;
    } else if (f) {
      uint32_t tw = micros();
      size_t wrote = f->write(buf, got);
      copyFlashUs += micros() - tw;
      if (wrote != (size_t)got) return false;
    }
    n -= got;
    last = millis();
  }
  return true;
}

// confirms a batch of stored items to the worker in one request, on `client`.
// If the connection from the sync is still open it is reused, which saves a whole
// new secure handshake; otherwise HTTPClient simply opens a new one
bool ackOnClient(ResumableTLS& client, const std::vector<String>& ids) {
  if (ids.empty()) return true;
  JsonDocument doc;
  JsonArray arr = doc["ids"].to<JsonArray>();
  for (const String& id : ids) arr.add(id);
  String body;
  serializeJson(doc, body);
  if (client.connected()) {
    Serial.println("  ack: reusing the sync connection");
  } else if (!connectWorker(client, "  ack, new connection")) {
    return false;
  }
  long len;
  int code = rawRequest(client, "POST", String("/lamp/") + LAMP_ID + "/ack", body, len);
  client.stop();  // done with the connection
  return code == 200;
}

// the same on a brand-new connection (the fallback)
bool ackItems(const std::vector<String>& ids) {
  ResumableTLS client;
  return ackOnClient(client, ids);
}

// ---------- background flash writer ----------
// After a sync that held everything in PSRAM, a task on the other core writes
// the images to flash, saves the manifest once, removes evicted files, acks and
// turns WiFi off, while the main task is already drawing from PSRAM. Nothing is
// acked before the manifest is on flash, so a power cut still loses nothing:
// the worker just sends the images again.

// the sync's connection, kept open on the heap so the writer can ack on it
struct SyncConn {
  ResumableTLS client;
};

struct WriteJob {
  std::vector<MemImage> images;  // the data stays owned by memImages
  JsonDocument manifest;         // what the manifest on flash should become
  std::vector<String> evicted;   // files to remove once the manifest is saved
  std::vector<String> ackAlways; // already stored or unusable: acked whatever happens
  std::vector<String> ackNew;    // acked only once they are safely on flash
  SyncConn* conn;
};

volatile bool writerBusy = false;

void runWriteJob(WriteJob* job) {
  uint32_t t0 = millis();
  bool ok = true;
  for (const MemImage& m : job->images) {
    uint32_t t = millis();
    // straight to its final name: until the manifest lists it, a half-written
    // file is just an orphan that the next download of it overwrites
    File f = LittleFS.open("/" + m.id, "w");
    bool written = f && f.write(m.data, m.size) == m.size;  // one call: LittleFS writes whole blocks
    if (f) f.close();
    Serial.printf("[writer] %s: %u ms (%.1f KB)\n", m.id.c_str(), (unsigned)(millis() - t), m.size / 1024.0);
    if (!written) {
      ok = false;
      break;
    }
  }
  if (ok) ok = saveManifest(job->manifest);
  if (ok) {
    for (const String& id : job->evicted) LittleFS.remove("/" + id);
  } else {
    // the manifest on flash stays as it was, and unacked, the images come again
    // next sync. What is on screen now is still drawn from PSRAM
    Serial.println("[writer] flash write FAILED, the new images will be downloaded again");
  }
  memManifestLive = false;  // either way the manifest on flash is the truth again
  Serial.printf("[writer] flash done in %u ms (%u ms since wake)\n", (unsigned)(millis() - t0), (unsigned)millis());

  std::vector<String> ids = job->ackAlways;
  if (ok) ids.insert(ids.end(), job->ackNew.begin(), job->ackNew.end());
  if (!ids.empty()) {
    uint32_t tAck = millis();
    bool acked = ackOnClient(job->conn->client, ids);
    if (!acked) acked = ackItems(ids);
    Serial.printf("[writer] ack of %u item(s) %s in %u ms\n", (unsigned)ids.size(), acked ? "done" : "FAILED (will dedupe next sync)",
                  (unsigned)(millis() - tAck));
  }
  delete job->conn;
  delete job;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Serial.printf("[writer] finished, WiFi off (%u ms since wake)\n", (unsigned)millis());
  writerBusy = false;
}

void writerTask(void* arg) {
  runWriteJob((WriteJob*)arg);
  vTaskDelete(nullptr);
}

// anything that touches the manifest on flash (a new sync, saving seen flags,
// sleeping) waits for the writer first
void waitForWriter() {
  if (!writerBusy) return;
  uint32_t t0 = millis();
  while (writerBusy) {
    otaguard::tick();
    delay(10);
  }
  Serial.printf("Waited %u ms for the background flash write\n", (unsigned)(millis() - t0));
}

// downloads everything waiting in a single response. added = images stored
// (with PSRAM: drawable now, and on their way to flash in the background)
SyncResult syncBundle(int& added) {
  added = 0;
  std::unique_ptr<SyncConn> conn(new SyncConn);
  ResumableTLS& client = conn->client;
  uint32_t tStart = millis();
  if (!connectWorker(client, "Sync")) {
    Serial.println("Bundle sync: couldn't connect");
    return SYNC_FALLBACK;
  }
  bool tank = psramFound();
  uint32_t tGet = millis();
  long bodyLen;
  int code = rawRequest(client, "GET", String("/lamp/") + LAMP_ID + "/sync", "", bodyLen);
  Serial.printf("Request to response headers %u ms (WiFi signal %d dBm)\n", (unsigned)(millis() - tGet), (int)WiFi.RSSI());
  if (code != 200) {
    Serial.printf("Bundle sync: HTTP %d\n", code);
    return SYNC_FALLBACK;
  }
  WiFiClient* stream = &client;
  uint32_t deadline = millis() + BUNDLE_BUDGET_MS;

  String line;
  JsonDocument head;
  if (!readLineFrom(stream, line) || deserializeJson(head, line) || (head["v"] | 0) != 1) {
    Serial.println("Bundle sync: unreadable header");
    return SYNC_FALLBACK;
  }
  workerFwVersion = head["firmware"]["version"] | "";
  workerFwSize = head["firmware"]["size"] | 0;
  int count = head["count"] | 0;
  Serial.printf("Bundle: %d item(s) waiting (header after %u ms)\n", count, (unsigned)(millis() - tStart));
  passMoreComing = count > 0;

  JsonDocument manifestDoc;
  loadManifest(manifestDoc);
  JsonArray stored = manifestDoc.as<JsonArray>();
  Serial.printf("Device: %u image(s) stored\n", (unsigned)stored.size());
  LittleFS.remove(TMP_PATH);  // leftover from an interrupted sync

  std::vector<String> toAck;  // everything we're finished with, stored or not
  SyncResult result = SYNC_OK;

  // With PSRAM the images are received into memory at network speed. Written
  // while receiving, every 4 KB flash erase (~45 ms, flash cache off, so the
  // WiFi/TCP code mostly stalls too) also stalled the download. Without PSRAM,
  // or when it is full, an image is streamed straight to flash as before
  struct Held { String id; uint8_t* data; size_t size; JsonDocument meta; };
  std::vector<Held> held;  // oldest first, as they arrive
  uint32_t netMs = 0, flashMs = 0;
  // the synchronous way (PSRAM full mid-stream, or the stream broke off): writes
  // the held images to flash now, commits them with a single manifest save, and frees them
  auto flushHeld = [&]() -> bool {
    bool ok = true;
    std::vector<String> evicted, done;
    int batchAdded = 0;
    for (Held& h : held) {
      if (ok) {
        uint32_t t0 = millis();
        File f = LittleFS.open(TMP_PATH, "w");
        bool written = f && f.write(h.data, h.size) == h.size;
        if (f) f.close();
        uint32_t tWritten = millis();
        CommitResult cr = written ? commitTmpItem(manifestDoc, stored, h.id.c_str(), h.meta["sentAt"], h.meta["sentGeo"], &evicted)
                                  : COMMIT_FAIL;
        if (cr == COMMIT_FAIL) {
          Serial.printf("- %s: flash write failed\n", h.id.c_str());
          LittleFS.remove(TMP_PATH);
          ok = false;
        } else {
          if (cr == COMMIT_OK) batchAdded++;
          done.push_back(h.id);
          Serial.printf("- %s: flash write %u ms (%.1f KB), validate+store %u ms\n", h.id.c_str(), (unsigned)(tWritten - t0),
                        h.size / 1024.0, (unsigned)(millis() - tWritten));
        }
        flashMs += millis() - t0;
      }
      for (size_t i = 0; i < memImages.size(); i++) {  // from now on it is drawn from flash
        if (memImages[i].data == h.data) {
          memImages.erase(memImages.begin() + i);
          break;
        }
      }
      free(h.data);
    }
    held.clear();
    // what was stored only counts (and is acked) once the manifest lists it.
    // Renamed files a failed save leaves behind are overwritten when they come again
    uint32_t t0 = millis();
    if (done.empty() || saveManifest(manifestDoc)) {
      for (const String& id : evicted) LittleFS.remove("/" + id);
      added += batchAdded;
      toAck.insert(toAck.end(), done.begin(), done.end());
      Serial.printf("Manifest saved once for %u image(s), %u evicted, in %u ms\n", (unsigned)done.size(),
                    (unsigned)evicted.size(), (unsigned)(millis() - t0));
    } else {
      Serial.println("  manifest write failed");
      loadManifest(manifestDoc);  // back to what is really on flash
      stored = manifestDoc.as<JsonArray>();
      ok = false;
    }
    flashMs += millis() - t0;
    return ok;
  };

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
    uint8_t* mem = tank && size ? (uint8_t*)ps_malloc(size) : nullptr;
    if (mem) {
      bool got = copyBytes(stream, nullptr, size, deadline, mem);
      uint32_t ms = millis() - tItem;
      netMs += ms;
      if (!got) {
        Serial.println("  download incomplete");
        free(mem);
        result = SYNC_PARTIAL;
        break;
      }
      Serial.printf("  received into PSRAM: %u ms (%.1f KB, %.0f KB/s)\n", (unsigned)ms, size / 1024.0,
                    ms ? size / 1.024 / ms : 0.0);
      held.push_back({itemId, mem, size, std::move(item)});
      MemFile mf(mem, size);
      EpdImage img;
      if (epdParse(mf, img)) {  // an unusable one is discarded after the stream
        memImages.push_back({itemId, mem, size});
        passQueue.push_back(itemId);
      }
      passMoreComing = k + 1 < count;
      passTick();
      continue;
    }
    // PSRAM missing or full: what is held goes to flash first, so images stay in arrival order
    if (!held.empty()) {
      Serial.println("  PSRAM full, writing what is held");
      if (!flushHeld()) { result = SYNC_PARTIAL; break; }
      tItem = millis();
    }
    File f = LittleFS.open(TMP_PATH, "w");
    if (!f) { result = SYNC_PARTIAL; break; }
    copyFlashUs = 0;
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
    if (cr == COMMIT_OK) {
      added++;
      passQueue.push_back(itemId);
    }
    toAck.push_back(itemId);  // stored, or unusable: either way it's done with
    passMoreComing = k + 1 < count;
    uint32_t dl = tDownloaded - tItem;
    double kb = size / 1024.0;
    uint32_t itemFlashMs = copyFlashUs / 1000;
    uint32_t itemNetMs = dl > itemFlashMs ? dl - itemFlashMs : 0;
    Serial.printf("  timing: download %u ms (%.1f KB, %.0f KB/s) = network %u + flash writes %u, validate+store %u ms\n", (unsigned)dl, kb,
                  dl ? kb * 1000.0 / dl : 0.0, (unsigned)itemNetMs, (unsigned)itemFlashMs, (unsigned)(millis() - tDownloaded));
    netMs += itemNetMs;
    flashMs += millis() - tItem - itemNetMs;
  }
  // a sync that broke off may have left unread bytes: its ack goes on a new connection
  if (result != SYNC_OK) client.stop();
  passMoreComing = false;

  if (!held.empty() && result == SYNC_OK) {
    // the fast way: check and list the held images in memory (no flash), hand
    // the flash work and the ack to the writer task, and return so drawing can start
    WriteJob* job = new WriteJob;
    for (Held& h : held) {
      MemFile mf(h.data, h.size);
      EpdImage img;
      if (!epdParse(mf, img)) {
        // can never be shown, and leaving it queued would block everything behind it
        Serial.printf("- %s: no valid epRb/epRa payload, discarding\n", h.id.c_str());
        free(h.data);
        job->ackAlways.push_back(h.id);
        continue;
      }
      String oldId = makeRoom(stored);
      if (oldId.length()) job->evicted.push_back(oldId);
      addEntry(stored, h.id.c_str(), h.meta["sentAt"], h.meta["sentGeo"], img);
      job->images.push_back({h.id, h.data, h.size});
      job->ackNew.push_back(h.id);
      added++;
    }
    held.clear();
    job->ackAlways.insert(job->ackAlways.end(), toAck.begin(), toAck.end());
    job->manifest.set(manifestDoc);
    sortedManifest(manifestDoc, memManifest);
    memManifestLive = true;
    job->conn = conn.release();
    writerBusy = true;
    // core 0 (WiFi's core), below WiFi's priority; the stack fits a fresh TLS handshake if the ack needs one
    if (xTaskCreatePinnedToCore(writerTask, "writer", 16384, job, 1, nullptr, 0) != pdPASS) {
      Serial.println("Couldn't start the background writer, writing now");
      runWriteJob(job);
    }
    Serial.printf("Sync phases: network %u ms; %d image(s) drawable from PSRAM now, flash and ack in the background\n",
                  (unsigned)netMs, added);
    return SYNC_OK;
  }

  // the stream is done (or broke off): whatever arrived whole is still stored and acked
  if (!held.empty()) {
    Serial.printf("Received %u image(s) into PSRAM, writing to flash\n", (unsigned)held.size());
    if (!flushHeld()) result = SYNC_PARTIAL;
  }
  Serial.printf("Sync phases: network %u ms, flash %u ms%s\n", (unsigned)netMs, (unsigned)flashMs, tank ? "" : " (no PSRAM)");

  if (!toAck.empty()) {
    uint32_t tAck = millis();
    bool acked = ackOnClient(client, toAck);
    if (!acked) {
      Serial.println("  ack on the open connection failed, retrying on a new one");
      acked = ackItems(toAck);
    }
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
// g is the display (library drawing) or a MonoCanvas (the fast path's picture)
void drawBoxedText(Adafruit_GFX& g, const char* txt, int x, int y) {
  g.setTextSize(2);
  g.setTextColor(GxEPD_BLACK);
  int16_t bx, by;
  uint16_t bw, bh;
  g.getTextBounds(txt, 0, 0, &bx, &by, &bw, &bh);
  const int pad = 5;
  int w = bw + 2 * pad, h = bh + 2 * pad;
  if (x < 0) x = g.width() + x - w;   // negative = measured from the right edge
  if (y < 0) y = g.height() + y - h;  // negative = measured from the bottom edge
  g.fillRect(x, y, w, h, GxEPD_WHITE);
  g.drawRect(x, y, w, h, GxEPD_BLACK);
  g.drawRect(x + 1, y + 1, w - 2, h - 2, GxEPD_BLACK);
  g.setCursor(x + pad - bx, y + pad - by);
  g.print(txt);
}

// the number of unseen images other than the one on screen, bottom-left corner
void drawBadge(Adafruit_GFX& g, int n) {
  char txt[8];
  snprintf(txt, sizeof(txt), "%d", n);
  drawBoxedText(g, txt, 6, -6);
}

// top-left corner: feedback that the 5 s sync hold registered
void drawSyncBadge(Adafruit_GFX& g) {
  drawBoxedText(g, "Syncing...", 6, 6);
}

// draws into a packed 1-bit picture laid out the way the panel takes it
// (row-major, MSB first, 1 = white), so badges go on the fast path too
class MonoCanvas : public Adafruit_GFX {
  uint8_t* buf;
 public:
  MonoCanvas(uint8_t* b, int16_t w, int16_t h) : Adafruit_GFX(w, h), buf(b) {}
  void drawPixel(int16_t x, int16_t y, uint16_t color) override {
    if (x < 0 || y < 0 || x >= width() || y >= height()) return;
    uint8_t& byte = buf[y * ((width() + 7) / 8) + (x >> 3)];
    uint8_t mask = 0x80 >> (x & 7);
    if (color == GxEPD_WHITE) byte |= mask;
    else byte &= ~mask;
  }
};

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
bool drawDirect(const EpdImage& img, bool partial, int badge, bool syncBadge) {
  const int W = GxEPD2_420_GDEY042T81::WIDTH, H = GxEPD2_420_GDEY042T81::HEIGHT;
  if (!FAST_DRAW || img.w != W || img.h != H) return false;
  uint32_t t0 = millis();
  const uint8_t* bitmap;
  if (img.bpp == 2) {
    ditherToMono(img, frameBuf, monoBuf);
    bitmap = monoBuf;
  } else if (img.bpp == 1 && badge <= 0 && !syncBadge) {
    bitmap = frameBuf;
  } else if (img.bpp == 1) {
    memcpy(monoBuf, frameBuf, sizeof(monoBuf));  // a badge goes on a copy, the frame stays as read
    bitmap = monoBuf;
  } else {
    return false;
  }
  if (badge > 0 || syncBadge) {
    MonoCanvas canvas(monoBuf, W, H);
    if (badge > 0) drawBadge(canvas, badge);
    if (syncBadge) drawSyncBadge(canvas);
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
  // full-size landscape frames (badges included) go straight to the panel; anything else uses the library
  if (drawDirect(img, partial, badge, syncBadge)) return;

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
    if (badge > 0) drawBadge(display, badge);
    if (syncBadge) drawSyncBadge(display);
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

// badge > 0 draws that number bottom-left (on an animation, on its resting first frame).
// F is a File, or a MemFile for an image still held in PSRAM
template <class F>
void showImageFrom(F& f, const char* from, const String& itemId, size_t num, size_t total, bool full, int badge) {
  EpdImage img;
  if (!f || !epdParse(f, img)) {
    Serial.printf("Can't open %s\n", itemId.c_str());
    if (f) f.close();
    return;
  }
  Serial.printf("Image %u/%u: %s (%u frame(s), %.1f KB, from %s)\n", (unsigned)num, (unsigned)total, itemId.c_str(),
                img.frames, f.size() / 1024.0, from);

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

void showImage(const String& itemId, size_t num, size_t total, bool full, int badge = 0) {
  if (const MemImage* m = findMemImage(itemId)) {
    MemFile mf(m->data, m->size);
    showImageFrom(mf, "PSRAM", itemId, num, total, full, badge);
    return;
  }
  File f = LittleFS.open("/" + itemId, "r");
  showImageFrom(f, "flash", itemId, num, total, full, badge);
}


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

// the next unseen image after idx (wrapping round), or -1 when there are none
int nextUnseenAfter(int idx) {
  int total = storedSeen.size();
  for (int k = 1; k <= total; k++) {
    int i = (idx + k) % total;
    if (i != idx && !storedSeen[i]) return i;
  }
  return -1;
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
  waitForWriter();  // the manifest on flash is the writer's until it is done
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
const uint32_t SYNC_HOLD_MS = 5000;          // [PLACEHOLDER] hold NEXT this long to re-check the worker for new images/firmware
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
RTC_DATA_ATTR bool rtcInTour = false; // NEXT is stepping through the unseen images (ends by wrapping to image 1)
RTC_DATA_ATTR int rtcBadge = 0;       // the number on the image on screen (redraws and replays show it again)
// what the last successful connect looked like, so the next wake can skip the channel scan and DHCP
RTC_DATA_ATTR uint8_t rtcWifiChannel = 0;  // 0 = nothing cached
RTC_DATA_ATTR uint8_t rtcBssid[6];
RTC_DATA_ATTR uint32_t rtcIp = 0, rtcGw = 0, rtcMask = 0, rtcDns = 0;
RTC_DATA_ATTR uint32_t rtcDhcpAgeS = 0;    // seconds since the cached address came from DHCP
RTC_DATA_ATTR int rtcManualFails = 0; // 5 s-hold syncs in a row that couldn't connect (the 2nd opens the setup portal)

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
// [DIAGNOSTIC] pings the router and the internet in the background (5 each)
// while the sync runs, to tell a slow WiFi/router from a slow internet path.
// Results print as they come in, tagged [ping router] / [ping internet]
void pingDiag(IPAddress target, const char* label) {
  esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
  IP_ADDR4(&cfg.target_addr, target[0], target[1], target[2], target[3]);
  cfg.count = 5;
  cfg.interval_ms = 300;
  cfg.timeout_ms = 2000;
  esp_ping_callbacks_t cbs = {};
  cbs.cb_args = (void*)label;
  cbs.on_ping_success = [](esp_ping_handle_t h, void* arg) {
    uint32_t ms = 0;
    esp_ping_get_profile(h, ESP_PING_PROF_TIMEGAP, &ms, sizeof(ms));
    Serial.printf("[ping %s] %u ms\n", (const char*)arg, (unsigned)ms);
  };
  cbs.on_ping_timeout = [](esp_ping_handle_t h, void* arg) { Serial.printf("[ping %s] timeout\n", (const char*)arg); };
  cbs.on_ping_end = [](esp_ping_handle_t h, void* arg) { esp_ping_delete_session(h); };
  esp_ping_handle_t h;
  if (esp_ping_new_session(&cfg, &cbs, &h) == ESP_OK) esp_ping_start(h);
}

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
    pingDiag(WiFi.gatewayIP(), "router");
    pingDiag(IPAddress(1, 1, 1, 1), "internet");
    return true;
  }
  Serial.println("WiFi didn't connect, skipping sync");
  return false;
}

// connects and downloads new images, then turns WiFi off (the background
// writer does that, after its ack, when it is still storing them). A newer
// firmware the worker reported is installed only at the end of the wake (see
// updateBeforeSleep), so nobody waits for it. False if it couldn't get online
bool doSync(bool portalIfUnconfigured, int& added) {
  added = 0;
  waitForWriter();  // an earlier sync this wake may still be storing
  passQueue.clear();  // the pass shows only what this sync brings
  passRunning = true;
  passMoreComing = false;
  passDrawn = 0;
  // its images are on flash now: their PSRAM copies make room for this sync's
  for (MemImage& m : memImages) free(m.data);
  memImages.clear();
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
  if (!writerBusy) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
  return true;
}

// the last thing a wake does, after the images were shown and the button window
// closed: installs the newer firmware a sync reported this wake (reconnecting,
// which with the cached access point takes ~0.1 s). Reboots into it if installed
void updateBeforeSleep() {
  if (workerFwVersion.isEmpty()) return;  // no sync reached the worker this wake
  if (workerFwVersion != FW_VERSION && !otaguard::isBad(workerFwVersion)) {
    Serial.println("Firmware update waiting: reconnecting to install it");
    if (!connectForSync(false)) return;  // retried at the next sync
  }
  updateFirmwareIfNeeded();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

// logs when the first image of this wake goes up (once)
void logFirstDraw() {
  static bool logged = false;
  if (logged) return;
  logged = true;
  Serial.printf("First draw starts %u ms after wake\n", (unsigned)millis());
}

// reads the first frame of image id into frameBuf (from PSRAM if it is still there)
bool loadFirstFrame(const String& id, EpdImage& img) {
  if (const MemImage* m = findMemImage(id)) {
    MemFile mf(m->data, m->size);
    return epdParse(mf, img) && epdReadFrame(mf, img, 0, frameBuf);
  }
  File f = LittleFS.open("/" + id, "r");
  bool ok = f && epdParse(f, img) && epdReadFrame(f, img, 0, frameBuf);
  if (f) f.close();
  return ok;
}

// draws image idx as a still: an animation shows its first frame without playing
void drawIndexStill(int idx, int badge, bool full) {
  logFirstDraw();
  EpdImage img;
  if (loadFirstFrame(storedIds[idx], img)) drawFrame(img, !full, badge);
  rtcViewIdx = idx;
  rtcBadge = badge;
  cycleOk = true;
}

// draws image idx (newest-first); full = flashing full refresh (clears ghosting)
void drawIndex(int idx, int badge, bool full) {
  logFirstDraw();
  showImage(storedIds[idx], idx + 1, storedIds.size(), full, badge);
  rtcViewIdx = idx;
  rtcBadge = badge;
  cycleOk = true;
}

// draws the next image of the pass if it is due (see passQueue). A press of
// the button ends the pass: the lamp goes straight to image 1 instead
void passTick() {
  if (!passRunning || passQueue.empty()) return;
  if (nextPressed || replayRequested || syncRequested) {
    passRunning = false;
    nextPressed = replayRequested = false;  // the press meant "skip the pass", not also "step"
    Serial.println("Pass stopped by the button");
    return;
  }
  if (passQueue.size() == 1 && !passMoreComing) return;  // the last one: it rests, drawn by quickPass()
  if (passDrawn > 0 && millis() - passLastDraw < STILL_HOLD_MS) return;
  String id = passQueue.front();
  passQueue.erase(passQueue.begin());
  logFirstDraw();
  EpdImage img;
  if (loadFirstFrame(id, img)) drawFrame(img, true);  // a still (an animation's first frame), fast partial refresh, no badge
  passDrawn++;
  Serial.printf("Auto pass, %s new image: %s (%u ms after wake)\n", passDrawn == 1 ? "oldest" : "next", id.c_str(),
                (unsigned)millis());
  passLastDraw = millis();  // the hold counts from when it is fully up
  cycleOk = true;
}

void stepNext();

// finishes the pass of the images this sync brought (oldest first; most of it
// already played during the download) and rests on the newest, image 1: a
// partial refresh, a still, and the badge counting every unseen image. A press
// ends the pass on image 1; a sync request ends it too and is left for the caller.
// With nothing new it just draws image 1
void quickPass() {
  passMoreComing = false;
  // what is left, minus the newest (it rests) and anything no longer stored
  std::vector<String> left;
  for (const String& id : passQueue) {
    if (id == storedIds[0]) continue;
    for (const String& s : storedIds) {
      if (s == id) {
        left.push_back(id);
        break;
      }
    }
  }
  passQueue = left;
  passQueue.push_back(storedIds[0]);  // held back by passTick: the resting image
  Serial.printf("%d unseen image(s), %d left to show before image 1\n", countUnseen(), (int)passQueue.size() - 1);
  while (passRunning && passQueue.size() > 1) {
    passTick();
    otaguard::tick();
    delay(20);
  }
  passRunning = false;
  passQueue.clear();
  if (syncRequested) return;
  int unseen = countUnseen();
  rtcInTour = unseen > 0;
  if (passDrawn > 0) {
    Serial.println("Auto pass complete, resting on image 1, the newest (press the button to step)");
    if (millis() - passLastDraw < STILL_HOLD_MS) delay(STILL_HOLD_MS - (millis() - passLastDraw));
  }
  drawIndexStill(0, unseen, false);  // fast partial refresh (after power-up the library makes the first one full itself)
}

// redraws the image on screen as a still (first frame) with or without the
// "Syncing..." badge, as a fast partial refresh. The panel can't be updated around one
// spot without the picture, so the picture is re-read and drawn again
void redrawCurrent(bool syncBadge) {
  if (storedIds.empty()) return;
  EpdImage img;
  if (loadFirstFrame(storedIds[rtcViewIdx], img)) drawFrame(img, true, rtcBadge, syncBadge);
}

// pressing NEXT means the user is here: what's on screen counts as viewed, and
// so does the image it steps to. While unseen images remain it steps only
// through those, the badge counting the unseen ones left including the one
// shown (4, 3, 2, 1); after the last one it wraps back to image 1 without a
// badge (the one full refresh), and from there it is plain 1, 2 ... 12, 1 again
// (all partial refreshes)
void stepNext() {
  int total = storedIds.size();
  markSeen(rtcViewIdx);
  int next = nextUnseenAfter(rtcViewIdx);
  bool full = false;
  if (next >= 0) {
    rtcInTour = true;
  } else if (rtcInTour) {
    rtcInTour = false;  // done with the unseen ones: back to the newest
    if (rtcViewIdx == 0) {
      // already on it (one new image, showing a "1"): this press only clears the
      // badge, a fast partial refresh of the same still; the next press goes on
      drawIndexStill(0, 0, false);
      return;
    }
    next = 0;
    full = true;  // the only full refresh: back on image 1 after flipping through the unseen ones
  } else {
    next = (rtcViewIdx + 1) % total;
  }
  int badge = rtcInTour ? countUnseen() : 0;  // counted before the one shown is marked seen
  markSeen(next);
  drawIndex(next, badge, full);
}

// plays the animation on screen again (it rests on its first frame afterwards)
void replayCurrent() {
  showImage(storedIds[rtcViewIdx], rtcViewIdx + 1, storedIds.size(), false, rtcBadge);
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

// 5 s hold: "sync now" for the impatient. Same sync a timer wake does, plus a
// retry of a blocklisted firmware. The setup portal opens only if WiFi fails
// to connect on two of these in a row, so a lamp with working WiFi never sees it
void manualSync() {
  Serial.printf("NEXT held %us: syncing with the worker\n", (unsigned)(SYNC_HOLD_MS / 1000));
  if (rtcViewIdx >= (int)storedIds.size()) rtcViewIdx = 0;
  redrawCurrent(true);  // "Syncing..." top-left: the hold registered
  syncRequested = false;  // handled: left set, the pass would read it as a press and stop
  speedTestPending = true;  // [DIAGNOSTIC] link test at the end of this wake
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
// can step through the images; a 2 s hold replays an animation, a 5 s hold syncs
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

// [DIAGNOSTIC, temporary] after a manual sync, at the very end of the wake
// (nobody waits for it): fetches 100 KB from the worker over HTTPS and then
// plain HTTP, with raw requests (no HTTPClient) and bulk reads, and logs the
// connect time, time to first byte and throughput of each. Tells the ESP32's
// TLS apart from its network stack. Remove once answered
void speedTestOne(Client& c, bool tls) {
  String host = workerHost();
  const char* name = tls ? "HTTPS" : "HTTP";
  uint32_t t0 = millis();
  if (tls ? !connectWorker((ResumableTLS&)c, "[speed HTTPS]") : !c.connect(host.c_str(), 80)) {
    Serial.printf("[speed %s] connect failed\n", name);
    return;
  }
  Serial.printf("[speed %s] connect %u ms\n", name, (unsigned)(millis() - t0));
  for (long n : {0L, 100000L}) {
    String req = "GET /speedtest?n=" + String(n) + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: keep-alive\r\n\r\n";
    uint32_t t1 = millis(), tFirst = 0, tBody = 0;
    c.write((const uint8_t*)req.c_str(), req.length());
    long len = -1, got = 0;
    bool headersDone = false;
    String line;
    uint8_t buf[2048];
    uint32_t last = millis();
    while (millis() - last < 10000) {
      otaguard::tick();
      int a = c.available();
      if (a <= 0) {
        if (!c.connected()) break;
        delay(1);
        continue;
      }
      if (!tFirst) tFirst = millis();
      last = millis();
      if (!headersDone) {
        int ch = c.read();
        if (ch < 0) continue;
        if (ch == '\n') {
          if (!line.length()) {
            headersDone = true;
            tBody = millis();
            if (len <= 0) break;
          } else {
            String l = line;
            l.toLowerCase();
            if (l.startsWith("content-length:")) len = l.substring(15).toInt();
            line = "";
          }
        } else if (ch != '\r') {
          line += (char)ch;
        }
      } else {
        int r = c.read(buf, (size_t)min((long)sizeof(buf), len - got));
        if (r > 0) got += r;
        if (got >= len) break;
      }
    }
    uint32_t tEnd = millis();
    if (n == 0) {
      Serial.printf("[speed %s] empty response: first byte after %u ms\n", name, (unsigned)(tFirst ? tFirst - t1 : 0));
    } else {
      uint32_t bodyMs = tBody ? tEnd - tBody : 0;
      Serial.printf("[speed %s] %ld of %ld bytes: first byte after %u ms, body %u ms = %.0f KB/s\n", name, got, len,
                    (unsigned)(tFirst ? tFirst - t1 : 0), (unsigned)bodyMs, bodyMs ? got / 1.024 / bodyMs : 0.0);
    }
  }
  c.stop();
}

void speedTest() {
  Serial.println("[speed] link test (diagnostic)");
  if (!connectForSync(false)) return;
  {
    ResumableTLS sc;
    speedTestOne(sc, true);
  }
  {
    WiFiClient pc;
    speedTestOne(pc, false);
  }
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

[[noreturn]] void goToSleep() {
  waitForWriter();                     // the images must be on flash (and acked) before sleep wipes PSRAM
  if (cycleOk) otaguard::markValid();  // the cycle did its job: keep this firmware
  else otaguard::beforeSleep();        // it didn't (say, no WiFi): a rollback proves nothing about it

  saveSeenFlags();  // before a firmware update's reboot, too
  if (cycleOk && speedTestPending) speedTest();
  if (cycleOk) updateBeforeSleep();

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
    rtcInTour = false;
    rtcBadge = 0;
    rtcManualFails = 0;
  }
  Serial.printf("Wake: %s, PSRAM %u KB free\n", buttonWake ? "button" : timerWake ? "timer" : "power-up/reset",
                (unsigned)(ESP.getFreePsram() / 1024));
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
  if (!storedIds.empty() && (!timerWake || added > 0 || passDrawn > 0)) {  // a pass the sync started must end on image 1
    quickPass();
    idleWindow();
  } else if (otaguard::updatePending() && !storedIds.empty()) {
    // first run of a new firmware with nothing new to show: still prove the display path
    Serial.println("First run of a new firmware: proving the display path before keeping it");
    drawIndex(rtcViewIdx, rtcBadge, rtcViewIdx == 0);
  }
  goToSleep();
}

void loop() {
  goToSleep();  // not reached; setup() always ends in deep sleep
}
