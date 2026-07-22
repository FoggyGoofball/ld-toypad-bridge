# fix-boot-plugins-clean.ps1
# Fix boot_plugins.txt to ONLY contain webftp_server.sprx
# Removes ldtoypad.sprx from boot-time loading

$PS3_IP = "192.168.0.47"
$CRED = New-Object System.Net.NetworkCredential("mike","mike")

Write-Host "=== Step 1: Read current boot_plugins.txt ==="
try {
    $wc = New-Object System.Net.WebClient
    $wc.Credentials = $CRED
    $content = $wc.DownloadString("ftp://${PS3_IP}/dev_hdd0/boot_plugins.txt")
    Write-Host "Current content:"
    Write-Host $content
    Write-Host "---"
} catch {
    Write-Host "Could not read: $($_.Exception.Message)"
    $content = ""
}

Write-Host "=== Step 2: Create clean content (ONLY webftp_server.sprx) ==="
$clean = "/dev_hdd0/plugins/webftp_server.sprx`n"
Write-Host "New content will be: '$clean'"

Write-Host "=== Step 3: Upload via FtpWebRequest ==="
try {
    $ftp = [System.Net.FtpWebRequest]::Create("ftp://${PS3_IP}/dev_hdd0/boot_plugins.txt")
    $ftp.Method = [System.Net.WebRequestMethods+Ftp]::UploadFile
    $ftp.Credentials = $CRED
    $ftp.UseBinary = $true
    $ftp.UsePassive = $true
    
    $bytes = [System.Text.Encoding]::ASCII.GetBytes($clean)
    $stream = $ftp.GetRequestStream()
    $stream.Write($bytes, 0, $bytes.Length)
    $stream.Close()
    $resp = $ftp.GetResponse()
    Write-Host "Upload response: $($resp.StatusDescription)"
    $resp.Close()
} catch {
    Write-Host "Upload FAILED: $($_.Exception.Message)"
}

Write-Host "=== Step 4: Verify read-back ==="
try {
    $wc2 = New-Object System.Net.WebClient
    $wc2.Credentials = $CRED
    $verify = $wc2.DownloadString("ftp://${PS3_IP}/dev_hdd0/boot_plugins.txt")
    Write-Host "Verified content:"
    Write-Host "---"
    Write-Host $verify
    Write-Host "---"
    if ($verify.Contains("ldtoypad")) {
        Write-Host "PROBLEM: ldtoypad.sprx STILL present!"
        exit 1
    } elseif ($verify.Contains("webftp")) {
        Write-Host "SUCCESS: Only webftp_server.sprx - safe to re-enable Cobra and reboot!"
    } else {
        Write-Host "UNEXPECTED content - please check manually"
    }
} catch {
    Write-Host "Verify failed: $($_.Exception.Message)"
}
