# Firmware notes

Working notes for the Candlelight firmware, kept in the repo so they survive a codespace reset.

## Flashing: where `merged.bin` lives

- `firmware/fw/merged.bin` is **always the newest CI-built merged image**. Flash it over USB at address `0x0`.
- When a newer build is downloaded from CI, first move the current `merged.bin` into `firmware/fw/old-versions/` (named `merged-<shortsha>.bin`), then put the new one at `firmware/fw/merged.bin`. Don't leave extra copies in other folders.
- `old-versions/` is git-ignored. `merged.bin` itself is ignored too.
- To see which commit a `merged.bin` contains: `strings merged.bin | grep -oE '[0-9a-f]{40}'`. It should match the latest CI run's commit.
- The file is about 9.4MB. Most of that is blank padding between the lamp firmware (at `0x10000`) and the recovery app (at `0x810000`); real content is about 2.1MB. OTA still downloads only the ~1.1MB firmware file.
- USB flashing needs the board in download mode (jumper D5 to GND, then RESET). Remove the jumper afterwards and check the button is back on D2 and GND.
- Why this rule exists: a stale `merged.bin` was flashed once because several copies of different ages sat in different folders.

## Deep-sleep wake cycle (implemented in `fw/src/main.cpp`, untested on hardware)

The lamp sleeps almost all the time (the e-ink panel keeps its image unpowered) and wakes for one of three reasons:

| Wake | What it does |
|---|---|
| Power-up / reset / crash / firmware update | connect (setup portal only if no WiFi is saved), check firmware, sync, quick 1 s pass through all images, rest on the newest with the new-image badge, stay up 15 s for the button, sleep |
| Sleep timer (every 10 min while prototyping) | connect with a 20 s timeout, sync, pass + badge only if new images arrived, sleep. No WiFi: skip the sync and sleep, never the portal |
| NEXT button | no WiFi: step to the next image (partial refresh; full refresh when wrapping to the newest), stay up 15 s after the last press, sleep |

- Firmware update check: once a day (every `FW_CHECK_EVERY_WAKES` timer wakes) and on every power-up/reset or manual sync. Image sync runs on every sync wake.
- Badge: one small dot per image downloaded but not yet stepped through, stacked in a column up the bottom-left corner (white ring so it reads over any image). It accumulates across wakes and clears on the first button press.
- NEXT button holds (timed from the moment the press began, even on a wake):
  - tap (under 2 s): next image
  - 2 s or more, then release: replay the animation on screen; on a still it is just a step
  - 10 s (fires while held): full sync including a firmware check, clears the blocklist, replays the pass. If WiFi can't connect it opens the setup portal
  - 8 s hold on BOOT (D5) forgets the saved WiFi
- Firmware download is our own loop in `downloadFirmware()` (not HTTPUpdate, whose stall handling can hang for many minutes): abandoned after 90 s total or 15 s without data, retried at the next check, and it is not counted as a crash.
- State kept across sleep in RTC memory: current image, unseen count, wake count. It is reset on a true power-on.
- Knobs at the top of `main.cpp`: `SYNC_INTERVAL_S` (600 s while prototyping; plan on 30 min or more for battery), `WIFI_TIMEOUT_MS`, `AWAKE_IDLE_MS`, `STILL_HOLD_MS`, `OTA_BUDGET_MS`, `OTA_STALL_MS`, `FULL_REFRESH_ON_WAKE`.
- Rollback guard hooks (`src/ota_guard.h`): `markValid()` runs before sleep when the wake reached the worker or drew something; otherwise `beforeSleep()` marks the cycle inconclusive so a bootloader rollback isn't blocklisted. A first run of a new firmware with nothing to show still redraws the current image to prove the display path. The 300 s watchdog is only a backstop: connect steps have their own short timeouts.

Still to verify on the board: partial refresh after deep sleep (panel initialised with `initial=false`; set `FULL_REFRESH_ON_WAKE` if it ghosts or glitches), button wake, and real sleep current.

Still to build: skip the update step when the battery is low (waiting for the battery pack on the ADC; the lamp runs on laptop USB power for now). A brownout during a new firmware's first wake looks like a failure and rolls back a good build (the 10 s hold clears the blocklist).

## Working agreement: commits and pushes

- Never commit or push without being asked.
- After editing tracked files, remind the user that changes are uncommitted and ask whether to commit and push, at natural stopping points. Reason: a config file was lost once in a codespace wipe because it only existed locally.
- A push to `main` also publishes a new firmware version through CI, and a lamp flashed with an older `merged.bin` will update itself over the air on its next boot.
