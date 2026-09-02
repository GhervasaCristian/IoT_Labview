/**
 * @file ESP_OLED_Sensor_PWM.ino
 * @brief Wemos D1 Mini (ESP8266) - DHT11 & Touch sensor, SSD1306 OLED display,
 *        MQ-135 air quality sensor, and a 1 kHz PWM analog output for temperature.
 *
 * Hardware Connections:
 * - OLED display (SSD1306 128x64 I2C):
 *   - SDA -> D2 (GPIO4)
 *   - SCL -> D1 (GPIO5)
 * - Touch sensor (Digital Out):
 *   - OUT -> D7 (GPIO13)
 * - DHT11 Temperature/Humidity Sensor:
 *   - DATA -> D5 (GPIO14) [D5 supports internal pull-up]
 * - Analog Temperature Output (via RC Low-Pass Filter):
 *   - D6 (GPIO12) used as PWM output at 1 kHz
 *   - RC Filter: 2 kΩ resistor in series, 10 µF capacitor to GND
 *     -> Cut-off freq fc ≈ 7.95 Hz, so 1 kHz PWM carrier is smoothed to a clean DC level.
 * - MQ-135 Gas Sensor:
 *   - Analog Out -> A0 (ADC0)
 *
 * PWM Architecture:
 * - The PWM output has its OWN independent clock via the Ticker library.
 *   The Ticker fires every PWM_APPLY_INTERVAL_MS (10 ms) to call analogWrite().
 *   This is completely decoupled from sensor reads, OLED updates, and the main loop.
 * - The 1 kHz carrier (hardware timer) runs continuously in background.
 *   The Ticker only updates the duty cycle value at 100 Hz.
 *
 * PWM Calibration Curve (Temperature -> Duty Cycle -> Voltage after RC filter):
 * - Linear mapping across the full DHT11 measurement range:
 *     0°C  ->   0% duty cycle -> 0.00 V (DC after filter)
 *    25°C  ->  50% duty cycle -> 1.65 V
 *    50°C  -> 100% duty cycle -> 3.30 V
 * - Formula: duty = (T_celsius / 50.0) * 1023
 * - Inverse (LabVIEW side): T_celsius = V_measured * (50.0 / 3.3)
 *   e.g. 1.65 V -> 25.0°C
 *
 * Required Libraries:
 * - Adafruit SSD1306 (by Adafruit)
 * - Adafruit GFX Library (by Adafruit)
 * - DHT sensor library (by Adafruit)
 * - Adafruit Unified Sensor (dependency for DHT)
 * - Ticker (built-in for ESP8266 Arduino core)
 */

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <Ticker.h>

// --- Configuration & Pin Definitions ---
#define SCREEN_WIDTH 128     // OLED display width, in pixels
#define SCREEN_HEIGHT 64     // OLED display height, in pixels
#define OLED_RESET    -1     // Reset pin # (or -1 if sharing Arduino reset pin)
#define SCREEN_ADDRESS 0x3C  // Common SSD1306 I2C address (0x3C or 0x3D)

// I2C Pin Definitions for ESP8266 Wemos D1 Mini
#define I2C_SDA_PIN 4        // D2 (GPIO4)
#define I2C_SCL_PIN 5        // D1 (GPIO5)

// DHT11 Pin and Type
#define DHTPIN 14            // D5 (GPIO14)
#define DHTTYPE DHT11

// Touch Sensor Pin
#define TOUCH_PIN 13         // D7 (GPIO13)

// Analog Output Pin (PWM)
#define PWM_OUT_PIN 12       // D6 (GPIO12)

// PWM Constants
// analogWriteFreq sets the 1 kHz hardware carrier — runs independently via timer.
// The Ticker updates the duty cycle register at 100 Hz (every 10 ms).
#define PWM_FREQUENCY         1000   // 1 kHz PWM carrier frequency
#define PWM_MAX_RANGE         1023   // 10-bit resolution (0 = 0%, 1023 = 100%)
#define PWM_APPLY_INTERVAL_MS 10     // Ticker interval: 10 ms -> 100 Hz duty update rate

// Calibration: DHT11 full measurement range mapped linearly to 0-100% duty cycle
#define TEMP_MIN_C  0.0f             // 0°C  ->   0% duty ->  0.00 V after RC filter
#define TEMP_MAX_C  50.0f            // 50°C -> 100% duty ->  3.30 V after RC filter
// LabVIEW inverse: T_celsius = V_measured * (50.0 / 3.3)

// Timing Constants
const unsigned long DHT_UPDATE_INTERVAL = 2000; // Read DHT11 every 2 seconds
const unsigned long MQ_UPDATE_INTERVAL = 500;    // Read MQ-135 every 500ms

// =============================================================
// PWM Task — Independent Ticker
// =============================================================
// targetPWMValue is written by the main loop sensor logic and read by the
// Ticker callback. 'volatile' prevents the compiler caching it across the
// ISR/main-loop boundary.
volatile int targetPWMValue = 0;

Ticker pwmTicker;

/**
 * @brief PWM Ticker callback — fires every PWM_APPLY_INTERVAL_MS (10 ms).
 *
 * Runs from the Ticker timer context, completely independent of loop().
 * Applies the latest duty cycle so the 1 kHz square wave on D6 is always
 * present, even when the main loop is blocked by I2C, DHT bit-banging, etc.
 */
void pwmApplyDuty() {
  analogWrite(PWM_OUT_PIN, targetPWMValue);
}


// --- Object Initializations ---
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
DHT dht(DHTPIN, DHTTYPE);

// --- Global Variables for State Tracking ---
unsigned long lastDHTReadTime = 0;
unsigned long lastMQReadTime = 0;

float currentHumidity = 0.0;
float currentTemperature = 0.0;
bool dhtReadingValid = false;

float mqVoltage = 0.0;
String aqState = "GOOD";
bool lastTouchedState = false;

// Flag to request an OLED update only when values change
bool displayNeedsUpdate = true;

void setup() {
  // Initialize Serial Monitor for debugging
  Serial.begin(115200);
  delay(100);
  Serial.println(F("\n--- System Starting Up ---"));

  // Configure touch sensor pin as input
  pinMode(TOUCH_PIN, INPUT);

  // Configure PWM output pin (No pinMode call to avoid overriding software PWM setup)
  analogWriteFreq(PWM_FREQUENCY);  // Set 1 kHz carrier — hardware timer runs this continuously
  analogWriteRange(PWM_MAX_RANGE); // 10-bit duty resolution (0-1023)
  analogWrite(PWM_OUT_PIN, 0);     // Start at 0% duty -> 0 V

  // Launch the independent PWM Ticker (fires every 10 ms = 100 Hz).
  // From this point, pwmApplyDuty() owns all analogWrite() calls for D6.
  // The main loop ONLY updates targetPWMValue — never calls analogWrite() on D6.
  pwmTicker.attach_ms(PWM_APPLY_INTERVAL_MS, pwmApplyDuty);
  Serial.println(F("PWM Ticker started: 1 kHz carrier, duty updated at 100 Hz"));

  // Initialize custom I2C pins for Wemos D1 Mini
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(400000); // Set I2C speed to 400kHz (Fast Mode) to minimize blocking time

  // Initialize OLED display
  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println(F("OLED SSD1306 allocation failed!"));
    for (;;);
  }
  Serial.println(F("OLED SSD1306 initialized successfully!"));

  // Initialize DHT sensor
  dht.begin();

  // Initial display setup
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(F("System Initializing..."));
  display.display();
  delay(1000);
}

void loop() {
  unsigned long currentMillis = millis();

  // 1. Read Touch Sensor continuously (very fast digital read)
  int touchState = digitalRead(TOUCH_PIN);
  bool isTouched = (touchState == HIGH);
  
  // Trigger display update immediately if the touch state changes
  if (isTouched != lastTouchedState) {
    lastTouchedState = isTouched;
    displayNeedsUpdate = true;
  }

  // 2. Read DHT11 Sensor every 2 seconds
  if (currentMillis - lastDHTReadTime >= DHT_UPDATE_INTERVAL || lastDHTReadTime == 0) {
    lastDHTReadTime = currentMillis;

    float hum = dht.readHumidity();
    float temp = dht.readTemperature();

    if (isnan(hum) || isnan(temp)) {
      // Read failed: mark invalid for display, but HOLD the current duty cycle.
      // Do NOT call analogWrite(0) here — that caused the "0V dropout" glitch.
      // The Ticker keeps the last valid duty active until the next good reading.
      if (dhtReadingValid) {
        dhtReadingValid = false;
        displayNeedsUpdate = true;
      }
      Serial.println(F("Warning: DHT11 read failed — holding last PWM duty"));
    } else {
      if (!dhtReadingValid || currentTemperature != temp || currentHumidity != hum) {
        currentHumidity = hum;
        currentTemperature = temp;
        dhtReadingValid = true;
        displayNeedsUpdate = true;
      }

      // Update duty cycle target — the Ticker applies it within the next 10 ms.
      // NEVER call analogWrite() here; the Ticker owns D6 from setup() onward.
      float constrainedTemp = constrain(currentTemperature, TEMP_MIN_C, TEMP_MAX_C);
      targetPWMValue = (int)((constrainedTemp / TEMP_MAX_C) * (float)PWM_MAX_RANGE);
    }
  }

  // 3. Read MQ-135 Sensor from A0 every 500ms
  if (currentMillis - lastMQReadTime >= MQ_UPDATE_INTERVAL || lastMQReadTime == 0) {
    lastMQReadTime = currentMillis;

    int rawADC = analogRead(A0);
    float newVoltage = rawADC * (5.0f / 1023.0f);

    // Classify air quality state
    String newAqState;
    if (newVoltage < 1.5f) {
      newAqState = F("GOOD");
    } else if (newVoltage < 3.0f) {
      newAqState = F("MOD");
    } else {
      newAqState = F("POOR");
    }

    // Trigger update if voltage or state changed significantly
    if (abs(newVoltage - mqVoltage) > 0.05f || newAqState != aqState) {
      mqVoltage = newVoltage;
      aqState = newAqState;
      displayNeedsUpdate = true;
    }
  }

  // 4. Update the OLED Display ONLY when a change occurs
  if (displayNeedsUpdate) {
    displayNeedsUpdate = false;

    display.clearDisplay();
    display.setTextSize(1, 2); // Tight horizontal, stretched vertical

    // --- Left Column: Temp, Hum, Touch ---
    display.setCursor(0, 4);
    if (dhtReadingValid) {
      display.print(F("T: "));
      display.print(currentTemperature, 1);
      display.write(247); // Degree symbol
      display.println(F("C"));
    } else {
      display.println(F("T: ERR"));
    }

    display.setCursor(0, 24);
    if (dhtReadingValid) {
      display.print(F("H: "));
      display.print(currentHumidity, 1);
      display.println(F("%"));
    } else {
      display.println(F("H: ERR"));
    }

    display.setCursor(0, 44);
    display.print(F("Touch:"));
    if (isTouched) {
      display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
      display.print(F(" ON "));
      display.setTextColor(SSD1306_WHITE);
    } else {
      display.print(F(" OFF"));
    }

    // --- Right Column: MQ-135 Voltage & Air Quality ---
    display.setCursor(70, 4);
    display.print(F("MQ:"));
    display.print(mqVoltage, 2);
    display.println(F("V"));

    display.setCursor(70, 24);
    display.print(F("AQ:"));
    if (aqState == "POOR") {
      display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
      display.print(aqState);
      display.setTextColor(SSD1306_WHITE);
    } else {
      display.print(aqState);
    }

    display.display();
  }

  // 5. Debug logging to Serial Monitor every 2 seconds
  static unsigned long lastLogTime = 0;
  if (currentMillis - lastLogTime >= 2000) {
    lastLogTime = currentMillis;
    if (dhtReadingValid) {
      float pwmVoltage = (targetPWMValue / (float)PWM_MAX_RANGE) * 3.3f;
      Serial.print(F("Temp: "));     Serial.print(currentTemperature, 1); Serial.print(F(" C | "));
      Serial.print(F("Hum: "));      Serial.print(currentHumidity, 1);    Serial.print(F("% | "));
      Serial.print(F("PWM duty: ")); Serial.print(targetPWMValue);
      Serial.print(F("/1023 -> "));  Serial.print(pwmVoltage, 2);         Serial.print(F(" V | "));
    } else {
      Serial.print(F("DHT: ERR | "));
    }
    Serial.print(F("MQ-135: ")); Serial.print(mqVoltage, 2); Serial.print(F("V ("));
    Serial.print(aqState); Serial.print(F(") | "));
    Serial.print(F("Touch: ")); Serial.println(isTouched ? F("ON") : F("OFF"));
  }
}
