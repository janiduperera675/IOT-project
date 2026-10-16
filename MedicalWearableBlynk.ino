/*
  Medical Wearable Monitor -> Blynk Cloud -> CareFlow iOS app
  Board: Arduino UNO R4 WiFi
  Sensors: DS18B20 (D2), MAX30102 (I2C), BMP180 (I2C)

  Blynk datastreams (template "Medical Wearable Monitor"):
    V1 Heart Rate (bpm)      0 = no valid reading (finger off)
    V2 SpO2 (%)              0 = no valid reading (finger off)
    V3 Temperature (C)
    V4 Ambient Pressure (Pa; NOT blood pressure)
    V5 Altitude (m)
    V6 Wearable ID (string)

  The CareFlow app polls https://blynk.cloud/external/api/getAll every 5 s and
  treats 0 on V1/V2 as "no reading", so stale values never trigger alerts.
*/

#include <OneWire.h>
#include <DallasTemperature.h>
#include <Wire.h>
#include "MAX30105.h"
#include "spo2_algorithm.h"
#include <Adafruit_BMP085.h>
#include <WiFiS3.h>
#include <WiFiSSLClient.h>
#include "arduino_secrets.h"

#define ONE_WIRE_BUS 2
#define BUFFER_SIZE 100   // 4 s of samples at 25 Hz, as the Maxim algorithm expects
#define REFRESH_SIZE 25   // recompute once per second

// Demo limits only. Patient-specific limits live in the iOS app.
const int MIN_HEART_RATE = 50;
const int MAX_HEART_RATE = 120;
const int MIN_SPO2 = 92;
const float MAX_TEMPERATURE_C = 38.0;

const unsigned long UPLOAD_INTERVAL_MS = 5000;
const unsigned long WIFI_RETRY_INTERVAL_MS = 30000;
const uint32_t FINGER_IR_THRESHOLD = 50000;  // below this, nothing is on the sensor
const float NO_TEMPERATURE = -1000.0;

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature tempSensor(&oneWire);
MAX30105 particleSensor;
Adafruit_BMP085 bmp;
WiFiSSLClient blynkClient;

bool pulseSensorReady = false;
bool pressureSensorReady = false;
bool temperatureSensorReady = false;

uint32_t irBuffer[BUFFER_SIZE];
uint32_t redBuffer[BUFFER_SIZE];
int32_t rawSpO2 = 0;
int32_t rawHeartRate = 0;
int8_t validSpO2 = 0;
int8_t validHeartRate = 0;

// Reported values: smoothed, 0 when there is no trustworthy reading.
int heartRate = 0;
int spo2 = 0;
const int SMOOTHING = 4;
int heartHistory[SMOOTHING];
int spo2History[SMOOTHING];
int historyCount = 0;
int historyIndex = 0;

float temperatureC = NO_TEMPERATURE;
float pressurePa = 0;
float altitudeM = 0;

unsigned long lastUpload = 0;
unsigned long lastWiFiAttempt = 0;
bool wifiAttempted = false;

bool highHeartLatched = false;
bool lowHeartLatched = false;
bool lowSpO2Latched = false;
bool highTemperatureLatched = false;
bool sensorFaultLatched = false;

// ---------- Wi-Fi and Blynk ----------

bool ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  // WiFi.begin blocks for several seconds, so don't retry on every upload.
  if (wifiAttempted && millis() - lastWiFiAttempt < WIFI_RETRY_INTERVAL_MS) return false;
  wifiAttempted = true;
  lastWiFiAttempt = millis();

  if (WiFi.status() == WL_NO_MODULE) {
    Serial.println("Wi-Fi module not responding.");
    return false;
  }

  Serial.print("Connecting to Wi-Fi \"");
  Serial.print(WIFI_SSID);
  Serial.println("\"...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  for (int attempt = 0; attempt < 20 && WiFi.status() != WL_CONNECTED; attempt++) delay(250);

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi failed. On an iPhone hotspot, turn on Maximize Compatibility (2.4 GHz). Retrying in 30 s.");
    return false;
  }

  // The R4 can report connected before DHCP finishes.
  for (int attempt = 0; attempt < 20 && WiFi.localIP() == IPAddress(0, 0, 0, 0); attempt++) delay(250);
  Serial.print("Wi-Fi connected. IP: ");
  Serial.println(WiFi.localIP());
  return true;
}

// Sends one HTTPS GET to Blynk and returns the HTTP status code (0 if unreachable).
int blynkGet(const String& path) {
  if (!ensureWiFi()) return 0;
  if (!blynkClient.connect(BLYNK_SERVER, 443)) {
    Serial.println("Could not reach Blynk over TLS. See WINDOWS_SETUP.md (firmware updater / regional server).");
    return 0;
  }

  blynkClient.print("GET ");
  blynkClient.print(path);
  blynkClient.println(" HTTP/1.1");
  blynkClient.print("Host: ");
  blynkClient.println(BLYNK_SERVER);
  blynkClient.println("Connection: close");
  blynkClient.println();

  unsigned long deadline = millis() + 3000;
  while (!blynkClient.available() && blynkClient.connected() && millis() < deadline) delay(5);
  String statusLine = blynkClient.available() ? blynkClient.readStringUntil('\n') : "";
  blynkClient.stop();

  // "HTTP/1.1 200 OK"
  int firstSpace = statusLine.indexOf(' ');
  int status = firstSpace > 0 ? statusLine.substring(firstSpace + 1, firstSpace + 4).toInt() : 0;
  if (status != 200) {
    Serial.print("Blynk request failed: ");
    Serial.println(statusLine.length() ? statusLine : "no response");
    if (status == 400 || status == 401 || status == 403) {
      Serial.println("Check BLYNK_AUTH_TOKEN and BLYNK_SERVER in arduino_secrets.h.");
    }
  }
  return status;
}

void logEventOnce(const char* code, bool active, bool& latch) {
  if (active && !latch) {
    String path = "/external/api/logEvent?token=";
    path += BLYNK_AUTH_TOKEN;
    path += "&code=";
    path += code;
    blynkGet(path);  // fails harmlessly if the event isn't defined in the template
  }
  latch = active;
}

void uploadReadings() {
  String path = "/external/api/batch/update?token=";
  path += BLYNK_AUTH_TOKEN;
  if (pulseSensorReady) {
    // Always send V1/V2 so the app sees "no reading" instead of an old value.
    path += "&v1=" + String(heartRate);
    path += "&v2=" + String(spo2);
  }
  if (temperatureC > NO_TEMPERATURE) path += "&v3=" + String(temperatureC, 2);
  if (pressureSensorReady) {
    path += "&v4=" + String(pressurePa, 0);
    path += "&v5=" + String(altitudeM, 2);
  }
  path += "&v6=" + String(WEARABLE_ID);

  if (blynkGet(path) == 200) Serial.println("Uploaded to Blynk.");

  logEventOnce("heart_rate_high", heartRate > MAX_HEART_RATE, highHeartLatched);
  logEventOnce("heart_rate_low", heartRate > 0 && heartRate < MIN_HEART_RATE, lowHeartLatched);
  logEventOnce("spo2_low", spo2 > 0 && spo2 < MIN_SPO2, lowSpO2Latched);
  logEventOnce("temperature_high", temperatureC > MAX_TEMPERATURE_C, highTemperatureLatched);
  logEventOnce("sensor_fault", !pulseSensorReady || !pressureSensorReady || temperatureC <= NO_TEMPERATURE, sensorFaultLatched);
}

// ---------- Sensors ----------

void readPulseSample(int index) {
  while (!particleSensor.available()) particleSensor.check();
  redBuffer[index] = particleSensor.getRed();
  irBuffer[index] = particleSensor.getIR();
  particleSensor.nextSample();
}

void resetPulseHistory() {
  historyCount = 0;
  historyIndex = 0;
  heartRate = 0;
  spo2 = 0;
}

void updatePulse() {
  for (int i = REFRESH_SIZE; i < BUFFER_SIZE; i++) {
    redBuffer[i - REFRESH_SIZE] = redBuffer[i];
    irBuffer[i - REFRESH_SIZE] = irBuffer[i];
  }
  for (int i = BUFFER_SIZE - REFRESH_SIZE; i < BUFFER_SIZE; i++) readPulseSample(i);

  uint32_t irSum = 0;
  for (int i = BUFFER_SIZE - REFRESH_SIZE; i < BUFFER_SIZE; i++) irSum += irBuffer[i] / REFRESH_SIZE;
  if (irSum < FINGER_IR_THRESHOLD) {
    resetPulseHistory();
    return;
  }

  maxim_heart_rate_and_oxygen_saturation(irBuffer, BUFFER_SIZE, redBuffer, &rawSpO2, &validSpO2, &rawHeartRate, &validHeartRate);

  // Discard physiologically implausible results, then average the last few good ones.
  bool plausible = validHeartRate && validSpO2 &&
                   rawHeartRate >= 30 && rawHeartRate <= 220 &&
                   rawSpO2 >= 70 && rawSpO2 <= 100;
  if (!plausible) return;

  heartHistory[historyIndex] = rawHeartRate;
  spo2History[historyIndex] = rawSpO2;
  historyIndex = (historyIndex + 1) % SMOOTHING;
  if (historyCount < SMOOTHING) historyCount++;

  long heartSum = 0, spo2Sum = 0;
  for (int i = 0; i < historyCount; i++) {
    heartSum += heartHistory[i];
    spo2Sum += spo2History[i];
  }
  heartRate = heartSum / historyCount;
  spo2 = spo2Sum / historyCount;
}

void updateTemperature() {
  if (!temperatureSensorReady) return;
  // Conversion was started last cycle (non-blocking), so the value is ready now.
  float value = tempSensor.getTempCByIndex(0);
  temperatureC = value == DEVICE_DISCONNECTED_C ? NO_TEMPERATURE : value;
  tempSensor.requestTemperatures();
}

void updatePressure() {
  if (!pressureSensorReady) return;
  pressurePa = bmp.readPressure();
  altitudeM = bmp.readAltitude();
}

void printReadings() {
  Serial.println("---- Sensor Readings ----");
  Serial.print("Wearable ID: "); Serial.println(WEARABLE_ID);
  Serial.print("DS18B20 Temperature: ");
  if (temperatureC > NO_TEMPERATURE) { Serial.print(temperatureC); Serial.println(" C"); } else Serial.println("no reading");
  Serial.print("MAX30102 Heart Rate: ");
  if (heartRate > 0) { Serial.print(heartRate); Serial.println(" bpm"); } else Serial.println("no reading (place finger on sensor)");
  Serial.print("MAX30102 SpO2: ");
  if (spo2 > 0) { Serial.print(spo2); Serial.println(" %"); } else Serial.println("no reading");
  if (pressureSensorReady) {
    Serial.print("BMP180 Pressure: "); Serial.print(pressurePa); Serial.println(" Pa");
    Serial.print("BMP180 Altitude: "); Serial.print(altitudeM); Serial.println(" m");
  }
}

void setup() {
  Serial.begin(9600);
  unsigned long serialDeadline = millis() + 3000;
  while (!Serial && millis() < serialDeadline) delay(10);
  Serial.println("CareFlow wearable starting...");

  Wire.begin();
  Wire.setClock(100000);

  tempSensor.begin();
  temperatureSensorReady = tempSensor.getDeviceCount() > 0;
  if (temperatureSensorReady) {
    tempSensor.setResolution(12);
    tempSensor.setWaitForConversion(false);
    tempSensor.requestTemperatures();
  } else {
    Serial.println("DS18B20 not found on D2. Check data wire and 4.7k pull-up.");
  }

  for (int attempt = 1; attempt <= 5 && !pulseSensorReady; attempt++) {
    pulseSensorReady = particleSensor.begin(Wire, I2C_SPEED_STANDARD);
    if (!pulseSensorReady) delay(500);
  }
  if (pulseSensorReady) {
    // 100 Hz averaged over 4 samples = 25 Hz, the rate spo2_algorithm assumes.
    particleSensor.setup(60, 4, 2, 100, 411, 4096);
    for (int i = 0; i < BUFFER_SIZE; i++) readPulseSample(i);
  } else {
    Serial.println("MAX30102 not found. Check SDA/SCL and power. Continuing without it.");
  }

  pressureSensorReady = bmp.begin();
  if (!pressureSensorReady) Serial.println("BMP180 not found. Check SDA/SCL. Continuing without it.");

  ensureWiFi();
  Serial.println("Setup complete.");
}

void loop() {
  if (pulseSensorReady) updatePulse(); else delay(1000);  // ~1 s per cycle either way
  updateTemperature();
  updatePressure();

  if (millis() - lastUpload >= UPLOAD_INTERVAL_MS) {
    lastUpload = millis();
    printReadings();
    uploadReadings();
  }
}
