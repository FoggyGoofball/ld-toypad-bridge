# deploy-enable.ps1 -- Create one-shot enable flag on PS3
#
# Reads PS3 IP from ps3-ip.txt in the script directory.
# Override: .\deploy-enable.ps1 -PS3IP 192.168.0.47

param(
    [string]$PS3IP
)

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$cacheFile = Join-Path $scriptDir "ps3-ip.txt"

if (-not $PS3IP) {
    if (Test-Path $cacheFile) {
        $PS3IP = (Get-Content $cacheFile -First 1).Trim()
        Write-Host "Using cached PS3 IP: $PS3IP"
    } else {
        Write-Warning "No PS3 IP provided and $cacheFile not found."
        Write-Host "Usage: .\deploy-enable.ps1 -PS3IP 192.168.0.47"
        exit 1
    }
}

$REMOTE_DIR = "/dev_hdd0/plugins"

try {
    $tempFile = [System.IO.Path]::GetTempFileName()
    [System.IO.File]::WriteAllText($tempFile, "enabled")

    $wc = New-Object System.Net.WebClient
    $wc.Credentials = New-Object System.Net.NetworkCredential("mike","mike")

    $uri = "ftp://${PS3IP}${REMOTE_DIR}/ldtoypad.enable"
    Write-Host "Uploading enable token to ${uri}..."
    $wc.UploadFile($uri, $tempFile)
    Write-Host "OK - Enable token uploaded (one-shot, consumed on next boot)"

    Remove-Item $tempFile -Force
} catch {
    Write-Warning "Failed to upload enable token: $($_.Exception.Message)"
    Write-Host ""
    Write-Host "To create manually using FTP client:"
    Write-Host "  1. Connect to ftp://${PS3IP} (mike/mike)"
    Write-Host "  2. Create file at /dev_hdd0/plugins/ldtoypad.enable with content 'enabled'"
}
