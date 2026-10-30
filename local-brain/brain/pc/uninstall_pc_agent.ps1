# Removes EMO PC Agent (called from Settings > Apps, or run it directly).
$ErrorActionPreference = "Continue"
$dst = Join-Path $env:LOCALAPPDATA "Programs\EMO PC Agent"
Get-CimInstance Win32_Process -Filter "Name='pythonw.exe' OR Name='python.exe'" |
  Where-Object { $_.CommandLine -match "emo_pc_agent_app" } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
foreach ($lnk in @((Join-Path ([Environment]::GetFolderPath("Programs")) "EMO PC Agent.lnk"),
                   (Join-Path ([Environment]::GetFolderPath("Startup")) "EMO PC Agent.lnk"))) {
  Remove-Item $lnk -Force -ErrorAction SilentlyContinue
}
Remove-Item "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\EMOPCAgent" -Recurse -Force -ErrorAction SilentlyContinue
try {
  Start-Process cmd -ArgumentList '/c netsh advfirewall firewall delete rule name="EMO PC agent"' -Verb RunAs -Wait -WindowStyle Hidden
} catch {}
# the folder is removed by a separate process once this script has exited
Start-Process cmd -ArgumentList "/c timeout /t 2 /nobreak >nul & rmdir /s /q `"$dst`"" -WindowStyle Hidden
Write-Host "EMO PC Agent removed."
