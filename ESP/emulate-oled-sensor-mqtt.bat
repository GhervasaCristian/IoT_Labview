@echo off
REM Emulates the ThingsBoard telemetry ESP_OLED_Sensor_PWM.ino sends
REM (a JSON telemetry message every ~2s), without the real ESP8266 board.
REM Requires Docker Desktop running (uses the eclipse-mosquitto image's mosquitto_pub).
REM
REM Usage (token defaults to the ESP OLED Sensor demo device below -- pass one to override):
REM   emulate-oled-sensor-mqtt.bat
REM   emulate-oled-sensor-mqtt.bat YOUR_DEVICE_ACCESS_TOKEN
REM   emulate-oled-sensor-mqtt.bat YOUR_DEVICE_ACCESS_TOKEN 192.168.100.223 1883
REM   emulate-oled-sensor-mqtt.bat YOUR_DEVICE_ACCESS_TOKEN 192.168.100.223 1883 10
REM     (4th arg = duration in seconds, e.g. 10 = publish every 2s for 10s then stop; omit to run forever)

set TB_TOKEN=%~1
if "%TB_TOKEN%"=="" set TB_TOKEN=FuFDPgDZK2hBapYSq0Ra
set TB_BROKER=%~2
set TB_PORT=%~3
set TB_DURATION=%~4
if "%TB_BROKER%"=="" set TB_BROKER=192.168.100.223
if "%TB_PORT%"=="" set TB_PORT=1883
if "%TB_DURATION%"=="" set TB_DURATION=0

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0emulate-oled-sensor-mqtt.ps1" -Token "%TB_TOKEN%" -Broker "%TB_BROKER%" -Port %TB_PORT% -DurationSeconds %TB_DURATION%
