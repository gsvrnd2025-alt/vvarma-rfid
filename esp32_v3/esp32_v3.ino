

/**
 * V-VARMA RFID ATTENDANCE SYSTEM - ENHANCED INTEGRATED VERSION (FIXED)
 * Hardware: ESP32 + RDM6300 (RFID) + I2C LCD (16x2) + LED (Pin2) + Buzzer
 * (Pin25)
 *
 * Features (All gaps closed):
 * - 3-minute boot delay with scrolling intro
 * - Strict startup validation (WiFi, Internet, Database) – errors stay on
 * screen
 * - Exact home screen layout [MODE] [WiFi] [DB] [NET] [TIME]
 * - Step-by-step LCD flow (Card → Validating → Result → Name → Status)
 * - Extended status parsing (Present, Early/Late, Slot Over, Inventory Not
 * Assigned)
 * - Single-scan lock (no repeated reads)
 * - Time sync only once; 00:00 if no internet
 * - No background polling
 * - Real-time dashboard sync
 * - Complete inventory capture flow
 * - Instant RFID detection (non-blocking, HardwareSerial)
 **/

#include <ArduinoJson.h> // Install via Library Manager
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <HardwareSerial.h>
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPUpdate.h>
#include <Wire.h>
#include <esp_task_wdt.h>
#include <time.h>

// ArduinoJson 6/7 Compatibility
#if ARDUINOJSON_VERSION_MAJOR >= 7
#define JSON_DOC_TYPE JsonDocument
#else
#define JSON_DOC_TYPE DynamicJsonDocument
#endif

// ==================== CONFIGURATION ====================
#define FIRMWARE_VERSION "v7.1.0-ELITE"
#define DEVICE_NAME "vvarma-rfid"
#define AP_SSID "VARMA RFID SYSTEM"
#define AP_PASS "GSVRF001"
#define BOOT_DELAY_MS 10000 // 10sec  intro as per audio request

// Intervals moved to const globals for logic consistency

// ==================== PINS ====================
#define BTN_STATS 26
#define BTN_MODE 15
#define LED_PIN 2    // RED LED
#define LED_GREEN 13 // GREEN LED
#define BUZZER_PIN 25
#define RFID_RX_PIN 16

// ==================== ENUMS ====================
enum SystemState {
  BOOT_INTRO,
  BOOT_WIFI_INIT,
  BOOT_IP_SHOW,
  BOOT_VALIDATE,
  BOOT_ERROR,
  SYSTEM_READY,
  SYSTEM_WIFI_CONFIG,
  SYSTEM_PROCESSING,
  SYSTEM_RESULT,
  SYSTEM_INFO_SHOW
};

enum UIState {
  UI_HOME,
  UI_CARD,
  UI_VALIDATING,
  UI_RESULT,
  UI_ERROR,
  UI_INFO_SHOW,
  UI_BLOCKING_RESULT
};

UIState uiState = UI_HOME;
unsigned long uiTimer = 0;
String uiMessage = "";

enum ResultStep {
  RES_NONE,
  RES_DENIED_LABEL,   // ⭐️ NEW: Show "DENIED" keyword first
  RES_DENIED_1,
  RES_DENIED_2,
  RES_DENIED_3,
  RES_NAME,
  RES_REG,
  RES_COLLEGE,
  RES_BATCH,
  RES_SLOT,
  RES_PUNCH_TIME,
  RES_STATUS_SUCCESS,
  RES_STATUS_DETAIL,
  RES_REASON_SHOW     // ⭐️ NEW: Show denial reason for 2s at end
};

ResultStep resultStep = RES_NONE;
unsigned long resultTimer = 0;

enum SyncStatus {
  SYNC_INITIAL,
  SYNC_OK,
  SYNC_WIFI_ERROR,
  SYNC_DATABASE_CONFIG_ERROR,
  SYNC_DATABASE_SYNC_ERROR
};

enum OperationMode { MODE_IN, MODE_OUT, MODE_INV, MODE_VERIFY };

enum ProcessStep {
  STEP_NONE,
  STEP_CARD_NUM,
  STEP_VALIDATING,
  STEP_RESULT,
  STEP_NAME,
  STEP_STATUS
};

// ==================== OBJECTS ====================
LiquidCrystal_I2C lcd(0x27, 16, 2);
Preferences preferences;
WebServer server(80);
DNSServer dnsServer;
HardwareSerial rfidSerial(2); // Use Serial2
// RFID_RX_PIN already defined above (Pin 16)

// ==================== GLOBALS ====================
volatile SystemState currentState = BOOT_INTRO;
OperationMode currentMode = MODE_IN;
ProcessStep currentStep = STEP_NONE;
volatile bool hasInternet = false;
volatile bool deviceVerified = false;
bool timeSynced = false;
String savedSSID = "", savedPass = "", scriptId = "";
String otaSource = "github", otaLink = "https://raw.githubusercontent.com/gsvrnd2025-alt/vvarma-rfid/master/version.json", otaId = "";
String deviceUser = "", devicePass = ""; // ⭐️ ADDED: Mandatory Auth
String localIpStr = "0.0.0.0", myMac = "";
volatile SyncStatus currentSyncStatus = SYNC_INITIAL;
unsigned long lastErrorBeepTime = 0;
bool initialSyncDone = false;
bool autoUpdateEnabled = true;

// OTA background task tracking
volatile int otaProgress = 0;
volatile bool otaRunning = false;
String otaStatusStr = "idle";
String otaErrorMsg = "";
String otaTargetVerVal = "";
TaskHandle_t otaTaskHandle = NULL;

// RFID Debounce & Scan Lock
const unsigned long RFID_DEBOUNCE_MS = 3000; // Ignore same card for 3s
const unsigned long SCAN_COOLDOWN_MS = 1500; // Min time between ANY scans
unsigned long lastSuccessfulScanTime = 0;
String lastScannedCard = "";
unsigned long lastScannedCardTime = 0;

unsigned long stateTimer = 0;
unsigned long lastLcdUpdate = 0;
unsigned long lastScrollTime = 0;
int scrollPosition = 0;

// RFID Buffer
char rfidBuffer[15];
int rfidIndex = 0;

// Web Log Infrastructure
String webSerialLog = "";
void webLog(String msg) {
  Serial.println(msg);
  webSerialLog += msg + "\n";
  if (webSerialLog.length() > 2000) {
    webSerialLog = webSerialLog.substring(1000); // Truncate old logs
  }
}
unsigned long rebootTimer = 0;
String lastCardNum = "---";
String lastUserName = "---";
String lastStatusMsg = "Scan Ready...";
String lastDetailedStatus = "";
String capturedUid = "--------";
String currentStatusText = "";
unsigned long lastRfidPacketTime = 0;
String lastRawUid = "";
bool cardInRange = false;

// ==================== LED BLINK ENGINE ====================
// LED modes:
//   IDLE        → RED steady (ready to scan) or RED steady (processing)
//   SUCCESS     → GREEN blinks fast for ~2s, then returns to IDLE (RED steady)
//   DENIED      → RED blinks 4 times fast, then returns to IDLE (RED steady)
enum LedMode {
  LED_MODE_IDLE,           // RED steady (scan ready / processing)
  LED_MODE_RESULT_SUCCESS, // GREEN blinks fast, RED off
  LED_MODE_RESULT_DENIED   // RED blinks 4x fast then IDLE
};
volatile LedMode currentLedMode = LED_MODE_IDLE;
unsigned long ledModeStartTime = 0;
const unsigned long LED_RESULT_DURATION_MS = 2000; // How long result blink lasts
unsigned long lastLedBlinkTime = 0;
bool ledBlinkState = false;
int deniedBlinkCount = 0;          // ⭐️ Count RED blinks for denied (max 4)
const int DENIED_BLINK_MAX = 4;    // ⭐️ 4 blinks then return to steady RED

// 🔧 ADDED MISSING GLOBALS
volatile bool cardProcessing = false;
volatile bool lastResponseSuccess = false; // ⭐️ Track cloud response outcome
volatile bool scanLocked = false;          // ⭐️ Master scan lock
String currentCardNum = "";
String currentName = "";
String currentRegId = ""; // ⭐️ ADDED: Registration ID
String currentRegNo = ""; // ⭐️ ADDED: Register Number (Roll Number)
String currentCollege = "";
String currentSlotTiming = "";
String currentBatch = "";
String currentPunchTime = "";
String currentDetailStatus = "";
String currentStartDate = ""; // ⭐️ Internship Start Date
String currentEndDate = "";   // ⭐️ Internship End Date
String currentPhotoUrl = "";
String lastRegId = "";
String lastRegNo = "";
String lastCollege = "";
String lastBatch = "";
String lastPhotoUrl = "";
int infoCycle = 0;        // 0: None, 1: MAC, 2: IP
unsigned long infoTimer = 0;
unsigned long lastScanTime = 0;
unsigned long lastSyncTime = 0; // For background heartbeat sync
const unsigned long HEARTBEAT_OK =
    900000; // 15 min regular heartbeat (Industrial Std)
const unsigned long HEARTBEAT_ERROR = 5000; // 5 sec recovery ping (Spec §11)
SemaphoreHandle_t lcdMutex;  // ⭐️ THREAD SAFETY: Mutex for LCD access
SemaphoreHandle_t syncMutex; // ⭐️ Priority lock for background sync

void lcdLock() {
  if (lcdMutex)
    xSemaphoreTake(lcdMutex, portMAX_DELAY);
}
void lcdUnlock() {
  if (lcdMutex)
    xSemaphoreGive(lcdMutex);
}
const unsigned long WDT_TIMEOUT = 1800000; // 30 mins system watchdog
unsigned long lastActivityTime = 0;        // For WDT Tracking
int syncFailureCount = 0; // Watchdog for repeated sync failures
const int MAX_SYNC_FAILURES =
    3; // Show error on LCD only after 3 failures (Industrial Stability)
bool serverStarted = false;
bool rfidAutomationMode = false; // ⭐️ ADDED: Persistence for Auto Assign
bool isDisplayingCommand = false;
unsigned long commandReceivedTime = 0;

// LCD Custom Icons
byte wifiIcon[8] = {0b00000, 0b01110, 0b10001, 0b00100,
                    0b01010, 0b00100, 0b00000, 0b00000};
byte netIcon[8] = {0b00000, 0b01110, 0b10001, 0b11111,
                   0b10101, 0b01110, 0b00100, 0b00000};
byte dbIcon[8] = {0b00000, 0b01110, 0b11111, 0b10011,
                  0b10101, 0b11001, 0b11111, 0b00000};
byte signal0[8] = {0b00000, 0b00000, 0b00000, 0b00000,
                   0b00000, 0b00000, 0b00000, 0b11111};
byte signal1[8] = {0b00000, 0b00000, 0b00000, 0b00000,
                   0b00000, 0b00000, 0b01110, 0b10001};
byte signal2[8] = {0b00000, 0b00000, 0b00000, 0b00000,
                   0b01110, 0b10001, 0b10001, 0b10001};
byte signal3[8] = {0b00000, 0b00000, 0b01110, 0b10001,
                   0b10001, 0b10001, 0b10001, 0b10001};
byte coffeeIcon[8] = {0b00000, 0b01010, 0b01010, 0b11111,
                      0b11111, 0b11111, 0b01110, 0b00000};

struct FirmwareDetails {
  bool success;
  String latestVersion;
  String downloadUrl;
  bool updateAvailable;
  String rawVersionsJson; // JSON array of versions
};

// ==================== PROTOTYPES ====================
FirmwareDetails fetchOnlineFirmwareDetails();
void startOtaUpdateTask(String url);
void otaTask(void *pvParameters);
void webLog(String msg);
void initializeLCD();
void mDNS_begin();
void setupWebServer();
void startAPMode();
void connectWiFi();
bool checkInternet();
void checkDB();
void factoryReset();
void updateUIEngine();
void scrollRow2(String text);
void setCustomIcons();
void beep(int freq, int duration);
void beepSuccess();
void beepDenied();
void triggerResultLed(bool success);
String formatCardNumber(String hexUid);
void processCard(String uid);
void processCardTask(void *parameter);
void handleBlockingResultDisplay();
void handleBlockingVrfDisplay();
void scrollRow2Blocking(String header, String text, unsigned long durationMs);
void backgroundSyncTask(void *pvParameters);
void parseGoogleResponse(String response);
void saveInventoryToSheet(String uid);
void handleRFID();
void handleButtons();
void beepInventorySuccess();
void initiateOtaCheckOnBoot();
void buttonWatchdogTask(void *pvParameters);

void updateLcdConnectionIcons();
void showBootError(String errorMsg);
void updateRTC();
String getRTC();
String getRTCDate();
void setRtcTime(String timeStr);


// Web Handlers
void serveAPLoginPage();
void serveConfigPage();
void handleDashboard();
void handleLogin();
void handleStatus();
void handleMode();
void handleCapture();
void handleSaveInventory();
void handleSaveNetwork();
void handleSaveSystem();
void handleFactoryReset();
void handleScanWiFi();
void handleLogs();
void handleInventory();
void handleValidate();
void handleResetDisplay();
void handleGetScriptId();
// checkBootFactoryReset and bootDelay removed (handled by buttonWatchdogTask)

// ==================== SETUP ====================
void setup() {
  // On ESP-IDF 5.x (Core 3.x), the TWDT is auto-initialized.
  // We simply disable it during setup to avoid spam during blocking boot.
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  esp_task_wdt_deinit(); // Disable during setup
#endif

  Serial.begin(115200);
  rfidSerial.begin(9600, SERIAL_8N1, 16, -1); // Force Pin 16 for RX
  rfidSerial.setRxBufferSize(256);

  pinMode(BTN_STATS, INPUT_PULLUP);
  pinMode(BTN_MODE, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  digitalWrite(LED_GREEN, LOW);

  // ⭐️ Initialize Mutex  // Threads & Locks
  lcdMutex = xSemaphoreCreateMutex();
  syncMutex = xSemaphoreCreateMutex();

  // Create Background Sync Task on Core 0 (Priority 1)
  xTaskCreatePinnedToCore(backgroundSyncTask, "SyncTask", 8192, NULL, 1, NULL,
                          0);

  // Create Button Watchdog Task for factory reset on Core 1 (Priority 10)
  xTaskCreatePinnedToCore(buttonWatchdogTask, "BtnWatchdog", 2048, NULL, 10, NULL, 1);

  Serial.println("System: Industrial Multi-Threading Initialized");

  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  lcdLock();
  setCustomIcons();
  lcdUnlock();

  preferences.begin("gsv-rfid", false);
  savedSSID = preferences.getString("ssid", "");
  savedPass = preferences.getString("pass", "");
  scriptId = preferences.getString("scriptId", "");
  if (scriptId == "") {
    scriptId = ""; // No default, force portal config
  }
  otaSource = preferences.getString("otaSource", "github");
  otaLink = preferences.getString("otaLink", "https://raw.githubusercontent.com/gsvrnd2025-alt/vvarma-rfid/master/version.json");
  otaId = preferences.getString("otaId", "");
  deviceUser = preferences.getString("devUser", "admin");
  devicePass = preferences.getString("devPass", "password123");
  autoUpdateEnabled = preferences.getBool("autoUpdate", true);
  preferences.end();

  Serial.println("Hardware: Admin Credentials Loaded:");
  Serial.println("  User: " + deviceUser);
  Serial.println("  Pass: " + String(devicePass.length()) + " chars");

  // Initialize Network mode to get MAC ID
  WiFi.mode(WIFI_STA);
  delay(100);
  myMac = WiFi.macAddress();
  myMac.toUpperCase();
  webLog("System Booting: MAC ID is " + myMac);

  // 1. INTRO SCREEN (5 seconds)
  lcdLock();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("  V-VARMA RFID  ");
  lcd.setCursor(0, 1);
  lcd.print("Attndnce Sys V3 ");
  lcdUnlock();
  delay(5000);

  // 2. CONFIGURATION VALIDATION (3 seconds)
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Checking Config ");
  lcd.setCursor(0, 1);
  lcd.print("Please wait...  ");
  feedWatchdog();
  delay(3000);
  feedWatchdog();

  // AP MODE CHECK (Only if credentials are wiped)
  if (savedSSID == "") {
    webLog("AP Mode required");
    startAPMode();
  } else {
    // Try to connect to WiFi (maximum 3 attempts / 60 seconds)
    int retryCount = 0;
    bool connected = false;
    while (retryCount < 3) {
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Connecting WiFi:");
      lcd.setCursor(0, 1);
      lcd.print(savedSSID.substring(0, 16));

      WiFi.mode(WIFI_AP_STA); // AP_STA mode so local dashboard hotspot remains live
      WiFi.softAP(AP_SSID, AP_PASS);
      WiFi.setHostname(DEVICE_NAME);
      WiFi.begin(savedSSID.c_str(), savedPass.c_str());
      int counter = 0;
      while (WiFi.status() != WL_CONNECTED &&
             counter < 40) { // 20 seconds per cycle
        delay(500);
        lcdLock();
        lcd.setCursor(15, 0);
        lcd.print(counter % 2 == 0 ? "." : " ");
        lcdUnlock();
        feedWatchdog(); // 🔥 FIX: Prevents WiFi connection reboot loops
        counter++;
      }

      if (WiFi.status() == WL_CONNECTED) {
        connected = true;
        break;
      }

      retryCount++;
      webLog("WiFi retry " + String(retryCount));
      lcd.setCursor(0, 1);
      lcd.print("Retry #" + String(retryCount) + "...    ");
      feedWatchdog();
      delay(3000);
    }

    if (connected) {
      WiFi.mode(WIFI_STA); // Turn off softAP, keep only STA
      hasInternet = true;
      localIpStr = WiFi.localIP().toString();

      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("WiFi Connected!");
      lcd.setCursor(0, 1);
      lcd.print("Signal: GOOD   ");
      delay(3000);

      // 1. IP Address Display (Static, 3 seconds)
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("IP Address:     ");
      lcd.setCursor(0, 1);
      lcd.print(localIpStr.substring(0, 16));
      delay(3000);

      // 2. MAC Address Display (Static, 3 seconds)
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("MAC Address:    ");
      lcd.setCursor(0, 1);
      lcd.print(myMac.substring(0, 16));
      delay(3000);

      // ⭐️ STABILIZED FLOW: Finalize Network identity
      WiFi.setHostname(DEVICE_NAME);
      delay(200);

      setupWebServer();
      mDNS_begin();

      webLog("Identity set");
      webLog("Dashboard OK");

      if (scriptId != "") {
        // Clear LCD completely and show connecting status for 3 seconds
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Connecting DB...");
        lcd.setCursor(0, 1);
        lcd.print("Please wait...  ");
        delay(3000);

        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Validating DB...");
        checkDB();

        // Database connection verification loop
        while (currentSyncStatus != SYNC_OK) {
          lcd.clear();
          lcd.setCursor(0, 0);
          lcd.print("DB Conn Failed! ");
          lcd.setCursor(0, 1);
          lcd.print("Retrying in 3s  ");
          for (int i = 0; i < 30; i++) {
            server.handleClient();
            delay(100);
            feedWatchdog();
          }
          lcd.clear();
          lcd.setCursor(0, 0);
          lcd.print("Validating DB...");
          checkDB();
          feedWatchdog();
        }

        initialSyncDone = true; // ✅ ONLY ONCE
        delay(1000);
      } else {
        webLog("Script ID missing");
        currentSyncStatus = SYNC_DATABASE_CONFIG_ERROR;
        deviceVerified = false;
      }

      // Flush and drain any boot-up serial noise
      while (rfidSerial.available()) {
        rfidSerial.read();
      }
      delay(100);
      while (rfidSerial.available()) {
        rfidSerial.read();
      }

      currentState = SYSTEM_READY;
      webLog("System active");

      // ⭐️ STARTUP BEEP: System ready and home screen starting
      beep(1200, 200);
      delay(100);
      beep(1800, 300);

      initiateOtaCheckOnBoot();

      return; // Success! Exit setup
    } else {
      // WiFi failed to connect after 3 attempts -> Activate AP mode!
      webLog("AP Mode activated");
      startAPMode();
    }
  }
}

void loop() {
  // WiFi Connection instant responder
  static bool wasConnected = (WiFi.status() == WL_CONNECTED);
  bool isConnected = (WiFi.status() == WL_CONNECTED);
  if (isConnected && !wasConnected) {
    wasConnected = true;
    localIpStr = WiFi.localIP().toString();
    setupWebServer();
    mDNS_begin();
    webLog("WiFi: Connected/Reconnected. Instantly starting mDNS & WebServer.");
  } else if (!isConnected) {
    wasConnected = false;
  }

  handleRFID(); // 🔥 PRIORITY 1: Instant Detection

  // Local Sync / Reboot Logic
  if (rebootTimer > 0 && millis() > rebootTimer) {
    ESP.restart();
  }

  // UI Engine runs on high frequency (Core 1)
  static unsigned long lastUIRun = 0;
  if (!isDisplayingCommand && (millis() - lastUIRun > 100)) {
    updateUIEngine();
    lastUIRun = millis();
  }

  // Essential non-blocking background tasks
  server.handleClient();

  feedWatchdog();

  // Mode/Info Buttons - only when not processing
  if (!cardProcessing && !scanLocked) {
    handleButtons();
  }

  // ⭐️ FIX: Restore command display timeout (15 sec)
  if (isDisplayingCommand && (millis() - commandReceivedTime > 15000)) {
    isDisplayingCommand = false;
    lcd.clear();
  }

  // ⭐️ SAFETY: Stuck-state watchdog
  // If scanLocked is true for > 25s, force unlock
  static unsigned long stuckCheckTime = 0;
  if (scanLocked) {
    if (stuckCheckTime == 0) {
      stuckCheckTime = millis();
    } else if (millis() - stuckCheckTime > 25000) {
      webLog("SAFETY: Force-unlocking stuck scan lock!");
      scanLocked = false;
      cardProcessing = false;
      uiState = UI_HOME;
      currentStep = STEP_NONE;
      stuckCheckTime = 0;
    }
  } else {
    stuckCheckTime = 0;
  }

  // Auto-timeout for info screens (10 seconds)
  if (currentState == SYSTEM_INFO_SHOW && (millis() - infoTimer > 10000)) {
    currentState = SYSTEM_READY;
    uiState = UI_HOME;
    infoCycle = 0;
    lcd.clear();
  }

  // Periodic State Debug Print
  static unsigned long lastDebugPrint = 0;
  if (millis() - lastDebugPrint > 2000) {
    Serial.printf("STATE DEBUG: scanLocked=%d, cardProcessing=%d, currentSyncStatus=%d, currentState=%d, wifiStatus=%d\n",
                  scanLocked, cardProcessing, currentSyncStatus, currentState, WiFi.status());
    lastDebugPrint = millis();
  }
}

// 🌐 BACKGROUND SYNC TASK (Core 0)
// This task handles all blocking network calls so RFID detection stays instant.
void backgroundSyncTask(void *pvParameters) {
  for (;;) {
    unsigned long now = millis();

    // ⭐️ SYSTEM READY GATE: Do not run background sync tasks during boot setup
    if (currentState != SYSTEM_READY && currentState != SYSTEM_INFO_SHOW) {
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    // ⭐️ HARD BLOCK: Never do ANY network work during card processing
    if (cardProcessing || scanLocked) {
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    // ⭐️ PRIORITY LOCK: Only sync if idle on Home/Error screen
    if (isDisplayingCommand || (uiState != UI_HOME && uiState != UI_ERROR)) {
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    // ── WiFi MONITORING & RECOVERY ──
    static wl_status_t lastWiFiStatus = WL_IDLE_STATUS;
    static unsigned long disconnectStartTime = 0;
    wl_status_t currentWiFiStatus = WiFi.status();

    if (currentWiFiStatus == WL_CONNECTED) {
      disconnectStartTime = 0; // Reset disconnect timer
      if (lastWiFiStatus != WL_CONNECTED) {
        webLog("WiFi: Reconnected! Re-starting mDNS.");
        WiFi.mode(WIFI_STA); // Turn off softAP, keep only STA
        localIpStr = WiFi.localIP().toString();
        WiFi.setHostname(DEVICE_NAME);
        vTaskDelay(pdMS_TO_TICKS(500)); // Let stack stabilize
        mDNS_begin();
      }
    } else {
      // WiFi Disconnected - Background Reconnect
      if (disconnectStartTime == 0) {
        disconnectStartTime = now;
      }
      
      static unsigned long lastReconnectAttempt = 0;
      if (now - lastReconnectAttempt > 10000 && savedSSID != "") {
        WiFi.reconnect();
        lastReconnectAttempt = now;
        webLog("WiFi: Lost connection. Retrying...");
      }

      // If disconnected for more than 30 seconds, activate AP mode!
      if (now - disconnectStartTime > 30000 && currentState != SYSTEM_WIFI_CONFIG) {
        webLog("WiFi: Lost connection for >30s. Activating AP Mode.");
        startAPMode();
      }

      lastWiFiStatus = currentWiFiStatus;
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue; // Skip all sync if WiFi is down
    }
    lastWiFiStatus = currentWiFiStatus;

    // ── ADAPTIVE SYNC: only sync in error mode (no heartbeat when OK) ──
    if (currentSyncStatus != SYNC_OK) {
      if (now - lastSyncTime > HEARTBEAT_ERROR || lastSyncTime == 0) {
        // Re-check: still safe to sync?
        if (!cardProcessing && !scanLocked) {
          xSemaphoreTake(syncMutex, portMAX_DELAY);
          if (WiFi.status() == WL_CONNECTED && scriptId != "") {
            // Internet check first (quick)
            hasInternet = checkInternet();

            if (hasInternet) {
              checkDB();
            } else {
              currentSyncStatus = SYNC_WIFI_ERROR;
            }
          }
          lastSyncTime = millis();
          xSemaphoreGive(syncMutex);
        }
      }
    }

    // ── PERIODIC INTERNET CHECK (separate from sync) ──
    static unsigned long lastNetCheck = 0;
    static int netFailCount = 0;
    unsigned long netCheckInterval =
        (currentSyncStatus == SYNC_OK) ? 30000 : 8000;
    if (now - lastNetCheck > netCheckInterval) {
      if (!cardProcessing && !scanLocked) {
        if (WiFi.status() == WL_CONNECTED) {
          bool netOk = checkInternet();
          if (netOk) {
            hasInternet = true;
            netFailCount = 0;
          } else {
            netFailCount++;
            if (netFailCount >= 3) {
              hasInternet = false;
            }
          }
        } else {
          hasInternet = false;
          netFailCount = 3;
        }
        lastNetCheck = now;
      }
    }

    // ── 30-MINUTE ONLINE HEARTBEAT ──
    static unsigned long lastOnlinePing = 0;
    if (now - lastOnlinePing > 1800000 ||
        lastOnlinePing == 0) { // 1800000 ms = 30 minutes
      if (!cardProcessing && !scanLocked && WiFi.status() == WL_CONNECTED &&
          hasInternet) {
        xSemaphoreTake(syncMutex, portMAX_DELAY);
        checkDB();
        lastOnlinePing = millis();
        xSemaphoreGive(syncMutex);
      } else if (lastOnlinePing == 0) {
        // Prevent immediate spamming if booting up without wifi
        lastOnlinePing = millis();
      }
    }

    // DNS Maintenance (Only in Config Mode)
    if (currentState == SYSTEM_WIFI_CONFIG) {
      dnsServer.processNextRequest();
    }

    vTaskDelay(pdMS_TO_TICKS(200)); // Keep task yielding
  }
}

void feedWatchdog() {
  // On ESP-IDF 5.x, we don't use TWDT for the main loop.
  // Our stuck-state watchdog in loop() handles lockup detection.
  // Just yield to prevent soft WDT triggers.
  yield();
}

void sendOnlineStatus() {
  if (scriptId == "")
    return;
  HTTPClient http;
  String url = "https://script.google.com/macros/s/" + scriptId +
               "/exec?action=online&mac=" + myMac;
  http.begin(url);
  http.GET();
  http.end();
  webLog("System: Executed 30 min online status ping");
}

// ==================== LCD FUNCTIONS ====================
void setCustomIcons() {
  lcd.createChar(0, wifiIcon);
  lcd.createChar(1, netIcon);
  lcd.createChar(2, dbIcon);
  lcd.createChar(3, signal0);
  lcd.createChar(4, signal1);
  lcd.createChar(5, signal2);
  lcd.createChar(6, signal3);
  lcd.createChar(7, coffeeIcon);
}

String getRTC() {
  if (!hasInternet)
    return "00:00:00";
  if (!timeSynced) {
    configTime(19800, 0, "pool.ntp.org", "time.google.com");
    timeSynced = true;
    return "00:00";
  }
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 100))
    return "00:00";
  char t[6];
  strftime(t, sizeof(t), "%H:%M", &timeinfo);
  return String(t);
}

String getRTCDate() {
  if (!hasInternet)
    return "00/00";
  if (!timeSynced) {
    configTime(19800, 0, "pool.ntp.org", "time.google.com");
    timeSynced = true;
    return "00/00";
  }
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 100))
    return "00/00";
  char d[6];
  strftime(d, sizeof(d), "%d/%m", &timeinfo);
  return String(d);
}

String cleanLCD(String s) {
  String res = "";
  for (int i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c >= 32 && c <= 126)
      res += c; // Only printable ASCII
    else
      res += ' ';
  }
  return res;
}

void updateLCDRow1() {
  lcd.setCursor(0, 0);
  if (currentMode == MODE_VERIFY)
    lcd.print("VRF");
  else if (currentMode == MODE_INV)
    lcd.print("INV");
  else if (currentMode == MODE_IN)
    lcd.print("IN ");
  else
    lcd.print("OUT");

  lcd.print("  "); // 2 spaces

  // Icons: ☕ (Wifi), 🌐 (Net), 📦 (DB)
  if (WiFi.status() == WL_CONNECTED)
    lcd.write((uint8_t)7);
  else
    lcd.print("X");
  if (hasInternet)
    lcd.write((uint8_t)1);
  else
    lcd.print("X");
  if (currentSyncStatus == SYNC_OK)
    lcd.write((uint8_t)2);
  else
    lcd.print("X");

  lcd.print("   "); // Clear indices 8, 9, 10
  lcd.setCursor(11, 0);
  lcd.print(getRTC());
}

void scrollRow2(String text) {
  static unsigned long lastScroll = 0;
  static int index = 0;
  static String prev = "";

  if (text != prev) {
    index = 0;
    prev = text;
    lastScroll = 0; // Force immediate update on text change
  }

  // If text fits in 16 chars, just display it statically
  if (text.length() <= 16) {
    if (millis() - lastScroll > 500) { // Refresh rate for static text
      lastScroll = millis();
      String padded = text;
      while (padded.length() < 16)
        padded += " ";
      lcdLock();
      lcd.setCursor(0, 1);
      lcd.print(padded);
      lcdUnlock();
    }
    return;
  }

  // Scrolling for long text
  if (millis() - lastScroll > 300) {
    lastScroll = millis();

    String padded = text + "    ";
    int maxIndex = padded.length() - 16;
    if (maxIndex < 0)
      maxIndex = 0;

    String view = padded.substring(index, index + 16);

    lcdLock();
    lcd.setCursor(0, 1);
    lcd.print(view);
    lcdUnlock();

    index++;
    if (index > maxIndex)
      index = 0;
  }
}

void updateUIEngine() {
  if (!lcdMutex)
    return;

  if (otaRunning) {
    return; // Let the OTA task handle the LCD display completely
  }

  // ==================== NON-BLOCKING LED BLINK ENGINE ====================
  unsigned long nowLed = millis();

  if (currentLedMode == LED_MODE_RESULT_SUCCESS) {
    // ⭐️ GREEN blinks fast every 150ms for LED_RESULT_DURATION_MS, then back to IDLE (steady GREEN)
    if (nowLed - ledModeStartTime > LED_RESULT_DURATION_MS) {
      currentLedMode = LED_MODE_IDLE;
      digitalWrite(LED_GREEN, HIGH); // steady GREEN
      digitalWrite(LED_PIN, LOW);   // RED off
      lastLedBlinkTime = nowLed;
    } else if (nowLed - lastLedBlinkTime >= 150) {
      lastLedBlinkTime = nowLed;
      ledBlinkState = !ledBlinkState;
      digitalWrite(LED_GREEN, ledBlinkState ? HIGH : LOW);
      digitalWrite(LED_PIN, LOW); // RED off during success blink
    }
  } else if (currentLedMode == LED_MODE_RESULT_DENIED) {
    // ⭐️ RED blinks exactly 4 times (200ms on / 200ms off), then back to steady GREEN
    if (nowLed - lastLedBlinkTime >= 200) {
      lastLedBlinkTime = nowLed;
      ledBlinkState = !ledBlinkState;
      if (ledBlinkState) {
        digitalWrite(LED_PIN, HIGH);
        digitalWrite(LED_GREEN, LOW);
      } else {
        digitalWrite(LED_PIN, LOW);
        // Count falling edges as completed blinks
        deniedBlinkCount++;
        if (deniedBlinkCount >= DENIED_BLINK_MAX) {
          currentLedMode = LED_MODE_IDLE;
          deniedBlinkCount = 0;
          ledBlinkState = false;
          digitalWrite(LED_GREEN, HIGH); // steady GREEN again
          digitalWrite(LED_PIN, LOW);   // RED off
        }
      }
    }
  } else {
    // IDLE mode:
    // If card is processing or scan is locked, only RED light is ON (stable RED, GREEN goes off).
    // Else, GREEN is ON (stable GREEN, ready to scan).
    if (cardProcessing || scanLocked) {
      digitalWrite(LED_PIN, HIGH);
      digitalWrite(LED_GREEN, LOW);
    } else {
      digitalWrite(LED_GREEN, HIGH);
      digitalWrite(LED_PIN, LOW);
    }
  }

  // --- BOOT SCREENS (can use clear) ---
  if (currentState == BOOT_INTRO || currentState == BOOT_WIFI_INIT ||
      currentState == BOOT_IP_SHOW || currentState == SYSTEM_WIFI_CONFIG ||
      currentState == BOOT_ERROR) {
    static SystemState lastBootState = BOOT_INTRO;
    lcdLock();
    if (currentState != lastBootState) {
      lcd.clear();
      lastBootState = currentState;
    }
    if (currentState == BOOT_INTRO) {
      lcd.setCursor(0, 0);
      lcd.print("  V-VARMA RFID  ");
      lcdUnlock();
      scrollRow2("Attndnce Sys V3 ");
      return;
    }
    if (currentState == BOOT_WIFI_INIT) {
      lcd.setCursor(0, 0);
      lcd.print("WiFi Connecting ");
      lcd.setCursor(0, 1);
      lcd.print(savedSSID.substring(0, 16));
      lcdUnlock();
      return;
    }
    if (currentState == BOOT_IP_SHOW) {
      lcd.setCursor(0, 0);
      lcd.print("Assigned IP     ");
      lcd.setCursor(0, 1);
      lcd.print(localIpStr + "      ");
      lcdUnlock();
      return;
    }
    if (currentState == SYSTEM_WIFI_CONFIG) {
      lcd.setCursor(0, 0);
      lcd.print("Connect to WIFI ");
      lcdUnlock();
      scrollRow2(String(AP_SSID) + " ");
    }
    return;
  }

  // --- MAIN ENGINE (NON-BLOCKING, NO CLEAR) ---
  if (uiState != UI_RESULT && uiState != UI_BLOCKING_RESULT) {
    lcdLock();
    updateLCDRow1();
    lcdUnlock();
  }

  switch (uiState) {
  case UI_HOME:
    if (!hasInternet)
      scrollRow2(">>> INTERNET DISCONNECTED <<< ");
    else if (currentSyncStatus == SYNC_WIFI_ERROR)
      scrollRow2(">>> WIFI SYNC ERROR <<< ");
    else if (currentSyncStatus == SYNC_DATABASE_CONFIG_ERROR)
      scrollRow2(">>> DATABASE CONFIG ERROR <<< ");
    else if (currentSyncStatus == SYNC_DATABASE_SYNC_ERROR)
      scrollRow2(">>> DATABASE ERROR <<< ");
    else
      scrollRow2(">>> Ready to Scan <<<");
    break;

  case UI_CARD:
    lcdLock();
    lcd.setCursor(0, 1);
    {
      String msg = "Card: " + currentCardNum;
      while (msg.length() < 16)
        msg += " ";
      lcd.print(msg);
    }
    lcdUnlock();

    if (millis() - uiTimer > 800) { // Reduced delay for snappier feel
      uiState = UI_VALIDATING;
      uiTimer = millis();
    }
    break;

  case UI_VALIDATING:
    scrollRow2(">>> Validating Please Wait <<<");
    break;

  case UI_RESULT: {
    String displayMsg = "";
    unsigned long elapsed = millis() - resultTimer;

    // ── INVENTORY MODE: do not cycle student info, just hold success/fail screen ──
    if (currentMode == MODE_INV) {
      if (lastResponseSuccess) {
        lcdLock();
        lcd.setCursor(0, 0);
        lcd.print("CARD ADDED OK   ");
        lcd.setCursor(0, 1);
        lcd.print("Added to Pool   ");
        lcdUnlock();
      } else {
        lcdLock();
        lcd.setCursor(0, 0);
        lcd.print("CARD ADDED FAIL ");
        lcd.setCursor(0, 1);
        String err = lastStatusMsg;
        if (err == "") err = "Already Exists";
        if (err.length() > 16) err = err.substring(0, 16);
        while (err.length() < 16) err += " ";
        lcd.print(err);
        lcdUnlock();
      }
      if (elapsed > 2000) {
        uiState = UI_HOME;
        resultStep = RES_NONE;
      }
      break;
    }

    // ── VRF MODE: separate short flow ──
    if (currentMode == MODE_VERIFY) {
      if (resultStep == RES_DENIED_LABEL || resultStep == RES_DENIED_1 || resultStep == RES_DENIED_2 || resultStep == RES_DENIED_3) {
        if (resultStep == RES_DENIED_1) {
          displayMsg = lastUserName;
          if (elapsed > 2000) { resultStep = RES_DENIED_2; resultTimer = millis(); }
        } else if (resultStep == RES_DENIED_2) {
          displayMsg = lastDetailedStatus;
          if (elapsed > 2000) {
            if (lastDetailedStatus == "Card Available") { resultStep = RES_DENIED_3; resultTimer = millis(); }
            else { uiState = UI_HOME; resultStep = RES_NONE; }
          }
        } else if (resultStep == RES_DENIED_3) {
          displayMsg = lastStatusMsg;
          if (elapsed > 2000) { uiState = UI_HOME; resultStep = RES_NONE; }
        }
      } else {
        if (resultStep == RES_NAME) {
          displayMsg = currentName;
          if (elapsed > 2000) { resultStep = RES_REG; resultTimer = millis(); }
        } else if (resultStep == RES_REG) {
          displayMsg = currentRegId;
          if (elapsed > 2000) { resultStep = RES_COLLEGE; resultTimer = millis(); }
        } else if (resultStep == RES_COLLEGE) {
          displayMsg = currentCollege;
          if (elapsed > 3000) { resultStep = RES_BATCH; resultTimer = millis(); }
        } else if (resultStep == RES_BATCH) {
          displayMsg = "Batch: " + currentBatch;
          if (elapsed > 2000) { resultStep = RES_SLOT; resultTimer = millis(); }
        } else if (resultStep == RES_SLOT) {
          displayMsg = "Start: " + currentSlotTiming;
          if (elapsed > 2000) { uiState = UI_HOME; resultStep = RES_NONE; }
        }
      }

    // ── STANDARD ATTENDANCE MODE ──
    } else {

      // ── DENIED path with NO student info (unregistered card, system error) ──
      if (resultStep == RES_DENIED_LABEL) {
        // ⭐️ Step 0: Show "DENIED" keyword first (2s)
        displayMsg = ">> DENIED <<";
        if (elapsed > 2000) {
          resultStep = RES_DENIED_1;
          resultTimer = millis();
        }

      } else if (resultStep == RES_DENIED_1) {
        // Step 1: Show status/reason message (3s for reason)
        displayMsg = lastStatusMsg.length() > 0 ? lastStatusMsg : "Access Denied";
        if (elapsed > 3000) {
          resultStep = RES_DENIED_2;
          resultTimer = millis();
        }

      } else if (resultStep == RES_DENIED_2) {
        // Step 2: Show detailed message (2s)
        displayMsg = lastDetailedStatus.length() > 0 ? lastDetailedStatus : "Card Not Found";
        if (elapsed > 2000) {
          uiState = UI_HOME;
          resultStep = RES_NONE;
        }

      // ── SUCCESS or DENIED-with-student-info: Full sequence ──
      // ⭐️ FULL SEQUENCE:
      // DENIED/SUCCESS label (2s) → Reason (3s) → Name (3s) → RegID (2s) 
      // → College (2s) → Batch (2s) → Slot Time (2s) → Scan Time (2s) → UI_HOME
      } else if (resultStep == RES_STATUS_SUCCESS) {
        // ⭐️ Show SUCCESS or DENIED keyword first in this path (2s)
        displayMsg = lastResponseSuccess ? ">> SUCCESS <<" : ">> DENIED <<";
        if (elapsed > 2000) {
          resultStep = RES_STATUS_DETAIL; // Go to Reason next
          resultTimer = millis();
        }

      } else if (resultStep == RES_STATUS_DETAIL) {
        // ⭐️ Show the denial/success reason - same as dashboard (3s for reason)
        String reason = lastResponseSuccess ? currentDetailStatus : lastStatusMsg;
        displayMsg = reason.length() > 0 ? reason : (lastResponseSuccess ? "Marked Present" : "Access Denied");
        if (elapsed > 3000) {
          resultStep = RES_NAME; // Go to Student Name next
          resultTimer = millis();
        }

      } else if (resultStep == RES_NAME) {
        // Show student name (2s for name)
        displayMsg = currentName;
        if (elapsed > 2000) {
          resultStep = RES_REG;
          resultTimer = millis();
        }

      } else if (resultStep == RES_REG) {
        // Show Registration ID (2s)
        displayMsg = currentRegId;
        if (elapsed > 2000) {
          resultStep = RES_COLLEGE;
          resultTimer = millis();
        }

      } else if (resultStep == RES_COLLEGE) {
        // Show College (2s)
        displayMsg = currentCollege;
        if (elapsed > 2000) {
          resultStep = RES_BATCH;
          resultTimer = millis();
        }

      } else if (resultStep == RES_BATCH) {
        // Show Batch (2s)
        displayMsg = "Batch: " + currentBatch;
        if (elapsed > 2000) {
          resultStep = RES_SLOT;
          resultTimer = millis();
        }

      } else if (resultStep == RES_SLOT) {
        // Show Slot (2s)
        displayMsg = "Slot: " + currentSlotTiming;
        if (elapsed > 2000) {
          resultStep = RES_PUNCH_TIME;
          resultTimer = millis();
        }

      } else if (resultStep == RES_PUNCH_TIME) {
        // Show Punch Time (2s)
        String scanTime = currentPunchTime.length() > 0 ? currentPunchTime : getRTC();
        displayMsg = "Time: " + scanTime;
        if (elapsed > 2000) {
          uiState = UI_HOME;
          resultStep = RES_NONE;
        }
      }
    }

    scrollRow2(displayMsg);
    break;
  }

  case UI_ERROR: {
    scrollRow2(">>> DATABASE ERROR <<<");
    // Recovery is now managed by adaptive heartbeats in systemControlBlock
    break;
  }

  case UI_INFO_SHOW:
    scrollRow2((infoCycle == 1 ? ("IP: " + localIpStr) : ("MAC: " + myMac)) +
               "   ");
    break;

  case UI_BLOCKING_RESULT:
    // Blocking display handled synchronously in task
    break;

  default:
    break;
  }
}

void showBootError(String errorMsg) {
  currentState = BOOT_ERROR;
  lastStatusMsg = errorMsg;
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("BOOT FAILURE");
  lcd.setCursor(0, 1);
  lcd.print(errorMsg);
  // Start AP mode anyway so user can reconfigure
  startAPMode();
}

bool isValidHex(String s) {
  if (s.length() < 10)
    return false;
  for (int i = 0; i < s.length(); i++) {
    char c = toupper(s[i]);
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F')))
      return false;
  }
  return true;
}

String formatCardNumber(String hexUid) {
  if (hexUid.length() < 6)
    return "0000000000";
  String hex6 = hexUid.substring(hexUid.length() - 6);
  long val = strtol(hex6.c_str(), NULL, 16);
  char buf[11];
  sprintf(buf, "%010ld", val);
  return String(buf);
}

// ==================== RFID HANDLING ====================
void handleRFID() {
  if (otaRunning) {
    while (rfidSerial.available())
      rfidSerial.read(); // drain
    return;
  }
  unsigned long now = millis();

  // ── DRAIN BUFFER when locked (always read to prevent overflow) ──
  if (scanLocked || cardProcessing) {
    static unsigned long lastBusyBeep = 0;
    bool cardDetectedWhileBusy = false;
    while (rfidSerial.available()) {
      byte b = rfidSerial.read();
      if (b == 0x03 && rfidIndex >= 12)
        cardDetectedWhileBusy = true;
      if (b == 0x02)
        rfidIndex = 0;
    }
    if (cardDetectedWhileBusy && (now - lastBusyBeep > 2000)) {
      beep(300, 100);
      webLog("RFID: Scan locked - rejected");
      lastBusyBeep = now;
    }
    return;
  }

  // ── STATE GATE: Only allow scanning when system is booted and NO DB ERROR ──
  if (currentState < SYSTEM_READY || currentSyncStatus != SYNC_OK) {
    while (rfidSerial.available())
      rfidSerial.read(); // drain
    return;
  }

  // ── COOLDOWN: Minimum gap between any two scans ──
  if (now - lastSuccessfulScanTime < SCAN_COOLDOWN_MS) {
    while (rfidSerial.available())
      rfidSerial.read(); // drain
    return;
  }

  while (rfidSerial.available()) {
    byte b = rfidSerial.read();
    Serial.printf("RFID Byte: 0x%02X (%c)\n", b, isPrintable(b) ? (char)b : '.');

    if (b == 0x02) {
      Serial.println("RFID: STX received");
      rfidIndex = 0;
      memset(rfidBuffer, 0, sizeof(rfidBuffer));
    } else if (b == 0x03) {
      if (rfidIndex >= 12) {
        rfidBuffer[rfidIndex] = '\0';
        String rawUid = String(rfidBuffer);

        if (isValidHex(rawUid)) {
          String hexId = rawUid.substring(0, 10);
          String cardNum = formatCardNumber(hexId);

          if (cardNum != "0000000000") {
            // ── DEBOUNCE: Same card within window → ignore ──
            if (cardNum == lastScannedCard &&
                (now - lastScannedCardTime < RFID_DEBOUNCE_MS)) {
              // Same card still on reader - silently ignore
              rfidIndex = 0;
              continue;
            }

            Serial.println("RDM6300 Valid: " + cardNum);
            lastScannedCard = cardNum;
            lastScannedCardTime = now;

            // ⭐️ LOCK immediately before any processing
            scanLocked = true;
            cardProcessing = true;

            // ⭐️ INSTANT FEEDBACK
            beep(1500, 150);
            uiState = UI_CARD;
            uiTimer = millis();
            currentCardNum = cardNum;

            processCard(cardNum);
          } else {
            webLog("RDM6300: Ignored null card read (noise).");
          }
        } else {
          webLog("RDM6300: Invalid hex packet ignored.");
        }
      }
      rfidIndex = 0;
    } else if (rfidIndex < 14 && isPrintable(b)) {
      rfidBuffer[rfidIndex++] = (char)b;
    }
  }
}

void processCardTask(void *parameter);

void processCard(String uid) {
  // Lock already set by handleRFID — don't re-check cardProcessing here

  // ⭐️ CRITICAL: Clear ALL stale state from previous scan
  lastResponseSuccess = false;
  lastUserName = "---";
  lastStatusMsg = "Scan Ready...";
  lastDetailedStatus = "";
  currentName = "";
  currentRegId = "";
  currentCollege = "";
  currentSlotTiming = "";
  currentBatch = "";
  currentPunchTime = "";
  currentDetailStatus = "";
  currentStartDate = "";
  currentEndDate = "";
  currentRegNo = "";
  currentPhotoUrl = "";
  lastRegId = "";
  lastRegNo = "";
  lastCollege = "";
  lastBatch = "";
  lastPhotoUrl = "";
  currentStep = STEP_CARD_NUM;

  capturedUid = uid;

  // Spawn validation task (higher priority than background sync)
  String *uidPtr = new String(uid);
  xTaskCreate(processCardTask, "rfid_task", 16384, (void *)uidPtr, 3, NULL);
}

void processCardTask(void *parameter) {
  String uid = *((String *)parameter);
  delete (String *)parameter;

  webLog("Process: Card detected " + uid);

  // Wait for UI to show card number (UI_CARD → UI_VALIDATING transition)
  int waitCount = 0;
  while (uiState == UI_CARD && waitCount < 200) { // Max 2s safety
    vTaskDelay(pdMS_TO_TICKS(10));
    waitCount++;
  }
  uiState = UI_VALIDATING;

  // ════════════════════════════════════════════════════
  //  PRIORITY VALIDATION CHAIN
  // ════════════════════════════════════════════════════

  // ── P1: WiFi ──
  // ── P1: WiFi ──
  if (WiFi.status() != WL_CONNECTED) {
    webLog("Process: P1 FAIL - WiFi disconnected.");
    lastUserName = "NO WIFI";
    lastDetailedStatus = "WiFi Disconnected";
    lastStatusMsg = "WiFi Disconnected";
    currentName = "NO WIFI";
    lastResponseSuccess = false;

    if (currentMode == MODE_VERIFY) {
      handleBlockingVrfDisplay();
    } else if (currentMode == MODE_INV) {
      lcdLock();
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("  >> DENIED <<  ");
      lcdUnlock();
      triggerResultLed(false);
      beepDenied();
      uiState = UI_RESULT;
      resultStep = RES_DENIED_1;
      resultTimer = millis();
    } else {
      handleBlockingResultDisplay();
    }
    goto WAIT_AND_UNLOCK;
  }

  // ── P1b: Internet ──
  if (!hasInternet && WiFi.status() == WL_CONNECTED) {
    hasInternet = checkInternet();
  }
  if (!hasInternet) {
    webLog("Process: P1 FAIL - No internet.");
    lastUserName = "NO INTERNET";
    lastDetailedStatus = "Check Connection";
    lastStatusMsg = "Check Connection";
    currentName = "NO INTERNET";
    lastResponseSuccess = false;

    if (currentMode == MODE_VERIFY) {
      handleBlockingVrfDisplay();
    } else if (currentMode == MODE_INV) {
      lcdLock();
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("  >> DENIED <<  ");
      lcdUnlock();
      triggerResultLed(false);
      beepDenied();
      uiState = UI_RESULT;
      resultStep = RES_DENIED_1;
      resultTimer = millis();
    } else {
      handleBlockingResultDisplay();
    }
    goto WAIT_AND_UNLOCK;
  }

  // ── P2: Script ID ──
  if (scriptId == "") {
    webLog("Process: P2 FAIL - Script ID missing.");
    lastUserName = "CONFIG ERROR";
    lastDetailedStatus = "No Script ID";
    lastStatusMsg = "No Script ID";
    currentName = "CONFIG ERROR";
    lastResponseSuccess = false;

    if (currentMode == MODE_VERIFY) {
      handleBlockingVrfDisplay();
    } else if (currentMode == MODE_INV) {
      lcdLock();
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("  >> DENIED <<  ");
      lcdUnlock();
      triggerResultLed(false);
      beepDenied();
      uiState = UI_RESULT;
      resultStep = RES_DENIED_1;
      resultTimer = millis();
    } else {
      handleBlockingResultDisplay();
    }
    goto WAIT_AND_UNLOCK;
  }

  // ── P2b: Database sync status ──
  if (currentSyncStatus != SYNC_OK) {
    webLog("Process: P2 FAIL - DB not verified. Status=" +
           String(currentSyncStatus));
    lastUserName = "DATABASE ERROR";
    lastDetailedStatus = "Device Not Verified";
    lastStatusMsg = "Device Not Verified";
    currentName = "DATABASE ERROR";
    lastResponseSuccess = false;

    if (currentMode == MODE_VERIFY) {
      handleBlockingVrfDisplay();
    } else if (currentMode == MODE_INV) {
      lcdLock();
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("  >> DENIED <<  ");
      lcdUnlock();
      triggerResultLed(false);
      beepDenied();
      uiState = UI_RESULT;
      resultStep = RES_DENIED_1;
      resultTimer = millis();
    } else {
      handleBlockingResultDisplay();
    }
    goto WAIT_AND_UNLOCK;
  }

  // ── P3: Cloud API Call ──
  {
    webLog("Process: Pre-checks OK. Calling cloud...");
    currentStep = STEP_VALIDATING;

    String action =
        (currentMode == MODE_VERIFY)
            ? "check"
            : ((currentMode == MODE_INV)
                   ? "inventory"
                   : (currentMode == MODE_IN ? "checkin" : "checkout"));

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    String url = "https://script.google.com/macros/s/" + scriptId +
                 "/exec?action=" + action + "&uid=" + uid + "&mac=" + myMac +
                 "&user=" + deviceUser + "&pass=" + devicePass;

    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    int code = -1;
    // ⭐️ RETRY LOGIC: Google Apps Script TLS can fail transiently (HTTP -11)
    for (int attempt = 0; attempt < 3; attempt++) {
      if (http.begin(client, url)) {
        http.setTimeout(15000); // Increased from 8s to 15s to handle Apps Script latencies
        code = http.GET();
        if (code == 200 || code == 302) {
          break; // Success
        }
        http.end();
        client.stop(); // release SSL resources immediately
        webLog("Cloud: Attempt " + String(attempt + 1) + " failed (HTTP " +
               String(code) + "). Retrying...");
      }
      if (attempt < 2) {
        vTaskDelay(pdMS_TO_TICKS(1000)); // Wait before retry
        client.stop();
        client.setInsecure(); // Re-init TLS
      }
    }

    if (code == 200 || code == 302) {
      String response = http.getString();
      http.end();
      client.stop(); // release SSL resources immediately
      webLog("Cloud: HTTP " + String(code) + " OK");
      parseGoogleResponse(response);

      if (currentMode == MODE_VERIFY) {
        // VRF mode: Use blocking VRF display
        handleBlockingVrfDisplay();
      } else if (currentMode == MODE_INV) {
        // 1. Immediately display the outcome on the LCD
        lcdLock();
        lcd.clear();
        if (lastResponseSuccess) {
          lcd.setCursor(0, 0);
          lcd.print("CARD ADDED OK   ");
          lcd.setCursor(0, 1);
          lcd.print("Added to Pool   ");
        } else {
          lcd.setCursor(0, 0);
          lcd.print("CARD ADDED FAIL ");
          lcd.setCursor(0, 1);
          String err = lastStatusMsg;
          if (err == "") err = "Already Exists";
          if (err.length() > 16) err = err.substring(0, 16);
          while (err.length() < 16) err += " ";
          lcd.print(err);
        }
        lcdUnlock();

        // 2. Trigger LED result flash
        triggerResultLed(lastResponseSuccess);

        // 3. Trigger Beep (blocking)
        if (lastResponseSuccess) {
          beepInventorySuccess();
          delay(1000); // Hold success screen for extra 1s
        } else {
          beepDenied(); // Blocks for 3s (total 3s visibility during alarm)
        }

        // 4. Route to next step in UI result flow and start timer AFTER beep finishes
        uiState = UI_RESULT;
        resultTimer = millis();
        resultStep = RES_STATUS_DETAIL;
      } else {
        // Attendance modes (IN / OUT)
        handleBlockingResultDisplay();
      }
    } else {
      http.end();
      client.stop(); // release SSL resources immediately
      webLog("Cloud Error: HTTP " + String(code) + " after 3 attempts");
      lastUserName = "CLOUD ERROR";
      lastDetailedStatus = "Code: " + String(code);
      lastStatusMsg = "HTTP " + String(code);
      currentName = "CLOUD ERROR";
      lastResponseSuccess = false;

      if (currentMode == MODE_VERIFY) {
        // VRF mode: Use blocking VRF display
        handleBlockingVrfDisplay();
      } else if (currentMode == MODE_INV) {
        lcdLock();
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("  >> DENIED <<  ");
        lcdUnlock();

        triggerResultLed(false);
        beepDenied(); // Blocking 3s beep

        uiState = UI_RESULT;
        resultStep = RES_DENIED_1;
        resultTimer = millis();
      } else {
        // Attendance modes (IN / OUT)
        handleBlockingResultDisplay();
      }
    }
  }

WAIT_AND_UNLOCK:
  // ⭐️ CRITICAL: Wait for UI result sequence to complete BEFORE unlocking
  // In VRF/INV modes, unlock immediately (operators scan many cards quickly).
  // In attendance modes, wait so the student has time to read their result.
  {
    if (currentMode == MODE_INV) {
      // Just let the first frame of UI_RESULT render (≤800 ms), then unlock
      int fastCounter = 0;
      while (uiState != UI_RESULT && fastCounter < 100) {
        vTaskDelay(pdMS_TO_TICKS(10));
        fastCounter++;
      }
      vTaskDelay(pdMS_TO_TICKS(800)); // Show result briefly before unlock
    } else if (currentMode == MODE_VERIFY) {
      // Already blocked and returned to UI_HOME in handleBlockingVrfDisplay()
    } else {
      // ⭐️ Wait for the FULL result display sequence to complete before unlocking.
      // Full sequence max: Name(2)+Reg(2)+College(3)+Batch(2)+Start(2)+Scan(2)+Status(2)+Reason(3) = 18s
      // Safety cap at 30s to prevent infinite lock.
      int safetyCounter = 0;
      while (uiState == UI_RESULT && safetyCounter < 3000) { // Max 30s safety
        vTaskDelay(pdMS_TO_TICKS(10));
        safetyCounter++;
      }
    }
  }

  // ⭐️ FULL UNLOCK SEQUENCE
  currentStep = STEP_NONE;
  cardProcessing = false;
  lastSuccessfulScanTime = millis(); // Start cooldown timer

  // Small extra delay before unlocking scan to prevent immediate re-detect
  vTaskDelay(pdMS_TO_TICKS(500));
  scanLocked = false;

  webLog("Process: Scan unlocked. Ready for next card.");
  vTaskDelete(NULL);
}

// (startProcessingSteps logic merged into processCardTask)

void parseGoogleResponse(String response) {
#if ARDUINOJSON_VERSION_MAJOR >= 7
  JsonDocument doc;
#else
  DynamicJsonDocument doc(1024);
#endif

  DeserializationError error = deserializeJson(doc, response);
  if (error) {
    if (response.indexOf("<html") != -1 ||
        response.indexOf("Google Accounts") != -1) {
      lastUserName = "DATABASE ERROR";
      lastDetailedStatus = "Script Private";
      webLog("ERROR: Google Script is PRIVATE. Change access to 'Anyone'.");
    } else {
      lastUserName = "DATABASE ERROR";
      lastDetailedStatus = "Bad Response";
      webLog("ERROR: JSON Parse failed. Length: " + String(response.length()));
    }
    return;
  }

  if (doc["status"] == "success") {
    lastResponseSuccess = true;
    String name = doc["name"] | "Unknown";
    String regId = doc["regId"] | "";

    // ⭐️ VRF MODE REFINEMENT: If card is in inventory but student is unknown
    if (currentMode == MODE_VERIFY && (name == "Unknown" || regId == "")) {
      lastResponseSuccess = false; // Treat as "Denied path" to show Inventory msg
      lastUserName = "UNASSIGNED";
      lastDetailedStatus = "Inventory Available"; // Trigger "Record Found"
      lastStatusMsg = "Not Assigned";
      currentName = "UNASSIGNED";
      return;
    }

    lastUserName = name;
    currentName = lastUserName;
    currentRegId = regId;
    currentRegNo = doc["regNo"] | "N/A";
    currentCollege = doc["college"] | "N/A";
    currentSlotTiming = doc["slotTiming"] | "N/A";
    currentBatch = doc["batch"] | "N/A";
    currentPunchTime = doc["time"] | "";
    currentDetailStatus = doc["detailStatus"] | "";
    currentStartDate = doc["startDate"] | "N/A";
    currentEndDate = doc["endDate"] | "N/A";
    lastStatusMsg = doc["message"] | "Success";
    currentPhotoUrl = doc["photoUrl"] | "";
    if (currentPhotoUrl.length() == 0 && doc["student"]["photoUrl"]) {
      currentPhotoUrl = doc["student"]["photoUrl"].as<String>();
    }
    lastRegId = currentRegId;
    lastRegNo = currentRegNo;
    lastCollege = currentCollege;
    lastBatch = currentBatch;
    lastPhotoUrl = currentPhotoUrl;
    lastStatusMsg = doc["message"] | "Success";
    lastDetailedStatus = lastStatusMsg;
  } else {
    lastResponseSuccess = false; // ⭐️ §3: Track validation failure
    String errName = doc["name"] | "Denied";
    String errMsg = doc["message"] | "Access Denied";

    if (errName == "UNAUTHORIZED") {
      lastUserName = "REGISTRY ERROR";
      lastDetailedStatus = "Device not Config";
      // ⭐️ Device-level error → re-enter DB Error Mode
      currentSyncStatus = SYNC_DATABASE_SYNC_ERROR;
      deviceVerified = false;
    } else if (errName == "BLOCKED") {
      lastUserName = "DEVICE BLOCKED";
      lastDetailedStatus = "Contact Admin";
      // ⭐️ Device-level error → re-enter DB Error Mode
      currentSyncStatus = SYNC_DATABASE_SYNC_ERROR;
      deviceVerified = false;
    } else if (errName == "DATABASE ERROR") {
      lastUserName = "DATABASE ERROR";
      lastDetailedStatus = errMsg;
      // ⭐️ Device auth failed → re-enter DB Error Mode with recovery pings
      currentSyncStatus = SYNC_DATABASE_SYNC_ERROR;
      deviceVerified = false;
    } else {
      // Card-level errors (TAG NOT FOUND, UNASSIGNED, etc.) → NOT a DB error
      lastUserName = errName;
      lastDetailedStatus = errMsg;
      currentRegId = doc["regId"] | "";
      currentRegNo = doc["regNo"] | "N/A";
      currentCollege = doc["college"] | "N/A";
      currentSlotTiming = doc["slotTiming"] | "N/A";
      currentBatch = doc["batch"] | "N/A";
      currentPunchTime = doc["time"] | "";
      currentDetailStatus = doc["detailStatus"] | "";
      currentPhotoUrl = doc["photoUrl"] | "";
      if (currentPhotoUrl.length() == 0 && doc["student"]["photoUrl"]) {
        currentPhotoUrl = doc["student"]["photoUrl"].as<String>();
      }
      if (currentRegId.length() > 0 || errName != "UNKNOWN CARD") {
        lastRegId = currentRegId;
        lastRegNo = currentRegNo;
        lastCollege = currentCollege;
        lastBatch = currentBatch;
        lastPhotoUrl = currentPhotoUrl;
      }
    }

    currentName = lastUserName;
    lastStatusMsg = lastDetailedStatus;
    
    // ⭐️ VRF MODE REFINEMENT: Suppress generic "Denied" message, use status codes
    if (currentMode == MODE_VERIFY) {
      String lowerMsg = lastStatusMsg;
      lowerMsg.toLowerCase();
      if (lowerMsg.indexOf("not found") != -1 || lowerMsg.indexOf("no record") != -1) {
         lastUserName = "Record Not Found";
         lastDetailedStatus = "Add To Inventory";
      } else if (lowerMsg.indexOf("unassigned") != -1 || lowerMsg.indexOf("available") != -1 || lastUserName == "UNASSIGNED") {
         lastUserName = "Record Found";
         lastDetailedStatus = "Card Available";
         lastStatusMsg = "Not Assigned";
      }
    }
  }
}

void handleBlockingResultDisplay() {
  uiState = UI_BLOCKING_RESULT;

  // Check if student info is available (registered student denied/success)
  bool hasStudentInfo = (currentRegId.length() > 0 && currentRegId != "N/A" &&
                         currentName != "UNAUTHORIZED" && currentName != "BLOCKED" &&
                         currentName != "DATABASE ERROR" && currentName != "REGISTRY ERROR" &&
                         currentName != "DEVICE BLOCKED" && currentName != "NO WIFI" &&
                         currentName != "NO INTERNET" && currentName != "CONFIG ERROR" &&
                         currentName != "CLOUD ERROR");

  // 1. Clear LCD and display outcome
  lcdLock();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(lastResponseSuccess ? "  >> SUCCESS << " : "  >> DENIED <<  ");
  
  // Display status message/reason on row 1
  lcd.setCursor(0, 1);
  String statusMsg = lastStatusMsg.length() > 0 ? lastStatusMsg : (lastResponseSuccess ? "Marked Present" : "Access Denied");
  // Pad or truncate statusMsg to fit 16 chars
  if (statusMsg.length() > 16) {
    statusMsg = statusMsg.substring(0, 16);
  } else {
    while (statusMsg.length() < 16) statusMsg += " ";
  }
  lcd.print(statusMsg);
  lcdUnlock();

  // 2. Trigger LED and Buzzer
  triggerResultLed(lastResponseSuccess);
  if (lastResponseSuccess) {
    beepSuccess();
    delay(1050); // Hold success screen for ~1.5s total (beep + delay)
  } else {
    beepDenied(); // Blocks for 3.0s total, showing >> DENIED << and reason
  }

  // 3. Display Student Details if available
  if (hasStudentInfo) {
    // Show Name (scroll if > 16 chars, else static)
    scrollRow2Blocking("Name:", currentName, 3000);

    // Show Reg ID (scroll if > 16 chars, else static)
    scrollRow2Blocking("Reg ID:", currentRegId, 2000);

    // Show College (scroll if > 16 chars, else static)
    scrollRow2Blocking("College:", currentCollege, 3000);

    // Show Batch (scroll if > 16 chars, else static)
    scrollRow2Blocking("Batch:", currentBatch, 2000);

    // If success, also show slot and punch time
    if (lastResponseSuccess) {
      // Show Slot (scroll if > 16 chars, else static)
      scrollRow2Blocking("Slot:", currentSlotTiming, 2000);

      // Show Punch Time (scroll if > 16 chars, else static)
      String pTime = currentPunchTime.length() > 0 ? currentPunchTime : getRTC();
      scrollRow2Blocking("Time:", pTime, 2000);
    } else {
      // If it was denied, also show the reason in details using scroll row block (3s)
      String denReason = lastStatusMsg.length() > 0 ? lastStatusMsg : "Access Denied";
      scrollRow2Blocking("Reason:", denReason, 3000);
    }
  } else {
    // If NO student info and it was denied, show Detailed status/reason screen for 2 more seconds
    if (!lastResponseSuccess) {
      String detDisp = lastDetailedStatus.length() > 0 ? lastDetailedStatus : "Access Denied";
      scrollRow2Blocking("Reason:", detDisp, 2000);
    }
  }

  // Clear LCD when done and transition to UI_HOME
  lcdLock();
  lcd.clear();
  lcdUnlock();
  uiState = UI_HOME;
}

void scrollRow2Blocking(String header, String text, unsigned long durationMs) {
  // Lock LCD and clear it
  lcdLock();
  lcd.clear();
  lcd.setCursor(0, 0);
  String headDisp = header.length() > 16 ? header.substring(0, 16) : header;
  while (headDisp.length() < 16) headDisp += " ";
  lcd.print(headDisp);
  
  if (text.length() <= 16) {
    // Pad text to 16 chars and print statically
    String padded = text;
    while (padded.length() < 16) padded += " ";
    lcd.setCursor(0, 1);
    lcd.print(padded);
    lcdUnlock();
    // Static delay
    delay(durationMs);
  } else {
    // Scrolling text
    String padded = text + "    ";
    int maxIndex = padded.length() - 16;
    if (maxIndex < 0) maxIndex = 0;
    
    unsigned long startTime = millis();
    int scrollIndex = 0;
    
    lcdUnlock(); // Release lock before looping to let other tasks run if needed
    
    while (millis() - startTime < durationMs) {
      String view = padded.substring(scrollIndex, scrollIndex + 16);
      lcdLock();
      lcd.setCursor(0, 1);
      lcd.print(view);
      lcdUnlock();
      
      scrollIndex++;
      if (scrollIndex > maxIndex) {
        scrollIndex = 0;
      }
      
      delay(350); // Scrolling delay between steps
    }
  }
}

void handleBlockingVrfDisplay() {
  uiState = UI_BLOCKING_RESULT;

  // 0. Handle System Errors (WiFi, Internet, Cloud, Config, etc.)
  bool isSystemError = (currentName == "NO WIFI" || currentName == "NO INTERNET" ||
                        currentName == "CONFIG ERROR" || currentName == "DATABASE ERROR" ||
                        currentName == "CLOUD ERROR" || currentName == "SYSTEM ERROR");

  if (isSystemError) {
    lcdLock();
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("  >> ERROR <<   ");
    lcdUnlock();

    triggerResultLed(false);
    beepDenied(); // blocks for 3.0s total

    String errReason = lastDetailedStatus.length() > 0 ? lastDetailedStatus : "System Error";
    scrollRow2Blocking("Reason:", errReason, 3000);
  }
  // Case 1: Success (Card assigned to a student)
  else if (lastResponseSuccess) {
    // Skip outcomes screen (>> SUCCESS <<) and go straight to student details sequence
    triggerResultLed(true);
    beepSuccess(); // blocks for 450ms

    // Show details sequence in VRF mode with proper delays:
    // Name (3s) -> Reg ID (2s) -> College (3s) -> Reg No (2s) -> Slot (2s) -> Batch (2s) -> Start Date (2s) -> End Date (2s)
    scrollRow2Blocking("Name:", currentName, 3000);
    scrollRow2Blocking("Reg ID:", currentRegId, 2000);
    scrollRow2Blocking("College:", currentCollege, 3000);
    scrollRow2Blocking("Reg No:", currentRegNo, 2000);
    scrollRow2Blocking("Slot:", currentSlotTiming, 2000);
    scrollRow2Blocking("Batch:", currentBatch, 2000);
    scrollRow2Blocking("Start:", currentStartDate, 2000);
    scrollRow2Blocking("End:", currentEndDate, 2000);
  } 
  // Case 2: Not assigned in Inventory
  else if (currentName == "CARD UNASSIGNED" || currentName == "UNASSIGNED" || 
           lastDetailedStatus.indexOf("Unassigned") != -1 || lastDetailedStatus == "Card Available" || 
           lastDetailedStatus == "Inventory Available" || lastStatusMsg == "Not Assigned") {
    // Show static denied on LCD during buzzer beep (3s)
    lcdLock();
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("  >> DENIED <<  ");
    lcd.setCursor(0, 1);
    lcd.print("Not Assigned    ");
    lcdUnlock();

    triggerResultLed(false);
    beepDenied(); // Blocks for 3.0s total

    // Show status: "Not Assigned in Inventory"
    scrollRow2Blocking("Card Status:", "Not Assigned in Inventory", 4000);
  } 
  // Case 3: Not found / not in inventory
  else {
    // Show static denied on LCD during buzzer beep (3s)
    lcdLock();
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("  >> DENIED <<  ");
    lcd.setCursor(0, 1);
    lcd.print("Card Not Found  ");
    lcdUnlock();

    triggerResultLed(false);
    beepDenied(); // Blocks for 3.0s total

    // Show status: "The card not found add to inventory"
    scrollRow2Blocking("Card Status:", "The card not found add to inventory", 4000);
  }

  // Clear LCD when done and transition to UI_HOME
  lcdLock();
  lcd.clear();
  lcdUnlock();
  uiState = UI_HOME;
}

void saveInventoryToSheet(String uid) {
  if (!hasInternet || scriptId == "") {
    lastStatusMsg = "Cannot save: no internet";
    return;
  }
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url = "https://script.google.com/macros/s/" + scriptId +
               "/exec?action=inventory&uid=" + uid + "&mac=" + myMac;
  // 🔧 FIXED: use the library constant directly
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (http.begin(client, url)) {
    int code = http.GET();
    if (code == 200 || code == 302) {
      lastStatusMsg = "Inventory saved";
      beep(2500, 100);
    } else {
      lastStatusMsg = "Save failed";
    }
    http.end();
  }
}

// ==================== BUTTON HANDLING ====================
void handleButtons() {
  if (millis() < 3000) {
    // Ignore startup pin glitches during boot
    return;
  }

  static bool lastStatsState = HIGH;
  static bool lastModeState = HIGH;
  static unsigned long statsPressStart = 0;
  static unsigned long modePressStart = 0;

  bool statsPressed = (digitalRead(BTN_STATS) == LOW);
  bool modePressed = (digitalRead(BTN_MODE) == LOW);

  // Stats button (Pin 26) - Long press reset, Short press Toggle Info
  if (statsPressed && !lastStatsState)
    statsPressStart = millis();
  if (!statsPressed && lastStatsState && statsPressStart > 0) {
    unsigned long dur = millis() - statsPressStart;
    if (dur > 5000) {
      factoryReset();
    } else if (dur > 50) {
      // Cycle: Home (0) -> IP (1) -> MAC (2) -> Home (0)
      if (currentState == SYSTEM_READY || currentState == SYSTEM_INFO_SHOW) {
        infoCycle = (infoCycle + 1) % 3;
        if (infoCycle == 0) {
          currentState = SYSTEM_READY;
          uiState = UI_HOME;
        } else {
          currentState = SYSTEM_INFO_SHOW;
          uiState = UI_INFO_SHOW;
          infoTimer = millis();
          beep(1200, 100);
          webLog("UI: Showing " + String(infoCycle == 1 ? "IP" : "MAC"));
        }
      }
    }
    statsPressStart = 0;
  }

  // Mode button (Pin 15) - Short press toggle IN/OUT, Long press for Inventory
  static bool modeLongPressedTriggered = false;
  if (modePressed && !lastModeState) {
    modePressStart = millis();
    modeLongPressedTriggered = false;
  }
  
  if (modePressed && modePressStart > 0 && !modeLongPressedTriggered) {
    if (millis() - modePressStart > 3000) {
      currentMode = MODE_INV;
      beep(1500, 200);
      modeLongPressedTriggered = true;
    }
  }

  if (!modePressed && lastModeState && modePressStart > 0) {
    if (!modeLongPressedTriggered) {
      unsigned long dur = millis() - modePressStart;
      if (dur > 50) {
        if (currentMode == MODE_INV)
          currentMode = MODE_IN;
        else if (currentMode == MODE_IN)
          currentMode = MODE_OUT;
        else if (currentMode == MODE_OUT)
          currentMode = MODE_VERIFY;
        else
          currentMode = MODE_IN;
        beep(1000, 100);
      }
    }
    modePressStart = 0;
  }

  lastStatsState = statsPressed;
  lastModeState = modePressed;
}

void beep(int freq, int duration) { tone(BUZZER_PIN, freq, duration); }
void beepInventorySuccess() {
  tone(BUZZER_PIN, 2000, 80);
  delay(100);
  tone(BUZZER_PIN, 2500, 100);
}



FirmwareDetails fetchOnlineFirmwareDetails() {
  FirmwareDetails details = {false, "", "", false, "[]"};
  
  String url = "";
  if (otaSource == "github") {
    if (otaLink == "") return details;
    url = otaLink;
  } else {
    String activeScriptId = (otaId != "") ? otaId : scriptId;
    if (activeScriptId == "") return details;
    url = "https://script.google.com/macros/s/" + activeScriptId + "/exec?action=get_firmware_version";
  }
  
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  
  int code = -1;
  for (int attempt = 0; attempt < 3; attempt++) {
    if (http.begin(client, url)) {
      http.setTimeout(10000);
      code = http.GET();
      if (code == 200 || code == 302) {
        break;
      }
      http.end();
      client.stop();
    }
    delay(1000);
  }
  
  if (code == 200 || code == 302) {
    String response = http.getString();
    http.end();
    client.stop();
    
    // Parse JSON
    JSON_DOC_TYPE doc;
    DeserializationError error = deserializeJson(doc, response);
    if (!error) {
      details.success = true;
      details.latestVersion = doc["version"] | "";
      details.downloadUrl = doc["downloadUrl"] | "";
      if (details.latestVersion != "" && details.latestVersion != String(FIRMWARE_VERSION)) {
        details.updateAvailable = true;
      }
      
      // Extract versions array if present
      if (doc.containsKey("versions")) {
        String versionsStr;
        serializeJson(doc["versions"], versionsStr);
        details.rawVersionsJson = versionsStr;
      } else {
        // Fallback: create a single element array using latest version
        details.rawVersionsJson = "[{\"version\":\"" + details.latestVersion + "\",\"downloadUrl\":\"" + details.downloadUrl + "\"}]";
      }
    }
  } else {
    http.end();
    client.stop();
  }
  return details;
}

void otaTask(void *pvParameters) {
  otaRunning = true;
  otaProgress = 0;
  otaStatusStr = "updating";
  otaErrorMsg = "";
  
  String* urlPtr = (String*)pvParameters;
  String downloadUrl = *urlPtr;
  delete urlPtr; // Free the allocated String
  
  Serial.println("OTA Task: Starting update from " + downloadUrl);

  // Clear LCD and show initial progress immediately
  lcdLock();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("UPDATING FW...  ");
  lcd.setCursor(0, 1);
  lcd.print("Pls Wait     0%");
  lcdUnlock();
  
  // Track update attempts to prevent boot loops
  preferences.begin("gsv-rfid", false);
  String targetVer = preferences.getString("ota_target_ver", "");
  int retryCount = preferences.getInt("ota_retry_count", 0);
  
  if (otaTargetVerVal != "") {
    if (otaTargetVerVal == targetVer) {
      retryCount++;
    } else {
      targetVer = otaTargetVerVal;
      retryCount = 1;
    }
    preferences.putString("ota_target_ver", targetVer);
    preferences.putInt("ota_retry_count", retryCount);
  }
  preferences.end();
  
  WiFiClientSecure client;
  client.setInsecure();
  
  // Set up progress callback
  httpUpdate.onProgress([](int cur, int total) {
    int pct = (total > 0) ? ((cur * 100) / total) : 0;
    otaProgress = pct;
    otaStatusStr = "updating";
    
    static unsigned long lastLcdProgressUpdate = 0;
    if (millis() - lastLcdProgressUpdate > 1000) {
      lastLcdProgressUpdate = millis();
      lcdLock();
      lcd.setCursor(0, 0);
      lcd.print("UPDATING FW...  ");
      lcd.setCursor(0, 1);
      char buf[17];
      snprintf(buf, sizeof(buf), "Pls Wait   %3d%%", pct);
      lcd.print(buf);
      lcdUnlock();
    }
  });
  
  // Turn off reboot on update so we can control the reboot timing and send status to UI
  httpUpdate.rebootOnUpdate(false);
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  
  t_httpUpdate_return ret = httpUpdate.update(client, downloadUrl);
  
  if (ret == HTTP_UPDATE_OK) {
    Serial.println("OTA Task: HTTP_UPDATE_OK. Rebooting in 3 seconds...");
    otaProgress = 100;
    otaStatusStr = "rebooting";
    
    lcdLock();
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Update Success! ");
    lcd.setCursor(0, 1);
    lcd.print("Rebooting...    ");
    lcdUnlock();
    
    beepSuccess();
    
    delay(3000); // Give time for web browser to receive "rebooting" status
    ESP.restart();
  } else {
    otaRunning = false;
    otaStatusStr = "failed";
    otaErrorMsg = httpUpdate.getLastErrorString();
    if (otaErrorMsg == "") otaErrorMsg = "Update failed (code " + String(ret) + ")";
    Serial.printf("OTA Task: Failed. Error (%d): %s\r\n", httpUpdate.getLastError(), otaErrorMsg.c_str());
    
    lcdLock();
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Update Failed!  ");
    lcd.setCursor(0, 1);
    String err = otaErrorMsg;
    if (err.length() > 16) err = err.substring(0, 16);
    lcd.print(err);
    lcdUnlock();
    
    beepDenied();
    
    // Clear task handle
    otaTaskHandle = NULL;
    vTaskDelete(NULL);
  }
}

void startOtaUpdateTask(String url) {
  if (otaRunning) {
    Serial.println("OTA: Already running.");
    return;
  }
  
  // Save current version as previous version before updating
  preferences.begin("gsv-rfid", false);
  preferences.putString("ota_prev_ver", String(FIRMWARE_VERSION));
  preferences.end();
  
  // Allocate the url string dynamically so it can be passed to the task safely
  String* urlPtr = new String(url);
  
  xTaskCreatePinnedToCore(
    otaTask,           /* Task function. */
    "ota_task",        /* name of task. */
    8192,              /* Stack size of task */
    (void*)urlPtr,     /* parameter of the task */
    1,                 /* priority of the task */
    &otaTaskHandle,    /* Task handle */
    1                  /* pin task to core 1 */
  );
}

// performOtaUpdate removed (unused)

void initiateOtaCheckOnBoot() {
  if (!autoUpdateEnabled) {
    Serial.println("OTA: Auto update disabled, skipping boot check.");
    return;
  }
  
  preferences.begin("gsv-rfid", false);
  String targetVer = preferences.getString("ota_target_ver", "");
  int retryCount = preferences.getInt("ota_retry_count", 0);
  preferences.end();
  
  Serial.println("OTA: Auto update enabled, checking for update on boot...");
  FirmwareDetails details = fetchOnlineFirmwareDetails();
  if (details.success && details.updateAvailable) {
    if (details.latestVersion == targetVer && retryCount >= 3) {
      Serial.println("OTA: Skipping update to " + details.latestVersion + " to prevent bootloop (failed " + String(retryCount) + " times).");
      return;
    }
    
    Serial.println("OTA: Found newer version " + details.latestVersion + ", performing update...");
    lcdLock();
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Updating FW...  ");
    lcd.setCursor(0, 1);
    String verSub = details.latestVersion;
    if (verSub.length() > 16) verSub = verSub.substring(0, 16);
    lcd.print(verSub);
    lcdUnlock();
    
    otaTargetVerVal = details.latestVersion;
    startOtaUpdateTask(details.downloadUrl);
  } else {
    Serial.println("OTA: No update available on boot.");
    // If we booted successfully on the target version, reset the retry counter!
    if (String(FIRMWARE_VERSION) == targetVer && targetVer != "") {
      preferences.begin("gsv-rfid", false);
      preferences.remove("ota_target_ver");
      preferences.putInt("ota_retry_count", 0);
      preferences.end();
      Serial.println("OTA: Successfully booted on target version. Resetting retry count.");
    }
  }
}

// ✅ SUCCESS sound: single loud clear ding (one clean tone)
void beepSuccess() {
  tone(BUZZER_PIN, 2500, 400); // Single loud high-pitched ding
  delay(450);
  noTone(BUZZER_PIN);
}

// ❌ DENIED sound: loud continuous buzzer for 3 seconds
void beepDenied() {
  // Loud alternating two-tone alarm for 3 seconds
  unsigned long startTime = millis();
  while (millis() - startTime < 3000) {
    tone(BUZZER_PIN, 800, 200);
    delay(220);
    tone(BUZZER_PIN, 400, 200);
    delay(220);
  }
  noTone(BUZZER_PIN);
}

// ⭐️ Trigger LED result flash (called from processCardTask on Core1)
// SUCCESS → GREEN blinks fast 2s then returns to steady RED
// DENIED  → RED blinks exactly 4 times then returns to steady RED
void triggerResultLed(bool success) {
  ledModeStartTime = millis();
  lastLedBlinkTime = millis();
  ledBlinkState = false;
  deniedBlinkCount = 0; // Reset denied blink counter
  digitalWrite(LED_PIN, LOW);   // Turn off RED during transition
  digitalWrite(LED_GREEN, LOW); // Turn off GREEN during transition
  currentLedMode = success ? LED_MODE_RESULT_SUCCESS : LED_MODE_RESULT_DENIED;
}

// ==================== WIFI & NETWORK ====================
void connectWiFi() {
  if (savedSSID == "" || scriptId == "") {
    startAPMode();
    return;
  }

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);
  WiFi.setHostname(DEVICE_NAME);
  WiFi.begin(savedSSID.c_str(), savedPass.c_str());
  Serial.println("Attempting connection to: " + savedSSID);

  // We don't block infinitely here; loop() handles the IP assigned transition
  // But we give it a few seconds initially
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 5000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    WiFi.mode(WIFI_STA); // Turn off softAP, keep only STA
    localIpStr = WiFi.localIP().toString();
    configTime(19800, 0, "pool.ntp.org", "time.google.com");
    timeSynced = true;
  }
}

bool checkInternet() {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  http.setTimeout(5000);
  
  // Endpoint 1: Google generate_204
  http.begin("http://clients3.google.com/generate_204");
  int code = http.GET();
  http.end();
  if (code == 200 || code == 204) return true;

  // Endpoint 2: Cloudflare generate_204
  http.begin("http://cp.cloudflare.com/generate_204");
  code = http.GET();
  http.end();
  if (code == 200 || code == 204) return true;

  // Endpoint 3: Google gstatic fallback
  http.begin("http://connectivitycheck.gstatic.com/generate_204");
  code = http.GET();
  http.end();
  return (code == 200 || code == 204);
}

void checkDB() {
  if (scriptId == "")
    return;

  if (myMac == "" || myMac == "00:00:00:00:00:00") {
    myMac = WiFi.macAddress();
    myMac.toUpperCase();
    webLog("Hardware: MAC re-captured: " + myMac);
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  String rssi = String(WiFi.RSSI());
  String ip = WiFi.localIP().toString();

  String url = "https://script.google.com/macros/s/" + scriptId +
               "/exec?action=check_device&mac=" + myMac + "&rssi=" + rssi +
               "&ip=" + ip +
               "&user=" + (deviceUser == "" ? "admin" : deviceUser) +
               "&pass=" + (devicePass == "" ? "password123" : devicePass);

  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (http.begin(client, url)) {
    http.setTimeout(8000); // ⭐️ OPTIMIZED: 8s timeout for background sync
    int code = http.GET();
    // webLog("DB: Response Code: " + String(code));
    if (code == 200 || code == 302) {
      String res = http.getString();
      String escapedRes = res;
      escapedRes.replace("\r", " ");
      escapedRes.replace("\n", " ");
      webLog("DB: RAW RES: [" + escapedRes + "]");
      JSON_DOC_TYPE doc;
      DeserializationError error = deserializeJson(doc, res);

      if (!error) {
        if (doc.containsKey("command")) {
          String cmd = doc["command"] | "";
          if (cmd.startsWith("HANDSHAKE:")) {
            webLog("Handshake: Request Received: " + cmd);
            int firstColon = cmd.indexOf(':');
            int secondColon = cmd.indexOf(':', firstColon + 1);
            if (secondColon != -1) {
              String hUser = cmd.substring(firstColon + 1, secondColon);
              String hPass = cmd.substring(secondColon + 1);
              bool hMatch = (hUser == deviceUser && hPass == devicePass);

              String reportUrl =
                  "https://script.google.com/macros/s/" + scriptId +
                  "/exec?action=handshake_result&mac=" + myMac +
                  "&success=" + String(hMatch ? "true" : "false") +
                  "&ip=" + WiFi.localIP().toString();

              WiFiClientSecure rClient;
              rClient.setInsecure();
              HTTPClient rHttp;
              rHttp.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
              if (rHttp.begin(rClient, reportUrl)) {
                rHttp.GET();
                rHttp.end();
                webLog("Handshake: Result sent to cloud -> " +
                       String(hMatch ? "SUCCESS" : "FAILED"));
                // Visual/Audio Feedback
                if (hMatch) {
                  digitalWrite(LED_GREEN, HIGH);
                  beep(1000, 100);
                  delay(50);
                  beep(1500, 150);
                  digitalWrite(LED_GREEN, LOW);
                } else {
                  digitalWrite(LED_PIN, HIGH);
                  beep(400, 500);
                  digitalWrite(LED_PIN, LOW);
                }
              }
              // ⭐️ HANDSHAKE COMPLETED: Return to home screen immediately
              isDisplayingCommand = false;
              lcdLock();
              lcd.clear();
              lcdUnlock();
            }
          }
        }
        // -----------------------
        String status = doc["status"] | "";
        String msg = doc["message"] | "";

        deviceVerified = (status == "REGISTERED" || status == "ok" ||
                          status == "success" || status == "SUCCESS");

        if (deviceVerified) {
          currentSyncStatus = SYNC_OK;
          initialSyncDone = true;
        } else {
          currentSyncStatus = SYNC_DATABASE_SYNC_ERROR;
          if (status == "NOT_REGISTERED" || status == "PENDING") {
            lastDetailedStatus = "Awaiting Approval";
          } else {
            lastDetailedStatus = "DATABASE ERROR";
          }
          // ⭐️ ERROR BEEP: Unauthorized or Database issue detected
          beep(300, 1000);
        }

        syncFailureCount = 0;

        // Process Commands...
        if (doc.containsKey("command")) {
          String cmd = doc["command"] | "";
          if (cmd != "") {
            commandReceivedTime = millis();
            isDisplayingCommand = true;
            if (cmd == "SET_MODE_IN") {
              currentMode = MODE_IN;
              lcdLock();
              lcd.clear();
              lcd.setCursor(0, 0);
              lcd.print("Remote Command:");
              lcd.setCursor(0, 1);
              lcd.print("Switching -> IN ");
              lcdUnlock();
              beep(1000, 100);
            } else if (cmd == "SET_MODE_OUT") {
              currentMode = MODE_OUT;
              lcdLock();
              lcd.clear();
              lcd.setCursor(0, 0);
              lcd.print("Remote Command:");
              lcd.setCursor(0, 1);
              lcd.print("Switching -> OUT");
              lcdUnlock();
              beep(1000, 100);
            } else if (cmd == "SET_MODE_INV") {
              currentMode = MODE_INV;
              lcdLock();
              lcd.clear();
              lcd.setCursor(0, 0);
              lcd.print("Remote Command:");
              lcd.setCursor(0, 1);
              lcd.print("Switching -> INV");
              lcdUnlock();
              beep(1000, 100);
            } else if (cmd == "RFID_CAPTURE_START") {
              currentMode = MODE_INV;
              capturedUid = "--------";
              lcdLock();
              lcd.clear();
              lcd.setCursor(0, 0);
              lcd.print("Remote Capture:");
              lcd.setCursor(0, 1);
              lcd.print("Place Card Now  ");
              lcdUnlock();
              beep(1200, 200);
            } else if (cmd == "PING") {
              lcdLock();
              lcd.clear();
              lcd.setCursor(0, 0);
              lcd.print("Hardware Ping:");
              lcd.setCursor(0, 1);
              lcd.print("Echo: 200 OK    ");
              lcdUnlock();
              beep(1500, 100);
              delay(100);
              beep(1500, 100);
              // ⭐️ STARTUP FANFARE
              delay(500);
              beep(800, 100);
              delay(100);
              beep(1000, 100);
              delay(100);
              beep(1200, 300);
            }
          }
        }
      } else {
        webLog("DB: JSON Error: " + String(error.c_str()));
        webLog("DB: Code: " + String(code));
        if (res.length() > 0) {
          webLog("DB: Start of Payload: " + res.substring(0, 100));
        } else {
          webLog("DB: Payload was EMPTY");
        }
        currentSyncStatus = SYNC_DATABASE_CONFIG_ERROR;
        lastStatusMsg = "DB: Invalid Resp";
      }

      webLog(deviceVerified ? "DB: Verified OK" : "DB: Verification Failed");
    } else {
      String err = "DB Error: HTTP " + String(code);
      webLog(err);
      if (code == 404)
        currentSyncStatus = SYNC_DATABASE_CONFIG_ERROR;
      else
        currentSyncStatus = SYNC_DATABASE_SYNC_ERROR;

      deviceVerified = false;
      syncFailureCount++;
      webLog("DB: Sync Failure #" + String(syncFailureCount) +
             " - HTTP Code: " + String(code));
    }
    http.end();
  } else {
    currentSyncStatus = SYNC_DATABASE_SYNC_ERROR;
    webLog("DB: Could not start HTTP connection");
  }
}

void initializeLCD() {
  Wire.begin(21, 22);
  Wire.setClock(100000); // 100kHz for stability
  if (lcdMutex == NULL)
    lcdMutex = xSemaphoreCreateMutex();
  lcdLock();
  lcd.init();
  delay(100);
  lcd.backlight();
  setCustomIcons();
  lcdUnlock();
}

void mDNS_begin() {
  bool staActive = (WiFi.status() == WL_CONNECTED);
  bool apActive = (WiFi.softAPIP() != IPAddress(0, 0, 0, 0) && (WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA));

  if (!staActive && !apActive)
    return;

  // ⭐️ REFRESH: Always end before begin to clear cache/sockets
  MDNS.end();

  if (MDNS.begin(DEVICE_NAME)) {
    MDNS.addService("http", "tcp", 80);
    // Add TXT records for enhanced discovery
    MDNS.addServiceTxt("http", "tcp", "id", myMac);
    MDNS.addServiceTxt("http", "tcp", "ver", FIRMWARE_VERSION);

    webLog("Responder: mDNS active at http://" + String(DEVICE_NAME) +
           ".local");
  } else {
    webLog("CRITICAL: mDNS startup failed");
  }
}

void startAPMode() {
  currentState = SYSTEM_WIFI_CONFIG;

  if (savedSSID == "") {
    WiFi.mode(WIFI_AP); // Only AP mode, do not open STA mode since credentials are empty
  } else {
    WiFi.mode(WIFI_AP_STA); // Use AP_STA to allow scanning while in AP mode
  }
  WiFi.softAP(AP_SSID, AP_PASS);
  delay(1000);
  IPAddress apIP = WiFi.softAPIP();
  localIpStr = apIP.toString();

  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(53, "*", apIP);
  setupWebServer();
  mDNS_begin(); // Call mDNS_begin so that http://vvarma-rfid.local works in AP mode!

  Serial.println("\n=== AP MODE STARTED ===");
  Serial.print("SSID: ");
  Serial.println(AP_SSID);
  Serial.print("Password: ");
  Serial.println(AP_PASS);
  Serial.print("IP: ");
  Serial.println(apIP);

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("AP: CONFIGURE");
  lcd.setCursor(0, 1);
  lcd.print(apIP.toString());
}

void factoryReset() {
  lcd.clear();
  lcd.print("FACTORY RESET...");
  lcd.setCursor(0, 1);
  lcd.print("Please Wait");
  preferences.begin("gsv-rfid", false);
  preferences.clear();
  preferences.end();
  delay(2000);
  ESP.restart();
}

// ==================== AUTH HELPER ====================
bool isAuth() {
  lastActivityTime = millis(); // ⭐️ Track web activity
  if (server.hasHeader("Cookie")) {
    String cookie = server.header("Cookie");
    if (cookie.indexOf("VARMA_SID_V4_SECURE=GSV_OS_TRUSTED") != -1)
      return true;
  }
  return false;
}

// ==================== WEB SERVER ====================
void setupWebServer() {
  if (serverStarted)
    return;

  const char *headerkeys[] = {"Cookie"};
  size_t headerkeyssize = sizeof(headerkeys) / sizeof(char *);
  server.collectHeaders(headerkeys, headerkeyssize);

  server.on("/", []() {
    bool authed = isAuth();
    if (currentState == SYSTEM_WIFI_CONFIG) {
      if (authed)
        serveConfigPage();
      else
        serveAPLoginPage();
    } else {
      // In STA Mode, we redirect to dashboard which handles its own overlay
      // login
      server.sendHeader("Location", "/dashboard", true);
      server.send(302, "text/plain", "");
    }
  });
  // Captive portal redirects
  server.on("/generate_204", []() {
    server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/",
                      true);
    server.send(302, "text/plain", "");
  });
  server.on("/hotspot-detect.html", []() {
    server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/",
                      true);
    server.send(302, "text/plain", "");
  });
  server.on("/library/test/success.html", []() {
    server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/",
                      true);
    server.send(302, "text/plain", "");
  });
  server.on("/connecttest.txt", []() {
    server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/",
                      true);
    server.send(302, "text/plain", "");
  });
  server.on("/ncsi.txt", []() {
    server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/",
                      true);
    server.send(302, "text/plain", "");
  });

  server.on("/serve_config", []() {
    if (currentState == SYSTEM_WIFI_CONFIG)
      serveConfigPage();
    else
      server.send(403, "text/plain", "Forbidden");
  });

  server.on("/serial_data", []() {
    server.send(200, "text/plain", webSerialLog);
    webSerialLog = ""; // Clear after sending to avoid duplicates
  });
  server.on("/dashboard", handleDashboard);
  server.on("/login", handleLogin); // Allow GET/POST
  server.on("/status", handleStatus);
  server.on("/mode", handleMode);
  server.on("/capture", handleCapture);
  server.on("/sync", handleSaveInventory); // New mapping for 'SAVE TO CLOUD'
  server.on("/save_inventory", HTTP_POST, handleSaveInventory);
  server.on("/save_net", handleSaveNetwork);
  server.on("/save_sys", handleSaveSystem);
  server.on("/factory_reset", handleFactoryReset);
  server.on("/scan_wifi", handleScanWiFi);
  server.on("/logs", handleLogs);
  server.on("/inventory", handleInventory);
  server.on("/validate", handleValidate);
  server.on("/reset_display", handleResetDisplay);
  server.on("/get_script_id", handleGetScriptId);

  server.on("/toggle_auto_update", []() {
    if (!isAuth()) {
      server.send(403, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }
    String enabled = server.arg("enabled");
    autoUpdateEnabled = (enabled == "1");
    preferences.begin("gsv-rfid", false);
    preferences.putBool("autoUpdate", autoUpdateEnabled);
    preferences.end();
    server.send(200, "application/json", "{\"success\":true}");
  });

  server.on("/check_version", []() {
    if (!isAuth()) {
      server.send(403, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }
    FirmwareDetails details = fetchOnlineFirmwareDetails();
    if (details.success) {
      preferences.begin("gsv-rfid", true);
      String prevVer = preferences.getString("ota_prev_ver", "None");
      preferences.end();
      
      String json = "{\"success\":true,\"local\":\"" + String(FIRMWARE_VERSION) + 
                    "\",\"previous\":\"" + prevVer + 
                    "\",\"latest\":\"" + details.latestVersion + 
                    "\",\"updateAvailable\":" + String(details.updateAvailable ? "true" : "false") + 
                    ",\"versions\":" + (details.rawVersionsJson != "" ? details.rawVersionsJson : "[]") + "}";
      server.send(200, "application/json", json);
    } else {
      server.send(500, "application/json", "{\"success\":false,\"message\":\"Failed to check version\"}");
    }
  });

  server.on("/trigger_update", []() {
    if (!isAuth()) {
      server.send(403, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }
    
    String customUrl = server.arg("url");
    String customVer = server.arg("version");
    
    if (customUrl != "") {
      server.send(200, "application/json", "{\"success\":true,\"message\":\"Starting custom OTA update\"}");
      otaTargetVerVal = customVer;
      startOtaUpdateTask(customUrl);
    } else {
      FirmwareDetails details = fetchOnlineFirmwareDetails();
      if (details.success && details.updateAvailable) {
        server.send(200, "application/json", "{\"success\":true,\"message\":\"Starting OTA update\"}");
        otaTargetVerVal = details.latestVersion;
        startOtaUpdateTask(details.downloadUrl);
      } else {
        server.send(400, "application/json", "{\"success\":false,\"message\":\"No update available or check failed\"}");
      }
    }
  });

  server.on("/ota_status", []() {
    if (!isAuth()) {
      server.send(403, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }
    
    String json = "{\"running\":" + String(otaRunning ? "true" : "false") + 
                  ",\"progress\":" + String(otaProgress) + 
                  ",\"status\":\"" + otaStatusStr + 
                  "\",\"error\":\"" + otaErrorMsg + "\"}";
    server.send(200, "application/json", json);
  });

  server.on("/save_ota", []() {
    if (!isAuth()) {
      server.send(403, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }
    String source = server.arg("source");
    String link = server.arg("link");
    String id = server.arg("id");

    if (source != "gdrive" && source != "github") {
      server.send(400, "application/json", "{\"success\":false,\"message\":\"Invalid source\"}");
      return;
    }

    otaSource = source;
    otaLink = link;
    otaId = id;

    preferences.begin("gsv-rfid", false);
    preferences.putString("otaSource", otaSource);
    preferences.putString("otaLink", otaLink);
    preferences.putString("otaId", otaId);
    preferences.end();

    server.send(200, "application/json", "{\"success\":true,\"message\":\"OTA configuration saved successfully\"}");
  });

  server.on("/test_ota", []() {
    if (!isAuth()) {
      server.send(403, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }
    
    String testSource = server.arg("source");
    if (testSource == "") testSource = otaSource;
    
    String testLink = server.arg("link");
    if (testLink == "") testLink = otaLink;
    
    String testId = server.arg("id");
    if (testId == "") testId = otaId;
    
    String url = "";
    if (testSource == "github") {
      url = testLink;
    } else {
      String activeScriptId = (testId != "") ? testId : scriptId;
      url = "https://script.google.com/macros/s/" + activeScriptId + "/exec?action=get_firmware_version";
    }

    if (url == "") {
      server.send(400, "application/json", "{\"success\":false,\"message\":\"Missing URL/Script ID for test\"}");
      return;
    }

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    
    bool testSuccess = false;
    int code = -1;
    String errorMessage = "";
    String bodyContent = "";

    if (http.begin(client, url)) {
      http.setTimeout(8000);
      code = http.GET();
      if (code == 200 || code == 302) {
        testSuccess = true;
        bodyContent = http.getString();
      } else {
        errorMessage = "HTTP Error Code: " + String(code);
      }
      http.end();
      client.stop();
    } else {
      errorMessage = "Failed to begin HTTP request";
    }

    String json = "{\"success\":" + String(testSuccess ? "true" : "false") + 
                  ",\"code\":" + String(code) + 
                  ",\"message\":\"" + (testSuccess ? "Connection successful" : errorMessage) + "\"" +
                  ",\"response\":" + (bodyContent != "" ? "true" : "false") + "}";
    server.send(200, "application/json", json);
  });

  server.onNotFound([]() {
    if (currentState == SYSTEM_WIFI_CONFIG) {
      server.sendHeader("Location",
                        "http://" + WiFi.softAPIP().toString() + "/", true);
      server.send(302, "text/plain", "");
    } else {
      server.send(404, "text/plain", "Not Found");
    }
  });
  server.begin();
  serverStarted = true;
}

const char AP_STYLE[] PROGMEM = R"rawliteral(:root{--v-orange:#f39c12;--v-brown:#8B4513;--v-green:#2ecc71;--bg-dark:#0d0d0d;--card-bg:#1a1a1a;--border-color:#333;}*{margin:0;padding:0;box-sizing:border-box;font-family:'Space Grotesk',sans-serif;}body{background:var(--bg-dark);color:#eee;min-height:100vh;display:flex;align-items:center;justify-content:center;padding:20px;}.container{width:100%;max-width:500px;margin:0 auto;}.login-card, .v-card{background:var(--card-bg);border:2px solid var(--border-color);border-radius:24px;padding:2rem;margin-bottom:1.5rem;text-align:center;}.login-card{border-color:var(--v-orange);max-width:420px;box-shadow:0 0 40px rgba(243,156,18,0.15);}.v-card{border-left:6px solid var(--v-brown);text-align:left;}.header{text-align:center;margin-bottom:20px;}.v-shield-logo{font-size:60px;color:var(--v-orange);margin:0 auto 10px;}.card-title{color:var(--v-orange);font-weight:800;text-transform:uppercase;margin-bottom:1.5rem;display:flex;align-items:center;gap:10px;font-size:0.9rem;}.input-label{display:block;font-size:0.75rem;color:#888;margin-bottom:0.5rem;text-transform:uppercase;}.input-group-v{position:relative;margin-bottom:1.2rem;}.v-input{width:100%;background:#0a0a0a;border:2px solid var(--border-color);padding:0.8rem 1rem;border-radius:12px;color:white;outline:none;}.v-input:focus{border-color:var(--v-orange);}.btn-v{width:100%;padding:0.8rem 1rem;border-radius:12px;font-weight:800;text-transform:uppercase;border:none;cursor:pointer;transition:0.3s;font-size:0.85rem;background:linear-gradient(135deg, var(--v-brown), var(--v-orange));color:white;}.btn-v-blue{background:#2980b9;margin-bottom:1rem;}.wifi-list{max-height:180px;overflow-y:auto;background:#0a0a0a;border-radius:12px;margin-bottom:1.5rem;display:none;border:1px solid #333;}.wifi-item{padding:10px 15px;cursor:pointer;border-bottom:1px solid #222;display:flex;justify-content:space-between;font-size:0.85rem;}.wifi-item:hover{background:#111;color:var(--v-orange);}.error-msg{color:#ff4d4d;font-size:0.8rem;margin-top:1rem;display:block;}.mac-info{font-size:0.7rem;color:#555;text-align:center;font-family:monospace;margin-top:15px;}.eye-icon{position:absolute;right:15px;top:50%;transform:translateY(-50%);color:#555;cursor:pointer;})rawliteral";

void serveAPLoginPage() {
  String html = String(R"rawliteral(<!DOCTYPE html><html lang="en"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1.0"><title>V-VARMA | Secure Access</title><link href="https://fonts.googleapis.com/css2?family=Space+Grotesk:wght@300;400;600;700&display=swap" rel="stylesheet"><link rel="stylesheet" href="https://cdnjs.cloudflare.com/ajax/libs/font-awesome/6.4.0/css/all.min.css"><style>)rawliteral") +
                AP_STYLE +
                R"rawliteral(</style></head><body><div class="login-card"><div class="v-shield-logo"><i class="fa-solid fa-shield-halved"></i></div><h1 style="letter-spacing: 4px;font-weight: 800;margin-bottom: 0.5rem;">V-VARMA</h1><div class="login-subtext">Configuration Access Mode</div><div class="input-group-v"><input type="text" id="login-user" class="v-input" value="admin" placeholder="Admin Username" required></div><div class="input-group-v"><input type="password" id="login-pass" class="v-input" placeholder="Security Key" required><i class="fa-solid fa-eye eye-icon" onclick="togglePass('login-pass', this)" style="position: absolute;right: 15px;top: 50%;transform: translateY(-50%);color: #555;cursor: pointer;"></i></div><div class="input-group-v" style="display: flex;align-items: center;gap: 10px;margin-top: -0.5rem;margin-bottom: 1.5rem;"><input type="checkbox" id="remember" style="width: 18px;height: 18px;cursor: pointer;accent-color: var(--v-orange);"><label for="remember" style="font-size: 0.85rem;color: #888;cursor: pointer;user-select: none;">Stay logged in (365 days)</label></div><button onclick="doLogin()" class="btn-v">Unlock System</button><div id="login-err" class="error-msg" style="display:none;">Verification Failed: Invalid Key</div><script>function togglePass(id, el){const inp = document.getElementById(id);if (inp.type === 'password'){inp.type = 'text';el.classList.replace('fa-eye', 'fa-eye-slash');}else{inp.type = 'password';el.classList.replace('fa-eye-slash', 'fa-eye');}}async function doLogin(){const u = document.getElementById('login-user').value;const p = document.getElementById('login-pass').value;const r = document.getElementById('remember').checked ? 1 : 0;const err = document.getElementById('login-err');err.style.display = 'none';try{const res = await fetch(`/login?u=${u}&p=${p}&r=${r}`);const data = await res.json();if (data.success){location.href = '/';}else{err.style.display = 'block';}}catch(e){err.innerText = "Connection Error";err.style.display = 'block';}}</script></div></body></html>)rawliteral";
  server.send(200, "text/html", html);
}

void serveConfigPage() {
  String html = String(R"rawliteral(<!DOCTYPE html><html><head><meta name="viewport" content="width=device-width, initial-scale=1.0"><title>V-VARMA | System Configuration</title><link href="https://fonts.googleapis.com/css2?family=Space+Grotesk:wght@300;400;600;700&display=swap" rel="stylesheet"><link rel="stylesheet" href="https://cdnjs.cloudflare.com/ajax/libs/font-awesome/6.4.0/css/all.min.css"><style>)rawliteral") +
                AP_STYLE +
                R"rawliteral(</style></head><body><div class="container"><div class="header"><div class="v-shield-logo"><i class="fa-solid fa-shield-halved"></i></div><h2 style="letter-spacing: 2px;">V-VARMA</h2><div class="mac-info">MAC:)rawliteral" +
                myMac + R"rawliteral(</div></div><div class="v-card"><div class="card-title"><i class="fas fa-wifi"></i>AP Mode - WiFi Credential Integration</div><label class="input-label">Available SSIDs</label><button class="btn-v btn-v-blue" onclick="scanWiFi()">Scan for Networks</button><div id="wifiList" class="wifi-list"></div><label class="input-label">SSID</label><input type="text" id="ssid" class="v-input" placeholder="Select or type SSID"><label class="input-label">Security Key</label><div class="input-group-v"><input type="password" id="pass" class="v-input" placeholder="WiFi Password"><i class="fa-solid fa-eye eye-icon" onclick="togglePass('pass', this)"></i></div><div class="card-title" style="margin-top: 1rem;"><i class="fas fa-lock"></i>Admin Credentials</div><label class="input-label">Admin Name</label><input type="text" id="devUser" class="v-input" value=")rawliteral" +
                deviceUser + R"rawliteral("><label class="input-label">Admin Password</label><div class="input-group-v"><input type="password" id="devPass" class="v-input" value=")rawliteral" +
                devicePass + R"rawliteral("><i class="fa-solid fa-eye eye-icon" onclick="togglePass('devPass', this)"></i></div><div class="card-title" style="margin-top: 1rem;"><i class="fas fa-link"></i>Cloud Sync</div><label class="input-label">Google Script ID</label><input type="text" id="scriptId" class="v-input" placeholder="AKfycb..." value=")rawliteral" +
                scriptId + R"rawliteral("><button class="btn-v" onclick="save()">Initialize & Reboot</button><div id="status-msg"></div><div class="mac-info" style="margin-top: 1.5rem;border-top: 1px solid #333;padding-top: 1rem;">DEVICE MAC: <span style="color:var(--v-orange)">)rawliteral" +
                myMac + R"rawliteral(</span></div></div></div><script>function togglePass(id, el){const inp = document.getElementById(id);if (inp.type === 'password'){inp.type = 'text';el.classList.replace('fa-eye', 'fa-eye-slash');}else{inp.type = 'password';el.classList.replace('fa-eye-slash', 'fa-eye');}}async function scanWiFi(){const list = document.getElementById('wifiList');list.style.display = 'block';list.innerHTML = '<div style="padding:15px;color:#888;">Scanning...</div>';try{const res = await fetch('/scan_wifi');const nets = await res.json();list.innerHTML = nets.map(n =>`<div class="wifi-item" onclick="select('${n.ssid}')"><span>${n.ssid}</span><span>${n.rssi}dBm</span></div>`).join('');}catch(e){list.innerHTML = '<div style="padding:15px;color:red;">Scan failed</div>';}}function select(s){document.getElementById('ssid').value = s;document.getElementById('wifiList').style.display = 'none';}async function save(){const s = document.getElementById('ssid').value;const p = document.getElementById('pass').value;const sid = document.getElementById('scriptId').value;const u = document.getElementById('devUser').value;const pw = document.getElementById('devPass').value;const msg = document.getElementById('status-msg');msg.innerHTML = "Syncing with hardware...";const data = new URLSearchParams();data.append('ssid', s);data.append('pass', p);data.append('scriptId', sid);data.append('adminUser', u);data.append('adminPass', pw);try{const res = await fetch('/save_net',{method: 'POST', body: data});const resSys = await fetch('/save_sys?script=' + encodeURIComponent(sid) + '&adminUser=' + encodeURIComponent(u) + '&adminPass=' + encodeURIComponent(pw));if(res.ok && resSys.ok){msg.innerHTML = "<span style='color:var(--v-green)'>SUCCESS! Rebooting system...</span>";setTimeout(() =>location.reload(), 5000);}else msg.innerHTML = "<span style='color:red'>Hardware Reject</span>";}catch(e){msg.innerHTML = "<span style='color:red'>Network Error</span>";}}</script></body></html>)rawliteral";
  server.send(200, "text/html", html);
}

void handleLogin() {
  String u = server.hasArg("u") ? server.arg("u") : server.arg("user");
  String p = server.hasArg("p") ? server.arg("p") : server.arg("pass");
  String r = server.arg("r"); // remember checkbox

  bool authenticated = false;
  // 1. Check against saved credentials
  if (u == deviceUser && p == devicePass) {
    authenticated = true;
  }
  // 2. Fallback for defaults (prevent lockout)
  if (!authenticated && u == "admin" &&
      (p == "password123" || p == "GSVRF001")) {
    authenticated = true;
  }

  if (authenticated) {
    String cookie = "VARMA_SID_V4_SECURE=GSV_OS_TRUSTED; path=/";
    if (r == "1") {
      cookie += "; Max-Age=31536000"; // 1 Year persistence
    }
    server.sendHeader("Set-Cookie", cookie);
    server.send(200, "application/json", "{\"success\":true}");
  } else {
    server.send(200, "application/json", "{\"success\":false}");
  }
}

void handleDashboard() {
  bool authed = isAuth();
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");

  server.sendContent(R"rawliteral(<!DOCTYPE html><html lang="en"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1.0"><title>V-VARMA RFID | GSV ELECTRICAL ENTERPRISES )rawliteral");
  server.sendContent(FIRMWARE_VERSION);
  server.sendContent(R"rawliteral(</title><link href="https://fonts.googleapis.com/css2?family=Space+Grotesk:wght@400;600;700&display=swap" rel="stylesheet"><link rel="stylesheet" href="https://cdnjs.cloudflare.com/ajax/libs/font-awesome/6.4.0/css/all.min.css"><link href="https://cdn.jsdelivr.net/npm/bootstrap@5.3.0/dist/css/bootstrap.min.css" rel="stylesheet"><style>:root{--v-orange:#f39c12;--v-brown:#8B4513;--v-red:#ff4d4d;--v-green:#2ecc71;--v-blue:#1a2a6c;--bg-dark:#0d0d0d;--card-bg:#1a1a1a;--border-color:#333;--header-gradient:linear-gradient(135deg,#1a2a6c,#b21f1f,#fdbb2d);}*{margin:0;padding:0;box-sizing:border-box;font-family:'Space Grotesk',sans-serif;}body{background:var(--bg-dark);color:#eee;min-height:100vh;overflow-x:hidden;}#login-overlay{position:fixed;inset:0;background:rgba(0,0,0,0.98);z-index:9999;display:flex;align-items:center;justify-content:center;backdrop-filter:blur(20px);}.login-card{background:var(--card-bg);border:2px solid var(--v-orange);border-radius:28px;padding:3rem;width:90%;max-width:420px;text-align:center;box-shadow:0 0 60px rgba(243,156,18,0.2);}.v-shield-logo-big{width:140px;height:140px;color:var(--v-orange);margin:0 auto 1rem auto;filter:drop-shadow(0 0 15px rgba(243,156,18,0.5));animation:shieldPulse 2s infinite ease-in-out;}.login-subtext{font-size:1rem;color:#888;text-transform:lowercase;margin-bottom:0.2rem;}@keyframes shieldPulse{0%,100%{transform:scale(1);}50%{transform:scale(1.05);}}.v-header{background:var(--header-gradient);padding:1.5rem 0;box-shadow:0 10px 30px rgba(0,0,0,0.5);position:sticky;top:0;z-index:1000;}.header-title{font-size:1.4rem;font-weight:800;display:flex;align-items:center;justify-content:center;gap:20px;}.header-logo{width:75px;height:75px;color:#fff;filter:drop-shadow(0 0 10px rgba(255,255,255,0.3));}.nav-links-container{background:rgba(0,0,0,0.3);border-radius:50px;padding:5px;display:inline-flex;gap:5px;margin-top:1.2rem;border:1px solid rgba(255,255,255,0.1);flex-wrap:wrap;justify-content:center;}.nav-tab{padding:0.5rem 1.3rem;border-radius:30px;cursor:pointer;transition:0.3s;font-weight:700;font-size:0.8rem;color:rgba(255,255,255,0.75);}.nav-tab.active{background:#fff;color:var(--v-blue);box-shadow:0 4px 15px rgba(0,0,0,0.2);}.status-container{position:absolute;top:1.5rem;right:2rem;display:flex;gap:1.2rem;align-items:center;}.stat-icon{font-size:1.2rem;transition:0.3s;}.stat-online{color:var(--v-green);filter:drop-shadow(0 0 5px var(--v-green));}.stat-offline{color:var(--v-red);filter:drop-shadow(0 0 5px var(--v-red));}.pulse{animation:p 2s infinite;}.container-custom{max-width:1100px;margin:2rem auto;padding:0 1.2rem;}.tab-content{display:none;animation:slideUp 0.4s ease;}.tab-content.active{display:block;}.v-card{background:var(--card-bg);border:2px solid var(--border-color);border-radius:24px;padding:1.6rem;margin-bottom:1.5rem;border-left:6px solid var(--v-brown);}.card-title{color:var(--v-orange);font-weight:800;text-transform:uppercase;letter-spacing:1.2px;margin-bottom:1.2rem;display:flex;align-items:center;gap:10px;font-size:0.95rem;}.live-monitor{background:linear-gradient(180deg,#0f0f0f,#000);border-radius:25px;padding:2.2rem 1.5rem;text-align:center;border:1px solid #333;margin-bottom:2rem;box-shadow:0 20px 50px rgba(0,0,0,0.8);}.status-badge{font-size:3rem;font-weight:900;line-height:1;color:var(--v-orange);text-shadow:0 0 25px rgba(243,156,18,0.4);margin-bottom:1rem;letter-spacing:4px;}.student-name{font-size:1.7rem;font-weight:600;color:#ccc;margin-bottom:0.6rem;text-transform:uppercase;}.uid-text{font-family:'Inter',sans-serif;font-size:3.5rem;font-weight:900;color:#fff;letter-spacing:2px;line-height:1.1;margin-bottom:0.8rem;}.btn-v{padding:0.75rem 1.3rem;border-radius:12px;font-weight:800;text-transform:uppercase;transition:0.3s;border:none;display:inline-flex;align-items:center;justify-content:center;gap:8px;font-size:0.82rem;}.btn-v-orange{background:linear-gradient(135deg,var(--v-brown),var(--v-orange));color:#fff;}.btn-v-green{background:var(--v-green);color:#fff;}.btn-v-red{background:var(--v-red);color:#fff;}.btn-v-outline{background:transparent;border:2px solid var(--border-color);color:#fff;}.btn-v:active{transform:scale(0.95);}#serial-monitor{background:#000;border:1px solid #333;border-radius:12px;margin-top:1rem;display:none;overflow:hidden;}.serial-header{background:#1a1a1a;padding:8px 15px;font-size:0.75rem;color:#666;display:flex;justify-content:space-between;align-items:center;border-bottom:1px solid #333;}.serial-output{height:150px;overflow-y:auto;padding:10px;font-family:monospace;font-size:0.75rem;color:#0f0;line-height:1.4;}.input-group-v{position:relative;margin-bottom:1rem;}.v-input{width:100%;background:#0a0a0a;border:2px solid var(--border-color);padding:0.9rem;border-radius:12px;color:#fff;outline:none;transition:0.3s;}.v-input:focus{border-color:var(--v-orange);}.eye-icon{position:absolute;right:15px;top:50%;transform:translateY(-50%);color:#555;cursor:pointer;z-index:10;}.eye-icon:hover{color:var(--v-orange);}@keyframes p{0%,100%{opacity:1;}50%{opacity:0.4;}}@keyframes slideUp{from{opacity:0;transform:translateY(16px);}to{opacity:1;transform:translateY(0);}}.toast-v{position:fixed;bottom:30px;right:30px;background:var(--card-bg);border-left:5px solid var(--v-orange);padding:1rem 1.8rem;border-radius:15px;box-shadow:0 10px 40px rgba(0,0,0,0.6);display:none;z-index:10001;animation:slideUp 0.3s ease;}.inv-history-container{max-height:420px;overflow-y:auto;padding-right:5px;}.inv-history-container::-webkit-scrollbar{width:6px;}.inv-history-container::-webkit-scrollbar-thumb{background:var(--v-brown);border-radius:10px;}</style></head><body><div id="login-overlay" style="display:)rawliteral");
  server.sendContent(authed ? "none" : "flex");
  server.sendContent(R"rawliteral(;"><div class="login-card"><div class="v-shield-logo-big" style="display:flex;align-items:center;justify-content:center;"><i class="fa-solid fa-shield-halved" style="font-size:100px;color:var(--v-orange);"></i></div><h1 class="fw-bold text-white mb-1" style="letter-spacing:4px;">V-VARMA</h1><div class="login-subtext">secure login for dash board</div><div class="input-group-v"><input type="text" class="v-input" id="admin-user" value="admin" placeholder="Username"></div><div class="input-group-v"><input type="password" class="v-input" id="admin-pass" placeholder="Authorization Key"><i class="fa-solid fa-eye eye-icon" onclick="togglePass('admin-pass',this)"></i></div><div class="input-group-v" style="display:flex;align-items:center;gap:10px;margin-top:-0.5rem;margin-bottom:1.5rem;text-align:left;"><input type="checkbox" id="admin-remember" style="width:18px;height:18px;cursor:pointer;accent-color:var(--v-orange);"><label for="admin-remember" style="font-size:0.85rem;color:#888;cursor:pointer;">Stay logged in (365 days)</label></div><button class="btn-v btn-v-orange w-100 mt-2" onclick="handleLogin()">GRANT SYSTEM ACCESS</button></div></div><div id="app-ui" style="display:)rawliteral");
  server.sendContent(authed ? "block" : "none");
  server.sendContent(R"rawliteral(;"><header class="v-header text-center"><div class="container"><div class="header-title text-white"><div class="header-logo" style="display:flex;align-items:center;justify-content:center;"><i class="fa-solid fa-shield-halved" style="font-size:50px;color:#fff;"></i></div><div>V-VARMA RFID Attendance Management System</div></div><p class="text-white-50 small mt-1" id="header-version-text">GSV Electrical Enterprises | )rawliteral");
  server.sendContent(FIRMWARE_VERSION);
  server.sendContent(R"rawliteral(</p><div class="nav-links-container"><div class="nav-tab active" onclick="switchTab('overview',this)">OVERVIEW</div><div class="nav-tab" onclick="switchTab('attendance',this)">ATTENDANCE</div><div class="nav-tab" onclick="switchTab('inventory',this)">RFID INVENTORY</div><div class="nav-tab" onclick="switchTab('verify',this)">VERIFY RFID</div><div class="nav-tab" onclick="switchTab('settings',this)">CONFIGURATION</div></div></div><div class="status-container"><i class="fa-solid fa-terminal stat-icon" onclick="toggleSerial()" title="Serial Monitor" style="color:#888;cursor:pointer;"></i><i class="fa-solid fa-microchip stat-icon stat-offline pulse" id="stat-mcu" title="Controller"></i><i class="fa-solid fa-network-wired stat-icon stat-offline pulse" id="stat-net" title="Network"></i><i class="fa-solid fa-database stat-icon stat-offline pulse" id="stat-db" title="Database"></i><i class="fa-solid fa-wifi stat-icon stat-offline pulse" id="stat-wifi" title="WiFi"></i></div></header><div class="container-custom"><div id="serial-monitor"><div class="serial-header"><span><i class="fa-solid fa-terminal me-2"></i>HARDWARE SERIAL DATA FLOW</span><button class="btn btn-sm btn-outline-secondary py-0 px-2" style="font-size:0.6rem;" onclick="clearSerial()">CLEAR</button></div><div id="serial-out" class="serial-output"></div></div><div id="overview" class="tab-content active"><div class="live-monitor"><div class="status-badge" id="live-status">SCAN READY</div><div class="student-name" id="live-name">---</div><div class="uid-text" id="live-uid">0000000000</div><div id="live-status-msg" style="color:var(--v-orange);font-weight:900;margin-top:14px;font-size:1.3rem;text-transform:uppercase;text-shadow:0 0 15px rgba(243,156,18,0.5);"></div><div id="live-cand-box" style="display:none;margin:15px auto 0 auto;max-width:520px;background:rgba(255,255,255,0.06);padding:14px 18px;border-radius:16px;border:1px solid #444;"><div class="d-flex align-items-center justify-content:center gap-3"><img id="live-photo" referrerpolicy="no-referrer" src="" alt="Candidate" style="width:76px;height:76px;border-radius:50%;object-fit:cover;border:3px solid var(--v-orange);display:none;background:#222;"><div class="text-start"><div id="live-college" class="text-warning small fw-bold"></div><div id="live-meta" class="small text-white-50"></div></div></div></div></div><div class="row"><div class="col-md-6"><div class="v-card"><div class="card-title"><i class="fa-solid fa-broadcast-tower"></i>DEVICE INFO</div><div class="d-flex justify-content-between py-2 border-bottom border-dark"><span>Identity</span><span class="text-white" id="disp-dev-name">--</span></div><div class="d-flex justify-content-between py-2 border-bottom border-dark"><span>Assigned IP</span><span class="text-white" id="disp-ip">--</span></div><div class="d-flex justify-content-between py-2 border-bottom border-dark"><span>MAC ID</span><span class="text-white small" id="disp-mac">--</span></div></div></div><div class="col-md-6"><div class="v-card"><div class="card-title"><i class="fa-solid fa-cloud"></i>CLOUD STATUS</div><div class="d-flex justify-content-between py-2 border-bottom border-dark"><span>Sync Engine</span><span class="text-white" id="disp-script">--</span></div><div class="d-flex justify-content-between py-2 border-bottom border-dark"><span>Signal</span><span class="text-white" id="disp-rssi">--</span></div></div></div></div></div><div id="attendance" class="tab-content"><div class="v-card"><div class="card-title"><i class="fa-solid fa-sliders"></i>MODE CONTROL</div><div class="d-flex gap-2 mb-4 flex-wrap"><button class="btn-v btn-v-green" onclick="setMode('IN')"><i class="fa-solid fa-right-to-bracket"></i>SET IN</button><button class="btn-v btn-v-red" onclick="setMode('OUT')"><i class="fa-solid fa-right-from-bracket"></i>SET OUT</button><button class="btn-v btn-v-orange" onclick="setMode('INV')"><i class="fa-solid fa-boxes-stacked"></i>SET INV</button><button class="btn-v btn-v-outline" onclick="resetUI(this)"><i class="fa-solid fa-refresh"></i>CLEAR MONITOR</button></div><div class="card-title mt-4 d-flex justify-content-between align-items-center"><span><i class="fa-solid fa-clock-rotate-left"></i>LOG HISTORY</span><button class="btn-v btn-v-outline py-1" onclick="fetchLogs(this)"><i class="fa-solid fa-rotate"></i>RELOAD</button></div><div id="log-loading-bar" style="display:none;" class="alert alert-warning py-2 px-3 small mb-2"><i class="fa-solid fa-spinner fa-spin me-2"></i>Loading attendance history...</div><div id="log-list"></div></div></div><div id="inventory" class="tab-content"><div class="v-card text-center" style="min-height:230px;"><div class="card-title justify-content-center"><i class="fa-solid fa-id-card"></i>TAG REGISTRATION</div><div class="d-flex justify-content-center gap-3 mb-4 flex-wrap"><button class="btn-v btn-v-orange" id="btn-capture" onclick="startCaptureFlow()"><i class="fa-solid fa-crosshairs"></i>START CAPTURE</button><button class="btn-v btn-v-outline" onclick="resetCapture()"><i class="fa-solid fa-eraser"></i>REFRESH</button><button class="btn-v btn-v-green" id="cloud-save-btn" style="display:none;" onclick="pushToCloud()"><i class="fa-solid fa-cloud-arrow-up"></i>SAVE TO CLOUD</button></div><div id="capture-uid" class="status-badge" style="font-size:2.8rem;letter-spacing:5px;color:var(--v-blue);">--------</div><div id="capture-msg" class="text-white-50 small mt-2">Ready for card registration</div></div><div class="v-card"><div class="card-title d-flex justify-content-between align-items-center"><span><i class="fa-solid fa-history"></i>INVENTORY DATABASE</span><button class="btn-v btn-v-outline py-1" onclick="fetchInventory(this)"><i class="fa-solid fa-rotate"></i>RELOAD</button></div><div id="inv-loading-bar" style="display:none;" class="alert alert-warning py-2 px-3 small mb-2"><i class="fa-solid fa-spinner fa-spin me-2"></i>Loading RFID inventory...</div><div id="inv-history" class="inv-history-container"></div></div></div><div id="settings" class="tab-content"><div class="row"><div class="col-md-6"><div class="v-card"><div class="card-title"><i class="fa-solid fa-wifi"></i>NETWORK TOPOLOGY</div><label class="text-white-50 small mb-2">TARGET SSID</label><div class="input-group-v"><input type="text" class="v-input" id="conf-ssid" placeholder="Select or type SSID"><button class="btn-v btn-v-outline" onclick="scanWiFi(this)" style="padding:0 15px;"><i class="fa-solid fa-magnifying-glass"></i>SCAN</button></div><div id="wifi-scan-results" style="max-height:150px;overflow-y:auto;background:#000;border-radius:8px;margin-bottom:1rem;display:none;border:1px solid #333;"></div><label class="text-white-50 small mb-2">SECURITY KEY</label><div class="input-group-v"><input type="password" class="v-input" id="conf-pass"><i class="fa-solid fa-eye eye-icon" onclick="togglePass('conf-pass',this)"></i></div><button class="btn-v btn-v-orange w-100" id="btn-save-net" onclick="saveNetwork()"><i class="fa-solid fa-save me-2"></i>SAVE & CONNECT</button></div></div><div class="col-md-6"><div class="v-card"><div class="card-title"><i class="fa-solid fa-gears"></i>SYSTEM CONFIG</div><label class="text-white-50 small mb-2">GOOGLE SCRIPT ID</label><input type="text" class="v-input mb-3" id="conf-script"><label class="text-white-50 small mb-2">ADMIN NAME</label><input type="text" class="v-input mb-3" id="conf-admin-user"><label class="text-white-50 small mb-2">ADMIN PASSWORD</label><div class="input-group-v mb-3"><input type="password" class="v-input" id="conf-admin-pass"><i class="fa-solid fa-eye eye-icon" onclick="togglePass('conf-admin-pass',this)"></i></div><button class="btn-v btn-v-green w-100" id="btn-save-sys" onclick="saveSystem()"><i class="fa-solid fa-save me-2"></i>SAVE & REBOOT</button></div></div></div><div class="v-card mb-4"><div class="card-title text-info"><i class="fa-solid fa-cloud-arrow-up"></i>SYSTEM FIRMWARE & OTA UPGRADE</div><div class="row"><div class="col-md-6 mb-3 mb-md-0"><div class="d-flex align-items-center mb-2"><span class="text-white-50 me-2">Local Version:</span><span class="fw-bold text-success" id="firmware-local-version">)rawliteral");
  server.sendContent(FIRMWARE_VERSION);
  server.sendContent(R"rawliteral(</span></div><div class="d-flex align-items-center mb-2"><span class="text-white-50 me-2">Previous Version:</span><span class="fw-bold text-white-50" id="firmware-previous-version">None</span></div><div class="d-flex align-items-center mb-3"><span class="text-white-50 me-2">Latest Online:</span><span class="fw-bold text-warning" id="firmware-online-version">Checking...</span></div><div class="mb-3 d-none" id="version-select-container"><label class="text-white-50 small mb-2 d-block">Select Version to Deploy</label><select class="form-select bg-dark text-white border-secondary mb-3" id="firmware-version-select" onchange="onVersionSelectChange()"></select></div><div class="form-check form-switch mb-3"><input class="form-check-input" type="checkbox" id="ota-auto-update" checked onclick="toggleAutoUpdate(this)"><label class="form-check-label text-white-50 small" for="ota-auto-update">Enable Automatic Updates (Checks on Boot)</label></div></div><div class="col-md-6 text-md-end d-flex flex-column justify-content-end align-items-md-end"><div><button class="btn-v btn-v-outline me-2 mb-2" onclick="checkFirmwareVersion()"><i class="fa-solid fa-arrows-rotate me-2"></i>CHECK FOR UPDATE</button><button class="btn-v btn-v-green mb-2" id="btn-trigger-update" onclick="triggerOTAUpdate()" disabled><i class="fa-solid fa-circle-down me-2"></i>INSTALL UPDATE</button></div></div></div><div id="ota-progress-container" class="mt-3 d-none"><div class="progress bg-dark" style="height:10px;border:1px solid #333;border-radius:5px;"><div id="ota-progress-bar" class="progress-bar bg-success progress-bar-striped progress-bar-animated" role="progressbar" style="width:0%"></div></div><div class="text-white-50 small mt-1" id="ota-status-text">Downloading...</div></div><div class="border-top border-dark pt-3 mt-3"><div class="card-title text-info small mb-3" style="font-size:1rem;"><i class="fa-solid fa-gear"></i>OTA Update Configuration</div><div class="row"><div class="col-md-12 mb-3"><label class="text-white-50 small mb-2 d-block">Update Source</label><div class="form-check form-check-inline me-3 d-inline-block"><input class="form-check-input" type="radio" name="ota-source" id="ota-source-gdrive" value="gdrive" onclick="toggleOtaFields()"><label class="form-check-label text-white-50" for="ota-source-gdrive">Google Drive (Apps Script)</label></div><div class="form-check form-check-inline d-inline-block"><input class="form-check-input" type="radio" name="ota-source" id="ota-source-github" value="github" checked onclick="toggleOtaFields()"><label class="form-check-label text-white-50" for="ota-source-github">GitHub Release (JSON Link)</label></div></div><div class="col-md-6 mb-3" id="ota-id-group" style="display:none;"><label class="text-white-50 small mb-1">Apps Script ID (Optional fallback to default)</label><input type="text" class="form-control bg-dark text-white border-secondary" id="ota-script-id" placeholder="Enter custom Script ID"></div><div class="col-md-6 mb-3" id="ota-link-group"><label class="text-white-50 small mb-1">GitHub Version JSON Link</label><input type="text" class="form-control bg-dark text-white border-secondary" id="ota-github-link" placeholder="https://raw.githubusercontent.com/.../version.json"></div><div class="col-md-12 text-md-end mt-2"><button class="btn-v btn-v-outline me-2" onclick="testOtaConnection(this)"><i class="fa-solid fa-network-wired me-2"></i>TEST CONNECTION</button><button class="btn-v btn-v-green" onclick="saveOtaConfig(this)"><i class="fa-solid fa-save me-2"></i>SAVE CONFIG</button></div></div></div></div><div class="v-card"><div class="card-title text-danger"><i class="fa-solid fa-triangle-exclamation"></i>ADVANCED RECOVERY</div><p class="text-white-50 small mb-4">Factory reset wipes all local NVS memory (WiFi, Script ID). Device will reboot in AP mode.</p><button class="btn-v btn-v-red w-100" onclick="factoryReset()">WIPE DEVICE & RESET</button></div></div><div id="verify" class="tab-content"><div class="v-card"><div class="card-title"><i class="fa-solid fa-magnifying-glass"></i>INSTANT RFID PROFILER</div><div class="input-group-v mb-4"><input type="text" class="v-input mb-0" id="verify-search-input" placeholder="Enter Tag ID / QR / Barcode or Scan Card"><button class="btn-v btn-v-orange" id="btn-start-verify" onclick="startVerifyFlow()" title="Capture via Hardware"><i class="fa-solid fa-crosshairs"></i>CAPTURE</button><button class="btn-v btn-v-outline" onclick="performLookup()" title="Manual Search"><i class="fa-solid fa-search"></i></button><button class="btn-v btn-v-outline text-danger" onclick="resetVerifyUI()" title="Clear"><i class="fa-solid fa-trash-can"></i></button></div><div id="verify-loader" style="display:none;" class="text-center p-4"><div class="spinner-border text-warning" role="status"></div><div class="mt-2 small text-white-50">LOADING PROFILE...</div></div><div id="verify-results"></div></div></div></div></div><div id="toast" class="toast-v"><i class="fa-solid fa-circle-check me-2"></i><span id="toast-msg"></span></div><script>window.V_SCRIPT_ID=")rawliteral");
  server.sendContent(scriptId);
  server.sendContent(R"rawliteral(";window.V_MAC=")rawliteral");
  server.sendContent(myMac);
  server.sendContent(R"rawliteral(";window.V_FW=")rawliteral");
  server.sendContent(FIRMWARE_VERSION);
  server.sendContent(R"rawliteral(";let pollTimer=null;function togglePass(id,el){const i=document.getElementById(id);if(i.type==='password'){i.type='text';el.classList.replace('fa-eye','fa-eye-slash');}else{i.type='password';el.classList.replace('fa-eye-slash','fa-eye');}}function toggleSerial(){const s=document.getElementById('serial-monitor');s.style.display=s.style.display==='none'?'block':'none';}function logSerial(m){const o=document.getElementById('serial-out'),n=new Date().toLocaleTimeString('en-GB',{hour12:false}),l=document.createElement('div');l.style.fontSize='0.75rem';l.innerHTML=`<span style="color:#555;">[${n}]</span><span style="color:#0f0;">${m}</span>`;o.appendChild(l);o.scrollTop=o.scrollHeight;}function clearSerial(){document.getElementById('serial-out').innerHTML='';}
function getScriptId(){const el=document.getElementById('conf-script');return(el&&el.value)?el.value.trim():(window.V_SCRIPT_ID||localStorage.getItem('vv_sid')||'');}
function getMac(){const el=document.getElementById('disp-mac');const t=el?el.innerText.trim():'';return(t&&t!=='--')?t:(window.V_MAC||localStorage.getItem('vv_mac')||'');}
function getPhotoMap(){try{return JSON.parse(localStorage.getItem('vv_pmap')||'{}');}catch(e){return{};}}
function savePhotos(arr){if(!Array.isArray(arr))return;const m=getPhotoMap();let chg=false;arr.forEach(x=>{const p=x.photoUrl||x.assignedPhotoUrl||'';if(p&&p.length>5){const k1=(x.regId||x.reg_id||'').toUpperCase(),k2=(x.uid||x.tagId||'').toUpperCase();if(k1&&m[k1]!==p){m[k1]=p;chg=true;}if(k2&&m[k2]!==p){m[k2]=p;chg=true;}}});if(chg){try{localStorage.setItem('vv_pmap',JSON.stringify(m));}catch(e){}}}
function getLocalPhoto(id,uid){const m=getPhotoMap();const k1=(id||'').toUpperCase(),k2=(uid||'').toUpperCase();return m[k1]||m[k2]||'';}
function getB64Map(){try{return JSON.parse(localStorage.getItem('vv_b64')||'{}');}catch(e){return{};}}
function saveB64(key,val){if(!key||!val)return;const m=getB64Map();const keys=Object.keys(m);if(keys.length>=15)delete m[keys[0]];m[key]=val;try{localStorage.setItem('vv_b64',JSON.stringify(m));}catch(e){localStorage.removeItem('vv_b64');}}
async function fetchPhotoFallback(img,regId,uid,rawUrl){if(!img||img.dataset.fbDone)return;img.dataset.fbDone='1';const k=(regId||uid||'').toUpperCase();const bMap=getB64Map();if(k&&bMap[k]){img.src=bMap[k];img.style.display='block';return;}const sid=getScriptId();if(!sid){img.style.display='none';return;}try{const u=`https://script.google.com/macros/s/${encodeURIComponent(sid)}/exec?action=get_photo&regId=${encodeURIComponent(regId||uid||'')}&url=${encodeURIComponent(rawUrl||'')}`;const r=await fetch(u);const j=await r.json();if(j&&j.status==='success'&&j.dataUrl){saveB64(k,j.dataUrl);img.src=j.dataUrl;img.style.display='block';return;}}catch(e){}img.style.display='none';}
async function fetchAPI(ep,params={}){try{const u=new URL(ep,window.location.origin);Object.keys(params).forEach(k=>u.searchParams.append(k,params[k]));const c=new AbortController();const t=setTimeout(()=>c.abort(),12000);const r=await fetch(u,{signal:c.signal});clearTimeout(t);const tx=await r.text();try{return JSON.parse(tx);}catch(e){return tx;}}catch(err){return null;}}
async function fetchGasDirect(action,extra={}){const sid=getScriptId();if(!sid)return null;const mac=getMac();try{const u=new URL(`https://script.google.com/macros/s/${encodeURIComponent(sid)}/exec`);u.searchParams.set('action',action);if(mac)u.searchParams.set('mac',mac);Object.keys(extra).forEach(k=>u.searchParams.set(k,extra[k]));const c=new AbortController();const t=setTimeout(()=>c.abort(),9000);const r=await fetch(u.toString(),{signal:c.signal});clearTimeout(t);if(!r.ok)return null;return await r.json();}catch(e){return null;}}
let settingsInitialized=false;async function startPolling(){if(pollTimer)clearInterval(pollTimer);pollTimer=setInterval(async()=>{const data=await fetchAPI('/status');if(data&&typeof data==='object'){if(data.script_id){window.V_SCRIPT_ID=data.script_id;try{localStorage.setItem('vv_sid',data.script_id);}catch(e){}}if(data.mac){window.V_MAC=data.mac;try{localStorage.setItem('vv_mac',data.mac);}catch(e){}}updateDashboard(data);const pBar=document.getElementById('ota-progress-bar'),pCont=document.getElementById('ota-progress-container'),sText=document.getElementById('ota-status-text'),btn=document.getElementById('btn-trigger-update');if(data.ota_running){if(pCont)pCont.classList.remove('d-none');if(pBar)pBar.style.width=data.ota_progress+"%";if(btn)btn.disabled=true;if(sText)sText.innerText=data.ota_status==="updating"?`Downloading: ${data.ota_progress}%`:`${data.ota_status} (${data.ota_progress}%)`;}else{if(data.ota_status==="failed"){if(pCont)pCont.classList.remove('d-none');if(sText)sText.innerText=`Failed: ${data.ota_error}`;if(pBar)pBar.style.width="0%";if(btn)btn.disabled=false;}else if(data.ota_status==="rebooting"){if(pCont)pCont.classList.remove('d-none');if(pBar)pBar.style.width="100%";if(sText)sText.innerText="Rebooting device...";if(typeof showRebootOverlay==='function')showRebootOverlay();}else{if(sText&&!sText.innerText.startsWith("Failed:")&&pCont)pCont.classList.add('d-none');const sel=document.getElementById('firmware-version-select');if(btn&&sel&&sel.value!=="")btn.disabled=false;}}if(!settingsInitialized&&data.script_id){document.getElementById('conf-script').value=data.script_id;document.getElementById('conf-ssid').value=data.saved_ssid||'';document.getElementById('conf-admin-user').value=data.dev_user||'admin';document.getElementById('conf-admin-pass').value=data.dev_pass||'password123';if(data.auto_update!==undefined)document.getElementById('ota-auto-update').checked=data.auto_update;if(data.ota_source==='gdrive'){document.getElementById('ota-source-gdrive').checked=true;}else{document.getElementById('ota-source-github').checked=true;}document.getElementById('ota-script-id').value=data.ota_id||'';document.getElementById('ota-github-link').value=data.ota_link||'';if(typeof toggleOtaFields==='function')toggleOtaFields();settingsInitialized=true;}}const logs=await fetchAPI('/serial_data');if(logs&&typeof logs==='string'&&logs.trim()){logs.split('\n').forEach(line=>{if(line.trim())logSerial(line);});}},1000);}
let lastScanLookupKey='';
async function updateDashboard(d){document.getElementById('stat-mcu').className=`fa-solid fa-microchip stat-icon ${d.mcu?'stat-online':'stat-offline'}`;document.getElementById('stat-net').className=`fa-solid fa-network-wired stat-icon ${d.net?'stat-online':'stat-offline'}`;document.getElementById('stat-db').className=`fa-solid fa-database stat-icon ${d.db?'stat-online':'stat-offline'}`;document.getElementById('stat-wifi').className=`fa-solid fa-wifi stat-icon ${d.wifi?'stat-online':'stat-offline'}`;document.getElementById('disp-dev-name').innerText=d.name||'--';document.getElementById('disp-ip').innerText=d.ip||'0.0.0.0';document.getElementById('disp-mac').innerText=d.mac||'--';document.getElementById('disp-script').innerText=(d.script_id&&d.script_id.length>0)?'DEPLOYED':'NOT SET';document.getElementById('disp-rssi').innerText=(d.rssi||0)+' dBm';if(d.fw_version){const hv=document.getElementById('header-version-text');if(hv)hv.innerText='GSV Electrical Enterprises | '+d.fw_version;const lv=document.getElementById('firmware-local-version');if(lv)lv.innerText=d.fw_version;}const statusEl=document.getElementById('live-status'),nameEl=document.getElementById('live-name'),uidEl=document.getElementById('live-uid'),msgEl=document.getElementById('live-status-msg'),candBox=document.getElementById('live-cand-box'),photoEl=document.getElementById('live-photo'),collEl=document.getElementById('live-college'),metaEl=document.getElementById('live-meta');if(d.loading){statusEl.innerHTML='<i class="fa-solid fa-spinner fa-spin me-2"></i>VALIDATING';uidEl.innerText=d.last_uid;nameEl.innerText='CONTACTING CLOUD...';msgEl.innerText='';if(candBox)candBox.style.display='none';if(photoEl)photoEl.dataset.cur='';return;}if(d.last_uid&&d.last_uid!=="--------"&&d.last_uid!=="0"){statusEl.innerText=(d.process_step==='Ready'||d.process_step==='None')?d.mode:d.process_step.toUpperCase();uidEl.innerText=d.last_uid;nameEl.innerText=(d.last_user&&d.last_user!=='---')?d.last_user:'SCAN COMPLETED';msgEl.innerText=d.detailed_status||d.last_status_msg||"";}else{statusEl.innerText=d.mode;uidEl.innerText='WAITING FOR CARD...';nameEl.innerText='---';msgEl.innerText=d.last_status_msg||"";}const validUser=(d.last_uid&&d.last_uid!=="--------"&&d.last_user&&d.last_user!=='---'&&d.last_user!=='READY TO SCAN'&&!d.last_user.includes('ERROR')&&!d.last_user.includes('UNKNOWN')&&!d.last_user.includes('UNASSIGNED'));if(validUser){if(candBox)candBox.style.display='block';let pUrl=(d.last_photo_url&&d.last_photo_url.length>5)?d.last_photo_url:getLocalPhoto(d.last_reg_id,d.last_uid);if(collEl)collEl.innerText=d.last_college||'';let m=[];if(d.last_reg_no&&d.last_reg_no!=='N/A')m.push('Roll: '+d.last_reg_no);if(d.last_reg_id&&d.last_reg_id!=='N/A')m.push('ID: '+d.last_reg_id);if(d.last_batch&&d.last_batch!=='N/A')m.push('Batch: '+d.last_batch);if(metaEl)metaEl.innerText=m.join(' | ');const sKey=d.last_uid+'_'+(d.last_reg_id||'')+'_'+(d.detailed_status||'');if((!pUrl||pUrl.length<5)&&lastScanLookupKey!==sKey&&getScriptId()){lastScanLookupKey=sKey;const chk=await fetchGasDirect('check',{uid:d.last_uid});if(chk&&chk.status==='success'){savePhotos([chk]);const cp=chk.photoUrl||(chk.student&&chk.student.photoUrl)||'';if(cp&&photoEl){photoEl.dataset.fbDone='';photoEl.dataset.cur=cp;photoEl.src=cp;photoEl.style.display='block';photoEl.onerror=()=>fetchPhotoFallback(photoEl,chk.regId||d.last_reg_id,d.last_uid,cp);}if(collEl&&!collEl.innerText)collEl.innerText=chk.college||'';if(metaEl&&!metaEl.innerText){let mm=[];if(chk.regNo)mm.push('Roll: '+chk.regNo);if(chk.regId)mm.push('ID: '+chk.regId);if(chk.batch)mm.push('Batch: '+chk.batch);metaEl.innerText=mm.join(' | ');}}}if(photoEl&&pUrl&&pUrl.length>5){savePhotos([{regId:d.last_reg_id,uid:d.last_uid,photoUrl:pUrl}]);if(photoEl.dataset.cur!==pUrl){photoEl.dataset.fbDone='';photoEl.dataset.cur=pUrl;photoEl.src=pUrl;photoEl.style.display='block';photoEl.onerror=()=>fetchPhotoFallback(photoEl,d.last_reg_id,d.last_uid,pUrl);}}else if(photoEl&&!photoEl.dataset.cur){photoEl.style.display='none';}}else{if(candBox)candBox.style.display='none';}}
async function handleLogin(){const u=document.getElementById('admin-user').value,p=document.getElementById('admin-pass').value,r=document.getElementById('admin-remember').checked?1:0;const res=await fetchAPI('/login',{u,p,r});if(res&&res.success){document.getElementById('login-overlay').style.display='none';document.getElementById('app-ui').style.display='block';startPolling();fetchLogs();}else{showToast("Access Denied","danger");}}
function switchTab(id,el){document.querySelectorAll('.tab-content').forEach(c=>c.classList.remove('active'));document.querySelectorAll('.nav-tab').forEach(t=>t.classList.remove('active'));const t=document.getElementById(id);if(t)t.classList.add('active');if(el&&el.classList){el.classList.add('active');}else{const tabs=document.querySelectorAll('.nav-tab'),map={overview:0,attendance:1,inventory:2,verify:3,settings:4};if(map[id]!==undefined&&tabs[map[id]])tabs[map[id]].classList.add('active');}if(id==='verify'){setMode('VERIFY');setTimeout(()=>{const i=document.getElementById('verify-search-input');if(i){i.focus();i.select();}},150);}else if(id==='inventory'){setMode('INV');fetchInventory();}else if(id==='attendance'){setMode('IN');fetchLogs();}}
let availableVersions=[];async function checkFirmwareVersion(){const ov=document.getElementById('firmware-online-version'),lv=document.getElementById('firmware-local-version'),pv=document.getElementById('firmware-previous-version'),sc=document.getElementById('version-select-container'),se=document.getElementById('firmware-version-select');if(ov)ov.innerText="Checking...";try{const r=await fetchAPI('/check_version');if(r&&r.success){ov.innerText=r.latest;if(lv&&r.local)lv.innerText=r.local;if(pv&&r.previous)pv.innerText=r.previous;if(r.versions&&r.versions.length>0){availableVersions=r.versions;if(se){se.innerHTML="";r.versions.forEach((v,i)=>{const o=document.createElement('option');o.value=i;o.innerText=v.version+(v.version===r.latest?" (Latest)":"")+(v.version===r.local?" (Current)":"");se.appendChild(o);});if(sc)sc.classList.remove('d-none');}}else if(sc)sc.classList.add('d-none');onVersionSelectChange();showToast(r.updateAvailable?`New firmware ${r.latest} available!`:"Firmware is up to date",r.updateAvailable?"warning":"green");}else{ov.innerText="Error checking";showToast("Failed to check version","danger");}}catch(e){ov.innerText="Error";showToast("Error checking version","danger");}}
function onVersionSelectChange(){const s=document.getElementById('firmware-version-select'),b=document.getElementById('btn-trigger-update'),lv=document.getElementById('firmware-local-version');if(!s||!b)return;const idx=s.value;if(idx!==""&&availableVersions[idx]){const sv=availableVersions[idx].version,cv=lv?lv.innerText:"";b.disabled=false;if(sv===cv){b.innerText="REINSTALL CURRENT";b.className="btn-v btn-v-outline mb-2";}else{b.innerText="INSTALL UPDATE";b.className="btn-v btn-v-green mb-2";}}else b.disabled=true;}
function toggleOtaFields(){const g=document.getElementById('ota-source-github').checked,ig=document.getElementById('ota-id-group'),lg=document.getElementById('ota-link-group');if(ig)ig.style.display=g?'none':'block';if(lg)lg.style.display=g?'block':'none';}
async function saveOtaConfig(b){const s=document.getElementById('ota-source-github').checked?'github':'gdrive',id=document.getElementById('ota-script-id').value.trim(),l=document.getElementById('ota-github-link').value.trim();if(b)b.disabled=true;try{const r=await fetchAPI('/save_ota',{source:s,id,link:l});showToast(r&&r.success?"OTA saved!":(r&&r.message||"Save failed"),r&&r.success?"green":"danger");}catch(e){showToast("Error saving OTA","danger");}finally{if(b)b.disabled=false;}}
async function testOtaConnection(b){const s=document.getElementById('ota-source-github').checked?'github':'gdrive',id=document.getElementById('ota-script-id').value.trim(),l=document.getElementById('ota-github-link').value.trim();if(b){b.disabled=true;b.innerHTML='<i class="fa-solid fa-spinner fa-spin me-2"></i>Testing...';}try{const r=await fetchAPI('/test_ota',{source:s,id,link:l});showToast(r&&r.success?"OTA connection OK!":(r&&r.message||"Connection failed"),r&&r.success?"green":"danger");}catch(e){showToast("Test error","danger");}finally{if(b){b.disabled=false;b.innerHTML='<i class="fa-solid fa-network-wired me-2"></i>TEST CONNECTION';}}}
async function toggleAutoUpdate(el){const en=el.checked,r=await fetchAPI('/toggle_auto_update',{enabled:en?"1":"0"});if(r&&r.success)showToast(`Auto update ${en?'enabled':'disabled'}`,"green");else{showToast("Save failed","danger");el.checked=!en;}}
async function triggerOTAUpdate(){const s=document.getElementById('firmware-version-select');if(!s||s.value===""){showToast("No version selected","danger");return;}const v=availableVersions[s.value].version,u=availableVersions[s.value].downloadUrl;if(!confirm(`Deploy ${v}? Device will reboot.`))return;const pb=document.getElementById('ota-progress-bar'),pc=document.getElementById('ota-progress-container'),st=document.getElementById('ota-status-text'),b=document.getElementById('btn-trigger-update');if(pc)pc.classList.remove('d-none');if(pb)pb.style.width="0%";if(st)st.innerText="Initiating...";if(b)b.disabled=true;try{const r=await fetchAPI('/trigger_update',{url:u,version:v});if(r&&r.success)showToast("OTA Started!","warning");else{if(pc)pc.classList.add('d-none');if(b)b.disabled=false;showToast(r&&r.message||"Update failed","danger");}}catch(e){if(pc)pc.classList.add('d-none');if(b)b.disabled=false;showToast("Connection lost","danger");}}
function showRebootOverlay(){let o=document.getElementById('reboot-overlay');if(!o){o=document.createElement('div');o.id='reboot-overlay';o.style='position:fixed;inset:0;background:rgba(0,0,0,0.95);z-index:10000;display:flex;align-items:center;justify-content:center;backdrop-filter:blur(10px);';o.innerHTML='<div class="text-center p-4" style="max-width:400px;background:#1a1a1a;border:1px solid #333;border-radius:12px;"><i class="fa-solid fa-arrows-rotate fa-spin text-warning mb-4" style="font-size:3rem;"></i><h4 class="text-white mb-2">Device Rebooting</h4><p class="text-white-50 small mb-4">Update installed. Reconnecting...</p><div class="fw-bold text-info" id="reboot-countdown" style="font-size:1.4rem;">Reconnecting in 15s...</div></div>';document.body.appendChild(o);}else o.style.display='flex';let c=15;const ce=document.getElementById('reboot-countdown');const iv=setInterval(()=>{c--;if(ce)ce.innerText=`Reconnecting in ${c}s...`;if(c<=0){clearInterval(iv);window.location.reload();}},1000);}
async function setMode(m){const r=await fetchAPI('/mode',{m});if(r&&r.success){showToast(`Mode: ${m}`,"green");const s=document.getElementById('live-status');if(s)s.innerText=m;}else showToast("Mode switch failed","danger");}
let captureActive=false;async function startCaptureFlow(){if(captureActive)return;captureActive=true;const el=document.getElementById('capture-uid'),msg=document.getElementById('capture-msg'),btn=document.getElementById('btn-capture'),ic=btn.querySelector('i');el.innerText="SCAN NOW";el.style.color="var(--v-orange)";msg.innerText="Bring an unassigned card near the sensor...";ic.className='fa-solid fa-spinner fa-spin';await fetchAPI('/capture',{reset:'1'});let att=0;const p=setInterval(async()=>{const r=await fetchAPI('/capture');if(r&&r.uid&&r.uid!=="--------"){el.innerText=r.uid;el.style.color="var(--v-green)";msg.innerText="TAG DETECTED! Use save below.";document.getElementById('cloud-save-btn').style.display="inline-flex";ic.className='fa-solid fa-crosshairs';captureActive=false;clearInterval(p);}att++;if(att>30){el.innerText="TIMEOUT";el.style.color="var(--v-red)";msg.innerText="No card detected.";ic.className='fa-solid fa-crosshairs';captureActive=false;clearInterval(p);}},1000);}
async function pushToCloud(){const b=document.getElementById('cloud-save-btn'),ic=b.querySelector('i'),uid=document.getElementById('capture-uid').innerText;ic.className='fa-solid fa-spinner fa-spin';const r=await fetchAPI('/sync',{uid});ic.className='fa-solid fa-cloud-arrow-up';if(r){showToast('Cloud Sync Success','green');b.style.display="none";fetchInventory();}else showToast('Cloud Sync Failed','danger');}
async function saveNetwork(){const b=document.getElementById('btn-save-net'),ic=b.querySelector('i'),s=document.getElementById('conf-ssid').value,p=document.getElementById('conf-pass').value;ic.className='fa-solid fa-spinner fa-spin';const r=await fetchAPI('/save_net',{ssid:s,pass:p});ic.className='fa-solid fa-save';showToast(r?'WiFi Saved. Applying...':'Connection Error',r?'green':'danger');}
async function scanWiFi(b){const ic=b.querySelector('i'),o=ic.className,d=document.getElementById('wifi-scan-results');ic.className='fa-solid fa-spinner fa-spin';d.innerHTML='<div class="p-2 small text-white-50">Searching...</div>';d.style.display='block';const r=await fetchAPI('/scan_wifi');ic.className=o;if(r&&r.length>0){d.innerHTML=r.map(w=>`<div class="p-2 border-bottom border-dark small d-flex justify-content-between" style="cursor:pointer;" onclick="selectSSID('${w.ssid}')"><span>${w.ssid}</span><span class="text-white-50">${w.rssi}dBm</span></div>`).join('');showToast(`Found ${r.length} networks`,'green');}else{d.innerHTML='<div class="p-2 small text-danger">No networks found</div>';showToast("Scan failed","danger");}}
function selectSSID(s){document.getElementById('conf-ssid').value=s;document.getElementById('wifi-scan-results').style.display='none';}
async function saveSystem(){const b=document.getElementById('btn-save-sys'),ic=b.querySelector('i'),sc=document.getElementById('conf-script').value,u=document.getElementById('conf-admin-user').value,p=document.getElementById('conf-admin-pass').value;ic.className='fa-solid fa-spinner fa-spin';const r=await fetchAPI('/save_sys',{script:sc,adminUser:u,adminPass:p});ic.className='fa-solid fa-save';if(r){showToast('Rebooting in 3s...','green');setTimeout(()=>{window.location.href="http://vvarma-rfid.local/dashboard";},4000);}else showToast('Error','danger');}
async function factoryReset(){if(confirm("Wipe all system settings?")){await fetchAPI('/factory_reset');window.location.reload();}}
async function resetCapture(){await fetchAPI('/capture',{reset:'1'});document.getElementById('capture-uid').innerText="--------";document.getElementById('capture-msg').innerText="Capture cleared";document.getElementById('cloud-save-btn').style.display="none";showToast("Capture Ready","orange");}
async function fetchLogs(btn){let ic,o;if(btn){ic=btn.querySelector('i');o=ic.className;ic.className='fa-solid fa-spinner fa-spin';}const list=document.getElementById('log-list'),lb=document.getElementById('log-loading-bar');let hasCached=false;const cached=localStorage.getItem('vvarma_logs_cache');if(cached){try{const cd=JSON.parse(cached);if(Array.isArray(cd)&&cd.length>0){renderLogs(cd);hasCached=true;}}catch(e){}}if(hasCached){if(lb)lb.style.display='block';}else{if(lb)lb.style.display='none';list.innerHTML='<div class="text-center p-4 text-warning"><div class="spinner-border spinner-border-sm me-2"></div>Loading attendance logs...</div>';}let data=await fetchGasDirect('get_logs');if(!data||!Array.isArray(data)){data=await fetchAPI('/logs');}if(lb)lb.style.display='none';if(btn&&ic)ic.className=o;if(data&&Array.isArray(data)&&data.length>0){savePhotos(data);try{localStorage.setItem('vvarma_logs_cache',JSON.stringify(data));}catch(e){}renderLogs(data);}else if(!hasCached){list.innerHTML='<p class="text-center text-white-50 p-4">No Students Available (Today/Yesterday)</p>';}}
function renderLogs(data){const list=document.getElementById('log-list');list.innerHTML=data.map((l,idx)=>{const st=(l.status||'').toUpperCase(),isLate=st.includes('LATE'),sc=isLate?'var(--v-orange)':'var(--v-green)',p=l.photoUrl||getLocalPhoto(l.regId,'');const ph=p?`<img id="lph-${idx}" referrerpolicy="no-referrer" src="${p}" onerror="fetchPhotoFallback(this,'${(l.regId||'').replace(/'/g,'')}','','${p.replace(/'/g,'')}')" style="width:50px;height:50px;border-radius:50%;object-fit:cover;border:2px solid ${sc};margin-right:12px;background:#222;">`:`<div style="width:50px;height:50px;border-radius:50%;background:rgba(255,255,255,0.08);display:flex;align-items:center;justify-content:center;margin-right:12px;border:2px solid ${sc};"><i class="fa-solid fa-user text-white-50"></i></div>`;return `<div class="hist-item p-3 mb-2" style="background:rgba(255,255,255,0.03);border-radius:12px;border-left:4px solid ${sc};"><div class="d-flex justify-content-between align-items-center mb-2"><div class="d-flex align-items-center">${ph}<div><div class="fw-bold text-white fs-5">${l.name||'---'}</div><div class="small text-white-50">${l.regNo||l.regId||''} | ${l.date||''}</div>${l.college?`<div class="small text-warning" style="font-size:0.75rem;">${l.college}</div>`:''}</div></div><div class="badge" style="background:${sc}22;color:${sc};border:1px solid ${sc}44;font-size:0.75rem;padding:6px 12px;">${st||'PRESENT'}</div></div><div class="row g-2 small mt-1 pt-2 border-top border-dark"><div class="col-6"><i class="fa-solid fa-users-gear me-1 text-white-50"></i>BATCH: <span class="text-white">${l.batch||'---'}</span></div><div class="col-6"><i class="fa-solid fa-clock me-1 text-white-50"></i>SLOT: <span class="text-white">${l.slot||'---'}</span></div><div class="col-6"><i class="fa-solid fa-hourglass-start me-1 text-white-50"></i>IN: <span class="text-green fw-bold">${l.inTime||'--:--'}</span></div><div class="col-6"><i class="fa-solid fa-hourglass-end me-1 text-white-50"></i>OUT: <span class="text-orange fw-bold">${l.outTime||'--:--'}</span></div></div></div>`;}).join('');}
async function fetchInventory(btn){let ic,o;if(btn){ic=btn.querySelector('i');o=ic.className;ic.className='fa-solid fa-spinner fa-spin';}const list=document.getElementById('inv-history'),ib=document.getElementById('inv-loading-bar');let hasCached=false;const cached=localStorage.getItem('vvarma_inv_cache');if(cached){try{const cd=JSON.parse(cached);if(Array.isArray(cd)&&cd.length>0){renderInventory(cd);hasCached=true;}}catch(e){}}if(hasCached){if(ib)ib.style.display='block';}else{if(ib)ib.style.display='none';list.innerHTML='<div class="text-center p-4 text-warning"><div class="spinner-border spinner-border-sm me-2"></div>Loading RFID inventory...</div>';}let data=await fetchGasDirect('get_inventory');if(!data||!Array.isArray(data)){data=await fetchAPI('/inventory');}if(ib)ib.style.display='none';if(btn&&ic)ic.className=o;if(data&&Array.isArray(data)&&data.length>0){savePhotos(data);try{localStorage.setItem('vvarma_inv_cache',JSON.stringify(data));}catch(e){}renderInventory(data);}else if(!hasCached){list.innerHTML='<p class="text-center text-white-50 p-3">No inventory items available</p>';}}
function renderInventory(data){const list=document.getElementById('inv-history');list.innerHTML=data.map(i=>{const isAv=(i.status||'').toLowerCase()==='available',sc=isAv?'var(--v-green)':'var(--v-orange)',p=i.photoUrl||i.assignedPhotoUrl||getLocalPhoto(i.reg_id,i.uid),rid=(i.reg_id||i.assignedToId||'').replace(/'/g,''),uid=(i.uid||i.tagId||'').replace(/'/g,'');const pt=(!isAv&&p)?`<img referrerpolicy="no-referrer" src="${p}" onerror="fetchPhotoFallback(this,'${rid}','${uid}','${p.replace(/'/g,'')}')" style="width:44px;height:44px;border-radius:50%;object-fit:cover;border:2px solid var(--v-orange);margin-right:10px;background:#222;">`:(!isAv?`<div style="width:44px;height:44px;border-radius:50%;background:rgba(255,255,255,0.08);display:flex;align-items:center;justify-content:center;margin-right:10px;border:2px solid var(--v-orange);"><i class="fa-solid fa-user text-white-50"></i></div>`:'');const dt=isAv?`<div class="small text-white-50 mt-1">AVAILABLE SINCE: ${i.date||'---'}</div>`:`<div class="d-flex align-items-center mt-2">${pt}<div><div class="text-white fw-bold">${i.name||i.assignedToName||'Assigned'}</div><div class="small text-white-50">ID: ${i.reg_id||i.assignedToId||'---'} | Roll: ${i.reg_no||i.staff_id||'---'}</div><div class="small text-warning" style="font-size:0.75rem;">${i.college||i.assignedCollege||''}</div>${i.batch?`<div class="small text-white-50" style="font-size:0.75rem;">Batch: ${i.batch}</div>`:''}</div></div>`;return `<div class="hist-item d-block text-start p-3" style="border-bottom:1px solid #333;" onmouseover="this.style.background='#1f1f1f'" onmouseout="this.style.background='transparent'"><div class="d-flex justify-content-between align-items-center"><div class="text-warning fw-bold fs-6"><i class="fa-solid fa-id-card me-2"></i>${i.uid||i.tagId}</div><div class="badge" style="background:${sc}22;color:${sc};border:1px solid ${sc}44;font-size:0.7rem;">${(i.status||'AVAILABLE').toUpperCase()}</div></div>${dt}</div>`;}).join('');}
let verifyActive=false;async function startVerifyFlow(){if(verifyActive)return;verifyActive=true;const inp=document.getElementById('verify-search-input'),btn=document.getElementById('btn-start-verify'),ic=btn.querySelector('i');inp.value="READING CARD...";ic.className='fa-solid fa-spinner fa-spin';await fetchAPI('/mode',{m:'VERIFY'});await fetchAPI('/capture',{reset:'1'});let att=0;const p=setInterval(async()=>{const r=await fetchAPI('/capture');if(r&&r.uid&&r.uid!=="--------"){inp.value=r.uid;clearInterval(p);verifyActive=false;ic.className='fa-solid fa-crosshairs';performLookup();}att++;if(att>20){clearInterval(p);verifyActive=false;ic.className='fa-solid fa-crosshairs';inp.value="";showToast("Verify Timeout","danger");}},1000);}
async function performLookup(){const uid=(document.getElementById('verify-search-input').value||'').trim();if(!uid||uid.length<4)return;const ld=document.getElementById('verify-loader'),rs=document.getElementById('verify-results');ld.style.display='block';rs.innerHTML='';let res=await fetchGasDirect('check',{uid});if(!res||!res.status)res=await fetchAPI('/validate',{action:'check',uid});ld.style.display='none';if(res&&res.status==='success'){savePhotos([res]);renderVerifyResults(res,uid);}else{const msg=(res&&res.message)||`Validation failed for [${uid}]`;const isNew=msg.toLowerCase().includes('not in')||msg.toLowerCase().includes('unknown');rs.innerHTML=isNew?`<div class="alert border-0 bg-dark text-white text-center py-4" style="border-radius:15px;border:2px dashed var(--v-orange)!important;"><i class="fa-solid fa-id-card fs-1 mb-3" style="color:var(--v-orange);"></i><h4 class="fw-bold" style="color:var(--v-orange);">NEW CARD SCANNED</h4><p class="mb-3 text-white-50">Card [${uid}] not in inventory.</p><button class="btn-v btn-v-orange px-4" onclick="switchTab('inventory',this);setTimeout(()=>startCaptureFlow(),500);"><i class="fa-solid fa-plus me-2"></i>ADD TO INVENTORY</button></div>`:`<div class="alert alert-danger border-0 bg-danger bg-opacity-10 text-danger text-center py-4"><i class="fa-solid fa-circle-exclamation fs-1 mb-2"></i><h4 class="fw-bold">TAG NOT FOUND</h4><p class="mb-0">${msg}</p></div>`;}}
function renderVerifyResults(d,uid){const rs=document.getElementById('verify-results'),s=d.student||d,inv=d.inventory||{};if(!s||(!s.name&&!s.studentName)||s.name==="UNAUTHORIZED"||s.name==="Unknown"||s.name==="---"){rs.innerHTML=`<div class="alert alert-warning border-0 bg-warning bg-opacity-10 text-warning text-center py-4"><i class="fa-solid fa-box-open fs-1 mb-3"></i><h4 class="fw-bold">AVAILABLE IN INVENTORY</h4><p class="mb-3">Unassigned tag.</p><div class="small text-white-50 mb-3">Added By: ${inv.AddedBy||'Admin'} | Date: ${inv.AddedDate||'---'}</div></div>`;return;}const ph=s.photoUrl||d.photoUrl||s.ProfilePhotoUrl||s.photo||getLocalPhoto(s.regId||s.reg_id,uid);const rid=(s.regId||s.reg_id||'').replace(/'/g,'');const phHtml=ph?`<img referrerpolicy="no-referrer" src="${ph}" onerror="fetchPhotoFallback(this,'${rid}','${uid.replace(/'/g,'')}','${ph.replace(/'/g,'')}')" style="width:80px;height:80px;border-radius:50%;object-fit:cover;border:3px solid var(--v-green);background:#222;">`:`<div class="bg-success bg-opacity-10 p-3 rounded-circle" style="width:70px;height:70px;display:flex;align-items:center;justify-content:center;"><i class="fa-solid fa-user-check text-success fs-1"></i></div>`;rs.innerHTML=`<div class="v-card border-success" style="border-left-color:var(--v-green);"><div class="row align-items-center mb-4"><div class="col-auto">${phHtml}</div><div class="col"><h3 class="fw-bold text-white mb-0 text-uppercase">${s.name||s.studentName||'---'}</h3><div class="text-success small fw-bold"><i class="fa-solid fa-shield-halved me-1"></i>VERIFIED POSITIVE</div></div></div><div class="row g-3"><div class="col-md-6 border-end border-dark"><div class="mb-3"><label class="text-white-50 x-small text-uppercase">Registration ID</label><div class="text-white fw-bold">${s.regId||s.reg_id||'---'}</div></div><div class="mb-3"><label class="text-white-50 x-small text-uppercase">Register Number</label><div class="text-white fw-bold">${s.regNo||s.reg_no||'---'}</div></div><div class="mb-3"><label class="text-white-50 x-small text-uppercase">College / University</label><div class="text-white small">${s.college||'---'}</div></div></div><div class="col-md-6"><div class="mb-3"><label class="text-white-50 x-small text-uppercase">Batch Allocation</label><div class="text-orange fw-bold">${s.batch||'---'}</div></div><div class="mb-3"><label class="text-white-50 x-small text-uppercase">Department</label><div class="text-white fw-bold">${s.dept||s.department||'---'}</div></div><div class="row"><div class="col-6"><label class="text-white-50 x-small text-uppercase">Start Date</label><div class="text-white small">${s.startDate||'---'}</div></div><div class="col-6"><label class="text-white-50 x-small text-uppercase">End Date</label><div class="text-white small">${s.endDate||'---'}</div></div></div></div></div><div class="mt-4 pt-3 border-top border-dark d-flex justify-content-between align-items-center"><div><span class="text-white-50 small">Tag Status:</span><span class="badge bg-success ms-1">${s.status||'ACTIVE'}</span></div><div class="text-white-50 x-small">MSG: <span class="text-white font-monospace">${d.message||s.message||'---'}</span></div></div></div>`;}
function resetVerifyUI(){document.getElementById('verify-search-input').value="";document.getElementById('verify-results').innerHTML="";fetchAPI('/capture',{reset:'1'});}
async function resetUI(btn){if(btn){const ic=btn.querySelector('i'),o=ic.className;ic.className='fa-solid fa-spinner fa-spin';await fetchAPI('/reset_display');ic.className=o;}else{await fetchAPI('/reset_display');}document.getElementById('live-status').innerText="WAITING";document.getElementById('live-uid').innerText="READY FOR SCAN";document.getElementById('live-name').innerText="---";document.getElementById('live-status-msg').innerText="";const cb=document.getElementById('live-cand-box');if(cb)cb.style.display='none';showToast("Display Reset","green");}
function showToast(m,t='orange'){const el=document.getElementById('toast');document.getElementById('toast-msg').innerText=m;el.style.borderLeftColor=t==='danger'?'var(--v-red)':(t==='green'?'var(--v-green)':'var(--v-orange)');el.style.display='block';setTimeout(()=>{el.style.display='none';},3000);}
let vDeb=null;const vi=document.getElementById('verify-search-input');if(vi){vi.addEventListener('keydown',e=>{if(e.key==='Enter'){e.preventDefault();if(vDeb)clearTimeout(vDeb);performLookup();}});vi.addEventListener('input',()=>{if(vDeb)clearTimeout(vDeb);if(vi.value.trim().length>=6)vDeb=setTimeout(()=>performLookup(),350);});}
if(document.getElementById('app-ui').style.display==='block'){startPolling();}
</script></body></html>)rawliteral");
  server.sendContent("");
}

void handleStatus() {
  if (!isAuth()) {
    server.send(403, "application/json", "{\"error\":\"Unauthorized\"}");
    return;
  }
  String stepStr;
  switch (currentStep) {
  case STEP_CARD_NUM:
    stepStr = "Card Number";
    break;
  case STEP_VALIDATING:
    stepStr = "Validating";
    break;
  case STEP_RESULT:
    stepStr = "Result";
    break;
  case STEP_NAME:
    stepStr = "Name";
    break;
  case STEP_STATUS:
    stepStr = "Status";
    break;
  default:
    stepStr = "Ready";
    break;
  }

  auto escapeJ = [](String s) {
    s.replace("\"", "'");
    s.replace("\r", "");
    s.replace("\n", " ");
    return s;
  };

  String json = "{";
  json += "\"mcu\":true,";
  json +=
      "\"net\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") +
      ",";
  json += "\"db\":" + String(deviceVerified ? "true" : "false") + ",";
  json +=
      "\"wifi\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") +
      ",";
  json += "\"name\":\"" + escapeJ(String(DEVICE_NAME)) + "\",";
  json += "\"fw_version\":\"" + escapeJ(String(FIRMWARE_VERSION)) + "\",";
  json += "\"ip\":\"" + localIpStr + "\",";
  json += "\"mac\":\"" + myMac + "\",";
  json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  json += "\"script_id\":\"" + scriptId + "\",";
  json += "\"saved_ssid\":\"" + escapeJ(savedSSID) + "\",";
  json += "\"loading\":" + String(cardProcessing ? "true" : "false") + ",";
  json += "\"mode\":\"" +
          String(currentMode == MODE_IN
                     ? "IN"
                     : (currentMode == MODE_OUT
                            ? "OUT"
                            : (currentMode == MODE_INV ? "INV" : "VERIFY"))) +
          "\",";
  json += "\"last_uid\":\"" + escapeJ(capturedUid) + "\",";
  json += "\"last_user\":\"" + escapeJ(lastUserName) + "\",";
  json += "\"detailed_status\":\"" + escapeJ(lastDetailedStatus) + "\",";
  json += "\"last_status_msg\":\"" + escapeJ(lastStatusMsg) + "\",";
  json += "\"last_reg_id\":\"" + escapeJ(lastRegId.length() > 0 ? lastRegId : currentRegId) + "\",";
  json += "\"last_reg_no\":\"" + escapeJ(lastRegNo.length() > 0 ? lastRegNo : currentRegNo) + "\",";
  json += "\"last_college\":\"" + escapeJ(lastCollege.length() > 0 ? lastCollege : currentCollege) + "\",";
  json += "\"last_batch\":\"" + escapeJ(lastBatch.length() > 0 ? lastBatch : currentBatch) + "\",";
  json += "\"last_photo_url\":\"" + escapeJ(lastPhotoUrl.length() > 0 ? lastPhotoUrl : currentPhotoUrl) + "\",";
  json += "\"dev_user\":\"" + escapeJ(deviceUser) + "\",";
  json += "\"dev_pass\":\"" + escapeJ(devicePass) + "\",";
  json += "\"auto_update\":" + String(autoUpdateEnabled ? "true" : "false") + ",";
  json += "\"ota_source\":\"" + escapeJ(otaSource) + "\",";
  json += "\"ota_link\":\"" + escapeJ(otaLink) + "\",";
  json += "\"ota_id\":\"" + escapeJ(otaId) + "\",";
  json += "\"ota_running\":" + String(otaRunning ? "true" : "false") + ",";
  json += "\"ota_progress\":" + String(otaProgress) + ",";
  json += "\"ota_status\":\"" + escapeJ(otaStatusStr) + "\",";
  json += "\"ota_error\":\"" + escapeJ(otaErrorMsg) + "\",";
  json += "\"process_step\":\"" + stepStr + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

void handleMode() {
  if (!isAuth()) {
    server.send(403, "application/json", "{\"error\":\"Unauthorized\"}");
    return;
  }
  if (server.hasArg("m")) {
    String m = server.arg("m");
    if (m == "IN")
      currentMode = MODE_IN;
    else if (m == "OUT")
      currentMode = MODE_OUT;
    else if (m == "INV")
      currentMode = MODE_INV;
    else if (m == "VERIFY")
      currentMode = MODE_VERIFY;
    server.send(200, "application/json", "{\"success\":true}");
  } else {
    server.send(400, "application/json", "{\"success\":false}");
  }
}

void handleCapture() {
  if (!isAuth()) {
    server.send(403, "application/json", "{\"error\":\"Unauthorized\"}");
    return;
  }
  if (server.hasArg("reset")) {
    capturedUid = "--------";
    server.send(200, "application/json", "{\"success\":true}");
  } else {
    server.send(200, "application/json", "{\"uid\":\"" + capturedUid + "\"}");
  }
}

void handleSaveInventory() {
  if (!isAuth()) {
    server.send(403, "application/json", "{\"error\":\"Unauthorized\"}");
    return;
  }
  if (server.hasArg("uid")) {
    String uid = server.arg("uid");
    saveInventoryToSheet(uid);
    server.send(200, "application/json", "{\"success\":true}");
  } else {
    server.send(400, "application/json", "{\"success\":false}");
  }
}

void handleSaveNetwork() {
  if (!isAuth()) {
    server.send(403, "application/json", "{\"error\":\"Unauthorized\"}");
    return;
  }
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  String script = server.arg("scriptId");

  preferences.begin("gsv-rfid", false);
  preferences.putString("ssid", ssid);
  preferences.putString("pass", pass);
  preferences.putString("scriptId", script); // Always write, even if empty
  preferences.end();

  server.send(200, "application/json", "{\"success\":true}");
  rebootTimer = millis() + 2000;
}

void handleSaveSystem() {
  if (!isAuth()) {
    server.send(403, "application/json", "{\"error\":\"Unauthorized\"}");
    return;
  }
  String script = server.arg("script");
  String user = server.arg("adminUser");
  String pass = server.arg("adminPass");

  preferences.begin("gsv-rfid", false);
  preferences.putString("scriptId", script); // Always write, even if empty
  scriptId = script;
  if (user != "") {
    preferences.putString("devUser", user);
    deviceUser = user;
  }
  if (pass != "") {
    preferences.putString("devPass", pass);
    devicePass = pass;
  }
  preferences.end();

  server.send(200, "application/json", "{\"success\":true}");
  rebootTimer = millis() + 2000;
}

void handleFactoryReset() {
  if (!isAuth()) {
    server.send(403, "application/json", "{\"error\":\"Unauthorized\"}");
    return;
  }
  server.send(200, "application/json", "{\"success\":true}");
  delay(500);
  factoryReset();
}

void handleScanWiFi() {
  if (!isAuth()) {
    server.send(403, "application/json", "{\"error\":\"Unauthorized\"}");
    return;
  }
  webLog("WiFi: Scanning networks...");
  int n = WiFi.scanNetworks(false, false, false, 300); // Faster scan
  if (n < 0) {
    webLog("WiFi: Scan failed!");
    server.send(500, "application/json", "[]");
    return;
  }

  String json = "[";
  for (int i = 0; i < n; i++) {
    if (i > 0)
      json += ",";
    int encType = (int)WiFi.encryptionType(i);
    bool isEncrypted = (encType != 0);
    json += "{\"ssid\":\"" + WiFi.SSID(i) +
            "\",\"rssi\":" + String(WiFi.RSSI(i)) +
            ",\"encrypted\":" + String(isEncrypted) + "}";
  }
  json += "]";
  server.send(200, "application/json", json);
  WiFi.scanDelete();
  webLog("WiFi: Found " + String(n) + " networks.");
}

void handleLogs() {
  if (!isAuth()) {
    server.send(403, "application/json", "{\"error\":\"Unauthorized\"}");
    return;
  }
  if (scriptId == "") {
    server.send(200, "application/json", "[]");
    return;
  }
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url = "https://script.google.com/macros/s/" + scriptId +
               "/exec?action=get_logs&mac=" + myMac + "&user=" + deviceUser + "&pass=" + devicePass;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (http.begin(client, url)) {
    http.setTimeout(15000);
    int code = http.GET();
    if (code == 200 || code == 302)
      server.send(200, "application/json", http.getString());
    else
      server.send(200, "application/json", "[]");
    http.end();
  } else
    server.send(200, "application/json", "[]");
}

void handleInventory() {
  if (!isAuth()) {
    server.send(403, "application/json", "{\"error\":\"Unauthorized\"}");
    return;
  }
  if (scriptId == "") {
    server.send(200, "application/json", "[]");
    return;
  }
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url =
      "https://script.google.com/macros/s/" + scriptId +
      "/exec?action=get_inventory&mac=" + myMac + "&user=" + deviceUser +
      "&pass=" +
      devicePass; // ⭐ FIX: Include auth params for reliable GSheet access
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (http.begin(client, url)) {
    http.setTimeout(15000); // ⭐ FIX: Add timeout to prevent hanging
    int code = http.GET();
    if (code == 200 || code == 302)
      server.send(200, "application/json", http.getString());
    else
      server.send(200, "application/json", "[]");
    http.end();
  } else
    server.send(200, "application/json", "[]");
}

void handleValidate() {
  if (!server.hasArg("uid")) {
    server.send(400, "application/json",
                "{\"status\":\"error\",\"message\":\"Missing UID\"}");
    return;
  }
  String uid = server.arg("uid");
  String action = server.hasArg("action") ? server.arg("action") : "check";

  if (scriptId == "") {
    server.send(
        400, "application/json",
        "{\"status\":\"error\",\"message\":\"Script ID not configured\"}");
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url = "https://script.google.com/macros/s/" + scriptId +
               "/exec?action=" + action + "&uid=" + uid + "&mac=" + myMac +
               "&user=" + deviceUser +
               "&pass=" + devicePass; // ⭐ FIX: Include auth for verify lookup
  // 🔧 FIXED: use the library constant directly
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (http.begin(client, url)) {
    http.setTimeout(10000);
    int code = http.GET();
    if (code == 200 || code == 302) {
      server.send(200, "application/json", http.getString());
    } else {
      server.send(500, "application/json",
                  "{\"status\":\"error\",\"message\":\"HTTP " + String(code) +
                      "\"}");
    }
    http.end();
  } else {
    server.send(500, "application/json",
                "{\"status\":\"error\",\"message\":\"Connection Failed\"}");
  }
}

void handleResetDisplay() {
  capturedUid = "--------";
  lastUserName = "READY TO SCAN";
  lastStatusMsg = "Scan Ready...";
  lastDetailedStatus = "";
  currentRegNo = "";
  currentPhotoUrl = "";
  lastRegId = "";
  lastRegNo = "";
  lastCollege = "";
  lastBatch = "";
  lastPhotoUrl = "";
  server.send(200, "application/json", "{\"status\":\"ok\"}");
}

void handleGetScriptId() {
  server.send(200, "application/json", "{\"scriptId\":\"" + scriptId + "\"}");
}

void buttonWatchdogTask(void *pvParameters) {
  unsigned long pressStart = 0;
  for (;;) {
    if (digitalRead(BTN_STATS) == LOW) {
      if (pressStart == 0) {
        pressStart = millis();
      } else if (millis() - pressStart >= 5000) {
        factoryReset();
      }
    } else {
      pressStart = 0;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}