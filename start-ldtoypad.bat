@echo off
setlocal enabledelayedexpansion

set "SERVER_DIR=%~dp0ld-toypad-server"
set "UDP_PORT=28472"
set "DEBUG_PORT=28473"
set "HTTP_PORT=8080"
set "SERVER_URL=http://localhost:%HTTP_PORT%"
set "API_URL=%SERVER_URL%/api/status"
set "PS3_CACHE_FILE=%~dp0ps3-ip.txt"
set "PS3_IP="

if not exist "%SERVER_DIR%\server.js" (
  echo Could not find ld-toypad-server\server.js next to this script.
  pause
  exit /b 1
)

if not exist "%SERVER_DIR%\node_modules" (
  echo Node dependencies are missing.
  echo Open a terminal in ld-toypad-server and run: npm install
  pause
  exit /b 1
)

:: Read cached PS3 IP (if any)
set "CACHED_IP="
if exist "%PS3_CACHE_FILE%" (
  set /p CACHED_IP=<"%PS3_CACHE_FILE%"
)

echo ============================================================
echo  LD-ToyPad Bridge Launcher (Quick Start)
echo ============================================================
echo.

:: [1/4] Kill any old bridge processes
echo [1/4] Stopping old bridge processes...
taskkill /F /FI "WindowTitle eq ldtoypad-bridge*" /T >nul 2>&1
ping -n 2 127.0.0.1 >nul

:: [2/4] Upload one-shot enable token to PS3
echo [2/4] Uploading enable token to PS3...
if not defined CACHED_IP (
  echo  No cached PS3 IP. Skipping token upload.
  echo  To arm the plugin, run: deploy-enable.ps1 -PS3IP 192.168.0.xx
) else (
  powershell -NoProfile -Command ^
    "$f=[System.IO.Path]::GetTempFileName();" ^
    "[System.IO.File]::WriteAllText($f,'enabled');" ^
    "$wc=New-Object System.Net.WebClient;" ^
    "$wc.Credentials=New-Object System.Net.NetworkCredential('mike','mike');" ^
    "try{$wc.UploadFile('ftp://!CACHED_IP!/dev_hdd0/plugins/ldtoypad.enable',$f);" ^
    "Write-Host '  Token uploaded — one-shot, consumed on reboot'}catch{Write-Host '  FAILED to upload token. PS3 FTP may be down.'};" ^
    "Remove-Item $f -Force"
)

:: [3/4] Start bridge server in background with timestamped log
echo [3/4] Starting bridge server in background...
cd /d "%SERVER_DIR%"

set "NOW=%DATE:/=-%_%TIME::=-%"
set "NOW=!NOW: =0!"
set "LOG_FILE=%TEMP%\ldtoypad-server-!NOW!.log"

if defined CACHED_IP (
  echo  Using cached PS3 IP: !CACHED_IP! (directed probe)
  start "ldtoypad-bridge" cmd /c "title ldtoypad-bridge && node server.js --host 0.0.0.0 --http-port %HTTP_PORT% --port %UDP_PORT% --debug-port %DEBUG_PORT% --ps3-ip !CACHED_IP! > "%LOG_FILE%" 2>&1"
) else (
  start "ldtoypad-bridge" cmd /c "title ldtoypad-bridge && node server.js --host 0.0.0.0 --http-port %HTTP_PORT% --port %UDP_PORT% --debug-port %DEBUG_PORT% > "%LOG_FILE%" 2>&1"
)

echo Waiting for server to start...
ping -n 3 127.0.0.1 >nul

:: [4/4] Wait for PS3 client
echo [4/4] Waiting for PS3 to connect...
if defined CACHED_IP (
  set /a "MAX_SEC=10"
) else (
  set /a "MAX_SEC=60"
)
set /a "ATTEMPT=0"
set /a "MAX_ATTEMPTS=MAX_SEC/2"

:wait_loop
if !ATTEMPT! geq !MAX_ATTEMPTS! (
  echo  ^|  Timeout. PS3 not detected within !MAX_SEC!s.
  goto :after_wait
)
set /a "ATTEMPT=ATTEMPT+1"
set /a "ELAPSED=ATTEMPT*2"

for /f "usebackq delims=" %%J in (`powershell -NoProfile -Command "try{$r=Invoke-WebRequest -Uri '%API_URL%' -UseBasicParsing -TimeoutSec 3 -ErrorAction Stop;$j=$r.Content|ConvertFrom-Json;if($j.client.address){[Console]::Write($j.client.address.Trim())}}catch{}"`) do (
  set "PS3_IP=%%J"
)
if defined PS3_IP (
  echo  ^|  PS3 detected at !PS3_IP! after !ELAPSED!s
  goto :after_wait
)

set /a "MOD=ATTEMPT %% 4"
if !MOD! equ 0 set "SPIN=/" & if !MOD! equ 1 set "SPIN=-" & if !MOD! equ 2 set "SPIN=\" & if !MOD! equ 3 set "SPIN=|"
<nul set /p "=  !SPIN! !ELAPSED!s / !MAX_SEC!s  "
ping -n 3 127.0.0.1 >nul
goto :wait_loop

:after_wait
echo.

:: Cache PS3 IP
if defined PS3_IP (
  >"%PS3_CACHE_FILE%" echo !PS3_IP!
  echo PS3 IP cached to %PS3_CACHE_FILE%
) else if defined CACHED_IP (
  echo PS3 not responding at !CACHED_IP!.
  echo  The enable token has been uploaded.
  echo.
  echo  NEXT STEP: **Restart (reboot) your PS3 now.**
  echo  The plugin will fire once, consume the token, and connect.
  echo.
  echo  If it still doesn't appear after reboot, check:
  echo   1. PS3 is turned ON
  echo   2. Firewall allows UDP !UDP_PORT! and !DEBUG_PORT!
  echo   3. Plugin is registered in boot_plugins.txt
) else (
  echo PS3 not detected yet.
  echo.
  echo  NEXT STEP: Upload enable token and restart PS3:
  echo   .\deploy-enable.ps1 -PS3IP 192.168.0.xx
  echo   (then restart PS3)
)

:: Open browser UI
echo Opening browser UI...
start "" "http://localhost:%HTTP_PORT%"

echo.
echo  Browser UI: %SERVER_URL%
echo.
pause
endlocal
