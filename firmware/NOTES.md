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

## Planned: deep-sleep wake cycle

The lamp is meant to be very low power: wake briefly, sync with the worker, show any new images, then deep sleep. It does not deep-sleep yet (it runs always-on while prototyping). When the sleep cycle is added:

- Check for new firmware only **once a day** (last-check time in NVS or RTC memory), not on every wake, because the HTTPS version check keeps the radio on. Sync images on every wake as now.
- Call `otaguard::markValid()` once the wake cycle's work is done, **including a sync that found nothing to draw**.
- Call `otaguard::beforeSleep()` right before `esp_deep_sleep_start()`. A deep-sleep wake is not counted as a crash, but the bootloader rolls back an unvalidated update on any reset, and this call stops that rollback from blocklisting a good build.
- On the first wake after an update (`otaguard::updatePending()`), redraw the current image even if nothing is new, so the display path is proven before the update is kept.
- Don't let a stuck WiFi portal keep the lamp awake. The 300 s watchdog is only a backstop.
- Later: skip the OTA step when the battery is low. A brownout during a new firmware's first wake looks like a failure and rolls back a good build. The 4 s NEXT-button hold clears the blocklist.

The guard code is in `firmware/fw/src/ota_guard.h`.

## Working agreement: commits and pushes

- Never commit or push without being asked.
- After editing tracked files, remind the user that changes are uncommitted and ask whether to commit and push, at natural stopping points. Reason: a config file was lost once in a codespace wipe because it only existed locally.
- A push to `main` also publishes a new firmware version through CI, and a lamp flashed with an older `merged.bin` will update itself over the air on its next boot.
