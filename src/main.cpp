#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <driver/twai.h>
#include <time.h>

#include <WiFi.h>
#include <WebServer.h>
#include <ElegantOTA.h>

#include "secrets.h"

// --- NETWORK CREDENTIAL MANAGEMENT ---
// Real values live in include/secrets.h (gitignored) — see include/secrets.h.example
const char* ssid = SECRET_HOTSPOT_SSID;
const char* password = SECRET_HOTSPOT_PASSWORD;

const char* home_ssid = SECRET_HOME_SSID;
const char* home_password = SECRET_HOME_PASSWORD;

// --- TIMEZONE CONFIGURATION ---
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 10 * 3600;     // UTC +10 hours (AEST)
const int   daylightOffset_sec = 3600;     // 1 hour daylight savings shift
static const int NTP_SYNC_MAX_ATTEMPTS = 3;
static const uint32_t NTP_SYNC_TIMEOUT_MS = 4000; // 1000ms was too short for a real DNS lookup + UDP round trip
// WiFi is busiest right after association (DHCP, ARP) in its own background
// task, independent of our sequential code — a brief pause here before any
// SD activity reduces (doesn't guarantee zero) the odds of an SD/WiFi
// electrical collision on marginal wiring/power.
static const uint32_t WIFI_SETTLE_DELAY_MS = 500;

// --- PROXIMITY ENGINE TRACKING VARIABLES ---
static uint32_t zeroCurrentStartTime = 0;
static bool trackingZeroCurrent = false;
static bool hasScannedThisIdleSession = false;

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>ZEVA BMS Cell Monitor</title>
    <!-- Spaced word path bypasses auto-formatter truncation layout errors -->
    <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
    <style>
        body { font-family: Arial, sans-serif; background: #121212; color: #e0e0e0; padding: 10px; margin: 0; text-align: center; }
        h1 { color: #cccccc; font-size: 22px; margin: 10px 0 5px 0; }
        .summary-container { display: flex; justify-content: center; gap: 15px; margin-bottom: 10px; }
        .card { background: #1e1e1e; padding: 10px; border-radius: 8px; width: 130px; border: 1px solid #333; }
        .reading { font-size: 18px; font-weight: bold; }
        .v-text { color: #2e8df3; }
        .i-text { color: #efe52c; }
        .chart-box { max-width: 750px; margin: 10px auto; background: #1e1e1e; padding: 10px; border-radius: 10px; border: 1px solid #333; }
        .chart-title { font-size: 15px; margin-bottom: 8px; color: #909090; font-weight: bold; text-align: left; }
        .cell-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(75px, 1fr)); gap: 6px; max-width: 750px; margin: 15px auto; padding: 5px; }
        .cell-badge { background: #1e1e1e; border: 1px solid #333; border-radius: 4px; padding: 4px; font-size: 11px; text-align: center; font-weight: bold; }
        .color-dot { display: inline-block; width: 8px; height: 8px; border-radius: 5px; margin-right: 4px; }
        .cell-v { color: #ffffff; display: block; font-size: 13px; margin-top: 2px; }
        .btn { display: inline-block; background: #333; color: #fff; padding: 8px 16px; text-decoration: none; border-radius: 5px; margin-top: 10px; font-size: 13px; }
        .btn:hover { background: #444; }
    </style>
</head>
<body>
    <h1>ZEVA BMS Dashboard</h1>
    
    <div class="summary-container">
        <div class="card"><div>Pack Voltage</div><div class="reading v-text"><span id="packV">0.0</span>V</div></div>
        <div class="card"><div>Pack Current</div><div class="reading i-text"><span id="packI">0.0</span>A</div></div>
    </div>

    <!-- CHART 1: Overall Vehicle Load Profile (Dual Axis) -->
    <div class="chart-box">
        <div class="chart-title">Pack Dynamics (Voltage & Current)</div>
        <div style="position: relative; height: 220px; width: 100%;">
            <canvas id="packLoadChart"></canvas>
        </div>
    </div>

    <!-- CHART 2: Per-Cell Voltage Matrix Layer -->
    <div class="chart-box">
        <div class="chart-title" id="dynamic-title">Cell Voltages</div>
        <div style="position: relative; height: 380px; width: 100%;">
            <canvas id="cellTrendChart"></canvas>
        </div>
    </div>

    <div class="cell-grid" id="liveCellGrid"></div>

    <a href="/update" class="btn">Go to Firmware Update</a>

    <script>
        let cellChart = null;
        let packLoadChart = null;
        let isInitialized = false;

        // Initialize the separate Top Pack Chart instantly
        const ctxLoad = document.getElementById('packLoadChart').getContext('2d');
        packLoadChart = new Chart(ctxLoad, {
            type: 'line',
            data: {
                labels: [],
                datasets: [
                    {
                        label: 'Pack Volts',
                        data: [],
                        borderColor: '#2e8df3',
                        borderWidth: 2,
                        pointRadius: 0,
                        yAxisID: 'yVolts'
                    },
                    {
                        label: 'Pack Amps',
                        data: [],
                        borderColor: '#efe52c',
                        borderWidth: 2,
                        pointRadius: 0,
                        yAxisID: 'yAmps'
                    }
                ]
            },
            options: {
                responsive: true,
                maintainAspectRatio: false,
                scales: {
                    x: { grid: { color: '#252525' }, ticks: { color: '#aaa', font: { size: 10 }, maxTicksLimit: 6 } },
                    yVolts: { type: 'linear', display: true, position: 'left', grid: { color: '#252525' }, ticks: { color: '#909090', font: { size: 10 } } },
                    yAmps: { type: 'linear', display: true, position: 'right', grid: { drawOnChartArea: false }, ticks: { color: '#909090', font: { size: 10 } } }
                },
                plugins: { legend: { display: false } }
            }
        });

        function initializeCellDashboard(cellCount) {
            const cellDatasets = [];
            const gridContainer = document.getElementById('liveCellGrid');
            gridContainer.innerHTML = ''; 
            
            document.getElementById('dynamic-title').innerText = `Cell Voltages (${cellCount} Cells Detected)`;

            const tableau20 = [
                '#1f77b4', '#aec7e8', '#ff7f0e', '#ffbb78', '#2ca02c', 
                '#98df8a', '#d62728', '#ff9896', '#9467bd', '#c5b0d5', 
                '#8c564b', '#c49c94', '#e377c2', '#f7b6d2', '#7f7f7f', 
                '#c7c7c7', '#bcbd22', '#dbdb8d', '#17becf', '#9edae5'
            ];

            for (let i = 0; i < cellCount; i++) {
                let colorString = tableau20[i % 20];
                cellDatasets.push({
                    label: `Cell ${i+1}`,
                    data: [],
                    borderColor: colorString,
                    borderWidth: 1.2,
                    pointRadius: 0,
                    tension: 0.1,
                    fill: false
                });

                gridContainer.innerHTML += `
                    <div class="cell-badge">
                        <span class="color-dot" style="background-color: ${colorString}"></span>C${i+1}
                        <span class="cell-v" id="grid-v-${i}">0.00V</span>
                    </div>`;
            }

            const ctxCell = document.getElementById('cellTrendChart').getContext('2d');
            cellChart = new Chart(ctxCell, {
                type: 'line',
                data: { labels: [], datasets: cellDatasets },
                options: {
                    responsive: true,
                    maintainAspectRatio: false,
                    scales: {
                        x: { grid: { color: '#252525' }, ticks: { color: '#aaa', font: { size: 10 }, maxTicksLimit: 6 } },
                        y: { 
                            grid: { color: '#252525' }, 
                            ticks: { color: '#aaa', font: { size: 10 } }, 
                            title: { display: true, text: 'Voltage (V)', color: '#aaa' },
                            min: 2.5,
                            max: 4.5
                        }
                    },
                    plugins: { legend: { display: false } }
                }
            });
            isInitialized = true;
        }

        setInterval(function() {
            fetch('/data').then(res => res.json()).then(data => {
                let liveV = data.voltage / 10;
                let liveI = data.current / 1000;
                
                document.getElementById('packV').innerText = liveV.toFixed(1);
                document.getElementById('packI').innerText = liveI.toFixed(1);

                const timeNow = new Date().toLocaleTimeString([], { hour12: false });

                // 1. Update the overall Pack Load profile trend graph
                packLoadChart.data.labels.push(timeNow);
                packLoadChart.data.datasets[0].data.push(liveV);
                packLoadChart.data.datasets[1].data.push(liveI);

                if (packLoadChart.data.labels.length > 300) {
                    packLoadChart.data.labels.shift();
                    packLoadChart.data.datasets[0].data.shift();
                    packLoadChart.data.datasets[1].data.shift();
                }
                packLoadChart.update('none');

                // 2. Update the isolated cell matrix trend graph
                if (data.cells && data.cells.length > 0) {
                    if (!isInitialized) {
                        initializeCellDashboard(data.cells.length);
                    }

                    cellChart.data.labels.push(timeNow);

                    data.cells.forEach((mvValue, i) => {
                        if (cellChart.data.datasets[i]) {
                            let actualVolts = mvValue / 1000;
                            cellChart.data.datasets[i].data.push(actualVolts);
                            document.getElementById(`grid-v-${i}`).innerText = actualVolts.toFixed(2) + 'V';
                        }
                    });

                    if (cellChart.data.labels.length > 300) {
                        cellChart.data.labels.shift();
                        cellChart.data.datasets.forEach(dataset => dataset.data.shift());
                    }
                    cellChart.update('none');
                }
            });
        }, 1000);
    </script>
</body>
</html>
)rawliteral";


IPAddress local_IP(172, 20, 10, 9);    // The specific IP you want (172.20.10.3)
IPAddress gateway(172, 20, 10, 1);     // Your phone's hotspot controller address
IPAddress subnet(255, 255, 255, 0);   // Standard hotspot network mask
IPAddress primaryDNS(172, 20, 10, 1);  // Use your phone for DNS requests

// Global definition of your web server
WebServer server(80);

static const gpio_num_t CAN_TX_PIN = GPIO_NUM_21;
static const gpio_num_t CAN_RX_PIN = GPIO_NUM_22;
static const uint32_t CAN_SPEED = 250000;
static const uint32_t BMS_BASE_ID = 300;
static const uint8_t MAX_BMS_MODULES = 16;

// ZEVA CAN IDs (from ZEVA EVMS Common.h)
static const uint32_t CAN_BASE_ID = 30;
static const uint32_t CORE_BROADCAST_STATUS = CAN_BASE_ID;
static const uint32_t CORE_SEND_CELL_NUMS = 56; // ZEVA core -> monitor explicit module cell counts
#define CAN_CURRENT_SENSOR_ID 40

struct BmsModuleState {
	bool seen[4];
	uint16_t volts[12];
};

static BmsModuleState bmsModules[MAX_BMS_MODULES];
static bool moduleReady[MAX_BMS_MODULES] = { false };
static uint8_t bmsCellCounts[MAX_BMS_MODULES] = { 0 };
static bool haveBmsCellCounts = false;

// EVMS-level values
static int packVoltage = 0;     // raw ZEVA value in tenths of a volt
static long packCurrent = 0;    // raw current in milliamps after offset correction

// Sensor noise/offset means packCurrent will rarely read exactly 0 at rest;
// treat anything within this deadband as idle. Tune to your current sensor's
// actual noise floor once real hardware is connected.
static const long CURRENT_IDLE_THRESHOLD_MA = 500;

static bool isPackIdle() {
	return labs(packCurrent) < CURRENT_IDLE_THRESHOLD_MA;
}

static void appendPackVoltageValue(char *buffer, size_t size, int &len, int rawVoltage) {
	int whole = rawVoltage / 10;
	int frac = abs(rawVoltage % 10);
	len += snprintf(buffer + len, size - len, ",%d.%d", whole, frac);
}

static void appendPackCurrentValue(char *buffer, size_t size, int &len, long rawCurrent) {
	bool negative = rawCurrent < 0;
	long absCurrent = negative ? -rawCurrent : rawCurrent;
	long currentTenths = (absCurrent + 50) / 100; // round to 0.1 A
	long whole = currentTenths / 10;
	int frac = currentTenths % 10;
	len += snprintf(buffer + len, size - len, ",%s%ld.%d", negative ? "-" : "", whole, frac);
}

static void appendCellVoltageValue(char *buffer, size_t size, int &len, uint16_t rawVoltage) {
	int whole = rawVoltage / 1000;
	int frac = rawVoltage % 1000;
	len += snprintf(buffer + len, size - len, ",%d.%03d", whole, frac);
}

static const int SD_CS_PIN = 27;
static const int SD_MISO_PIN = 19;
static const int SD_MOSI_PIN = 23;
static const int SD_SCK_PIN = 18;
static SPIClass sdSpi(VSPI);
static File logFile;
// A single generic data file rather than one per day: the backend now
// appends every sync onto one persistent Drive file regardless of when the
// data came from, so there's no need to distinguish local files by date —
// the local file only ever holds whatever hasn't been synced yet, and gets
// deleted once a sync fully succeeds (see the idle-sync block in loop()).
static const char *DATA_FILE_PATH = "/data.csv";
// Persists how many bytes of DATA_FILE_PATH have already been successfully
// synced. A backlog (long ride, or several unsynced short ones) can exceed
// what fits in RAM at once, so a sync only has to read/upload as much as
// currently fits, then record how far it got here — the next idle period
// picks up from this offset instead of needing the whole file to fit in one
// pass. Only ever reset (deleted) once the file is fully drained, or when a
// brand-new data file is created (see openDataFile()) since a stale offset
// from a previous file's lifetime can't apply to a new one.
static const char *SYNC_OFFSET_PATH = "/data.offset";
static bool sdInitialized = false;
// Only true once NTP has synced this boot; gates whether we write rows to SD
// at all, since log timestamps are wall-clock based and meaningless without it.
static bool timeSynced = false;

// --- DEBUG/EVENT LOG ---
// A single evergreen text file that mirrors boot/connectivity/upload events
// (not raw telemetry) to SD with a timestamp on each line, so a run can be
// inspected afterward with no serial monitor attached (e.g. battery testing).
// Disabled for now: SD writes during active WiFi (search/connect/NTP) were
// triggering sdCommand() failures on this hardware, and the CSV data logging
// ran fine on its own without this extra SD traffic. Flip back to true to
// re-enable once the underlying SD/WiFi issue is resolved (e.g. decoupling
// capacitor added) — nothing else needs to change, DebugLog just falls
// through to Serial-only while this is off.
static const bool DEBUG_LOG_ENABLED = false;
static const char *DEBUG_LOG_PATH = "/log.txt";
static File debugFile;

static bool openDebugLogFile() {
	if (!DEBUG_LOG_ENABLED || !sdInitialized) return false;
	if (debugFile) debugFile.close();
	debugFile = SD.open(DEBUG_LOG_PATH, FILE_APPEND);
	return (bool)debugFile;
}

static void formatDebugTimestamp(char *buf, size_t size) {
	// Check the clock directly rather than the timeSynced flag: configTime()
	// takes effect immediately, before the caller gets a chance to set that
	// flag, so this is accurate even for the sync-succeeded message itself.
	time_t now = time(nullptr);
	if (now > 24 * 3600) {
		struct tm timeinfo;
		localtime_r(&now, &timeinfo);
		strftime(buf, size, "%Y-%m-%d %H:%M:%S", &timeinfo);
	} else {
		// Clock isn't synced yet (or never syncs this boot) — still useful to
		// know when in the boot timeline something happened.
		snprintf(buf, size, "boot+%lums", (unsigned long)millis());
	}
}

// Mirrors Print output to both the real Serial port and the debug log file.
// Writes go to SD immediately (nothing sits buffered in RAM waiting for a
// future newline), but flushing is rate-limited: a WiFi search/connect loop
// calls print(".") up to ~20 times over 10s, and forcing a physical SD flush
// on every single one of those hammers the card during WiFi's busiest window
// (association, DHCP) — observed to trigger SD errors on marginal wiring. So
// we always flush when a line starts (a crash still records that something
// began) and when a line completes, but a run of dots in between only forces
// a flush if it's been a while, as a safety net for a genuine stall.
class TeeLogger : public Print {
public:
	size_t write(uint8_t c) override {
		if (writeByte(c)) doFlush();
		return Serial.write(c);
	}

	size_t write(const uint8_t *buffer, size_t size) override {
		bool shouldFlush = false;
		for (size_t i = 0; i < size; ++i) {
			if (writeByte(buffer[i])) shouldFlush = true;
		}
		if (shouldFlush) doFlush();
		return Serial.write(buffer, size);
	}

private:
	bool atLineStart = true;
	uint32_t lastFlushMs = 0;
	static const uint32_t MAX_FLUSH_INTERVAL_MS = 3000;

	// Returns true if this byte is a point worth flushing at.
	bool writeByte(uint8_t c) {
		if (!debugFile) return false;
		if (c == '\r') return false; // normalize CRLF -> LF
		if (c == '\n') {
			if (!atLineStart) {
				debugFile.write((uint8_t)'\n');
				atLineStart = true;
				return true; // line completed
			}
			return false; // blank line, swallowed
		}
		bool lineJustStarted = false;
		if (atLineStart) {
			char stamp[24];
			formatDebugTimestamp(stamp, sizeof(stamp));
			debugFile.printf("[%s] ", stamp);
			atLineStart = false;
			lineJustStarted = true;
		}
		debugFile.write(c);
		if (lineJustStarted) return true; // line started
		return (millis() - lastFlushMs) >= MAX_FLUSH_INTERVAL_MS; // stall safety net
	}

	void doFlush() {
		debugFile.flush();
		lastFlushMs = millis();
	}
};

static TeeLogger DebugLog;

static void writeLogHeader(File &file, int totalCellCount);

static size_t readSyncOffset() {
	if (!SD.exists(SYNC_OFFSET_PATH)) return 0; // avoids a noisy vfs_api.cpp error for the common no-offset-yet case
	File f = SD.open(SYNC_OFFSET_PATH, FILE_READ);
	if (!f) return 0;
	char buf[16] = {0};
	size_t n = f.read((uint8_t *)buf, sizeof(buf) - 1);
	f.close();
	buf[n] = '\0';
	long val = atol(buf);
	return val > 0 ? (size_t)val : 0;
}

// Removes and recreates the offset file rather than overwriting in place —
// FILE_WRITE doesn't truncate, so a shorter new value (e.g. "512" replacing
// "268729") would otherwise leave trailing digits from the old one behind.
static void writeSyncOffset(size_t offset) {
	if (SD.exists(SYNC_OFFSET_PATH)) SD.remove(SYNC_OFFSET_PATH);
	File f = SD.open(SYNC_OFFSET_PATH, FILE_WRITE);
	if (!f) return;
	f.print(offset);
	f.close();
}

static void clearSyncOffset() {
	if (SD.exists(SYNC_OFFSET_PATH)) SD.remove(SYNC_OFFSET_PATH);
}

// True for the rest of this boot after setup() eagerly creates a brand-new
// /data.csv with a placeholder-width header (MAX_TOTAL_CELL_COUNT, so the
// file exists immediately for bench testing without real CAN data) — cleared
// once the first real CAN row rewrites that header to the true cell count
// (see trimDataFileHeaderIfNeeded()). Never set when resuming an existing
// file, since its header (from whenever it was first created) is already
// meaningful and shouldn't be touched again.
static bool dataFileNeedsHeaderTrim = false;

// totalCellCount only matters if this call ends up creating a brand-new
// file — it becomes the header's cell1..cellN width.
static bool openDataFile(int totalCellCount) {
	if (!sdInitialized) return false;
	if (logFile) return true; // already open

	bool isNewFile = !SD.exists(DATA_FILE_PATH);

	logFile = SD.open(DATA_FILE_PATH, FILE_APPEND);
	if (!logFile) {
		return false;
	}

	if (isNewFile) {
		writeLogHeader(logFile, totalCellCount);
		clearSyncOffset(); // a leftover offset can't apply to this new file
	}
	return true;
}

// Called once the real cell count is known (the first CAN row of this
// boot). If setup() created /data.csv fresh with the MAX_TOTAL_CELL_COUNT
// placeholder header, that file so far contains nothing but that one header
// line (data rows only start once real CAN current is flowing) — safe to
// discard and recreate with the header it should have had all along.
static void trimDataFileHeaderIfNeeded(int totalCellCount) {
	if (!dataFileNeedsHeaderTrim) return;
	dataFileNeedsHeaderTrim = false;
	if (logFile) logFile.close();
	SD.remove(DATA_FILE_PATH);
	logFile = SD.open(DATA_FILE_PATH, FILE_APPEND);
	if (logFile) {
		writeLogHeader(logFile, totalCellCount);
	}
	// If this reopen failed for some reason, logFile is left invalid and the
	// normal openDataFile() call right after this naturally recreates the
	// file (correctly, since it no longer exists) instead of getting stuck.
}

static uint8_t countUsedModuleCells(const BmsModuleState &state) {
	const uint16_t threshold = 1000; // assume unused cells are near 0
	uint8_t count = 0;
	for (int i = 0; i < 12; ++i) {
		if (state.volts[i] >= threshold)
			++count;
		else
			break;
	}
	return count;
}

// Placeholder header width used only for the eager boot-time file creation
// (see setup()) — trimmed down to the real cell count on the first CAN row.
static const int MAX_TOTAL_CELL_COUNT = MAX_BMS_MODULES * 12;

static uint8_t getModuleCellCount(uint8_t moduleId) {
	if (haveBmsCellCounts)
		return bmsCellCounts[moduleId];
	return countUsedModuleCells(bmsModules[moduleId]);
}

static bool readyPackIsContiguous() {
	int maxModule = -1;
	for (int i = 0; i < MAX_BMS_MODULES; ++i) {
		if (moduleReady[i]) maxModule = i;
	}
	if (maxModule < 0) return false;
	for (int i = 0; i <= maxModule; ++i) {
		if (!moduleReady[i]) return false;
	}
	return true;
}

static void clearReadyModules() {
	for (int i = 0; i < MAX_BMS_MODULES; ++i) moduleReady[i] = false;
}

static void writeLogHeader(File &file, int totalCellCount) {
	file.print("timestamp,packV,packI");
	for (int i = 1; i <= totalCellCount; ++i) {
		file.print(",cell");
		file.print(i);
	}
	file.println();
}

void printPackVoltages(uint32_t timestamp) {
	time_t rawTime = (time_t)timestamp;
	struct tm timeinfo;
	localtime_r(&rawTime, &timeinfo);
	char timeStr[24];
	strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);

	char buffer[2048];
	int len = snprintf(buffer, sizeof(buffer), "%s", timeStr);
	appendPackVoltageValue(buffer, sizeof(buffer), len, packVoltage);
	appendPackCurrentValue(buffer, sizeof(buffer), len, packCurrent);

	int totalCellCount = 0;
	for (int m = 0; m < MAX_BMS_MODULES; ++m) {
		uint8_t used = getModuleCellCount(m);
		for (int c = 0; c < used; ++c) {
			appendCellVoltageValue(buffer, sizeof(buffer), len, bmsModules[m].volts[c]);
			totalCellCount++;
		}
	}

	// Always output to the Serial Terminal for instant debugging
	Serial.println(buffer);

	// ONLY write to the SD card if the clock is synced (timestamps are wall-clock
	// based) and current is outside the idle deadband (charging or discharging)
	if (sdInitialized && timeSynced && !isPackIdle()) {
		trimDataFileHeaderIfNeeded(totalCellCount);
		if (openDataFile(totalCellCount)) {
			logFile.println(buffer);
			logFile.flush(); // Safely saves data to the flash sector
		} else {
			DebugLog.println("Failed to open data file.");
		}
	}
}

bool setupCAN() {
	const twai_timing_config_t timing_config = TWAI_TIMING_CONFIG_250KBITS();
	const twai_filter_config_t filter_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();
	const twai_general_config_t general_config = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_PIN, CAN_RX_PIN, TWAI_MODE_NORMAL);

	esp_err_t err = twai_driver_install(&general_config, &timing_config, &filter_config);
	if (err != ESP_OK) {
		DebugLog.printf("TWAI driver install failed: %d\n", err);
		return false;
	}

	err = twai_start();
	if (err != ESP_OK) {
		DebugLog.printf("TWAI start failed: %d\n", err);
		return false;
	}

	DebugLog.printf("CAN initialized at %u kbps\n", CAN_SPEED / 1000);
	return true;
}

static bool syncTimeFromNTP() {
	for (int attempt = 1; attempt <= NTP_SYNC_MAX_ATTEMPTS; ++attempt) {
		DebugLog.printf("Syncing calendar time from internet NTP (attempt %d/%d)...\n", attempt, NTP_SYNC_MAX_ATTEMPTS);
		// Re-issued each attempt, not just re-waited on: a single lost NTP
		// packet would otherwise leave nothing to retry within the timeout.
		configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

		struct tm timeinfo;
		if (getLocalTime(&timeinfo, NTP_SYNC_TIMEOUT_MS)) {
			DebugLog.println("System time synchronized successfully!");
			return true;
		}

		if (attempt < NTP_SYNC_MAX_ATTEMPTS) {
			delay(500);
		}
	}
	DebugLog.println("NTP Sync timed out after multiple attempts.");
	return false;
}

void setup() {
	Serial.begin(115200);
	delay(1000);

	// Bring up the SD card and the debug log first, before any WiFi activity,
	// so the boot/connectivity sequence below gets captured to /log.txt even
	// with no serial monitor attached.
	pinMode(SD_CS_PIN, OUTPUT);
	digitalWrite(SD_CS_PIN, HIGH);
	delay(10);
	sdSpi.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);

	if (!SD.begin(SD_CS_PIN, sdSpi)) {
		Serial.println("SD initialization failed. Check SD card wiring.");
	} else {
		sdInitialized = true;
		Serial.println("SD initialized.");
		if (!openDebugLogFile()) {
			if (DEBUG_LOG_ENABLED) {
				Serial.println("Failed to open debug log file; continuing with Serial only.");
			} else {
				Serial.println("Debugging turned off.");
			}
		}
	}

	WiFi.mode(WIFI_STA);

	// 0. If we're already in range of home Wi-Fi (e.g. starting the ride from
	// home), connect briefly just to sync the clock before the data file for
	// this session gets named further down.
	DebugLog.print("Checking for Home Wi-Fi (10s timeout)");
	WiFi.begin(home_ssid, home_password);

	int homeAttemptCounter = 0;
	while (WiFi.status() != WL_CONNECTED && homeAttemptCounter < 20) {
		delay(500);
		DebugLog.print(".");
		homeAttemptCounter++;
	}

	if (WiFi.status() == WL_CONNECTED) {
		DebugLog.println("\nHome Wi-Fi found.");
		delay(WIFI_SETTLE_DELAY_MS); // let association/DHCP traffic settle before more SD activity
		timeSynced = syncTimeFromNTP();
		WiFi.disconnect();
		delay(200);
	} else {
		DebugLog.println("\nHome Wi-Fi not found at boot.");
	}

	// 1. Set wireless station properties
	WiFi.begin(ssid, password);
	DebugLog.print("Searching for Hotspot (10s timeout)");

	int attemptCounter = 0;
	bool wifiConnected = true;

	// Loop 20 times with a 500ms delay = 10 seconds total tracking window
	while (WiFi.status() != WL_CONNECTED) {
		delay(500);
		DebugLog.print(".");
		attemptCounter++;

		if (attemptCounter >= 20) {
			wifiConnected = false;
			break;
		}
	}

	// BRANCH A: ONLINE MODE (Hotspot Found!)
	if (wifiConnected) {
		DebugLog.println("\nConnected successfully!!! Network node established.");
		delay(WIFI_SETTLE_DELAY_MS); // let association/DHCP traffic settle before more SD activity
		DebugLog.print("IP Address: ");
		DebugLog.println(WiFi.localIP());

		// 2. Request chosen static IP configuration allocation
		if (!WiFi.config(local_IP, gateway, subnet, primaryDNS)) {
			DebugLog.println("Static IP configuration failed to apply!");
		}

		// 3. Fall back to NTP here too, in case home Wi-Fi wasn't in range
		// (or had no internet) at boot.
		if (!timeSynced) {
			timeSynced = syncTimeFromNTP();
		}

		// 4. Mount local system web routes
		server.on("/", HTTP_GET, [](){
			server.send_P(200, "text/html", index_html);
		});

		server.on("/data", HTTP_GET, [](){
			String json = "{";
			json += "\"voltage\":" + String(packVoltage) + ",";
			json += "\"current\":" + String(packCurrent) + ",";
			json += "\"cells\":[";

			bool firstCell = true;
			for (int m = 0; m < MAX_BMS_MODULES; m++) {
				uint8_t cellsInThisModule = getModuleCellCount(m);
				for (int c = 0; c < cellsInThisModule; c++) {
					if (!firstCell) json += ",";
					json += String(bmsModules[m].volts[c]);
					firstCell = false;
				}
			}
			json += "]}";
			server.send(200, "application/json", json);
		});

		// 5. Initialize the WebOTA Dashboard Interface and Start Server
		ElegantOTA.begin(&server);
		server.begin();
		DebugLog.println("Web Server initialized.");

	// BRANCH B: OFFLINE MODE (Hotspot Missing)
	} else {
		DebugLog.println("\nHotspot not found. Switching to Offline Logger Mode.");
		WiFi.disconnect(true);
		WiFi.mode(WIFI_OFF); // Power off antenna arrays completely
	}

	// BOTH BRANCHES CONTINUOUSLY RUN INDEPENDENT HARDWARE CONFIGURATIONS BELOW HERE
	DebugLog.println("ZEVA ESP32 CAN cell voltage logger");
	DebugLog.println("Waiting for CAN data...");

	if (!setupCAN()) {
		DebugLog.println("CAN initialization failed. Check CAN wiring and transceiver.");
	}

	// SD card is already mounted (see top of setup()); now that the clock
	// situation is known, open (or resume appending to) the data file if
	// synced — eagerly, with a placeholder-width header, so the file exists
	// immediately (useful for bench testing without real CAN data). The
	// header gets trimmed to the real cell count on the first CAN row
	// actually logged (see trimDataFileHeaderIfNeeded()) — only when this
	// call is the one creating the file fresh, not when resuming an
	// existing one that already has a meaningful header.
	if (sdInitialized) {
		if (timeSynced) {
			bool wasNewFile = !SD.exists(DATA_FILE_PATH);
			if (openDataFile(MAX_TOTAL_CELL_COUNT)) {
				if (wasNewFile) {
					dataFileNeedsHeaderTrim = true;
				}
				DebugLog.printf("Logging enabled: %s\n", DATA_FILE_PATH);
				logFile.flush();
			} else {
				DebugLog.println("Failed to open data file for writing.");
			}
		} else {
			DebugLog.println("Time not synced this boot; SD logging disabled for this session.");
		}
	}

	memset(bmsModules, 0, sizeof(bmsModules));

	Serial.printf("[Heap] End of setup — free: %u, largest block: %u\n",
		(unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
}


void processBmsMessage(uint32_t packetId, const uint8_t data[8], size_t length) {
	if (packetId >= BMS_BASE_ID) {
		uint32_t index = packetId - BMS_BASE_ID;
		uint8_t moduleId = index / 10;
		uint8_t packetType = index % 10;

		if (moduleId >= MAX_BMS_MODULES) {
			return;
		}

		BmsModuleState &state = bmsModules[moduleId];

		switch (packetType) {
		case 1:
			for (int i = 0; i < 4 && i * 2 + 1 < length; ++i) {
				state.volts[i] = (uint16_t(data[i * 2]) << 8) | data[i * 2 + 1];
			}
			state.seen[0] = true;
			break;
		case 2:
			for (int i = 0; i < 4 && i * 2 + 1 < length; ++i) {
				state.volts[4 + i] = (uint16_t(data[i * 2]) << 8) | data[i * 2 + 1];
			}
			state.seen[1] = true;
			break;
		case 3:
			for (int i = 0; i < 4 && i * 2 + 1 < length; ++i) {
				state.volts[8 + i] = (uint16_t(data[i * 2]) << 8) | data[i * 2 + 1];
			}
			state.seen[2] = true;
			break;
		case 4:
			state.seen[3] = true;
			break;
		default:
			return;
	}

	if (state.seen[0] && state.seen[1] && state.seen[2] && state.seen[3]) {
		moduleReady[moduleId] = true;
		if (readyPackIsContiguous()) {
			printPackVoltages((uint32_t)time(nullptr));
			clearReadyModules();
		}
	}
	return;
	}

	// Handle EVMS/core messages (IDs below BMS_BASE_ID)
	switch (packetId) {
		case CORE_BROADCAST_STATUS:
			// Pack voltage is stored in bytes 3..4 (big-endian) per ZEVA EVMS
			if (length >= 5) {
				packVoltage = (int(data[3]) << 8) | data[4];
			}
			break;
		case CORE_SEND_CELL_NUMS:
			if (length >= 8) {
				for (int i = 0; i < 8; ++i) {
					uint8_t byte = data[i];
					bmsCellCounts[i*2] = byte & 0x0F;
					bmsCellCounts[i*2 + 1] = byte >> 4;
				}
				haveBmsCellCounts = true;
			}
			break;
		case CAN_CURRENT_SENSOR_ID:
			// 24-bit value with offset; replicate ZEVA behavior: value - 8388608
			if (length >= 3) {
				unsigned long raw = ((unsigned long)data[0] << 16) | ((unsigned long)data[1] << 8) | (unsigned long)data[2];
				packCurrent = (long)raw - 8388608L;
			}
			break;
		default:
			// Other messages not handled here
			break;
	}
}

#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// A ride's worth of data can exceed what fits in one RAM allocation (seen in
// practice: many short rides sharing one growing file). Reading/uploading in
// chunks instead of one big buffer avoids needing a single large contiguous
// allocation.
//
// The chunk size is adaptive rather than fixed: testing showed a 32KB
// malloc() fail with 244KB free heap and a 110KB largest block reported
// available, and — after shrinking to 16KB specifically to stay under a
// ~40KB floor observed during that failure — a 16KB malloc() then ALSO fail
// with a 40KB largest block still reported available. In both cases the
// reported "largest free block" was comfortably bigger than the request that
// failed, so it isn't a reliable predictor of what will actually succeed
// here. Rather than continuing to guess a fixed "safe" size against numbers
// that don't seem to line up with reality, mallocChunkAdaptive() just tries
// the preferred size and halves it on failure until something works (or a
// floor is hit), and reads exactly that much — so chunk sizes vary at
// runtime based on what's actually available, not a static guess.
static const size_t UPLOAD_CHUNK_SIZE = 16384;
static const size_t MIN_UPLOAD_CHUNK_SIZE = 512;
static const int MAX_UPLOAD_CHUNKS = 128;
// Hard cap on total bytes read into RAM in one batch, independent of how
// much more memory is technically available. Found the real cause of every
// "mystery" heap failure chased earlier: reading everything that would fit
// (once, 196KB across 26 chunks) starved WiFi of the memory it needs to
// initialize its own task/buffers on the way back up afterward
// (`wifi:create wifi task: failed to create task`). Capping how much we ever
// hold at once guarantees WiFi has room to start regardless of exactly how
// much total heap turns out to be available at that moment.
static const size_t MAX_BATCH_BYTES = 32768;

struct UploadChunk {
    uint8_t *data;
    size_t length;
};

// Tries `desired` bytes, halving on failure down to MIN_UPLOAD_CHUNK_SIZE.
// Returns the buffer and sets *outSize to whatever size actually succeeded,
// or returns nullptr if even the floor size failed.
static uint8_t *mallocChunkAdaptive(size_t desired, size_t *outSize) {
    size_t trySize = desired;
    while (trySize >= MIN_UPLOAD_CHUNK_SIZE) {
        uint8_t *buf = (uint8_t *)malloc(trySize);
        if (buf) {
            *outSize = trySize;
            return buf;
        }
        trySize /= 2;
    }
    *outSize = 0;
    return nullptr;
}

// Single-attempt chunked read (see readDataFileToChunks() for the retry
// wrapper), starting at startOffset (bytes already synced from a previous
// batch). *outBytesRead is the total across all chunks in this batch;
// *outReachedEOF says whether this batch reached the end of the file (i.e.
// nothing will remain unsynced once this batch uploads), or stopped early
// (hit the chunk cap, or ran out of memory) with more left for next time —
// CAN reception is blocked for the whole sync attempt, so the file can't
// grow out from under this read mid-way.
//
// Returns the chunk count (>= 0, possibly 0 at genuine EOF) on any run that
// made progress or found nothing left to do. Only returns -1 — a real
// failure worth retrying via readDataFileToChunks()'s SD reinit — for an
// actual I/O problem (open/seek/short-read), or hitting the memory ceiling
// before a single chunk could be read at all. Running out of memory *after*
// reading at least one chunk is treated as a normal stopping point, not a
// failure: whatever was read is still good data, worth uploading and
// resuming from rather than discarding and starting over.
static int readDataFileToChunksOnce(UploadChunk *chunks, int maxChunks, size_t startOffset, size_t *outBytesRead, bool *outReachedEOF) {
    File fileToStream = SD.open(DATA_FILE_PATH, FILE_READ);
    if (!fileToStream) {
        DebugLog.println("[Uploader] Failed to open data file for reading.");
        return -1;
    }
    if (startOffset > 0 && !fileToStream.seek(startOffset)) {
        DebugLog.println("[Uploader] Failed to seek to sync offset.");
        fileToStream.close();
        return -1;
    }

    int chunkCount = 0;
    size_t totalBytes = 0;
    bool ioFailure = false;
    while (fileToStream.available() > 0) {
        if (chunkCount >= maxChunks) {
            // Not a failure — upload what we have and continue from here next sync.
            DebugLog.printf("[Uploader] Batch limit (%d chunks) reached; remaining data will sync next time.\n", maxChunks);
            break;
        }
        if (totalBytes >= MAX_BATCH_BYTES) {
            // Not a failure either — deliberately stop well short of "everything
            // that fits" so WiFi has enough free heap left to start up afterward.
            DebugLog.printf("[Uploader] Batch size cap (%u bytes) reached; remaining data will sync next time.\n", (unsigned)MAX_BATCH_BYTES);
            break;
        }

        size_t desired = (size_t)fileToStream.available();
        if (desired > UPLOAD_CHUNK_SIZE) desired = UPLOAD_CHUNK_SIZE;

        size_t toRead = 0;
        uint8_t *buf = mallocChunkAdaptive(desired, &toRead);
        if (!buf) {
            // Also not a failure if we already have something: stop here,
            // upload what fit, and continue from this point next sync.
            DebugLog.printf("[Uploader] Out of memory for further chunks (have %d so far); uploading that and continuing next sync. Free heap: %u, largest block: %u\n",
                chunkCount, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
            break;
        }
        if (toRead < desired) {
            DebugLog.printf("[Uploader] Wanted %u-byte chunk, only got %u; continuing with smaller chunks.\n",
                (unsigned)desired, (unsigned)toRead);
        }

        size_t got = fileToStream.read(buf, toRead);
        if (got != toRead) {
            DebugLog.println("[Uploader] Short read while chunking data file.");
            free(buf);
            ioFailure = true;
            break;
        }

        chunks[chunkCount].data = buf;
        chunks[chunkCount].length = got;
        totalBytes += got;
        chunkCount++;
    }
    bool reachedEOF = !ioFailure && (fileToStream.available() == 0);
    fileToStream.close();

    if (ioFailure) {
        for (int i = 0; i < chunkCount; ++i) free(chunks[i].data);
        return -1;
    }
    if (chunkCount == 0 && !reachedEOF) {
        // Couldn't make any progress at all this attempt (out of memory
        // before even the first chunk) — signal failure so the retry
        // wrapper gives it another shot rather than reporting a hollow
        // "success" with nothing to actually upload.
        return -1;
    }

    *outBytesRead = totalBytes;
    *outReachedEOF = reachedEOF;
    return chunkCount;
}

// Reads (a batch of, starting at startOffset) the data file into RAM chunks.
// Meant to be called while WiFi is off — the only time SD access on this
// hardware is guaranteed not to collide with active radio transmission — so
// the later upload step needs no more SD access at all, regardless of how
// busy WiFi gets while connecting/sending. Returns the chunk count (>= 0) on
// success, or -1 on failure.
//
// SD is already mounted from setup() and hasn't been touched by WiFi since
// (the radio's been off this whole idle period), so the first attempt just
// reads directly rather than reinitializing SD first — repeated SD.end()/
// SD.begin() cycles were observed to leak a large amount of heap (~130KB in
// one case) on this SD library, and the "radio might have crashed it"
// justification for reiniting preemptively no longer applies now that SD
// access never overlaps with WiFi. The reinit is kept as an actual recovery
// step for retries, since the SD bus can still glitch mid-read on its own.
static int readDataFileToChunks(UploadChunk *chunks, int maxChunks, size_t startOffset, size_t *outBytesRead, bool *outReachedEOF) {
    const int MAX_READ_ATTEMPTS = 3;
    for (int attempt = 1; attempt <= MAX_READ_ATTEMPTS; ++attempt) {
        if (attempt > 1) {
            DebugLog.printf("[Uploader] Retrying SD read (attempt %d/%d)...\n", attempt, MAX_READ_ATTEMPTS);
            SD.end();
            delay(200);
            if (!SD.begin(SD_CS_PIN, sdSpi)) {
                DebugLog.println("[Uploader] SD reinit failed during retry.");
                continue;
            }
            openDebugLogFile(); // reinit above invalidated the debug file handle too
        }

        int chunkCount = readDataFileToChunksOnce(chunks, maxChunks, startOffset, outBytesRead, outReachedEOF);
        if (chunkCount >= 0) {
            DebugLog.printf("[Uploader] Read %u bytes into %d chunk(s) from offset %u%s.\n",
                (unsigned)*outBytesRead, chunkCount, (unsigned)startOffset, *outReachedEOF ? " (reached end of file)" : "");
            return chunkCount;
        }
    }

    DebugLog.println("[Uploader] Giving up after repeated SD read failures.");
    return -1;
}

// POSTs one chunk; the backend appends it to the persistent Drive file
// (isFirstChunk tells it whether to strip a leading header line, since only
// the first chunk of a batch can contain one). No SD access — call only once
// WiFi is connected.
static bool uploadOneChunk(uint8_t *buffer, size_t size, bool isFirstChunk) {
    DebugLog.printf("[Uploader] Uploading chunk (%d bytes, first=%s)...\n", (int)size, isFirstChunk ? "true" : "false");

    String serverPath = String(SECRET_UPLOAD_SCRIPT_PATH) + "?filename=data.csv&first=" + (isFirstChunk ? "true" : "false");
    String fullUrl = "https://script.google.com" + serverPath;

    WiFiClientSecure client;
    client.setInsecure(); // Bypass strict Google certificate chains

    HTTPClient http;
    http.setTimeout(25000); // 25 seconds for slow cellular links
    // Handled manually below: Google Apps Script's /exec endpoint 302-redirects to a
    // signed googleusercontent.com URL, and HTTPClient can't reliably resend a
    // streamed file body when auto-following a redirect.
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);

    bool success = false;

    if (http.begin(client, fullUrl)) {
        http.addHeader("Content-Type", "text/csv");

        int httpCode = http.sendRequest("POST", buffer, size);

        if (httpCode == HTTP_CODE_FOUND || httpCode == HTTP_CODE_MOVED_PERMANENTLY || httpCode == HTTP_CODE_TEMPORARY_REDIRECT) {
            // Google has already executed doPost() and computed the result by this point;
            // the redirect target just serves that result back and only accepts GET (no body).
            String redirectUrl = http.getLocation();
            http.end();

            if (redirectUrl.length() > 0) {
                DebugLog.println("[Uploader] Following redirect to execution result...");
                if (http.begin(client, redirectUrl)) {
                    httpCode = http.GET();
                } else {
                    DebugLog.println("[Uploader] Failed to initialize redirect connection handle.");
                }
            } else {
                DebugLog.println("[Uploader] Redirect response missing Location header.");
            }
        }

        if (httpCode > 0) {
            String response = http.getString();
            DebugLog.printf("[Uploader] Server response code: %d\n", httpCode);
            DebugLog.println("[Uploader] Response: " + response);

            success = response.indexOf("UPLOAD_SUCCESS") != -1;
            if (success) {
                DebugLog.println("[Uploader] Chunk archived to Google Drive.");
            }
        } else {
            DebugLog.printf("[Uploader] Transport layer failed: %s\n", http.errorToString(httpCode).c_str());
        }
        http.end();
    } else {
        DebugLog.println("[Uploader] Failed to initialize connection handle.");
    }

    return success;
}

// Uploads a sequence of already-read chunks, stopping at the first failure
// rather than skipping ahead (so a partial failure never leaves a gap in the
// uploaded data — this batch just gets retried from its starting offset next
// sync, at the cost of possible duplicate rows if some chunks had already
// landed). batchStartsAtFileBeginning should be true only when this batch's
// offset is 0 — only then can its first chunk possibly contain a CSV header
// for uploadOneChunk() to flag. Frees every chunk buffer before returning, on
// every path. Returns true only if every chunk was uploaded successfully.
static bool uploadChunksToGoogleDrive(UploadChunk *chunks, int chunkCount, bool batchStartsAtFileBeginning) {
    bool allOk = (WiFi.status() == WL_CONNECTED);
    if (!allOk) {
        DebugLog.println("[Uploader] Error: Wi-Fi disconnected.");
    }

    for (int i = 0; i < chunkCount; ++i) {
        if (allOk) {
            allOk = uploadOneChunk(chunks[i].data, chunks[i].length, batchStartsAtFileBeginning && i == 0);
        }
        free(chunks[i].data);
        chunks[i].data = nullptr;
    }

    return allOk;
}


void loop() {
	static uint32_t lastPrint = 0;

	// 1. Continuous high-speed CAN packet processing
	twai_message_t message;
	if (twai_receive(&message, 0) == ESP_OK) {
		if (message.extd) {
			processBmsMessage(message.identifier, message.data, message.data_length_code);
		}
	}

	// 2. THE IDLE PROXIMITY ENGINE (Monitors home location arrival states)
	if (isPackIdle()) {
		if (!trackingZeroCurrent) {
			zeroCurrentStartTime = millis();
			trackingZeroCurrent = true;
			hasScannedThisIdleSession = false;
			DebugLog.print("Vehicle Idle...");
		}

		// If current stays at exactly 0.0A for 15 seconds straight, scan for garage network
		if (trackingZeroCurrent && (millis() - zeroCurrentStartTime >= 15000) && !hasScannedThisIdleSession) {
			hasScannedThisIdleSession = true;

			DebugLog.println("\n[Idle Sensor] Vehicle stationary for 15s.");

			if (!SD.exists(DATA_FILE_PATH)) {
				DebugLog.println("[Idle Sensor] No data file this session; nothing to upload.");
			} else {
				DebugLog.println("[Idle Sensor] Booting up Wi-Fi Radio Hardware...");

				// 1. Reactivate station mode cleanly without dropping credential buffers
				WiFi.mode(WIFI_STA);
				delay(400);
				WiFi.disconnect(false); // Soft reset channels only, leaves parameters safe!
				delay(300);

				DebugLog.println("[Idle Sensor] Scanning for Home Wi-Fi...");

				int networkCount = WiFi.scanNetworks();
				bool foundHome = false;

				for (int i = 0; i < networkCount; ++i) {
					if (WiFi.SSID(i) == String(home_ssid)) {
						foundHome = true;
						break;
					}
				}
				WiFi.scanDelete();

				if (!foundHome) {
					DebugLog.println("[Idle Sensor] Home network not spotted. Going dark.");
				} else {
					DebugLog.println("[Idle Sensor] Home Wi-Fi within range! Syncing full backlog, one batch at a time...");

					// Keep looping — read a batch (radio off), connect, upload it,
					// and if there's still more backlog, go straight into the
					// next batch — until the whole file is drained or something
					// stops progress. Deliberately unhurried: as long as the
					// vehicle stays parked here, blocking CAN reception a while
					// longer to fully catch up is an acceptable trade for never
					// losing logged rows to a capped per-visit sync.
					bool keepGoing = true;
					int batchNum = 0;
					const int MAX_BATCHES_PER_SESSION = 500; // safety backstop, not an expected ceiling

					while (keepGoing && batchNum < MAX_BATCHES_PER_SESSION) {
						batchNum++;

						// Radio off for the SD read — already off from the scan
						// above on the first batch; a prior batch's upload left
						// it on, so later batches need to power it down again.
						WiFi.disconnect(true);
						WiFi.mode(WIFI_OFF);
						delay(200);

						if (logFile) { logFile.close(); } // release the handle before the reinit below
						UploadChunk pendingChunks[MAX_UPLOAD_CHUNKS];
						size_t syncStartOffset = readSyncOffset();
						size_t pendingBytesRead = 0;
						bool pendingReachedEOF = false;
						int pendingChunkCount = readDataFileToChunks(pendingChunks, MAX_UPLOAD_CHUNKS, syncStartOffset, &pendingBytesRead, &pendingReachedEOF);

						if (pendingChunkCount < 0) {
							DebugLog.println("[Idle Sensor] Read failed; will resume this backlog next sync.");
							break;
						}
						if (pendingChunkCount == 0) {
							// Nothing left to read — already fully synced (e.g. a
							// prior session finished but cleanup didn't run).
							if (pendingReachedEOF) {
								SD.remove(DATA_FILE_PATH);
								clearSyncOffset();
								DebugLog.println("[Idle Sensor] Data file was already fully synced; cleared.");
							}
							break;
						}

						// --- FORCE AN ABSOLUTE HARDWARE & STACK REBOOT OF THE RADIO ---
						WiFi.disconnect(true, true); // True #2 completely clears stored system credentials
						WiFi.mode(WIFI_OFF);         // Shut down native stack drivers completely
						delay(200);                  // Let registers drain completely

						WiFi.mode(WIFI_STA);         // Spin up a brand new, empty network profile instance
						delay(200);
						// ---------------------------------------------------------------

						DebugLog.printf("[Idle Sensor] Connecting to Home Router for batch %d...\n", batchNum);
						WiFi.begin(home_ssid, home_password);
						int connectAttempts = 0;

						while (WiFi.status() != WL_CONNECTED && connectAttempts < 20) {
							delay(500);
							DebugLog.print(".");
							connectAttempts++;
						}

						if (WiFi.status() != WL_CONNECTED) {
							DebugLog.println("\n[Idle Sensor] Lost the home connection mid-sync; will resume this backlog next sync.");
							for (int i = 0; i < pendingChunkCount; ++i) {
								if (pendingChunks[i].data) free(pendingChunks[i].data);
							}
							break;
						}
						DebugLog.print("\n[Idle Sensor] Connected! IP: ");
						DebugLog.println(WiFi.localIP());

						// EXECUTE THE SECURE SYNCHRONIZATION PIPELINE — pure network
						// I/O from here, the batch was already read into RAM above.
						bool uploadSucceeded = uploadChunksToGoogleDrive(pendingChunks, pendingChunkCount, syncStartOffset == 0); // frees chunks internally

						if (!uploadSucceeded) {
							DebugLog.println("[Idle Sensor] Upload failed; will resume this backlog next sync.");
							break;
						}

						size_t newOffset = syncStartOffset + pendingBytesRead;
						if (pendingReachedEOF) {
							SD.remove(DATA_FILE_PATH); // fully synced — next row logged starts a fresh file
							clearSyncOffset();
							DebugLog.println("[Idle Sensor] Data file fully synced and cleared.");
							keepGoing = false;
						} else {
							writeSyncOffset(newOffset);
							DebugLog.printf("[Idle Sensor] Synced %u bytes so far this visit; more remains, continuing...\n", (unsigned)newOffset);
							// keepGoing stays true — loop straight into the next batch.
						}
					}
				}

				WiFi.disconnect(true);
				WiFi.mode(WIFI_OFF);
			}
		}
	} else {
		// Vehicle is drawing power or regen-braking! Reset tracking states instantly
		if (trackingZeroCurrent && !hasScannedThisIdleSession) {
			DebugLog.println(); // close off the "Vehicle Idle..." dot line
		}
		trackingZeroCurrent = false;
		if (hasScannedThisIdleSession) {
			WiFi.disconnect(true);
			WiFi.mode(WIFI_OFF);
			hasScannedThisIdleSession = false;
		}
	}

	// 3. Keep-Alive Diagnostics print block
	if (millis() - lastPrint >= 1000) {
		lastPrint = millis();
		if (isPackIdle()) {
			if (!hasScannedThisIdleSession) {
				Serial.print(".");
			}
		} else {
			Serial.println("Waiting for CAN data... [Bike Moving]");
		}
	}
}
