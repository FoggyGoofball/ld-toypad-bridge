$wc = New-Object System.Net.WebClient
$wc.Credentials = New-Object System.Net.NetworkCredential('mike','mike')

Write-Host "=== boot_plugins.txt ==="
try { $wc.DownloadString('ftp://192.168.0.22/dev_hdd0/boot_plugins.txt') } catch { Write-Host "ERROR: $_" }

Write-Host ""
Write-Host "=== plugins dir listing ==="
$req = [System.Net.FtpWebRequest]::Create("ftp://192.168.0.22/dev_hdd0/plugins/")
$req.Method = [System.Net.WebRequestMethods+Ftp]::ListDirectory
$req.Credentials = $wc.Credentials
try {
    $resp = $req.GetResponse()
    $sr = New-Object System.IO.StreamReader($resp.GetResponseStream())
    while ($line = $sr.ReadLine()) { Write-Host "  $line" }
    $sr.Close(); $resp.Close()
} catch { Write-Host "ERROR: $_" }

Write-Host ""
Write-Host "=== enable token check ==="
try { $wc.DownloadString('ftp://192.168.0.22/dev_hdd0/plugins/ldtoypad.enable') } catch { Write-Host "No enable token found" }
