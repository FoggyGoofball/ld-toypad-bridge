param([string]$PS3IP = "192.168.0.47")
$cred = New-Object System.Net.NetworkCredential("mike","mike")

Write-Host "============================================"
Write-Host "PS3 Boot Log Check - $PS3IP"
Write-Host "============================================"
Write-Host ""

# Try FTP listing of plugins dir with WebClient
$wc = New-Object System.Net.WebClient
$wc.Credentials = $cred

# Check 1: plugins dir listing
Write-Host "[1] Testing FTP connectivity to $PS3IP (port 21)..."
try {
    # FtpWebRequest approach
    $req = [System.Net.FtpWebRequest]::Create("ftp://$PS3IP/dev_hdd0/plugins/")
    $req.Method = [System.Net.WebRequestMethods+Ftp]::ListDirectory
    $req.Credentials = $cred
    $resp = $req.GetResponse()
    $stream = $resp.GetResponseStream()
    $reader = New-Object System.IO.StreamReader($stream)
    $listing = $reader.ReadToEnd()
    Write-Host "  FTP OK! Directory listing:"
    Write-Host $listing
    $reader.Close()
    $resp.Close()
} catch {
    Write-Host "  FTP FAILED (game running or FTP not started):"
    Write-Host "  $($_.Exception.Message)"
}

Write-Host ""

# Check 2: boot log at /dev_hdd0/plugins/
Write-Host "[2] Checking boot log (new path)..."
try {
    $req2 = [System.Net.FtpWebRequest]::Create("ftp://$PS3IP/dev_hdd0/plugins/ldtoypad_boot.log")
    $req2.Method = [System.Net.WebRequestMethods+Ftp]::DownloadFile
    $req2.Credentials = $cred
    $resp2 = $req2.GetResponse()
    $stream2 = $resp2.GetResponseStream()
    $reader2 = New-Object System.IO.StreamReader($stream2)
    $bootlog = $reader2.ReadToEnd()
    Write-Host "  BOOT LOG FOUND!"
    Write-Host "============================================"
    Write-Host $bootlog
    Write-Host "============================================"
    $reader2.Close()
    $resp2.Close()
} catch {
    Write-Host "  NOT FOUND: $($_.Exception.Message)"
}

Write-Host ""

# Check 3: enable token
Write-Host "[3] Checking enable token..."
try {
    $req3 = [System.Net.FtpWebRequest]::Create("ftp://$PS3IP/dev_hdd0/plugins/ldtoypad.enable")
    $req3.Method = [System.Net.WebRequestMethods+Ftp]::DownloadFile
    $req3.Credentials = $cred
    $resp3 = $req3.GetResponse()
    $stream3 = $resp3.GetResponseStream()
    $reader3 = New-Object System.IO.StreamReader($stream3)
    $token = $reader3.ReadToEnd()
    Write-Host "  ENABLE TOKEN: PRESENT (not consumed)"
    $reader3.Close()
    $resp3.Close()
} catch {
    Write-Host "  ENABLE TOKEN: NOT FOUND (consumed or never created)"
}

Write-Host ""
Write-Host "============================================"
Write-Host "Done."
