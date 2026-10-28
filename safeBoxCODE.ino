#include <WiFi.h>
#include <time.h>
#include <Firebase_ESP_Client.h>
#include <Keypad.h>
#include <ESP32Servo.h>
#include "addons/TokenHelper.h"

// ======== WIFI AND FIREBASE DETAILS ========
// Tip: if PU_GOA does not work, use your phone hotspot (2.4 GHz) here.
#define WIFI_SSID "PU_GOA"
#define WIFI_PASSWORD "PU@GOA2025"
#define API_KEY "AIzaSyCYt9MSLvT49qjK3wYXSgQjbnDjoahZLIU"
#define PROJECT_ID "safe-box-cbcf8"
#define USER_EMAIL "esp32@safebox.com"
#define USER_PASSWORD "esp32password"

// ======== HARDWARE PINS AND SETTINGS ========
#define SERVO_PIN 25
#define RELAY_PIN 26            // controls the 12V solenoid through the relay
#define CLOSED_ANGLE 0
#define OPEN_ANGLE 180
#define RELAY_ON LOW            // if the solenoid does the opposite, swap LOW and HIGH
#define RELAY_OFF HIGH
const unsigned long SOLENOID_ON_MS = 3000;   // 0 = keep powered until the door closes
const bool SHOW_DIGITS = true;               // false = show * instead of typed digits

// ======== KEYPAD ========
const byte ROWS = 4;
const byte COLS = 4;
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
byte rowPins[ROWS] = {13, 14, 16, 17};
byte colPins[COLS] = {18, 19, 21, 22};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// ======== OBJECTS AND VARIABLES ========
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;
Servo doorServo;

String enteredOTP = "";
int64_t otpExpiresAt = 0;
bool waitingToClose = false;
int wrongTries = 0;
unsigned long lockedUntil = 0;
unsigned long solenoidOffAt = 0;
unsigned long lastWifiCheck = 0;

// ======== UPDATE FIRESTORE STATUS ========
void setStatus(const char* newStatus) {
  if (!Firebase.ready()) {
    Serial.println("Firebase not ready; status not updated.");
    return;
  }
  FirebaseJson content;
  content.set("fields/status/stringValue", newStatus);
  if (Firebase.Firestore.patchDocument(&fbdo, PROJECT_ID, "", "safe_box/device_001", content.raw(), "status")) {
    Serial.print("Firebase status updated to: ");
    Serial.println(newStatus);
  } else {
    Serial.print("Status update failed: ");
    Serial.println(fbdo.errorReason());
  }
}

// ======== FIREBASE TEST (press A on the keypad) ========
void testFirebase() {
  Serial.println("\n----- FIREBASE TEST -----");
  Serial.print("WiFi connected: ");
  Serial.println(WiFi.status() == WL_CONNECTED ? "yes" : "NO");
  Serial.print("WiFi signal (RSSI, closer to 0 is better): ");
  Serial.println(WiFi.RSSI());
  Serial.print("Firebase ready: ");
  Serial.println(Firebase.ready() ? "yes" : "NO");
  Serial.print("Free memory: ");
  Serial.println(ESP.getFreeHeap());

  if (Firebase.ready()) {
    Serial.println("Reading document safe_box/device_001 ...");
    unsigned long t = millis();
    if (Firebase.Firestore.getDocument(&fbdo, PROJECT_ID, "", "safe_box/device_001", "status")) {
      Serial.println("Read OK:");
      Serial.println(fbdo.payload());
    } else {
      Serial.print("Read FAILED: ");
      Serial.println(fbdo.errorReason());
    }
    Serial.print("Took (ms): ");
    Serial.println(millis() - t);
  }
  Serial.println("-------------------------");
}

// ======== VERIFY SIX-DIGIT OTP ========
void verifyOTP(String inputCode) {
  if (millis() < lockedUntil) {
    Serial.println("Keypad locked. Wait 30 seconds.");
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi is disconnected. Reconnecting...");
    WiFi.reconnect();
    return;
  }
  if (!Firebase.ready()) {
    Serial.println("Firebase is not ready yet. Wait a few seconds and try again.");
    return;
  }

  Serial.println("Reading from Firebase...");
  unsigned long t = millis();
  if (!Firebase.Firestore.getDocument(&fbdo, PROJECT_ID, "", "safe_box/device_001", "current_otp,status,expires_at")) {
    Serial.print("Firestore read failed: ");
    Serial.println(fbdo.errorReason());
    return;
  }
  Serial.print("Read took (ms): ");
  Serial.println(millis() - t);

  FirebaseJson payload;
  FirebaseJsonData jsonData;
  payload.setJsonData(fbdo.payload().c_str());

  String status = "";
  if (payload.get(jsonData, "fields/status/stringValue")) status = jsonData.stringValue;

  String validOTP = "";
  if (payload.get(jsonData, "fields/current_otp/stringValue")) validOTP = jsonData.stringValue;

  String expiryText = "";
  if (payload.get(jsonData, "fields/expires_at/integerValue")) expiryText = jsonData.stringValue;
  if (expiryText.length() == 0) {
    if (payload.get(jsonData, "fields/expires_at/stringValue")) expiryText = jsonData.stringValue;
  }

  bool validTimestamp = expiryText.length() > 0;
  for (unsigned int i = 0; i < expiryText.length(); i++) {
    if (expiryText[i] < '0' || expiryText[i] > '9') { validTimestamp = false; break; }
  }
  int64_t expiresAt = 0;
  if (validTimestamp) {
    expiresAt = strtoll(expiryText.c_str(), nullptr, 10);
    if (expiresAt <= 0) validTimestamp = false;
  }
  int64_t now = (int64_t)time(nullptr);

  Serial.println("\n----- OTP DEBUG -----");
  Serial.print("Firebase status: ");
  Serial.println(status);
  Serial.print("Expiry timestamp: ");
  Serial.println((long long)expiresAt);
  Serial.print("Current timestamp: ");
  Serial.println((long long)now);
  Serial.println("---------------------");

  if (status != "ACTIVE") {
    Serial.println("Code is not ACTIVE. Generate a new key in the app.");
    return;
  }
  if (!validTimestamp) {
    Serial.println("Invalid expires_at field. Is the updated app installed?");
    return;
  }
  if (now >= expiresAt) {
    Serial.println("Code expired.");
    setStatus("EXPIRED");
    return;
  }
  if (validOTP.length() != 6) {
    Serial.println("Firebase OTP is not six digits.");
    return;
  }

  if (validOTP == inputCode) {
    Serial.println("Correct OTP! Door opening.");
    wrongTries = 0;

    digitalWrite(RELAY_PIN, RELAY_ON);   // 1. release the solenoid latch
    delay(500);                          // 2. wait for the latch to clear
    doorServo.write(OPEN_ANGLE);         // 3. servo opens the door
    if (SOLENOID_ON_MS > 0) solenoidOffAt = millis() + SOLENOID_ON_MS;

    otpExpiresAt = expiresAt;
    waitingToClose = true;
    setStatus("USED");
    Serial.println("SafeBox unlocked.");
  } else {
    Serial.println("Wrong OTP.");
    wrongTries++;
    if (wrongTries >= 3) {
      lockedUntil = millis() + 30000UL;
      wrongTries = 0;
      Serial.println("Too many wrong tries. Keypad locked for 30 seconds.");
    }
  }
}

// ======== READ KEYPAD ========
void readKeypad() {
  char key = keypad.getKey();
  if (key == NO_KEY) return;

  if (key == 'A') {              // test button
    testFirebase();
    return;
  }
  if (key == '*' || key == '#') {
    enteredOTP = "";
    Serial.println("\nCleared");
    return;
  }
  if (key < '0' || key > '9') return;   // ignore B C D
  if (enteredOTP.length() >= 6) return;

  enteredOTP += key;
  Serial.print(SHOW_DIGITS ? key : '*');

  if (enteredOTP.length() == 6) {
    String code = enteredOTP;
    enteredOTP = "";
    Serial.println("\nChecking OTP...");
    verifyOTP(code);
  }
}

// ======== TURN OFF SOLENOID AFTER A FEW SECONDS ========
void handleSolenoid() {
  if (solenoidOffAt != 0 && (long)(millis() - solenoidOffAt) >= 0) {
    digitalWrite(RELAY_PIN, RELAY_OFF);
    solenoidOffAt = 0;
    Serial.println("Solenoid power off.");
  }
}

// ======== CLOSE THE DOOR WHEN THE OTP EXPIRES ========
void handleAutoClose() {
  if (!waitingToClose) return;
  int64_t now = (int64_t)time(nullptr);
  if (now < 1700000000) return;
  if (now < otpExpiresAt) return;

  waitingToClose = false;
  doorServo.write(CLOSED_ANGLE);         // 1. servo closes the door
  delay(500);                            // 2. wait for it to finish moving
  digitalWrite(RELAY_PIN, RELAY_OFF);    // 3. make sure the solenoid is off (locked)
  solenoidOffAt = 0;
  Serial.println("OTP expired. SafeBox locked.");
}

// ======== KEEP WIFI ALIVE ========
void handleWifi() {
  if (millis() - lastWifiCheck < 10000) return;
  lastWifiCheck = millis();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi lost. Reconnecting...");
    WiFi.reconnect();
  }
}

// ======== STARTUP HELPERS ========
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  for (int attempt = 1; attempt <= 3; attempt++) {
    Serial.printf("\nConnecting to WiFi (try %d of 3)", attempt);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
      delay(500);
      Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nWiFi connected.");
      Serial.print("IP address: ");
      Serial.println(WiFi.localIP());
      return;
    }
    WiFi.disconnect();
    delay(1000);
  }
  Serial.println("\nWiFi FAILED. Check the name and password, and use a 2.4 GHz network or phone hotspot.");
  Serial.println("Restarting in 5 seconds...");
  delay(5000);
  ESP.restart();
}

void syncTime() {
  configTime(0, 0, "time.google.com", "pool.ntp.org", "time.cloudflare.com");
  Serial.print("Synchronizing time");
  unsigned long start = millis();
  while (time(nullptr) < 1700000000 && millis() - start < 30000) {
    delay(500);
    Serial.print(".");
  }
  if (time(nullptr) < 1700000000) {
    Serial.println("\nTime sync FAILED. This WiFi may block time servers. Try a phone hotspot.");
    Serial.println("Restarting in 5 seconds...");
    delay(5000);
    ESP.restart();
  }
  Serial.println("\nTime synchronized.");
  Serial.print("Current Unix timestamp: ");
  Serial.println((long long)time(nullptr));
}

void connectFirebase() {
  config.api_key = API_KEY;
  auth.user.email = USER_EMAIL;
  auth.user.password = USER_PASSWORD;
  config.token_status_callback = tokenStatusCallback;

  // Bigger buffers and clear time limits, so problems show up as messages, not hangs
  fbdo.setBSSLBufferSize(4096, 1024);
  fbdo.setResponseSize(4096);
  config.timeout.serverResponse = 15 * 1000;
  config.timeout.socketConnection = 15 * 1000;
  config.timeout.sslHandshake = 30 * 1000;

  Firebase.reconnectWiFi(true);
  Firebase.begin(&config, &auth);

  Serial.print("Signing in to Firebase");
  unsigned long start = millis();
  while (!Firebase.ready() && millis() - start < 30000) {
    delay(500);
    Serial.print(".");
  }
  if (Firebase.ready()) {
    Serial.println("\nFirebase signed in.");
  } else {
    Serial.println("\nFirebase sign-in not finished. Check: ESP32 user exists, Email/Password enabled, API key correct.");
    Serial.println("It will keep trying in the background.");
  }
}

// ======== SETUP ========
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nESP32 SafeBox starting...");

  // Relay off BEFORE it becomes an output, so the solenoid does not twitch
  digitalWrite(RELAY_PIN, RELAY_OFF);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_OFF);

  ESP32PWM::allocateTimer(0);
  doorServo.setPeriodHertz(50);
  doorServo.attach(SERVO_PIN, 500, 2400);
  doorServo.write(CLOSED_ANGLE);

  connectWiFi();
  syncTime();
  connectFirebase();

  Serial.println("SafeBox ready. Enter your six-digit OTP.");
  Serial.println("Tip: press A on the keypad to test the Firebase connection.");
}

// ======== MAIN LOOP ========
void loop() {
  readKeypad();
  handleSolenoid();
  handleAutoClose();
  handleWifi();
}