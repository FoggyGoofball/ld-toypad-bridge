# check-boot-log.ps1
$wc = New-Object System.Net.WebClient
$wc.Credentials = New-Object System.Net.NetworkCredential("mike","mike")

Write-Host "=== Checking PS3 boot state ==="
Write-Host ""

# Check boot log
try {
    $content = $wc.DownloadString("ftp://192.168.0.47/dev_hdd0/plugins/ldtoypad_boot.log")
    Write-Host "BOOT LOG FOUND at /dev_hdd0/plugins/ldtoypad_boot.log:"
    Write-Host "----------------------------------------"
    Write-Host $content
    Write-Host "----------------------------------------"
} catch {
    Write-Host "Boot log NOT FOUND at /dev_hdd0/plugins/ - 550"
}

# Check old path
try {
    $content2 = $wc.DownloadString("ftp://192.168.0.47/dev_flash/tmp/ldtoypad_boot.log")
    Write-Host "BOOT LOG FOUND at /dev_flash/tmp/ldtoypad_boot.log (old path):"
    Write-Host "----------------------------------------"
    Write-Host $content2
    Write-Host "----------------------------------------"
} catch {
    Write-Host "Boot log NOT FOUND at /dev_flash/tmp/ - 550"
}

# Check enable token
try {
    $content3 = $wc.DownloadString("ftp://192.168.0.47/dev_hdd0/plugins/ldtoypad.enable")
    Write-Host "ENABLE TOKEN: PRESENT (not consumed)"
} catch {
    Write-Host "ENABLE TOKEN: NOT FOUND (consumed or never created)"
}

# List plugins directory
try {
    $wc2 = New-Object System.Net.WebClient
    $wc2.Credentials = New-Object System.Net.NetworkCredential("mike","mike")
    $listing = $wc2.DownloadString("ftp://192.168.0.47/dev_hdd0/plugins/")
    Write-Host ""
    Write-Host "=== /dev_hdd0/plugins/ directory listing ==="
    Write-Host $listing
} catch {
    Write-Host "Cannot list /dev_hdd0/plugins/ directory"
}

Write-Host ""
Write-Host "=== Done ==="
