# fix-boot-plugins.ps1
$PS3_IP = "192.168.0.47"
$CRED = New-Object System.Net.NetworkCredential("mike","mike")
$bootUri = "ftp://${PS3_IP}/dev_hdd0/boot_plugins.txt"
$pluginDir = "/dev_hdd0/plugins"

Write-Host "=== Step 1: Verify local ldtoypad.sprx ==="
$localSprx = Join-Path $PSScriptRoot "sprx-plugin\build\ldtoypad.sprx"
if (!(Test-Path $localSprx)) { Write-Warning "MISSING: $localSprx"; exit 1 }
$localInfo = Get-Item $localSprx
Write-Host "  $($localInfo.Length) bytes"

Write-Host ""
Write-Host "=== Step 2: Download boot_plugins.txt ==="
$wc = New-Object System.Net.WebClient
$wc.Credentials = $CRED
try { $raw = $wc.DownloadString($bootUri); Write-Host "  Got $($raw.Length) chars" }
catch { Write-Host "  Not found or FTP error: $($_.Exception.Message)"; $raw = "" }

$lines = $raw -split "`n" | ForEach-Object { $_.Trim() }
Write-Host "  Lines: $($lines.Count)"
$i = 0; foreach ($l in $lines) { Write-Host "  [$i] '$l'"; $i++ }

Write-Host ""
Write-Host "=== Step 3: Deduplicate ==="
$seen = @{}; $clean = @()
foreach ($l in $lines) {
    if ($l -eq "") { continue }
    if (!$seen.ContainsKey($l)) { $seen[$l] = $true; $clean += $l }
    else { Write-Host "  REMOVED DUPLICATE: '$l'" }
}

$ldEntry = "/dev_hdd0/plugins/ldtoypad.sprx"
if ($seen.ContainsKey($ldEntry)) { Write-Host "  ldtoypad.sprx already registered" }
else { Write-Host "  ADDING ldtoypad.sprx"; $clean += $ldEntry }

Write-Host ""
Write-Host "=== Step 4: Upload clean boot_plugins.txt ==="
$content = ($clean -join "`n") + "`n"
$localBoot = [System.IO.Path]::GetTempFileName()
try {
    [System.IO.File]::WriteAllBytes($localBoot, [System.Text.Encoding]::ASCII.GetBytes($content))
    $wc.UploadFile($bootUri, $localBoot)
    Write-Host "  Uploaded ($($content.Length) chars, LF-only)"
    Write-Host "  Entries:"; foreach ($e in $clean) { Write-Host "    $e" }
} finally { if (Test-Path $localBoot) { Remove-Item $localBoot -Force } }

Write-Host ""
Write-Host "=== Step 5: Verify read-back ==="
try { $verify = $wc.DownloadString($bootUri); Write-Host "  Verified: $($verify.Length) chars" }
catch { Write-Warning "  Verify failed: $($_.Exception.Message)" }

Write-Host ""
Write-Host "=== Step 6: Upload ldtoypad.sprx ==="
$pluginUri = "ftp://${PS3_IP}${pluginDir}/ldtoypad.sprx"
try {
    $wc.UploadFile($pluginUri, $localSprx)
    Write-Host "  Uploaded ($($localInfo.Length) bytes)"
} catch {
    Write-Warning "  Upload failed: $($_.Exception.Message)"
    Start-Sleep -Seconds 2
    try { $wc.UploadFile($pluginUri, $localSprx); Write-Host "  Retry SUCCEEDED" }
    catch { Write-Warning "  Retry failed: $($_.Exception.Message)" }
}

Write-Host ""
Write-Host "=== Step 7: Verify remote size ==="
try {
    $rs = $wc.OpenRead("ftp://${PS3_IP}${pluginDir}/ldtoypad.sprx")
    $remoteLen = $rs.Length; $rs.Close()
    Write-Host "  Remote: $remoteLen bytes"
    if ($localInfo.Length -eq $remoteLen) { Write-Host "  SIZE MATCH OK" }
    else { Write-Host "  SIZE MISMATCH! Local=$($localInfo.Length) Remote=$remoteLen" }
} catch { Write-Warning "  Cannot verify: $($_.Exception.Message)" }

Write-Host ""
Write-Host "=== COMPLETE ==="
Write-Host "Reboot PS3 and check /dev_hdd0/plugins/ldtoypad_boot.log"
