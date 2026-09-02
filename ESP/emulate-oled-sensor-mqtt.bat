<# : batch script
@echo off
echo Starting ESP OLED Sensor Emulation...
powershell -NoProfile -ExecutionPolicy Bypass -Command "Invoke-Expression (Get-Content '%~f0' -Raw)"
pause
goto :eof
#>

# ============================================================================
# Emulates the ThingsBoard telemetry ESP_OLED_Sensor_PWM.ino sends
# (temperature, humidity, mq_voltage, air_quality, touch), without the real
# ESP8266/OLED/DHT11/MQ-135 hardware connected. Same values, same telemetry
# keys as tb.sendTelemetryData() in the firmware.
#
# Modeled on MedicalResearch/LabTest/MQTT Simulation/simulate_docker_mqtt.bat:
# a single polyglot .bat/PowerShell file (self-invokes via Invoke-Expression),
# streams JSON via stdin into mosquitto_pub (-s) instead of a -m argument, and
# targets host.docker.internal so it doesn't need to know a LAN IP for a
# ThingsBoard instance running on this same PC.
#
# Requires Docker Desktop running.
# ============================================================================

# ============================================================================
# CONFIGURATION
# ============================================================================
$token           = "FuFDPgDZK2hBapYSq0Ra"     # ThingsBoard device access token (demo device)
$topic           = "v1/devices/me/telemetry"
$image           = "thingsboard/mosquitto-clients"
$intervalMs      = 2000  # Publish every 2s, matching TB_PUBLISH_INTERVAL_MS in the .ino
$durationSeconds = 0     # 0 = run forever (Ctrl+C to stop); set e.g. 10 for a 10s burst
$jsonOutputFile  = "sent_data.json"

Write-Host "Target Topic: $topic"
Write-Host "Device Token: $token"
Write-Host "JSON Log File: $jsonOutputFile"
Write-Host "----------------------------------------------------"

# --- Simulated sensor state, wandering like the real DHT11 / MQ-135 readings ---
$temperature = 23.5
$humidity    = 50.0
$mqVoltage   = 0.8
$touchOn     = $false

$sentDataList = [System.Collections.Generic.List[Object]]::new()
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$count = 0

while ($durationSeconds -le 0 -or $sw.Elapsed.TotalSeconds -lt $durationSeconds) {
    # Slow random walk, clamped to plausible indoor ranges (mirrors TEMP_MIN_C/TEMP_MAX_C
    # and the DHT11/MQ-135 behavior in the firmware).
    $temperature += (Get-Random -Minimum -0.2 -Maximum 0.2)
    $temperature  = [Math]::Max(18.0, [Math]::Min(30.0, $temperature))

    $humidity += (Get-Random -Minimum -0.75 -Maximum 0.75)
    $humidity  = [Math]::Max(30.0, [Math]::Min(70.0, $humidity))

    $mqVoltage += (Get-Random -Minimum -0.075 -Maximum 0.075)
    $mqVoltage  = [Math]::Max(0.2, [Math]::Min(4.0, $mqVoltage))

    # Same thresholds as the firmware's air-quality classification
    if ($mqVoltage -lt 1.5) { $airQuality = "GOOD" }
    elseif ($mqVoltage -lt 3.0) { $airQuality = "MOD" }
    else { $airQuality = "POOR" }

    # Occasional brief "touch" presses
    if ($touchOn) {
        if ((Get-Random -Minimum 0.0 -Maximum 1.0) -lt 0.5) { $touchOn = $false }
    } elseif ((Get-Random -Minimum 0.0 -Maximum 1.0) -lt 0.05) {
        $touchOn = $true
    }

    $payloadObj = [ordered]@{
        temperature = [math]::Round($temperature, 1)
        humidity    = [math]::Round($humidity, 1)
        mq_voltage  = [math]::Round($mqVoltage, 2)
        air_quality = $airQuality
        touch       = $touchOn
    }
    $sentDataList.Add($payloadObj)

    $jsonPayload = $payloadObj | ConvertTo-Json -Compress

    # Stream JSON via STDIN directly into mosquitto_pub (avoids Windows arg-quoting
    # issues with -m "...", and avoids needing a LAN IP for the broker).
    $jsonPayload | docker run -i --rm --add-host=host.docker.internal:host-gateway $image mosquitto_pub -q 1 -h host.docker.internal -p 1883 -t $topic -u $token -s

    $count++
    Write-Host "[t=$([Math]::Round($sw.Elapsed.TotalSeconds, 1))s] $jsonPayload"

    Start-Sleep -Milliseconds $intervalMs
}

$sentDataList | ConvertTo-Json -Depth 5 | Set-Content -Path $jsonOutputFile -Encoding UTF8

Write-Host "----------------------------------------------------"
Write-Host "Emulation complete! Published $count telemetry messages."
Write-Host "Transmitted JSON history saved to: $jsonOutputFile"
