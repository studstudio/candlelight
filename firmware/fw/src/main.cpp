#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "secrets.h"

const char* URL = "https://candlelight.daniloinfinite.workers.dev/";

void setup() {
  Serial.begin(115200);
  delay(1000);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting");
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 40) {
    delay(500);
    Serial.print(".");
    tries++;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nWiFi failed. Check name/password and that it's 2.4 GHz.");
    return;
  }
  Serial.println("\nConnected: " + WiFi.localIP().toString());

  WiFiClientSecure client;
  client.setInsecure();  // prototype only
  HTTPClient http;
  http.setTimeout(10000);
  http.begin(client, URL);
  int code = http.GET();
  if (code > 0) {
    Serial.printf("HTTP %d, %d bytes\n", code, http.getSize());
  } else {
    Serial.printf("Request failed: %s\n", http.errorToString(code).c_str());
  }
  http.end();
}

void loop() {
  Serial.printf("A0: %d mV\n", analogReadMilliVolts(A0));
  delay(1000);
}
