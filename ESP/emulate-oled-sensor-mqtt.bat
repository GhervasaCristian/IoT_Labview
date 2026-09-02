@echo off
REM Emulates the MQTT traffic ESP_OLED_Sensor_PWM.ino sends to Home Assistant
REM (discovery + a JSON sensor state every ~2s), without the real ESP8266 board.
REM Requires Docker Desktop running (uses the eclipse-mosquitto image's mosquitto_pub).
REM
REM Usage:
REM   emulate-oled-sensor-mqtt.bat YOUR_MQTT_PASSWORD
REM   emulate-oled-sensor-mqtt.bat YOUR_MQTT_PASSWORD 192.168.1.137 1883 homeassistant

if "%~1"=="" (
    echo Usage: %~nx0 ^<mqtt_password^> [broker] [port] [username]
    echo   e.g.  %~nx0 mySecretPassword
    exit /b 1
)

set MQTT_PASSWORD=%~1
set MQTT_BROKER=%~2
set MQTT_PORT=%~3
set MQTT_USERNAME=%~4
if "%MQTT_BROKER%"=="" set MQTT_BROKER=192.168.1.137
if "%MQTT_PORT%"=="" set MQTT_PORT=1883
if "%MQTT_USERNAME%"=="" set MQTT_USERNAME=homeassistant

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0emulate-oled-sensor-mqtt.ps1" -Broker "%MQTT_BROKER%" -Port %MQTT_PORT% -Username "%MQTT_USERNAME%" -Password "%MQTT_PASSWORD%"
