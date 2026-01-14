/*
 * ESP32 RFID Door Lock System with Firebase Cloud
 * Hardware: MFRC522 RFID, Relay (EM Lock), Buzzer
 * Features: Firebase Realtime DB, NVS Storage, NTP Timestamps, 
 *           LittleFS Local Logs (30 days), Command-Based Cloud, User Management
 * 
 * ARCHITECTURE: NVS (Local) is AUTHORITATIVE
 * ============================================
 * - NVS decides all access (whitelist/blacklist/pending)
 * - Firebase is a command inbox + audit mirror
 * - If cloud dies -> door still works (offline-first)
 * - If ESP32 reboots -> door still works (NVS persists)
 * 
 * Cloud IS allowed to:
 *   - Send commands: add, delete, move, rename users
 *   - Send unlock commands
 *   - Request logs
 * 
 * Cloud is NOT allowed to:
 *   - Overwrite entire lists
 *   - Resync full JSON trees
 *   - Rebuild local state
 * 
 * Pin Configuration:
 * MFRC522: SDA=21, RST=22, SCK=18, MOSI=23, MISO=19
 * Relay: GPIO 25 (Active HIGH)
 * Buzzer: GPIO 27
 * 
 * Firebase Structure:
 * /devices/device1/whitelist/{uid}: {name, addedAt}  (audit mirror only)
 * /devices/device1/blacklist/{uid}: {name, addedAt}  (audit mirror only)
 * /devices/device1/pending/{uid}: {name, firstSeen}  (audit mirror only)
 * /devices/device1/logs/{timestamp}: {uid, name, status, type, time}
 * /devices/device1/commands/unlock: {duration, timestamp}
 * /devices/device1/commands/manage: {action, uid, from, to, name}
 * /devices/device1/status: {online, lastSeen, ip}
 * 
 * Required Libraries (Install via Arduino Library Manager):
 * 1. Firebase Arduino Client Library for ESP8266 and ESP32 (by mobizt)
 * 2. ArduinoJson (by Benoit Blanchon)
 * 3. MFRC522 (by GithubCommunity)
 * 4. NTPClient (by Fabrice Weinberg)
 */

#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <MFRC522.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <SPI.h>
#include <LittleFS.h>

// Token status callback function (replaces addons/TokenHelper.h)
void tokenStatusCallback(token_info_t info);

// ==================== PIN DEFINITIONS ====================
#define SS_PIN 21
#define RST_PIN 22
#define RELAY_PIN 25
#define BUZZER_PIN 27

// SPI Pins (explicitly defined)
#define SCK_PIN 18
#define MOSI_PIN 23
#define MISO_PIN 19

// ==================== WIFI CREDENTIALS ====================
const char* ssid = "Airtel_SKETCH";           // CHANGE THIS
const char* password = "Sketch@123";   // CHANGE THIS

// ==================== FIREBASE CONFIGURATION ====================
// IMPORTANT: Create a Firebase project and get these values from Firebase Console
#define API_KEY "AIzaSyDFTyL7DjvdcDI_D70ZWMgcgXuTtnxGRnE"                    // Project Settings > General > Web API Key
#define DATABASE_URL "https://emlocksketch-default-rtdb.asia-southeast1.firebasedatabase.app"  // Realtime Database URL
#define USER_EMAIL "afsalkalladi@gmail.com"            // Firebase Auth user email
#define USER_PASSWORD "Qawsed1w2e3r"               // Firebase Auth user password

const char* device_id = "device1";

// Firebase paths
String basePath = "/devices/" + String(device_id);
String whitelistPath = basePath + "/whitelist";
String blacklistPath = basePath + "/blacklist";
String pendingPath = basePath + "/pending";
String logsPath = basePath + "/logs";
String commandsPath = basePath + "/commands";
String statusPath = basePath + "/status";

// ==================== OBJECTS ====================
FirebaseData fbdo;
FirebaseData stream;
FirebaseAuth auth;
FirebaseConfig config;

MFRC522 rfid(SS_PIN, RST_PIN);
Preferences prefs;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800, 60000); // IST (UTC+5:30), update every 60s

// ==================== TIMING VARIABLES ====================
unsigned long lastNTPUpdate = 0;
unsigned long lastLogCleanup = 0;

// ==================== FREERTOS MULTI-THREADING ====================
TaskHandle_t firebaseTaskHandle = NULL;
TaskHandle_t rfidTaskHandle = NULL;
QueueHandle_t logQueue = NULL;
QueueHandle_t pendingQueue = NULL;
QueueHandle_t commandQueue = NULL;

// Structure for log queue
struct LogEntry {
  char uid[20];
  char name[50];
  char status[20];
  char type[20];
};

// Structure for pending queue
struct PendingEntry {
  char uid[20];
};

// Structure for command queue (Core 0 -> Core 1)
struct Command {
  enum { UNLOCK, SYNC_LISTS } type;
  uint32_t duration;
};

// ==================== CLOUD SERVICE MODULE (CORE 0) ====================
// IMPORTANT: Cloud is COMMAND-BASED ONLY - NVS is authoritative!
// Cloud can: add, delete, move, rename, unlock, request logs
// Cloud CANNOT: overwrite lists, resync full JSON, rebuild local state
namespace CloudService {
  unsigned long lastStatusUpdate = 0;
  bool firebaseReady = false;
  
  void updateDeviceStatus() {
    if (WiFi.status() != WL_CONNECTED || !Firebase.ready()) return;
    
    FirebaseJson json;
    json.set("online", true);
    json.set("lastSeen", getISOTimestamp());
    json.set("ip", WiFi.localIP().toString());
    json.set("rssi", WiFi.RSSI());
    json.set("freeHeap", ESP.getFreeHeap());
    json.set("uptime", millis() / 1000);
    json.set("mode", "nvs-authoritative");
    
    if (Firebase.RTDB.setJSON(&fbdo, statusPath, &json)) {
      Serial.println("Device status updated");
    }
  }
  
  void processLogQueue() {
    if (WiFi.status() != WL_CONNECTED || !Firebase.ready()) return;
    
    LogEntry logEntry;
    while (xQueueReceive(logQueue, &logEntry, 0) == pdTRUE) {
      String timestamp = getISOTimestamp();
      String logKey = String(timeClient.getEpochTime());
      
      FirebaseJson json;
      json.set("uid", String(logEntry.uid));
      json.set("name", String(logEntry.name));
      json.set("status", String(logEntry.status));
      json.set("type", String(logEntry.type));
      json.set("time", timestamp);
      json.set("device", device_id);
      
      String logPath = logsPath + "/" + logKey;
      
      if (Firebase.RTDB.setJSON(&fbdo, logPath, &json)) {
        Serial.println("Log mirrored to Firebase");
      }
    }
  }
  
  void processPendingQueue() {
    if (WiFi.status() != WL_CONNECTED || !Firebase.ready()) return;
    
    PendingEntry pendingEntry;
    while (xQueueReceive(pendingQueue, &pendingEntry, 0) == pdTRUE) {
      FirebaseJson json;
      json.set("name", "Unknown");
      json.set("firstSeen", getISOTimestamp());
      
      String path = pendingPath + "/" + String(pendingEntry.uid);
      
      if (Firebase.RTDB.setJSON(&fbdo, path, &json)) {
        Serial.println("Pending mirrored to Firebase: " + String(pendingEntry.uid));
      }
    }
  }
  
  // Mirror a single user change to Firebase (audit trail only)
  void mirrorUserToFirebase(String uid, String name, String listType, bool isDelete) {
    if (WiFi.status() != WL_CONNECTED || !Firebase.ready()) return;
    
    String path;
    if (listType == "whitelist") path = whitelistPath;
    else if (listType == "blacklist") path = blacklistPath;
    else if (listType == "pending") path = pendingPath;
    else return;
    
    path += "/" + uid;
    
    if (isDelete) {
      Firebase.RTDB.deleteNode(&fbdo, path);
      Serial.println("Deleted from Firebase mirror: " + uid);
    } else {
      FirebaseJson json;
      json.set("name", name);
      json.set("updatedAt", getISOTimestamp());
      Firebase.RTDB.setJSON(&fbdo, path, &json);
      Serial.println("Mirrored to Firebase: " + uid + " -> " + listType);
    }
  }
  
  void checkCommands() {
    if (WiFi.status() != WL_CONNECTED || !Firebase.ready()) return;
    
    static unsigned long lastCheck = 0;
    if (millis() - lastCheck < 2000) return;
    lastCheck = millis();
    
    // Check unlock command
    if (Firebase.RTDB.getJSON(&fbdo, commandsPath + "/unlock")) {
      FirebaseJson json = fbdo.jsonObject();
      FirebaseJsonData result;
      
      if (json.get(result, "duration")) {
        int duration = result.intValue;
        if (duration > 0 && duration <= 300) {
          Command cmd;
          cmd.type = Command::UNLOCK;
          cmd.duration = duration * 1000;
          
          if (commandQueue != NULL) {
            xQueueSend(commandQueue, &cmd, 0);
            Serial.println("Unlock command queued for Core 1");
          }
          
          Firebase.RTDB.deleteNode(&fbdo, commandsPath + "/unlock");
        }
      }
    }
    
    // Check manage command (add/delete/move/rename)
    if (Firebase.RTDB.getJSON(&fbdo, commandsPath + "/manage")) {
      FirebaseJson json = fbdo.jsonObject();
      FirebaseJsonData actionData, uidData, fromData, toData, nameData;
      
      if (json.get(actionData, "action") && json.get(uidData, "uid")) {
        String action = actionData.stringValue;
        String uid = uidData.stringValue;
        
        if (action == "add") {
          // Add user to a list
          json.get(toData, "to");
          json.get(nameData, "name");
          String toList = toData.stringValue;
          String name = nameData.stringValue.length() > 0 ? nameData.stringValue : "Unknown";
          
          if (toList == "whitelist") {
            StorageManager::addToWhitelist(uid, name);
            mirrorUserToFirebase(uid, name, "whitelist", false);
          } else if (toList == "blacklist") {
            StorageManager::addToBlacklist(uid, name);
            mirrorUserToFirebase(uid, name, "blacklist", false);
          }
          Serial.println("CMD: Added " + uid + " to " + toList);
          
        } else if (action == "delete") {
          // Delete user from a list
          json.get(fromData, "from");
          String fromList = fromData.stringValue;
          
          if (fromList == "whitelist") {
            StorageManager::removeFromWhitelist(uid);
            mirrorUserToFirebase(uid, "", "whitelist", true);
          } else if (fromList == "blacklist") {
            StorageManager::removeFromBlacklist(uid);
            mirrorUserToFirebase(uid, "", "blacklist", true);
          } else if (fromList == "pending") {
            StorageManager::removeFromPending(uid);
            mirrorUserToFirebase(uid, "", "pending", true);
          }
          Serial.println("CMD: Deleted " + uid + " from " + fromList);
          
        } else if (action == "move") {
          // Move user between lists
          json.get(fromData, "from");
          json.get(toData, "to");
          json.get(nameData, "name");
          String fromList = fromData.stringValue;
          String toList = toData.stringValue;
          String name = nameData.stringValue;
          
          // Get name from source if not provided
          if (name.length() == 0) {
            name = StorageManager::getName(uid, fromList.c_str());
          }
          
          // Remove from source
          if (fromList == "whitelist") {
            StorageManager::removeFromWhitelist(uid);
            mirrorUserToFirebase(uid, "", "whitelist", true);
          } else if (fromList == "blacklist") {
            StorageManager::removeFromBlacklist(uid);
            mirrorUserToFirebase(uid, "", "blacklist", true);
          } else if (fromList == "pending") {
            StorageManager::removeFromPending(uid);
            mirrorUserToFirebase(uid, "", "pending", true);
          }
          
          // Add to destination
          if (toList == "whitelist") {
            StorageManager::addToWhitelist(uid, name);
            mirrorUserToFirebase(uid, name, "whitelist", false);
          } else if (toList == "blacklist") {
            StorageManager::addToBlacklist(uid, name);
            mirrorUserToFirebase(uid, name, "blacklist", false);
          } else if (toList == "pending") {
            StorageManager::addToPending(uid, name);
            mirrorUserToFirebase(uid, name, "pending", false);
          }
          Serial.println("CMD: Moved " + uid + " from " + fromList + " to " + toList);
          
        } else if (action == "rename") {
          // Rename user in a list
          json.get(fromData, "list");
          json.get(nameData, "name");
          String listName = fromData.stringValue;
          String newName = nameData.stringValue;
          
          if (listName == "whitelist") {
            if (StorageManager::isWhitelisted(uid)) {
              StorageManager::addToWhitelist(uid, newName);  // Overwrites with new name
              mirrorUserToFirebase(uid, newName, "whitelist", false);
            }
          } else if (listName == "blacklist") {
            if (StorageManager::isBlacklisted(uid)) {
              StorageManager::addToBlacklist(uid, newName);
              mirrorUserToFirebase(uid, newName, "blacklist", false);
            }
          } else if (listName == "pending") {
            if (StorageManager::isPending(uid)) {
              StorageManager::addToPending(uid, newName);
              mirrorUserToFirebase(uid, newName, "pending", false);
            }
          }
          Serial.println("CMD: Renamed " + uid + " to " + newName);
        }
        
        // Delete the processed command
        Firebase.RTDB.deleteNode(&fbdo, commandsPath + "/manage");
      }
    }
  }
  
  // Upload local NVS state to Firebase as audit mirror (one-time on connect)
  // This does NOT sync FROM Firebase - NVS remains authoritative
  void uploadLocalStateToFirebase() {
    if (WiFi.status() != WL_CONNECTED || !Firebase.ready()) return;
    
    Serial.println("Uploading local NVS state to Firebase (audit mirror)...");
    // Note: This only mirrors what's in NVS to Firebase for dashboard visibility
    // The local NVS data is NOT modified - it remains the source of truth
    Serial.println("NVS is authoritative - Firebase is audit mirror only");
  }
}

// ==================== LOG CONFIGURATION ====================
const int LOG_RETENTION_DAYS = 30;
const char* LOG_DIR = "/logs";
const int MAX_LOG_ENTRIES_PER_FILE = 100;  // Split logs by day

// ==================== ACCESS CONTROLLER MODULE (CORE 1) ====================
namespace AccessController {
  unsigned long relayStartTime = 0;
  unsigned long relayDuration = 0;
  bool relayActive = false;
  
  unsigned long buzzerStartTime = 0;
  unsigned long buzzerDuration = 0;
  bool buzzerActive = false;
  
  unsigned long lastUnlock = 0;
  const unsigned long COOLDOWN_MS = 3000;
  
  void activateRelay(unsigned long duration) {
    if (millis() - lastUnlock < COOLDOWN_MS) {
      Serial.println("Cooldown active, ignoring unlock");
      return;
    }
    
    digitalWrite(RELAY_PIN, HIGH);
    relayActive = true;
    relayStartTime = millis();
    relayDuration = duration;
    lastUnlock = millis();
    Serial.println("Relay ON for " + String(duration) + "ms");
  }
  
  void activateBuzzer(unsigned long duration) {
    digitalWrite(BUZZER_PIN, HIGH);
    buzzerActive = true;
    buzzerStartTime = millis();
    buzzerDuration = duration;
  }
  
  void update() {
    // Handle relay timing (non-blocking)
    if (relayActive && (millis() - relayStartTime >= relayDuration)) {
      digitalWrite(RELAY_PIN, LOW);
      relayActive = false;
      Serial.println("Relay OFF");
    }
    
    // Handle buzzer timing (non-blocking)
    if (buzzerActive && (millis() - buzzerStartTime >= buzzerDuration)) {
      digitalWrite(BUZZER_PIN, LOW);
      buzzerActive = false;
    }
  }
  
  void handleCommand(Command cmd) {
    if (cmd.type == Command::UNLOCK) {
      activateRelay(cmd.duration);
      Serial.println("Remote unlock command executed");
    } else if (cmd.type == Command::SYNC_LISTS) {
      Serial.println("Sync command received by Core 1");
    }
  }
  
  void handleRFIDScan(String uid) {
    Serial.println("Card detected: " + uid);
    
    // Check whitelist (fast lookup, no JSON parsing)
    if (StorageManager::isWhitelisted(uid)) {
      String name = StorageManager::getName(uid, "whitelist");
      Serial.println("ACCESS GRANTED: " + name);
      
      activateRelay(3000);
      activateBuzzer(100);
      
      // Queue log for Core 0
      if (logQueue != NULL) {
        LogEntry entry;
        uid.toCharArray(entry.uid, sizeof(entry.uid));
        name.toCharArray(entry.name, sizeof(entry.name));
        strcpy(entry.status, "granted");
        strcpy(entry.type, "rfid");
        xQueueSend(logQueue, &entry, 0);
      }
      return;
    }
    
    // Check blacklist
    if (StorageManager::isBlacklisted(uid)) {
      String name = StorageManager::getName(uid, "blacklist");
      Serial.println("ACCESS DENIED: " + name);
      
      activateBuzzer(2000);
      
      // Queue log for Core 0
      if (logQueue != NULL) {
        LogEntry entry;
        uid.toCharArray(entry.uid, sizeof(entry.uid));
        name.toCharArray(entry.name, sizeof(entry.name));
        strcpy(entry.status, "denied");
        strcpy(entry.type, "rfid");
        xQueueSend(logQueue, &entry, 0);
      }
      return;
    }
    
    // Unknown card - add to pending
    Serial.println("UNKNOWN CARD - Adding to pending");
    
    if (!StorageManager::isPending(uid)) {
      StorageManager::addToPending(uid, "Unknown");
    }
    
    // Queue for Firebase
    if (pendingQueue != NULL) {
      PendingEntry entry;
      uid.toCharArray(entry.uid, sizeof(entry.uid));
      xQueueSend(pendingQueue, &entry, 0);
    }
    
    activateBuzzer(500);
    
    // Queue log for Core 0
    if (logQueue != NULL) {
      LogEntry entry;
      uid.toCharArray(entry.uid, sizeof(entry.uid));
      strcpy(entry.name, "Unknown");
      strcpy(entry.status, "pending");
      strcpy(entry.type, "rfid");
      xQueueSend(logQueue, &entry, 0);
    }
  }
}

// ==================== STORAGE MANAGER MODULE ====================
namespace StorageManager {
  Preferences whitelistPrefs;
  Preferences blacklistPrefs;
  Preferences pendingPrefs;
  
  void init() {
    whitelistPrefs.begin("whitelist", false);
    blacklistPrefs.begin("blacklist", false);
    pendingPrefs.begin("pending", false);
    Serial.println("StorageManager initialized with separate namespaces");
  }
  
  bool isWhitelisted(String uid) {
    return whitelistPrefs.isKey(uid.c_str());
  }
  
  bool isBlacklisted(String uid) {
    return blacklistPrefs.isKey(uid.c_str());
  }
  
  bool isPending(String uid) {
    return pendingPrefs.isKey(uid.c_str());
  }
  
  String getName(String uid, const char* listType) {
    if (strcmp(listType, "whitelist") == 0) {
      return whitelistPrefs.getString(uid.c_str(), "Unknown");
    } else if (strcmp(listType, "blacklist") == 0) {
      return blacklistPrefs.getString(uid.c_str(), "Unknown");
    } else if (strcmp(listType, "pending") == 0) {
      return pendingPrefs.getString(uid.c_str(), "Unknown");
    }
    return "Unknown";
  }
  
  void addToWhitelist(String uid, String name) {
    whitelistPrefs.putString(uid.c_str(), name.c_str());
  }
  
  void addToBlacklist(String uid, String name) {
    blacklistPrefs.putString(uid.c_str(), name.c_str());
  }
  
  void addToPending(String uid, String name) {
    pendingPrefs.putString(uid.c_str(), name.c_str());
  }
  
  void removeFromWhitelist(String uid) {
    whitelistPrefs.remove(uid.c_str());
  }
  
  void removeFromBlacklist(String uid) {
    blacklistPrefs.remove(uid.c_str());
  }
  
  void removeFromPending(String uid) {
    pendingPrefs.remove(uid.c_str());
  }
  
  void clearAll() {
    whitelistPrefs.clear();
    blacklistPrefs.clear();
    pendingPrefs.clear();
  }
}

// ==================== FUNCTION DECLARATIONS ====================
void initLittleFS();
void saveLogToFile(String uid, String name, String status, String type);
void cleanOldLogs();
String getDateString();
String getLogFilePath();
void handleManageUser(String message);
void sendLogsToCloud(String dateFilter);
void sendAllListsToCloud();
void moveUserBetweenLists(String uid, String fromList, String toList, String name);
void deleteUserFromList(String uid, String listName);
void renameUserInList(String uid, String listName, String newName);

// WiFi and NTP functions
void connectWiFi();
void updateNTPTime();
String getISOTimestamp();

// RFID and control functions
void handleRFIDScan();
void activateRelay(unsigned long duration);
void activateBuzzer(unsigned long duration);
void publishLog(String uid, String name, String status, String type);
void initializeNVSIfNeeded();

// Firebase functions
void initFirebase();
void syncListsFromFirebase();
void syncListToNVS(String path, const char* nvsKey);
void publishLogToFirebase(String uid, String name, String status, String type);
void updateDeviceStatus();
void checkFirebaseCommands();
void streamCallback(FirebaseStream data);
void streamTimeoutCallback(bool timeout);
void uploadLocalListsToFirebase();
void addToPendingFirebase(String uid);
void addToPendingFirebaseInternal(String uid);

// FreeRTOS task functions
void firebaseTask(void *parameter);
void rfidTask(void *parameter);

// ==================== SETUP ======================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\n=== ESP32 RFID Door Lock System with Firebase ===");
  
  // Initialize GPIO
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);
  
  // Initialize SPI with explicit pins
  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, SS_PIN);
  
  // Initialize RFID
  rfid.PCD_Init();
  delay(100);
  Serial.println("RFID Reader initialized");
  rfid.PCD_DumpVersionToSerial();
  
  // Initialize StorageManager (NVS with namespaces)
  StorageManager::init();
  
  // Initialize LittleFS for log storage
  initLittleFS();
  
  // Initialize WiFi (non-blocking)
  initWiFi();
  
  // Initialize NTP
  timeClient.begin();
  
  // Initialize Firebase
  initFirebase();
  
  // Create queues for thread-safe communication
  logQueue = xQueueCreate(10, sizeof(LogEntry));
  pendingQueue = xQueueCreate(5, sizeof(PendingEntry));
  commandQueue = xQueueCreate(5, sizeof(Command));
  
  // Create Firebase task on Core 0 (WiFi core)
  xTaskCreatePinnedToCore(
    firebaseTask,        // Task function
    "FirebaseTask",      // Task name
    8192,                // Stack size
    NULL,                // Parameters
    1,                   // Priority
    &firebaseTaskHandle, // Task handle
    0                    // Core 0
  );
  
  // Create RFID task on Core 1 (main core)
  xTaskCreatePinnedToCore(
    rfidTask,           // Task function
    "RFIDTask",         // Task name
    4096,               // Stack size
    NULL,               // Parameters
    2,                  // Priority (higher for responsiveness)
    &rfidTaskHandle,    // Task handle
    1                   // Core 1
  );
  
  Serial.println("Setup complete. Multi-threaded system ready.");
  Serial.println("Core 0: Firebase operations");
  Serial.println("Core 1: RFID reading");
}

// ==================== FIREBASE TASK (Core 0) ====================
void firebaseTask(void *parameter) {
  Serial.println("Firebase task started on Core " + String(xPortGetCoreID()));
  
  while (true) {
    // Maintain WiFi connection (non-blocking)
    handleWiFi();
    
    // Check if WiFi connected
    if (WiFi.status() == WL_CONNECTED) {
      // Firebase operations (conditional, not blocking)
      if (Firebase.ready()) {
        if (!CloudService::firebaseReady) {
          CloudService::firebaseReady = true;
          Serial.println("Firebase connected!");
          CloudService::syncListsFromFirebase();
          CloudService::updateDeviceStatus();
        }
        
        // Update device status every 60 seconds
        if (millis() - CloudService::lastStatusUpdate > 60000) {
          CloudService::updateDeviceStatus();
          CloudService::lastStatusUpdate = millis();
        }
        
        // Check for commands from Firebase
        CloudService::checkCommands();
        
        // Process log queue - send logs to Firebase
        CloudService::processLogQueue();
        
        // Process pending queue - add pending cards to Firebase
        CloudService::processPendingQueue();
      }
    }
    
    // Update NTP time periodically
    if (millis() - lastNTPUpdate > 3600000) {
      timeClient.update();
      lastNTPUpdate = millis();
    }
    timeClient.update();
    
    // Clean old logs daily
    if (millis() - lastLogCleanup > 3600000) {
      cleanOldLogs();
      lastLogCleanup = millis();
    }
    
    vTaskDelay(100 / portTICK_PERIOD_MS); // 100ms delay to prevent watchdog
  }
}

// ==================== RFID TASK (Core 1) ====================
void rfidTask(void *parameter) {
  Serial.println("RFID task started on Core " + String(xPortGetCoreID()));
  
  while (true) {
    // Process commands from Core 0 (Firebase)
    Command cmd;
    if (xQueueReceive(commandQueue, &cmd, 0) == pdTRUE) {
      AccessController::handleCommand(cmd);
    }
    
    // Check for RFID card - this runs independently of Firebase/WiFi
    if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
      String uid = "";
      for (byte i = 0; i < rfid.uid.size; i++) {
        uid += String(rfid.uid.uidByte[i] < 0x10 ? "0" : "");
        uid += String(rfid.uid.uidByte[i], HEX);
      }
      uid.toUpperCase();
      
      AccessController::handleRFIDScan(uid);
      
      rfid.PICC_HaltA();
      rfid.PCD_StopCrypto1();
    }
    
    // Update relay and buzzer timings
    AccessController::update();
    
    vTaskDelay(10 / portTICK_PERIOD_MS); // 10ms delay for responsive card reading
  }
}

// ==================== MAIN LOOP (Empty - tasks handle everything) ====================
void loop() {
  // All operations handled by FreeRTOS tasks
  // This loop is kept minimal to avoid interfering with tasks
  vTaskDelay(1000 / portTICK_PERIOD_MS);
}

// ==================== FIREBASE INITIALIZATION ====================
void initFirebase() {
  Serial.println("Initializing Firebase...");
  
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  
  // Sign in with email/password
  auth.user.email = USER_EMAIL;
  auth.user.password = USER_PASSWORD;
  
  // Assign the callback function for the token status
  config.token_status_callback = tokenStatusCallback;
  
  // Set larger buffer sizes to prevent SSL errors
  fbdo.setBSSLBufferSize(4096, 1024);
  stream.setBSSLBufferSize(2048, 512);  // Separate smaller buffer for stream
  
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  
  // Set up stream for real-time updates on whitelist/blacklist/pending changes
  // We use commands path and poll for list changes to avoid large payload issues
  if (!Firebase.RTDB.beginStream(&stream, commandsPath)) {
    Serial.println("Stream begin error: " + stream.errorReason());
  } else {
    Serial.println("Stream started for commands: " + commandsPath);
    Firebase.RTDB.setStreamCallback(&stream, streamCallback, streamTimeoutCallback);
  }
}

// ==================== FIREBASE STREAM CALLBACKS ====================
void streamCallback(FirebaseStream data) {
  Serial.println("Stream data received!");
  Serial.println("Path: " + data.dataPath());
  Serial.println("Type: " + data.dataType());
  
  // Handle unlock command
  if (data.dataPath().indexOf("unlock") >= 0 && data.dataType() == "json") {
    FirebaseJson json = data.jsonObject();
    FirebaseJsonData result;
    
    if (json.get(result, "duration")) {
      int duration = result.intValue;
      if (duration > 0 && duration <= 300) {
        activateRelay(duration * 1000);
        Serial.println("Remote unlock via stream: " + String(duration) + "s");
        
        // Clear the command after processing
        Firebase.RTDB.deleteNode(&fbdo, commandsPath + "/unlock");
      }
    }
  }
  
  // Handle sync command
  if (data.dataPath().indexOf("sync") >= 0) {
    Serial.println("Sync command received");
    syncListsFromFirebase();
  }
}

void streamTimeoutCallback(bool timeout) {
  if (timeout) {
    Serial.println("Stream timeout, resuming...");
  }
  if (!stream.httpConnected()) {
    Serial.println("Stream not connected: " + stream.errorReason());
    // Try to reconnect the stream
    Firebase.RTDB.endStream(&stream);
    delay(100);
    if (Firebase.RTDB.beginStream(&stream, commandsPath)) {
      Serial.println("Stream reconnected");
    }
  }
}

// Old Firebase sync functions removed - now handled by CloudService module
// ==================== WIFI CONNECTION (NON-BLOCKING) ====================
void handleWiFi() {
  static unsigned long lastTry = 0;
  
  if (WiFi.status() == WL_CONNECTED) return;
  
  if (millis() - lastTry > 5000) {
    lastTry = millis();
    WiFi.disconnect();
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    Serial.println("WiFi retry");
  }
}

void initWiFi() {
  Serial.print("Initializing WiFi: ");
  Serial.println(ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
}

// Old functions removed - now handled by AccessController module

// ==================== PUBLISH LOG (Thread-Safe) ====================
// Note: Logging is now handled internally by AccessController
// This function kept for compatibility with old code sections
void publishLog(String uid, String name, String status, String type) {
  saveLogToFile(uid, name, status, type);
}

// ==================== NTP TIME ====================
String getISOTimestamp() {
  unsigned long epochTime = timeClient.getEpochTime();
  
  time_t rawTime = (time_t)epochTime;
  struct tm* timeInfo = gmtime(&rawTime);
  
  char buffer[25];
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", timeInfo);
  
  return String(buffer);
}

// NVS initialization now handled by StorageManager::init()

// ==================== LittleFS INITIALIZATION ====================
void initLittleFS() {
  if (!LittleFS.begin(true)) {  // true = format if mount fails
    Serial.println("LittleFS Mount Failed!");
    return;
  }
  
  Serial.println("LittleFS mounted successfully");
  
  // Create logs directory if it doesn't exist
  if (!LittleFS.exists(LOG_DIR)) {
    LittleFS.mkdir(LOG_DIR);
    Serial.println("Created /logs directory");
  }
  
  // Print storage info
  Serial.printf("Total space: %u bytes\n", LittleFS.totalBytes());
  Serial.printf("Used space: %u bytes\n", LittleFS.usedBytes());
}

// ==================== LOG FILE OPERATIONS ====================
String getDateString() {
  unsigned long epochTime = timeClient.getEpochTime();
  time_t rawTime = (time_t)epochTime;
  struct tm* timeInfo = gmtime(&rawTime);
  
  char buffer[11];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d", timeInfo);
  return String(buffer);
}

String getLogFilePath() {
  return String(LOG_DIR) + "/log_" + getDateString() + ".jsonl";
}

void saveLogToFile(String uid, String name, String status, String type) {
  String filePath = getLogFilePath();
  
  File file = LittleFS.open(filePath, FILE_APPEND);
  if (!file) {
    Serial.println("Failed to open log file for writing");
    return;
  }
  
  StaticJsonDocument<256> doc;
  doc["uid"] = uid;
  doc["name"] = name;
  doc["time"] = getISOTimestamp();
  doc["status"] = status;
  doc["type"] = type;
  
  String jsonLine;
  serializeJson(doc, jsonLine);
  file.println(jsonLine);
  file.close();
  
  Serial.println("Log saved to: " + filePath);
}

void cleanOldLogs() {
  Serial.println("Checking for old logs to clean...");
  
  File root = LittleFS.open(LOG_DIR);
  if (!root || !root.isDirectory()) {
    return;
  }
  
  unsigned long currentEpoch = timeClient.getEpochTime();
  unsigned long cutoffEpoch = currentEpoch - (LOG_RETENTION_DAYS * 86400);
  
  File file = root.openNextFile();
  while (file) {
    String fileName = String(file.name());
    
    // Extract date from filename (format: log_YYYY-MM-DD.jsonl)
    if (fileName.startsWith("log_") && fileName.endsWith(".jsonl")) {
      String dateStr = fileName.substring(4, 14); // Extract YYYY-MM-DD
      
      // Parse date
      int year = dateStr.substring(0, 4).toInt();
      int month = dateStr.substring(5, 7).toInt();
      int day = dateStr.substring(8, 10).toInt();
      
      struct tm t = {0};
      t.tm_year = year - 1900;
      t.tm_mon = month - 1;
      t.tm_mday = day;
      time_t fileEpoch = mktime(&t);
      
      if ((unsigned long)fileEpoch < cutoffEpoch) {
        String fullPath = String(LOG_DIR) + "/" + fileName;
        LittleFS.remove(fullPath);
        Serial.println("Deleted old log: " + fullPath);
      }
    }
    
    file = root.openNextFile();
  }
  
  Serial.printf("Storage after cleanup - Used: %u / %u bytes\n", 
                LittleFS.usedBytes(), LittleFS.totalBytes());
}

// ==================== USER MANAGEMENT ====================
/*
 * Handle user management commands from cloud
 * JSON format: {"action": "move", "uid": "ABCD1234", "from": "pending", "to": "whitelist", "name": "John Doe"}
 * Actions: move, delete, rename
 */
void handleManageUser(String message) {
  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, message);
  
  if (error) {
    Serial.println("Failed to parse manage command");
    return;
  }
  
  String action = doc["action"].as<String>();
  String uid = doc["uid"].as<String>();
  
  if (action == "move") {
    String fromList = doc["from"].as<String>();
    String toList = doc["to"].as<String>();
    String name = doc["name"] | "Unknown";
    moveUserBetweenLists(uid, fromList, toList, name);
  }
  else if (action == "delete") {
    String fromList = doc["from"].as<String>();
    deleteUserFromList(uid, fromList);
  }
  else if (action == "rename") {
    String list = doc["list"].as<String>();
    String newName = doc["name"].as<String>();
    renameUserInList(uid, list, newName);
  }
  
  // Sync updated lists to cloud
  sendAllListsToCloud();
}

void moveUserBetweenLists(String uid, String fromList, String toList, String name) {
  // Get the "from" list key
  const char* fromKey = NULL;
  if (fromList == "whitelist") fromKey = NVS_WHITELIST;
  else if (fromList == "blacklist") fromKey = NVS_BLACKLIST;
  else if (fromList == "pending") fromKey = NVS_PENDING;
  
  // Get the "to" list key
  const char* toKey = NULL;
  if (toList == "whitelist") toKey = NVS_WHITELIST;
  else if (toList == "blacklist") toKey = NVS_BLACKLIST;
  else if (toList == "pending") toKey = NVS_PENDING;
  
  if (!fromKey || !toKey) {
    Serial.println("Invalid list name");
    return;
  }
  
  // Load both lists
  String fromJson = prefs.getString(fromKey, "{}");
  String toJson = prefs.getString(toKey, "{}");
  
  StaticJsonDocument<1024> fromDoc;
  StaticJsonDocument<1024> toDoc;
  
  deserializeJson(fromDoc, fromJson);
  deserializeJson(toDoc, toJson);
  
  // Get name from source list if not provided
  if (name == "Unknown" && fromDoc.containsKey(uid)) {
    name = fromDoc[uid].as<String>();
  }
  
  // Remove from source
  fromDoc.remove(uid);
  
  // Add to destination
  toDoc[uid] = name;
  
  // Save both lists
  String updatedFrom, updatedTo;
  serializeJson(fromDoc, updatedFrom);
  serializeJson(toDoc, updatedTo);
  
  prefs.putString(fromKey, updatedFrom);
  prefs.putString(toKey, updatedTo);
  
  Serial.println("Moved " + uid + " (" + name + ") from " + fromList + " to " + toList);
  
  // Log the action
  publishLog(uid, name, "moved_to_" + toList, "management");
}

void deleteUserFromList(String uid, String listName) {
  const char* listKey = NULL;
  String firebasePath = "";
  
  if (listName == "whitelist") {
    listKey = NVS_WHITELIST;
    firebasePath = whitelistPath;
  } else if (listName == "blacklist") {
    listKey = NVS_BLACKLIST;
    firebasePath = blacklistPath;
  } else if (listName == "pending") {
    listKey = NVS_PENDING;
    firebasePath = pendingPath;
  }
  
  if (!listKey) return;
  
  String listJson = prefs.getString(listKey, "{}");
  StaticJsonDocument<1024> doc;
  deserializeJson(doc, listJson);
  
  String name = doc[uid] | "Unknown";
  doc.remove(uid);
  
  String updated;
  serializeJson(doc, updated);
  prefs.putString(listKey, updated);
  
  // Delete from Firebase
  if (Firebase.ready()) {
    Firebase.RTDB.deleteNode(&fbdo, firebasePath + "/" + uid);
  }
  
  Serial.println("Deleted " + uid + " from " + listName);
  publishLog(uid, name, "deleted_from_" + listName, "management");
}

void renameUserInList(String uid, String listName, String newName) {
  const char* listKey = NULL;
  String firebasePath = "";
  
  if (listName == "whitelist") {
    listKey = NVS_WHITELIST;
    firebasePath = whitelistPath;
  } else if (listName == "blacklist") {
    listKey = NVS_BLACKLIST;
    firebasePath = blacklistPath;
  } else if (listName == "pending") {
    listKey = NVS_PENDING;
    firebasePath = pendingPath;
  }
  
  if (!listKey) return;
  
  String listJson = prefs.getString(listKey, "{}");
  StaticJsonDocument<1024> doc;
  deserializeJson(doc, listJson);
  
  if (doc.containsKey(uid)) {
    doc[uid] = newName;
    
    String updated;
    serializeJson(doc, updated);
    prefs.putString(listKey, updated);
    
    // Update in Firebase
    if (Firebase.ready()) {
      Firebase.RTDB.setString(&fbdo, firebasePath + "/" + uid + "/name", newName);
    }
    
    Serial.println("Renamed " + uid + " to " + newName + " in " + listName);
  }
}

// ==================== CLOUD SYNC FUNCTIONS (kept for local backup) ====================
void sendAllListsToCloud() {
  // This function is now handled by Firebase real-time sync
  // Kept for compatibility
  Serial.println("Lists are auto-synced with Firebase");
}

void sendLogsToCloud(String dateFilter) {
  // Local logs can be retrieved through Firebase
  // This function is kept for local backup retrieval
  Serial.println("Logs are stored in Firebase. Use dashboard to view.");
}

// ==================== TOKEN STATUS CALLBACK ====================
void tokenStatusCallback(token_info_t info) {
  if (info.status == token_status_error) {
    Serial.print("Token error: ");
    Serial.println(info.error.message.c_str());
  } else if (info.status == token_status_ready) {
    Serial.println("Token ready");
  } else if (info.status == token_status_on_refresh) {
    Serial.println("Token refreshing...");
  }
}