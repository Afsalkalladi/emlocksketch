/*
 * ESP32 RFID Door Lock System with Firebase Cloud
 * Version: 2.0 (Architectural Rewrite)
 * 
 * Hardware: MFRC522 RFID, Relay (EM Lock), Buzzer, Exit Sensor
 * Features: Firebase Realtime DB, NVS Storage (Authoritative), NTP Timestamps, 
 *           LittleFS Local Logs (30 days), Cloud Commands, User Management
 * 
 * ============================================================================
 * ARCHITECTURE PRINCIPLES:
 * ============================================================================
 * 
 * 1. NVS (ESP32 local storage) is AUTHORITATIVE for access decisions
 *    - If cloud dies → door still works
 *    - If ESP32 reboots → door still works (NVS persists)
 * 
 * 2. Firebase is a COMMAND INBOX + AUDIT LOG (not a mirror!)
 *    - Commands: add, delete, move, rename, unlock
 *    - Audit: logs of access events
 *    - Status: device health reporting
 *    - NOTE: Firebase state may be stale after reboot - this is acceptable
 * 
 * 3. Cloud is NOT allowed to:
 *    - Overwrite lists directly
 *    - Resync full JSON trees to device
 *    - "Rebuild" local state from cloud
 * 
 * 4. Thread Safety:
 *    - NVS mutex protects all Preferences access
 *    - Mutex created BEFORE any NVS operations
 *    - Preferences opened/closed per operation (safe pattern)
 * 
 * 5. Command Processing:
 *    - Pure polling (no stream) for simplicity and reliability
 *    - Validate BEFORE delete (prevents silent data loss)
 *    - Error status written back to Firebase on invalid commands
 * 
 * ============================================================================
 * PIN CONFIGURATION:
 * ============================================================================
 * MFRC522: SDA=21, RST=22, SCK=18, MOSI=23, MISO=19
 * Relay: GPIO 25 (Active HIGH)
 * Buzzer: GPIO 27
 * Exit Sensor: GPIO 26 (NC switch, LOW=idle, HIGH=hand detected)
 * 
 * ============================================================================
 * FIREBASE STRUCTURE:
 * ============================================================================
 * /devices/device1/whitelist/{uid}: {name, addedAt}
 * /devices/device1/blacklist/{uid}: {name, addedAt}
 * /devices/device1/pending/{uid}: {name, firstSeen}
 * /devices/device1/logs/{timestamp}: {uid, name, status, type, time}
 * /devices/device1/commands/unlock: {duration, timestamp}
 * /devices/device1/commands/add: {list, uid, name, timestamp}
 * /devices/device1/commands/delete: {list, uid, timestamp}
 * /devices/device1/commands/move: {from, to, uid, name, timestamp}
 * /devices/device1/commands/rename: {list, uid, name, timestamp}
 * /devices/device1/commandErrors/{timestamp}: {command, error, originalData}
 * /devices/device1/status: {online, lastSeen, ip, rssi, freeHeap, uptime, queueDrops}
 * 
 * ============================================================================
 * REQUIRED LIBRARIES (Install via Arduino Library Manager):
 * ============================================================================
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
#include <esp_task_wdt.h>

// Token status callback function
void tokenStatusCallback(token_info_t info);

// ==================== PIN DEFINITIONS ====================
#define SS_PIN 21
#define RST_PIN 22
#define RELAY_PIN 25
#define BUZZER_PIN 27
#define EXIT_SENSOR_PIN 26  // NC switch: LOW=idle, HIGH=hand detected

// SPI Pins
#define SCK_PIN 18
#define MOSI_PIN 23
#define MISO_PIN 19

// ==================== WIFI CREDENTIALS ====================
const char* ssid = "Airtel_SKETCH";           // CHANGE THIS
const char* password = "Sketch@123";          // CHANGE THIS

// ==================== FIREBASE CONFIGURATION ====================
#define API_KEY "AIzaSyDFTyL7DjvdcDI_D70ZWMgcgXuTtnxGRnE"
#define DATABASE_URL "https://emlocksketch-default-rtdb.asia-southeast1.firebasedatabase.app"
#define USER_EMAIL "afsalkalladi@gmail.com"
#define USER_PASSWORD "Qawsed1w2e3r"

const char* device_id = "device1";

// Firebase paths
String basePath = "/devices/" + String(device_id);
String whitelistPath = basePath + "/whitelist";
String blacklistPath = basePath + "/blacklist";
String pendingPath = basePath + "/pending";
String logsPath = basePath + "/logs";
String commandsPath = basePath + "/commands";
String commandErrorsPath = basePath + "/commandErrors";
String statusPath = basePath + "/status";

// ==================== GLOBAL OBJECTS ====================
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

MFRC522 rfid(SS_PIN, RST_PIN);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800, 60000); // IST (UTC+5:30)

// ==================== TIMING CONSTANTS ====================
const unsigned long NTP_UPDATE_INTERVAL = 3600000;      // 1 hour
const unsigned long LOG_CLEANUP_INTERVAL = 86400000;    // 24 hours
const unsigned long STATUS_UPDATE_INTERVAL = 60000;     // 1 minute
const unsigned long COMMAND_CHECK_INTERVAL = 2000;      // 2 seconds
const unsigned long WIFI_RETRY_INTERVAL = 5000;         // 5 seconds
const unsigned long EXIT_DEBOUNCE_MS = 500;             // 500ms debounce for exit sensor
const unsigned long UNLOCK_COOLDOWN_MS = 3000;          // 3 second cooldown between unlocks
const unsigned long RELAY_DEFAULT_DURATION = 3000;      // 3 second default unlock
const int LOG_RETENTION_DAYS = 30;

// ==================== TIMING VARIABLES ====================
unsigned long lastNTPUpdate = 0;
unsigned long lastLogCleanup = 0;

// ==================== FREERTOS OBJECTS ====================
TaskHandle_t firebaseTaskHandle = NULL;
TaskHandle_t rfidTaskHandle = NULL;
QueueHandle_t logQueue = NULL;
QueueHandle_t pendingQueue = NULL;
QueueHandle_t commandQueue = NULL;
SemaphoreHandle_t nvsMutex = NULL;  // MUST be created before ANY NVS access

// Queue overflow counters (for diagnostics)
volatile uint32_t logQueueDrops = 0;
volatile uint32_t pendingQueueDrops = 0;
volatile uint32_t commandQueueDrops = 0;

// ==================== DATA STRUCTURES ====================
struct LogEntry {
  char uid[24];      // Max 20 chars + null + padding
  char name[50];
  char status[16];
  char type[16];
};

struct PendingEntry {
  char uid[24];
};

struct Command {
  enum Type { UNLOCK, NONE } type;
  uint32_t duration;
};

// ==================== FORWARD DECLARATIONS ====================
String getISOTimestamp();
unsigned long getEpochTime();

// ==================== INPUT VALIDATION MODULE ====================
namespace Validation {
  // UID: hex string, even length, 8-20 characters (4-10 byte cards)
  const int MIN_UID_LENGTH = 8;
  const int MAX_UID_LENGTH = 20;
  const int MAX_NAME_LENGTH = 45;
  
  bool isValidUID(const String& uid) {
    int len = uid.length();
    
    // Check length bounds
    if (len < MIN_UID_LENGTH || len > MAX_UID_LENGTH) return false;
    
    // Must be even length (hex representation of bytes)
    if (len % 2 != 0) return false;
    
    // Only hex characters allowed
    for (int i = 0; i < len; i++) {
      char c = uid.charAt(i);
      if (!isxdigit(c)) return false;
    }
    return true;
  }
  
  String sanitizeName(const String& name) {
    String sanitized = name;
    sanitized.trim();
    if (sanitized.length() > MAX_NAME_LENGTH) {
      sanitized = sanitized.substring(0, MAX_NAME_LENGTH);
    }
    if (sanitized.length() == 0) {
      sanitized = "Unknown";
    }
    return sanitized;
  }
  
  bool isValidList(const String& list) {
    return (list == "whitelist" || list == "blacklist" || list == "pending");
  }
  
  bool isValidDuration(int duration) {
    return (duration > 0 && duration <= 300);  // 1-300 seconds
  }
}

// ==================== STORAGE MANAGER MODULE ====================
// Thread-safe NVS access with open/close per operation pattern
// This prevents flash handle corruption during long-running operations
namespace StorageManager {
  
  // Read operations - open/close per call for safety
  bool isWhitelisted(const String& uid) {
    if (nvsMutex == NULL) return false;
    if (xSemaphoreTake(nvsMutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
    
    Preferences prefs;
    bool result = false;
    if (prefs.begin("whitelist", true)) {  // true = read-only
      result = prefs.isKey(uid.c_str());
      prefs.end();
    }
    
    xSemaphoreGive(nvsMutex);
    return result;
  }
  
  bool isBlacklisted(const String& uid) {
    if (nvsMutex == NULL) return false;
    if (xSemaphoreTake(nvsMutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
    
    Preferences prefs;
    bool result = false;
    if (prefs.begin("blacklist", true)) {
      result = prefs.isKey(uid.c_str());
      prefs.end();
    }
    
    xSemaphoreGive(nvsMutex);
    return result;
  }
  
  bool isPending(const String& uid) {
    if (nvsMutex == NULL) return false;
    if (xSemaphoreTake(nvsMutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
    
    Preferences prefs;
    bool result = false;
    if (prefs.begin("pending", true)) {
      result = prefs.isKey(uid.c_str());
      prefs.end();
    }
    
    xSemaphoreGive(nvsMutex);
    return result;
  }
  
  String getName(const String& uid, const char* listType) {
    if (nvsMutex == NULL) return "Unknown";
    if (xSemaphoreTake(nvsMutex, pdMS_TO_TICKS(1000)) != pdTRUE) return "Unknown";
    
    Preferences prefs;
    String result = "Unknown";
    if (prefs.begin(listType, true)) {
      result = prefs.getString(uid.c_str(), "Unknown");
      prefs.end();
    }
    
    xSemaphoreGive(nvsMutex);
    return result;
  }
  
  // Write operations
  bool addToWhitelist(const String& uid, const String& name) {
    if (nvsMutex == NULL) return false;
    if (xSemaphoreTake(nvsMutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
    
    Preferences prefs;
    bool result = false;
    if (prefs.begin("whitelist", false)) {
      result = prefs.putString(uid.c_str(), name.c_str()) > 0;
      prefs.end();
    }
    
    xSemaphoreGive(nvsMutex);
    return result;
  }
  
  bool addToBlacklist(const String& uid, const String& name) {
    if (nvsMutex == NULL) return false;
    if (xSemaphoreTake(nvsMutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
    
    Preferences prefs;
    bool result = false;
    if (prefs.begin("blacklist", false)) {
      result = prefs.putString(uid.c_str(), name.c_str()) > 0;
      prefs.end();
    }
    
    xSemaphoreGive(nvsMutex);
    return result;
  }
  
  bool addToPending(const String& uid, const String& name) {
    if (nvsMutex == NULL) return false;
    if (xSemaphoreTake(nvsMutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
    
    Preferences prefs;
    bool result = false;
    if (prefs.begin("pending", false)) {
      result = prefs.putString(uid.c_str(), name.c_str()) > 0;
      prefs.end();
    }
    
    xSemaphoreGive(nvsMutex);
    return result;
  }
  
  bool removeFromWhitelist(const String& uid) {
    if (nvsMutex == NULL) return false;
    if (xSemaphoreTake(nvsMutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
    
    Preferences prefs;
    bool result = false;
    if (prefs.begin("whitelist", false)) {
      result = prefs.remove(uid.c_str());
      prefs.end();
    }
    
    xSemaphoreGive(nvsMutex);
    return result;
  }
  
  bool removeFromBlacklist(const String& uid) {
    if (nvsMutex == NULL) return false;
    if (xSemaphoreTake(nvsMutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
    
    Preferences prefs;
    bool result = false;
    if (prefs.begin("blacklist", false)) {
      result = prefs.remove(uid.c_str());
      prefs.end();
    }
    
    xSemaphoreGive(nvsMutex);
    return result;
  }
  
  bool removeFromPending(const String& uid) {
    if (nvsMutex == NULL) return false;
    if (xSemaphoreTake(nvsMutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
    
    Preferences prefs;
    bool result = false;
    if (prefs.begin("pending", false)) {
      result = prefs.remove(uid.c_str());
      prefs.end();
    }
    
    xSemaphoreGive(nvsMutex);
    return result;
  }
  
  // Generic operations by list name
  bool addToList(const String& list, const String& uid, const String& name) {
    if (list == "whitelist") return addToWhitelist(uid, name);
    if (list == "blacklist") return addToBlacklist(uid, name);
    if (list == "pending") return addToPending(uid, name);
    return false;
  }
  
  bool removeFromList(const String& list, const String& uid) {
    if (list == "whitelist") return removeFromWhitelist(uid);
    if (list == "blacklist") return removeFromBlacklist(uid);
    if (list == "pending") return removeFromPending(uid);
    return false;
  }
}

// ==================== CLOUD SERVICE MODULE (CORE 0) ====================
// Pure polling architecture - no streams for simplicity and reliability
namespace CloudService {
  unsigned long lastStatusUpdate = 0;
  unsigned long lastCommandCheck = 0;
  bool firebaseReady = false;
  
  // Report command error back to Firebase for dashboard visibility
  void reportCommandError(const String& command, const String& error, const String& originalData) {
    if (!Firebase.ready()) return;
    
    FirebaseJson errorJson;
    errorJson.set("command", command);
    errorJson.set("error", error);
    errorJson.set("originalData", originalData);
    errorJson.set("timestamp", getISOTimestamp());
    
    String errorPath = commandErrorsPath + "/" + String(getEpochTime());
    Firebase.RTDB.setJSON(&fbdo, errorPath, &errorJson);
    
    Serial.println("Command error reported: " + command + " - " + error);
  }
  
  void updateDeviceStatus() {
    if (WiFi.status() != WL_CONNECTED || !Firebase.ready()) return;
    
    FirebaseJson json;
    json.set("online", true);
    json.set("lastSeen", getISOTimestamp());
    json.set("ip", WiFi.localIP().toString());
    json.set("rssi", WiFi.RSSI());
    json.set("freeHeap", ESP.getFreeHeap());
    json.set("uptime", millis() / 1000);
    json.set("logQueueDrops", (int)logQueueDrops);
    json.set("pendingQueueDrops", (int)pendingQueueDrops);
    json.set("firmwareVersion", "2.0");
    
    if (Firebase.RTDB.setJSON(&fbdo, statusPath, &json)) {
      Serial.println("Device status updated");
    } else {
      Serial.println("Status update FAILED: " + fbdo.errorReason());
    }
  }
  
  void processLogQueue() {
    if (WiFi.status() != WL_CONNECTED || !Firebase.ready()) return;
    
    LogEntry logEntry;
    int processed = 0;
    const int MAX_BATCH = 5;  // Process max 5 logs per cycle to prevent blocking
    
    while (processed < MAX_BATCH && xQueueReceive(logQueue, &logEntry, 0) == pdTRUE) {
      String logKey = String(getEpochTime()) + "_" + String(processed);
      
      FirebaseJson json;
      json.set("uid", String(logEntry.uid));
      json.set("name", String(logEntry.name));
      json.set("status", String(logEntry.status));
      json.set("type", String(logEntry.type));
      json.set("time", getISOTimestamp());
      json.set("device", device_id);
      
      String logPath = logsPath + "/" + logKey;
      
      if (Firebase.RTDB.setJSON(&fbdo, logPath, &json)) {
        Serial.println("Log uploaded: " + String(logEntry.uid));
      } else {
        Serial.println("Log upload FAILED: " + fbdo.errorReason());
      }
      processed++;
      
      // Yield to prevent watchdog
      vTaskDelay(10 / portTICK_PERIOD_MS);
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
        Serial.println("Added to pending in Firebase: " + String(pendingEntry.uid));
      } else {
        Serial.println("Pending upload FAILED: " + fbdo.errorReason());
      }
      
      vTaskDelay(10 / portTICK_PERIOD_MS);
    }
  }
  
  // Process unlock command
  void handleUnlockCommand() {
    if (!Firebase.RTDB.getJSON(&fbdo, commandsPath + "/unlock")) return;
    
    FirebaseJson json = fbdo.jsonObject();
    FirebaseJsonData durationData;
    
    if (!json.get(durationData, "duration")) return;
    
    int duration = durationData.intValue;
    String originalData = "duration=" + String(duration);
    
    // VALIDATE FIRST (before delete)
    if (!Validation::isValidDuration(duration)) {
      reportCommandError("unlock", "Invalid duration (must be 1-300)", originalData);
      Firebase.RTDB.deleteNode(&fbdo, commandsPath + "/unlock");
      return;
    }
    
    // DELETE command
    if (!Firebase.RTDB.deleteNode(&fbdo, commandsPath + "/unlock")) {
      Serial.println("Failed to delete unlock command - skipping");
      return;
    }
    
    // EXECUTE
    Command cmd;
    cmd.type = Command::UNLOCK;
    cmd.duration = duration * 1000;
    
    if (commandQueue != NULL) {
      if (xQueueSend(commandQueue, &cmd, 0) != pdTRUE) {
        commandQueueDrops++;
      } else {
        Serial.println("Unlock command queued: " + String(duration) + "s");
      }
    }
  }
  
  // Process add command
  void handleAddCommand() {
    if (!Firebase.RTDB.getJSON(&fbdo, commandsPath + "/add")) return;
    
    FirebaseJson json = fbdo.jsonObject();
    FirebaseJsonData listData, uidData, nameData;
    
    if (!json.get(listData, "list") || !json.get(uidData, "uid")) return;
    
    String list = listData.stringValue;
    String uid = uidData.stringValue;
    uid.toUpperCase();
    String name = "Unknown";
    if (json.get(nameData, "name")) name = Validation::sanitizeName(nameData.stringValue);
    
    String originalData = "list=" + list + ", uid=" + uid + ", name=" + name;
    
    // VALIDATE FIRST
    if (!Validation::isValidList(list)) {
      reportCommandError("add", "Invalid list type", originalData);
      Firebase.RTDB.deleteNode(&fbdo, commandsPath + "/add");
      return;
    }
    
    if (!Validation::isValidUID(uid)) {
      reportCommandError("add", "Invalid UID format (must be 8-20 hex chars, even length)", originalData);
      Firebase.RTDB.deleteNode(&fbdo, commandsPath + "/add");
      return;
    }
    
    // DELETE command
    if (!Firebase.RTDB.deleteNode(&fbdo, commandsPath + "/add")) {
      Serial.println("Failed to delete add command - skipping");
      return;
    }
    
    // EXECUTE - NVS first (authoritative)
    if (!StorageManager::addToList(list, uid, name)) {
      reportCommandError("add", "NVS write failed", originalData);
      return;
    }
    
    // Mirror to Firebase (audit log)
    FirebaseJson userJson;
    userJson.set("name", name);
    userJson.set(list == "pending" ? "firstSeen" : "addedAt", getISOTimestamp());
    
    String fbPath = basePath + "/" + list + "/" + uid;
    Firebase.RTDB.setJSON(&fbdo, fbPath, &userJson);
    
    Serial.println("Add command processed: " + uid + " -> " + list);
  }
  
  // Process delete command
  void handleDeleteCommand() {
    if (!Firebase.RTDB.getJSON(&fbdo, commandsPath + "/delete")) return;
    
    FirebaseJson json = fbdo.jsonObject();
    FirebaseJsonData listData, uidData;
    
    if (!json.get(listData, "list") || !json.get(uidData, "uid")) return;
    
    String list = listData.stringValue;
    String uid = uidData.stringValue;
    uid.toUpperCase();
    
    String originalData = "list=" + list + ", uid=" + uid;
    
    // VALIDATE FIRST
    if (!Validation::isValidList(list)) {
      reportCommandError("delete", "Invalid list type", originalData);
      Firebase.RTDB.deleteNode(&fbdo, commandsPath + "/delete");
      return;
    }
    
    if (!Validation::isValidUID(uid)) {
      reportCommandError("delete", "Invalid UID format", originalData);
      Firebase.RTDB.deleteNode(&fbdo, commandsPath + "/delete");
      return;
    }
    
    // DELETE command
    if (!Firebase.RTDB.deleteNode(&fbdo, commandsPath + "/delete")) {
      Serial.println("Failed to delete delete command - skipping");
      return;
    }
    
    // EXECUTE
    StorageManager::removeFromList(list, uid);
    
    String fbPath = basePath + "/" + list + "/" + uid;
    Firebase.RTDB.deleteNode(&fbdo, fbPath);
    
    Serial.println("Delete command processed: " + uid + " from " + list);
  }
  
  // Process move command
  void handleMoveCommand() {
    if (!Firebase.RTDB.getJSON(&fbdo, commandsPath + "/move")) return;
    
    FirebaseJson json = fbdo.jsonObject();
    FirebaseJsonData fromData, toData, uidData, nameData;
    
    if (!json.get(fromData, "from") || !json.get(toData, "to") || !json.get(uidData, "uid")) return;
    
    String fromList = fromData.stringValue;
    String toList = toData.stringValue;
    String uid = uidData.stringValue;
    uid.toUpperCase();
    String name = "Unknown";
    
    if (json.get(nameData, "name")) {
      name = Validation::sanitizeName(nameData.stringValue);
    } else {
      name = StorageManager::getName(uid, fromList.c_str());
    }
    
    String originalData = "from=" + fromList + ", to=" + toList + ", uid=" + uid;
    
    // VALIDATE FIRST
    if (!Validation::isValidList(fromList) || !Validation::isValidList(toList)) {
      reportCommandError("move", "Invalid list type", originalData);
      Firebase.RTDB.deleteNode(&fbdo, commandsPath + "/move");
      return;
    }
    
    if (!Validation::isValidUID(uid)) {
      reportCommandError("move", "Invalid UID format", originalData);
      Firebase.RTDB.deleteNode(&fbdo, commandsPath + "/move");
      return;
    }
    
    if (fromList == toList) {
      reportCommandError("move", "Source and destination are the same", originalData);
      Firebase.RTDB.deleteNode(&fbdo, commandsPath + "/move");
      return;
    }
    
    // DELETE command
    if (!Firebase.RTDB.deleteNode(&fbdo, commandsPath + "/move")) {
      Serial.println("Failed to delete move command - skipping");
      return;
    }
    
    // EXECUTE - NVS operations
    StorageManager::removeFromList(fromList, uid);
    StorageManager::addToList(toList, uid, name);
    
    // Mirror to Firebase
    Firebase.RTDB.deleteNode(&fbdo, basePath + "/" + fromList + "/" + uid);
    
    FirebaseJson userJson;
    userJson.set("name", name);
    userJson.set(toList == "pending" ? "firstSeen" : "addedAt", getISOTimestamp());
    Firebase.RTDB.setJSON(&fbdo, basePath + "/" + toList + "/" + uid, &userJson);
    
    Serial.println("Move command processed: " + uid + " from " + fromList + " to " + toList);
  }
  
  // Process rename command
  void handleRenameCommand() {
    if (!Firebase.RTDB.getJSON(&fbdo, commandsPath + "/rename")) return;
    
    FirebaseJson json = fbdo.jsonObject();
    FirebaseJsonData listData, uidData, nameData;
    
    if (!json.get(listData, "list") || !json.get(uidData, "uid") || !json.get(nameData, "name")) return;
    
    String list = listData.stringValue;
    String uid = uidData.stringValue;
    uid.toUpperCase();
    String newName = Validation::sanitizeName(nameData.stringValue);
    
    String originalData = "list=" + list + ", uid=" + uid + ", name=" + newName;
    
    // VALIDATE FIRST
    if (!Validation::isValidList(list)) {
      reportCommandError("rename", "Invalid list type", originalData);
      Firebase.RTDB.deleteNode(&fbdo, commandsPath + "/rename");
      return;
    }
    
    if (!Validation::isValidUID(uid)) {
      reportCommandError("rename", "Invalid UID format", originalData);
      Firebase.RTDB.deleteNode(&fbdo, commandsPath + "/rename");
      return;
    }
    
    // DELETE command
    if (!Firebase.RTDB.deleteNode(&fbdo, commandsPath + "/rename")) {
      Serial.println("Failed to delete rename command - skipping");
      return;
    }
    
    // EXECUTE
    StorageManager::addToList(list, uid, newName);  // Overwrites existing
    Firebase.RTDB.setString(&fbdo, basePath + "/" + list + "/" + uid + "/name", newName);
    
    Serial.println("Rename command processed: " + uid + " -> " + newName);
  }
  
  // Main command check function
  void checkCommands() {
    if (WiFi.status() != WL_CONNECTED || !Firebase.ready()) return;
    
    if (millis() - lastCommandCheck < COMMAND_CHECK_INTERVAL) return;
    lastCommandCheck = millis();
    
    // Process each command type with yield between them
    handleUnlockCommand();
    vTaskDelay(10 / portTICK_PERIOD_MS);
    
    handleAddCommand();
    vTaskDelay(10 / portTICK_PERIOD_MS);
    
    handleDeleteCommand();
    vTaskDelay(10 / portTICK_PERIOD_MS);
    
    handleMoveCommand();
    vTaskDelay(10 / portTICK_PERIOD_MS);
    
    handleRenameCommand();
  }
}

// ==================== LOG CONFIGURATION ====================
const char* LOG_DIR = "/logs";

// ==================== ACCESS CONTROLLER MODULE (CORE 1) ====================
namespace AccessController {
  unsigned long relayStartTime = 0;
  unsigned long relayDuration = 0;
  bool relayActive = false;
  
  unsigned long buzzerStartTime = 0;
  unsigned long buzzerDuration = 0;
  bool buzzerActive = false;
  
  unsigned long lastUnlock = 0;
  
  // Exit sensor with proper debounce (independent of relay state)
  bool lastExitSensorState = false;
  bool exitSensorTriggered = false;
  unsigned long lastExitTriggerTime = 0;
  
  void activateBuzzer(unsigned long duration) {
    digitalWrite(BUZZER_PIN, HIGH);
    buzzerActive = true;
    buzzerStartTime = millis();
    buzzerDuration = duration;
  }
  
  void activateRelay(unsigned long duration) {
    // Cooldown check - independent of current relay state
    if (millis() - lastUnlock < UNLOCK_COOLDOWN_MS) {
      Serial.println("Cooldown active, ignoring unlock request");
      return;
    }
    
    digitalWrite(RELAY_PIN, HIGH);
    relayActive = true;
    relayStartTime = millis();
    relayDuration = duration;
    lastUnlock = millis();
    Serial.println("Relay ON for " + String(duration) + "ms");
  }
  
  // Exit sensor handling with hardware debounce protection
  void handleExitSensor() {
    bool currentState = digitalRead(EXIT_SENSOR_PIN) == HIGH;
    unsigned long now = millis();
    
    // Debounce: ignore state changes within EXIT_DEBOUNCE_MS
    if (currentState != lastExitSensorState) {
      // State changed - check if enough time has passed since last trigger
      if (now - lastExitTriggerTime < EXIT_DEBOUNCE_MS) {
        // Bounce detected, ignore
        return;
      }
    }
    
    // Edge detection: trigger only on LOW -> HIGH transition
    if (currentState && !lastExitSensorState && !exitSensorTriggered) {
      exitSensorTriggered = true;
      lastExitTriggerTime = now;
      
      // Unlock door
      activateRelay(RELAY_DEFAULT_DURATION);
      activateBuzzer(100);
      
      Serial.println("EXIT SENSOR: Door unlocked (debounced edge-triggered)");
      
      // Queue log
      if (logQueue != NULL) {
        LogEntry entry;
        strcpy(entry.uid, "EXIT_SENSOR");
        strcpy(entry.name, "Exit Button");
        strcpy(entry.status, "granted");
        strcpy(entry.type, "exit");
        
        if (xQueueSend(logQueue, &entry, 0) != pdTRUE) {
          logQueueDrops++;
        }
      }
    }
    
    // Reset latch when sensor returns to idle
    // Use time-based reset to prevent rapid re-triggering
    if (!currentState && exitSensorTriggered) {
      if (now - lastExitTriggerTime > EXIT_DEBOUNCE_MS) {
        exitSensorTriggered = false;
      }
    }
    
    lastExitSensorState = currentState;
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
      activateBuzzer(100);
      Serial.println("Remote unlock executed: " + String(cmd.duration) + "ms");
    }
  }
  
  void handleRFIDScan(const String& uid) {
    Serial.println("Card detected: " + uid);
    
    // Validate UID format first
    if (!Validation::isValidUID(uid)) {
      Serial.println("Invalid UID format detected: " + uid);
      activateBuzzer(200);
      return;
    }
    
    // Check whitelist
    if (StorageManager::isWhitelisted(uid)) {
      String name = StorageManager::getName(uid, "whitelist");
      Serial.println("ACCESS GRANTED: " + name);
      
      activateRelay(RELAY_DEFAULT_DURATION);
      activateBuzzer(100);
      
      if (logQueue != NULL) {
        LogEntry entry;
        uid.toCharArray(entry.uid, sizeof(entry.uid));
        name.toCharArray(entry.name, sizeof(entry.name));
        strcpy(entry.status, "granted");
        strcpy(entry.type, "rfid");
        
        if (xQueueSend(logQueue, &entry, 0) != pdTRUE) {
          logQueueDrops++;
        }
      }
      return;
    }
    
    // Check blacklist
    if (StorageManager::isBlacklisted(uid)) {
      String name = StorageManager::getName(uid, "blacklist");
      Serial.println("ACCESS DENIED (blacklisted): " + name);
      
      activateBuzzer(2000);
      
      if (logQueue != NULL) {
        LogEntry entry;
        uid.toCharArray(entry.uid, sizeof(entry.uid));
        name.toCharArray(entry.name, sizeof(entry.name));
        strcpy(entry.status, "denied");
        strcpy(entry.type, "rfid");
        
        if (xQueueSend(logQueue, &entry, 0) != pdTRUE) {
          logQueueDrops++;
        }
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
      
      if (xQueueSend(pendingQueue, &entry, 0) != pdTRUE) {
        pendingQueueDrops++;
      }
    }
    
    activateBuzzer(500);
    
    if (logQueue != NULL) {
      LogEntry entry;
      uid.toCharArray(entry.uid, sizeof(entry.uid));
      strcpy(entry.name, "Unknown");
      strcpy(entry.status, "pending");
      strcpy(entry.type, "rfid");
      
      if (xQueueSend(logQueue, &entry, 0) != pdTRUE) {
        logQueueDrops++;
      }
    }
  }
}

// ==================== FUNCTION DECLARATIONS ====================
void initLittleFS();
void saveLogToFile(const String& uid, const String& name, const String& status, const String& type);
void cleanOldLogs();
String getDateString();
String getLogFilePath();
void handleWiFi();
void initWiFi();
void initFirebase();
void firebaseTask(void *parameter);
void rfidTask(void *parameter);

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\n=== ESP32 RFID Door Lock System v2.0 ===");
  Serial.println("Architectural rewrite with proper thread safety");
  
  // *** CRITICAL: Create mutex BEFORE any NVS operations ***
  nvsMutex = xSemaphoreCreateMutex();
  if (nvsMutex == NULL) {
    Serial.println("FATAL: Failed to create NVS mutex!");
    while (1) { delay(1000); }  // Halt - cannot proceed safely
  }
  Serial.println("NVS mutex created successfully");
  
  // Initialize GPIO
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(EXIT_SENSOR_PIN, INPUT);
  digitalWrite(RELAY_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);
  
  // Initialize SPI
  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, SS_PIN);
  
  // Initialize RFID
  rfid.PCD_Init();
  delay(100);
  Serial.println("RFID Reader initialized");
  rfid.PCD_DumpVersionToSerial();
  
  // Initialize LittleFS
  initLittleFS();
  
  // Initialize WiFi (non-blocking)
  initWiFi();
  
  // Initialize NTP
  timeClient.begin();
  
  // Initialize Firebase
  initFirebase();
  
  // Create queues with reasonable sizes
  logQueue = xQueueCreate(20, sizeof(LogEntry));       // Increased from 10
  pendingQueue = xQueueCreate(10, sizeof(PendingEntry)); // Increased from 5
  commandQueue = xQueueCreate(5, sizeof(Command));
  
  if (logQueue == NULL || pendingQueue == NULL || commandQueue == NULL) {
    Serial.println("FATAL: Failed to create queues!");
    while (1) { delay(1000); }
  }
  
  // Enable task watchdog (30 second timeout) - ESP-IDF v5.x API
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = 30000,
    .idle_core_mask = 0,  // Don't watch idle tasks
    .trigger_panic = true
  };
  esp_task_wdt_init(&wdt_config);
  
  // Create Firebase task on Core 0
  xTaskCreatePinnedToCore(
    firebaseTask,
    "FirebaseTask",
    12288,  // Increased stack for SSL operations
    NULL,
    1,
    &firebaseTaskHandle,
    0
  );
  
  // Create RFID task on Core 1
  xTaskCreatePinnedToCore(
    rfidTask,
    "RFIDTask",
    4096,
    NULL,
    2,
    &rfidTaskHandle,
    1
  );
  
  Serial.println("Setup complete. Multi-threaded system ready.");
  Serial.println("Core 0: Firebase/WiFi operations");
  Serial.println("Core 1: RFID/Access Control");
}

// ==================== FIREBASE TASK (Core 0) ====================
void firebaseTask(void *parameter) {
  Serial.println("Firebase task started on Core " + String(xPortGetCoreID()));
  
  // Subscribe to watchdog
  esp_task_wdt_add(NULL);
  
  static bool firebaseInitialized = false;
  static unsigned long tokenWaitStart = 0;
  
  while (true) {
    // Feed watchdog
    esp_task_wdt_reset();
    
    // Maintain WiFi connection
    handleWiFi();
    
    if (WiFi.status() == WL_CONNECTED) {
      // Initialize Firebase if not done yet (deferred init)
      if (!firebaseInitialized) {
        Serial.println("WiFi connected - initializing Firebase now...");
        
        config.api_key = API_KEY;
        config.database_url = DATABASE_URL;
        auth.user.email = USER_EMAIL;
        auth.user.password = USER_PASSWORD;
        config.token_status_callback = tokenStatusCallback;
        fbdo.setBSSLBufferSize(4096, 1024);
        
        Firebase.begin(&config, &auth);
        Firebase.reconnectWiFi(true);
        
        firebaseInitialized = true;
        tokenWaitStart = millis();
        Serial.println("Firebase initialization started - waiting for token...");
      }
      
      // Wait for token to be ready (with timeout feedback)
      if (firebaseInitialized && !CloudService::firebaseReady) {
        if (Firebase.ready()) {
          CloudService::firebaseReady = true;
          Serial.println("Firebase authenticated and ready!");
          // Small delay to ensure token is fully ready
          vTaskDelay(500 / portTICK_PERIOD_MS);
          CloudService::updateDeviceStatus();
        } else {
          // Print waiting status every 5 seconds
          if (millis() - tokenWaitStart > 5000) {
            Serial.println("Still waiting for Firebase token...");
            tokenWaitStart = millis();
          }
        }
      }
      
      if (firebaseInitialized && CloudService::firebaseReady && Firebase.ready()) {
        
        // Update status periodically
        if (millis() - CloudService::lastStatusUpdate > STATUS_UPDATE_INTERVAL) {
          CloudService::updateDeviceStatus();
          CloudService::lastStatusUpdate = millis();
        }
        
        // Check commands (pure polling)
        CloudService::checkCommands();
        
        // Process queues
        CloudService::processLogQueue();
        CloudService::processPendingQueue();
      }
    } else {
      CloudService::firebaseReady = false;
    }
    
    // Update NTP periodically
    if (millis() - lastNTPUpdate > NTP_UPDATE_INTERVAL) {
      timeClient.update();
      lastNTPUpdate = millis();
    }
    timeClient.update();
    
    // Clean old logs periodically
    if (millis() - lastLogCleanup > LOG_CLEANUP_INTERVAL) {
      cleanOldLogs();
      lastLogCleanup = millis();
    }
    
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
}

// ==================== RFID TASK (Core 1) ====================
void rfidTask(void *parameter) {
  Serial.println("RFID task started on Core " + String(xPortGetCoreID()));
  
  while (true) {
    // Process commands from Core 0
    Command cmd;
    if (xQueueReceive(commandQueue, &cmd, 0) == pdTRUE) {
      AccessController::handleCommand(cmd);
    }
    
    // Check exit sensor (works offline)
    AccessController::handleExitSensor();
    
    // Check for RFID card (works offline)
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
    
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

// ==================== MAIN LOOP ====================
void loop() {
  vTaskDelay(1000 / portTICK_PERIOD_MS);
}

// ==================== FIREBASE INITIALIZATION ====================
// Firebase is initialized in firebaseTask() to avoid duplicate init
void initFirebase() {
  Serial.println("Firebase will be initialized after WiFi connects...");
}

// ==================== WIFI FUNCTIONS ====================
void handleWiFi() {
  static unsigned long lastTry = 0;
  
  if (WiFi.status() == WL_CONNECTED) return;
  
  if (millis() - lastTry > WIFI_RETRY_INTERVAL) {
    lastTry = millis();
    WiFi.disconnect();
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    Serial.println("WiFi reconnecting...");
  }
}

void initWiFi() {
  Serial.print("Connecting to WiFi: ");
  Serial.println(ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  
  // Wait for connection with timeout (15 seconds)
  int timeout = 30;  // 30 * 500ms = 15 seconds
  while (WiFi.status() != WL_CONNECTED && timeout > 0) {
    delay(500);
    Serial.print(".");
    timeout--;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.println("WiFi connected!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    Serial.print("RSSI: ");
    Serial.println(WiFi.RSSI());
  } else {
    Serial.println();
    Serial.println("WiFi connection failed! Will retry in background.");
  }
}

// ==================== TIME FUNCTIONS ====================
unsigned long getEpochTime() {
  return timeClient.getEpochTime();
}

String getISOTimestamp() {
  unsigned long epochTime = getEpochTime();
  
  // Use IST (UTC+5:30) - all timestamps in IST
  time_t rawTime = (time_t)epochTime;
  struct tm* timeInfo = gmtime(&rawTime);  // Already IST due to NTP offset
  
  char buffer[30];
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S+05:30", timeInfo);
  
  return String(buffer);
}

// ==================== LittleFS FUNCTIONS ====================
void initLittleFS() {
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS Mount Failed!");
    return;
  }
  
  Serial.println("LittleFS mounted successfully");
  
  if (!LittleFS.exists(LOG_DIR)) {
    LittleFS.mkdir(LOG_DIR);
    Serial.println("Created /logs directory");
  }
  
  Serial.printf("Storage: %u / %u bytes used\n", LittleFS.usedBytes(), LittleFS.totalBytes());
}

String getDateString() {
  unsigned long epochTime = getEpochTime();
  time_t rawTime = (time_t)epochTime;
  struct tm* timeInfo = gmtime(&rawTime);
  
  char buffer[11];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d", timeInfo);
  return String(buffer);
}

String getLogFilePath() {
  return String(LOG_DIR) + "/log_" + getDateString() + ".jsonl";
}

void saveLogToFile(const String& uid, const String& name, const String& status, const String& type) {
  String filePath = getLogFilePath();
  
  File file = LittleFS.open(filePath, FILE_APPEND);
  if (!file) {
    Serial.println("Failed to open log file");
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
}

void cleanOldLogs() {
  Serial.println("Cleaning old logs...");
  
  File root = LittleFS.open(LOG_DIR);
  if (!root || !root.isDirectory()) return;
  
  // Use pure epoch math (no mktime timezone issues)
  unsigned long currentEpoch = getEpochTime();
  unsigned long cutoffEpoch = currentEpoch - (LOG_RETENTION_DAYS * 86400UL);
  
  File file = root.openNextFile();
  while (file) {
    String fileName = String(file.name());
    
    if (fileName.startsWith("log_") && fileName.endsWith(".jsonl")) {
      // Extract date: log_YYYY-MM-DD.jsonl
      String dateStr = fileName.substring(4, 14);
      
      int year = dateStr.substring(0, 4).toInt();
      int month = dateStr.substring(5, 7).toInt();
      int day = dateStr.substring(8, 10).toInt();
      
      // Calculate epoch using pure math (avoids mktime timezone issues)
      // Days since epoch calculation (simplified, good enough for comparison)
      unsigned long fileDays = (year - 1970) * 365UL + (year - 1969) / 4;  // Approximate leap years
      
      // Add months
      const int daysInMonth[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
      fileDays += daysInMonth[month - 1] + day;
      
      // Leap year adjustment for current year
      if (month > 2 && (year % 4 == 0) && (year % 100 != 0 || year % 400 == 0)) {
        fileDays++;
      }
      
      unsigned long fileEpoch = fileDays * 86400UL;
      
      if (fileEpoch < cutoffEpoch) {
        String fullPath = String(LOG_DIR) + "/" + fileName;
        LittleFS.remove(fullPath);
        Serial.println("Deleted old log: " + fullPath);
      }
    }
    
    file = root.openNextFile();
  }
  
  Serial.printf("Storage after cleanup: %u / %u bytes\n", LittleFS.usedBytes(), LittleFS.totalBytes());
}

// ==================== TOKEN CALLBACK ====================
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
