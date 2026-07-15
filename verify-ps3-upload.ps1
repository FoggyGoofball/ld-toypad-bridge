# verify-ps3-upload.ps1 — Verify SPRX on PS3

$PS3_IP = "192.168.0.47"
$REMOTE_DIR = "/dev_hdd0/plugins"

try {
    $wc = New-Object System.Net.WebClient
    $wc.Credentials = New-Object System.Net.NetworkCredential("mike","mike")

    # List remote directory
    $uri = "ftp://${PS3_IP}${REMOTE_DIR}/"
    $list = [Text.Encoding]::ASCII.GetString($wc.DownloadData($uri))
    Write-Host "Files on PS3 ${REMOTE_DIR}:"
    Write-Host $list

    # Verify our files exist
    if ($list.Contains("ldtoypad.sprx")) {
        Write-Host ""
        Write-Host "✅ ldtoypad.sprx is present on PS3"
    } else {
        Write-Host "❌ ldtoypad.sprx NOT found on PS3"
    }
} catch {
    Write-Warning "FTP list failed: $($_.Exception.Message)"
}
