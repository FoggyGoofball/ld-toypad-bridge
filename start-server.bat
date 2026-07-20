@echo off
cd /d "%~dp0ld-toypad-server"
echo Starting LD-ToyPad Bridge Server...
echo PS3 IP: 192.168.0.47
echo.
node server.js --host 0.0.0.0 --http-port 8080 --port 28472 --debug-port 28473 --ps3-ip 192.168.0.47 --verbose
pause
