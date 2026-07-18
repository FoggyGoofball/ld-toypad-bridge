# ftp-deploy.ps1 — Deploy SPRX to PS3 and register in boot_plugins.txt

$PS3_IP = "192.168.0.47"
$SPRX_FILE = Join-Path $PSScriptRoot "sprx-plugin\build\ldtoypad.sprx"
$REMOTE_DIR = "/dev_hdd0/plugins"
$REMOTE_PLUGIN_PATH = "${REMOTE_DIR}/ldtoypad.sprx"

try {
    $wc = New-Object System.Net.WebClient
    $wc.Credentials = New-Object System.Net.NetworkCredential("mike","mike")

    # ---------------------------------------------------------------
    # Step 1: Upload SPRX
    # ---------------------------------------------------------------
    $uri = "ftp://${PS3_IP}${REMOTE_DIR}/ldtoypad.sprx"
    Write-Host "Uploading ldtoypad.sprx to ${uri}..."
    $wc.UploadFile($uri, $SPRX_FILE)
    Write-Host "  OK - ldtoypad.sprx uploaded ($(Get-Item $SPRX_FILE).Length bytes)"

    # ---------------------------------------------------------------
    # Step 2: Register in /dev_hdd0/boot_plugins.txt (Cobra)
    # ---------------------------------------------------------------
    Write-Host ""
    Write-Host "--- boot_plugins.txt registration ---"

    $bootPluginsUri = "ftp://${PS3_IP}/dev_hdd0/boot_plugins.txt"
    $localTemp = [System.IO.Path]::GetTempFileName()
    $needsLine = $true

    try {
        $wc.DownloadFile($bootPluginsUri, $localTemp)
        $content = Get-Content $localTemp -Raw
        Write-Host "Existing boot_plugins.txt found."
        if ($content -match [regex]::Escape($REMOTE_PLUGIN_PATH)) {
            $needsLine = $false
            Write-Host "  ldtoypad.sprx already registered."
        } else {
            Write-Host "  ldtoypad.sprx NOT registered - will append."
        }
    } catch {
        Write-Host "boot_plugins.txt does not exist yet - will create."
        $content = ""
    }

    if ($needsLine) {
        # Safely append to existing plugins — never overwrite boot chain
        $newContent = ($content.TrimEnd("`r`n") + "`n" + $REMOTE_PLUGIN_PATH + "`n")
        $newContent = $newContent -replace "`r", ""
        [System.IO.File]::WriteAllText($localTemp, $newContent, [System.Text.Encoding]::ASCII)
        $wc.UploadFile($bootPluginsUri, $localTemp)
        Write-Host "  ldtoypad.sprx appended to boot_plugins.txt (LF-only)"
    } else {
        # Even if the line exists, CRLF may have snuck in — rewrite with LF-only
        Write-Host "  Re-writing boot_plugins.txt with LF-only line endings..."
        $lines = $content -split '\r?\n' | Where-Object { $_.Trim() -ne '' }
        $newContent = ($lines -join "`n") + "`n"
        # Strip any stray carriage returns that could corrupt Cobra's parser
        $newContent = $newContent -replace "`r", ""
        [System.IO.File]::WriteAllText($localTemp, $newContent, [System.Text.Encoding]::ASCII)
        $wc.UploadFile($bootPluginsUri, $localTemp)
        Write-Host "  boot_plugins.txt normalized to LF-only"
    }

    Remove-Item $localTemp -Force
    # ---------------------------------------------------------------

    Write-Host ""
    Write-Host "Deployment complete!"
    Write-Host ""
    Write-Host "Next: run deploy-enable.ps1 (if not done), then reboot PS3."

} catch {
    Write-Warning "FTP operation failed: $($_.Exception.Message)"
    Write-Host "Make sure the PS3 FTP server is running on $PS3_IP"
    Write-Host "  (Credentials: mike / mike)"
}
