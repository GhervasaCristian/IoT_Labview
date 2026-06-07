/**
 * @file ESP_OLED_Sensor_PWM.ino
 * @brief Wemos D1 Mini (ESP8266) project reading DHT11 & Touch sensor,
 *        updating an I2C OLED display, and outputting temperature as a filtered analog voltage.
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
 *   - D6 (GPIO12) used as PWM output
 *   - Filter: 2 kΩ resistor in series, 10 µF capacitor to GND
 * - MQ-135 Gas Sensor:
 *   - Analog Out -> A0 (ADC0)
 * 
 * Required Libraries:
 * - Adafruit SSD1306 (by Adafruit)
 * - Adafruit GFX Library (by Adafruit)
 * - DHT sensor library (by Adafruit)
 * - Adafruit Unified Sensor (dependency for DHT)
 */

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>

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
#define PWM_FREQUENCY 1000   // 1 kHz (well above the ~8Hz cutoff of the 2k + 10uF filter)
#define PWM_MAX_RANGE 1023   // 10-bit PWM range (0-1023)

// Timing Constants
const unsigned long DHT_UPDATE_INTERVAL = 2000; // Read DHT11 every 2 seconds
const unsigned long MQ_UPDATE_INTERVAL = 500;    // Read MQ-135 every 500ms

// --- Object Initializations ---
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
DHT dht(DHTPIN, DHTTYPE);

// --- Global Variables for State Tracking ---
unsigned long lastDHTReadTime = 0;
unsigned long lastMQReadTime = 0;

float currentHumidity = 0.0;
float currentTemperature = 0.0;
bool dhtReadingValid = false;
int currentPWMValue = 0;

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
  analogWriteFreq(PWM_FREQUENCY);  // Set frequency suitable for RC low-pass filter
  analogWriteRange(PWM_MAX_RANGE); // Configure range (0-1023)
  analogWrite(PWM_OUT_PIN, 0);     // Start with 0V output

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

    // Check if readings are valid
    if (isnan(hum) || isnan(temp)) {
      if (dhtReadingValid) { // Only trigger update if state changed
        dhtReadingValid = false;
        displayNeedsUpdate = true;
      }
      Serial.println(F("Error: Failed to read from DHT11 sensor!"));
      currentPWMValue = 0;
      analogWrite(PWM_OUT_PIN, 0); // Force 0V output on error
    } else {
      // Trigger display update if temperature or humidity changes
      if (!dhtReadingValid || currentTemperature != temp || currentHumidity != hum) {
        currentHumidity = hum;
        currentTemperature = temp;
        dhtReadingValid = true;
        displayNeedsUpdate = true;
      }

      // 3. Generate PWM signal proportional to Temperature
      // Linear mapping: 20.0 C -> 1.5V, 35.0 C -> 3.3V
      float constrainedTemp = constrain(currentTemperature, 20.0f, 35.0f);
      float targetVoltage = 1.5f + (constrainedTemp - 20.0f) * ((3.3f - 1.5f) / (35.0f - 20.0f));
      
      currentPWMValue = (int)((targetVoltage / 3.3f) * PWM_MAX_RANGE);
      analogWrite(PWM_OUT_PIN, currentPWMValue);
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
      Serial.print(F("Hum: ")); Serial.print(currentHumidity); Serial.print(F("% | "));
      Serial.print(F("Temp: ")); Serial.print(currentTemperature); Serial.print(F(" C | "));
      Serial.print(F("PWM: ")); Serial.print(currentPWMValue); Serial.print(F(" ("));
      Serial.print((currentPWMValue / 1023.0f) * 3.3f, 2); Serial.print(F("V) | "));
    }
    Serial.print(F("MQ-135: ")); Serial.print(mqVoltage, 2); Serial.print(F("V ("));
    Serial.print(aqState); Serial.print(F(") | "));
    Serial.print(F("Touch: ")); Serial.println(isTouched ? F("ON") : F("OFF"));
  }
}
