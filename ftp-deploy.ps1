# ftp-deploy.ps1 — Deploy SPRX to PS3

$PS3_IP = "192.168.0.47"
$SPRX_FILE = "c:\Users\Admin\source\repos\dimensions plugin\sprx-plugin\build\ldtoypad.sprx"
$FILESELF_FILE = "c:\Users\Admin\source\repos\dimensions plugin\sprx-plugin\build\ldtoypad.fake.self"
$REMOTE_DIR = "/dev_hdd0/plugins"

try {
    $wc = New-Object System.Net.WebClient
    $wc.Credentials = New-Object System.Net.NetworkCredential("mike","mike")

    # Upload SPRX
    $uri = "ftp://${PS3_IP}${REMOTE_DIR}/ldtoypad.sprx"
    Write-Host "Uploading ldtoypad.sprx to ${uri}..."
    $wc.UploadFile($uri, $SPRX_FILE)
    Write-Host "  OK - ldtoypad.sprx uploaded ($(Get-Item $SPRX_FILE).Length bytes)"

    # Upload fake.self (for reference)
    $uri2 = "ftp://${PS3_IP}${REMOTE_DIR}/ldtoypad.fake.self"
    Write-Host "Uploading ldtoypad.fake.self to ${uri2}..."
    $wc.UploadFile($uri2, $FILESELF_FILE)
    Write-Host "  OK - ldtoypad.fake.self uploaded"

    Write-Host ""
    Write-Host "Deployment complete!"
} catch {
    Write-Warning "FTP upload failed: $($_.Exception.Message)"
    Write-Host "Make sure the PS3 FTP server is running on $PS3_IP"
}
