$wc = New-Object System.Net.WebClient
$wc.Credentials = New-Object System.Net.NetworkCredential('mike','mike')

$dir = 'ftp://192.168.0.22/dev_hdd0/plugins/'

Write-Host "=== Wiping all ldtoypad files ==="

$files = @(
    'ldtoypad.sprx',
    'ldtoypad.enable',
    'ldtoypad_boot.log',
    'ldtoypad_debug.log',
    'ld_hooks.tmp',
    'ld_hooks_ready.txt'
)

foreach ($f in $files) {
    $uri = "$dir$f"
    $req = [System.Net.FtpWebRequest]::Create($uri)
    $req.Method = [System.Net.WebRequestMethods+Ftp]::DeleteFile
    $req.Credentials = $wc.Credentials
    try { 
        $req.GetResponse() | Out-Null
        Write-Host "  DELETED: $f" 
    } catch { 
        Write-Host "  (not found): $f" 
    }
}

Write-Host ""
Write-Host "=== Remaining plugins ==="
$listReq = [System.Net.FtpWebRequest]::Create($dir)
$listReq.Method = [System.Net.WebRequestMethods+Ftp]::ListDirectory
$listReq.Credentials = $wc.Credentials
try {
    $resp = $listReq.GetResponse()
    $sr = New-Object System.IO.StreamReader($resp.GetResponseStream())
    while ($line = $sr.ReadLine()) { Write-Host "  $line" }
    $sr.Close(); $resp.Close()
} catch { Write-Host "ERROR: $_" }
