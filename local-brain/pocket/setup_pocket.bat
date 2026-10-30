@echo off
rem Pocket TTS (Kyutai, CPU-only) in its own venv: the main .venv has AMD's ROCm torch, whose
rem CPU path is ~100x too slow for this model. Renders all built-in English voices to voices\.
cd /d "%~dp0"
set PIP_NO_CACHE_DIR=1
set PYTHONUTF8=1
set PYTHONUNBUFFERED=1
set LOG=%~dp0pocket.log
echo ==== %DATE% %TIME% start > "%LOG%"
if not exist .venv\Scripts\python.exe (
  py -3.12 -m venv .venv >> "%LOG%" 2>&1 || goto :fail
  .venv\Scripts\python.exe -m pip install --upgrade pip >> "%LOG%" 2>&1
)
.venv\Scripts\python.exe -c "import pocket_tts" 2>nul || .venv\Scripts\python.exe -m pip install pocket-tts praat-parselmouth >> "%LOG%" 2>&1 || goto :fail
.venv\Scripts\python.exe -c "import torch; print('torch', torch.__version__, 'threads', torch.get_num_threads())" >> "%LOG%" 2>&1
.venv\Scripts\python.exe -u pocket_voices.py >> "%LOG%" 2>&1 || goto :fail
echo ==== ALL DONE >> "%LOG%"
goto :eof
:fail
echo ==== FAILED >> "%LOG%"
