@echo off
setlocal enabledelayedexpansion

cd /d "%~dp0"

set "UDP_PORT=28472"
set "DEBUG_PORT=28473"
set "HTTP_PORT=8080"
set "RULE_MAIN=LD-ToyPad UDP 28472"
set "RULE_DEBUG=LD-ToyPad UDP 28473"
set "SERVER_URL=http://localhost:%HTTP_PORT%"
set "API_URL=%SERVER_URL%/api/status"
set "TEMP_FLAG=%TEMP%\ldtoypad.enable"
set "PS3_IP="
set "PS3_CACHE_FILE=%~dp0ps3-ip.txt"
set "TIMEOUT=%WINDIR%\System32\timeout.exe"

echo ============================================================
echo  LD-ToyPad Elevated Bridge Launcher
echo ============================================================
echo  Working Dir: %CD%
echo  UDP Port:    %UDP_PORT%
echo  Debug Port:  %DEBUG_PORT%
echo  HTTP Port:   %HTTP_PORT%
echo  API URL:     %API_URL%
echo ============================================================
echo.

:: [1/7] Firewall
echo [1/7] Resetting Windows Firewall rules...
netsh advfirewall firewall delete rule name="%RULE_MAIN%" >nul 2>&1
netsh advfirewall firewall delete rule name="%RULE_DEBUG%" >nul 2>&1
netsh advfirewall firewall add rule name="%RULE_MAIN%" dir=in action=allow protocol=UDP localport=%UDP_PORT% profile=any >nul
if errorlevel 1 echo ERROR: Firewall rule %UDP_PORT% failed & pause & exit /b 1
netsh advfirewall firewall add rule name="%RULE_DEBUG%" dir=in action=allow protocol=UDP localport=%DEBUG_PORT% profile=any >nul
if errorlevel 1 echo ERROR: Firewall rule %DEBUG_PORT% failed & pause & exit /b 1
echo [2/7] Firewall rules created.

:: [3/7] Kill old bridge
echo [3/7] Stopping any bridge process bound to UDP %UDP_PORT%/%DEBUG_PORT%...
for /f "usebackq delims=" %%P in (`powershell -NoProfile -Command "$ports=%UDP_PORT%,%DEBUG_PORT%; $ids=Get-NetUDPEndpoint ^| Where-Object { $ports -contains $_.LocalPort } ^| Select-Object -ExpandProperty OwningProcess -Unique; $ids ^| ForEach-Object { $_ }"`) do (
  taskkill /PID %%P /F >nul 2>&1
)
%TIMEOUT% /t 1 /nobreak >nul

:: Read cached PS3 IP
set "CACHED_IP="
if exist "%PS3_CACHE_FILE%" (
  set /p CACHED_IP=<"%PS3_CACHE_FILE%"
)

:: [4/7] Start server in background
echo [4/7] Starting bridge server in background...
set "LOG_FILE=%TEMP%\ldtoypad-server.log"
if exist "%LOG_FILE%" del /q "%LOG_FILE%" >nul 2>nul
if defined CACHED_IP (
  echo  Using cached PS3 IP: !CACHED_IP! (directed probe)
  start /b "" cmd /c "node server.js --host 0.0.0.0 --http-port %HTTP_PORT% --port %UDP_PORT% --debug-port %DEBUG_PORT% --ps3-ip !CACHED_IP! > "%LOG_FILE%" 2>&1"
) else (
  start /b "" cmd /c "node server.js --host 0.0.0.0 --http-port %HTTP_PORT% --port %UDP_PORT% --debug-port %DEBUG_PORT% > "%LOG_FILE%" 2>&1"
)
echo Waiting for server to start...
%TIMEOUT% /t 2 /nobreak >nul

:: [5/7] Wait for PS3 client (up to 10s with cached IP, 60s without)
echo [5/7] Waiting for PS3 to connect...
if defined CACHED_IP (
  set /a "TIMEOUT_SEC=10"
) else (
  set /a "TIMEOUT_SEC=60"
)
set /a "POLL_INTERVAL=2"
set /a "MAX_ATTEMPTS=TIMEOUT_SEC / POLL_INTERVAL"
set /a "ATTEMPT=0"

:wait_loop
if !ATTEMPT! geq !MAX_ATTEMPTS! (
  echo  ^|  Timeout. PS3 not detected within !ELAPSED!s.
  echo  ^|  Check: PS3 on? Firewall open? Enable flag /dev_hdd0/plugins/ldtoypad.enable present?
  goto :after_wait
)
set /a "ATTEMPT=ATTEMPT+1"
set /a "ELAPSED=ATTEMPT*POLL_INTERVAL"

:: Query API for client.address. Use [Console]::Write so no output = empty for /f captures nothing.
for /f "usebackq delims=" %%J in (`powershell -NoProfile -Command "try{$r=Invoke-WebRequest -Uri '%API_URL%' -UseBasicParsing -TimeoutSec 3 -ErrorAction Stop;$j=$r.Content|ConvertFrom-Json;if($j.client.address){[Console]::Write($j.client.address.Trim())}}catch{}"`) do (
  set "PS3_IP=%%J"
)
if defined PS3_IP (
  echo  ^|  PS3 detected at !PS3_IP! after !ELAPSED!s
  goto :after_wait
)

set /a "MOD=ATTEMPT %% 4"
if !MOD! equ 0 set "SPIN=/" & if !MOD! equ 1 set "SPIN=-" & if !MOD! equ 2 set "SPIN=\" & if !MOD! equ 3 set "SPIN=|"
<nul set /p "=  !SPIN! !ELAPSED!s / !TIMEOUT_SEC!s waiting for PS3...  "
%TIMEOUT% /t !POLL_INTERVAL! /nobreak >nul
goto :wait_loop

:after_wait
echo.

:: [6/7] Move server to foreground
echo [6/7] Moving server to foreground...
for /f "usebackq delims=" %%P in (`powershell -NoProfile -Command "$ports=%UDP_PORT%,%DEBUG_PORT%;$ids=Get-NetUDPEndpoint|Where-Object{$ports-contains$_.LocalPort}|Select-Object -ExpandProperty OwningProcess -Unique;$ids|ForEach-Object{$_}"`) do (
  taskkill /PID %%P /F >nul 2>&1
)
%TIMEOUT% /t 1 /nobreak >nul

start "LD-ToyPad Bridge" cmd /k "cd /d ""%~dp0"" && echo *** LD-ToyPad Bridge *** && echo PS3: !PS3_IP! && echo. && node server.js --http-port %HTTP_PORT% --port %UDP_PORT% --debug-port %DEBUG_PORT%"

echo.
echo  Browser UI: %SERVER_URL%
echo  PS3 cache:  %PS3_CACHE_FILE%
echo  Enable token is permanent — no reboot needed to re-arm.
echo.
endlocal
exit /b 0
