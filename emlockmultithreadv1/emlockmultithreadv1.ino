/*
 * ESP32 RFID Door Lock System with Firebase Cloud
 * Hardware: MFRC522 RFID, Relay (EM Lock), Buzzer, Exit Sensor
 * Features: Firebase Realtime DB, NVS Storage (Authoritative), NTP Timestamps, 
 *           LittleFS Local Logs (30 days), Cloud Commands, User Management
 * 
 * ARCHITECTURE PRINCIPLE:
 * NVS (ESP32 local storage) is AUTHORITATIVE for access decisions
 * Firebase is a command inbox + audit mirror
 * 
 * Cloud is NOT allowed to:
 * - Overwrite lists
 * - Resync full JSON trees
 * - "Rebuild" local state
 * 
 * Cloud IS allowed to:
 * - Send commands: add, delete, move, rename
 * - Request logs
 * - Send unlock commands
 * 
 * If cloud dies → door still works
 * If ESP32 reboots → door still works (NVS persists)
 * 
 * Pin Configuration:
 * MFRC522: SDA=21, RST=22, SCK=18, MOSI=23, MISO=19
 * Relay: GPIO 25 (Active HIGH)
 * Buzzer: GPIO 27
 * Exit Sensor: GPIO 26 (NC switch, LOW=idle, HIGH=hand detected)
 * 
 * Firebase Structure:
 * /devices/device1/whitelist/{uid}: {name, addedAt}
 * /devices/device1/blacklist/{uid}: {name, addedAt}
 * /devices/device1/pending/{uid}: {name, firstSeen}
 * /devices/device1/logs/{timestamp}: {uid, name, status, type, time}
 * /devices/device1/commands/unlock: {duration, timestamp}
 * /devices/device1/commands/add: {list, uid, name}
 * /devices/device1/commands/delete: {list, uid}
 * /devices/device1/commands/move: {from, to, uid, name}
 * /devices/device1/commands/rename: {list, uid, name}
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
#define EXIT_SENSOR_PIN 26  // NC switch: LOW=idle, HIGH=hand detected

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
SemaphoreHandle_t nvsMutex = NULL;  // Mutex for thread-safe NVS access

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

// ==================== FORWARD DECLARATIONS ====================
// These must come before namespaces that use them
String getISOTimestamp();

// ==================== STORAGE MANAGER MODULE ====================
// Thread-safe NVS access with mutex protection
// MUST be declared before CloudService and AccessController
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
  
  // Read operations - mutex protected
  bool isWhitelisted(String uid) {
    if (xSemaphoreTake(nvsMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      bool result = whitelistPrefs.isKey(uid.c_str());
      xSemaphoreGive(nvsMutex);
      return result;
    }
    return false;  // Fail-safe: deny if mutex unavailable
  }
  
  bool isBlacklisted(String uid) {
    if (xSemaphoreTake(nvsMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      bool result = blacklistPrefs.isKey(uid.c_str());
      xSemaphoreGive(nvsMutex);
      return result;
    }
    return false;
  }
  
  bool isPending(String uid) {
    if (xSemaphoreTake(nvsMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      bool result = pendingPrefs.isKey(uid.c_str());
      xSemaphoreGive(nvsMutex);
      return result;
    }
    return false;
  }
  
  String getName(String uid, const char* listType) {
    String result = "Unknown";
    if (xSemaphoreTake(nvsMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      if (strcmp(listType, "whitelist") == 0) {
        result = whitelistPrefs.getString(uid.c_str(), "Unknown");
      } else if (strcmp(listType, "blacklist") == 0) {
        result = blacklistPrefs.getString(uid.c_str(), "Unknown");
      } else if (strcmp(listType, "pending") == 0) {
        result = pendingPrefs.getString(uid.c_str(), "Unknown");
      }
      xSemaphoreGive(nvsMutex);
    }
    return result;
  }
  
  // Write operations - mutex protected
  void addToWhitelist(String uid, String name) {
    if (xSemaphoreTake(nvsMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
      whitelistPrefs.putString(uid.c_str(), name.c_str());
      xSemaphoreGive(nvsMutex);
    }
  }
  
  void addToBlacklist(String uid, String name) {
    if (xSemaphoreTake(nvsMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
      blacklistPrefs.putString(uid.c_str(), name.c_str());
      xSemaphoreGive(nvsMutex);
    }
  }
  
  void addToPending(String uid, String name) {
    if (xSemaphoreTake(nvsMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
      pendingPrefs.putString(uid.c_str(), name.c_str());
      xSemaphoreGive(nvsMutex);
    }
  }
  
  void removeFromWhitelist(String uid) {
    if (xSemaphoreTake(nvsMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
      whitelistPrefs.remove(uid.c_str());
      xSemaphoreGive(nvsMutex);
    }
  }
  
  void removeFromBlacklist(String uid) {
    if (xSemaphoreTake(nvsMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
      blacklistPrefs.remove(uid.c_str());
      xSemaphoreGive(nvsMutex);
    }
  }
  
  void removeFromPending(String uid) {
    if (xSemaphoreTake(nvsMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
      pendingPrefs.remove(uid.c_str());
      xSemaphoreGive(nvsMutex);
    }
  }
  
  void clearAll() {
    if (xSemaphoreTake(nvsMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
      whitelistPrefs.clear();
      blacklistPrefs.clear();
      pendingPrefs.clear();
      xSemaphoreGive(nvsMutex);
    }
  }
}

// ==================== CLOUD SERVICE MODULE (CORE 0) ====================
namespace CloudService {
  unsigned long lastStatusUpdate = 0;
  unsigned long lastFirebaseSync = 0;
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
        Serial.println("Log uploaded to Firebase");
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
        Serial.println("Added to pending in Firebase: " + String(pendingEntry.uid));
      }
    }
  }
  
  // Input validation constants
  const int MAX_UID_LENGTH = 16;   // RFID UIDs are typically 4-10 hex chars
  const int MAX_NAME_LENGTH = 45;  // Leave margin for LogEntry.name[50]
  
  // Validate and sanitize UID
  bool isValidUID(const String& uid) {
    if (uid.length() == 0 || uid.length() > MAX_UID_LENGTH) return false;
    // Only allow hex characters
    for (unsigned int i = 0; i < uid.length(); i++) {
      char c = uid.charAt(i);
      if (!isxdigit(c)) return false;
    }
    return true;
  }
  
  // Validate and sanitize name
  String sanitizeName(const String& name) {
    String sanitized = name;
    if (sanitized.length() > MAX_NAME_LENGTH) {
      sanitized = sanitized.substring(0, MAX_NAME_LENGTH);
    }
    return sanitized;
  }
  
  // Validate list name
  bool isValidList(const String& list) {
    return (list == "whitelist" || list == "blacklist" || list == "pending");
  }
  
  void checkCommands() {
    if (WiFi.status() != WL_CONNECTED || !Firebase.ready()) return;
    
    static unsigned long lastCheck = 0;
    if (millis() - lastCheck < 2000) return;
    lastCheck = millis();
    
    // Check unlock command
    // NOTE: Stream callback is disabled for unlock to prevent duplicate execution
    if (Firebase.RTDB.getJSON(&fbdo, commandsPath + "/unlock")) {
      FirebaseJson json = fbdo.jsonObject();
      FirebaseJsonData result;
      
      if (json.get(result, "duration")) {
        int duration = result.intValue;
        
        // DELETE FIRST to prevent duplicate execution on failure/reboot
        if (!Firebase.RTDB.deleteNode(&fbdo, commandsPath + "/unlock")) {
          Serial.println("Failed to delete unlock command - skipping to prevent duplicate");
          return;  // Don't execute if we can't delete (prevents duplicates)
        }
        
        if (duration > 0 && duration <= 300) {
          Command cmd;
          cmd.type = Command::UNLOCK;
          cmd.duration = duration * 1000;
          
          if (commandQueue != NULL) {
            xQueueSend(commandQueue, &cmd, 0);
            Serial.println("Unlock command queued for Core 1");
          }
        }
      }
    }
    
    // Check add command (cloud requests to add a user)
    if (Firebase.RTDB.getJSON(&fbdo, commandsPath + "/add")) {
      FirebaseJson json = fbdo.jsonObject();
      FirebaseJsonData listData, uidData, nameData;
      
      if (json.get(listData, "list") && json.get(uidData, "uid")) {
        String list = listData.stringValue;
        String uid = uidData.stringValue;
        String name = "Unknown";
        if (json.get(nameData, "name")) name = sanitizeName(nameData.stringValue);
        
        // DELETE FIRST to prevent duplicate execution
        if (!Firebase.RTDB.deleteNode(&fbdo, commandsPath + "/add")) {
          Serial.println("Failed to delete add command - skipping");
          return;
        }
        
        // Validate inputs
        if (!isValidUID(uid) || !isValidList(list)) {
          Serial.println("Invalid UID or list in add command");
          return;
        }
        
        // Apply to NVS (authoritative)
        if (list == "whitelist") {
          StorageManager::addToWhitelist(uid, name);
          // Mirror to Firebase
          FirebaseJson userJson;
          userJson.set("name", name);
          userJson.set("addedAt", getISOTimestamp());
          Firebase.RTDB.setJSON(&fbdo, whitelistPath + "/" + uid, &userJson);
        } else if (list == "blacklist") {
          StorageManager::addToBlacklist(uid, name);
          FirebaseJson userJson;
          userJson.set("name", name);
          userJson.set("addedAt", getISOTimestamp());
          Firebase.RTDB.setJSON(&fbdo, blacklistPath + "/" + uid, &userJson);
        } else if (list == "pending") {
          StorageManager::addToPending(uid, name);
          FirebaseJson userJson;
          userJson.set("name", name);
          userJson.set("firstSeen", getISOTimestamp());
          Firebase.RTDB.setJSON(&fbdo, pendingPath + "/" + uid, &userJson);
        }
        Serial.println("Add command processed: " + uid + " -> " + list);
      }
    }
    
    // Check delete command
    if (Firebase.RTDB.getJSON(&fbdo, commandsPath + "/delete")) {
      FirebaseJson json = fbdo.jsonObject();
      FirebaseJsonData listData, uidData;
      
      if (json.get(listData, "list") && json.get(uidData, "uid")) {
        String list = listData.stringValue;
        String uid = uidData.stringValue;
        
        // DELETE FIRST
        if (!Firebase.RTDB.deleteNode(&fbdo, commandsPath + "/delete")) {
          Serial.println("Failed to delete delete command - skipping");
          return;
        }
        
        // Validate inputs
        if (!isValidUID(uid) || !isValidList(list)) {
          Serial.println("Invalid UID or list in delete command");
          return;
        }
        
        // Apply to NVS (authoritative)
        if (list == "whitelist") {
          StorageManager::removeFromWhitelist(uid);
          Firebase.RTDB.deleteNode(&fbdo, whitelistPath + "/" + uid);
        } else if (list == "blacklist") {
          StorageManager::removeFromBlacklist(uid);
          Firebase.RTDB.deleteNode(&fbdo, blacklistPath + "/" + uid);
        } else if (list == "pending") {
          StorageManager::removeFromPending(uid);
          Firebase.RTDB.deleteNode(&fbdo, pendingPath + "/" + uid);
        }
        Serial.println("Delete command processed: " + uid + " from " + list);
      }
    }
    
    // Check move command
    if (Firebase.RTDB.getJSON(&fbdo, commandsPath + "/move")) {
      FirebaseJson json = fbdo.jsonObject();
      FirebaseJsonData fromData, toData, uidData, nameData;
      
      if (json.get(fromData, "from") && json.get(toData, "to") && json.get(uidData, "uid")) {
        String fromList = fromData.stringValue;
        String toList = toData.stringValue;
        String uid = uidData.stringValue;
        String name = "Unknown";
        
        // Get name from command or will get from source list after delete
        if (json.get(nameData, "name")) {
          name = sanitizeName(nameData.stringValue);
        } else {
          name = StorageManager::getName(uid, fromList.c_str());
        }
        
        // DELETE FIRST
        if (!Firebase.RTDB.deleteNode(&fbdo, commandsPath + "/move")) {
          Serial.println("Failed to delete move command - skipping");
          return;
        }
        
        // Validate inputs
        if (!isValidUID(uid) || !isValidList(fromList) || !isValidList(toList)) {
          Serial.println("Invalid UID or list in move command");
          return;
        }
        
        // Remove from source (NVS + Firebase)
        if (fromList == "whitelist") {
          StorageManager::removeFromWhitelist(uid);
          Firebase.RTDB.deleteNode(&fbdo, whitelistPath + "/" + uid);
        } else if (fromList == "blacklist") {
          StorageManager::removeFromBlacklist(uid);
          Firebase.RTDB.deleteNode(&fbdo, blacklistPath + "/" + uid);
        } else if (fromList == "pending") {
          StorageManager::removeFromPending(uid);
          Firebase.RTDB.deleteNode(&fbdo, pendingPath + "/" + uid);
        }
        
        // Add to destination (NVS + Firebase)
        if (toList == "whitelist") {
          StorageManager::addToWhitelist(uid, name);
          FirebaseJson userJson;
          userJson.set("name", name);
          userJson.set("addedAt", getISOTimestamp());
          Firebase.RTDB.setJSON(&fbdo, whitelistPath + "/" + uid, &userJson);
        } else if (toList == "blacklist") {
          StorageManager::addToBlacklist(uid, name);
          FirebaseJson userJson;
          userJson.set("name", name);
          userJson.set("addedAt", getISOTimestamp());
          Firebase.RTDB.setJSON(&fbdo, blacklistPath + "/" + uid, &userJson);
        } else if (toList == "pending") {
          StorageManager::addToPending(uid, name);
          FirebaseJson userJson;
          userJson.set("name", name);
          userJson.set("firstSeen", getISOTimestamp());
          Firebase.RTDB.setJSON(&fbdo, pendingPath + "/" + uid, &userJson);
        }
        
        Serial.println("Move command processed: " + uid + " from " + fromList + " to " + toList);
      }
    }
    
    // Check rename command
    if (Firebase.RTDB.getJSON(&fbdo, commandsPath + "/rename")) {
      FirebaseJson json = fbdo.jsonObject();
      FirebaseJsonData listData, uidData, nameData;
      
      if (json.get(listData, "list") && json.get(uidData, "uid") && json.get(nameData, "name")) {
        String list = listData.stringValue;
        String uid = uidData.stringValue;
        String newName = sanitizeName(nameData.stringValue);
        
        // DELETE FIRST
        if (!Firebase.RTDB.deleteNode(&fbdo, commandsPath + "/rename")) {
          Serial.println("Failed to delete rename command - skipping");
          return;
        }
        
        // Validate inputs
        if (!isValidUID(uid) || !isValidList(list)) {
          Serial.println("Invalid UID or list in rename command");
          return;
        }
        
        // Update NVS (authoritative) and mirror to Firebase
        if (list == "whitelist") {
          StorageManager::addToWhitelist(uid, newName);  // Overwrites existing
          Firebase.RTDB.setString(&fbdo, whitelistPath + "/" + uid + "/name", newName);
        } else if (list == "blacklist") {
          StorageManager::addToBlacklist(uid, newName);
          Firebase.RTDB.setString(&fbdo, blacklistPath + "/" + uid + "/name", newName);
        } else if (list == "pending") {
          StorageManager::addToPending(uid, newName);
          Firebase.RTDB.setString(&fbdo, pendingPath + "/" + uid + "/name", newName);
        }
        
        Serial.println("Rename command processed: " + uid + " -> " + newName);
      }
    }
  }
  
  // Upload local NVS state to Firebase (one-way mirror, NVS is authoritative)
  void mirrorNVSToFirebase() {
    if (WiFi.status() != WL_CONNECTED || !Firebase.ready()) return;
    
    Serial.println("Mirroring NVS to Firebase (NVS is authoritative)...");
    // This is called on boot to ensure Firebase reflects NVS state
    // Firebase is just a mirror/audit log, NOT authoritative
    
    // Note: Full mirror would require iterating NVS which is complex
    // Instead, we update Firebase incrementally as changes happen
    Serial.println("NVS is authoritative. Firebase updates happen incrementally.");
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
  
  // Exit sensor state for edge detection
  bool lastExitSensorState = false;  // Previous sensor reading
  bool exitSensorTriggered = false;  // Latched trigger state
  const unsigned long EXIT_UNLOCK_DURATION = 3000;  // Same as RFID access
  
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
  
  // Exit sensor handling with edge-triggered, latched behavior
  // Sensor is NC (Normally Closed): LOW=idle, HIGH=hand detected
  void handleExitSensor() {
    bool currentState = digitalRead(EXIT_SENSOR_PIN) == HIGH;
    
    // Edge detection: trigger only on LOW -> HIGH transition
    if (currentState && !lastExitSensorState && !exitSensorTriggered) {
      // Rising edge detected - hand just appeared
      exitSensorTriggered = true;  // Latch the trigger
      
      // Unlock door (same duration as RFID)
      activateRelay(EXIT_UNLOCK_DURATION);
      activateBuzzer(100);  // Short confirmation beep
      
      Serial.println("EXIT SENSOR: Door unlocked (edge-triggered)");
      
      // Queue log for Core 0 (Firebase)
      if (logQueue != NULL) {
        LogEntry entry;
        strcpy(entry.uid, "EXIT_SENSOR");
        strcpy(entry.name, "Exit Button");
        strcpy(entry.status, "granted");
        strcpy(entry.type, "exit");
        xQueueSend(logQueue, &entry, 0);
      }
    }
    
    // Reset latch when sensor returns to idle (hand removed)
    // AND the relay timer has expired (door is locked again)
    if (!currentState && !relayActive) {
      exitSensorTriggered = false;
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

// ==================== FUNCTION DECLARATIONS ====================
// LittleFS functions
void initLittleFS();
void saveLogToFile(String uid, String name, String status, String type);
void cleanOldLogs();
String getDateString();
String getLogFilePath();

// WiFi and NTP functions
void handleWiFi();
void initWiFi();
String getISOTimestamp();

// Logging
void publishLog(String uid, String name, String status, String type);

// Firebase functions
void initFirebase();
void streamCallback(FirebaseStream data);
void streamTimeoutCallback(bool timeout);

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
  pinMode(EXIT_SENSOR_PIN, INPUT);  // NC sensor: LOW=idle, HIGH=hand detected
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
  
  // Create mutex for thread-safe NVS access between cores
  nvsMutex = xSemaphoreCreateMutex();
  if (nvsMutex == NULL) {
    Serial.println("ERROR: Failed to create NVS mutex!");
  }
  
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
          // NVS is authoritative - we mirror TO Firebase, not FROM Firebase
          CloudService::mirrorNVSToFirebase();
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
    
    // Check exit sensor (edge-triggered, latched)
    // This works independently of WiFi/Firebase - fail-safe exit
    AccessController::handleExitSensor();
    
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
  
  // NOTE: Unlock command is handled ONLY by polling in checkCommands()
  // to prevent duplicate execution. Stream is used for immediate notification only.
  if (data.dataPath().indexOf("unlock") >= 0) {
    Serial.println("Unlock command detected via stream - will be processed by polling");
    // Do NOT process here - let checkCommands() handle it with delete-first logic
  }
  
  // Handle sync command - NVS is authoritative, so we mirror TO Firebase
  if (data.dataPath().indexOf("sync") >= 0) {
    Serial.println("Sync command received - NVS is authoritative");
    // Do NOT overwrite NVS from Firebase
    // Instead, mirror NVS to Firebase if needed
    CloudService::mirrorNVSToFirebase();
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
// NOTE: Legacy functions removed. All user management is now handled via
// Firebase commands in CloudService::checkCommands()
// Commands: add, delete, move, rename

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