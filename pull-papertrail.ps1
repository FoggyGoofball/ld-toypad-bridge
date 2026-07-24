param([string]$PS3IP = "192.168.0.47")

$files = @(
    "/dev_hdd0/plugins/ldtoypad_boot.log",
    "/dev_hdd0/plugins/ldtoypad_debug.log",
    "/dev_hdd0/tmp/ld_hooks_ready.txt",
    "/dev_hdd0/tmp/ld_hooks_shutdown.txt",
    "/dev_hdd0/tmp/ld_self_ip.txt",
    "/dev_hdd0/tmp/ld_recv_papertrail.txt",
    "/dev_hdd0/tmp/ld_probe_papertrail.txt",
    "/dev_hdd0/tmp/ld_paper.txt",
    "/dev_hdd0/tmp/ld_init_progress.txt"
)

try {
    $wc = New-Object System.Net.WebClient
    $wc.Credentials = New-Object System.Net.NetworkCredential("mike","mike")

    foreach ($f in $files) {
        $name = [System.IO.Path]::GetFileName($f)
        $uri = "ftp://${PS3IP}${f}"
        $local = Join-Path $PSScriptRoot "papertrail_$name"
        try {
            $wc.DownloadFile($uri, $local)
            $size = (Get-Item $local).Length
            Write-Host "[OK] $name - $size bytes"
        } catch {
            Write-Host "[MISSING] $name"
        }
    }
} catch {
    Write-Host "[FATAL] FTP connection failed: $($_.Exception.Message)"
}

Write-Host ""
Write-Host "=== DOWNLOADED FILES ==="
Get-ChildItem $PSScriptRoot\papertrail_* -ErrorAction SilentlyContinue | ForEach-Object {
    Write-Host "$($_.Name) - $($_.Length) bytes"
}
