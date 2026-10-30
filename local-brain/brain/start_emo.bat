@echo off
rem EMO's local voice server: Qwen3-ASR + Gemma 4 12B on the GPU, Pocket TTS on the CPU,
rem Claude (with web search) as Gemma's helper when brain\anthropic_key.txt or ANTHROPIC_API_KEY is set.
rem Options, e.g.:  start_emo.bat --style 14 --city <your city> --country <ISO code> --claude-model claude-haiku-5-5
cd /d "%~dp0"
set PYTHONUTF8=1
set PYTHONUNBUFFERED=1
..\.venv\Scripts\python.exe -c "import anthropic" 2>nul || ..\.venv\Scripts\python.exe -m pip install -q anthropic
..\.venv\Scripts\python.exe -c "import mcp.server.fastmcp" 2>nul || ..\.venv\Scripts\python.exe -m pip install -q "mcp<2"
..\.venv\Scripts\python.exe -u emo_local.py %*
pause
