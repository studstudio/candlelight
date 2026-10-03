#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>

// Candlelight recovery app. It lives in the "factory" partition and only runs
// when neither OTA slot holds a bootable firmware (the lamp firmware erases
// otadata to send us here). It joins WiFi (reusing the saved network, or
// opening the same Candlelight-XXXX setup portal), downloads a firmware from
// the worker into an OTA slot, and reboots into it. Nothing else.

const char* BASE_URL = "https://candlelight.daniloinfinite.workers.dev";  // same worker as ../fw
const uint32_t RETRY_MS = 60000;

// the version the lamp firmware blocklisted after it failed (NVS shared with ../fw)
String badVersion() {
  Preferences p;
  p.begin("otaguard", true);
  String v = p.getString("bad", "");
  p.end();
  return v;
}

String setupApName() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char name[32];
  snprintf(name, sizeof(name), "Candlelight-%02X%02X", mac[4], mac[5]);
  return String(name);
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

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("*** Candlelight RECOVERY mode ***");

  WiFiManager wm;
  wm.setConnectTimeout(20);
  wm.setConfigPortalTimeout(180);
  wm.setTitle("Candlelight recovery");
  String ap = setupApName();
  while (!wm.autoConnect(ap.c_str())) Serial.println("No WiFi yet, retrying");
  Serial.println("Connected: " + WiFi.localIP().toString());
}

void loop() {
  JsonDocument info;
  if (getVersionInfo(info)) {
    String latest = info["version"] | "";
    String previous = info["previousVersion"] | "";
    String bad = badVersion();

    // newest firmware unless it's the one that failed; then the one before it
    const char* file = nullptr;
    String version;
    if (latest.length() && latest != bad) {
      file = "latest.bin";
      version = latest;
    } else if (previous.length() && previous != bad) {
      file = "previous.bin";
      version = previous;
    }

    if (file) {
      Serial.printf("Installing firmware %s (%s)\n", version.c_str(), file);
      WiFiClientSecure client;
      client.setInsecure();  // prototype only
      httpUpdate.rebootOnUpdate(true);
      t_httpUpdate_return ret = httpUpdate.update(client, String(BASE_URL) + "/firmware/" + file);
      if (ret == HTTP_UPDATE_FAILED) {
        Serial.printf("Install failed (%d): %s\n", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
      }
    } else {
      Serial.println("No usable firmware published yet");
    }
  }
  delay(RETRY_MS);
}
