# IoT & LabVIEW Integration Project

This repository contains a complete IoT-enabled environment monitoring system that integrates a **Wemos D1 Mini (ESP8266)** microcontroller with **National Instruments LabVIEW** via an **NI USB-6009 DAQ** card. 

The system reads ambient temperature, humidity, air quality (gas concentration), and touch input, displays them locally on an OLED screen, and transmits the temperature as a filtered analog voltage to the LabVIEW interface for real-time monitoring and processing.

---

## 📂 Repository Structure

*   📁 **`ESP/`**: Microcontroller firmware written in C++/Arduino.
    *   `ESP_OLED_Sensor_PWM.ino`: Core ESP8266 firmware handling sensor acquisition, local OLED rendering, and analog output generation.
*   📁 **`6009/`**: NI DAQmx & LabVIEW files.
    *   `Model.vi`: LabVIEW Virtual Instrument (VI) designed to interface with the NI USB-6009 to read and visualize data.
    *   `6009 Pinout.url`: Reference link for NI USB-6009 device terminal pinouts.
*   📁 **`Schematics/`**: Altium Designer project files for hardware implementation.
    *   `DAQmx.SchDoc`: Hardware schematic document.
    *   `IoT_Labview.PcbDoc`: PCB layout design.
    *   `Karaoke.PrjPcb` & structure: Altium project configuration.
    *   `Schematics.pdf`: PDF export of the hardware schematics.
*   📁 **`Documentation/`**: Project reports and presentation files.
    *   `Air Quality Analyser.pdf`: Detailed project manual and analysis report.
    *   `IoT Labview.pptx`: Technical presentation for the project.

---

## 🔌 Hardware Connections & Pins

### Wemos D1 Mini (ESP8266) Wiring Table

| Component | Pin on Component | Pin on Wemos D1 Mini | GPIO / Channel | Notes |
| :--- | :--- | :--- | :--- | :--- |
| **SSD1306 OLED** | SDA | **D2** | GPIO4 | I2C Data (Fast-mode 400kHz) |
| **SSD1306 OLED** | SCL | **D1** | GPIO5 | I2C Clock |
| **DHT11 Sensor** | DATA | **D5** | GPIO14 | DHT11 Signal |
| **Touch Sensor** | OUT | **D7** | GPIO13 | Digital touch status input |
| **MQ-135 Sensor**| AOUT (Analog) | **A0** | ADC0 | Air quality raw voltage output |
| **PWM Analog Out**| PWM Signal | **D6** | GPIO12 | Filtered through Low-Pass RC |

### 📈 Temperature PWM-to-Analog Filter (D6 ➔ NI USB-6009)
To read the temperature in LabVIEW, the ESP8266 D6 pin outputs a 1 kHz PWM signal which is converted to a smooth DC analog voltage using a **Passive RC Low-Pass Filter**:
*   **Resistor ($R$):** $2\text{ k}\Omega$
*   **Capacitor ($C$):** $10\ \mu\text{F}$
*   **Cut-off frequency ($f_c$):** $\approx 7.95\text{ Hz}$ (smooths the 1 kHz PWM carrier frequency perfectly)
*   **Linear Mapping:** $20.0^\circ\text{C}$ to $35.0^\circ\text{C}$ is mapped linearly to a $1.5\text{ V}$ to $3.3\text{ V}$ DC voltage range. This analog voltage is connected to the analog input channel of the **NI USB-6009 DAQ** to be read by `Model.vi`.

---

## 🛠️ Software & Setup

### Arduino IDE Prerequisites
To compile the ESP8266 firmware, ensure you have the ESP8266 board package installed in your Arduino IDE along with the following libraries:
1. **Adafruit SSD1306**
2. **Adafruit GFX Library**
3. **DHT Sensor Library**
4. **Adafruit Unified Sensor**

### LabVIEW Setup
1. Connect the filtered analog output from the ESP8266 pin D6 (post-RC filter) to an Analog Input channel on the **NI USB-6009** DAQmx hardware.
2. Open `6009/Model.vi` in NI LabVIEW (ensure NI-DAQmx drivers are installed).
3. Configure the DAQmx physical channel in LabVIEW to match the input channel connected to the filter.
