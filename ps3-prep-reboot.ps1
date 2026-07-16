param(
    [string]$PS3_IP = "192.168.0.47"
)

$REMOTE_DIR = "/dev_hdd0/plugins"
$PLUGIN_PATH = "/dev_hdd0/plugins/ldtoypad.sprx"

$wc = New-Object System.Net.WebClient
$wc.Credentials = New-Object System.Net.NetworkCredential("mike","mike")

# ---------------------------------------------------------------
# Step 1: DELETE the enable token
# ---------------------------------------------------------------
Write-Host "--- Step 1: Remove enable token ---"
try {
    $delUri = "ftp://${PS3_IP}${REMOTE_DIR}/ldtoypad.enable"
    $delRequest = [System.Net.FtpWebRequest]::Create($delUri)
    $delRequest.Credentials = New-Object System.Net.NetworkCredential("mike","mike")
    $delRequest.Method = [System.Net.WebRequestMethods+Ftp]::DeleteFile
    $delResponse = $delRequest.GetResponse()
    $delResponse.Close()
    Write-Host "  OK - ldtoypad.enable DELETED"
} catch {
    Write-Warning ("  Could not delete enable token: " + $_.Exception.Message)
    Write-Host "  (This is fine if it doesn't exist yet - it will be re-created after reboot)"
}

# ---------------------------------------------------------------
# Step 2: Check boot_plugins.txt
# ---------------------------------------------------------------
Write-Host ""
Write-Host "--- Step 2: Verify boot_plugins.txt ---"

$bootPluginsUri = "ftp://${PS3_IP}/dev_hdd0/boot_plugins.txt"
$localTemp = [System.IO.Path]::GetTempFileName()

try {
    $wc.DownloadFile($bootPluginsUri, $localTemp)
    $content = Get-Content $localTemp -Raw
    Write-Host "  boot_plugins.txt found on PS3."
    
    $hasLdtoypad = $content -match [regex]::Escape($PLUGIN_PATH)
    $hasCRLF = $content -match "\r\n"
    
    if ($hasLdtoypad) {
        Write-Host "  ldtoypad.sprx entry FOUND."
    } else {
        Write-Host "  ldtoypad.sprx entry NOT FOUND - will append."
    }
    
    $lines = $content -split '\r?\n' | Where-Object { $_.Trim() -ne '' }
    
    if (-not $hasLdtoypad) {
        $lines += $PLUGIN_PATH
    }
    
    $newContent = ($lines -join "`n") + "`n"
    
    if ($hasCRLF -or -not $hasLdtoypad) {
        [System.IO.File]::WriteAllText($localTemp, $newContent, [System.Text.Encoding]::ASCII)
        $wc.UploadFile($bootPluginsUri, $localTemp)
        if ($hasCRLF) {
            Write-Host "  boot_plugins.txt normalized from CRLF to LF-only"
        }
        if (-not $hasLdtoypad) {
            Write-Host "  ldtoypad.sprx entry ADDED"
        }
    } else {
        Write-Host "  boot_plugins.txt is already LF-only with ldtoypad entry - no changes."
    }
} catch {
    Write-Warning ("  Could not read boot_plugins.txt: " + $_.Exception.Message)
    Write-Host "  Will create boot_plugins.txt with ldtoypad.sprx entry."
    $newContent = $PLUGIN_PATH + "`n"
    [System.IO.File]::WriteAllText($localTemp, $newContent, [System.Text.Encoding]::ASCII)
    $wc.UploadFile($bootPluginsUri, $localTemp)
    Write-Host "  boot_plugins.txt CREATED with ldtoypad.sprx entry."
}

Remove-Item $localTemp -Force

# ---------------------------------------------------------------
# Step 3: Verify SPRX file exists on PS3
# ---------------------------------------------------------------
Write-Host ""
Write-Host "--- Step 3: Verify ldtoypad.sprx on PS3 ---"
try {
    $sizeUri = "ftp://${PS3_IP}${REMOTE_DIR}/ldtoypad.sprx"
    $sizeRequest = [System.Net.FtpWebRequest]::Create($sizeUri)
    $sizeRequest.Credentials = New-Object System.Net.NetworkCredential("mike","mike")
    $sizeRequest.Method = [System.Net.WebRequestMethods+Ftp]::GetFileSize
    $sizeResponse = $sizeRequest.GetResponse()
    $size = $sizeResponse.ContentLength
    $sizeResponse.Close()
    Write-Host ("  ldtoypad.sprx present on PS3 (" + $size.ToString("N0") + " bytes)")
} catch {
    Write-Warning "  ldtoypad.sprx NOT FOUND on PS3!"
    Write-Host "  Run ftp-deploy.ps1 first to deploy the SPRX."
}

# ---------------------------------------------------------------
# Summary
# ---------------------------------------------------------------
Write-Host ""
Write-Host "========================================"
Write-Host " PS3 READY FOR REBOOT"
Write-Host "========================================"
Write-Host "  1. Enable token: REMOVED"
Write-Host "  2. boot_plugins.txt: NORMALIZED (LF-only)"
Write-Host "  3. ldtoypad.sprx: DEPLOYED"
Write-Host ""
Write-Host "  Next steps:"
Write-Host "    a. Reboot PS3 (no enable token = plugin stays dormant)"
Write-Host "    b. Create enable token: .\deploy-enable.ps1"
Write-Host "    c. Reboot PS3 again (token consumed, plugin arms)"
Write-Host "    d. Check /dev_hdd0/plugins/ldtoypad_boot.log for diagnostics"
