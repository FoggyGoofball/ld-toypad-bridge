# check-ps3-log.ps1 — Retrieve debug log from PS3
param([string]$PS3IP = "192.168.0.47")
try {
    $wc = New-Object System.Net.WebClient
    $wc.Credentials = New-Object System.Net.NetworkCredential("mike","mike")
    $log = $wc.DownloadString("ftp://${PS3IP}/dev_hdd0/plugins/ldtoypad_debug.log")
    Write-Host "=== DEBUG LOG ($($log.Length) chars) ==="
    Write-Host $log
    Write-Host "=== END LOG ==="
} catch {
    Write-Warning "Failed to retrieve debug log: $($_.Exception.Message)"
}
