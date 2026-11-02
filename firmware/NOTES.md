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
| Power-up / reset / crash / firmware update | connect (setup portal only if no WiFi is saved), sync, quick pass through the **unseen** images (just the newest if none are unseen), rest on the newest with the badge, stay up 30 s for the button, sleep |
| Sleep timer (every 10 min while prototyping) | connect with a 20 s timeout, sync, quick pass + badge only if new images arrived, sleep. No WiFi: skip the sync and sleep, never the portal |
| NEXT button | no WiFi: step to the next image (partial refresh; full refresh when wrapping to the newest), stay up 30 s after the last press, sleep |

- Firmware updates: the worker includes the current firmware version in its `/queue` response, so every sync wake learns about updates at no extra cost (no separate request). If the version differs and isn't blocklisted, the lamp installs it **at the very end of the wake** (`updateBeforeSleep()`): after the quick pass and the 30 s button window, right before sleep, so nobody waits for it. It reconnects for that (about 0.1 s with the cached access point), marks the current firmware valid and saves the seen flags first, then downloads and reboots into the new one. `GET /firmware/version` still exists for the recovery app.
- "Power-up" means a real power-on, a reset, a crash or the restart after an update. It does not mean a wake from deep sleep.
- Seen / unseen: every stored image has a `seen` flag in `manifest.json` (so it survives power loss). New downloads are **unseen**. An image becomes **seen** only when the user views it with the button: pressing NEXT marks the image on screen and the one it steps to as seen (a 2 s replay marks the current one). Being drawn by the automatic quick pass does not count. Images stored before this existed count as seen.
- Quick pass: after a sync that downloaded something it shows **only the images that sync brought, oldest first, ending on the newest (image 1)**. Each is a **still**: an animation shows only its first frame and plays later, when the user steps to it with the button. Every image in the pass, image 1 at the end included, is a fast partial refresh; image 1 carries the badge. Unseen images from earlier syncs are not in the pass but are counted by the badge and visited by the button tour. A press during the pass skips straight to image 1 (badge and all). On power-up with nothing new it just draws image 1.
- Badge: a small boxed **number** (white box, black border) in the bottom-left corner, counting the unseen images **including the one on screen**. After a pass with 4 new images the lamp rests on image 1 with a "4". Nothing is drawn when the count is 0. The number is remembered (`rtcBadge`) so a redraw or replay shows it again.
- Stepping through new images (the "tour", `rtcInTour`, kept across sleep): while unseen images remain, NEXT steps **only through the unseen ones**, newest to oldest, and the badge counts down (4, 3, 2, 1: the last unseen image shows a 1). The next press **wraps back to image 1** (no badge, full refresh) instead of carrying on through the older images. With a single new image (image 1 showing a "1") the first press **only clears the badge** (fast partial refresh, same image) and the second press goes to image 2. From there NEXT is plain 1, 2 ... 12, 1 again (a **full refresh on every wrap to image 1**, to clear ghosting; never after the auto pass); stepping to an animation plays it. Badges are drawn straight into the fast path's picture (`MonoCanvas`), so a badge doesn't slow a step down. (Dots were tried and dropped: not visible enough.)
- Sync feedback: when the 5 s hold fires, the lamp redraws the image on screen (as a still, first frame) with a boxed **"Syncing..."** badge in the top-left corner. When the sync ends it either plays the quick pass (which redraws everything) or redraws the image without the badge.
- Sync protocol (one connection): `GET /lamp/<id>/sync` returns the firmware version, the queue and every queued image in **one response** (a text header line, then per item a text header line and exactly `size` raw bytes). The lamp stores each image as it streams in, then confirms them all with one `POST /lamp/<id>/ack {"ids":[...]}`. That is 2 secure connections per sync however many images arrive, instead of 2 per image plus 1. Nothing is deleted from the queue until the ack. The bundle sends each image **without its PNG preview** (the lamp never decodes it; it only reads the e-ink `epRa`/`epRb` chunks): about 63% fewer bytes for a still and 24% for an animation, which matters most for flash writes. The worker fetches all the images in parallel and builds the response in one go. The ack goes out on the sync's already-open connection (a new handshake only if that connection is gone). The `/items` fallback still serves full PNGs. Fallbacks: if `/sync` fails, or breaks off midway, the lamp finishes with the older per-image path (`/queue`, `/items/<id>`, `DELETE`), which still works against any worker. The lamp no longer needs a separate queue read: the bundle header carries what it used to fetch from the queue. **Streamed:** at upload the worker records each image's stripped size (`lampSize` in the R2 metadata), so `/sync` knows the exact total up front: it sends the header at once and each image as soon as its fetch is done, with a real `Content-Length` (no chunked encoding, same bytes as before, so any firmware reads it). An item that vanished is sent as zeros of the promised size, which the lamp discards and acks. Items queued before `lampSize` existed get the old buffered response.
- Fast WiFi: each wake remembers the access point's channel and BSSID (and the IP, DNS and gateway) in RTC memory and reconnects without a channel scan; while the cached address is fresher than `DHCP_REUSE_S` it also skips DHCP. If the fast connect fails it falls back to a normal connect and clears the cache. WiFi modem power-save is off during the sync.
- TLS session resumption (`tls_resume.h`, `connectWorker()`): a full TLS handshake costs the ESP32 ~1.6 s. After one, Cloudflare hands out a session ticket (TLS 1.2, lifetime 18 h, checked with `openssl s_client -sess_out/-sess_in`: "Reused"), which the lamp keeps in RTC memory (`rtcTlsSession`, 1 KB slot, ~0.4 KB used; kept through deep sleep, cleared by power loss). The core is built with `CONFIG_MBEDTLS_SSL_KEEP_PEER_CERTIFICATE=y`, so a session carries the server's certificate (~0.9 KB); it is dropped before saving (resuming doesn't need it), otherwise the session doesn't fit and is never saved (the first version failed exactly like that). The log says `TLS session saved for next time (N bytes)` or `NOT saved: error ... (needs N bytes)`. The sync and an ack on a new connection offer it, and the server skips the key exchange. The ticket is not tied to the lamp's IP, so a new DHCP lease or network doesn't matter. If the server refuses it (expired, keys rotated) the handshake just runs in full, as before; a failed connect forgets the saved session. `ResumableTLS` is `WiFiClientSecure` with its own connect (the library's socket and TLS setup with `setInsecure()`, plus `mbedtls_ssl_set_session` / `mbedtls_ssl_get_session`); the log shows `TLS handshake N ms (saved session offered | full, no saved session)`. The firmware download and the rare fallback paths still use plain `WiFiClientSecure`.
- Direct draw (`FAST_DRAW`): a full-size landscape frame without a badge is sent straight to the panel using the same controller commands the display library uses internally (write, refresh, write again), skipping the library's pixel buffer and per-pixel loop. 2-bit stills are halftoned into a 1-bit picture first. Frames carrying a badge or the "Syncing..." badge still go through the library. Tested on a PC against the old path: byte-identical output for 400 random frames. Not tested on the panel: if the display glitches, set `FAST_DRAW` to `false`. The animation timing log shows `build` (making the picture) and `panel` (sending it and refreshing) separately so the gain is visible.
- Sync timing log: the serial console prints how long WiFi took to connect (and whether it used the cache), the time to the bundle header, for each image the download time (with KB and KB/s) split into **network** and **flash writes**, and validate+store time, the single ack time, then the total sync time and when the first draw started, all measured from wake.
- PSRAM holding tank and background writer: with PSRAM (2 MB on this board, enabled by `-DBOARD_HAS_PSRAM` in `platformio.ini`) the sync receives every image into memory at network speed (writing while receiving made every 4 KB flash erase, ~45 ms with the flash cache off, stall the WiFi/TCP code too). When the stream ends it checks the images and updates the manifest **in memory only**, then hands the flash work to a background task (`writerTask`, core 0) and returns, so the quick pass starts at once and draws the new images **from PSRAM**. The task writes each image straight to its final file name, saves the manifest once, removes evicted files, acks on the still-open sync connection and turns WiFi off. Until that save, `loadManifest()` hands out the in-memory copy. Nothing is acked before the manifest is on flash, so a power cut still loses nothing (a half-written file isn't in the manifest and is overwritten when the worker sends it again). Anything that writes the manifest (seen flags, another sync, sleep) waits for the writer first. The PSRAM copies stay until the next sync in the same wake, or sleep. Without PSRAM, when PSRAM fills, or when the stream breaks off, images are written synchronously as before (one manifest save per batch). Log lines from the task start with `[writer]`; the boot line shows free PSRAM (0 means the tank is off), and the sync shows the TLS handshake time on its own. Untested on the panel: animation frames may stutter slightly while the writer erases flash, since an erase pauses code running from flash on both cores.
- Pass during the download: images arrive oldest first, and the pass also runs oldest first, so it plays **while the sync downloads**: each new image goes up as soon as it is in (from PSRAM, or from flash without PSRAM); `STILL_HOLD_MS` (now 0) can add a minimum time on screen. Each image stays up at least its own refresh (~0.36 s), since the next draw can only start after it. `passTick()` is called from the download loop. The newest image is held back and drawn last by `quickPass()` as the resting image. Whatever order images arrive in, `saveManifest()` writes the manifest sorted by id (ids start with the send time), so it stays oldest first. The worker logs its timings per sync (`list+head`, each `fetch`, each `sent`), visible with `npx wrangler tail candlelight`; the lamp logs request-to-headers time and WiFi signal (dBm).
- Slow downloads, what was found (with temporary ping and link-test diagnostics, since removed): the worker sends a whole sync within ~0.15 s and the network round trips were 10 to 60 ms, so the time was spent **on the ESP32**. Three fixes: (1) the download loops read the TLS stream through `Stream::readBytes`, one byte at a time (two TLS library calls per byte); they now read in bulk. (2) The Arduino `HTTPClient` took ~1.5 s per request (5 s for an ack) where a raw request answers in ~50 ms; the sync and the ack use `rawRequest()` (the rare fallback paths and the firmware download still use `HTTPClient`). (3) TLS session resumption (below) cut the handshake from ~1.6 s to ~0.5 s. Result: first image ~1.4 s after WiFi connects (was ~7.5 s), ack ~0.2 s. What is left: throughput (~35 to 120 KB/s per image) is capped by the core's fixed 5.7 KB TCP receive window; raising it needs an Arduino-on-ESP-IDF build with our own sdkconfig (judged not worth it for now: the pass is paced by the panel refresh anyway).
- Serial log: the port has a 4 KB send buffer (`Serial.setTxBufferSize(4096)` before `Serial.begin`), so printing doesn't stall the code (unbuffered at 115200 baud, a 4-image sync's ~3 KB of log cost ~0.25 s). Side effect: after a crash the last lines may not come out.
- First measured sync with the tank (6 stills, 29.3 KB each, 10.4 s): header 2.7 s (mostly the ESP32's TLS handshake: the worker answers in ~0.2 s), network 3.1 s (~56 KB/s, capped by the Arduino core's fixed 5.7 KB TCP window, so only fewer bytes help), flash 2.9 s (~250 ms write + ~180 ms store per image), ack 1.6 s (the worker takes ~0.15 s, so likely a new handshake).
- When the lamp is full (`MAX_IMAGES`), the oldest **seen** image is removed to make room; an unseen one is only removed if every stored image is unseen.
- NEXT button holds (timed from the moment the press began, even on a wake):
  - tap (under 2 s): next image
  - 2 s or more, then release: replay the animation on screen; on a still it is just a step
  - 5 s (fires while held): "sync now" for the impatient: the same sync a timer wake does (images, plus a firmware update if the worker has one), clears the blocklist, and plays the quick pass if anything is unseen. If WiFi doesn't connect it just skips, and the lamp counts it. On the **second failed 5 s-hold sync in a row** it opens the `Candlelight-XXXX` setup portal with the QR screen (one 3 min attempt; if nobody joins, the current image is put back and the lamp sleeps), so the user can see what's wrong and fix the WiFi. A 5 s hold with working WiFi never shows the portal, and any successful connection resets the count
  - 8 s hold on BOOT (D5) forgets the saved WiFi and restarts into the setup portal with the QR screen. To reach it from sleep, press NEXT to wake the lamp, then hold BOOT right away (inside the 30 s awake window)
- Firmware download is our own loop in `downloadFirmware()` (not HTTPUpdate, whose stall handling can hang for many minutes): abandoned after 150 s total (a 1.1MB image needs only ~7.5 KB/s) or 15 s without data, retried at the next sync, and it is not counted as a crash. The stall limit is the real protection against a dead connection; the total only has to be longer than a slow-but-working download.
- State kept across sleep in RTC memory: current image, unseen count, wake count. It is reset on a true power-on.
- All timings are prototype placeholders. See **Timing reference** below for the full list, how to change them, and what depends on what. `FULL_REFRESH_ON_WAKE` (top of `main.cpp`) is the one non-timing knob.
- Rollback guard hooks (`src/ota_guard.h`): `markValid()` runs before sleep when the wake reached the worker or drew something; otherwise `beforeSleep()` marks the cycle inconclusive so a bootloader rollback isn't blocklisted. A first run of a new firmware with nothing to show still redraws the current image to prove the display path. The 300 s watchdog is only a backstop: connect steps have their own short timeouts.

Still to verify on the board: partial refresh after deep sleep (panel initialised with `initial=false`; set `FULL_REFRESH_ON_WAKE` if it ghosts or glitches), button wake, and real sleep current.

Still to build: skip the update step when the battery is low (waiting for the battery pack on the ADC; the lamp runs on laptop USB power for now). A brownout during a new firmware's first wake looks like a failure and rolls back a good build (the 5 s hold clears the blocklist).

## TEG voltage monitor (`teg_monitor.html`)

- Wiring: TEG + to A0 (GPIO36), TEG - to the common GND rail (shared with the FireBeetle GND and the button).
- `tegTask` (in `main.cpp`) listens on USB serial. The page sends `TEG ON` every 3 s while connected; while those keep coming the lamp prints `TEG,<mV>` every 250 ms (average of 3 calibrated `analogReadMilliVolts` reads, 11 dB attenuation: about 0.1 to 3.1 V, reads 0 below ~100 mV) and **does not sleep** (it stays in the idle window, buttons still work). `TEG OFF` (sent on Disconnect) or 2 min without a heartbeat ends it (long because Chrome slows a hidden tab's timers to one a minute; the page also sends the heartbeat from a Web Worker timer, which isn't slowed), and the lamp sleeps after the usual 30 s idle.
- Held awake by the page, the lamp keeps its usual sync schedule (every `SYNC_INTERVAL_S`), and after any sync installs a newer firmware at once instead of before the sleep (`installUpdateNow`; it marks the running firmware valid first, since the sleep that would have done so never comes). The page's "sync now" sends `SYNC` (same as a 5 s NEXT hold); "test ltc" sends `LTC TEST`.
- The page reads only `TEG,` lines and ignores the rest of the log. If the lamp is asleep when the page connects, tap NEXT to wake it.
- Keep the TEG under 3.3 V at the pin (the page warns at 2.8 V).
- Pack voltage: pack + -> 100k -> A1 (GPIO39) -> 100k -> GND (optional 100 nF from A1 to GND to steady the reading). The lamp prints `BAT,<pack mV>` once a second (8 reads averaged, times `BAT_DIVIDER` = 2). 4x NiMH reaches ~5.8 V while charging, 2.9 V at the pin. The divider draws ~25 uA from the pack all the time: fine on the bench; for a battery lamp use 1M/1M plus the 100 nF.

## Coulomb counter (LTC4150, untested on hardware)

> **PROTOTYPE ONLY: remove before shipping.** The shipping lamp has no LTC4150. Every piece of this is tagged `[PROTOTYPE-ONLY: LTC4150]`; see **Before shipping** below.

- Measures real charge into and out of the pack. SparkFun BOB-12052: pack + on **IN**, everything else (BQ25504 VBAT, and the lamp in a battery test) on **OUT**, so all pack current passes through it. The chip is powered from OUT (VDD = SENSE+ = OUT on that board), so its own ~0.1 mA flows through the sense resistor and is counted as "out".
- Board jumpers: **SJ1 open** (default closed, ties INT to CLR), **SJ2 and SJ3 closed** (3.3 V logic; open, CLR and SHDN reach the chip through 47k/75k dividers and sit near its input threshold).
- Wiring: INT -> D3 (GPIO26), POL -> A2 (GPIO34; the FireBeetle 2 ESP32-E has no D4 on its header, IO27 only drives its onboard button), CLR -> A4 (GPIO15), VIO -> 3V3, SHDN unconnected (3.3k pull-up to VIO on the board), GND common. INT and POL have 3.3k pull-ups to VIO on the board. **CLR must not be tied to INT**: tied, INT pulses for ~1 us, too short for the ULP.
- The ULP coprocessor (macro-assembled in `coulombBegin()`, no extra toolchain) polls INT every 100 ms, awake or in deep sleep: on a new latched pulse (INT high at the last poll, low now; an INT stuck low counts once, not every poll) it reads POL, bumps one of two 16-bit counters in RTC slow memory (words 0 and 1) and pulses CLR low for ~50 us. The main CPU never wakes for a pulse. `coulombPoll()` folds the counters into 32-bit totals (`rtcCoulIn`, `rtcCoulOut`), reset on a real power-on. Deep sleep keeps the RTC peripherals powered for the pins.
- 0.1707 mAh per pulse with the SparkFun board's 0.05 ohm sense resistor (`COUL_MAH_PER_TICK`). If "in" and "out" come out swapped, flip `COUL_POL_HIGH_IS_CHARGE`.
- Reporting: while the monitor page is connected, `COUL,<in>,<out>,<mAh per tick>,<INT>,<POL>,<stuck>` once a second over serial (the last three are the raw pin levels and whether INT has read low 3+ polls in a row, i.e. CLR isn't releasing it; the page shows them under the gauge) (the page shows this session's in/out, and relays them to the phone view). Every sync also carries `?cin=&cout=&mpt=` on its request; the worker stores them at `teg/<lampId>/coulomb`, and the page shows those totals when nothing is connected, so a battery test needs no USB.
- With USB plugged in the board runs from USB: lamp use is only measured on battery with USB unplugged (and the pack must never share a power rail with USB). TEG charge can be measured with USB in, as long as the pack is wired only to the counter.

### Debrief, 2026-10-17 and 2026-10-18: LTC4150 + BQ25504 bench (resume from the 2026-10-18 part)

**Bench setup now:** a USB step-down (set ~1 V, A0 reads ~1.1-1.57 V) stands in for the TEG and feeds BQ25504 VIN; A0 reads the same row. Pack (4xAA NiMH) + -> LTC IN, LTC OUT -> BQ BAT (shared row). LTC: INT -> D3, POL -> **A2 (GPIO34)** (the FireBeetle 2 has no D4 pin), CLR -> A4, VIO and SHDN -> one row fed from 3V3, GND common. FireBeetle on laptop USB; nothing from the pack reaches its VCC/BAT. No 100k resistors for the A1 pack-voltage divider yet, so `BAT,` reads nothing.

**The board:** HiLetgo LTC4150 module from Amazon, a SparkFun BOB-12052 copy: IN = SENSE-, OUT = SENSE+ and VDD, 3.3k pull-ups to VIO on INT/POL, 47k/75k dividers into the chip's CLR and SHDN unless SJ2/SJ3 are closed. The LTC test showed SJ1 is effectively open (INT doesn't follow CLR).

**The fault:** the counter climbs ~4 ticks/s (~0.7 mAh/s) even with LTC OUT disconnected, i.e. with no current possible. `LTC TEST` (page: "test ltc") shows CLR releases INT fine, then INT re-latches every ~297 ms, steady, POL 1: the chip behaves as if ~120-150 mV sat across its sense resistor (~2.5-3 A). Nothing got warm. Not the wiring, not a short, not the step-down. Firmware side is verified (edge counting, CLR pulse, pin reads).

**2026-10-18 debrief (resume here; supersedes the suspect/next-steps above and the "dead OUT pad" entry pushed in f0fbb14, which was wrong):**

- **Root cause, most likely: the pack holders.** Every cell measures full on its own, but **both 4xAA holders read 0 V across their leads**: an open circuit in the holder (switch, spring, cell-to-cell strap, or a lead's crimp/solder). The LTC4150 runs only from the battery side (VDD = OUT, fed from IN through the sense resistor; VIO only sets logic levels), so with no real pack voltage it sat unpowered or back-fed from the 3.3 V logic pins through its protection diodes, below its 2.7 V minimum. That explains the steady phantom pulse every ~300 ms "with no current". **The LTC can't be tested without a working pack above 2.7 V on it.** (A 1.3 V reading from pack + to GND, taken with the pack in place, was probably an intermittent contact in a holder.)
- **SJ2 and SJ3 are now bridged** (right for 3.3 V logic; it changed nothing, as expected). No SJ1 on this board.
- **The "first OUT" pin is the board's GND pin** (the power side has a GND beside OUT, as on SparkFun's layout), not a dead OUT pad. It reads OL to the sense resistor and ~0 ohm to GND. Jumpering it into the BQ BAT row **shorted BQ BAT to GND**. That is why: the "clean" LTC tests (chip lost power), the BQ25504 stuck in cold start (VSTOR ~0.5 V on the TEG, ~1.04 V on USB), and the BQ getting **very hot on the USB step-down** (2.6-2.7 V in, pushing into the short). **Pull that jumper; only the real OUT goes to BQ BAT.**
- **The BQ25504 module is fine.** AliExpress purple board, sku 02182 ("Solar Cell"): screw terminals VIN/GND, STOR/GND, BAT/GND, an extra VIN/GND pair, a header block of jumper choices (REFS-GND/BAT, OCS-STOR/GND; R-DIV and BAT-OK outputs), three solder-jumper pads in a corner, 100 uF tantalum (107C). EN is tied on the board. OCS jumper is on OCS-GND. With BAT unplugged it cold-started on the TEG (1.2 V in) at once: VSTOR 4.7 V and rising.
- **Never run the BQ with BAT empty.** It needs a battery (or >= 100 uF) on BAT; without one VSTOR overshot to **6.3 V** (chip abs max 5.5 V) before the TEG was unplugged. It survived. The 6.3 V says nothing about the OV setting.
- **OV setting:** the module stops at 3.15 V as shipped. A **3.9 Mohm** resistor (orange-white-green-gold; megaohm, not ohm) from the VBAT_OV divider's bottom pin (module pin 6) to GND gave **4.7 V** in an earlier test. A **3.3 Mohm** is on order to replace it: worked out from those two points (board divider ~4.5 M bottom / ~3.3 M top, VBAT_OV = 1.815 V x (1 + top/bottom)) it should give **~5.0 V**, the target for 4xAA NiMH. Verify with the pack on BAT, not with BAT empty.
- **Keep the USB step-down off the BQ.** Use the TEG (current-limited by nature), or set the step-down to ~1 V before connecting. VIN limit ~3 V; A0 limit 3.3 V.
- `COUL_POL_HIGH_IS_CHARGE` stays true: pack + -> LTC IN, real OUT -> BQ BAT.

**Next session:**
1. Fix a pack holder until it reads ~5 V across its red/black leads (check for a switch; measure across cells in place to find the break; re-bend springs or resolder the lead). Measure it under a little load too: wiggle the holder while reading.
2. Wiring: pull the jumper from the LTC's GND ("first OUT") pin. Pack + -> LTC IN, real LTC OUT -> BQ BAT row, pack - -> GND. TEG -> BQ VIN and A0.
3. Run **test ltc** with the candle out: expect "no pulse within 1.5 s of a clear". If it still pulses with a good pack on it, measure OUT to GND (the chip's VDD, must be > 2.7 V) before suspecting the board.
4. Light the candle: VSTOR should sit near the pack voltage, BQ cool, "pack in" ticking slowly. Press **new session** first (today's totals are phantom).
5. When the 3.3 Mohm arrives, swap it for the 3.9 Mohm and check where a full pack stops charging (~5.0 V).
6. Breadboard contacts add resistance next to the 0.05 ohm sense resistor; once it counts, calibrate `COUL_MAH_PER_TICK` against a multimeter in series (meter mA / page mA).

**Built today:** monitor page split (TEG 3/4, pack column 1/4 with gauge, in/out, measured rate, net, net-charge graph), smooth yellow TEG trace, LTC fuel gauge (localStorage, auto-full after 20 min lit with no pulse), serial console with pop-out, "sync now" and "test ltc" buttons; firmware edge counting + stuck flag + raw pins on `COUL,`, `LTC TEST`, 1 ms CLR pulse, `BAT,` on A1, updates install while the monitor holds the lamp awake. Lamp runs `61be939` (merged.bin = same). Worker version 6226ab63.

### 2026-10-25 bookmark

- **The LTC got hot with only the FireBeetle on USB**: no pack connected, TEG not running. The LTC's only supply was then the 3V3 row (VIO, SHDN) and the GPIO lines, so the likely cause is **3V3 shorted to GND through the LTC board**: a mislabeled pin (this board already had a GND pin we took for OUT), or the header seated along one breadboard strip. Not yet found. Unknown whether the USB step-down was still on BQ VIN at the time, or which part got hot (chip, sense resistor, whole board).
- **LTC power pins mapped (continuity to signal-side GND):** each side has two holes, both labelled just IN or OUT. One of each pair is GND, and the GNDs sit **diagonally**: the OUT hole across from the IN-side GND is the **real OUT**; the OUT hole across from the real IN is GND (the 10-18 "first OUT"). Rule: **real IN and real OUT are the holes that don't beep to GND** (and they beep to each other through the 0.05 ohm sense resistor). Board is fine, no resolder.
- Everything is unplugged. The FireBeetle GND jumper stays: it goes to the common GND rail.
- **A resistor has come off the BQ25504 module.** Which one isn't recorded yet. It may be the added 3.9 Mohm OV resistor (pin 6 to GND), but that's unconfirmed. If it's one of the board's own SMD parts, look up its value before resoldering. Fix this first.

**2026-10-26 update:** the first LTC board was faulty. With the pack on it read 5.13 V on OUT (supply fine), SHDN 3.3 V and **0 mV across IN -> OUT**, yet `LTC TEST` pulsed every 154 ms (7.2/s, "221 mV"), well over a healthy chip's 1.6/s limit. Replaced by a new board: **no phantom pulses, "out" counts look right** (one 0.17 mAh out tick from the chip's own draw). No "in" counted with the TEG lit because the pack (5.11 V) is above the BQ's stop voltage (**OV now ~4.89 V**), so the BQ doesn't charge; the page's "harvested est." comes from the TEG voltage alone and can't know that. Next: run the pack down with **drain mode** (below) until it rests ~4.7-4.8 V, then light the candle and watch "pack in".

### 2026-11-01 night: SJ2 resoldered

- SJ2 pads soldered, pack 3.67 V, candle lit: **still no ticks.**
- **Leg 9 (CLR at the chip) to leg 7 = 3.3 V:** CLR is good, so SJ2 is fixed and the chip isn't held in clear. SJ2 was not the cause.
- **Next: legs 3-4 (CF+/CF-) on the 200 mV range, candle lit and charging.** About the legs 1-2 reading (0.9-2 mV) = the signal reaches the integrator, so the fault is past it (chip/INT). Near 0 = CF leaky or input fault.
- **Legs 3-4 = 1 mV while charging** (same as legs 1-2): CF is fine and the signal reaches the integrator. At 1 mV expect a tick every ~31 s.
- **Next: meter zero check.** Candle out, legs 1-2 (bottom-left corner + its right neighbour) on 200 mV. 0.0 = the 1 mV readings are real current, and every input to the chip checks out. ~1 mV with no current = meter offset, so the "current" was never really measured.
- User: legs 3-4 fell as the candle died, so the 1 mV is real (meter zero check not needed).
- **FIRST TICKS WITH THE CANDLE (session mvyb7foskfie):** six ticks, in pairs, "out" then "in" right after: t = 830.2/830.4, 848.4/848.6, 1032.0/1034.6 s (gaps 0.2, 0.2, 2.7 s). Pairs 18 s apart at TEG ~1.05 V, then 183 s apart as the TEG fell to 0.44 V, so the rate follows the current. ULP glitch counter 0: INT stayed low the full 1 ms both times. The second INT of each pair needs INT to go high after the clear and low again 1-2 polls later. Net in-out = 0, so no charge is counted. The BQ's open-circuit samples (every ~15.7 s) don't line up with the ticks (1.3-6.3 s apart). Cause unknown; couldn't fetch the datasheet (analog.com timed out) to check how POL/INT behave around a clear.
- **User doubts these are real ticks, and the trace agrees:** TEG loaded 1.2-1.58 V from t ~240 s to ~780 s (9 min, strongest harvest, legs 1-2 at ~1-2 mV) gave **zero ticks**, where ~30 were expected. The pairs only came once the harvest was falling (TEG 1.06 V and dropping). So they follow the current *changing* (falling toward zero, maybe flipping direction around the BQ's bursts), not the charge passed. A steady charge current produces no counts.
- **INT-stuck banner showed afterwards; LTC test (13:20) passed** (INT high, CLR works, no pulses). The banner stayed up until tapped, so it may have been left over from earlier. Likely the 2.7 s pair: by the ULP's logic a tick pair is INT going low (tick 1), staying low through several CLR pulses (stuck >= 3 polls = banner), briefly high, then low again (tick 2). If so, each "pair" is one long INT-low episode that CLR can't release, during falling current. Page now clears the banner on release and logs the episode's time and length to the console (891eb9b).

### 2026-11-02 drain result (resume here)

- **Drain (lamp on the pack via OUT -> VCC, USB off, candle out): IN-OUT = 6.7 mV** (~134 mA through the sense resistor, out of the pack). Expected an "out" tick every ~4.6 s and POL low. Got **POL 100% "into pack", 0 flips, 0 ticks** for the 35 s the lamp was posting (edge mode, every input interrupt-watched). The lamp then stopped posting at uptime 57 s (23:58:04 UTC; cause unknown, its 60 s drain report never arrived).
- So with every input verified (VDD in range, SHDN high and watched, CLR high, 1-2 mV charging / 6.7 mV draining at the sense pins, legs 3-4 follow), the chip's count and POL respond in neither direction. INT/POL outputs and the latch work (the rare latched out+in pairs). What's left is inside the LTC4150 chip (integrator/polarity front end). Cheap modules sometimes carry remarked chips; the board's other parts are fine. User has insisted the board is fine; this is the measured result, stated plainly.
- Drain mode was left ON at the worker (it won't auto-stop without counts): stop it from the page.

### 2026-11-01 drain + POL test (resume here)

- **POL watch result (candle, 10 min, TEG up to 0.9-1.3 V):** POL "into pack" 100%, 0 flips; 0 ticks. SHDN: 8 sub-5 us drops, all at 15:18:42 (local) together with 35 INT bounce ticks before the candle was lit (a wire bumped?), none after. So no sloshing current; every datasheet condition for counting checks out from outside, still no ticks during steady charge. Relay "not coming through" was the laptop tab running the old page (hard refresh, Ctrl+Shift+R).
- **User's call: drain test to see if POL switches** (to "out of pack"). USB is unplugged in drain, so: edge mode (INT/POL/SHDN interrupts) now also runs while drain mode is on; every `LTC ...` line goes into an 80-line ring that is POSTed to the TEG relay every 5 s while draining (kept-open connection; the sync report runs every 60 s, was 120) as session `drain-test-lamp-1` (`at` = lamp uptime ms). Read with `curl .../teg/test-lamp-1`. No worker change.
- **Expect:** ~100 mA out = ~5 mV = an "out" tick every ~6 s, POL "out of pack" ~100%.

### 2026-11-01 POL watch (resume here)

- **SHDN watch: no drops in 4 min** (SDA jumper in the 3V3 row = SHDN row; it sees the row, not the header pin's own contact). SHDN ruled out as far as it can be.
- **Wiring confirmed by the user:** INT in its own row with the D3 jumper only; VIO + SHDN in the 3V3 row; POL straight to A2, CLR straight to A4.
- **Real latched pair caught in edge mode (23:01:34, TEG ~0.8 V and falling):** out (INT low 17.5 ms, released by our clear) then in 0.2 s later (1.2 ms, our clear). Chip, INT, CLR and firmware all work. An out+in pair means the chip's up/down counter is sitting at its wrap point, i.e. **the net current the chip integrates is ~0**, despite the meter's 1-2 mV IN-OUT. A steady 20-40 mA in would march the counter up and give "in" every 15-30 s.
- **Lead:** current through the sense resistor goes both ways (BQ charge bursts, flow back out between them / during the ~16 s MPPT samples) with a near-zero average; a DMM can show mV on that. The user remembers something on the LTC swinging with the BQ's spikes (not recorded which).
- **Built: POL watch.** In edge mode a CHANGE interrupt on POL (A2/GPIO34) times POL high vs low; every 5 s (was 30 s): `LTC POL last 5 s: into pack X% / out of pack Y% | N flips ...`. POL shows the latest direction whenever INT is high (datasheet). No wiring needed.
- **Next:** sync now, candle on, ~5 min, pull ltcLog. Mostly "into" with no ticks would contradict the net-zero idea; ~50/50 with many flips = bidirectional current netting ~0.

### 2026-11-01 SHDN watch (resume here)

- **Edge mode result:** with the hardware interrupt on INT, a ~12 min candle run (TEG ~1.0 V loaded) gave **0 edges**: INT never moved, not even for microseconds. So short pulses weren't the cause. Plugging and unplugging the INT wire gave 102 + 54 bounce ticks (fake, flagged TOO FAST), which proves the firmware, D3 and the page register INT. (Debounce still to add: ignore edges for ~0.3 s after a tick.)
- **Datasheet (rev C, read 11-01):** an up/down counter steps on every integrator reversal; INT latches only when it over/underflows (1024 steps = 1/(Vsense x 32.55 Hz/V), 15-30 s at 1-2 mV). **SHDN low resets the part, and so does VDD < 2.5 V (UVLO)**; while shut down, INT and POL float (read high via pull-ups), the same as "counting, no overflow yet". SENSE- goes on the battery side, SENSE+ = VDD on the charger/load side (our wiring is right). CLR resets INT only, counting unaffected.
- **New lead (user asked for an INT/VIO angle; the user insists the board is fine):** SHDN or VDD dropping briefly, more often than every ~15 s, keeps resetting the counter, so INT never fires. That fits all of it: steady strong charge = 0 ticks; after a reset the counter sits at the wrap point, so a wobble in falling current gives an out+in pair netting 0 (the ghost pairs); manual touches bypass the chip. Contacts are the suspects: the SHDN jumper / VIO pin (bad VIO contact already happened on 10-31), and the pack holder (VDD).
- **Built: SHDN watch.** Jumper from the SHDN row to **SDA (GPIO21)**, input only, never driven. In edge mode a CHANGE interrupt counts every drop (even < 5 us) and logs `LTC SHDN DROPPED xN ...` with the width and the gap; `LTC SHDN watch: no drops in the last 60 s` once a minute; the start line says whether SHDN reads high. The page now relays every `LTC...` line to the worker (curl /teg/test-lamp-1, ltcLog).
- **Next:** SHDN back on 3V3 + jumper SHDN row -> SDA; sync now; candle on, ~10 min. Drops = fix the SHDN connection (short soldered SHDN-VIO wire on the board). None = VDD dips (pack holder) are the next suspect.

### 2026-11-01 edge mode (resume here)

- **Blind spot found in my firmware:** the ULP only looked at INT every 100 ms. If the chip's pulses are short (INT released by itself instead of staying latched until our CLR), a 100 ms poll misses nearly all of them. That fits the data: a steady charge gives 0 ticks; the rare "ghost" pairs are the few times INT happened to stay low long enough to be caught. The 1-minute code-free meter test can't rule this out either, because a meter doesn't show microsecond dips.
- **Built (uncommitted at writing): edge mode.** While the monitor page is connected, the ULP is paused and INT goes to a hardware GPIO interrupt. That latches an edge however short it is, reads POL, and the TEG task clears with CLR (1 ms). Back to the ULP when the page lets go and before sleep. Every tick line now says how long INT stayed low and what released it: "released by our clear" (latched, normal), "BY ITSELF before our clear" (with its width), or "SHORT pulse" (< ~5 us).
- **Pushed 1b36bbc, CI green, uploaded to the worker (2026-11-01 21:47 UTC).** Lamp gets it on "sync now" from the monitor page; console then shows "LTC: edge mode on".
- **Bookmark (user lights the candle later):** edge mode only runs while the page is connected; in deep sleep the ULP's 100 ms poll still counts (sleep draw + unattended charging). If the pulses turn out short, sleep counting has the same blind spot (user's point). Then the fix is to wake the CPU on INT low (ext0/ext1) instead of polling, after checking the wake logic catches a short pulse. If they're latched ("released by our clear"), the missed ticks have another cause: chase that next.
- **Test:** flash/OTA, open the monitor page, light the candle, leave it ~5 min. Ticks every ~15-30 s = fixed (the chip was always sending; the poller missed them). Read the tick lines for the release type.

### 2026-11-01 late bookmark (resume here)

**Independent audit (fresh agent, 2026-11-01, from the datasheet rev C + SparkFun's LTC4150_BOB.sch v10):**
- **Firmware: no bug** (RTC pin numbers, edge/glitch logic, 1 ms CLR pulse, deep-sleep power all fine).
- SparkFun nets: VDD = SENSE+ = OUT; SENSE- = IN (pack on IN is right). VIO only feeds three 3.3k pull-ups (INT, POL, SHDN header). **CLR: header -> 47k -> chip CLR, chip CLR -> 75k -> GND; SJ2 shorts the 47k.** SHDN the same with SJ3. SJ1 ties INT to CLR headers. SJ2 and SJ3 sit at opposite ends of the board: **closing them can't join CLR to SHDN** (my 10-27/28 "clone quirk" was wrong, likely measured through breadboard wiring; so were "SHDN tied to VIO on the board" and the 3.03 V VIO "back-feed", both explained by the SHDN wire on the 3V3 row).
- **With SJ2 open, chip CLR sees 3.3 x 75/122 = 2.03 V, barely above VIH 1.9 V.** A shoddy SJ2 makes CLR marginal, so the chip may sit in "clear". Fixing SJ2 is a real lead.
- Logic thresholds fixed (VIL 0.7 V, VIH 1.9 V); abs max on CLR/SHDN = VDD + 0.3 V (3.3 V logic needs VDD >= 3.0 V). CF may be left open; a leaky CF shrinks the signal. Offset ±150-200 uV.
- The 1-minute code-free test is weak (at 0.9 mV ± meter error, a tick every 34-77 s).
- **Checks after the SJ2 fix, in order (single readings, no drain):** (1) leg 9 (CLR at the chip) to GND, powered, normal wiring: ~3.3 V good, ~2.0 V SJ2 still open, < 1.9 V chip held in clear. (2) legs 3-4 on 200 mV while charging: about legs 1-2; near 0 = CF/input fault. (3) legs 1-2 with the candle out: should read 0.0 (meter zero check).
- Bare-board reference values (all wires off, SJ1 open, SJ2/SJ3 closed): INT, POL, SHDN header ~3.3k to VIO; legs 5 and 9 ~75k to GND; CLR header to VIO no path; INT to CLR ~157k.

- **Legs 1-2 (SENSE+/SENSE-) read 0.9 mV** with a weaker candle (IN-OUT 2 mV at full candle): the chip sees the current.
- **SJ3 is closed; SJ2 has sparse, shoddy solder** (may be open or intermittent). Ignore my earlier same-day line saying both were open.
- **Code-free test:** candle charging (BQ VSTOR 3.64 V, VIN 1.34 V), CLR on 3V3, INT unplugged from D3, meter on INT: **stayed 3.3 V for 1+ min.** No firmware pin on INT or CLR, so it's not the code. Expected at 0.9 mV: a latch every ~34 s.
- Asked and answered: the TEG on OUT / pack on IN is correct (the chip is bidirectional, POL gives direction; charging = "in"). Low current isn't a cutoff: there's no minimum, ticks just come further apart (2 mV ~15 s, 0.9 mV ~34 s).
- The user won't drain while the pack is low and is fed up with test chains: one reading at a time, plain conclusions.
- **Next (user, 2026-11-02):** user will redo SJ2's solder properly (SJ3 is already closed). (The CLR-to-SHDN caution was wrong, see the audit above.) **FIRST TEST after resoldering: leg 9 (CLR at the chip, top row second from left) to GND (leg 7), powered, normal wiring, 20 V range.** ~3.3 V = good, light the candle and watch the page for ticks; ~2.0 V = SJ2 still open; < 1.9 V = chip held in clear. If ticks still don't come: legs 3-4 on 200 mV while charging.
- Only board part never checked: the CF capacitor (legs 3-4 voltage while charging, 200 mV range; should be about legs 1-2).

### 2026-11-01 bookmark

- **Board is a HiLetGo LTC4150.** The user is sure it works and won't buy another: don't call it dead.
- **Lamp-as-load test, first try, invalid (my test design):** INT unplugged from D3 -> D3's pull-down reads low -> the ULP's stuck path pulses CLR every 100 ms and clears the chip. The meter read ~3.24 V. IN-OUT under drain: 7-10 mV (0-18).
- **Second try, CLR moved from A4 to 3V3 (nothing can clear):** INT stayed ~3.30 V, sagging slowly with the rail. No latch.
- **INT to CLR header: 3.3 kOhm** (200k range), so not tied (SJ1 idea wrong). The board's INT pull-up looks like it goes to the CLR net, so INT reading 3.3 V says nothing about VIO.
- **Powered, normal wiring, drain off: leg 5 (SHDN) 3.3 V, leg 8 (VDD) 2.94 V.** VDD comes from the pack: 3 NiMH at ~1 V/cell = nearly flat, just above the LTC's 2.7 V minimum. Under the ~100 mA drain it almost certainly dropped below 2.7 V, so the drain tests ran the chip starved.
- **Next:** candle on, drain off, watch for "in" ticks while the BQ charges (VDD is already above 2.7 V and charging only raises it). No more drain until leg 8 reads >= 3.6 V. Orientation as the user holds the board: leg 5 bottom-right; bottom row 1-5 left to right, top row 10..6 left to right.

### 2026-10-31 late bookmark

**Decisive run (session mvvpbvzu7tpl, 01:07-01:14):** VIO wire on, candle hot, **0 ticks, 0 glitches, INT high for 7 min**, while the TEG trace shows the BQ's MPPT open-circuit samples every ~16 s (loaded ~1.4 V, open ~1.9 V): the BQ was harvesting the whole time. 2 mV across IN-OUT measured earlier = a tick every ~15 s expected (32.55 Hz/V x 2 mV, independent of the sense resistor's value).
- **All header-to-chip paths check out (power off, ohms):** INT -> leg 10, POL -> leg 6, IN/OUT -> legs 1/2, CLR -> leg 9 (0 ohm), SHDN = VIO -> leg 5. SJ2/SJ3 bridged, leave them alone.
- **CF is fine:** legs 3-4 actually read OL; the 0.1 ohm was both probes on one leg (user). **Next = the lamp-as-load test below.**
- (Superseded) legs 3-4 (CF+/CF-, the filter capacitor pins) read 0.1 ohm on the 200 ohm range. A short there zeroes the signal the chip integrates: powered, enabled, 2 mV in, no counts, everything else normal. **Unconfirmed:** check the pack + USB were unplugged (ohms invalid otherwise) and the probes didn't touch (legs 3/4 are adjacent; compare to probes shorted together).
- **Next (user, 2026-11-01):** everything unplugged; re-measure leg 3-4, leg 3-GND, leg 4-GND. If shorted: find the part with both ends beeping to legs 3/4 and neither to GND (the CF cap, should be ~4.7 uF): steady ~0 ohm across it = shorted/wrong part, remove it (datasheet: CF pins may be left open). No such part = solder bridge between legs 3 and 4: flux + wick. Then candle lit: expect "in" every ~15 s at 2 mV.
- **TOMORROW: lamp-as-load test** (the user calls it the drain test; no TEG involved):
  1. LTC OUT -> FireBeetle VCC, unplug USB; press NEXT now and then to keep it awake (~100 mA through the sense resistor).
  2. Unplug INT from D3; meter (20 V) on the INT header pin: ~3.3 V.
  3. A working chip drops INT to 0 V within ~6 s and holds it (nothing clears it).
  4. Meter IN -> OUT on 200 mV: expect ~5 mV.
  - INT drops: the chip counts; chase the INT wire / D3 contact / firmware. INT stays high with ~5 mV: everything around the chip is verified, so the chip itself isn't counting.
- The user doubts board faults (the first board's 154 ms phantom pulses likely came from the same INT-pull-up -> VIO backfeed, not a defect: I was wrong to call it bad). Firmware on the lamp: `b5a9936` (INT pull-down, tick/glitch log relayed as ltcLog, glitch filter; VIO check removed).

### 2026-10-31 bookmark

**The LTC4150 counts now.** Root cause of the zero counts: **a bad contact at the VIO header pin** (VIO floated 1-3 V; reseating the jumper fixed it). On this purple clone **SHDN is tied to VIO on the board** (leg 5 to VIO 0.06; the SHDN header pin is not connected to the chip, leave it unplugged), so a flaky VIO shut the chip down. Chip legs with a 3.69 V pack: 1, 2, 3, 4 at pack voltage, 5 SHDN 3.3 V, 7 GND, 8 VDD = OUT (3.74 V; the earlier 3.44 V was a probe slip), pinout from the datasheet (1 SENSE+, 2 SENSE-, 3 CF+, 4 CF-, 5 SHDN, 6 POL, 7 GND, 8 VDD, 9 CLR, 10 INT; leg 5 is the bottom-right corner with the board's orientation as the user holds it).
- Candle lit, 2.0 mV across IN-OUT (~40 mA if real: a tick every ~15 s). After minutes of 0, **in 1 + out 3 arrived within seconds** while the user was off the page: physically impossible from the TEG, so phantom ticks. Leaving the page does NOT trigger anything on the lamp (the heartbeat runs in a worker timer); a scheduled quiet sync (every 10 min while held awake) is a suspect, as is a loose INT/POL/CLR contact.
- **Built (uncommitted at writing):** the ULP re-reads INT 1 ms after an edge and drops it as a "glitch" if INT came back by itself (a real pulse stays latched until CLR); `LTC tick:` / `LTC glitch:` serial lines with gap since the last tick, a TOO FAST flag (< 3 s or several at once), TEG mV, WiFi state, sync/panel running or seconds since, INT/POL, uptime, ULP poll count; COUL line gains `,<glitches>,<polls>`. The page relays the lines as `ltcLog` in the `/teg/<lampId>` head (newest 100), so they can be read with `curl .../teg/test-lamp-1`.
- Burst at 00:29 (5 ticks in 9 s, WiFi off, no sync/panel, 0 glitches) happened while the user probed VIO, which read 0.7 V floating then 3.3 V. **VIO measured 3.3 V with the VIO and CLR wires both unplugged: the ESP's internal pull-up on INT fed VIO backwards through the board's 3.3k.** That weak feed half-ran the chip whenever the VIO wire had no contact; any tick (INT low) or POL low dragged VIO/SHDN down and reset it: bursts, phantom "out" ticks, lost charge. **Fixed in firmware: INT now has a pull-down** (the board's 3.3k still pulls it to ~3.1 V with VIO present). With that, VIO still read **3.03 V with its wire and CLR unplugged: the board feeds VIO from the pack side** (VDD 3.74 V; resistor or diode, unverified). A weak feed collapses to ~0.7-0.9 V whenever the chip pulls INT low through the 3.3k (fits the 0.7 V reading), so without a solid VIO wire each tick can reset the chip. A firmware "VIO LOST" check was built and **removed**: on this board it can't see a missing wire (VIO sits at ~3.0 V). Meter on the VIO pin: 3.30 V = wire connected, ~3.0 V or lower = not.
- The user points out VIO was at 3.3 V at times it still didn't work; only the 00:22-00:29 failure is proven to be VIO. **Next (decisive):** meter left on the VIO header pin for the whole run, new session, candle 10 min, then read the ltcLog. Regular "in" ticks at 3.30 V = VIO was the fault; bursts/out ticks/gaps at 3.30 V = something else, chase it from the log.

### 2026-10-28 bookmark

**Pack is now 3xNiMH, not 4.** 4xNiMH can't work: the BQ25504's OV tops out at ~5.25 V (1.31 V/cell) and a charging NiMH cell sits at 1.3-1.45 V, so at OV 4.89 V (1.22 V/cell) the BQ stopped as soon as current flowed. Also a full 4-cell pack (5.6-5.8 V) exceeds the FireBeetle's RT9080 LDO max (5.5 V). 3 cells: 3.6-4.35 V, inside the FireBeetle's 3.7-4.2 V Li-ion design range; LDO dropout ~0.2 V at WiFi peaks, so the lamp runs down to ~3.2-3.3 V pack under load (~1.1 V/cell).
- One slot of the 4xAA holder bridged with a wire/dummy. Pack read 3.65 V, later 3.57 V resting after the candle died down.
- **OV stays at ~4.89 V (3.9 Mohm, no change).** It's now only a ceiling (1.63 V/cell, never reached): the BQ trickles whenever the candle is lit. TEG ~C/150 on 2000 mAh is safe for NiMH. The 3.3 Mohm isn't needed.
- Page setting "pack volts" 3.7 (mAh stays the cells' rating, series doesn't add mAh). Drain mode / future low-battery cutoff: stop at ~3.3 V under load, not 4.0 V.
- User is buying a **spot-welded 3xAA NiMH pack with leads** (no spring contacts) and **skipping the TL431 clamp** (47k/47k design, ~40 uA, was offered against VSTOR overshoot if the pack disconnects). Until then keep FireBeetle VCC off the BAT row during charge tests.
- MPPT confirmed working: loaded TEG ~0.55-0.6 x open circuit (OCS jumper on GND = 50%). A0 reads ~40 mV above a meter at BQ VIN: normal.

**LTC4150: still no "in" pulses.**
- SJ2/SJ3 now open (CLR and SHDN separate). LTC TEST passes. Then **SHDN was found floating** (meter: bouncing 0-3 V, resting at 3.16 V): with SJ2/SJ3 open nothing held it high, so the chip kept shutting down/resetting. Likely cause of the in+out double ticks too (one more at a sync time, ~11 min into a session). **SHDN now jumpered to 3V3** (CLR-SHDN checked not to beep). Still no "in" pulses after that.
- Strong candle (~0.7 V loaded, 1.2 V Voc, est. ~25 mA) gave zero ticks earlier, but the current wasn't measured then. Weak candle (VIN 0.35 V): **0.1-0.2 mV across IN-OUT** (~2-4 mA at 0.05 ohm), a tick every 2.5-5 min at most.
- LTC OUT / BQ BAT are NOT in the 3V3/VIO strip (user checked).
- User is sure the new board is fine. No load resistors on hand (only the 3.3 Mohm).

**Next (pick up here):**
1. **INT latch test, no firmware involved:** unplug only INT at D3 (CLR stays high from the ESP, so nothing clears INT). Meter INT pin to GND (~3.3 V). Light the candle to a strong flame (VIN ~0.6-0.7 V): the first pulse latches INT at ~0 V within ~30-40 s (weak flame: up to 5 min). Confirm ~1 mV IN-OUT on 200 mV.
   - Or use the lamp as the load: LTC OUT strip -> FireBeetle VCC, USB out, drain mode on (~100 mA): INT should latch within ~6 s. Pack is low, keep it short.
2. INT latches -> the chip counts; the fault is the INT wire / D3 contact / firmware. Plug INT back and watch "out"/"in" on the page.
3. INT never latches with current confirmed -> read the sense resistor marking (R050 vs R010) and the VDD pin (datasheet pinout first).
4. Firmware offered, not built: log each tick with time + WiFi state; drop an in+out pair within ~3 s (physically impossible below ~0.6 A).

### 2026-10-27 bookmark

**Today's fixes (all pushed, firmware on the lamp):** the lamp stays awake with the monitor page in the background (`736d422`); the same picture is never redrawn, a reboot doesn't flash, automatic syncs are quiet, the seen flags are saved early (`a4b7c5e`); the page smooths the TEG reading with a 2 s median and shows the BQ's open-circuit samples as text (`c0c6531`). The user wants **as few full refreshes as possible** (the pass ends with a partial; only the wrap to image 1 is full).

**LTC4150: still not counting properly. The user thinks the board isn't working.**
- Fixed: CLR was tied to 3V3 through the SHDN wire (see "2026-10-27, CLR fault found" below). The SHDN wire is off; LTC TEST passes.
- Since then: **one** tick, showing as in AND out at once (+0.17 each), around when the user touched the chip. Then an update reboot reset the counts to 0 (the totals don't survive a reboot, only deep sleep). After that, **no pulses** while the page's estimate reached ~2.1 mAh in. That estimate is a model (rInt/eff settings), not a measurement.
- Charging is real: BAT = VSTOR = 4.77 V (BQ connected, charging), VCC jumper out, lamp on USB. Pack + -> LTC IN, LTC OUT -> BQ BAT.
- Not yet checked: the real charging current (meter in series, mA) and the sense resistor marking (R050 = 0.05 ohm assumed; R010 would mean 5x fewer pulses). The user doubts either is the cause.
- Suspects: (1) the CLR+SHDN join on this purple clone (likely SJ3): every 1 ms clear also resets the chip. That may cause the in+out double tick. It may also be worse than thought: if the chip comes out of each reset in a bad state, the next pulse never comes. (2) the chip isn't powered properly or isn't seeing the sense voltage: read every leg to GND with the pack at 4.77 V (the pin numbering is unverified, so get the datasheet pinout first), (3) a bad chip or board.

**Next options:**
- **Cleanest:** wick SJ3 (and SJ2) open so CLR and SHDN separate; check on continuity that CLR to SHDN doesn't beep; LTC TEST. Then CLR/SHDN go through the board's dividers. The thresholds may be marginal at 3.3 V, so check with LTC TEST.
- **Bypass the question:** a quick bench test with a known load: a resistor from the OUT row to GND (e.g. 100 ohm = ~48 mA at 4.8 V, ~0.23 W) should give ~1 tick every 13 s on "out". If it doesn't count that, the board is bad.
- **Firmware (offered, not built):** ignore an INT edge within ~0.3 s of our own clear (kills the phantom double tick); keep the totals through reboots (`RTC_NOINIT_ATTR` + magic). Probably also: `RTC_DATA_ATTR` is re-initialised on a software reset, contrary to the comment in `setup()`.

### 2026-10-26 evening bookmark

- **Drain mode is live** (commit `02a9b92`, worker `bcb11cdb`): lamp on `02a9b92`, running from the pack (VCC jumper -> BQ BAT / LTC OUT row, USB out), drain started at 800 mAh target. It reports every 2 min as planned.
- **Current is flowing:** ~7 mV across IN -> OUT = **~140 mA** (one 30 mV reading was a sync burst or a bad probe contact). 800 mAh at that rate is ~6 h.
- **But the LTC counts nothing:** every report says `in 0, out 0` since power-up. Header pins all read 3.3 V on the 20 V range: 3V3, INT, CLR, SHDN (earlier "INT 0 / OL" and "CLR 2.5" were the meter on 200 mV). INT high = the chip never latches a pulse.
- **Suspect: the new board's SJ2/SJ3 are open** (factory default). Then CLR and SHDN reach the chip through the 47k/75k dividers, ~2 V at the chip: SHDN near threshold (chip in shutdown) or CLR read as low (held in clear). Either gives INT high + 0 counts. The header pins read 3.3 V either way because the ESP32 drives them.
- **Because nothing is counted, drain won't stop by itself.** Press stop on the page (or pull the VCC jumper) after ~6 h at most.

**Next:**
1. Pull the VCC jumper (lamp off) before soldering.
2. Look at SJ2 and SJ3 on the new LTC board; bridge both if open. Or first measure at the chip's legs (20 V range): pin 5 SHDN and pin 9 CLR should read 3.3 V; ~2 V = jumpers open.
3. Reconnect: the lamp boots from the pack and carries on draining (the worker keeps the session; the counter restarting at 0 is handled). Within a report or two the panel should show mAh rising, ~4 s per tick at 140 mA.
4. If the count still stays 0: add INT/POL/stuck + raw ULP counters to the sync report so the drain panel shows them (offered, not built).

**Later the same night:** the user bridged SJ2 + SJ3, and it still counted 0. Then the pack turned out to be flat: **2.9 V loaded, 3.6 V rested** (~0.9 V/cell, over-discharged) after the drain never stopped by itself. BQ VSTOR read 0.8 V (BAT below its under-voltage limit, VSTOR cut off). No LTC chip leg read above 2.6 V, which fits a flat pack. The chip was below its 2.7 V minimum by then. That doesn't explain the 0 counts from the start, when the pack was full. Chip orientation: no dot; with the text readable, bottom-left reads ~0 V (GND?). My pin numbering is unverified (the search results disagree with it), so check the datasheet before asking for readings by pin number. (0.9 V/cell is a mild over-discharge; the cells are probably fine.) **Next:** stop drain on the page, VCC jumper out, then recharge from the TEG through the BQ with the pack on BAT. The TEG is the only charger, and that's fine: the 6.3 V VSTOR was with nothing on BAT. It's slow (tens of mW), and once the LTC works it should count it as "in"; then, with a full pack, read every LTC leg to GND (expect ~pack V on the sense/VDD legs) and look for a separate VDD pin on this generic board.

**2026-10-27, CLR fault found:** LTC TEST said "CLR (A4) can't go low". With the CLR wire off the board, A4 tested fine, so the board end was at fault. On continuity, CLR beeped to VIO; with the SHDN wire pulled (it went to the 3V3 row), that stopped. CLR and SHDN are still joined on the board itself, with clean solder, so probably by design once SJ3 is bridged (unverified). SJ1 is fine: CLR to INT doesn't beep. **The SHDN wire stays off** (the board's pull-up keeps SHDN high). Now every 1 ms clear also pulses SHDN low (a chip reset), losing at most ~0.1 s of counting per pulse, acceptable for the bench; clean fix if needed: wick SJ3 and re-check CLR to SHDN. LTC TEST then passed ("CLR pad reads 1 driven high, 0 driven low"). Next: watch for "in" ticks while the TEG charges the flat pack (a few to tens of mA = one tick every ~30 s to a few min). The board is the purple Amazon clone of SparkFun BOB-12052 (same header order, SJ1 front by VIO, SJ2/SJ3 on the back).

**Screen refresh rules (2026-10-27):**
- **The same picture is never redrawn.** `drawFrame()` fingerprints what it would put up (CRC of the frame plus badge, spinner, "Draining", info box) and skips the panel when it matches what's already there (`rtcScreenHash` in `RTC_NOINIT_ATTR` memory, so it survives reboots as well as sleep; cleared on power-up, during a refresh and by the setup screen).
- **As few full refreshes as possible (user, 2026-10-27):** only when NEXT wraps back to image 1, plus the library's forced first refresh after a non-sleep boot, and that one only if the picture actually changes. The auto pass ends with a fast partial refresh (user: that's fine).
- **Syncs nobody asked for are quiet** (`quietSync()`: the scheduled sync while the monitor page holds the lamp awake, and drain reports): no spinner, and the screen is only touched if images arrived.
- Suspected cause of the full flash every 2 min in drain mode: **the lamp rebooting at each report** (WiFi current on the weak pack -> brownout); every reboot went through the power-up path, a forced full refresh plus a pass. The wake line now prints the reset reason (9 = brownout). Not confirmed: needs a serial log on the pack, which isn't possible with USB unplugged.

**Unseen badge bug, likely fixed (2026-10-27, untested):** the seen flags only lived in RAM until sleep or the next *successful* sync, so (1) a reboot (brownout on the weak pack during drain, a crash, an update outside `installUpdateNow`) lost them, and the power-up pass put the badge back; (2) a sync whose WiFi connect failed skipped `saveSeenFlags()`, then `loadStoredIds()` reloaded the old flags from flash. Now `doSync()` saves them before connecting, and `idleWindow()` writes them `SEEN_SAVE_MS` (3 s) after the last press. If it still happens, get a serial log of it.

**Old notes on the badge bug:** User flipped through 4 new images (badge 4 -> gone), then a sync happened and the badge was back to 4 on the same images, twice. The worker is NOT re-sending them (queue empty, last deliveries days ago), so the lamp is losing its seen flags locally: the flash manifest still has `seen: false` after the sync. Not found yet. Places to look: `saveSeenFlags()` (matches by id against `storedIds`; skipped unless `seenDirty`), the background writer's `job->manifest` copy (saved after the flags were?), `memManifest`/`memManifestLive` in `loadManifest()`, and whether the "sync" was the monitor page's held-awake scheduled sync (`manualSync()` -> quick pass when `countUnseen() > 0`). Ask the user whether USB/the monitor page was connected, and get a serial log of it happening.

**Reconnect plan once the BQ is fixed (done):**
1. LTC loose, continuity from its GND to every pin: only GND pins may beep. Also check the power-side GNDs beep to the signal-side GND.
2. Seat the LTC across the centre gap, every pin in its own row.
3. Stage 1, signal side only: GND -> GND rail, VIO + SHDN -> 3V3 row, INT -> D3, POL -> A2, CLR -> A4. USB on, wait 30 s, feel the LTC. Cool -> `LTC TEST`.
4. Stage 2: real LTC OUT -> BQ BAT row; LTC IN in its own row. The GNDs beside IN/OUT stay empty (tied to GND on the board). Measure IN row and BAT row to GND in ohms (neither near 0), then pack + (red) -> IN row, pack - -> GND rail.
5. Pack holders: confirm ~5 V across red/black before connecting (they read 0 V on 10-18).

### Drain mode (runs the pack down for a charge test) [PROTOTYPE-ONLY: LTC4150]

- Started and stopped from the **drain mode** panel on `teg_monitor.html` (works from a phone): `POST teg/<lampId>/drain {targetMah}` / `{stop: true}`, `GET` reads it. State lives in R2 at `teg/<lampId>/drain`.
- Every `/sync` header carries `drain: {on, mah, target}`. While `on`, the lamp doesn't sleep: WiFi stays connected with power save off (~100 mA from the pack) and it syncs every `DRAIN_REPORT_S` (2 min), which reports the LTC totals on the query string. The worker adds `(out - in)` ticks since the last report (a total lower than last time = the lamp lost power and restarted from 0) and sets `on: false` at the target; the lamp then sleeps as usual. Firmware updates install at a report, as with the monitor page holding it awake.
- A **"Draining"** badge sits top-left on the screen while it runs. The lamp gives up on its own after `DRAIN_LOST_S` (1 h) without a header from the worker.
- Wiring: pack + -> LTC IN, LTC OUT row -> BQ BAT **and FireBeetle VCC** (5 V input; not the BAT/JST LiPo socket), GND common, **USB unplugged** (on USB the board runs from USB and nothing drains). The lamp picks up a start at its next sync (within 10 min), or at once with a 5 s NEXT hold. The first report only sets the baseline.
- No pack-voltage reading (no A1 divider resistors yet), so it stops by charge, not voltage: start with ~600-800 mAh, rest the pack 10 min, measure, and run again if it's still above ~4.8 V. Don't let it go below ~4.0 V under load.

## Before shipping: remove prototype-only code

Bench-measurement code that must not ship. Find every piece with:

```
grep -rn PROTOTYPE-ONLY firmware/ candlelight-worker/
```

- **LTC4150 coulomb counter** (`[PROTOTYPE-ONLY: LTC4150]`):
  - `fw/src/main.cpp`: the whole "coulomb counter" section, the three ULP `#include`s, the `coulombBegin()` call in `setup()`, the `esp_sleep_pd_config(RTC_PERIPH)` line in `goToSleep()` (only the ULP needs it; the NEXT button's ext0 wake keeps those pins powered on its own), the `?cin=&cout=&mpt=` on the sync request (back to plain `/lamp/<id>/sync`), and the `COUL,` serial line plus `lastCoul` in `tegTask`.
  - `candlelight-worker/src/index.js`: the `GET /teg/{lampId}/coulomb` route, the `cin/cout` block at the top of the sync handler, and the `coulomb` and `gauge` fields in the `/teg` relay.
  - `teg_monitor.html`: the pack in/out tiles, the "fuel gauge" section and its `gauge` relay field (the gauge is built on the counter).
  - Drain mode: in `main.cpp` the drain globals after `coulMux`, the `drain` read in `syncBundle()`, `drainReport()`/`drainKeepWiFi()`, the drain block in `idleWindow()`, the `|| drainOn` in `goToSleep()`, and the "Draining" badge (`drainOn` in `drawDirect()`/`drawFrame()`, back to `drawSyncBadge(g)`); in the worker the `teg/{lampId}/drain` route, the drain block in the sync handler and the header's `drain` field; in the page the drain section.
  - Also delete the stored objects `teg/<lampId>/coulomb` and `teg/<lampId>/drain` in R2.
- After removing: build, and check `grep -rn "coul\|ulp" firmware/fw/src` comes back empty.

## Timing reference (everything here is a prototype placeholder)

Every value below is a placeholder chosen to make prototyping fast. All of them are expected to change for the final product. Each one is tagged `[PLACEHOLDER]` in the code, so this lists them all:

```
grep -rn PLACEHOLDER firmware/fw/src firmware/recovery/src
```

**How to change one:** edit the constant, push to `main`. A change to the lamp firmware (`fw/`) reaches lamps by OTA at their next sync (within one `SYNC_INTERVAL_S`). A change to the recovery app (`recovery/`) reaches a lamp **only by a USB flash of `merged.bin`**, because OTA never updates the factory partition. Check the dependencies table below before changing anything.

### Lamp firmware: `fw/src/main.cpp`

| Constant | Now | What it controls |
|---|---|---|
| `SYNC_INTERVAL_S` | 600 s (10 min) | Deep-sleep time between timer wakes. Sets battery life, the longest wait before a new image arrives, and the longest wait before a firmware update arrives. Earlier plan for the final product: 30 min or more |
| `WIFI_TIMEOUT_MS` | 20 000 ms | How long a sync wake waits for the saved WiFi before skipping the sync and sleeping |
| `AWAKE_IDLE_MS` | 30 000 ms | How long the lamp stays awake after its last activity (a button press, or the end of the pass) so NEXT can step through images |
| `STILL_HOLD_MS` | 0 ms | Extra time each image stays up during the auto pass, **on top of** the e-ink refresh time (about 0.36 s partial, about 1.7 s full). At 0 the next image goes up as soon as it has arrived |
| `FAST_WIFI_MS` | 6 000 ms | The fast WiFi connect (cached channel/BSSID/IP) gives up after this and does a full connect |
| `DHCP_REUSE_S` | 1 800 s | How long the cached IP address is reused before asking DHCP again |
| `BUNDLE_STALL_MS` | 15 000 ms | The sync response is abandoned if no data arrives for this long |
| `BUNDLE_BUDGET_MS` | 120 000 ms | Total time allowed for the sync response |
| `OTA_BUDGET_MS` | 150 000 ms | Total time limit for downloading new firmware; past it the download is abandoned and retried at the next sync |
| `OTA_STALL_MS` | 15 000 ms | A firmware download that gets no data for this long is abandoned |
| `REPLAY_HOLD_MS` | 2 000 ms | NEXT held this long, then released, replays an animation (a tap shorter than this is "next") |
| `SYNC_HOLD_MS` | 5 000 ms | NEXT held this long fires "sync now" while still held |
| `RESET_HOLD_MS` | 8 000 ms | BOOT (D5) held this long forgets the saved WiFi and restarts |
| `PORTAL_TIMEOUT_S` | 180 s | How long a setup portal stays open (first-time setup, and the portal after two failed 5 s-hold syncs) |

### Rollback guard: `fw/src/ota_guard.h`

| Constant | Now | What it controls |
|---|---|---|
| `WATCHDOG_S` | 300 s | A hang longer than this resets the lamp. It is a backstop only, not the way a wake normally ends |

### Recovery app: `recovery/src/main.cpp`

| Constant | Now | What it controls |
|---|---|---|
| `WIFI_TIMEOUT_MS` | 30 000 ms | How long to wait for the saved WiFi before giving up |
| `OTA_BUDGET_MS` | 600 000 ms (10 min) | Total time limit for a firmware download |
| `OTA_STALL_MS` | 30 000 ms | A download with no data for this long is abandoned |
| `PORTAL_SECONDS` | 180 s | How long the setup portal stays open (only when a person is present) |
| `RETRY_SLEEP_S` | 300 s (5 min) | Sleep after a failed download before trying again |
| `WIFI_FAIL_SLEEP_S` | 1 800 s (30 min) | Sleep after WiFi wouldn't connect, or after a portal nobody used |
| `IDLE_SLEEP_S` | 1 800 s (30 min) | Sleep when nothing safe is left to install, before polling the worker again |

### Not named constants: numbers written directly in the code

These are technical timeouts rather than prototype placeholders, and are unlikely to need changing. They are listed so nothing is hidden:

| Where | Value | What it is |
|---|---|---|
| `main.cpp` `downloadItem()` | 3 attempts, 20 s read timeout, 0.5 s between attempts | Image download retries |
| `main.cpp` and `recovery` HTTP requests | 10 s read timeout | Queue list, delete/ack and firmware requests |
| `main.cpp` `connectWiFi()` (first-time setup portal) | `setConnectTimeout(20)` = 20 s | How long WiFiManager waits for a network to connect |
| `main.cpp` `goToSleep()`, `recovery` `sleepAndRetry()` | up to 10 s | Waits for NEXT to be released before sleeping (a held button would wake the lamp at once) |
| `buttonTask()` | 50 ms poll | Button polling and debounce |
| `waitForNext()` | 20 ms poll | Wait loop between button checks |
| Various | 100 to 500 ms | Short settle delays before a restart or sleep |
| Each animation | `intervalMs` stored in the image | Time between frames |

### Counts and limits (not times, but changed along with them)

| Value | Where | Now |
|---|---|---|
| `MAX_IMAGES` | `main.cpp` | 12 images kept on the lamp |
| `MAX_QUEUE_DEPTH` | `candlelight-worker/src/index.js` | 12 items waiting per lamp |
| `ANIM_LOOPS` | `main.cpp` | 1 play-through per animation |
| `MAX_CRASHES` | `ota_guard.h` | 4 crashes in a row before rolling back |
| `MAX_BAD` | `ota_guard.h` and recovery | 4 blocklisted firmware versions remembered |

### Dependencies between the values

Change these in step, or something quietly stops working:

| If you change... | Keep in mind |
|---|---|
| `AWAKE_IDLE_MS` | It must stay longer than `SYNC_HOLD_MS` plus about 2 s of wake-up time, or the 5 s sync hold from sleep can't finish. It must also leave room to press NEXT and then hold BOOT for `RESET_HOLD_MS`. At 30 s there is comfortable room; it got tight at 15 s, and below about 12 s the BOOT "forget WiFi" procedure stops working. Staying awake costs little next to the WiFi connections, so a longer window is cheap |
| `SYNC_HOLD_MS`, `REPLAY_HOLD_MS` | Keep the order: tap < `REPLAY_HOLD_MS` < `SYNC_HOLD_MS` |
| `PORTAL_TIMEOUT_S` / `PORTAL_SECONDS` | The portal blocks without feeding the watchdog, so keep `WATCHDOG_S` at least `PORTAL_TIMEOUT_S` plus about 60 s |
| `OTA_BUDGET_MS` | The firmware needs at least size divided by budget: 1.1 MB over 150 s is about 7.5 KB/s. If the firmware grows or links are slow, a budget that is too short means the update can never finish and keeps retrying |
| `DHCP_REUSE_S` | Keep it well under your router's DHCP lease time (often hours), or the lamp may use an address the router has handed to something else. Reuse is capped by wake time: it adds `SYNC_INTERVAL_S` per timer wake |
| `OTA_STALL_MS`, `BUNDLE_STALL_MS` | Keep it above normal WiFi hiccups (a few seconds), or healthy downloads get cut off |
| `SYNC_INTERVAL_S` | Also the delay before a firmware update or a new image shows up. Combined with the awake time per wake (WiFi, image downloads, pass) it determines battery life |
| `STILL_HOLD_MS` | Total pass time is unseen images × (refresh + hold), and that is awake time on every wake that brings new images |
| Recovery values | Only change via USB flash. They are independent of the lamp's values |

## Recovery app (`recovery/`, runs from the factory partition)

Runs only when neither OTA slot is bootable. It joins the saved WiFi. If that doesn't connect within 30 s (or none is saved) it opens the `Candlelight-XXXX` setup portal for 3 min, but **only when a person is present**: after a power-on, the reset button, or a press of NEXT (the button wakes recovery from sleep). A sleep-timer wake, or the automatic restart that sends a crashing lamp into recovery, never opens it, because a lamp must not wake on its own to broadcast a portal nobody sees. This is how a lamp whose WiFi is gone can still be pointed at a new network (there is no on-screen help in recovery; join the network from a phone). It then reads `/firmware/version`, and installs the newest firmware that isn't blocklisted, otherwise the previous one (`/firmware/previous.bin`). It uses the same bounded download loop as the lamp firmware but with a 10 min total limit and a 30 s stall limit, because it is the lamp's only way back. After a failed attempt it deep-sleeps 5 min and tries again; when WiFi won't connect (or a portal nobody used), or nothing safe is left to install, it sleeps 30 min. If the firmware it installed was rolled back by the bootloader, it blocklists that version (shared NVS keys `trying` and `bad` in namespace `otaguard`) so it doesn't reinstall it. The blocklist is a list of the last 4 failed versions (not one), so two failures can't cancel each other and cause an install loop; when both published builds are blocklisted, recovery just polls every 30 min until a new build is pushed. It keeps using `/firmware/version` rather than the queue response because it needs `previousVersion` and has no lamp ID.

## Working agreement: commits and pushes

- Never commit or push without being asked.
- After editing tracked files, remind the user that changes are uncommitted and ask whether to commit and push, at natural stopping points. Reason: a config file was lost once in a codespace wipe because it only existed locally.
- A push to `main` also publishes a new firmware version through CI, and a lamp flashed with an older `merged.bin` will update itself over the air on its next boot.
