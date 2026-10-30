# One-time setup of EMO's Spotify player (go-librespot, a Spotify Connect daemon; Premium account needed).
#   1. downloads the latest go-librespot Windows build into spotify\bin
#   2. writes spotify\config\config.yml: device "EMO", desktop speakers (WASAPI), API on localhost:3678
#   3. starts it once and logs you in with a pairing code (spotify.com/pair); credentials are kept in
#      spotify\config\state.json, so this is needed only once
$ErrorActionPreference = "Continue"
$ProgressPreference = "SilentlyContinue"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$bin = Join-Path $here "bin"
$cfgDir = Join-Path $here "config"
New-Item -ItemType Directory -Force -Path $bin, $cfgDir | Out-Null

# ---- 1. binary
$exe = Get-ChildItem $bin -Recurse -Filter "go-librespot*.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $exe) {
  $url = $null
  try {
    $rel = Invoke-RestMethod "https://api.github.com/repos/devgianlu/go-librespot/releases/latest" -Headers @{ "User-Agent" = "emo-setup" }
    $asset = $rel.assets | ForEach-Object { $_ } | Where-Object { $_.name -match "windows_amd64\.(tar\.gz|zip)$" } | Select-Object -First 1
    if ($asset) { $url = $asset.browser_download_url; Write-Host "go-librespot $($rel.tag_name): $($asset.name)" }
  } catch { Write-Host "GitHub API: $($_.Exception.Message)" }
  if (-not $url) {
    $url = "https://github.com/devgianlu/go-librespot/releases/download/v0.10.5/go-librespot_windows_amd64.tar.gz"
    Write-Host "using $url"
  }
  $arc = Join-Path $env:TEMP ([IO.Path]::GetFileName($url))
  & curl.exe -L --fail --retry 3 -s -S -o $arc $url
  if ($LASTEXITCODE) { Write-Host "download failed"; exit 1 }
  & tar.exe -xf $arc -C $bin
  Remove-Item $arc
  $exe = Get-ChildItem $bin -Recurse -Filter "go-librespot*.exe" | Select-Object -First 1
  if (-not $exe) { Write-Host "no go-librespot.exe in the archive:"; Get-ChildItem $bin -Recurse | ForEach-Object FullName; exit 1 }
}
Write-Host "player: $($exe.FullName)"

# ---- 1b. decoder DLLs: the Windows build links the MinGW (MSYS2 mingw64) libFLAC / libvorbis / libmpg123
#      DLLs but does not ship them. Taken from the official MSYS2 package repository, next to the exe.
$exeDir = Split-Path -Parent $exe.FullName
# no "DLL not found" message boxes from child processes: they fail with 0xC0000135 straight away
Add-Type -Namespace Emo -Name Err -MemberDefinition '[DllImport("kernel32.dll")] public static extern uint SetErrorMode(uint mode);'
[void][Emo.Err]::SetErrorMode(0x8003)
function Test-Player {
  $t = Start-Process -FilePath $exe.FullName -ArgumentList "-h" -PassThru -WindowStyle Hidden
  $null = $t.Handle                                # keeps ExitCode readable after exit
  if (-not $t.WaitForExit(10000)) { Stop-Process -Id $t.Id -Force; return $true }
  return $t.ExitCode -ne -1073741515              # 0xC0000135 = a DLL is missing
}
if (-not (Test-Player)) {
  Write-Host "fetching the decoder DLLs (MSYS2 mingw64 packages)..."
  $repo = "https://repo.msys2.org/mingw/mingw64/"
  $index = (Invoke-WebRequest -UseBasicParsing $repo).Content
  $pyExe = Join-Path $here "..\..\.venv\Scripts\python.exe"
  $tmp = Join-Path $env:TEMP "emo_msys2"
  Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
  New-Item -ItemType Directory -Force -Path $tmp | Out-Null
  foreach ($name in @("flac", "libvorbis", "libogg", "mpg123", "gcc-libs", "libwinpthread", "libwinpthread-git")) {
    $m = [regex]::Matches($index, "mingw-w64-x86_64-$([regex]::Escape($name))-\d[^`"<>]*?-any\.pkg\.tar\.(zst|xz)(?=`")")
    if ($m.Count -eq 0) { continue }
    $file = $m[$m.Count - 1].Value                  # the listing is sorted: last = newest
    Write-Host "  $file"
    $pkg = Join-Path $tmp $file
    & curl.exe -L --fail --retry 3 -s -S -o $pkg ($repo + $file)
    if ($LASTEXITCODE) { Write-Host "  download failed"; continue }
    $out = Join-Path $tmp ($name + "_x")
    New-Item -ItemType Directory -Force -Path $out | Out-Null
    & tar.exe -xf $pkg -C $out 2> $null
    if ($LASTEXITCODE -or -not (Test-Path (Join-Path $out "mingw64\bin"))) {
      # this Windows' tar cannot read .zst: unpack with Python + zstandard instead
      if (-not $script:zstdReady) { & $pyExe -m pip install -q zstandard; $script:zstdReady = $true }
      & $pyExe -c "import sys, tarfile, zstandard; f = open(sys.argv[1], 'rb'); r = zstandard.ZstdDecompressor().stream_reader(f) if sys.argv[1].endswith('.zst') else f; tarfile.open(fileobj=r, mode='r|*').extractall(sys.argv[2])" $pkg $out
    }
    Get-ChildItem (Join-Path $out "mingw64\bin") -Filter *.dll -ErrorAction SilentlyContinue |
      Copy-Item -Destination $exeDir -Force
  }
  Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
  if (-not (Test-Player)) {
    Write-Host "go-librespot still misses a DLL. Fallback: install MSYS2 (msys2.org), run in its shell"
    Write-Host "  pacman -S mingw-w64-x86_64-libvorbis mingw-w64-x86_64-flac mingw-w64-x86_64-mpg123"
    Write-Host "and copy C:\msys64\mingw64\bin\*.dll next to $($exe.FullName)"
    exit 1
  }
  Write-Host "decoder DLLs OK"
}

# ---- 2. config
$cfg = Join-Path $cfgDir "config.yml"
if (-not (Test-Path $cfg)) {
@"
# go-librespot for EMO - schema: https://github.com/devgianlu/go-librespot/blob/master/config_schema.json
device_name: EMO
device_type: speaker
audio_backend: wasapi      # Windows default playback device (the desktop speakers)
bitrate: 320
initial_volume: 60
volume_steps: 100
zeroconf_enabled: true     # also shows up as "EMO" in the Spotify app on this network
credentials:
  type: device_auth        # one-time pairing code at spotify.com/pair
server:
  enabled: true
  address: localhost
  port: 3678
"@ | Set-Content -Encoding ASCII $cfg
  Write-Host "wrote $cfg"
}

# ---- 3. log in once (the player's output goes to spotify\setup-player*.log)
$api = "http://localhost:3678"
$logOut = Join-Path $here "setup-player.log"
$logErr = Join-Path $here "setup-player.err.log"
function Run-Login {
  $p = Start-Process -FilePath $exe.FullName -ArgumentList @("--config_dir", "`"$cfgDir`"") -PassThru `
         -WindowStyle Hidden -RedirectStandardOutput $logOut -RedirectStandardError $logErr
  $null = $p.Handle
  Write-Host "starting the player..."
  $deadline = (Get-Date).AddMinutes(5)
  $shown = $false
  $ok = $false
  while ((Get-Date) -lt $deadline -and -not $p.HasExited) {
    Start-Sleep -Seconds 2
    try {
      $t = Invoke-WebRequest -UseBasicParsing -Method Post "$api/token" -TimeoutSec 3
      if ($t.StatusCode -eq 200) { $ok = $true; break }
    } catch {}
    if (-not $shown) {
      try {
        $a = Invoke-WebRequest -UseBasicParsing "$api/auth/code" -TimeoutSec 3
        if ($a.StatusCode -eq 200) {
          $j = $a.Content | ConvertFrom-Json
          Write-Host ""
          Write-Host "Log in to Spotify: open $($j.url)"
          Write-Host "(or go to spotify.com/pair and enter the code $($j.code))"
          Start-Process $j.url
          $shown = $true
          Write-Host "waiting for you to approve..."
        }
      } catch {}
    }
  }
  $self = $p.HasExited
  if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; Start-Sleep -Milliseconds 500 }
  $log = ((Get-Content $logOut, $logErr -ErrorAction SilentlyContinue) -join "`n")
  return @{ ok = $ok; code = $(if ($self -and -not $ok) { $p.ExitCode } else { $null }); log = $log }
}

$r = Run-Login
if (-not $r.ok -and $r.log -match "(?i)zeroconf|mdns|5353|multicast" -and (Get-Content $cfg -Raw) -match "zeroconf_enabled: true") {
  Write-Host "Zeroconf (Spotify Connect discovery) failed on this PC - turning it off and retrying"
  (Get-Content $cfg -Raw) -replace "zeroconf_enabled: true[^\r\n]*", "zeroconf_enabled: false" | Set-Content -Encoding ASCII $cfg
  $r = Run-Login
}
if ($r.ok) { Write-Host ""; Write-Host "Spotify login OK - EMO can play music now." }
else {
  if ($r.code -ne $null) { Write-Host "the player stopped (exit code $($r.code)). Its last messages:" }
  else { Write-Host "no login within 5 minutes. The player's last messages:" }
  ($r.log -split "`n" | Where-Object { $_ -match "\S" } | Select-Object -Last 15) | ForEach-Object { Write-Host "  $_" }
  Write-Host "(full output: $logOut / $logErr)"
}
