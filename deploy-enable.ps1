# deploy-enable.ps1 — Create enable flag on PS3 (zero-byte marker file)

$PS3_IP = "192.168.0.47"
$REMOTE_DIR = "/dev_hdd0/plugins"

try {
    # WebClient's UploadFile creates/overwrites the target file
    # We upload an empty file to serve as the marker
    $tempFile = [System.IO.Path]::GetTempFileName()
    [System.IO.File]::WriteAllText($tempFile, "enabled")

    $wc = New-Object System.Net.WebClient
    $wc.Credentials = New-Object System.Net.NetworkCredential("mike","mike")

    $uri = "ftp://${PS3_IP}${REMOTE_DIR}/ldtoypad.enable"
    Write-Host "Creating enable flag at ${uri}..."
    $wc.UploadFile($uri, $tempFile)
    Write-Host "✅ Enable flag created successfully"

    Remove-Item $tempFile -Force
} catch {
    Write-Warning "Failed to create enable flag: $($_.Exception.Message)"
    Write-Host ""
    Write-Host "To create manually using FTP client:"
    Write-Host "  1. Connect to ftp://192.168.0.47 (mike/mike)"
    Write-Host "  2. Create empty file at /dev_hdd0/plugins/ldtoypad.enable"
}
