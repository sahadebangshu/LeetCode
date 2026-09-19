/*
  ESP32 DevKitV1 + MAX30102
  Osteotrack OA Prototype
  --------------------------------------------

  Sensor:
    MAX30102

  Firebase outputs:
    hpfDegradation
    hypervascularization
    presymptomaticIndex
    deviceStatus
    timestamp

  IMPORTANT:
    MAX30102 is being used as a prototype/proxy sensor.
    These values are experimental and are NOT a clinical OA diagnosis.
*/

#include <WiFi.h>
#include <Wire.h>
#include "MAX30105.h"
#include "heartRate.h"

#include <Firebase_ESP_Client.h>

// =====================================================
// Wi-Fi
// =====================================================

#define WIFI_SSID "YOUR_WIFI_NAME"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// =====================================================
// Firebase
// =====================================================

// Use the Web API Key from:
// Firebase Console
// Project Settings -> General -> Your apps

#define API_KEY "YOUR_FIREBASE_WEB_API_KEY"

// Your Firebase Realtime Database URL
#define DATABASE_URL "https://oa-early-detection-5c20b-default-rtdb.europe-west1.firebasedatabase.app/"

// =====================================================
// Firebase objects
// =====================================================

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// =====================================================
// MAX30102
// =====================================================

MAX30105 particleSensor;

// =====================================================
// Heart-rate detection
// =====================================================

const byte RATE_SIZE = 4;

byte rates[RATE_SIZE];
byte rateSpot = 0;

long lastBeat = 0;

float beatsPerMinute = 0;
int beatAvg = 0;

// =====================================================
// Windowed waveform analysis
// =====================================================

const int WINDOW_SIZE = 50;

long irWindow[WINDOW_SIZE];

int windowPos = 0;

bool windowFull = false;

// =====================================================
// Prototype metrics
// =====================================================

float vasodilationIndex = 0;
float PERF_INDEX_SCALE = 5.0;

float hpfDegradationIndex = 0;

// =====================================================
// Combined index
// =====================================================

float WEIGHT_HPF = 0.5;
float WEIGHT_VASO = 0.5;

float preSymptomaticIndex = 0;

// =====================================================
// Contact detection
// =====================================================

const long CONTACT_THRESHOLD = 50000;

bool contactDetected = false;

// =====================================================
// Firebase upload timing
// =====================================================

unsigned long lastFirebaseUpdate = 0;

const unsigned long FIREBASE_INTERVAL = 2000;

// =====================================================
// Setup
// =====================================================

void setup() {

  Serial.begin(115200);

  delay(200);

  Serial.println();
  Serial.println("======================================");
  Serial.println("Osteotrack ESP32 Prototype");
  Serial.println("ESP32 + MAX30102 + Firebase");
  Serial.println("======================================");

  // ---------------------------------------------------
  // I2C
  // ---------------------------------------------------

  Wire.begin(21, 22);

  // ---------------------------------------------------
  // MAX30102
  // ---------------------------------------------------

  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {

    Serial.println("MAX30102 not found.");

    Serial.println("Check:");
    Serial.println("SDA -> GPIO21");
    Serial.println("SCL -> GPIO22");
    Serial.println("VIN -> 3.3V");
    Serial.println("GND -> GND");

    while (1) {
      delay(10);
    }
  }

  Serial.println("MAX30102 found.");

  // ---------------------------------------------------
  // MAX30102 configuration
  // ---------------------------------------------------

  byte ledBrightness = 0x1F;
  byte sampleAverage = 4;
  byte ledMode = 2;

  int sampleRate = 100;
  int pulseWidth = 411;
  int adcRange = 4096;

  particleSensor.setup(
    ledBrightness,
    sampleAverage,
    ledMode,
    sampleRate,
    pulseWidth,
    adcRange
  );

  // ---------------------------------------------------
  // Wi-Fi
  // ---------------------------------------------------

  Serial.println();
  Serial.print("Connecting to Wi-Fi");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {

    delay(500);

    Serial.print(".");
  }

  Serial.println();

  Serial.println("Wi-Fi connected.");

  Serial.print("ESP32 IP: ");
  Serial.println(WiFi.localIP());

  // ---------------------------------------------------
  // Firebase configuration
  // ---------------------------------------------------

  config.api_key = API_KEY;

  config.database_url = DATABASE_URL;

  // ---------------------------------------------------
  // Firebase anonymous authentication
  // ---------------------------------------------------

  if (Firebase.signUp(
        &config,
        &auth,
        "",
        ""
      )) {

    Serial.println("Firebase authentication successful.");

  } else {

    Serial.print("Firebase authentication failed: ");
    Serial.println(config.signer.signupError.message.c_str());
  }

  Firebase.begin(&config, &auth);

  Firebase.reconnectWiFi(true);

  Serial.println("Firebase initialized.");

  Serial.println();
  Serial.println("Place MAX30102 on skin with gentle contact.");
}

// =====================================================
// Calculate prototype metrics
// =====================================================

void updateWindowMetrics(long irValue) {

  irWindow[windowPos] = irValue;

  windowPos++;

  if (windowPos >= WINDOW_SIZE) {

    windowPos = 0;

    windowFull = true;

    long irMin = irWindow[0];
    long irMax = irWindow[0];

    long irSum = 0;

    int minIdx = 0;
    int maxIdx = 0;

    // -------------------------------------------------
    // Find minimum, maximum and average
    // -------------------------------------------------

    for (int i = 0; i < WINDOW_SIZE; i++) {

      irSum += irWindow[i];

      if (irWindow[i] < irMin) {

        irMin = irWindow[i];

        minIdx = i;
      }

      if (irWindow[i] > irMax) {

        irMax = irWindow[i];

        maxIdx = i;
      }
    }

    float irDC = irSum / (float)WINDOW_SIZE;

    float irAC = irMax - irMin;

    // -------------------------------------------------
    // Contact detection
    // -------------------------------------------------

    contactDetected = (irDC >= CONTACT_THRESHOLD);

    // -------------------------------------------------
    // Calculate metrics only with contact
    // -------------------------------------------------

    if (contactDetected) {

      // -----------------------------------------------
      // Hypervascularization proxy
      // -----------------------------------------------

      float perfusionIndexPct =
        (irAC / irDC) * 100.0;

      vasodilationIndex =
        constrain(
          perfusionIndexPct * PERF_INDEX_SCALE,
          0,
          100
        );

      // -----------------------------------------------
      // HPF Degradation proxy
      // -----------------------------------------------

      int sampleGap = maxIdx - minIdx;

      if (sampleGap < 0) {
        sampleGap += WINDOW_SIZE;
      }

      float riseTimeFraction =
        sampleGap / (float)WINDOW_SIZE;

      hpfDegradationIndex =
        constrain(
          (1.0 - riseTimeFraction) * 100.0,
          0,
          100
        );

      // -----------------------------------------------
      // Combined pre-symptomatic index
      // -----------------------------------------------

      preSymptomaticIndex =
        constrain(
          WEIGHT_HPF * hpfDegradationIndex +
          WEIGHT_VASO * vasodilationIndex,
          0,
          100
        );
    }
  }
}

// =====================================================
// Send data to Firebase
// =====================================================

void sendDataToFirebase() {

  if (WiFi.status() != WL_CONNECTED) {

    Serial.println("Wi-Fi disconnected.");

    return;
  }

  if (!Firebase.ready()) {

    Serial.println("Firebase not ready.");

    return;
  }

  // ---------------------------------------------------
  // Firebase path
  // ---------------------------------------------------

  String path = "/sensorData";

  // ---------------------------------------------------
  // Create JSON object
  // ---------------------------------------------------

  FirebaseJson json;

  if (windowFull && contactDetected) {

    // HPF Degradation
    json.set(
      "hpfDegradation",
      hpfDegradationIndex
    );

    // Hypervascularization
    json.set(
      "hypervascularization",
      vasodilationIndex
    );

    // Pre-symptomatic Index
    json.set(
      "presymptomaticIndex",
      preSymptomaticIndex
    );

    // Device status
    json.set(
      "deviceStatus",
      "ONLINE"
    );

  } else {

    // No valid sensor contact
    json.set(
      "hpfDegradation",
      0
    );

    json.set(
      "hypervascularization",
      0
    );

    json.set(
      "presymptomaticIndex",
      0
    );

    json.set(
      "deviceStatus",
      "NO CONTACT"
    );
  }

  // ---------------------------------------------------
  // Timestamp
  // ---------------------------------------------------

  json.set(
    "timestamp",
    millis()
  );

  // ---------------------------------------------------
  // Reference BPM
  // ---------------------------------------------------

  json.set(
    "referenceBPM",
    beatAvg
  );

  // ---------------------------------------------------
  // Send to Firebase
  // ---------------------------------------------------

  if (Firebase.RTDB.setJSON(
        &fbdo,
        path.c_str(),
        &json
      )) {

    Serial.println("Firebase update successful.");

  } else {

    Serial.print("Firebase error: ");
    Serial.println(fbdo.errorReason());
  }
}

// =====================================================
// Main loop
// =====================================================

void loop() {

  // ---------------------------------------------------
  // Read MAX30102
  // ---------------------------------------------------

  long irValue = particleSensor.getIR();

  // ---------------------------------------------------
  // Heart-rate reference calculation
  // ---------------------------------------------------

  if (checkForBeat(irValue)) {

    long delta = millis() - lastBeat;

    lastBeat = millis();

    beatsPerMinute =
      60.0 / (delta / 1000.0);

    if (
      beatsPerMinute > 20 &&
      beatsPerMinute < 255
    ) {

      rates[rateSpot++] =
        (byte)beatsPerMinute;

      rateSpot %= RATE_SIZE;

      beatAvg = 0;

      for (byte x = 0; x < RATE_SIZE; x++) {

        beatAvg += rates[x];
      }

      beatAvg /= RATE_SIZE;
    }
  }

  // ---------------------------------------------------
  // Update metrics
  // ---------------------------------------------------

  updateWindowMetrics(irValue);

  // ---------------------------------------------------
  // Serial Monitor
  // ---------------------------------------------------

  Serial.print("IR=");
  Serial.print(irValue);

  Serial.print("  %HPFDeg=");
  Serial.print(
    windowFull ? hpfDegradationIndex : 0
  );

  Serial.print("  %Hypervascularization=");
  Serial.print(
    windowFull ? vasodilationIndex : 0
  );

  Serial.print("  RefBPM=");
  Serial.print(beatAvg);

  Serial.print("  |  OA Pre-symptomatic Index: ");

  if (
    windowFull &&
    contactDetected
  ) {

    Serial.print(
      preSymptomaticIndex,
      1
    );

    Serial.println(
      "% (experimental proxy)"
    );

  } else {

    Serial.println(
      "-- (No contact detected)"
    );
  }

  // ---------------------------------------------------
  // Firebase update every 2 seconds
  // ---------------------------------------------------

  if (
    millis() - lastFirebaseUpdate >=
    FIREBASE_INTERVAL
  ) {

    lastFirebaseUpdate = millis();

    sendDataToFirebase();
  }

  // ---------------------------------------------------
  // Sampling delay
  // ---------------------------------------------------

  delay(20);
}
