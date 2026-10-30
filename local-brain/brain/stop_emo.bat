@echo off
rem Ends EMO's processes and frees their GPU memory: llama-server (Gemma), go-librespot (Spotify), and the
rem Python processes running emo_local.py / tts_server.py / the benchmarks. Other Python programs are left alone.
taskkill /IM llama-server.exe /F 2>nul
taskkill /IM go-librespot.exe /F 2>nul
powershell -NoProfile -Command "Get-CimInstance Win32_Process -Filter \"Name='python.exe'\" | Where-Object { $_.CommandLine -match 'emo_local\.py|tts_server\.py|pc_mcp\.py|check_fit\.py|bench_stt\.py' } | ForEach-Object { Write-Output ('ending ' + $_.ProcessId + ': ' + $_.CommandLine); Stop-Process -Id $_.ProcessId -Force }"
echo.
powershell -NoProfile -Command "$u = ((Get-Counter '\GPU Adapter Memory(*)\Dedicated Usage').CounterSamples | Measure-Object CookedValue -Maximum).Maximum; Write-Output ('GPU memory in use now: {0:N1} GB' -f ($u/1GB))"
pause
