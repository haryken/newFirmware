@echo off
rem Runs the installed EMO PC agent in this console instead of the tray (for debugging; quit the tray app first).
rem Options: --name "Living room PC"  --server http://<brain-pc-ip>:8798
set PYTHONUTF8=1
set APPDIR=%LOCALAPPDATA%\Programs\EMO PC Agent
"%APPDIR%\.venv\Scripts\python.exe" -u "%APPDIR%\pc_agent.py" %*
pause
