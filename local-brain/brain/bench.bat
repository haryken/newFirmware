@echo off
rem EMO local brain benchmark: noisy test set (Pocket TTS, CPU) -> STT comparison -> fit/speed check.
rem Results: bench\results\summary.md, real.csv, fit.md. Log: bench.log
cd /d "%~dp0"
set PYTHONUTF8=1
set PYTHONUNBUFFERED=1
echo ==== %DATE% %TIME% start > bench.log
if not exist bench\testset\manifest.json (
  echo ==== make_testset >> bench.log
  ..\pocket\.venv\Scripts\python.exe -u bench\make_testset.py >> bench.log 2>&1 || goto :fail
)
echo ==== check_fit >> bench.log
..\.venv\Scripts\python.exe -u bench\check_fit.py >> bench.log 2>&1 || goto :fail
echo ==== bench_stt >> bench.log
..\.venv\Scripts\python.exe -u bench\bench_stt.py >> bench.log 2>&1 || goto :fail
echo ==== ALL DONE >> bench.log
goto :eof
:fail
echo ==== FAILED >> bench.log
