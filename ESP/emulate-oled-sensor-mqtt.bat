@echo off
REM Emulates the ThingsBoard telemetry ESP_OLED_Sensor_PWM.ino sends
REM (a JSON telemetry message every ~2s), without the real ESP8266 board.
REM Requires Docker Desktop running (uses the eclipse-mosquitto image's mosquitto_pub).
REM
REM Usage:
REM   emulate-oled-sensor-mqtt.bat YOUR_DEVICE_ACCESS_TOKEN
REM   emulate-oled-sensor-mqtt.bat YOUR_DEVICE_ACCESS_TOKEN 192.168.100.223 1883

if "%~1"=="" (
    echo Usage: %~nx0 ^<thingsboard_device_token^> [broker] [port]
    echo   e.g.  %~nx0 A1B2C3D4E5F6G7H8I9J0
    exit /b 1
)

set TB_TOKEN=%~1
set TB_BROKER=%~2
set TB_PORT=%~3
if "%TB_BROKER%"=="" set TB_BROKER=192.168.100.223
if "%TB_PORT%"=="" set TB_PORT=1883

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0emulate-oled-sensor-mqtt.ps1" -Token "%TB_TOKEN%" -Broker "%TB_BROKER%" -Port %TB_PORT%
