/*
  AIoT-Based Intelligent Classroom Energy Monitoring
  Arduino Uno - six-sensor integration

  Hardware:
    D2 = DHT11 data                 (Adafruit DHT library)
    D3 = SW-420 vibration DO        (interrupt-driven pulse capture)
    D4 = IR obstacle sensor DO      (debounced event detection)
    A0 = MQ gas module AO           (raw reading, NOT gas ppm)
    A1 = LDR module AO              (raw reading)
    A2 = ACS712 OUT                 (current sensor)

  Serial Monitor: 115200 baud, human-readable report every 2 seconds.
  'DATA,...' CSV line on each report is for a USB serial dashboard (included in package).

  WARNING: ONLY test an ACS712 load using an appropriately protected,
  SAFE LOW-VOLTAGE circuit. Do not connect mains wiring to a breadboard.
  ACS712 must have a real series-connected load for current measurement.
*/

#include <Arduino.h>
#include <DHT.h>
#include <math.h>

const uint8_t DHT_PIN       = 2;
const uint8_t VIBRATION_PIN = 3;
const uint8_t IR_PIN        = 4;
const uint8_t GAS_PIN       = A0;
const uint8_t LDR_PIN       = A1;
const uint8_t ACS_PIN       = A2;

DHT dht(DHT_PIN, DHT11);

// Adjust only after confirming your actual module behavior.
const uint8_t VIB_ACTIVE_LEVEL = LOW;  // Change to HIGH if reversed.
const uint8_t IR_ACTIVE_LEVEL  = LOW;  // Most IR obstacle modules are active LOW.
const bool LDR_HIGH_IS_BRIGHT  = true; // Flip if brighter light makes A1 DECREASE.

// Change these two thresholds according to your own observed readings.
const int LDR_BRIGHT_THRESHOLD = 500;
const int GAS_DEMO_RAW_THRESHOLD = 650; // Demonstration only; NOT a safety threshold.
const unsigned long GAS_WARMUP_MS = 60000UL; // At least a startup indication;
                                            // MQ sensors may require far longer.

// IMPORTANT: Set for the marking of your ACS712 module:
//  5 A: 0.185 V/A       20 A: 0.100 V/A        30 A: 0.066 V/A
const float ACS_SENSITIVITY_V_PER_A = 0.185f; // EXAMPLE: ACS712-05B.
const float UNO_ADC_REFERENCE_V = 5.00f;     // Measure Uno 5V for better accuracy.
const float ACS_MIN_DETECTABLE_A = 0.15f;    // Starting cutoff; verify with a real load.
const float ACS_NOISE_MULTIPLIER = 1.8f;    // Accounts for measured no-load noise.
const uint16_t ACS_ZERO_SAMPLES = 750;
const unsigned long ACS_SAMPLE_INTERVAL_MS = 2UL;

const unsigned long REPORT_INTERVAL_MS = 2000UL;
const unsigned long VIBRATION_HOLD_MS = 4000UL;
const unsigned long IR_PASSED_HOLD_MS = 4000UL;
const unsigned long IR_DEBOUNCE_MS = 50UL;

volatile bool vibrationPulseFlag = false;
unsigned long lastVibrationAt = 0;
bool vibrationSeen = false;
unsigned long vibrationEventCount = 0;

bool irLastRawDetected = false;
bool irStableDetected = false;
unsigned long irRawChangedAt = 0;
unsigned long irLastPassedAt = 0;
bool irPassSeen = false;
unsigned long irEventCount = 0;

float acsZeroAdc = 512.0f;
float acsNoiseAmps = 0.0f;
float acsDetectionThresholdA = ACS_MIN_DETECTABLE_A;
float acsSquaredAdcSum = 0.0f;
uint16_t acsWindowSamples = 0;
unsigned long lastAcsSampleAt = 0;
unsigned long lastReportAt = 0;

void vibrationInterrupt() {
  // D3 has an external interrupt on Uno. Capture short SW-420 pulses.
  if (digitalRead(VIBRATION_PIN) == VIB_ACTIVE_LEVEL) {
    vibrationPulseFlag = true;
  }
}

void calibrateAcsZero() {
  // Keep the ACS712 current terminals completely unloaded during startup.
  float sum = 0.0f;
  float sumSq = 0.0f;
  for (uint16_t i = 0; i < ACS_ZERO_SAMPLES; ++i) {
    int adc = analogRead(ACS_PIN);
    sum += (float)adc;
    sumSq += (float)adc * (float)adc;
    delay(2);
  }
  acsZeroAdc = sum / ACS_ZERO_SAMPLES;
  float variance = sumSq / ACS_ZERO_SAMPLES - acsZeroAdc * acsZeroAdc;
  if (variance < 0.0f) variance = 0.0f;

  const float ampsPerAdcCount = UNO_ADC_REFERENCE_V /
                               (1023.0f * ACS_SENSITIVITY_V_PER_A);
  acsNoiseAmps = sqrt(variance) * ampsPerAdcCount;
  acsDetectionThresholdA = fmaxf(ACS_MIN_DETECTABLE_A,
                                 acsNoiseAmps * ACS_NOISE_MULTIPLIER);

  Serial.print(F("ACS zero ADC: "));
  Serial.println(acsZeroAdc, 2);
  Serial.print(F("ACS no-load RMS noise: "));
  Serial.print(acsNoiseAmps, 3);
  Serial.println(F(" A"));
  Serial.print(F("ACS detect threshold: "));
  Serial.print(acsDetectionThresholdA, 3);
  Serial.println(F(" A (small loads may be below sensor resolution)"));
}

void pollVibration(unsigned long now) {
  noInterrupts();
  bool pulse = vibrationPulseFlag;
  vibrationPulseFlag = false;
  interrupts();
  if (pulse) {
    vibrationSeen = true;
    lastVibrationAt = now;
    ++vibrationEventCount;
  }
}

bool vibrationIsActive(unsigned long now) {
  return vibrationSeen && (now - lastVibrationAt < VIBRATION_HOLD_MS);
}

void pollIR(unsigned long now) {
  bool rawDetected = (digitalRead(IR_PIN) == IR_ACTIVE_LEVEL);
  if (rawDetected != irLastRawDetected) {
    irLastRawDetected = rawDetected;
    irRawChangedAt = now;
  }
  if ((rawDetected != irStableDetected) &&
      (now - irRawChangedAt >= IR_DEBOUNCE_MS)) {
    irStableDetected = rawDetected;
    if (irStableDetected) {
      ++irEventCount; // Object-detection events, NOT student counts.
    } else {
      irPassSeen = true;
      irLastPassedAt = now;
    }
  }
}

const __FlashStringHelper* getIRStatus(unsigned long now) {
  if (irStableDetected) return F("OBJECT DETECTED");
  if (irPassSeen && (now - irLastPassedAt < IR_PASSED_HOLD_MS)) {
    return F("OBJECT PASSED");
  }
  return F("NO OBJECT");
}

const char* getIRCsvStatus(unsigned long now) {
  if (irStableDetected) return "OBJECT_DETECTED";
  if (irPassSeen && (now - irLastPassedAt < IR_PASSED_HOLD_MS)) {
    return "OBJECT_PASSED";
  }
  return "NO_OBJECT";
}

void pollACS(unsigned long now) {
  if (now - lastAcsSampleAt < ACS_SAMPLE_INTERVAL_MS) return;
  lastAcsSampleAt = now;
  float diffAdc = (float)analogRead(ACS_PIN) - acsZeroAdc;
  acsSquaredAdcSum += diffAdc * diffAdc;
  ++acsWindowSamples;
}

// A current above the detection limit is measured, not fabricated.
// Below that limit we intentionally report NO LOAD instead of "0.000 A".
float getWindowCurrentA() {
  if (acsWindowSamples == 0) return NAN;
  const float rmsAdc = sqrt(acsSquaredAdcSum / acsWindowSamples);
  return rmsAdc * UNO_ADC_REFERENCE_V /
         (1023.0f * ACS_SENSITIVITY_V_PER_A);
}

void printDhtField(float value) {
  if (isnan(value)) Serial.print(F("ERR"));
  else Serial.print(value, 1);
}

void setup() {
  Serial.begin(115200);
  pinMode(VIBRATION_PIN, INPUT);
  pinMode(IR_PIN, INPUT);
  pinMode(GAS_PIN, INPUT);
  pinMode(LDR_PIN, INPUT);
  pinMode(ACS_PIN, INPUT);
  dht.begin();

  Serial.println(F("=== AIoT CLASSROOM : ARDUINO UNO ==="));
  Serial.println(F("6 sensors; 2-second reports; Serial CSV for dashboard"));
  Serial.println(F("Keep ACS712 CURRENT PATH UNLOADED during calibration."));
  calibrateAcsZero();

  // Establish a stable initial IR state, rather than counting boot noise.
  irLastRawDetected = (digitalRead(IR_PIN) == IR_ACTIVE_LEVEL);
  irStableDetected = irLastRawDetected;
  irRawChangedAt = millis();
  // Boot-up state is not counted as a new detection event.

  attachInterrupt(digitalPinToInterrupt(VIBRATION_PIN),
                  vibrationInterrupt, CHANGE);
  lastAcsSampleAt = millis();
  lastReportAt = millis();

  Serial.println(F("DATA format: DATA,ms,tempC,humidityPct,gasRaw,ldrRaw,currentA_or_NO_LOAD,loadStatus,vibration,irStatus,irEvents,gasStatus,lightStatus"));
  Serial.println(F("Ready. IR event total is NOT a people count."));
}

void loop() {
  unsigned long now = millis();
  pollVibration(now);
  pollIR(now);
  pollACS(now);

  if (now - lastReportAt < REPORT_INTERVAL_MS) return;
  lastReportAt = now;

  // DHT11 should be read only about once every 2 s.
  float humidity = dht.readHumidity();
  float temperature = dht.readTemperature();
  int gasRaw = analogRead(GAS_PIN);
  int ldrRaw = analogRead(LDR_PIN);
  bool lightBright = (ldrRaw >= LDR_BRIGHT_THRESHOLD);
  if (!LDR_HIGH_IS_BRIGHT) lightBright = !lightBright;

  bool gasWarmup = (now < GAS_WARMUP_MS);
  bool gasAboveDemo = (gasRaw >= GAS_DEMO_RAW_THRESHOLD);
  const char* gasStatus = gasWarmup ? "WARMUP" :
                          (gasAboveDemo ? "ABOVE_DEMO_THRESHOLD" :
                                          "BELOW_DEMO_THRESHOLD");
  const char* lightStatus = lightBright ? "BRIGHT" : "DARK";

  // Capture any extra vibration pulse during slow DHT readings.
  now = millis();
  pollVibration(now);
  pollIR(now);
  bool vibActive = vibrationIsActive(now);
  float currentA = getWindowCurrentA();
  bool acsValid = !isnan(currentA);
  bool loadDetected = acsValid && currentA >= acsDetectionThresholdA;

  Serial.println();
  Serial.println(F("========= CLASSROOM LIVE MONITOR ========="));
  Serial.print(F("Uptime            : "));
  Serial.print(now / 1000UL);
  Serial.println(F(" seconds"));
  Serial.print(F("Temperature       : "));
  printDhtField(temperature);
  Serial.println(F(" C"));
  Serial.print(F("Humidity          : "));
  printDhtField(humidity);
  Serial.println(F(" %"));
  Serial.print(F("MQ Gas (raw ADC)   : "));
  Serial.print(gasRaw);
  Serial.print(F(" / 1023 | "));
  Serial.println(gasStatus);
  Serial.print(F("LDR (raw ADC)     : "));
  Serial.print(ldrRaw);
  Serial.print(F(" / 1023 | "));
  Serial.println(lightStatus);
  Serial.print(F("ACS712 Current    : "));
  if (!acsValid) {
    Serial.println(F("SENSOR ERROR"));
  } else if (loadDetected) {
    Serial.print(currentA, 3);
    Serial.println(F(" A (measured RMS)"));
  } else {
    Serial.println(F("NO LOAD"));
  }
  Serial.print(F("Vibration         : "));
  Serial.print(vibActive ? F("DETECTED") : F("NORMAL"));
  Serial.print(F(" | Events: "));
  Serial.println(vibrationEventCount);
  Serial.print(F("IR Sensor         : "));
  Serial.print(getIRStatus(now));
  Serial.print(F(" | Detection Events: "));
  Serial.println(irEventCount);
  Serial.println(F("=========================================="));

  // One line of machine-readable CSV. The bundled dashboard parses only
  // lines beginning "DATA,"; PC software timestamps them on receipt.
  Serial.print(F("DATA,"));
  Serial.print(now);
  Serial.print(',');
  printDhtField(temperature);
  Serial.print(',');
  printDhtField(humidity);
  Serial.print(',');
  Serial.print(gasRaw);
  Serial.print(',');
  Serial.print(ldrRaw);
  Serial.print(',');
  if (!acsValid) Serial.print(F("ERR"));
  else if (loadDetected) Serial.print(currentA, 3);
  else Serial.print(F("NO_LOAD"));
  Serial.print(',');
  Serial.print(!acsValid ? F("ERROR") : (loadDetected ? F("LOAD") : F("NO_LOAD")));
  Serial.print(',');
  Serial.print(vibActive ? 1 : 0);
  Serial.print(',');
  Serial.print(getIRCsvStatus(now));
  Serial.print(',');
  Serial.print(irEventCount);
  Serial.print(',');
  Serial.print(gasStatus);
  Serial.print(',');
  Serial.println(lightStatus);

  acsSquaredAdcSum = 0.0f;
  acsWindowSamples = 0;
}
