$ftpHost = "192.168.0.22"
$user = "mike"
$pass = "mike"
$localRoot = "C:\temp\BLUS31473"
$remoteRoot = "/dev_hdd0/GAMES/BLUS31473"

function Ensure-RemoteDir($remoteDir) {
    $url = "ftp://$ftpHost$remoteDir/"
    try {
        $req = [System.Net.FtpWebRequest]::Create($url)
        $req.Method = [System.Net.WebRequestMethods+Ftp]::MakeDirectory
        $req.Credentials = New-Object System.Net.NetworkCredential($user, $pass)
        $req.Timeout = 5000
        $resp = $req.GetResponse()
        $resp.Close()
    } catch { }
}

function Upload-File($localPath, $remotePath) {
    $url = "ftp://$ftpHost$remotePath"
    try {
        $req = [System.Net.FtpWebRequest]::Create($url)
        $req.Method = [System.Net.WebRequestMethods+Ftp]::UploadFile
        $req.Credentials = New-Object System.Net.NetworkCredential($user, $pass)
        $req.UseBinary = $true
        $req.KeepAlive = $false
        $req.Timeout = 300000
        $req.ReadWriteTimeout = 300000

        $bytes = [System.IO.File]::ReadAllBytes($localPath)
        $req.ContentLength = $bytes.Length
        $stream = $req.GetRequestStream()
        $chunkSize = 65536
        $offset = 0
        while ($offset -lt $bytes.Length) {
            $len = [Math]::Min($chunkSize, $bytes.Length - $offset)
            $stream.Write($bytes, $offset, $len)
            $offset += $len
        }
        $stream.Close()
        $resp = $req.GetResponse()
        $resp.Close()
        return $true
    } catch {
        Write-Host "  ERROR: $_" -ForegroundColor Red
        return $false
    }
}

Write-Host "Creating directories..." -ForegroundColor Cyan
Ensure-RemoteDir $remoteRoot

$dirs = @(
    "$remoteRoot/LICDIR",
    "$remoteRoot/TROPDIR",
    "$remoteRoot/TROPDIR/NPWR07424_00",
    "$remoteRoot/USRDIR"
)
foreach ($d in $dirs) {
    Ensure-RemoteDir $d
}

Write-Host "Collecting files..." -ForegroundColor Cyan
$files = Get-ChildItem -Path $localRoot -Recurse -File | Sort-Object Length
$total = $files.Count
$i = 0
$totalSize = ($files | Measure-Object -Property Length -Sum).Sum
$uploaded = 0L

Write-Host "Uploading $total files ($([math]::Round($totalSize/1GB,2)) GB)..." -ForegroundColor Cyan
Write-Host ""

foreach ($f in $files) {
    $i++
    $relPath = $f.FullName.Substring($localRoot.Length).Replace("\", "/")
    $remotePath = "$remoteRoot$relPath"
    $sizeMB = [math]::Round($f.Length/1MB, 1)
    $pct = [math]::Round($i/$total*100, 1)
    
    Write-Host "[$i/$total $pct%] $sizeMB MB $($f.Name)" -NoNewline
    $elapsed = Measure-Command {
        $ok = Upload-File $f.FullName $remotePath
    }
    if ($ok) {
        $uploaded += $f.Length
        $speed = if ($elapsed.TotalSeconds -gt 0) { [math]::Round(($f.Length/1MB)/$elapsed.TotalSeconds, 1) } else { 0 }
        Write-Host " | ${speed} MB/s" -ForegroundColor Green
    }
}

$totalUploaded = [math]::Round($uploaded/1GB, 2)
Write-Host ""
Write-Host "Done! Uploaded $totalUploaded GB to $remoteRoot" -ForegroundColor Green
