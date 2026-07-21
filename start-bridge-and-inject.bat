@echo off
title LD-ToyPad Bridge + Injector
setlocal enabledelayedexpansion

echo ============================================================
echo  LD-ToyPad Bridge — Full Start (Server + Injector)
echo ============================================================
echo.

set "SCRIPT_DIR=%~dp0"
set "SERVER_DIR=%SCRIPT_DIR%ld-toypad-server"
set "PS3_IP_FILE=%SCRIPT_DIR%ps3-ip.txt"

:: Read PS3 IP
set "PS3_IP="
if exist "%PS3_IP_FILE%" (
  set /p PS3_IP=<"%PS3_IP_FILE%"
)
if not defined PS3_IP (
  echo ERROR: No PS3 IP found. Create ps3-ip.txt with your PS3 IP.
  echo Example: 192.168.0.47
  pause
  exit /b 1
)

echo PS3 IP: %PS3_IP%

:: Kill any existing node processes (server + injector)
echo [1/4] Killing old node processes...
taskkill /F /IM node.exe /FI "WINDOWTITLE eq ldtoypad*" >nul 2>&1
taskkill /F /FI "WindowTitle eq ldtoypad*" /T >nul 2>&1
ping -n 2 127.0.0.1 >nul
echo   Done.

:: Start the UDP bridge server in a new window
echo [2/4] Starting bridge server...
cd /d "%SERVER_DIR%"
set "LOG_FILE=%TEMP%\ldtoypad-server.log"
start "ldtoypad-server" cmd /c "title ldtoypad-server && node server.js --host 0.0.0.0 --http-port 8080 --port 28472 --debug-port 28473 --ps3-ip %PS3_IP% --verbose > "%LOG_FILE%" 2>&1"
echo   Server starting on port 28472... (log: %LOG_FILE%)

:: Wait a moment for the server to bind
ping -n 3 127.0.0.1 >nul

:: Launch the injector in a second window (waits for game, then injects)
echo [3/4] Launching injector (waits for LEGO Dimensions)...
start "ldtoypad-injector" cmd /k "title ldtoypad-injector && cd /d "%SCRIPT_DIR%" && node ld-toypad-server/scripts/inject-sprx.js --ps3-ip %PS3_IP% --wait 30 --verbose"

echo [4/4] Opening browser UI...
start "" "http://localhost:8080"

echo.
echo ============================================================
echo  All systems starting. The injector window will:
echo    1. Wait for LEGO Dimensions to launch
echo    2. Inject the SPRX into the game process
echo    3. Install USB hooks (Phase 2 preambles)
echo ============================================================
echo.
echo  Server log: %LOG_FILE%
echo  UI:         http://localhost:8080
echo.
echo  Press any key to close this window (server/injector stay open)
pause >nul
endlocal
