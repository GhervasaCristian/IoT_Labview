<#
.SYNOPSIS
  Emulates the ESP_OLED_Sensor_PWM.ino ThingsBoard telemetry (the JSON message it
  publishes every ~2s to v1/devices/me/telemetry), without needing the real
  ESP8266/OLED/DHT11/MQ-135 hardware connected.

  Uses the same telemetry keys as the firmware (temperature, humidity, mq_voltage,
  air_quality, touch), so it's a drop-in stand-in for testing the ThingsBoard
  dashboard: values keep flowing to the same device unmodified once the real
  board is flashed and starts publishing itself.

.PARAMETER Token
  ThingsBoard device access token (Devices -> your device -> Copy access token).
  Matches tbToken in the .ino. Required.

.PARAMETER Broker
  ThingsBoard server host/IP. Matches tbServer in the .ino.

.PARAMETER DurationSeconds
  How long to stream for, then stop on its own. 0 = run forever (Ctrl+C to stop).

.EXAMPLE
  .\emulate-oled-sensor-mqtt.ps1 -Token "A1B2C3D4E5F6G7H8I9J0"
.EXAMPLE
  # Publish a changing value every 2s for 10s, then stop (5 publishes)
  .\emulate-oled-sensor-mqtt.ps1 -Token "A1B2C3D4E5F6G7H8I9J0" -DurationSeconds 10
#>
param(
    [Parameter(Mandatory = $true)]
    [string]$Token,
    [string]$Broker = "192.168.100.223",
    [int]$Port = 1883,
    [double]$IntervalSeconds = 2.0,
    [double]$DurationSeconds = 0
)

$ErrorActionPreference = "Stop"

function Invoke-MqttPub {
    param([string]$Topic, [string]$Message)

    $dockerArgs = @(
        "run", "--rm", "eclipse-mosquitto", "mosquitto_pub",
        "-h", $Broker, "-p", $Port, "-u", $Token,
        "-t", $Topic, "-m", $Message
    )

    $env:MSYS_NO_PATHCONV = "1"
    & docker @dockerArgs
}

if ($DurationSeconds -gt 0) {
    $iterations = [Math]::Max(1, [Math]::Floor($DurationSeconds / $IntervalSeconds))
    Write-Host "Streaming simulated telemetry to ThingsBoard at ${Broker}:${Port} every $IntervalSeconds s, for $DurationSeconds s ($iterations publishes)." -ForegroundColor Cyan
} else {
    $iterations = [double]::PositiveInfinity
    Write-Host "Streaming simulated telemetry to ThingsBoard at ${Broker}:${Port} every $IntervalSeconds s. Ctrl+C to stop." -ForegroundColor Cyan
}
Write-Host "(Device identified by its access token -- make sure a device with that token exists in ThingsBoard.)" -ForegroundColor Cyan
Write-Host ""

# --- Simulated sensor state, wandering like the real DHT11 / MQ-135 readings ---
$temperature = 23.5
$humidity    = 50.0
$mqVoltage   = 0.8
$touchOn     = $false
$rand        = [System.Random]::new()

$i = 0
while ($i -lt $iterations) {
    $i++
    # Slow random walk, clamped to plausible indoor ranges (mirrors TEMP_MIN_C/TEMP_MAX_C
    # and the DHT11/MQ-135 behavior in the firmware).
    $temperature += ($rand.NextDouble() - 0.5) * 0.4
    $temperature  = [Math]::Max(18.0, [Math]::Min(30.0, $temperature))

    $humidity += ($rand.NextDouble() - 0.5) * 1.5
    $humidity  = [Math]::Max(30.0, [Math]::Min(70.0, $humidity))

    $mqVoltage += ($rand.NextDouble() - 0.5) * 0.15
    $mqVoltage  = [Math]::Max(0.2, [Math]::Min(4.0, $mqVoltage))

    # Same thresholds as the firmware's air-quality classification
    if ($mqVoltage -lt 1.5) { $airQuality = "GOOD" }
    elseif ($mqVoltage -lt 3.0) { $airQuality = "MOD" }
    else { $airQuality = "POOR" }

    # Occasional brief "touch" presses
    if ($touchOn) {
        if ($rand.NextDouble() -lt 0.5) { $touchOn = $false }
    } elseif ($rand.NextDouble() -lt 0.05) {
        $touchOn = $true
    }

    $payload = '{{"temperature":{0:N1},"humidity":{1:N1},"mq_voltage":{2:N2},"air_quality":"{3}","touch":{4}}}' -f `
        $temperature, $humidity, $mqVoltage, $airQuality, $(if ($touchOn) { "true" } else { "false" })

    Invoke-MqttPub -Topic "v1/devices/me/telemetry" -Message $payload
    Write-Host ("[{0:T}] {1}" -f (Get-Date), $payload)

    if ($i -lt $iterations) {
        Start-Sleep -Seconds $IntervalSeconds
    }
}

if ($DurationSeconds -gt 0) {
    Write-Host "`nDone -- published $i telemetry updates." -ForegroundColor Cyan
}
