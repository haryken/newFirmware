@echo off
rem Installs (or updates) the EMO PC Agent tray app for this Windows user.
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File install_pc_agent.ps1
