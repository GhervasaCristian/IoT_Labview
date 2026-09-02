<#
.SYNOPSIS
  Emulates the ESP_OLED_Sensor_PWM.ino MQTT traffic (Home Assistant discovery +
  the JSON state message it publishes every ~2s), without needing the real
  ESP8266/OLED/DHT11/MQ-135 hardware connected.

  Uses the same topics, unique_id's and JSON shape as the firmware, so it's a
  drop-in stand-in for the "ESP OLED Sensor" device in Home Assistant: entities
  created by this script keep working unmodified once the real board is flashed
  and starts publishing to the same topics.

.PARAMETER Broker
  MQTT broker host/IP. Matches MQTT_BROKER in the .ino.

.PARAMETER Password
  MQTT broker password. Left as a placeholder here (not committed with a real
  secret) -- pass -Password, or edit the default below for local-only runs.

.EXAMPLE
  .\emulate-oled-sensor-mqtt.ps1 -Password "your-mosquitto-password"
#>
param(
    [string]$Broker = "192.168.1.137",
    [int]$Port = 1883,
    [string]$Username = "homeassistant",
    [string]$Password = "YOUR_MQTT_PASSWORD",
    [string]$DeviceId = "esp_oled_sensor_01",
    [string]$DeviceName = "ESP OLED Sensor",
    [string]$StateTopic = "esp_oled_sensor/state",
    [double]$IntervalSeconds = 2.0
)

$ErrorActionPreference = "Stop"

if ($Password -eq "YOUR_MQTT_PASSWORD") {
    Write-Host "Set the real broker password first: re-run with -Password '<password>'" -ForegroundColor Yellow
    Write-Host "(the one you used for the 'homeassistant' MQTT user in mosquitto/config/passwd)" -ForegroundColor Yellow
    exit 1
}

function Invoke-MqttPub {
    param([string]$Topic, [string]$Message, [switch]$Retain)

    $dockerArgs = @(
        "run", "--rm", "eclipse-mosquitto", "mosquitto_pub",
        "-h", $Broker, "-p", $Port, "-u", $Username, "-P", $Password,
        "-t", $Topic, "-m", $Message
    )
    if ($Retain) { $dockerArgs += "-r" }

    $env:MSYS_NO_PATHCONV = "1"
    & docker @dockerArgs
}

function Publish-DiscoveryConfig {
    param(
        [string]$Component, [string]$ObjectId, [string]$Name,
        [string]$DeviceClass, [string]$Unit, [string]$ValueTemplate
    )

    $topic = "homeassistant/$Component/$DeviceId/$ObjectId/config"

    $payloadObj = [ordered]@{
        name              = $Name
        unique_id         = "${DeviceId}_$ObjectId"
        state_topic       = $StateTopic
        value_template    = $ValueTemplate
        device            = [ordered]@{
            identifiers  = @($DeviceId)
            name         = $DeviceName
            manufacturer = "DIY"
            model        = "Wemos D1 Mini ESP8266"
        }
    }
    if ($Unit) { $payloadObj["unit_of_measurement"] = $Unit }
    if ($DeviceClass) { $payloadObj["device_class"] = $DeviceClass }

    $json = $payloadObj | ConvertTo-Json -Compress -Depth 4
    Invoke-MqttPub -Topic $topic -Message $json -Retain
}

Write-Host "Publishing MQTT discovery configs (retained) for device '$DeviceId'..." -ForegroundColor Cyan
Publish-DiscoveryConfig -Component "sensor"        -ObjectId "temperature"    -Name "Temperature"    -DeviceClass "temperature" -Unit "°C" -ValueTemplate "{{ value_json.temperature }}"
Publish-DiscoveryConfig -Component "sensor"        -ObjectId "humidity"       -Name "Humidity"        -DeviceClass "humidity"    -Unit "%"  -ValueTemplate "{{ value_json.humidity }}"
Publish-DiscoveryConfig -Component "sensor"        -ObjectId "mq135_voltage"  -Name "MQ-135 Voltage"  -DeviceClass "voltage"     -Unit "V"  -ValueTemplate "{{ value_json.mq_voltage }}"
Publish-DiscoveryConfig -Component "sensor"        -ObjectId "air_quality"    -Name "Air Quality"     -DeviceClass $null         -Unit $null -ValueTemplate "{{ value_json.air_quality }}"
Publish-DiscoveryConfig -Component "binary_sensor" -ObjectId "touch"          -Name "Touch Sensor"    -DeviceClass $null         -Unit $null -ValueTemplate "{{ value_json.touch }}"
Write-Host "Discovery done. Home Assistant should show device '$DeviceName' now." -ForegroundColor Cyan
Write-Host ""
Write-Host "Streaming simulated sensor state to '$StateTopic' every $IntervalSeconds s. Ctrl+C to stop." -ForegroundColor Cyan

# --- Simulated sensor state, wandering like the real DHT11 / MQ-135 readings ---
$temperature = 23.5
$humidity    = 50.0
$mqVoltage   = 0.8
$touchOn     = $false
$rand        = [System.Random]::new()

while ($true) {
    # Slow random walk, clamped to plausible indoor ranges (mirrors TEMP_MIN_C/TEMP_MAX_C
    # and the DHT11/MQ-135 behavior in the firmware).
    $temperature += ($rand.NextDouble() - 0.5) * 0.4
    $temperature  = [Math]::Max(18.0, [Math]::Min(30.0, $temperature))

    $humidity += ($rand.NextDouble() - 0.5) * 1.5
    $humidity  = [Math]::Max(30.0, [Math]::Min(70.0, $humidity))

    $mqVoltage += ($rand.NextDouble() - 0.5) * 0.15
    $mqVoltage  = [Math]::Max(0.2, [Math]::Min(4.0, $mqVoltage))

    # Same thresholds as st25... er, as the firmware's air-quality classification
    if ($mqVoltage -lt 1.5) { $airQuality = "GOOD" }
    elseif ($mqVoltage -lt 3.0) { $airQuality = "MOD" }
    else { $airQuality = "POOR" }

    # Occasional brief "touch" presses
    if ($touchOn) {
        if ($rand.NextDouble() -lt 0.5) { $touchOn = $false }
    } elseif ($rand.NextDouble() -lt 0.05) {
        $touchOn = $true
    }

    $payload = '{{"temperature":{0:N1},"humidity":{1:N1},"mq_voltage":{2:N2},"air_quality":"{3}","touch":"{4}"}}' -f `
        $temperature, $humidity, $mqVoltage, $airQuality, $(if ($touchOn) { "ON" } else { "OFF" })

    Invoke-MqttPub -Topic $StateTopic -Message $payload
    Write-Host ("[{0:T}] {1}" -f (Get-Date), $payload)

    Start-Sleep -Seconds $IntervalSeconds
}
