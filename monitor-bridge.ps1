# monitor-bridge.ps1 — Continuous bridge telemetry monitor
#
# Polls UDP port and HTTP port of the LD-ToyPad Bridge server using
# Test-NetConnection and logs connectivity events to logs/ directory.
# Runs until Ctrl+C.
#
# Usage:
#   .\monitor-bridge.ps1                            # use defaults
#   .\monitor-bridge.ps1 -UdpPort 28472 -HttpPort 8080 -IntervalSec 5

param(
    [int]$UdpPort = 28472,
    [int]$HttpPort = 8080,
    [int]$IntervalSec = 5,
    [string]$LogDir = (Join-Path $PSScriptRoot "logs")
)

# ---------------------------------------------------------------
# Ensure log directory exists
# ---------------------------------------------------------------
if (-not (Test-Path $LogDir)) {
    New-Item -ItemType Directory -Path $LogDir -Force | Out-Null
}

$timestamp     = Get-Date -Format "yyyyMMdd-HHmmss"
$logFile       = Join-Path $LogDir "bridge-monitor-$timestamp.log"
$csvLogFile    = Join-Path $LogDir "bridge-monitor-$timestamp.csv"

# Write CSV header
"Timestamp,UDP_Port,HTTP_Port,UDP_Reachable,HTTP_Reachable,UDP_LatencyMs,HTTP_StatusCode,Notes" | Out-File $csvLogFile -Encoding ASCII

Write-Host "================================================"
Write-Host " LD-ToyPad Bridge Telemetry Monitor"
Write-Host "================================================"
Write-Host " UDP Port:    $UdpPort"
Write-Host " HTTP Port:   $HttpPort"
Write-Host " Interval:    ${IntervalSec}s"
Write-Host " Log File:    $logFile"
Write-Host " CSV Log:     $csvLogFile"
Write-Host " Press Ctrl+C to stop."
Write-Host "================================================"
Write-Host ""

# ---------------------------------------------------------------
# Continuous polling loop
# ---------------------------------------------------------------
while ($true) {
    $now        = Get-Date
    $timestampIso = $now.ToString("yyyy-MM-dd HH:mm:ss")
    $udpOk      = $false
    $httpOk     = $false
    $udpLatency = -1
    $httpStatus = -1
    $notes      = ""

    # ---- UDP port test ----
    try {
        $udpResult = Test-NetConnection -ComputerName 127.0.0.1 -Port $UdpPort -WarningAction SilentlyContinue -InformationLevel Quiet 2>$null
        if ($udpResult -eq $true) {
            $udpOk = $true
            # Test-NetConnection for UDP doesn't give latency; measure approximate
            $sw = [System.Diagnostics.Stopwatch]::StartNew()
            $udpCheck2 = Test-NetConnection -ComputerName 127.0.0.1 -Port $UdpPort -WarningAction SilentlyContinue -InformationLevel Quiet 2>$null
            $sw.Stop()
            $udpLatency = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)
        } else {
            $udpOk = $false
            $notes += "UDP UNREACHABLE; "
        }
    } catch {
        $udpOk = $false
        $notes += "UDP ERROR: $($_.Exception.Message); "
    }

    # ---- HTTP port test ----
    try {
        $httpRequest = [System.Net.WebRequest]::Create("http://127.0.0.1:${HttpPort}/api/status")
        $httpRequest.Timeout = 3000
        $httpResponse = $httpRequest.GetResponse()
        $httpStatus = [int]$httpResponse.StatusCode
        $httpResponse.Close()
        if ($httpStatus -eq 200) {
            $httpOk = $true
        } else {
            $notes += "HTTP returned $httpStatus; "
        }
    } catch {
        $httpOk = $false
        if ($_.Exception.InnerException) {
            $notes += "HTTP ERROR: $($_.Exception.InnerException.Message); "
        } else {
            $notes += "HTTP ERROR: $($_.Exception.Message); "
        }
    }

    # ---- Determine overall status icon ----
    if ($udpOk -and $httpOk) {
        $icon = "[OK]"
    } elseif ($udpOk -or $httpOk) {
        $icon = "[PARTIAL]"
    } else {
        $icon = "[DOWN]"
    }

    # ---- Console output ----
    $lineOut = "$timestampIso $icon UDP=${udpLatency}ms HTTP=${httpStatus}"
    if ($notes -ne "") {
        $lineOut += " $notes"
    }
    Write-Host $lineOut

    # ---- Log file output (append) ----
    $notes.TrimEnd("; ") | Out-File $logFile -Append -Encoding ASCII

    # ---- CSV output (append) ----
    $csvLine = "$timestampIso,$UdpPort,$HttpPort,$udpOk,$httpOk,$udpLatency,$httpStatus,`"$($notes.TrimEnd('; '))`""
    $csvLine | Out-File $csvLogFile -Append -Encoding ASCII

    # ---- Sleep until next poll ----
    Start-Sleep -Seconds $IntervalSec
}
