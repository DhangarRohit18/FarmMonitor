#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <ESP32Servo.h>
#include <TinyGPS++.h> 
#include <time.h>

// ======================== CONFIGURATION ========================
const char* ssid = "noname";
const char* password = "passwordnahiaahe";

const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 19800; // IST is GMT +5:30
const int   daylightOffset_sec = 0;

const String PROJECT_ID = "evbooking-1bb64";
const String DEVICE_ID  = "esp32_01";

const String BASE_URL = "https://firestore.googleapis.com/v1/projects/" + PROJECT_ID + "/databases/(default)/documents/farm_devices/";
const String FIRESTORE_URL = BASE_URL + DEVICE_ID;
const String HISTORY_URL = FIRESTORE_URL + "/history";

// ======================== PINS ========================
#define DHT_PIN 4
#define SOIL_MOISTURE_PIN 34
#define IR_PIN 19
#define LED_PIN 18

const int SERVO_PIN = 25;
#define RXD2 16
#define TXD2 17

// ======================== DEVICES ========================
DHT dht(DHT_PIN, DHT11);
Servo waterValveServo;
TinyGPSPlus gps;
HardwareSerial SerialGPS(2);

// ======================== VARIABLES ========================
int moistureThreshold = 25;

String currentMode = "AUTO";
bool motorOverride = false;
bool motorStatus = false;

bool securityAlert = false;

// ======================== SETUP ========================
void setup() {
  Serial.begin(115200);
  Serial.println("\n--- AgriSense Pro: Final System Initializing ---");

  // Sensors
  dht.begin();

  // IR + LED
  pinMode(IR_PIN, INPUT);
  pinMode(LED_PIN, OUTPUT);

  // Servo setup
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  waterValveServo.setPeriodHertz(50);
  waterValveServo.attach(SERVO_PIN, 500, 2400);

  // GPS Serial setup
  SerialGPS.begin(9600, SERIAL_8N1, RXD2, TXD2);

  // Safe start
  Serial.println("System: Closing Valve (Safe Start)...");
  waterValveServo.write(0);
  motorStatus = false;

  // WiFi
  WiFi.begin(ssid, password);
  Serial.print("WiFi: Connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print(".");
  }
  Serial.println("\nWiFi: Connected! IP: " + WiFi.localIP().toString());

  // Init and get the time
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  Serial.println("System: Syncing Time with NTP...");
}

// ======================== LOOP ========================
void loop() {

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("System: Waiting for WiFi...");
    delay(1000);
    return;
  }

  // 📡 0. PROCESS GPS
  while (SerialGPS.available() > 0) {
    gps.encode(SerialGPS.read());
  }

  // 🔐 1. SECURITY SYSTEM (IR SENSOR)
  int intruder = digitalRead(IR_PIN);

  if (intruder == HIGH) {
    securityAlert = true;
    digitalWrite(LED_PIN, HIGH);
    Serial.println("!!! ALERT: INTRUDER DETECTED !!!");
  } else {
    securityAlert = false;
    digitalWrite(LED_PIN, LOW);
  }

  // ☁️ 2. Fetch Cloud Commands
  fetchFirestoreConfig();

  // 🌱 3. Read Sensors
  int rawMoisture = analogRead(SOIL_MOISTURE_PIN);
  int moisturePercent = map(rawMoisture, 4095, 1500, 0, 100);
  moisturePercent = constrain(moisturePercent, 0, 100);

  float temp = dht.readTemperature();
  float hum = dht.readHumidity();

  // 🚿 4. Irrigation Logic
  bool motorShouldRun = false;

  if (currentMode == "MANUAL") {
    motorShouldRun = motorOverride;
  } else {
    if (moisturePercent < 22) {
      motorShouldRun = true;
    } else if (moisturePercent > 28) {
      motorShouldRun = false;
    } else {
      motorShouldRun = motorStatus;
    }
  }

  // ⚙️ 5. Actuate Servo
  if (motorShouldRun && !motorStatus) {
    Serial.println(">>> ACTION: OPEN VALVE (180°)");
    waterValveServo.write(180);
    motorStatus = true;
    logHistoricalEvent("Irrigation ON");
  } else if (!motorShouldRun && motorStatus) {
    Serial.println(">>> ACTION: CLOSE VALVE (0°)");
    waterValveServo.write(0);
    motorStatus = false;
    logHistoricalEvent("Irrigation OFF");
  }

  // ☁️ 6. Sync Data to Firebase
  syncDataToFirestore(moisturePercent, temp, hum);

  // 🧾 7. Debug Output
  Serial.print("Moisture: "); Serial.print(moisturePercent);
  Serial.print("% | Mode: "); Serial.print(currentMode);
  Serial.print(" | Motor: "); Serial.print(motorStatus ? "ACTIVE" : "OFF");
  Serial.print(" | Security: "); Serial.print(securityAlert ? "ALERT 🚨" : "SAFE");
  if (gps.location.isValid()) {
    Serial.print(" | GPS: "); Serial.print(gps.location.lat(), 6); Serial.print(","); Serial.println(gps.location.lng(), 6);
  } else {
    Serial.println(" | GPS: Searching...");
  }

  delay(1000);
}

// ======================== FETCH FIRESTORE ========================
void fetchFirestoreConfig() {
  HTTPClient http;
  http.setConnectTimeout(500);
  http.begin(FIRESTORE_URL);

  int httpCode = http.GET();

  if (httpCode == 200) {
    String payload = http.getString();
    DynamicJsonDocument doc(1500);
    deserializeJson(doc, payload);

    if (!doc["fields"]["mode"]["stringValue"].isNull()) {
      currentMode = doc["fields"]["mode"]["stringValue"].as<String>();
    }

    if (!doc["fields"]["motorOverride"]["booleanValue"].isNull()) {
      motorOverride = doc["fields"]["motorOverride"]["booleanValue"].as<bool>();
    }
  }

  http.end();
}

// ======================== FIREBASE SYNC ========================
void syncDataToFirestore(int moisture, float t, float h) {
  HTTPClient http;
  http.setConnectTimeout(500);

  String url = FIRESTORE_URL +
    "?updateMask.fieldPaths=moisture"
    "&updateMask.fieldPaths=temperature"
    "&updateMask.fieldPaths=humidity"
    "&updateMask.fieldPaths=motorStatus"
    "&updateMask.fieldPaths=securityAlert"
    "&updateMask.fieldPaths=latitude"
    "&updateMask.fieldPaths=longitude";

  http.begin(url);
  http.addHeader("Content-Type", "application/json");

  DynamicJsonDocument doc(1024);
  JsonObject fields = doc.createNestedObject("fields");

  fields["moisture"]["integerValue"] = moisture;
  fields["temperature"]["doubleValue"] = isnan(t) ? 0 : t;
  fields["humidity"]["doubleValue"] = isnan(h) ? 0 : h;
  fields["motorStatus"]["booleanValue"] = motorStatus;
  fields["securityAlert"]["booleanValue"] = securityAlert;

  if (gps.location.isValid()) {
    fields["latitude"]["doubleValue"] = gps.location.lat();
    fields["longitude"]["doubleValue"] = gps.location.lng();
  }

  String jsonPayload;
  serializeJson(doc, jsonPayload);

  http.PATCH(jsonPayload);
  http.end();
}

// ======================== HISTORY LOGGING ========================
void logHistoricalEvent(String eventType) {
  HTTPClient http;
  http.setConnectTimeout(1000);
  http.begin(HISTORY_URL);
  http.addHeader("Content-Type", "application/json");

  // Create a minimal JSON to record the event
  DynamicJsonDocument doc(512);
  JsonObject fields = doc.createNestedObject("fields");
  
  fields["event"]["stringValue"] = eventType;
  fields["mode"]["stringValue"] = currentMode;
  fields["timestampValue"]["stringValue"] = getFormattedTime(); // REAL TIME
  
  // Create a structured log entry
  String jsonPayload;
  serializeJson(doc, jsonPayload);

  int httpCode = http.POST(jsonPayload);
  if (httpCode > 0) {
    Serial.println("History Log Sent: " + eventType);
  } else {
    Serial.println("History Log Failed: " + String(httpCode));
  }
  http.end();
}

String getFormattedTime() {
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){
    return "00-00 00:00";
  }
  char timeStringBuff[50];
  // Format: "11 Apr, 01:52 PM"
  strftime(timeStringBuff, sizeof(timeStringBuff), "%d %b, %I:%M %p", &timeinfo);
  return String(timeStringBuff);
}