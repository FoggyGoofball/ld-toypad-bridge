# Download boot_plugins.txt from PS3 and inspect it byte-by-byte
$PS3_IP = "192.168.0.47"
$outFile = Join-Path $PSScriptRoot "boot_plugins_downloaded.txt"

try {
    $wc = New-Object System.Net.WebClient
    $wc.Credentials = New-Object System.Net.NetworkCredential("mike","mike")
    
    # Download raw bytes
    $data = $wc.DownloadData("ftp://${PS3_IP}/dev_hdd0/boot_plugins.txt")
    
    Write-Host "=== DOWNLOAD RESULTS ==="
    Write-Host "Total bytes downloaded: $($data.Length)"
    Write-Host ""
    
    # Save to local file
    [System.IO.File]::WriteAllBytes($outFile, $data)
    Write-Host "Saved to: $outFile"
    Write-Host ""
    
    # Hex dump
    Write-Host "=== HEX DUMP ==="
    for ($i = 0; $i -lt $data.Length; $i += 16) {
        $hex = ""
        $ascii = ""
        for ($j = 0; $j -lt 16 -and ($i + $j) -lt $data.Length; $j++) {
            $hex += "{0:X2} " -f $data[$i + $j]
            if ($data[$i + $j] -ge 0x20 -and $data[$i + $j] -le 0x7E) {
                $ascii += [char]$data[$i + $j]
            } else {
                $ascii += "."
            }
        }
        Write-Host ("{0:X4}: {1,-48} {2}" -f $i, $hex, $ascii)
    }
    
    Write-Host ""
    Write-Host "=== TEXT CONTENT ==="
    $text = [System.Text.Encoding]::ASCII.GetString($data)
    $lines = $text -split "`n"
    foreach ($line in $lines) {
        $lineClean = $line.TrimEnd("`r")
        Write-Host "  LINE: '$lineClean' (len=$($lineClean.Length))"
    }
    
} catch {
    Write-Warning "FAILED: $($_.Exception.Message)"
}
