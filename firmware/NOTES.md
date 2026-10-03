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
- Stepping through new images (the "tour", `rtcInTour`, kept across sleep): while unseen images remain, NEXT steps **only through the unseen ones**, newest to oldest, and the badge counts down (4, 3, 2, 1: the last unseen image shows a 1). The next press **wraps back to image 1** (no badge; this is the **only full refresh**) instead of carrying on through the older images. With a single new image (image 1 showing a "1") the first press **only clears the badge** (fast partial refresh, same image) and the second press goes to image 2. From there NEXT is plain 1, 2 ... 12, 1 again (all partial refreshes); stepping to an animation plays it. Badges are drawn straight into the fast path's picture (`MonoCanvas`), so a badge doesn't slow a step down. (Dots were tried and dropped: not visible enough.)
- Sync feedback: when the 5 s hold fires, the lamp redraws the image on screen (as a still, first frame) with a boxed **"Syncing..."** badge in the top-left corner. When the sync ends it either plays the quick pass (which redraws everything) or redraws the image without the badge.
- Sync protocol (one connection): `GET /lamp/<id>/sync` returns the firmware version, the queue and every queued image in **one response** (a text header line, then per item a text header line and exactly `size` raw bytes). The lamp stores each image as it streams in, then confirms them all with one `POST /lamp/<id>/ack {"ids":[...]}`. That is 2 secure connections per sync however many images arrive, instead of 2 per image plus 1. Nothing is deleted from the queue until the ack. The bundle sends each image **without its PNG preview** (the lamp never decodes it; it only reads the e-ink `epRa`/`epRb` chunks): about 63% fewer bytes for a still and 24% for an animation, which matters most for flash writes. The worker fetches all the images in parallel and builds the response in one go. The ack goes out on the sync's already-open connection (a new handshake only if that connection is gone). The `/items` fallback still serves full PNGs. Fallbacks: if `/sync` fails, or breaks off midway, the lamp finishes with the older per-image path (`/queue`, `/items/<id>`, `DELETE`), which still works against any worker. The lamp no longer needs a separate queue read: the bundle header carries what it used to fetch from the queue. **Streamed:** at upload the worker records each image's stripped size (`lampSize` in the R2 metadata), so `/sync` knows the exact total up front: it sends the header at once and each image as soon as its fetch is done, with a real `Content-Length` (no chunked encoding, same bytes as before, so any firmware reads it). An item that vanished is sent as zeros of the promised size, which the lamp discards and acks. Items queued before `lampSize` existed get the old buffered response.
- Fast WiFi: each wake remembers the access point's channel and BSSID (and the IP, DNS and gateway) in RTC memory and reconnects without a channel scan; while the cached address is fresher than `DHCP_REUSE_S` it also skips DHCP. If the fast connect fails it falls back to a normal connect and clears the cache. WiFi modem power-save is off during the sync.
- TLS session resumption (`tls_resume.h`, `connectWorker()`): a full TLS handshake costs the ESP32 ~1.6 s. After one, Cloudflare hands out a session ticket (TLS 1.2, lifetime 18 h, checked with `openssl s_client -sess_out/-sess_in`: "Reused"), which the lamp keeps in RTC memory (`rtcTlsSession`, 1 KB; kept through deep sleep, cleared by power loss). The sync, an ack on a new connection, and the link test offer it, and the server skips the key exchange. The ticket is not tied to the lamp's IP, so a new DHCP lease or network doesn't matter. If the server refuses it (expired, keys rotated) the handshake just runs in full, as before; a failed connect forgets the saved session. `ResumableTLS` is `WiFiClientSecure` with its own connect (the library's socket and TLS setup with `setInsecure()`, plus `mbedtls_ssl_set_session` / `mbedtls_ssl_get_session`); the log shows `TLS handshake N ms (saved session offered | full, no saved session)`. The firmware download and the rare fallback paths still use plain `WiFiClientSecure`.
- Direct draw (`FAST_DRAW`): a full-size landscape frame without a badge is sent straight to the panel using the same controller commands the display library uses internally (write, refresh, write again), skipping the library's pixel buffer and per-pixel loop. 2-bit stills are halftoned into a 1-bit picture first. Frames carrying a badge or the "Syncing..." badge still go through the library. Tested on a PC against the old path: byte-identical output for 400 random frames. Not tested on the panel: if the display glitches, set `FAST_DRAW` to `false`. The animation timing log shows `build` (making the picture) and `panel` (sending it and refreshing) separately so the gain is visible.
- Sync timing log: the serial console prints how long WiFi took to connect (and whether it used the cache), the time to the bundle header, for each image the download time (with KB and KB/s) split into **network** and **flash writes**, and validate+store time, the single ack time, then the total sync time and when the first draw started, all measured from wake.
- PSRAM holding tank and background writer: with PSRAM (2 MB on this board, enabled by `-DBOARD_HAS_PSRAM` in `platformio.ini`) the sync receives every image into memory at network speed (writing while receiving made every 4 KB flash erase, ~45 ms with the flash cache off, stall the WiFi/TCP code too). When the stream ends it checks the images and updates the manifest **in memory only**, then hands the flash work to a background task (`writerTask`, core 0) and returns, so the quick pass starts at once and draws the new images **from PSRAM**. The task writes each image straight to its final file name, saves the manifest once, removes evicted files, acks on the still-open sync connection and turns WiFi off. Until that save, `loadManifest()` hands out the in-memory copy. Nothing is acked before the manifest is on flash, so a power cut still loses nothing (a half-written file isn't in the manifest and is overwritten when the worker sends it again). Anything that writes the manifest (seen flags, another sync, sleep) waits for the writer first. The PSRAM copies stay until the next sync in the same wake, or sleep. Without PSRAM, when PSRAM fills, or when the stream breaks off, images are written synchronously as before (one manifest save per batch). Log lines from the task start with `[writer]`; the boot line shows free PSRAM (0 means the tank is off), and the sync shows the TLS handshake time on its own. Untested on the panel: animation frames may stutter slightly while the writer erases flash, since an erase pauses code running from flash on both cores.
- Pass during the download: images arrive oldest first, and the pass also runs oldest first, so it plays **while the sync downloads**: each new image goes up as soon as it is in (from PSRAM, or from flash without PSRAM); `STILL_HOLD_MS` (now 0) can add a minimum time on screen. Each image stays up at least its own refresh (~0.36 s), since the next draw can only start after it. `passTick()` is called from the download loop. The newest image is held back and drawn last by `quickPass()` as the resting image. Whatever order images arrive in, `saveManifest()` writes the manifest sorted by id (ids start with the send time), so it stays oldest first. The worker logs its timings per sync (`list+head`, each `fetch`, each `sent`), visible with `npx wrangler tail candlelight`; the lamp logs request-to-headers time and WiFi signal (dBm).
- [DIAGNOSTIC, temporary] Slow downloads: the worker has a whole sync fetched and sent in ~0.15 s (worker log), and the lamp's pings are fast (router 9 to 31 ms, internet 21 to 58 ms), yet the lamp waited ~1.5 s for response headers and got 13 to 25 KB/s. So the time is spent **on the ESP32**. Found and fixed: the download loops read the TLS stream through `Stream::readBytes`, which fetches **one byte at a time** (each byte two calls into the TLS library); they now read in bulk. The ~1.5 s to the first byte of every request (5 s for the ack) turned out to be the Arduino `HTTPClient`: the link test's raw requests on the same kind of connection got their first byte in ~50 ms. The sync and the ack now use our own minimal request code (`rawRequest()`); the rare fallback paths and the firmware download still use `HTTPClient`. Link test result (100 KB, raw, bulk reads): HTTPS 33 KB/s, plain HTTP 67 KB/s, TLS connect ~1.7 s; at ~40 ms round trips that throughput is the 5.7 KB TCP window, and the handshake is ESP32 crypto. Two diagnostics until that is answered: every sync pings the router and 1.1.1.1 (`pingDiag()`, `[ping ...]` lines), and after a manual (hold) sync the wake ends with a link test (`speedTest()`, `[speed ...]` lines): 100 KB from the worker's `/speedtest` over HTTPS and over plain HTTP, raw requests, logging connect time, time to first byte and KB/s. Remove both once answered.
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
