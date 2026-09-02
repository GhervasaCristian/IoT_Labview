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
 * - PubSubClient (by Nick O'Leary) -- MQTT client for Home Assistant
 * - ESP8266WiFi (built-in for ESP8266 Arduino core)
 *
 * Home Assistant / MQTT:
 * - Publishes readings as one JSON state message on MQTT_STATE_TOPIC.
 * - Publishes retained MQTT Discovery config messages on first connect, so
 *   Home Assistant's "MQTT" integration auto-creates the entities below
 *   (Settings -> Devices & Services -> MQTT -> the device appears automatically,
 *   no manual entity/YAML setup needed).
 */

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <Ticker.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>

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

// --- WiFi & MQTT / Home Assistant Configuration ---
const char* WIFI_SSID     = "YOUR_WIFI_SSID";     // TODO: set your WiFi network name
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD"; // TODO: set your WiFi password

const char* MQTT_BROKER    = "192.168.1.137"; // Mosquitto broker (runs alongside the HA docker container on this PC)
const uint16_t MQTT_PORT   = 1883;
const char* MQTT_USER      = "homeassistant";
const char* MQTT_PASSWORD  = "YOUR_MQTT_PASSWORD"; // TODO: set the broker password (see setup notes, not committed to git)
const char* MQTT_CLIENT_ID = "esp_oled_sensor_01";

const char* DEVICE_ID    = "esp_oled_sensor_01";
const char* DEVICE_NAME  = "ESP OLED Sensor";
const char* MQTT_STATE_TOPIC = "esp_oled_sensor/state";

const unsigned long WIFI_RETRY_INTERVAL_MS = 8000;
const unsigned long MQTT_RETRY_INTERVAL_MS = 5000;
const unsigned long MQTT_PUBLISH_INTERVAL_MS = 2000; // Publish state to HA every 2s

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
WiFiClient espClient;
PubSubClient mqttClient(espClient);

// --- Global Variables for State Tracking ---
unsigned long lastDHTReadTime = 0;
unsigned long lastMQReadTime = 0;
unsigned long lastWifiAttemptMs = 0;
unsigned long lastMqttAttemptMs = 0;
unsigned long lastMqttPublishMs = 0;
bool mqttDiscoveryPublished = false;

float currentHumidity = 0.0;
float currentTemperature = 0.0;
bool dhtReadingValid = false;

float mqVoltage = 0.0;
String aqState = "GOOD";
bool lastTouchedState = false;

// Flag to request an OLED update only when values change
bool displayNeedsUpdate = true;

// =============================================================
// WiFi & MQTT / Home Assistant Discovery
// =============================================================

/**
 * @brief Non-blocking WiFi connect/reconnect with backoff.
 */
void maintainWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  unsigned long now = millis();
  if (lastWifiAttemptMs != 0 && (now - lastWifiAttemptMs < WIFI_RETRY_INTERVAL_MS)) return;
  lastWifiAttemptMs = now;

  Serial.printf("[WiFi] Connecting to SSID: %s...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

/**
 * @brief Publishes one retained MQTT Discovery config message so Home
 * Assistant's MQTT integration auto-creates the entity. All entities share
 * MQTT_STATE_TOPIC and pick their value out of the shared JSON payload.
 */
void publishDiscoveryConfig(const char* component, const char* objectId, const char* name,
                             const char* deviceClass, const char* unit, const char* valueTemplate) {
  char configTopic[128];
  snprintf(configTopic, sizeof(configTopic), "homeassistant/%s/%s/%s/config", component, DEVICE_ID, objectId);

  char payload[512];
  int n = snprintf(payload, sizeof(payload),
    "{\"name\":\"%s\",\"unique_id\":\"%s_%s\",\"state_topic\":\"%s\",\"value_template\":\"%s\"",
    name, DEVICE_ID, objectId, MQTT_STATE_TOPIC, valueTemplate);

  if (unit != nullptr) {
    n += snprintf(payload + n, sizeof(payload) - n, ",\"unit_of_measurement\":\"%s\"", unit);
  }
  if (deviceClass != nullptr) {
    n += snprintf(payload + n, sizeof(payload) - n, ",\"device_class\":\"%s\"", deviceClass);
  }
  snprintf(payload + n, sizeof(payload) - n,
    ",\"device\":{\"identifiers\":[\"%s\"],\"name\":\"%s\",\"manufacturer\":\"DIY\",\"model\":\"Wemos D1 Mini ESP8266\"}}",
    DEVICE_ID, DEVICE_NAME);

  mqttClient.publish(configTopic, payload, true); // retained
}

void publishAllDiscoveryConfigs() {
  publishDiscoveryConfig("sensor", "temperature", "Temperature", "temperature", "°C", "{{ value_json.temperature }}");
  publishDiscoveryConfig("sensor", "humidity", "Humidity", "humidity", "%", "{{ value_json.humidity }}");
  publishDiscoveryConfig("sensor", "mq135_voltage", "MQ-135 Voltage", "voltage", "V", "{{ value_json.mq_voltage }}");
  publishDiscoveryConfig("sensor", "air_quality", "Air Quality", nullptr, nullptr, "{{ value_json.air_quality }}");
  publishDiscoveryConfig("binary_sensor", "touch", "Touch Sensor", nullptr, nullptr, "{{ value_json.touch }}");
  Serial.println(F("[MQTT] Discovery configs published to Home Assistant."));
}

/**
 * @brief Non-blocking MQTT connect/reconnect with backoff; re-publishes
 * discovery configs once after every fresh connection (retained, so this is
 * cheap and keeps HA in sync if the broker/device state was ever cleared).
 */
void maintainMQTT() {
  if (WiFi.status() != WL_CONNECTED) return;

  if (!mqttClient.connected()) {
    unsigned long now = millis();
    if (lastMqttAttemptMs != 0 && (now - lastMqttAttemptMs < MQTT_RETRY_INTERVAL_MS)) return;
    lastMqttAttemptMs = now;

    Serial.print(F("[MQTT] Connecting to broker..."));
    if (mqttClient.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD)) {
      Serial.println(F("connected"));
      mqttDiscoveryPublished = false;
    } else {
      Serial.printf("failed, rc=%d\n", mqttClient.state());
      return;
    }
  }

  mqttClient.loop();

  if (!mqttDiscoveryPublished) {
    publishAllDiscoveryConfigs();
    mqttDiscoveryPublished = true;
  }
}

/**
 * @brief Publishes the current sensor readings as one JSON state message.
 */
void publishMqttState() {
  if (!mqttClient.connected()) return;

  char payload[256];
  snprintf(payload, sizeof(payload),
    "{\"temperature\":%.1f,\"humidity\":%.1f,\"mq_voltage\":%.2f,\"air_quality\":\"%s\",\"touch\":\"%s\"}",
    currentTemperature, currentHumidity, mqVoltage, aqState.c_str(), lastTouchedState ? "ON" : "OFF");

  mqttClient.publish(MQTT_STATE_TOPIC, payload);
}

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

  // Kick off WiFi + MQTT (non-blocking from here on; loop() maintains both)
  maintainWiFi();
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setBufferSize(512); // discovery config payloads exceed the 256-byte default

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

  // 0. Keep WiFi & MQTT alive (both are non-blocking with their own backoff timers)
  maintainWiFi();
  maintainMQTT();

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

  // 5. Publish state to Home Assistant over MQTT every MQTT_PUBLISH_INTERVAL_MS
  if (currentMillis - lastMqttPublishMs >= MQTT_PUBLISH_INTERVAL_MS) {
    lastMqttPublishMs = currentMillis;
    publishMqttState();
  }

  // 6. Debug logging to Serial Monitor every 2 seconds
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
