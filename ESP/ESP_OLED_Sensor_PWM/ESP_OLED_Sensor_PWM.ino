/**
 * @file ESP_OLED_Sensor_PWM.ino
 * @brief Wemos D1 Mini (ESP8266) project reading DHT11 & Touch sensor,
 *        updating an I2C OLED display, and outputting humidity as a filtered analog voltage.
 * 
 * Hardware Connections:
 * - OLED display (SSD1306 128x64 I2C):
 *   - SDA -> D2 (GPIO4)
 *   - SCL -> D1 (GPIO5)
 * - Touch sensor (Digital Out):
 *   - OUT -> TX (GPIO1) [Note: Avoid using Serial to prevent hardware conflict]
 * - DHT11 Temperature/Humidity Sensor:
 *   - DATA -> D0 (GPIO16)
 * - Analog Humidity Output (via RC Low-Pass Filter):
 *   - D6 (GPIO12) used as PWM output
 *   - Filter: 2 kΩ resistor in series, 10 µF capacitor to GND
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
// Note: We moved this from D0 (GPIO16) to D5 (GPIO14) because D0 does not have 
// standard pull-up capabilities, which often causes DHT11 read failures.
#define DHTPIN 14            // D5 (GPIO14)
#define DHTTYPE DHT11

// Touch Sensor Pin
// If you move the Touch sensor to D7 (GPIO13):
#define TOUCH_PIN 13       // D7 (GPIO13)

// Analog Output Pin (PWM)
#define PWM_OUT_PIN 12       // D6 (GPIO12)

// PWM Constants
#define PWM_FREQUENCY 1000   // 1 kHz (well above the ~8Hz cutoff of the 2k + 10uF filter)
#define PWM_MAX_RANGE 1023   // 10-bit PWM range (0-1023)

// Timing Constants
const unsigned long DHT_UPDATE_INTERVAL = 2000; // Read DHT11 every 2 seconds

// --- Object Initializations ---
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
DHT dht(DHTPIN, DHTTYPE);

// --- Global Variables ---
unsigned long lastDHTReadTime = 0;
float currentHumidity = 0.0;
float currentTemperature = 0.0;
bool dhtReadingValid = false;
int currentPWMValue = 0; // Tracks the current 10-bit PWM output value (0-1023)

void setup() {
  // Initialize Serial Monitor for debugging (now safe since touch pin moved)
  Serial.begin(115200);
  delay(100);
  Serial.println(F("\n--- System Starting Up ---"));

  // Configure touch sensor pin as input
  pinMode(TOUCH_PIN, INPUT);

  // Configure PWM output pin
  // Note: We do NOT call pinMode(PWM_OUT_PIN, OUTPUT) here. On ESP8266, pinMode
  // can override the timer setup and cause analogWrite to behave as a digital high/low.
  analogWriteFreq(PWM_FREQUENCY); // Set frequency suitable for RC low-pass filter
  analogWriteRange(PWM_MAX_RANGE); // Configure the range (0-1023)
  analogWrite(PWM_OUT_PIN, 0);    // Start with 0V output

  // Initialize custom I2C pins for Wemos D1 Mini
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

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

  // 2. Read DHT11 Sensor every 2 seconds
  if (currentMillis - lastDHTReadTime >= DHT_UPDATE_INTERVAL || lastDHTReadTime == 0) {
    lastDHTReadTime = currentMillis;

    float hum = dht.readHumidity();
    float temp = dht.readTemperature();

    // Check if readings are valid (NaN check)
    if (isnan(hum) || isnan(temp)) {
      dhtReadingValid = false;
      Serial.println(F("Error: Failed to read from DHT11 sensor!"));
      analogWrite(PWM_OUT_PIN, 0); // Force 0V output on error
    } else {
      currentHumidity = hum;
      currentTemperature = temp;
      dhtReadingValid = true;

      // 3. Generate PWM signal proportional to Temperature
      // Linear mapping: 20.0 C -> 1.5V, 35.0 C -> 3.3V
      // Constrain temperature between 20.0 and 35.0 to stay within bounds
      float constrainedTemp = constrain(currentTemperature, 20.0f, 35.0f);
      
      // Calculate target voltage linearly: 1.5V + (temp - 20) * (change in voltage / change in temp)
      float targetVoltage = 1.5f + (constrainedTemp - 20.0f) * ((3.3f - 1.5f) / (35.0f - 20.0f));
      
      // Convert voltage (0.0V to 3.3V) to 10-bit PWM value (0 to 1023)
      currentPWMValue = (int)((targetVoltage / 3.3f) * PWM_MAX_RANGE);
      analogWrite(PWM_OUT_PIN, currentPWMValue);
    }
  }

  // 3. Read MQ-135 Sensor and Update Display every 200ms (prevents CPU starvation for PWM)
  static unsigned long lastUpdate200ms = 0;
  static float mqVoltage = 0.0f;
  static String aqState = "GOOD";

  if (currentMillis - lastUpdate200ms >= 200) {
    lastUpdate200ms = currentMillis;

    // ESP8266 ADC0 is 10-bit (0-1023). 
    // Wemos D1 Mini has an onboard resistor divider mapping 0-3.2V input to 0-1V at the chip.
    // We map the raw ADC value to the 0-5V sensor scale assuming voltage scaling is applied.
    int rawADC = analogRead(A0);
    mqVoltage = rawADC * (5.0f / 1023.0f);

    // Classify air quality state
    if (mqVoltage < 1.5f) {
      aqState = F("GOOD");
    } else if (mqVoltage < 3.0f) {
      aqState = F("MOD");
    } else {
      aqState = F("POOR");
    }

    // Update the OLED Display
    display.clearDisplay();

    // Set independent scaling: width scale 1 (tight), height scale 2 (stretched)
    display.setTextSize(1, 2);

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
      display.setTextColor(SSD1306_BLACK, SSD1306_WHITE); // Highlight ON state
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
      display.setTextColor(SSD1306_BLACK, SSD1306_WHITE); // Highlight POOR state
      display.print(aqState);
      display.setTextColor(SSD1306_WHITE);
    } else {
      display.print(aqState);
    }

    display.display();
  }

  // 4. Debug logging to Serial Monitor every 2 seconds
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
