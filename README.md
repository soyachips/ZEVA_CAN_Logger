# ZEVA ESP32 Cell Voltage Logger

An ESP32-S3-based logger for ZEVA BMS CAN data. It listens for ZEVA BMS packets on CAN, logs pack/cell voltages as CSV to the module's internal flash, serves a live web dashboard when in range of a WiFi hotspot, and automatically uploads whatever hasn't been synced yet to Google Drive once it detects it's parked at home.

## Current behavior
- uses ESP32 TWAI (native CAN) on pins `TX=IO12`, `RX=IO14` at 250 kbps
- listens for 29-bit ZEVA BMS packet IDs starting at decimal `300`, plus core EVMS broadcasts (pack voltage/current, per-module cell counts)
- merges module data into a single contiguous pack row per print/log cycle
- logs to a single file (`/data.csv`) on a LittleFS partition in internal flash, only while the clock has been NTP-synced this boot and pack current is non-zero (charging or discharging) — idle rows and rows from unsynced sessions are still printed to Serial but not written to flash
- if flash storage fills up (about 11.8 MB usable), logging pauses until the next home sync clears the file — see [Flash storage](#flash-storage)
- the local file is deleted only once its contents are *fully* synced to Drive, so the next row logged starts a fresh file — a backlog too big to sync in one pass instead makes incremental progress via a persistent byte offset — see [Idle-triggered Drive sync](#idle-triggered-drive-sync)
- serves a live WiFi dashboard (voltage/current + per-cell trend charts) when connected to a hotspot, plus firmware OTA updates at `/update` via ElegantOTA — see [Flashing firmware over WiFi (OTA)](#flashing-firmware-over-wifi-ota)
- automatically detects when the vehicle has been idle (current within a small deadband of 0 A) for 15 seconds, and if a known home WiFi network is in range, syncs the entire not-yet-synced backlog to a persistent file on Google Drive — one bounded batch at a time (read into RAM with WiFi off, then WiFi on to upload), looping through batches until fully caught up rather than syncing only a capped amount per visit — see [Idle-triggered Drive sync](#idle-triggered-drive-sync)
- can optionally mirror boot/connectivity/upload events (not raw telemetry) to a single evergreen `/log.txt` debug file in flash with per-line timestamps, so a run can be diagnosed without a serial monitor attached — off by default, see [Debug/event log](#debugevent-log-logtxt)

## Required hardware
- Custom board built around an ESP32-S3-WROOM-1-N16R8 module (16 MB flash, 8 MB octal PSRAM — the partition table and memory settings in `platformio.ini` assume exactly this part)
- CAN transceiver (for example MCP2562 or SN65HVD230)
- 120Ω termination resistor on each end of the CAN bus
- USB-to-serial adapter on UART0 for flashing and the serial monitor (the S3's native USB isn't used)

No SD card: logs are stored in the module's internal flash.

## Wiring
### CAN transceiver
- ESP32-S3 `IO12` → CAN transceiver `TX` / `TXD`
- ESP32-S3 `IO14` → CAN transceiver `RX` / `RXD`
- CAN transceiver `CANH` and `CANL` → CAN bus lines
- Terminate the bus with 120Ω at both ends

## Flash storage
`partitions.csv` splits the 16 MB flash into two 2 MB OTA app slots (the firmware is about 1 MB) and an 11.875 MB LittleFS data partition for `/data.csv`, `/data.offset` and the optional `/log.txt`. The partition is formatted automatically on first boot.

The firmware stops writing rows when less than 32 KB of free space is left, logs a "Storage full" message once, and carries on printing rows to Serial. Logging resumes once a home sync deletes the fully uploaded data file.

Changing `partitions.csv` moves the data partition, so flash the board over serial (not OTA) after any partition change and expect existing logs to be lost.

## Setup
1. Copy `include/secrets.h.example` to `include/secrets.h` and fill in your real values (this file is gitignored and never committed):
   - `SECRET_HOTSPOT_SSID` / `SECRET_HOTSPOT_PASSWORD` — the network used for the live dashboard (e.g. a phone hotspot)
   - `SECRET_HOME_SSID` / `SECRET_HOME_PASSWORD` — your home WiFi, used for NTP sync and automatic data upload
   - `SECRET_UPLOAD_SCRIPT_PATH` — the path portion of your Google Apps Script Web App deployment URL (everything after `script.google.com`)
2. Build and flash with PlatformIO (`pio run --target upload`). This board's auto-reset circuit isn't reliable — if you see `Wrong boot mode detected`, hold **BOOT**, tap **EN/RESET** once, keep holding BOOT until the upload starts connecting, then release.
3. Set `local_IP` / `gateway` / `subnet` / `primaryDNS` in `src/main.cpp` to match your hotspot's subnet if it's not an iPhone-style `172.20.10.x` hotspot.

### Google Apps Script upload endpoint
The firmware POSTs each chunk's raw CSV body to `https://script.google.com{SECRET_UPLOAD_SCRIPT_PATH}?filename=data.csv&first=<true|false>` with `Content-Type: text/csv`. The handler in [`google-apps-script/Code.gs`](google-apps-script/Code.gs):
- reads the chunk from `e.postData.contents`
- appends it to a persistent file (default name `data.csv`) in a folder read from a `DRIVE_FOLDER_ID` script property (see setup below), creating that file on the very first-ever sync
- if `first=true` (the first chunk of a sync — the only one that can contain a CSV header line) and the file already exists, strips a leading `timestamp,...` line from the incoming chunk before appending, so headers don't end up duplicated partway through the file
- returns a response containing the exact text `UPLOAD_SUCCESS` (the firmware only logs a successful archive when that substring shows up in the response body)
- must be deployed as a Web App with access set to **Anyone**, since the ESP32 connects anonymously

To deploy it:
1. Create a new project at [script.google.com](https://script.google.com) and paste in the contents of `google-apps-script/Code.gs`.
2. Under **Project Settings > Script Properties**, add a property named `DRIVE_FOLDER_ID` with the ID of the Drive folder you want logs uploaded to (the folder ID is the part of its URL after `/folders/`). This keeps the folder ID out of the committed script, the same way `include/secrets.h` keeps credentials out of the firmware source.
3. Deploy as a Web App (execute as yourself, access: Anyone), then copy the path portion of the deployment URL into `SECRET_UPLOAD_SCRIPT_PATH` in `include/secrets.h`.

Note: the Drive file just keeps growing across every successful sync (there's no local rotation/purge of it — that's a manual housekeeping task if it ever gets unwieldy), since the local device only ever holds the not-yet-synced portion and clears it after each successful upload.

## Boot sequence
1. Try connecting to home WiFi (≤10s). If found, sync the clock via NTP, then disconnect.
2. Try connecting to the dashboard hotspot (≤10s). If found: apply the static IP, sync NTP as a fallback if step 1 didn't already succeed, and start the web dashboard + OTA server. If not found: switch to offline logging mode.
3. Initialize CAN either way (flash storage is mounted at the very start of boot). If the clock was synced (from either network), open (or resume appending to) `/data.csv`. A brand-new file gets a placeholder-width header (`cell1..cell192`, covering the maximum possible module/cell configuration) so the file exists immediately — useful for bench testing without a live CAN bus — and that header is rewritten to the real cell count the moment the first actual CAN row is logged (see CSV data file format).

If neither network is in range, the ESP32's clock is never set for that boot session, and logging is skipped entirely — CAN data still prints to Serial for live viewing, but nothing is written to flash, since row timestamps are wall-clock based and would be meaningless without a synced clock. This also means a session with no clock sync has no data file to upload later, so the idle-triggered Drive sync becomes a no-op for that boot.

## Flashing firmware over WiFi (OTA)
No USB cable needed once the board is on the hotspot. PlatformIO doesn't push anything anywhere by itself — it only builds the `.bin` file; you upload that file manually through ElegantOTA's web page.

1. **Get the ESP32 onto the hotspot.** Power it on within range of `SECRET_HOTSPOT_SSID` (see Setup). It only opens the OTA server during its boot-time hotspot connection attempt, so it has to already be connected — if it boots out of range and falls into offline mode, power-cycle it in range to get another shot.
2. **Connect your computer to that same hotspot.** `/update` is only reachable from a device on the same network.
3. **Build (don't flash) the new firmware**: `pio run -e esp32s3`. This produces `.pio/build/esp32s3/firmware.bin` on your computer — it does not touch the ESP32.
4. **Open `http://172.20.10.9/update`** in a browser (adjust the IP if you changed `local_IP` in `src/main.cpp` — see Setup).
5. Choose `firmware.bin` on that page and upload it. The board flashes and reboots into the new firmware automatically.

**From a phone instead of a laptop**: upload `firmware.bin` to Google Drive from your computer, then on your phone download it from the Drive app to local storage (not just preview it — the browser's upload picker needs an actual file, e.g. in the Files app / Downloads). With the phone providing the hotspot (and the ESP32 already connected to it), open `http://172.20.10.9/update` in the phone's browser and upload the downloaded file the same way.

## Idle-triggered Drive sync
Once pack current has read within `CURRENT_IDLE_THRESHOLD_MA` of zero (a deadband, not an exact-zero check — current sensors have noise/offset and rarely read exactly 0 at rest) for 15 straight seconds, and the home WiFi network is actually in range, the logger loops through batches until the whole backlog is drained (or something stops progress):
1. reads how many bytes of `/data.csv` are already synced from a persistent marker file (`/data.offset`; 0 if it doesn't exist), then reads *from that offset* into a series of adaptively-sized RAM chunks (`readDataFileToChunks()`, preferred size `UPLOAD_CHUNK_SIZE` = 16KB, shrinking down to `MIN_UPLOAD_CHUNK_SIZE` = 512B if the preferred size won't allocate), stopping once it hits `MAX_UPLOAD_CHUNKS`, `MAX_BATCH_BYTES` (32KB total), or there's simply no more file left — **radio off for this step**
2. brings WiFi back up and connects to the home network
3. POSTs each chunk in turn (`uploadChunksToGoogleDrive()` / `uploadOneChunk()`) — pure network I/O, no more file access
4. if every chunk in the batch uploaded successfully: if that batch reached the end of the file, deletes the local file and offset marker (fully synced) and stops; otherwise advances `/data.offset` by however many bytes were just synced and **goes straight back to step 1 for the next batch**, radio off again for that read
5. once done (fully synced) or stopped early (read failure, lost connection mid-sync, or upload failure), disconnects/powers down the radio

This is deliberately unhurried — every CAN update gets logged (see Current behavior), and a sync will keep working through however large a backlog has built up rather than draining it a capped batch at a time across separate parking visits. The tradeoff is CAN reception stays blocked for the whole multi-batch sync, which can take a while for a large backlog; that's judged an acceptable cost against ever losing logged rows or waiting on multiple separate stop-start cycles to fully catch up.

Steps 2-3 are pure network I/O with no file access at all, and step 1 (repeated each batch) is the only file access, always done with the radio off. This split dates from the SD-card prototype, where SD I/O while WiFi was transmitting caused `sdCommand()` failures. Internal flash doesn't have that electrical problem, so streaming straight from the file during upload would now be viable and could simplify this batching/chunking machinery; the read-then-upload design was kept as-is in the move to flash.

**Why an offset, not "read the whole file every time"**: the local file accumulates everything logged since the last *fully completed* sync, which can add up across many short rides sharing one file, or across a single long one — large enough in practice to exceed total free heap, not just a single allocation (this happened during development: a 268KB backlog couldn't be held in RAM at once against ~244KB free heap, no matter how it was chunked, since every chunk has to stay resident until upload time). Chunking alone only solves needing one large *contiguous* block; it doesn't reduce the *total* memory a full-file read needs. The offset in `/data.offset` turns a sync into resumable batches instead: each batch reads/uploads as much as currently fits and records how far it got, so an oversized backlog (or a single very long ride) gets synced as a sequence of batches within one parked visit — or, if that visit ends early (see the loop's stop conditions), continued on the next one. A brand-new `/data.csv` always clears any leftover offset first (see `openDataFile()`), so a stale offset can never misapply to a different file's content.

**Why the chunk size is adaptive, not fixed**: `mallocChunkAdaptive()` tries the preferred size and, on failure, halves and retries down to `MIN_UPLOAD_CHUNK_SIZE`, reading exactly whatever size actually allocated, rather than failing outright. This is defensive breathing room for whatever the heap happens to look like at that moment — the real fix for reliably having room to work with is `MAX_BATCH_BYTES` below, not this.

**Why there's a hard cap on total batch size, separate from the chunk-count cap**: it took a long detour to find this — a series of "impossible" allocation failures where the reported largest free block was bigger than the size that had just failed to allocate. The actual cause turned out to be simple: reading as much as would successfully fit in RAM (once, 196KB across 26 chunks in one batch) meant that memory was still held, unfreed, when the code went to bring WiFi back up moments later — and WiFi failed to initialize (`wifi:create wifi task: failed to create task`) because there wasn't enough heap left for its own task/buffers. Every one of the earlier allocation failures was very likely this same underlying resource contention showing up at a smaller scale, not a heap bug. `MAX_BATCH_BYTES` (32KB) puts a firm ceiling on how much a sync will ever hold in RAM at once, independent of how much more the heap could technically provide in the moment, guaranteeing WiFi has room to start regardless of exactly how much total heap turns out to be free.

**Duplicate-row caveat**: uploading stops at the first chunk that fails rather than skipping ahead, so a mid-sync WiFi drop never leaves a gap in the data — but if some earlier chunks in that same batch had already landed on Drive before the failure, the offset isn't advanced, so the next sync re-sends the *entire batch* from its starting offset, duplicating whatever had already landed. This is a real gap, not a hidden one, and the offset mechanism actually shrinks its blast radius versus the old whole-file design: the duplication is now bounded to at most one batch (`MAX_UPLOAD_CHUNKS` worth of data) rather than the entire unsynced backlog. The consequence is extra, easily identifiable duplicate rows (every row has a real timestamp) rather than silent data loss. Building true exactly-once semantics would need staging/commit logic on the Apps Script side; it wasn't judged worth the added complexity given how rarely a batch upload will actually fail partway through.

This only runs once per idle period (it resets the next time current leaves the deadband), and it blocks CAN reception for the duration of the read/scan/connect/upload — acceptable since the vehicle is already parked by the time this triggers.

`CURRENT_IDLE_THRESHOLD_MA` defaults to 500 mA; tune it once you have the real current sensor connected and can see its actual noise floor at rest.

## CSV data file format (`/data.csv` locally, appended into a persistent file on Drive)
```
timestamp,packV,packI,cell1,cell2,...,cellN
```
- `timestamp`: local wall-clock date/time, formatted `YYYY-MM-DD HH:MM:SS`. Rows are only ever written once NTP has synced the clock that boot (see Boot sequence), so this is always a real, human-readable value — safe to sort/plot across multiple sessions without the timestamp resetting or losing meaning
- `packV`: pack voltage in volts (raw ZEVA value is in tenths of a volt)
- `packI`: pack current in amps, rounded to 0.1 A (raw ZEVA current is a signed 24-bit value offset by 8,388,608)
- `cellN`: individual cell voltage in volts to 3 decimal places (raw ZEVA cell voltage is in millivolts); the number of cell columns in a given row depends on how many modules/cells are actually active, not a fixed count
- locally, the header row is written once each time `/data.csv` is (re)created — i.e. once per full sync cycle, since the file (and its `/data.offset` marker) are only deleted once fully synced, not after each partial batch. On Drive, Code.gs strips the header from every batch except the one starting at offset 0, so the combined file only ever has one header row at the top
- a brand-new `/data.csv` is created at boot with a placeholder-width header (`cell1..cell192`, the maximum possible across `MAX_BMS_MODULES` × 12 cells/module) so the file exists immediately even with no CAN bus connected. The moment the first real CAN row is logged, that header is discarded and rewritten to the actual cell count from that row (`cell1..cellN`) — safe because at that point the file still contains nothing but the placeholder header itself, no real rows yet. This trim only ever happens once per boot, and only for a file setup() genuinely created fresh; resuming an existing `/data.csv` (from an earlier boot this session, or an unsynced backlog) leaves its already-real header untouched (see `openDataFile()` / `trimDataFileHeaderIfNeeded()`)

## Debug/event log (`/log.txt`)
A single evergreen text file in flash storage that mirrors boot, WiFi/NTP, and upload events (not raw telemetry) with a timestamp on each line — useful for figuring out what happened on a run with no serial monitor attached (e.g. battery-powered breadboard testing). It's opened before any WiFi activity, so it captures the whole boot sequence, including cases where NTP never syncs. Each line is prefixed `[YYYY-MM-DD HH:MM:SS]` once the clock is synced, or `[boot+Nms]` before that. It is never rotated or purged — it just keeps growing across boots — and it does *not* duplicate the per-row CSV telemetry or the 1-second "waiting for data" heartbeat, only connectivity/upload-relevant events (WiFi search results, NTP sync, storage errors, idle-sensor scans, upload attempts and responses).

**Off by default** (`DEBUG_LOG_ENABLED = false` in `src/main.cpp`). The file shares flash with the CSV log and is never synced or cleared, so leaving it on permanently slowly eats into logging space. Flip it to `true` for a specific diagnostic session (e.g. a blind battery test) and back to `false` for normal use. When disabled, every `DebugLog.*` call still prints to Serial as normal — only the file write is skipped.

## Known limitations
- The Google Drive upload connection doesn't validate the server's TLS certificate (`setInsecure()`), so it's not protected against a MITM on that connection.
- The live dashboard loads Chart.js from a public CDN, so it won't render charts if your hotspot has no real internet backhaul (only local link to the ESP32).
- Flash storage is limited to about 11.8 MB. If the vehicle runs for long stretches without a home sync, logging pauses once storage is full and those rows are lost (see [Flash storage](#flash-storage)).
- **WiFi needs a meaningful amount of free heap to initialize, and it competes directly with the idle sync's chunked read for that same memory.** This was the root cause behind a long chain of "impossible" allocation failures during development, where the reported largest free block was repeatedly bigger than the request that had just failed — reading as much of the backlog as would fit in RAM (196KB in one case) left too little heap for `WiFi.mode(WIFI_STA)` to spawn its own task afterward (`wifi:create wifi task: failed to create task`), which is a distinct failure from anything actually wrong with the read itself. `MAX_BATCH_BYTES` (see Idle-triggered Drive sync) now caps how much a sync ever holds in RAM specifically to leave WiFi enough room, regardless of how much more the heap could technically provide in the moment.
- A batch spanning more than one chunk can duplicate rows on Drive if WiFi drops partway through uploading it — see the duplicate-row caveat in Idle-triggered Drive sync. Bounded to at most one batch's worth of data since the offset mechanism was added, rather than a whole unsynced backlog.
- A big enough backlog syncs as a sequence of batches within one parked visit (see Idle-triggered Drive sync), which blocks CAN reception for as long as that takes — by design, given every CAN update is logged and nothing is throttled. If the visit ends before the backlog finishes (WiFi drops, upload fails, `MAX_BATCHES_PER_SESSION` safety backstop hit), the remainder resumes on the next idle period rather than being lost.
