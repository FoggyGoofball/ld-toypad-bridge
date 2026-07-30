$ftpHost = "192.168.0.22"
$user = "mike"
$pass = "mike"
$localFile = "C:\temp\BLUS31473\USRDIR\PAD.DAT"
$remotePath = "/dev_hdd0/GAMES/BLUS31473/USRDIR/PAD.DAT"
$url = "ftp://$ftpHost$remotePath"

Write-Host "Uploading PAD.DAT (11.9 GB) via stream..."
$fileInfo = Get-Item $localFile
$totalMB = [math]::Round($fileInfo.Length / 1MB, 1)

$req = [System.Net.FtpWebRequest]::Create($url)
$req.Method = [System.Net.WebRequestMethods+Ftp]::UploadFile
$req.Credentials = New-Object System.Net.NetworkCredential($user, $pass)
$req.UseBinary = $true
$req.KeepAlive = $false
$req.Timeout = -1
$req.ReadWriteTimeout = -1

$fileStream = [System.IO.File]::OpenRead($localFile)
$req.ContentLength = $fileStream.Length
$ftpStream = $req.GetRequestStream()

$buffer = New-Object byte[] (1MB)
$totalBytes = 0
$lastReport = Get-Date
$lastBytes = 0

while ($true) {
    $read = $fileStream.Read($buffer, 0, $buffer.Length)
    if ($read -eq 0) { break }
    $ftpStream.Write($buffer, 0, $read)
    $totalBytes += $read
    
    $now = Get-Date
    $elapsed = ($now - $lastReport).TotalSeconds
    if ($elapsed -ge 5) {
        $pct = [math]::Round($totalBytes / $fileStream.Length * 100, 1)
        $mbDone = [math]::Round($totalBytes / 1MB, 1)
        $speed = if ($elapsed -gt 0) { [math]::Round(($totalBytes - $lastBytes) / 1MB / $elapsed, 1) } else { 0 }
        $eta = if ($speed -gt 0) { 
            $remaining = $fileStream.Length - $totalBytes
            [math]::Round($remaining / 1MB / $speed / 60, 0)
        } else { "?" }
        Write-Host "  $pct% ($mbDone/$totalMB MB) ${speed} MB/s | ~${eta} min remaining"
        $lastReport = $now
        $lastBytes = $totalBytes
    }
}

$ftpStream.Close()
$fileStream.Close()
$resp = $req.GetResponse()
$resp.Close()
Write-Host "PAD.DAT uploaded successfully!" -ForegroundColor Green
