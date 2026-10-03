#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_task_wdt.h>

// Keeps a bad OTA update from taking the lamp out of action. Three layers:
//
// 1. Bootloader rollback. A freshly updated image boots "pending verify" and
//    is only kept once markValid() runs (a full wake cycle's work done: booted,
//    synced, drew). ANY reset before that, a crash or an unvalidated deep-sleep
//    wake included, makes the bootloader switch back to the previous slot.
//    This even covers a crash before setup() runs. The updater remembers which
//    version it was trying ("trying"), and the next boot of the old firmware
//    blocklists it ("bad") so it isn't downloaded again.
// 2. Crash-loop breaker. If the firmware crashes (panic or watchdog) 4 times
//    in a row without completing a wake cycle (markValid), it switches to the
//    other slot itself, or to the recovery app if that slot is empty.
// 3. Watchdog. A hang while awake becomes a reset, which feeds layers 1 and 2.
//
// Deep sleep: a wake from deep sleep is not a crash (ESP_RST_DEEPSLEEP isn't
// counted). Call beforeSleep() right before every esp_deep_sleep_start().
//
// Rollback state lives in NVS namespace "otaguard", which the recovery app
// reads too.

extern "C" bool verifyRollbackLater() { return true; }  // Arduino core: don't auto-validate at boot

namespace otaguard {

const uint8_t MAX_CRASHES = 4;
const uint32_t WATCHDOG_S = 300;                    // longer than any legitimate blocking step

static bool validated = false;  // per wake: RAM is cleared by every deep sleep
static bool pending = false;    // this boot is the first run of a freshly installed update

inline bool crashReset() {
  switch (esp_reset_reason()) {
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
      return true;
    default:
      return false;  // power-on, button, esp_restart() etc. aren't failures
  }
}

inline String getStr(const char* key) {
  Preferences p;
  p.begin("otaguard", true);
  String v = p.getString(key, "");
  p.end();
  return v;
}

inline void putStr(const char* key, const String& v) {
  Preferences p;
  p.begin("otaguard", false);
  p.putString(key, v);
  p.end();
}

inline String badVersion() { return getStr("bad"); }
inline void clearBad() { putStr("bad", ""); }

// the updater calls this just before it starts writing a new image
inline void setTrying(const String& version) {
  Preferences p;
  p.begin("otaguard", false);
  p.putString("trying", version);
  p.putBool("unsure", false);
  p.end();
}
inline void clearTrying() { putStr("trying", ""); }

inline void rollbackToOtherSlot(const char* version) {
  putStr("bad", version);
  const esp_partition_t* other = esp_ota_get_next_update_partition(nullptr);
  if (other && esp_ota_set_boot_partition(other) == ESP_OK) {  // fails if that slot holds no valid image
    Serial.println("Rolling back to the previous firmware");
    delay(200);
    ESP.restart();
  }
  const esp_partition_t* factory =
      esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
  const esp_partition_t* otadata =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
  if (factory && otadata) {  // with otadata erased the bootloader runs the factory (recovery) app
    Serial.println("No previous firmware, starting recovery app");
    esp_partition_erase_range(otadata, 0, otadata->size);
    delay(200);
    ESP.restart();
  }
  Serial.println("No rollback target, carrying on");
}

// first thing in setup()
inline void begin(const char* version) {
  esp_task_wdt_init(WATCHDOG_S, true);  // panic = reset
  enableLoopWDT();

  esp_ota_img_states_t runState;
  pending = esp_ota_get_state_partition(esp_ota_get_running_partition(), &runState) == ESP_OK &&
            runState == ESP_OTA_IMG_PENDING_VERIFY;

  Preferences p;
  p.begin("otaguard", false);

  // did the bootloader just throw out the version we were trying to install?
  String trying = p.getString("trying", "");
  if (trying.length()) {
    esp_ota_img_states_t st;
    const esp_partition_t* other = esp_ota_get_next_update_partition(nullptr);
    if (trying == version) {
      p.putString("trying", "");
    } else if (other && esp_ota_get_state_partition(other, &st) == ESP_OK && st == ESP_OTA_IMG_ABORTED) {
      if (p.getBool("unsure", false)) {
        // it went to sleep without finishing a cycle (e.g. no WiFi), so the
        // rollback proves nothing about the firmware: try again another time
        Serial.printf("Firmware %s was rolled back after an inconclusive cycle, will retry\n", trying.c_str());
      } else {
        Serial.printf("Firmware %s failed to start and was rolled back, blocklisting it\n", trying.c_str());
        p.putString("bad", trying);
      }
      p.putString("trying", "");
      p.putBool("unsure", false);
    }
  }

  uint8_t crashes = p.getUChar("crashes", 0);
  if (crashReset()) {
    crashes++;
    p.putUChar("crashes", crashes);
    Serial.printf("Previous run crashed (%u in a row)\n", crashes);
  }
  p.end();

  if (crashes >= MAX_CRASHES) {
    Serial.printf("Crash loop on firmware %s\n", version);
    rollbackToOtherSlot(version);
  }
}

// true on the first wake after an update, until markValid(). A wake that has
// nothing new to show should still draw the current image when this is set, so
// the display code is proven before the update is kept
inline bool updatePending() { return pending; }

// this wake cycle did its job (booted, synced, drew what it had to): keep this
// firmware and forget earlier crashes
inline void markValid() {
  if (validated) return;
  validated = true;
  pending = false;
  esp_ota_mark_app_valid_cancel_rollback();  // harmless if the image wasn't pending
  Preferences p;
  p.begin("otaguard", false);
  if (p.getUChar("crashes", 0)) p.putUChar("crashes", 0);
  if (p.getBool("unsure", false)) p.putBool("unsure", false);
  p.end();
}

// call right before esp_deep_sleep_start(). If the cycle never got far enough
// to markValid() (say the router was down), an update that is still pending
// will be rolled back by the bootloader on wake; this notes that the rollback
// says nothing against the firmware, so it isn't blocklisted
inline void beforeSleep() {
  if (validated) return;
  Preferences p;
  p.begin("otaguard", false);
  p.putBool("unsure", true);
  p.end();
}

// call regularly from the main task: feeds the watchdog
inline void tick() { esp_task_wdt_reset(); }

}  // namespace otaguard
