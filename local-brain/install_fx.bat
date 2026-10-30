@echo off
rem Installs praat-parselmouth (child-voice effect, voice_fx.py) into .venv and renders samples\child\.
cd /d "%~dp0"
set PIP_NO_CACHE_DIR=1
set LOG=%~dp0fx.log
echo ==== %DATE% %TIME% start > "%LOG%"
.venv\Scripts\python.exe -m pip install praat-parselmouth >> "%LOG%" 2>&1 || goto :fail
.venv\Scripts\python.exe -u render_child_samples.py >> "%LOG%" 2>&1 || goto :fail
echo ==== ALL DONE >> "%LOG%"
goto :eof
:fail
echo ==== FAILED >> "%LOG%"
