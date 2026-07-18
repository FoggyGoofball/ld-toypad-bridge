# deploy-hello-sdk.ps1 — Deploy helloworld.sprx (Sony SDK/scetool build)
# BINARY FTP for .sprx, clean LF-only boot_plugins.txt.
#
# The .sprx goes to /dev_hdd0/plugins/helloworld.sprx.
# The boot_plugins.txt entry matches the actual file path (plugins/).

$PS3_IP = "192.168.0.47"
$SPRX_FILE = Join-Path $PSScriptRoot "build\helloworld.sprx"
$REMOTE_PATH = "/dev_hdd0/plugins/helloworld.sprx"

try {
    $wc = New-Object System.Net.WebClient
    $wc.Credentials = New-Object System.Net.NetworkCredential("mike","mike")

    # Step 1: Upload .sprx to /dev_hdd0/plugins/ (BINARY)
    $uri = "ftp://${PS3_IP}${REMOTE_PATH}"
    Write-Host "Uploading ($([math]::Round((Get-Item $SPRX_FILE).Length/1KB,1)) KB) to ${uri}..."
    $wc.UploadFile($uri, $SPRX_FILE)
    Write-Host "  OK - $((Get-Item $SPRX_FILE).Length) bytes (BINARY)"

    # Step 2: Overwrite boot_plugins.txt with clean LF-only entry
    $bootUri = "ftp://${PS3_IP}/dev_hdd0/boot_plugins.txt"
    $localTemp = [System.IO.Path]::GetTempFileName()
    $line = "/dev_hdd0/plugins/helloworld.sprx"
    $content = $line + "`n"   # LF only, no CR
    [System.IO.File]::WriteAllBytes($localTemp, [System.Text.Encoding]::ASCII.GetBytes($content))
    $wc.UploadFile($bootUri, $localTemp)
    Write-Host "  boot_plugins.txt: 1 entry (LF-only)"
    Write-Host "    $line"
    Remove-Item $localTemp -Force

    Write-Host ""
    Write-Host "Deployment complete! Reboot PS3 now."
    Write-Host "Listen for a single quick buzzer beep within ~10 sec of XMB."
} catch {
    Write-Warning "FTP failed: $($_.Exception.Message)"
    Write-Host "Ensure PS3 FTP server is running at $PS3_IP"
}
