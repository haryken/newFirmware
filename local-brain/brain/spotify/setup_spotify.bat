@echo off
rem One-time Spotify setup for EMO (go-librespot). Needs a Spotify Premium account.
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File setup_spotify.ps1
pause
