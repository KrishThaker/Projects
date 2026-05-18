/*
 * ============================================================
 *  RFID + Fingerprint Dual-Verification Attendance System
 *  AUTH METHOD: Database Secret (simplest, no token needed)
 * ============================================================
 *
 *  Required Libraries:
 *    - MFRC522                              by GithubCommunity
 *    - Adafruit Fingerprint Sensor Library  by Adafruit
 *    - Firebase ESP Client                  by Mobizt
 *    - ArduinoJson                          by Benoit Blanchon
 * ============================================================
 */

#include <WiFi.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Adafruit_Fingerprint.h>
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"
#include <time.h>

// ─────────────────────────────────────────────
//  WiFi
// ─────────────────────────────────────────────
#define WIFI_SSID        "Kevin"
#define WIFI_PASSWORD    "juxc0453"

// ─────────────────────────────────────────────
//  Firebase — paste your Database Secret below
//  Firebase Console → Project Settings →
//  Service Accounts → Database secrets → Show
// ─────────────────────────────────────────────
#define DATABASE_SECRET  "Ut1tEZWbTWkpElLWiUN1iyL4SKI29ZRd5Ww6ZBCV"
#define DATABASE_URL     "https://attendance-system-904f3-default-rtdb.asia-southeast1.firebasedatabase.app"

// ─────────────────────────────────────────────
//  NTP — IST
// ─────────────────────────────────────────────
#define NTP_SERVER1      "pool.ntp.org"
#define NTP_SERVER2      "time.nist.gov"
#define GMT_OFFSET_SEC   19800   // UTC+5:30
#define DAYLIGHT_OFFSET  0

// ─────────────────────────────────────────────
//  Pins
// ─────────────────────────────────────────────
#define RFID_SS_PIN   5
#define RFID_RST_PIN  22
#define FP_RX_PIN     16
#define FP_TX_PIN     17
#define BUZZER_PIN    26
#define LED_GREEN     27
#define LED_RED       14

// ─────────────────────────────────────────────
//  Objects
// ─────────────────────────────────────────────
MFRC522               rfid(RFID_SS_PIN, RFID_RST_PIN);
HardwareSerial        fpSerial(2);
Adafruit_Fingerprint  finger(&fpSerial);
FirebaseData          fbdo;
FirebaseData          fbdo2;
FirebaseAuth          auth;
FirebaseConfig        config;

// ─────────────────────────────────────────────
//  State Machine
// ─────────────────────────────────────────────
enum State { IDLE, WAITING_FINGER };
State currentState = IDLE;

String pendingUID  = "";
String pendingName = "";
int    pendingFpId = -1;

unsigned long stateTimeout = 0;
const unsigned long FP_TIMEOUT = 10000;
bool firebaseReady = false;

// ─────────────────────────────────────────────
//  Buzzer / LED
// ─────────────────────────────────────────────
void beepWaiting() {
  tone(BUZZER_PIN, 900, 100); delay(150);
  tone(BUZZER_PIN, 1100, 100); delay(200);
}
void beepSuccess() {
  digitalWrite(LED_GREEN, HIGH);
  tone(BUZZER_PIN, 1000, 150); delay(200);
  tone(BUZZER_PIN, 1400, 200); delay(400);
  digitalWrite(LED_GREEN, LOW);
}
void beepFail() {
  digitalWrite(LED_RED, HIGH);
  tone(BUZZER_PIN, 400, 600); delay(700);
  digitalWrite(LED_RED, LOW);
}
void beepMismatch() {
  digitalWrite(LED_RED, HIGH);
  tone(BUZZER_PIN, 700, 200); delay(250);
  tone(BUZZER_PIN, 350, 400); delay(500);
  digitalWrite(LED_RED, LOW);
}
void beepReady() {
  tone(BUZZER_PIN, 800, 80); delay(120);
  tone(BUZZER_PIN, 1000, 80); delay(120);
}
void beepTimeout() {
  tone(BUZZER_PIN, 500, 100); delay(150);
  tone(BUZZER_PIN, 500, 100);
}

// ─────────────────────────────────────────────
//  Time
// ─────────────────────────────────────────────
String getTimestamp() {
  struct tm t;
  if (!getLocalTime(&t)) return "unknown";
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &t);
  return String(buf);
}
String getDate() {
  struct tm t;
  if (!getLocalTime(&t)) return "unknown";
  char buf[11];
  strftime(buf, sizeof(buf), "%Y-%m-%d", &t);
  return String(buf);
}
String getTimeStr() {
  struct tm t;
  if (!getLocalTime(&t)) return "unknown";
  char buf[9];
  strftime(buf, sizeof(buf), "%H:%M:%S", &t);
  return String(buf);
}

bool syncTime() {
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET, NTP_SERVER1, NTP_SERVER2);
  Serial.print("      Syncing time");
  struct tm timeinfo;
  int tries = 0;
  while (!getLocalTime(&timeinfo) && tries < 20) {
    delay(500); Serial.print("."); tries++;
  }
  if (getLocalTime(&timeinfo)) {
    char buf[30];
    strftime(buf, sizeof(buf), "%d-%b-%Y %H:%M:%S", &timeinfo);
    Serial.println(" ✓  " + String(buf) + " IST");
    return true;
  }
  Serial.println(" ✗ failed");
  return false;
}

// ─────────────────────────────────────────────
//  Firebase: Get user by UID
// ─────────────────────────────────────────────
bool getUserByUID(String uid, String &name, int &fpId) {
  String namePath = "/users/" + uid + "/name";
  Serial.println("  Path: " + namePath);

  if (!Firebase.RTDB.getString(&fbdo, namePath)) {
    Serial.println("  Error: " + fbdo.errorReason());
    Serial.println("  HTTP:  " + String(fbdo.httpCode()));
    return false;
  }

  name = fbdo.stringData();
  if (name.length() == 0) {
    Serial.println("  Error: name is empty");
    return false;
  }
  Serial.println("  Name: " + name);

  String fpPath = "/users/" + uid + "/fp_id";
  if (!Firebase.RTDB.getInt(&fbdo, fpPath)) {
    Serial.println("  Error getting fp_id: " + fbdo.errorReason());
    return false;
  }
  fpId = fbdo.intData();
  Serial.println("  fp_id: " + String(fpId));
  return true;
}

// ─────────────────────────────────────────────
//  Firebase: Log attendance
// ─────────────────────────────────────────────
void logAttendance(String uid, String name) {
  if (!firebaseReady) return;

  String date    = getDate();
  String timeStr = getTimeStr();
  String ts      = getTimestamp();

  FirebaseJson record;
  record.set("uid",       uid);
  record.set("name",      name);
  record.set("method",    "RFID+Fingerprint");
  record.set("timestamp", ts);
  record.set("time",      timeStr);
  record.set("date",      date);
  record.set("status",    "present");

  if (Firebase.RTDB.pushJSON(&fbdo2, "/attendance/" + date, &record)) {
    Serial.println("  ✓ Logged: " + name + " @ " + timeStr);
  } else {
    Serial.println("  ✗ Log error: " + fbdo2.errorReason());
  }

  FirebaseJson live;
  live.set("uid",    uid);
  live.set("name",   name);
  live.set("method", "RFID+Fingerprint");
  live.set("time",   timeStr);
  live.set("date",   date);
  live.set("status", "present");
  Firebase.RTDB.setJSON(&fbdo2, "/live/" + uid, &live);
}

// ─────────────────────────────────────────────
//  Reset
// ─────────────────────────────────────────────
void resetState() {
  currentState = IDLE;
  pendingUID = pendingName = "";
  pendingFpId  = -1;
  stateTimeout = 0;
}

// ─────────────────────────────────────────────
//  RFID Handler
// ─────────────────────────────────────────────
void handleRFID() {
  if (!rfid.PICC_IsNewCardPresent()) return;
  if (!rfid.PICC_ReadCardSerial())   return;

  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  Serial.println("\n──────────────────────");
  Serial.println("Card UID: " + uid);

  String name = "";
  int    fpId = -1;

  if (!getUserByUID(uid, name, fpId)) {
    Serial.println("✗ Not registered. Add /users/" + uid + " to Firebase.");
    beepFail();
    return;
  }

  Serial.println("✓ Hello " + name + " — place finger (10s)...");
  pendingUID   = uid;
  pendingName  = name;
  pendingFpId  = fpId;
  stateTimeout = millis() + FP_TIMEOUT;
  currentState = WAITING_FINGER;
  beepWaiting();
}

// ─────────────────────────────────────────────
//  Fingerprint Handler
// ─────────────────────────────────────────────
void handleFingerprint() {
  uint8_t p = finger.getImage();
  if (p == FINGERPRINT_NOFINGER) return;
  if (p != FINGERPRINT_OK) { beepFail(); resetState(); return; }

  p = finger.image2Tz();
  if (p != FINGERPRINT_OK) {
    Serial.println("  Image convert error");
    beepFail(); resetState(); return;
  }

  p = finger.fingerSearch();
  if (p != FINGERPRINT_OK) {
    Serial.println("✗ Fingerprint not found in sensor.");
    beepFail(); resetState(); return;
  }

  uint16_t matchedId  = finger.fingerID;
  uint16_t confidence = finger.confidence;
  Serial.println("  Matched slot=" + String(matchedId) + " conf=" + String(confidence));

  if (confidence < 50) {
    Serial.println("✗ Low confidence — rejected.");
    beepFail(); resetState(); return;
  }

  if ((int)matchedId != pendingFpId) {
    Serial.println("✗ MISMATCH — card owner fp_id=" + String(pendingFpId) +
                   " but finger is slot=" + String(matchedId));
    beepMismatch(); resetState(); return;
  }

  Serial.println("✓ VERIFIED — Welcome " + pendingName + "!");
  logAttendance(pendingUID, pendingName);
  beepSuccess();
  resetState();
}

// ─────────────────────────────────────────────
//  Enrollment
// ─────────────────────────────────────────────
void enrollFingerprint(uint8_t id) {
  Serial.println("\n── Enrolling slot " + String(id) + " ──");
  Serial.println("Step 1: Place finger...");
  int p = -1;
  while (p != FINGERPRINT_OK) p = finger.getImage();
  if (finger.image2Tz(1) != FINGERPRINT_OK) { Serial.println("Error 1"); return; }

  Serial.println("Step 2: Remove...");
  delay(2000);
  while (finger.getImage() != FINGERPRINT_NOFINGER) delay(200);

  Serial.println("Step 3: Same finger again...");
  p = -1;
  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
    if (p == FINGERPRINT_NOFINGER) continue;
  }
  if (finger.image2Tz(2) != FINGERPRINT_OK) { Serial.println("Error 2"); return; }
  if (finger.createModel()  != FINGERPRINT_OK) { Serial.println("No match"); return; }
  if (finger.storeModel(id) != FINGERPRINT_OK) { Serial.println("Store err"); return; }

  Serial.println("✓ Enrolled slot " + String(id));
  Serial.println("  Set fp_id=" + String(id) + " in Firebase for this person.");
}
void deleteFingerprint(uint8_t id) {
  Serial.println("Deleting fingerprint ID: " + String(id));

  uint8_t p = finger.deleteModel(id);

  if (p == FINGERPRINT_OK) {
    Serial.println("✅ Deleted successfully");
  } else {
    Serial.println("❌ Error deleting fingerprint");
  }
}
// ─────────────────────────────────────────────
//  Setup
// ─────────────────────────────────────────────
void setup() {
  
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n====================================");
  Serial.println("  Attendance System  Starting...");
  Serial.println("====================================");

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_GREEN,  OUTPUT);
  pinMode(LED_RED,    OUTPUT);
  digitalWrite(LED_GREEN, LOW);
  digitalWrite(LED_RED,   LOW);

  // RFID
  SPI.begin();
  rfid.PCD_Init();
  delay(100);
  Serial.println("[1/5] RFID ✓");

  // Fingerprint
  fpSerial.begin(57600, SERIAL_8N1, FP_RX_PIN, FP_TX_PIN);
  finger.begin(57600);
  delay(200);
  if (finger.verifyPassword()) {
    Serial.println("[2/5] Fingerprint ✓  Templates: " + String(finger.templateCount));
    
  } else {
    Serial.println("[2/5] ⚠ Fingerprint NOT found");
  }

  // WiFi
  Serial.print("[3/5] WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int t = 0;
  while (WiFi.status() != WL_CONNECTED && t < 40) {
    delay(500); Serial.print("."); t++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(" ✓  IP: " + WiFi.localIP().toString());
  } else {
    Serial.println(" ✗ WiFi failed!");
  }

  // NTP
  Serial.print("[4/5] NTP ");
  syncTime();

  // Firebase with Database Secret
  Serial.print("[5/5] Firebase");
  config.database_url = DATABASE_URL;
  config.signer.tokens.legacy_token = DATABASE_SECRET;

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  fbdo.setResponseSize(4096);
  fbdo2.setResponseSize(4096);

  delay(2000);
  firebaseReady = true;
  Serial.println(" ✓");

  // Test read to confirm connection
  Serial.println("  Testing Firebase connection...");
  if (Firebase.RTDB.getString(&fbdo, "/users/test")) {
    Serial.println("  Connection OK");
  } else {
    Serial.println("  Test result: " + fbdo.errorReason());
    Serial.println("  (path not found is OK — means DB is reachable)");
  }

  beepReady();
  Serial.println("\n====================================");
  Serial.println("  READY — Tap card | Send E=enroll");
  Serial.println("====================================\n");
}

// ─────────────────────────────────────────────
//  Loop
// ─────────────────────────────────────────────
void loop() {
  if (Serial.available()) {
    char cmd = Serial.read();
    if (cmd == 'E' || cmd == 'e') {
      Serial.println("\nSlot ID to enroll (1-127):");
      while (!Serial.available()) delay(100);
      uint8_t id = (uint8_t)Serial.parseInt();
      if (id >= 1 && id <= 127) enrollFingerprint(id);
      else Serial.println("Invalid. Use 1-127.");
    }
  }

  if (currentState == IDLE) {
    handleRFID();
  } else if (currentState == WAITING_FINGER) {
    if (millis() > stateTimeout) {
      Serial.println("⚠ Timeout — tap card again.");
      beepTimeout();
      resetState();
    } else {
      handleFingerprint();
    }
  }

  delay(50);
}
