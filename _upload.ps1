$wc = New-Object System.Net.WebClient
$wc.Credentials = New-Object System.Net.NetworkCredential('mike','mike')
$wc.UploadFile('ftp://192.168.0.22/dev_hdd0/plugins/ldtoypad.sprx', 'sprx-plugin\build\ldtoypad.sprx')
Write-Host "Upload OK $( (Get-Item 'sprx-plugin\build\ldtoypad.sprx').Length ) bytes"
