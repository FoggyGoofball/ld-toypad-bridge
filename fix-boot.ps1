# fix-boot.ps1 - Verify deployment and write clean boot_plugins.txt
$PS3_IP = "192.168.0.47"
$localSprx = "hello-plugin\build\helloworld.sprx"

Write-Host "=== Step 1: Verify local .sprx ==="
$local = Get-Item $localSprx
Write-Host "Local: $($local.Length) bytes, $($local.LastWriteTime)"

Write-Host ""
Write-Host "=== Step 2: Read boot_plugins.txt from PS3 ==="
try {
    $wc = New-Object System.Net.WebClient
    $wc.Credentials = New-Object System.Net.NetworkCredential("mike","mike")
    $bp = $wc.DownloadString("ftp://${PS3_IP}/dev_hdd0/boot_plugins.txt")
    Write-Host "Current content (raw lines follow):"
    $lines = $bp -split "`n"
    for ($i = 0; $i -lt $lines.Length; $i++) {
        $line = $lines[$i].TrimEnd("`r")
        Write-Host ("  Line " + $i + ": '" + $line + "' (len=" + $line.Length + ")")
    }
} catch {
    Write-Warning "Could not read boot_plugins.txt: $($_.Exception.Message)"
}

Write-Host ""
Write-Host "=== Step 3: Verify helloworld.sprx on PS3 ==="
try {
    $remoteStream = $wc.OpenRead("ftp://${PS3_IP}/dev_hdd0/plugins/helloworld.sprx")
    $remoteLen = $remoteStream.Length
    $remoteStream.Close()
    Write-Host "Remote helloworld.sprx: $remoteLen bytes"
    if ($local.Length -eq $remoteLen) {
        Write-Host "SIZE MATCH OK - same file"
    } else {
        Write-Host "SIZE MISMATCH! Local=$($local.Length) Remote=$remoteLen"
    }
} catch {
    Write-Host "Remote helloworld.sprx NOT FOUND at /dev_hdd0/plugins/helloworld.sprx"
    Write-Host "Checking /dev_hdd0/helloworld.sprx instead..."
    try {
        $remoteStream2 = $wc.OpenRead("ftp://${PS3_IP}/dev_hdd0/helloworld.sprx")
        $remoteLen2 = $remoteStream2.Length
        $remoteStream2.Close()
        Write-Host "Found at /dev_hdd0/helloworld.sprx: $remoteLen2 bytes"
    } catch {
        Write-Host "NOT FOUND at /dev_hdd0/ either!"
    }
}

Write-Host ""
Write-Host "=== Step 4: Verify webftp_server.sprx on PS3 ==="
try {
    $wf = $wc.OpenRead("ftp://${PS3_IP}/dev_hdd0/plugins/webftp_server.sprx")
    Write-Host "webftp_server.sprx: $($wf.Length) bytes at /dev_hdd0/plugins/"
    $wf.Close()
} catch {
    Write-Host "webftp_server.sprx NOT at /dev_hdd0/plugins/"
    try {
        $wf2 = $wc.OpenRead("ftp://${PS3_IP}/dev_hdd0/webftp_server.sprx")
        Write-Host "webftp_server.sprx: $($wf2.Length) bytes at /dev_hdd0/"
        $wf2.Close()
    } catch {
        Write-Host "webftp_server.sprx NOT FOUND anywhere"
    }
}

Write-Host ""
Write-Host "=== Step 5: Write CLEAN boot_plugins.txt ==="
try {
    # Create clean LF-only boot_plugins.txt with both entries
    $localBoot = [System.IO.Path]::GetTempFileName()
    $entries = @(
        "/dev_hdd0/plugins/webftp_server.sprx",
        "/dev_hdd0/plugins/helloworld.sprx"
    )
    $content = ($entries -join "`n") + "`n"
    [System.IO.File]::WriteAllBytes($localBoot, [System.Text.Encoding]::ASCII.GetBytes($content))
    $wc.UploadFile("ftp://${PS3_IP}/dev_hdd0/boot_plugins.txt", $localBoot)
    Remove-Item $localBoot -Force
    Write-Host "boot_plugins.txt written with $($entries.Count) entries:"
    foreach ($e in $entries) { Write-Host "  $e" }
    
    # Verify it was written correctly
    $verify = $wc.DownloadString("ftp://${PS3_IP}/dev_hdd0/boot_plugins.txt")
    Write-Host "VERIFICATION: Read back $($verify.Length) chars"
    Write-Host "--- BEGIN ---"
    Write-Host $verify
    Write-Host "--- END ---"
} catch {
    Write-Warning "Failed to write boot_plugins.txt: $($_.Exception.Message)"
}

Write-Host ""
Write-Host "=== SUMMARY ==="
Write-Host "Reboot PS3 now and listen for buzzer!"
