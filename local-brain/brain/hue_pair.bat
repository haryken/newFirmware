@echo off
rem Pair EMO with your Philips Hue bridge(s). Optional: bridge IPs, e.g.  hue_pair.bat 192.168.1.20
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File hue_pair.ps1 %*
pause
