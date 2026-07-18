# Quick verification: compare local and remote SPRX sizes
$PS3_IP = "192.168.0.47"
$localPath = "hello-plugin\build\helloworld.sprx"
$remotePath = "/dev_hdd0/plugins/helloworld.sprx"
$local = Get-Item $localPath
Write-Host "Local:  $($local.FullName) - $($local.Length) bytes - LastWrite $($local.LastWriteTime)"

try {
    $wc = New-Object System.Net.WebClient
    $wc.Credentials = New-Object System.Net.NetworkCredential("mike","mike")
    $remoteStream = $wc.OpenRead("ftp://${PS3_IP}${remotePath}")
    $remoteLen = $remoteStream.Length
    $remoteStream.Close()
    Write-Host "Remote: ftp://${PS3_IP}${remotePath} - $remoteLen bytes"
    if ($local.Length -eq $remoteLen) {
        Write-Host "SIZE MATCH OK - BINARY transfer confirmed (same byte count)"
    } else {
        Write-Host "SIZE MISMATCH! local=$($local.Length) remote=$remoteLen"
    }
} catch {
    Write-Warning "FTP check failed: $($_.Exception.Message)"
}
Write-Host ""
Write-Host "boot_plugins.txt contents:"
try {
    $bp = $wc.DownloadString("ftp://${PS3_IP}/dev_hdd0/boot_plugins.txt")
    Write-Host "--- BEGIN ---"
    Write-Host $bp
    Write-Host "--- END ---"
} catch {
    Write-Warning "Could not read boot_plugins.txt"
}
