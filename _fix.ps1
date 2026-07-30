$wc = New-Object System.Net.WebClient
$wc.Credentials = New-Object System.Net.NetworkCredential('mike','mike')

Write-Host "=== Deleting ldtoypad.enable ==="
$delUri = 'ftp://192.168.0.22/dev_hdd0/plugins/ldtoypad.enable'
$delReq = [System.Net.FtpWebRequest]::Create($delUri)
$delReq.Method = [System.Net.WebRequestMethods+Ftp]::DeleteFile
$delReq.Credentials = $wc.Credentials
try { $delReq.GetResponse() | Out-Null; Write-Host "DELETED" } catch { Write-Host "Delete failed: $_" }

Write-Host ""
Write-Host "=== Verify enable token is gone ==="
try { $wc.DownloadString('ftp://192.168.0.22/dev_hdd0/plugins/ldtoypad.enable'); Write-Host "STILL PRESENT!" } catch { Write-Host "Gone." }

Write-Host ""
Write-Host "=== Clean boot_plugins.txt ==="
try { Write-Host ($wc.DownloadString('ftp://192.168.0.22/dev_hdd0/boot_plugins.txt')) } catch { Write-Host "ERROR: $_" }
