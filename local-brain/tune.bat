@echo off
rem EMO voice: README steps 1 + 4 (desktop setup, XTTS fine-tune), then A/B samples. Safe to re-run: finished steps are skipped.
cd /d "%~dp0"
set PIP_NO_CACHE_DIR=1
set PYTHONUTF8=1
set PYTHONIOENCODING=utf-8
set PYTHONUNBUFFERED=1
set LOG=%~dp0tune.log
echo ==== %DATE% %TIME% start >> "%LOG%"
title EMO voice fine-tune - progress in tune.log

if not exist .venv\Scripts\python.exe (
  echo [1/5] creating venv
  echo ==== [1/5] venv >> "%LOG%"
  py -3.12 -m venv .venv >> "%LOG%" 2>&1 || goto :fail
  .venv\Scripts\python.exe -m pip install --upgrade pip >> "%LOG%" 2>&1
)
set PY=.venv\Scripts\python.exe

%PY% -c "import torch" 2>nul
if errorlevel 1 (
  echo [2/5] installing ROCm torch for gfx1201, several GB, takes a while
  echo ==== [2/5] torch >> "%LOG%"
  %PY% -m pip install --index-url https://stable.repo.amd.com/rocm/whl-next/ "torch[device-gfx1201]==2.14.0+rocm10.1.0" "torchvision[device-gfx1201]==0.29.0a0+rocm10.1.0" "torchaudio==2.11.0.3+rocm10.1.0" >> "%LOG%" 2>&1 || goto :fail
)

%PY% -c "import audio_compat, TTS, trainer" 2>nul
if errorlevel 1 (
  echo [3/5] installing coqui-tts
  echo ==== [3/5] requirements >> "%LOG%"
  %PY% -m pip install -r requirements-desktop.txt >> "%LOG%" 2>&1 || goto :fail
)

echo ==== GPU check >> "%LOG%"
%PY% -c "import torch; ok=torch.cuda.is_available(); print('torch', torch.__version__, 'cuda', ok, torch.cuda.get_device_name(0) if ok else ''); raise SystemExit(0 if ok else 1)" >> "%LOG%" 2>&1 || goto :fail

echo [4/5] fine-tuning XTTS on EMO's voice, 20-40 min
echo ==== [4/5] finetune >> "%LOG%"
%PY% -u finetune_xtts.py --dataset dataset --out run --epochs 12 >> "%LOG%" 2>&1 || goto :fail
echo [5/5] rendering A/B samples (stock vs fine-tuned) into samples\
echo ==== [5/5] samples >> "%LOG%"
%PY% -u make_samples.py >> "%LOG%" 2>&1 || goto :fail
echo ==== ALL DONE >> "%LOG%"
echo Done. See tune.log
goto :eof

:fail
echo ==== FAILED (errorlevel %ERRORLEVEL%) >> "%LOG%"
echo FAILED - see tune.log
