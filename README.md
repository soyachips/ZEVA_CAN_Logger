# ZEVA ESP32 Cell Voltage Logger

An ESP32-based logger for ZEVA BMS CAN data. It listens for ZEVA BMS packets on CAN, logs pack/cell voltages to an SD card as CSV, serves a live web dashboard when in range of a WiFi hotspot, and automatically uploads the day's log to Google Drive once it detects it's parked at home.

## Current behavior
- uses ESP32 TWAI (native CAN) on pins `TX=21`, `RX=22` at 250 kbps
- listens for 29-bit ZEVA BMS packet IDs starting at decimal `300`, plus core EVMS broadcasts (pack voltage/current, per-module cell counts)
- merges module data into a single contiguous pack row per print/log cycle
- logs to SD as CSV, one file per calendar day, only while the clock has been NTP-synced this boot and pack current is non-zero (charging or discharging) — idle rows and rows from unsynced sessions are still printed to Serial but not written to SD
- old log files are purged automatically, keeping the most recent `MAX_LOG_FILE_DAYS` (7) days
- serves a live WiFi dashboard (voltage/current + per-cell trend charts) when connected to a hotspot, plus firmware OTA updates at `/update` via ElegantOTA
- automatically detects when the vehicle has been idle (current within a small deadband of 0 A) for 15 seconds, scans for a known home WiFi network, and if found, connects and uploads the current day's log to Google Drive via a Google Apps Script Web App

## Required hardware
- ESP32-WROOM-32 module
- CAN transceiver module (for example MCP2562 or SN65HVD230)
- 120Ω termination resistor on each end of the CAN bus
- microSD card module

## Suggested wiring
### CAN transceiver
- ESP32 `GND` → CAN transceiver `GND`
- ESP32 `3.3V` → CAN transceiver `VCC` (or `5V` if the board requires it)
- ESP32 `GPIO21` → CAN transceiver `TX` / `TXD`
- ESP32 `GPIO22` → CAN transceiver `RX` / `RXD`
- CAN transceiver `CANH` and `CANL` → CAN bus lines
- Terminate the bus with 120Ω at both ends

### SD card module
- ESP32 `3.3V` → SD `3V3`
- ESP32 `GND` → SD `GND`
- ESP32 `GPIO23` → SD `MOSI`
- ESP32 `GPIO19` → SD `MISO`
- ESP32 `GPIO18` → SD `CLK` / `SCK`
- ESP32 `GPIO27` → SD `CS`

> Use the SD module's 3.3V supply pin, not 5V, since the ESP32 is 3.3V logic. If you see intermittent SD errors during uploads, check the module has solid decoupling — WiFi transmit bursts can brown out a marginal supply.

## Setup
1. Copy `include/secrets.h.example` to `include/secrets.h` and fill in your real values (this file is gitignored and never committed):
   - `SECRET_HOTSPOT_SSID` / `SECRET_HOTSPOT_PASSWORD` — the network used for the live dashboard (e.g. a phone hotspot)
   - `SECRET_HOME_SSID` / `SECRET_HOME_PASSWORD` — your home WiFi, used for NTP sync and automatic log upload
   - `SECRET_UPLOAD_SCRIPT_PATH` — the path portion of your Google Apps Script Web App deployment URL (everything after `script.google.com`)
2. Build and flash with PlatformIO (`pio run --target upload`). This board's auto-reset circuit isn't reliable — if you see `Wrong boot mode detected`, hold **BOOT**, tap **EN/RESET** once, keep holding BOOT until the upload starts connecting, then release.
3. Set `local_IP` / `gateway` / `subnet` / `primaryDNS` in `src/main.cpp` to match your hotspot's subnet if it's not an iPhone-style `172.20.10.x` hotspot.

### Google Apps Script upload endpoint
The firmware POSTs the raw CSV file body to `https://script.google.com{SECRET_UPLOAD_SCRIPT_PATH}?filename=<name>.csv` with `Content-Type: text/csv`. The handler in [`google-apps-script/Code.gs`](google-apps-script/Code.gs):
- reads the file from `e.postData.contents`
- saves it to a folder read from a `DRIVE_FOLDER_ID` script property (see setup below), replacing the content of an existing same-named file if one is already there, or creating a new one otherwise — so re-uploading the same day's log later just updates that one file instead of leaving duplicates
- returns a response containing the exact text `UPLOAD_SUCCESS` (the firmware only logs a successful archive when that substring shows up in the response body)
- must be deployed as a Web App with access set to **Anyone**, since the ESP32 connects anonymously

To deploy it:
1. Create a new project at [script.google.com](https://script.google.com) and paste in the contents of `google-apps-script/Code.gs`.
2. Under **Project Settings > Script Properties**, add a property named `DRIVE_FOLDER_ID` with the ID of the Drive folder you want logs uploaded to (the folder ID is the part of its URL after `/folders/`). This keeps the folder ID out of the committed script, the same way `include/secrets.h` keeps credentials out of the firmware source.
3. Deploy as a Web App (execute as yourself, access: Anyone), then copy the path portion of the deployment URL into `SECRET_UPLOAD_SCRIPT_PATH` in `include/secrets.h`.

Note: each idle-triggered sync uploads the full current log file, and the script replaces the existing Drive file of the same name rather than appending — so a day with several parking events converges on one up-to-date file rather than leaving stale partial copies behind.

## Boot sequence
1. Try connecting to home WiFi (≤10s). If found, sync the clock via NTP, then disconnect — this lets a session started at home get a real calendar date for its log filename before anything else needs it.
2. Try connecting to the dashboard hotspot (≤10s). If found: apply the static IP, sync NTP as a fallback if step 1 didn't already succeed, and start the web dashboard + OTA server. If not found: switch to offline logging mode.
3. Initialize CAN and the SD card either way. If the clock was synced (from either network), open (or create) today's dated log file and start writing rows to SD.

If neither network is in range, the ESP32's clock is never set for that boot session, and SD logging is skipped entirely — CAN data still prints to Serial for live viewing, but nothing is written to the card, since log timestamps are wall-clock based and would be meaningless without a synced clock. This also means a session with no clock sync has no log file to upload later, so the idle-triggered Drive sync becomes a no-op for that boot.

## Idle-triggered Drive sync
Once pack current has read within `CURRENT_IDLE_THRESHOLD_MA` of zero (a deadband, not an exact-zero check — current sensors have noise/offset and rarely read exactly 0 at rest) for 15 straight seconds, the logger scans for the home WiFi network. If found, it connects, uploads the current log file to Google Drive, and disconnects/powers down the radio again. This only runs once per idle period (it resets the next time current leaves the deadband), and it blocks CAN reception for the duration of the scan/connect/upload — acceptable since the vehicle is already parked by the time this triggers.

`CURRENT_IDLE_THRESHOLD_MA` defaults to 500 mA; tune it once you have the real current sensor connected and can see its actual noise floor at rest.

## CSV log format
```
timestamp,packV,packI,cell1,cell2,...,cellN
```
- `timestamp`: local wall-clock date/time, formatted `YYYY-MM-DD HH:MM:SS`. Rows are only ever written once NTP has synced the clock that boot (see Boot sequence), so this is always a real, human-readable value — safe to sort/plot across multiple sessions without the timestamp resetting or losing meaning
- `packV`: pack voltage in volts (raw ZEVA value is in tenths of a volt)
- `packI`: pack current in amps, rounded to 0.1 A (raw ZEVA current is a signed 24-bit value offset by 8,388,608)
- `cellN`: individual cell voltage in volts to 3 decimal places (raw ZEVA cell voltage is in millivolts); the number of cell columns in a given row depends on how many modules/cells are actually active, not a fixed count
- the header row is written once, the first time a given log file is created

## Known limitations
- The Google Drive upload connection doesn't validate the server's TLS certificate (`setInsecure()`), so it's not protected against a MITM on that connection.
- The live dashboard loads Chart.js from a public CDN, so it won't render charts if your hotspot has no real internet backhaul (only local link to the ESP32).
- The SD card can occasionally get wedged mid-transaction (seen as repeated `sdCommand(): Card Failed!` / `ff_sd_status(): Check status failed` errors on every read attempt, unrelated to WiFi activity). `uploadLatestLogToGoogleDrive()` retries the read a few times with a software `SD.end()`/`SD.begin()` reinit in between, but that's not guaranteed to recover a truly wedged card — only removing and reapplying power to the board reliably does, since a soft reset/reflash doesn't power-cycle the SD card itself. If uploads keep failing with these errors across resets, a full power cycle is the fix; the log for that session just won't upload until the next successful sync.
