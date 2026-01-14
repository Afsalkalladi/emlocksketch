/*
 * ESP32 RFID Door Lock System with Firebase Cloud
 * Hardware: MFRC522 RFID, Relay (EM Lock), Buzzer
 * Features: Firebase Realtime DB, NVS Storage, NTP Timestamps, 
 *           LittleFS Local Logs (30 days), Cloud Sync, User Management
 * 
 * Pin Configuration:
 * MFRC522: SDA=21, RST=22, SCK=18, MOSI=23, MISO=19
 * Relay: GPIO 25 (Active HIGH)
 * Buzzer: GPIO 15
 * 
 * Firebase Structure:
 * /devices/device1/whitelist/{uid}: {name, addedAt}
 * /devices/device1/blacklist/{uid}: {name, addedAt}
 * /devices/device1/pending/{uid}: {name, firstSeen}
 * /devices/device1/logs/{timestamp}: {uid, name, status, type, time}
 * /devices/device1/commands/unlock: {duration, timestamp}
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
#define BUZZER_PIN 15

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
unsigned long relayStartTime = 0;
unsigned long relayDuration = 0;
bool relayActive = false;

unsigned long buzzerStartTime = 0;
unsigned long buzzerDuration = 0;
bool buzzerActive = false;

unsigned long lastReconnectAttempt = 0;
unsigned long lastNTPUpdate = 0;
unsigned long lastLogCleanup = 0;
unsigned long lastStatusUpdate = 0;
unsigned long lastFirebaseSync = 0;

bool firebaseReady = false;
bool streamConnected = false;

// ==================== FREERTOS MULTI-THREADING ====================
TaskHandle_t firebaseTaskHandle = NULL;
TaskHandle_t rfidTaskHandle = NULL;
QueueHandle_t logQueue = NULL;
QueueHandle_t pendingQueue = NULL;  // Queue for pending cards

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

// ==================== LOG CONFIGURATION ====================
const int LOG_RETENTION_DAYS = 30;
const char* LOG_DIR = "/logs";
const int MAX_LOG_ENTRIES_PER_FILE = 100;  // Split logs by day

// ==================== NVS KEYS ====================
const char* NVS_WHITELIST = "whitelist";
const char* NVS_BLACKLIST = "blacklist";
const char* NVS_PENDING = "pending";

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
  
  // Initialize NVS
  prefs.begin("rfid-lock", false);
  initializeNVSIfNeeded();
  
  // Initialize LittleFS for log storage
  initLittleFS();
  
  // Connect to WiFi
  connectWiFi();
  
  // Initialize NTP
  timeClient.begin();
  updateNTPTime();
  
  // Initialize Firebase
  initFirebase();
  
  // Create queues for thread-safe communication
  logQueue = xQueueCreate(10, sizeof(LogEntry));
  pendingQueue = xQueueCreate(5, sizeof(PendingEntry));
  
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
    // Maintain WiFi connection
    if (WiFi.status() != WL_CONNECTED) {
      connectWiFi();
    }
    
    // Firebase operations
    if (Firebase.ready()) {
      if (!firebaseReady) {
        firebaseReady = true;
        Serial.println("Firebase connected!");
        uploadLocalListsToFirebase();
        syncListsFromFirebase();
        updateDeviceStatus();
      }
      
      // Update device status every 60 seconds
      if (millis() - lastStatusUpdate > 60000) {
        updateDeviceStatus();
        lastStatusUpdate = millis();
      }
      
      // Check for commands
      checkFirebaseCommands();
      
      // Process log queue - send logs to Firebase
      LogEntry logEntry;
      while (xQueueReceive(logQueue, &logEntry, 0) == pdTRUE) {
        publishLogToFirebase(String(logEntry.uid), String(logEntry.name), 
                            String(logEntry.status), String(logEntry.type));
      }
      
      // Process pending queue - add pending cards to Firebase
      PendingEntry pendingEntry;
      while (xQueueReceive(pendingQueue, &pendingEntry, 0) == pdTRUE) {
        addToPendingFirebaseInternal(String(pendingEntry.uid));
      }
    }
    
    // Update NTP time periodically
    if (millis() - lastNTPUpdate > 3600000) {
      updateNTPTime();
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
    // Check for RFID card - this runs independently of Firebase
    if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
      handleRFIDScan();
      rfid.PICC_HaltA();
      rfid.PCD_StopCrypto1();
    }
    
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

// ==================== SYNC LISTS FROM FIREBASE ====================
void syncListsFromFirebase() {
  Serial.println("Syncing lists from Firebase...");
  
  // Sync whitelist - REPLACE local with Firebase data
  if (Firebase.RTDB.getJSON(&fbdo, whitelistPath)) {
    FirebaseJson json = fbdo.jsonObject();
    StaticJsonDocument<2048> localDoc;
    
    size_t count = json.iteratorBegin();
    for (size_t i = 0; i < count; i++) {
      String key, value;
      int type;
      json.iteratorGet(i, type, key, value);
      
      if (type == FirebaseJson::JSON_OBJECT) {
        FirebaseJson childJson;
        childJson.setJsonData(value);
        FirebaseJsonData nameData;
        if (childJson.get(nameData, "name")) {
          localDoc[key] = nameData.stringValue;
        }
      }
    }
    json.iteratorEnd();
    
    String localJson;
    serializeJson(localDoc, localJson);
    prefs.putString(NVS_WHITELIST, localJson);
    Serial.println("Whitelist synced: " + localJson);
  } else {
    // If Firebase path doesn't exist or is empty, clear local
    if (fbdo.errorReason() == "path not exist" || fbdo.httpCode() == 200) {
      prefs.putString(NVS_WHITELIST, "{}");
      Serial.println("Whitelist cleared (empty in Firebase)");
    }
  }
  
  // Sync blacklist - REPLACE local with Firebase data
  if (Firebase.RTDB.getJSON(&fbdo, blacklistPath)) {
    FirebaseJson json = fbdo.jsonObject();
    StaticJsonDocument<2048> localDoc;
    
    size_t count = json.iteratorBegin();
    for (size_t i = 0; i < count; i++) {
      String key, value;
      int type;
      json.iteratorGet(i, type, key, value);
      
      if (type == FirebaseJson::JSON_OBJECT) {
        FirebaseJson childJson;
        childJson.setJsonData(value);
        FirebaseJsonData nameData;
        if (childJson.get(nameData, "name")) {
          localDoc[key] = nameData.stringValue;
        }
      }
    }
    json.iteratorEnd();
    
    String localJson;
    serializeJson(localDoc, localJson);
    prefs.putString(NVS_BLACKLIST, localJson);
    Serial.println("Blacklist synced: " + localJson);
  } else {
    // If Firebase path doesn't exist or is empty, clear local
    if (fbdo.errorReason() == "path not exist" || fbdo.httpCode() == 200) {
      prefs.putString(NVS_BLACKLIST, "{}");
      Serial.println("Blacklist cleared (empty in Firebase)");
    }
  }
  
  // Sync pending - REPLACE local with Firebase data
  if (Firebase.RTDB.getJSON(&fbdo, pendingPath)) {
    FirebaseJson json = fbdo.jsonObject();
    StaticJsonDocument<2048> localDoc;
    
    size_t count = json.iteratorBegin();
    for (size_t i = 0; i < count; i++) {
      String key, value;
      int type;
      json.iteratorGet(i, type, key, value);
      
      if (type == FirebaseJson::JSON_OBJECT) {
        FirebaseJson childJson;
        childJson.setJsonData(value);
        FirebaseJsonData nameData;
        if (childJson.get(nameData, "name")) {
          localDoc[key] = nameData.stringValue;
        }
      }
    }
    json.iteratorEnd();
    
    String localJson;
    serializeJson(localDoc, localJson);
    prefs.putString(NVS_PENDING, localJson);
    Serial.println("Pending synced: " + localJson);
  } else {
    // If Firebase path doesn't exist or is empty, clear local
    if (fbdo.errorReason() == "path not exist" || fbdo.httpCode() == 200) {
      prefs.putString(NVS_PENDING, "{}");
      Serial.println("Pending cleared (empty in Firebase)");
    }
  }
  
  lastFirebaseSync = millis();
  Serial.println("Sync complete!");
}

// ==================== UPLOAD LOCAL LISTS TO FIREBASE ====================
// Call this once to push existing local data to Firebase
void uploadLocalListsToFirebase() {
  Serial.println("Uploading local lists to Firebase...");
  
  // Upload whitelist
  String whitelistJson = prefs.getString(NVS_WHITELIST, "{}");
  StaticJsonDocument<2048> whiteDoc;
  deserializeJson(whiteDoc, whitelistJson);
  
  for (JsonPair kv : whiteDoc.as<JsonObject>()) {
    FirebaseJson json;
    json.set("name", kv.value().as<String>());
    json.set("addedAt", getISOTimestamp());
    String path = whitelistPath + "/" + String(kv.key().c_str());
    if (Firebase.RTDB.setJSON(&fbdo, path, &json)) {
      Serial.println("Uploaded whitelist: " + String(kv.key().c_str()));
    } else {
      Serial.println("Failed to upload whitelist: " + fbdo.errorReason());
    }
  }
  
  // Upload blacklist
  String blacklistJson = prefs.getString(NVS_BLACKLIST, "{}");
  StaticJsonDocument<2048> blackDoc;
  deserializeJson(blackDoc, blacklistJson);
  
  for (JsonPair kv : blackDoc.as<JsonObject>()) {
    FirebaseJson json;
    json.set("name", kv.value().as<String>());
    json.set("addedAt", getISOTimestamp());
    String path = blacklistPath + "/" + String(kv.key().c_str());
    if (Firebase.RTDB.setJSON(&fbdo, path, &json)) {
      Serial.println("Uploaded blacklist: " + String(kv.key().c_str()));
    } else {
      Serial.println("Failed to upload blacklist: " + fbdo.errorReason());
    }
  }
  
  // Upload pending
  String pendingJson = prefs.getString(NVS_PENDING, "{}");
  StaticJsonDocument<2048> pendDoc;
  deserializeJson(pendDoc, pendingJson);
  
  for (JsonPair kv : pendDoc.as<JsonObject>()) {
    FirebaseJson json;
    json.set("name", kv.value().as<String>());
    json.set("firstSeen", getISOTimestamp());
    String path = pendingPath + "/" + String(kv.key().c_str());
    if (Firebase.RTDB.setJSON(&fbdo, path, &json)) {
      Serial.println("Uploaded pending: " + String(kv.key().c_str()));
    } else {
      Serial.println("Failed to upload pending: " + fbdo.errorReason());
    }
  }
  
  Serial.println("Local lists upload complete!");
}

// ==================== CHECK FIREBASE COMMANDS ====================
void checkFirebaseCommands() {
  static unsigned long lastCommandCheck = 0;
  
  if (millis() - lastCommandCheck < 2000) return; // Check every 2 seconds
  lastCommandCheck = millis();
  
  // Check for unlock command
  if (Firebase.RTDB.getJSON(&fbdo, commandsPath + "/unlock")) {
    FirebaseJson json = fbdo.jsonObject();
    FirebaseJsonData result;
    
    if (json.get(result, "duration")) {
      int duration = result.intValue;
      if (duration > 0 && duration <= 300) {
        activateRelay(duration * 1000);
        Serial.println("Remote unlock command: " + String(duration) + "s");
        
        // Log the remote unlock
        publishLogToFirebase("REMOTE", "Admin", "granted", "remote_unlock");
        
        // Clear the command
        Firebase.RTDB.deleteNode(&fbdo, commandsPath + "/unlock");
      }
    }
  }
}

// ==================== UPDATE DEVICE STATUS ====================
void updateDeviceStatus() {
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

// ==================== PUBLISH LOG TO FIREBASE ====================
void publishLogToFirebase(String uid, String name, String status, String type) {
  String timestamp = getISOTimestamp();
  String logKey = String(timeClient.getEpochTime());
  
  FirebaseJson json;
  json.set("uid", uid);
  json.set("name", name);
  json.set("status", status);
  json.set("type", type);
  json.set("time", timestamp);
  json.set("device", device_id);
  
  String logPath = logsPath + "/" + logKey;
  
  if (Firebase.RTDB.setJSON(&fbdo, logPath, &json)) {
    Serial.println("Log published to Firebase: " + logPath);
  } else {
    Serial.println("Firebase log error: " + fbdo.errorReason());
  }
}

// ==================== ADD TO PENDING IN FIREBASE (Thread-Safe) ====================
// This queues the request - actual Firebase call happens in Firebase task
void addToPendingFirebase(String uid) {
  if (pendingQueue != NULL) {
    PendingEntry entry;
    uid.toCharArray(entry.uid, sizeof(entry.uid));
    
    if (xQueueSend(pendingQueue, &entry, 0) == pdTRUE) {
      Serial.println("Pending card queued for Firebase: " + uid);
    } else {
      Serial.println("Pending queue full");
    }
  }
}

// Internal function called by Firebase task
void addToPendingFirebaseInternal(String uid) {
  FirebaseJson json;
  json.set("name", "Unknown");
  json.set("firstSeen", getISOTimestamp());
  
  String path = pendingPath + "/" + uid;
  
  if (Firebase.RTDB.setJSON(&fbdo, path, &json)) {
    Serial.println("Added to pending in Firebase: " + uid);
  } else {
    Serial.println("Failed to add to pending: " + fbdo.errorReason());
  }
}
// ==================== WIFI CONNECTION ====================
void connectWiFi() {
  Serial.print("Connecting to WiFi: ");
  Serial.println(ssid);
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nWiFi connection failed. Retrying...");
    delay(3000);
    connectWiFi();
  }
}

// ==================== RFID SCAN HANDLER ====================
void handleRFIDScan() {
  // Read UID
  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    uid += String(rfid.uid.uidByte[i] < 0x10 ? "0" : "");
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  
  Serial.println("Card detected: " + uid);
  
  // Load lists from NVS
  String whitelistJson = prefs.getString(NVS_WHITELIST, "{}");
  String blacklistJson = prefs.getString(NVS_BLACKLIST, "{}");
  String pendingJson = prefs.getString(NVS_PENDING, "{}");
  
  // Parse JSON
  StaticJsonDocument<1024> whitelistDoc;
  StaticJsonDocument<1024> blacklistDoc;
  StaticJsonDocument<1024> pendingDoc;
  
  deserializeJson(whitelistDoc, whitelistJson);
  deserializeJson(blacklistDoc, blacklistJson);
  deserializeJson(pendingDoc, pendingJson);
  
  // Check whitelist
  if (whitelistDoc.containsKey(uid)) {
    String name = whitelistDoc[uid].as<String>();
    Serial.println("ACCESS GRANTED: " + name);
    
    activateRelay(3000); // 3 seconds
    activateBuzzer(100);  // Short beep
    publishLog(uid, name, "granted", "rfid");
    return;
  }
  
  // Check blacklist
  if (blacklistDoc.containsKey(uid)) {
    String name = blacklistDoc[uid].as<String>();
    Serial.println("ACCESS DENIED: " + name);
    
    activateBuzzer(2000); // Long beep
    publishLog(uid, name, "denied", "rfid");
    return;
  }
  
  // Unknown card - add to pending
  Serial.println("UNKNOWN CARD - Adding to pending");
  
  // Always add to local pending if not exists
  if (!pendingDoc.containsKey(uid)) {
    pendingDoc[uid] = "Unknown";
    
    String updatedPending = "";
    serializeJson(pendingDoc, updatedPending);
    prefs.putString(NVS_PENDING, updatedPending);
  }
  
  // Always try to add to Firebase (in case it was deleted from dashboard)
  addToPendingFirebase(uid);
  
  activateBuzzer(500); // Medium beep
  publishLog(uid, "Unknown", "pending", "rfid");
}

// ==================== RELAY CONTROL ====================
void activateRelay(unsigned long duration) {
  digitalWrite(RELAY_PIN, HIGH);
  relayActive = true;
  relayStartTime = millis();
  relayDuration = duration;
  Serial.println("Relay ON for " + String(duration) + "ms");
}

// ==================== BUZZER CONTROL ====================
void activateBuzzer(unsigned long duration) {
  digitalWrite(BUZZER_PIN, HIGH);
  buzzerActive = true;
  buzzerStartTime = millis();
  buzzerDuration = duration;
}

// ==================== PUBLISH LOG (Thread-Safe) ====================
void publishLog(String uid, String name, String status, String type) {
  // Save to local file storage (LittleFS) - fast operation
  saveLogToFile(uid, name, status, type);
  
  // Queue the log for Firebase (non-blocking) - Firebase task will process it
  if (logQueue != NULL) {
    LogEntry entry;
    uid.toCharArray(entry.uid, sizeof(entry.uid));
    name.toCharArray(entry.name, sizeof(entry.name));
    status.toCharArray(entry.status, sizeof(entry.status));
    type.toCharArray(entry.type, sizeof(entry.type));
    
    if (xQueueSend(logQueue, &entry, 0) == pdTRUE) {
      Serial.println("Log queued for Firebase");
    } else {
      Serial.println("Log queue full - saved locally only");
    }
  }
}

// ==================== NTP TIME ====================
void updateNTPTime() {
  timeClient.update();
  lastNTPUpdate = millis();
  Serial.println("NTP time updated: " + timeClient.getFormattedTime());
}

String getISOTimestamp() {
  unsigned long epochTime = timeClient.getEpochTime();
  
  time_t rawTime = (time_t)epochTime;
  struct tm* timeInfo = gmtime(&rawTime);
  
  char buffer[25];
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", timeInfo);
  
  return String(buffer);
}

// ==================== NVS INITIALIZATION ====================
void initializeNVSIfNeeded() {
  if (!prefs.isKey(NVS_WHITELIST)) {
    prefs.putString(NVS_WHITELIST, "{}");
    Serial.println("Initialized empty whitelist");
  }
  
  if (!prefs.isKey(NVS_BLACKLIST)) {
    prefs.putString(NVS_BLACKLIST, "{}");
    Serial.println("Initialized empty blacklist");
  }
  
  if (!prefs.isKey(NVS_PENDING)) {
    prefs.putString(NVS_PENDING, "{}");
    Serial.println("Initialized empty pending list");
  }
  
  Serial.println("NVS Storage initialized");
  Serial.println("Whitelist: " + prefs.getString(NVS_WHITELIST, "{}"));
  Serial.println("Blacklist: " + prefs.getString(NVS_BLACKLIST, "{}"));
  Serial.println("Pending: " + prefs.getString(NVS_PENDING, "{}"));
}

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