@echo off
rem One-time setup for EMO's local brain (see setup_brain.ps1). Log: setup_brain.log
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File setup_brain.ps1 > setup_brain.log 2>&1
if errorlevel 1 (echo ==== FAILED >> setup_brain.log) else (echo ==== ALL DONE >> setup_brain.log)
