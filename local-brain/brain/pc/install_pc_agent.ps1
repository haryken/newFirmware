# Installs "EMO PC Agent" (tray app that lets EMO control this PC) for the current user:
#   %LOCALAPPDATA%\Programs\EMO PC Agent, Start-menu entry, start with Windows, listed in
#   Settings > Apps (uninstall there). Needs Python 3.10+. Re-run to update.
param([switch]$Quiet)
$ErrorActionPreference = "Continue"
$ProgressPreference = "SilentlyContinue"
$src = Split-Path -Parent $MyInvocation.MyCommand.Path
$dst = Join-Path $env:LOCALAPPDATA "Programs\EMO PC Agent"
$version = "1.0.0"
function Done($msg, $code) { Write-Host $msg; if (-not $Quiet) { Read-Host "Press Enter to close" | Out-Null }; exit $code }

Write-Host "Installing EMO PC Agent $version into $dst"

# ---- Python
$py = $null
foreach ($c in @("py|-3", "python")) {
  $parts = $c.Split("|")
  $pre = @($parts | Select-Object -Skip 1)
  try { $v = & $parts[0] @pre -c "import sys; print(sys.version_info >= (3, 10))" 2>$null } catch { $v = $null }
  if ($v -eq "True") { $py = $parts; break }
}
if (-not $py) { Done "Python 3.10+ not found: install it from python.org (tick 'Add python.exe to PATH') and run this again." 1 }

# ---- stop a running copy (update)
Get-CimInstance Win32_Process -Filter "Name='pythonw.exe' OR Name='python.exe'" |
  Where-Object { $_.CommandLine -match "emo_pc_agent_app|pc_agent\.py" } |
  ForEach-Object { Write-Host "stopping running agent (pid $($_.ProcessId))"; Stop-Process -Id $_.ProcessId -Force }

# ---- shared secret
$brainPc = Test-Path (Join-Path $src "..\emo_local.py")
$secret = Join-Path $src "pc_secret.txt"
if (-not (Test-Path $secret) -and $brainPc) {
  $pre = @($py | Select-Object -Skip 1)
  Push-Location $src; & $py[0] @pre -c "from pc_common import load_secret; load_secret(create=True)"; Pop-Location
  Write-Host "created pc_secret.txt (copy the pc folder with it to your other PCs)"
}
if (-not (Test-Path $secret) -and -not (Test-Path (Join-Path $dst "pc_secret.txt"))) {
  Done "pc_secret.txt is missing: copy it from the EMO brain PC (voice\brain\pc\pc_secret.txt) next to this installer and run it again." 1
}

# ---- files
New-Item -ItemType Directory -Force -Path $dst | Out-Null
foreach ($f in @("pc_agent.py", "pc_common.py", "emo_pc_agent_app.pyw", "emo.ico", "uninstall_pc_agent.ps1")) {
  Copy-Item (Join-Path $src $f) $dst -Force
}
if (Test-Path $secret) { Copy-Item $secret $dst -Force }

# ---- private Python environment + packages
$vpy = Join-Path $dst ".venv\Scripts\python.exe"
if (-not (Test-Path $vpy)) {
  Write-Host "creating the app's Python environment..."
  $pre = @($py | Select-Object -Skip 1)
  & $py[0] @pre -m venv (Join-Path $dst ".venv")
}
Write-Host "installing packages (pystray, pycaw, Pillow, websocket-client, winrt media)..."
& $vpy -m pip install -q --disable-pip-version-check --upgrade pip
& $vpy -m pip install -q --disable-pip-version-check pystray pillow pycaw comtypes websocket-client winrt-runtime winrt-Windows.Foundation winrt-Windows.Media.Control
if ($LASTEXITCODE) { Done "package installation failed (see above)." 1 }
$pyw = Join-Path $dst ".venv\Scripts\pythonw.exe"
$app = Join-Path $dst "emo_pc_agent_app.pyw"
$ico = Join-Path $dst "emo.ico"

# ---- shortcuts: Start menu + start with Windows
$ws = New-Object -ComObject WScript.Shell
$programs = [Environment]::GetFolderPath("Programs")
$startup = [Environment]::GetFolderPath("Startup")
foreach ($lnk in @((Join-Path $programs "EMO PC Agent.lnk"), (Join-Path $startup "EMO PC Agent.lnk"))) {
  $s = $ws.CreateShortcut($lnk)
  $s.TargetPath = $pyw
  $s.Arguments = "`"$app`""
  $s.WorkingDirectory = $dst
  $s.IconLocation = $ico
  $s.Description = "Lets EMO control this PC"
  $s.Save()
}

# ---- Settings > Apps entry
$key = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\EMOPCAgent"
New-Item -Path $key -Force | Out-Null
$size = [int]((Get-ChildItem $dst -Recurse -File | Measure-Object Length -Sum).Sum / 1KB)
$props = @{
  DisplayName = "EMO PC Agent"; DisplayVersion = $version; Publisher = "EMO (local)"; DisplayIcon = $ico
  InstallLocation = $dst; EstimatedSize = $size; NoModify = 1; NoRepair = 1
  UninstallString = "powershell.exe -NoProfile -ExecutionPolicy Bypass -File `"$(Join-Path $dst 'uninstall_pc_agent.ps1')`""
}
foreach ($k in $props.Keys) {
  $type = if ($props[$k] -is [int]) { "DWord" } else { "String" }
  New-ItemProperty -Path $key -Name $k -Value $props[$k] -PropertyType $type -Force | Out-Null
}

# ---- firewall (one admin prompt; private networks only)
$rules = 'netsh advfirewall firewall delete rule name="EMO PC agent" >nul 2>&1 & ' +
         'netsh advfirewall firewall add rule name="EMO PC agent" dir=in action=allow protocol=TCP localport=8765 profile=private'
if ($brainPc) {
  $rules += ' & netsh advfirewall firewall delete rule name="EMO PC announcements" >nul 2>&1' +
            ' & netsh advfirewall firewall add rule name="EMO PC announcements" dir=in action=allow protocol=UDP localport=8799 profile=private' +
            ' & netsh advfirewall firewall delete rule name="EMO PC announcements http" >nul 2>&1' +
            ' & netsh advfirewall firewall add rule name="EMO PC announcements http" dir=in action=allow protocol=TCP localport=8798 profile=private'
}
Write-Host "adding firewall rules - accept the Windows admin prompt (No = only this PC can reach the agent)"
try { Start-Process cmd -ArgumentList "/c $rules" -Verb RunAs -Wait -WindowStyle Hidden } catch { Write-Host "firewall rules skipped" }

# ---- start it
Start-Process -FilePath $pyw -ArgumentList "`"$app`"" -WorkingDirectory $dst
Done "EMO PC Agent is installed and running: look for EMO's face in the notification area (taskbar, near the clock)." 0
