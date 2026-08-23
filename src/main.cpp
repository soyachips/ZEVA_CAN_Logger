#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <driver/twai.h>
#include <time.h>

#include <WiFi.h>
#include <WebServer.h>
#include <ElegantOTA.h>

// --- NETWORK CREDENTIAL MANAGEMENT ---
const char* ssid = "TestNet";
const char* password = "4985werd";

const char* home_ssid = "ABcb";
const char* home_password = "stomp-doghouse-splinter"; 

// --- TIMEZONE CONFIGURATION ---
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 10 * 3600;     // UTC +10 hours (AEST)
const int   daylightOffset_sec = 3600;     // 1 hour daylight savings shift

// --- PROXIMITY ENGINE TRACKING VARIABLES ---
static uint32_t zeroCurrentStartTime = 0;
static bool trackingZeroCurrent = false;
static bool hasScannedThisIdleSession = false;
static bool isSynchronizing = false;

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

    <!-- CHART 2: Sensitive 26-Cell Voltage Matrix Layer -->
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
static const int MAX_LOG_FILE_DAYS = 7;
static SPIClass sdSpi(VSPI);
static File logFile;
static char currentLogFileName[32] = "";
static bool sdInitialized = false;

static void getCurrentLogDate(char *dateString, size_t size) {
	time_t now = time(nullptr);
	if (now < 24 * 3600) {
		strncpy(dateString, "unknown", size);
		dateString[size - 1] = '\0';
		return;
	}
	struct tm timeinfo;
	localtime_r(&now, &timeinfo);
	strftime(dateString, size, "%Y%m%d", &timeinfo);
}

static void getLogFileName(char *filename, size_t size) {
	char dateString[16];
	getCurrentLogDate(dateString, sizeof(dateString));
	if (strcmp(dateString, "unknown") == 0) {
		snprintf(filename, size, "/log-session.csv");
	} else {
		snprintf(filename, size, "/log-%s.csv", dateString);
	}
}

static void purgeOldLogFiles() {
	if (!sdInitialized) return;

	File root = SD.open("/");
	if (!root) return;

	const int maxFiles = MAX_LOG_FILE_DAYS + 8;
	char logFiles[maxFiles][32];
	int fileCount = 0;

	while (true) {
		File entry = root.openNextFile();
		if (!entry) break;
		if (!entry.isDirectory()) {
			const char *name = entry.name();
			int len = strlen(name);
			if (len == 16 && strncmp(name, "log-", 4) == 0 && strcmp(name + 12, ".csv") == 0) {
				if (fileCount < maxFiles) {
					strcpy(logFiles[fileCount++], name);
				}
			}
		}
		entry.close();
	}
	root.close();

	if (fileCount <= MAX_LOG_FILE_DAYS) return;

	for (int i = 0; i < fileCount - 1; ++i) {
		for (int j = i + 1; j < fileCount; ++j) {
			if (strcmp(logFiles[i], logFiles[j]) > 0) {
				char temp[32];
				strcpy(temp, logFiles[i]);
				strcpy(logFiles[i], logFiles[j]);
				strcpy(logFiles[j], temp);
			}
		}
	}

	for (int i = 0; i < fileCount - MAX_LOG_FILE_DAYS; ++i) {
		char path[40];
		if (logFiles[i][0] == '/') {
			snprintf(path, sizeof(path), "%s", logFiles[i]);
		} else {
			snprintf(path, sizeof(path), "/%s", logFiles[i]);
		}
		SD.remove(path);
	}
}

static bool openLogFile() {
	if (!sdInitialized) return false;

	char filename[32];
	getLogFileName(filename, sizeof(filename));
	if (strcmp(filename, currentLogFileName) == 0 && logFile) {
		return true;
	}

	if (logFile) {
		logFile.close();
		currentLogFileName[0] = '\0';
	}

	logFile = SD.open(filename, FILE_APPEND);
	if (!logFile) {
		return false;
	}

	strncpy(currentLogFileName, filename, sizeof(currentLogFileName));
	currentLogFileName[sizeof(currentLogFileName) - 1] = '\0';
	return true;
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

static void writeLogHeader(File &file) {
	file.print("timestamp,packV,packI");
	for (int i = 1; i <= MAX_TOTAL_CELL_COUNT; ++i) {
		file.print(",cell");
		file.print(i);
	}
	file.println();
}

void printPackVoltages(uint32_t timestamp) {
	char buffer[2048];
	int len = snprintf(buffer, sizeof(buffer), "%lu", timestamp);
	appendPackVoltageValue(buffer, sizeof(buffer), len, packVoltage);
	appendPackCurrentValue(buffer, sizeof(buffer), len, packCurrent);

	for (int m = 0; m < MAX_BMS_MODULES; ++m) {
		uint8_t used = getModuleCellCount(m);
		for (int c = 0; c < used; ++c) {
			appendCellVoltageValue(buffer, sizeof(buffer), len, bmsModules[m].volts[c]);
		}
	}

	// Always output to the Serial Terminal for instant debugging
	Serial.println(buffer);

	// ONLY write to the SD card if current is non-zero (charging or discharging)
	if (sdInitialized && packCurrent != 0) {
		if (openLogFile()) {
			logFile.println(buffer);
			logFile.flush(); // Safely saves data to the flash sector
		} else {
			Serial.println("Failed to rotate/open log file.");
		}
	}
}

bool setupCAN() {
	const twai_timing_config_t timing_config = TWAI_TIMING_CONFIG_250KBITS();
	const twai_filter_config_t filter_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();
	const twai_general_config_t general_config = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_PIN, CAN_RX_PIN, TWAI_MODE_NORMAL);

	esp_err_t err = twai_driver_install(&general_config, &timing_config, &filter_config);
	if (err != ESP_OK) {
		Serial.printf("TWAI driver install failed: %d\n", err);
		return false;
	}

	err = twai_start();
	if (err != ESP_OK) {
		Serial.printf("TWAI start failed: %d\n", err);
		return false;
	}

	Serial.printf("CAN initialized at %u kbps\n", CAN_SPEED / 1000);
	return true;
}

void setup() {
	Serial.begin(115200);
	delay(1000);

	// 1. Set wireless station properties
	WiFi.mode(WIFI_STA);
	WiFi.begin(ssid, password);
	Serial.print("Searching for Hotspot (10s timeout)");
	
	int attemptCounter = 0;
	bool wifiConnected = true;

	// Loop 20 times with a 500ms delay = 10 seconds total tracking window
	while (WiFi.status() != WL_CONNECTED) {
		delay(500);
		Serial.print(".");
		attemptCounter++;

		if (attemptCounter >= 20) {
			wifiConnected = false;
			break; 
		}
	}
	
	// BRANCH A: ONLINE MODE (Hotspot Found!)
	if (wifiConnected) {
		Serial.println("\nConnected successfully!!! Network node established.");
		Serial.print("IP Address: ");
		Serial.println(WiFi.localIP());

		// 2. Request chosen static IP configuration allocation
		if (!WiFi.config(local_IP, gateway, subnet, primaryDNS)) {
			Serial.println("Static IP configuration failed to apply!");
		}

		// 3. Sync calendar time from internet NTP (Only safe when online!)
		Serial.println("Syncing calendar time from internet NTP...");
		configTime(gmtOffset_sec, daylightOffset_sec, "pool.ntp.org");
		
		struct tm timeinfo;
		if (getLocalTime(&timeinfo, 4000)) { 
			Serial.println("System time synchronized successfully!");
		} else {
			Serial.println("NTP Sync timed out! Falling back to 'unknown' naming.");
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
			
			int absoluteCellCounter = 0;
			for (int m = 0; m < MAX_BMS_MODULES; m++) {
				uint8_t cellsInThisModule = getModuleCellCount(m);
				for (int c = 0; c < cellsInThisModule; c++) {
					if (absoluteCellCounter < 26) {
						json += String(bmsModules[m].volts[c]);
						absoluteCellCounter++;
						if (absoluteCellCounter < 26) json += ",";
					}
				}
			}
			while (absoluteCellCounter < 26) {
				json += "0";
				absoluteCellCounter++;
				if (absoluteCellCounter < 26) json += ",";
			}
			json += "]}";
			server.send(200, "application/json", json);
		});

		// 5. Initialize the WebOTA Dashboard Interface and Start Server
		ElegantOTA.begin(&server);
		server.begin();
		Serial.println("Web Server initialized.");

	// BRANCH B: OFFLINE MODE (Hotspot Missing)
	} else {
		Serial.println("\nHotspot not found. Switching to Offline Logger Mode.");
		WiFi.disconnect(true); 
		WiFi.mode(WIFI_OFF); // Power off antenna arrays completely
	}

	// BOTH BRANCHES CONTINUOUSLY RUN INDEPENDENT HARDWARE CONFIGURATIONS BELOW HERE
	Serial.println("ZEVA ESP32 CAN cell voltage logger");
	Serial.println("Waiting for CAN data...");

	if (!setupCAN()) {
		Serial.println("CAN initialization failed. Check CAN wiring and transceiver.");
	}

	// Ensure CS is an output and driven HIGH before initializing SPI/SD.
	pinMode(SD_CS_PIN, OUTPUT);
	digitalWrite(SD_CS_PIN, HIGH);
	delay(10);

	sdSpi.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
	
	// Initialize SD File allocations using our auto-incrementing file counter mechanism
	if (!SD.begin(SD_CS_PIN, sdSpi)) {
		Serial.println("SD initialization failed. Check SD card wiring.");
	} else {
		sdInitialized = true;
		Serial.println("SD initialized.");
		purgeOldLogFiles();

		char dateString[16];
		getCurrentLogDate(dateString, sizeof(dateString));

		char filename[32];
		int sessionCounter = 1;

		while (true) {
			if (strcmp(dateString, "unknown") == 0) {
				snprintf(filename, sizeof(filename), "/log-session-%02d.csv", sessionCounter);
			} else {
				snprintf(filename, sizeof(filename), "/log-%s-%02d.csv", dateString, sessionCounter);
			}

			if (!SD.exists(filename)) {
				break; 
			}
			sessionCounter++;
		}

		logFile = SD.open(filename, FILE_WRITE);
		if (logFile) {
			strncpy(currentLogFileName, filename, sizeof(currentLogFileName));
			currentLogFileName[sizeof(currentLogFileName) - 1] = '\0';
			Serial.printf("Logging enabled: %s\n", currentLogFileName);
			writeLogHeader(logFile);
			logFile.flush();
		} else {
			Serial.println("Failed to open log file for writing.");
		}
	}

	memset(bmsModules, 0, sizeof(bmsModules));
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
			printPackVoltages(millis());
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

#include <HTTPClient.h>
#include <WiFiClientSecure.h>

void uploadLatestLogToGoogleDrive(const char* filepath) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[Uploader] Error: Wi-Fi disconnected.");
        return;
    }

    // 1. SELF-HEALING: RE-INITIALIZE THE SD CARD IF THE RADIO CRASHED IT
    Serial.println("[Uploader] Checking SD Card stability...");
    SD.end(); // Clear hanging driver handles
    delay(200);
    
    // Re-initialize the SPI / SD lines cleanly
    if (!SD.begin(SD_CS_PIN, sdSpi)) {
        Serial.println("[Uploader] CRITICAL: SD Card failed to wake up after WiFi boot! Retrying once...");
        delay(500);
        if (!SD.begin(SD_CS_PIN, sdSpi)) {
            Serial.println("[Uploader] CRITICAL: SD Card hardware unrecoverable.");
            return;
        }
    }
    Serial.println("[Uploader] SD Card communication restored.");

    // 2. PATH SANITIZATION & EMPTY STRING FALLBACK
    String correctPath = String(filepath);
    correctPath.trim();

    // If the tracking variable is empty, fall back to the standard session naming convention
    if (correctPath == "" || correctPath == "/") {
        Serial.println("[Uploader] Warning: Tracked filename was empty. Scanning for session fallback file...");
        
        // Scan for the most likely un-synchronized log file structure on your root card
        if (SD.exists("/log-session-01.csv")) {
            correctPath = "/log-session-01.csv";
        } else if (SD.exists("/log-session.csv")) {
            correctPath = "/log-session.csv";
        } else {
            Serial.println("[Uploader] Error: No valid session files found on SD card root directory.");
            return;
        }
    }

    if (!correctPath.startsWith("/")) {
        correctPath = "/" + correctPath;
    }

    File fileToStream = SD.open(correctPath.c_str(), FILE_READ);
    if (!fileToStream) {
        Serial.printf("[Uploader] Failed to read log file target path: %s\n", correctPath.c_str());
        return;
    }

    Serial.printf("[Uploader] Initiating upload for file: %s (%d bytes)\n", correctPath.c_str(), fileToStream.size());

    // Extract a clean name without the leading slash for the query parameter
    String cleanName = correctPath;
    if (cleanName.startsWith("/")) {
        cleanName = cleanName.substring(1);
    }

    // Create the parameter string block
    String serverPath = "/macros/s/AKfycbwiWTMQR1hKmJWM5A8PRbAPcyz4c5JaZ8fS9T36bLT9mcSXhCDs6Cv_to7G9T21VBScqA/exec?filename=" + cleanName;
    String fullUrl = "https://google.com" + serverPath;

    WiFiClientSecure client;
    client.setInsecure(); // Bypass strict Google certificate chains

    HTTPClient http;
    http.setTimeout(25000); // 25 seconds for slow cellular links
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS); 

    if (http.begin(client, fullUrl)) {
        http.addHeader("Content-Type", "text/csv");

        int httpCode = http.sendRequest("POST", &fileToStream, fileToStream.size());

        if (httpCode > 0) {
            String response = http.getString();
            Serial.printf("[Uploader] Server response code: %d\n", httpCode);
            Serial.println("[Uploader] Response: " + response);

            if (response.indexOf("UPLOAD_SUCCESS") != -1) {
                Serial.println("[Uploader] File successfully archived to Google Drive!");
            }
        } else {
            Serial.printf("[Uploader] Transport layer failed: %s\n", http.errorToString(httpCode).c_str());
        }
        http.end();
    } else {
        Serial.println("[Uploader] Failed to initialize connection handle.");
    }

    fileToStream.close();
}


void loop() {
	static uint32_t lastPrint = 0;

	if (WiFi.status() == WL_CONNECTED) {
		Serial.println("\n[Idle Sensor] Connected to Home Network!");

		// 1. ENGAGE SYNCHRONIZATION STATE OVERRIDE
		isSynchronizing = true; 

		// Disconnect cleanly and reconnect immediately without static config parameters
		// This strips old cellular data assignments and pulls a fresh DHCP lease
		WiFi.disconnect();
		delay(150); 
		WiFi.begin(home_ssid, home_password);
		
		// 2. BLOCK EXCLUSIVELY WHILE NEGOTIATING HANDSHAKE
		int dhcpTimeout = 0;
		while (WiFi.status() != WL_CONNECTED && dhcpTimeout < 30) {
			delay(500);
			Serial.print(".");
			dhcpTimeout++;
		}

		if (WiFi.status() == WL_CONNECTED) {
			Serial.print("\n[Idle Sensor] DHCP IP Successfully Assigned: ");
			Serial.println(WiFi.localIP());
			
			// 3. EXECUTE THE SECURE GOOGLE SYNC PIPELINE
			uploadLatestLogToGoogleDrive(currentLogFileName);
		} else {
			Serial.println("\n[Idle Sensor] DHCP Network handshake timed out.");
		}

		// 4. RESTORE VEHICLE INFRASTRUCTURE REGISTER TRACKING STATE
		WiFi.disconnect(true);
		WiFi.mode(WIFI_OFF);
		isSynchronizing = false; // Re-enable background loops safely
	}


	// 1. Continuous high-speed CAN packet processing
	twai_message_t message;
	if (twai_receive(&message, 0) == ESP_OK) {
		if (message.extd) {
			processBmsMessage(message.identifier, message.data, message.data_length_code);
		}
	}

	// 2. THE IDLE PROXIMITY ENGINE (Monitors home location arrival states)
	if (packCurrent == 0) {
		if (!trackingZeroCurrent) {
			zeroCurrentStartTime = millis();
			trackingZeroCurrent = true;
			hasScannedThisIdleSession = false; 
		}
		
		// If current stays at exactly 0.0A for 15 seconds straight, scan for garage network
		if (trackingZeroCurrent && (millis() - zeroCurrentStartTime >= 15000) && !hasScannedThisIdleSession) {
			hasScannedThisIdleSession = true; 
			
			Serial.println("\n[Idle Sensor] Vehicle stationary for 15s. Booting up Wi-Fi Radio Hardware...");
			
			// 1. Reactivate station mode cleanly without dropping credential buffers
			WiFi.mode(WIFI_STA);
			delay(400); 
			WiFi.disconnect(false); // Soft reset channels only, leaves parameters safe!
			delay(300); 

			Serial.println("[Idle Sensor] Scanning for Home Wi-Fi...");

			int networkCount = WiFi.scanNetworks();
			bool foundHome = false;
			
			for (int i = 0; i < networkCount; ++i) {
				if (WiFi.SSID(i) == String(home_ssid)) {
					foundHome = true;
					break;
				}
			}
			WiFi.scanDelete(); 
			
			if (foundHome) {
				Serial.println("[Idle Sensor] Home Wi-Fi within range! Resetting network stack...");
				if (logFile) { logFile.close(); } 
				
				// --- FORCE AN ABSOLUTE HARDWARE & STACK REBOOT OF THE RADIO ---
				WiFi.disconnect(true, true); // True #2 completely clears stored system credentials 
				WiFi.mode(WIFI_OFF);         // Shut down native stack drivers completely
				delay(200);                  // Let registers drain completely
				
				WiFi.mode(WIFI_STA);         // Spin up a brand new, empty network profile instance
				delay(200);
				// ---------------------------------------------------------------

				Serial.println("[Idle Sensor] Connecting to Home Router...");
				WiFi.begin(home_ssid, home_password);
				int connectAttempts = 0;

				while (WiFi.status() != WL_CONNECTED && connectAttempts < 20) {
					delay(500);
					Serial.print(".");
					connectAttempts++;
				}
				
				if (WiFi.status() == WL_CONNECTED) {
					Serial.println("\n[Idle Sensor] Connected to Home Network!");
					
					// 1. FORCE A NATIVE HANDSHAKE OVER DHCP
					// Disconnect cleanly and reconnect immediately without any config overrides.
					// This wipes the hardcoded static parameters and prompts the router for a fresh DHCP lease.
					WiFi.disconnect();
					delay(100); 
					WiFi.begin(home_ssid, home_password);
					
					// 2. AWAIT COMPLETED NETWORK HANDSHAKE
					int dhcpTimeout = 0;
					while (WiFi.status() != WL_CONNECTED && dhcpTimeout < 15) {
						delay(500);
						Serial.print(".");
						dhcpTimeout++;
					}

					if (WiFi.status() == WL_CONNECTED) {
						Serial.print("\n[Idle Sensor] DHCP IP Successfully Assigned: ");
						Serial.println(WiFi.localIP());
						
						// 3. EXECUTE THE SECURE SYNCHRONIZATION PIPELINE
						uploadLatestLogToGoogleDrive(currentLogFileName);
					} else {
						Serial.println("\n[Idle Sensor] DHCP Handshake timed out.");
						WiFi.disconnect(true);
						WiFi.mode(WIFI_OFF);
					}
				}

			} else {
				Serial.println("[Idle Sensor] Home network not spotted. Going dark.");
				WiFi.disconnect(true);
				WiFi.mode(WIFI_OFF); 
			}
		}
	} else {
		// Vehicle is drawing power or regen-braking! Reset tracking states instantly
		trackingZeroCurrent = false;
		if (hasScannedThisIdleSession) {
			WiFi.disconnect(true);
			WiFi.mode(WIFI_OFF);
			hasScannedThisIdleSession = false;
		}
	}

	// 3. Keep-Alive Diagnostics print block (Only run if not synchronizing data!)
	if (!isSynchronizing && (millis() - lastPrint >= 1000)) {
		lastPrint = millis();
		if (packCurrent == 0) {
			Serial.printf("Vehicle Idle... Proximity Scan Countdown: %lu/15s\n", (millis() - zeroCurrentStartTime) / 1000);
		} else {
			Serial.println("Waiting for CAN data... [Bike Moving]");
		}
	}
}
