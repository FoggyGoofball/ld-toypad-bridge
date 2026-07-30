Get-PnpDevice -PresentOnly | Where-Object { $_.Class -eq 'DiskDrive' -or $_.Class -eq 'USB' } | Select-Object FriendlyName, InstanceId
Write-Host '---'
Get-CimInstance Win32_DiskDrive | Where-Object { $_.InterfaceType -eq 'USB' } | ForEach-Object { Write-Host $_.PNPDeviceID }
