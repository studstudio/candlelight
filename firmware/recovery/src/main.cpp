#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>
#include <esp_ota_ops.h>
#include <esp_wifi.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>

// Candlelight recovery app. It lives in the "factory" partition and only runs
// when neither OTA slot holds a bootable firmware (the lamp firmware erases
// otadata to send us here, or the bootloader rolled back its last slot). It
// joins WiFi using the saved network, downloads a firmware from the worker
// into an OTA slot, and reboots into it. If anything fails it deep-sleeps and
// tries again later rather than sitting awake on WiFi. The setup portal opens
// only when a person is there (power-on, reset button or the NEXT button):
// a lamp must never wake on its own just to broadcast a portal nobody sees.

const char* BASE_URL = "https://candlelight.daniloinfinite.workers.dev";  // same worker as ../fw

const uint32_t WIFI_TIMEOUT_MS = 30000;          // [PLACEHOLDER] how long to wait for the saved WiFi to connect
const uint32_t OTA_BUDGET_MS = 10UL * 60 * 1000;  // [PLACEHOLDER] this is the lamp's only way back, so be patient
const uint32_t OTA_STALL_MS = 30000;              // [PLACEHOLDER] ...but a connection that stops delivering data is dead
const int NEXT_BUTTON = 25;                       // GPIO25 (labelled D2), same button as the lamp firmware
const uint32_t PORTAL_SECONDS = 180;              // [PLACEHOLDER] how long the setup portal stays open
const uint32_t RETRY_SLEEP_S = 300;               // [PLACEHOLDER] after a failed attempt, sleep this long before the next
const uint32_t WIFI_FAIL_SLEEP_S = 1800;          // [PLACEHOLDER] WiFi won't connect (or a portal nobody used): wait longer, it's expensive
const uint32_t IDLE_SLEEP_S = 1800;               // [PLACEHOLDER] nothing safe to install: just poll for a new build now and then

// ---- shared with the lamp firmware (NVS namespace "otaguard") ----
String getStr(const char* key) {
  Preferences p;
  p.begin("otaguard", true);
  String v = p.getString(key, "");
  p.end();
  return v;
}
void putStr(const char* key, const String& v) {
  Preferences p;
  p.begin("otaguard", false);
  p.putString(key, v);
  p.end();
}

// versions that failed on this lamp: comma-separated list, newest last (same format as the lamp firmware)
const int MAX_BAD = 4;
bool isBad(const String& version) {
  return version.length() && ("," + getStr("bad") + ",").indexOf("," + version + ",") >= 0;
}
void addBad(const String& version) {
  String list = getStr("bad");
  if (("," + list + ",").indexOf("," + version + ",") >= 0) return;
  list += (list.length() ? "," : "") + version;
  int n = 1;
  for (char c : list) n += (c == ',');
  while (n-- > MAX_BAD) list.remove(0, list.indexOf(',') + 1);
  putStr("bad", list);
}

// if the firmware we installed last time was thrown out by the bootloader (it
// crashed before proving itself), blocklist it so we don't install it again
void noteRolledBackInstall() {
  String trying = getStr("trying");
  if (trying.isEmpty()) return;
  for (esp_partition_subtype_t sub : {ESP_PARTITION_SUBTYPE_APP_OTA_0, ESP_PARTITION_SUBTYPE_APP_OTA_1}) {
    const esp_partition_t* part = esp_partition_find_first(ESP_PARTITION_TYPE_APP, sub, nullptr);
    esp_ota_img_states_t st;
    if (part && esp_ota_get_state_partition(part, &st) == ESP_OK && st == ESP_OTA_IMG_ABORTED) {
      Serial.printf("Firmware %s was rolled back, blocklisting it\n", trying.c_str());
      addBad(trying);
      putStr("trying", "");
      return;
    }
  }
}

[[noreturn]] void sleepAndRetry(uint32_t seconds = RETRY_SLEEP_S) {
  Serial.printf("Sleeping %u s, then trying again\n", (unsigned)seconds);
  Serial.flush();
  WiFi.mode(WIFI_OFF);
  pinMode(NEXT_BUTTON, INPUT_PULLUP);  // a button still held would wake us again at once
  for (uint32_t t0 = millis(); digitalRead(NEXT_BUTTON) == LOW && millis() - t0 < 10000;) delay(20);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)NEXT_BUTTON, 0);  // a press brings us back, and counts as "someone is here"
  rtc_gpio_pullup_en((gpio_num_t)NEXT_BUTTON);
  rtc_gpio_pulldown_dis((gpio_num_t)NEXT_BUTTON);
  esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
  esp_deep_sleep_start();  // otadata is still erased, so the next boot is this app again
}

String setupApName() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char name[32];
  snprintf(name, sizeof(name), "Candlelight-%02X%02X", mac[4], mac[5]);
  return String(name);
}

bool hasSavedWiFi() {
  wifi_config_t conf;
  return esp_wifi_get_config(WIFI_IF_STA, &conf) == ESP_OK && conf.sta.ssid[0] != 0;
}

// the saved network with a timeout. If it won't connect (or none is saved) the
// setup portal opens, but only when someone is present (allowPortal). True once
// connected. The portal has no on-screen instructions here: join the
// "Candlelight-XXXX" network from a phone
bool connectWiFi(bool allowPortal) {
  WiFi.mode(WIFI_STA);
  if (hasSavedWiFi()) {
    WiFi.begin();
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_TIMEOUT_MS) delay(100);
    if (WiFi.status() == WL_CONNECTED) return true;
    Serial.println("Saved WiFi didn't connect");
  } else {
    Serial.println("No saved WiFi");
  }
  if (!allowPortal) return false;

  WiFiManager wm;
  wm.setConfigPortalTimeout(PORTAL_SECONDS);
  wm.setTitle("Candlelight recovery");
  String ap = setupApName();
  Serial.println("Setup portal: join " + ap);
  return wm.startConfigPortal(ap.c_str());
}

bool getVersionInfo(JsonDocument& doc) {
  WiFiClientSecure client;
  client.setInsecure();  // prototype only, like the lamp firmware
  HTTPClient http;
  http.setReuse(false);
  http.setTimeout(10000);
  http.begin(client, String(BASE_URL) + "/firmware/version");
  int code = http.GET();
  String body = code == 200 ? http.getString() : "";
  http.end();
  if (code != 200) {
    Serial.printf("Version check: HTTP %d\n", code);
    return false;
  }
  return !deserializeJson(doc, body);
}

// streams the firmware into the next OTA slot; gives up after OTA_BUDGET_MS in
// total or OTA_STALL_MS without data. true only if the whole image arrived,
// verified, and the new slot is selected. (Same loop as the lamp firmware's.)
bool downloadFirmware(WiFiClientSecure& client, const String& url) {
  HTTPClient http;
  http.setReuse(false);
  http.setTimeout(10000);
  http.begin(client, url);
  int code = http.GET();
  int len = http.getSize();
  if (code != 200 || len <= 0) {
    Serial.printf("  download: HTTP %d, length %d\n", code, len);
    http.end();
    return false;
  }
  if (!Update.begin(len)) {
    Serial.printf("  download: no room for %d bytes\n", len);
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  uint8_t buf[1460];
  int written = 0;
  uint32_t t0 = millis(), lastData = millis();
  const char* failure = nullptr;
  while (written < len && !failure) {
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
  if (!failure && !Update.end(true)) failure = "image rejected";
  if (failure) {
    Serial.printf("  download %s after %u of %d bytes (%u s)\n", failure, (unsigned)written, len,
                  (unsigned)((millis() - t0) / 1000));
    Update.abort();
    return false;
  }
  Serial.printf("  downloaded in %u s\n", (unsigned)((millis() - t0) / 1000));
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("*** Candlelight RECOVERY mode ***");
  noteRolledBackInstall();

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  esp_reset_reason_t reset = esp_reset_reason();
  // someone is here after a button press, a power-on or the reset button. Not after the
  // sleep timer, nor after the automatic restart that sends a crashing lamp into recovery
  bool person = cause == ESP_SLEEP_WAKEUP_EXT0 || reset == ESP_RST_POWERON || reset == ESP_RST_EXT;
  if (cause == ESP_SLEEP_WAKEUP_EXT0) rtc_gpio_deinit((gpio_num_t)NEXT_BUTTON);

  if (!connectWiFi(person)) {
    Serial.println(person ? "No WiFi, and nobody used the setup portal" : "No WiFi");
    sleepAndRetry(WIFI_FAIL_SLEEP_S);
  }
  Serial.println("Connected: " + WiFi.localIP().toString());

  JsonDocument info;
  if (!getVersionInfo(info)) sleepAndRetry();
  String latest = info["version"] | "";
  String previous = info["previousVersion"] | "";

  // newest firmware unless it's the one that failed; then the one before it
  const char* file = nullptr;
  String version;
  if (latest.length() && !isBad(latest)) {
    file = "latest.bin";
    version = latest;
  } else if (previous.length() && !isBad(previous)) {
    file = "previous.bin";
    version = previous;
  }
  if (!file) {
    Serial.println("No usable firmware: everything published is blocklisted (or nothing is published). Waiting for a new build");
    sleepAndRetry(IDLE_SLEEP_S);
  }

  Serial.printf("Installing firmware %s (%s)\n", version.c_str(), file);
  putStr("trying", version);  // lets us blocklist it next time if it gets rolled back
  WiFiClientSecure client;
  client.setInsecure();  // prototype only
  if (downloadFirmware(client, String(BASE_URL) + "/firmware/" + file)) {
    Serial.println("Installed, rebooting into it");
    Serial.flush();
    delay(200);
    ESP.restart();
  }
  putStr("trying", "");
  sleepAndRetry();
}

void loop() {}
