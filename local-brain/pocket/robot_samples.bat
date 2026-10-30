@echo off
cd /d "%~dp0"
set PYTHONUTF8=1
set PYTHONUNBUFFERED=1
echo ==== %DATE% %TIME% start > robot.log
.venv\Scripts\python.exe -u robot_samples.py >> robot.log 2>&1 && echo ==== ALL DONE >> robot.log || echo ==== FAILED >> robot.log
