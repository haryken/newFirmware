@echo off
rem EMO's robot voice server: Pocket TTS (CPU) + robot_fx "pet" preset on port 8777.
rem Other options: --robot soft|pet|bot|off   --voices en=jane,it=giovanni   --quantize
cd /d "%~dp0.."
pocket\.venv\Scripts\python.exe tts_server.py --engine pocket %*
pause
