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

### 2026-10-25 bookmark (resume here)

- **The LTC got hot with only the FireBeetle on USB**: no pack connected, TEG not running. The LTC's only supply was then the 3V3 row (VIO, SHDN) and the GPIO lines, so the likely cause is **3V3 shorted to GND through the LTC board**: a mislabeled pin (this board already had a GND pin we took for OUT), or the header seated along one breadboard strip. Not yet found. Unknown whether the USB step-down was still on BQ VIN at the time, or which part got hot (chip, sense resistor, whole board).
- **LTC power pins mapped (continuity to signal-side GND):** each side has two holes, both labelled just IN or OUT. One of each pair is GND, and the GNDs sit **diagonally**: the OUT hole across from the IN-side GND is the **real OUT**; the OUT hole across from the real IN is GND (the 10-18 "first OUT"). Rule: **real IN and real OUT are the holes that don't beep to GND** (and they beep to each other through the 0.05 ohm sense resistor). Board is fine, no resolder.
- Everything is unplugged. The FireBeetle GND jumper stays: it goes to the common GND rail.
- **A resistor has come off the BQ25504 module.** Which one isn't recorded yet. It may be the added 3.9 Mohm OV resistor (pin 6 to GND), but that's unconfirmed. If it's one of the board's own SMD parts, look up its value before resoldering. Fix this first.

**2026-10-26 update:** the first LTC board was faulty. With the pack on it read 5.13 V on OUT (supply fine), SHDN 3.3 V and **0 mV across IN -> OUT**, yet `LTC TEST` pulsed every 154 ms (7.2/s, "221 mV"), well over a healthy chip's 1.6/s limit. Replaced by a new board: **no phantom pulses, "out" counts look right** (one 0.17 mAh out tick from the chip's own draw). No "in" counted with the TEG lit because the pack (5.11 V) is above the BQ's stop voltage (**OV now ~4.89 V**), so the BQ doesn't charge; the page's "harvested est." comes from the TEG voltage alone and can't know that. Next: run the pack down with **drain mode** (below) until it rests ~4.7-4.8 V, then light the candle and watch "pack in".

### 2026-10-26 evening bookmark (resume here)

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
