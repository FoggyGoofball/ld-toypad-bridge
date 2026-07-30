$wc = New-Object System.Net.WebClient
$wc.Credentials = New-Object System.Net.NetworkCredential('mike','mike')

$local = 'C:\Users\Admin\EBOOT_PATCHED.BIN'
$remote = 'ftp://192.168.0.22/dev_hdd0/game/BLUS31473INSTALLDATA/USRDIR/EBOOT.BIN'

Write-Host "Uploading EBOOT.BIN ($( (Get-Item $local).Length ) bytes)..."
$wc.UploadFile($remote, $local)
Write-Host "Upload OK!"
