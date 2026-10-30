# Pair EMO with your Philips Hue bridge(s) on the local network.
#   1. finds the bridges (Philips' discovery service, or the IPs you pass: hue_pair.bat 192.168.1.20 192.168.1.21)
#   2. asks you once to press the round link button on the bridge(s) you want, then polls all of
#      them together for 60 s and keeps every bridge that hands out a key
#   3. writes one line per bridge to hue_key.txt:  <ip> <application key> <bridge id> <name>
# Keep hue_key.txt private: the key controls your lights without a password.
param([string[]]$Ips)
$ErrorActionPreference = "Continue"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$keyFile = Join-Path $here "hue_key.txt"

# ---- bridge IPs (flattened: Windows PowerShell returns a JSON array as one object)
$found = @()
if ($Ips) {
  $found = @($Ips | ForEach-Object { $_ -split "[\s,]+" } | Where-Object { $_ })
} else {
  Write-Host "Looking for Hue bridges (discovery.meethue.com)..."
  try {
    $resp = Invoke-RestMethod -Uri "https://discovery.meethue.com/" -TimeoutSec 10
    $found = @($resp | ForEach-Object { $_ } | ForEach-Object { $_.internalipaddress } |
               ForEach-Object { $_ -split "\s+" } | Where-Object { $_ })
  } catch { Write-Host "Discovery failed: $($_.Exception.Message)" }
  if (-not $found) {
    $ip = Read-Host "No bridge found automatically. Bridge IP address(es), comma separated (Hue app: Settings > Bridges > i)"
    $found = @($ip -split "[\s,]+" | Where-Object { $_ })
  }
}
$found = @($found | Select-Object -Unique)
if (-not $found) { Write-Host "No bridge, nothing to do."; exit 1 }

$existing = [ordered]@{}
if (Test-Path $keyFile) {
  Get-Content $keyFile | Where-Object { $_ -match "\S" } | ForEach-Object { $p = $_ -split "\s+"; $existing[$p[0]] = $_ }
}

# ---- which bridges answer, and which still need a key
$todo = @{}
foreach ($ip in $found) {
  try { $cfg = Invoke-RestMethod -Uri "http://$ip/api/config" -TimeoutSec 5 } catch {
    Write-Host "[$ip] not reachable: $($_.Exception.Message)"; continue
  }
  $id = $cfg.bridgeid
  Write-Host "[$ip] bridge '$($cfg.name)' (id $id)" -NoNewline
  if ($existing.Contains($ip)) { Write-Host " - already paired"; continue }
  Write-Host ""
  $todo[$ip] = @{ name = $cfg.name; id = $id }
}
if ($todo.Count -eq 0) { Write-Host "Nothing new to pair. (Delete a line from hue_key.txt to pair that bridge again.)"; exit 0 }

Write-Host ""
Read-Host "Press the round link button on the bridge(s) you want EMO to use, then press Enter (60 seconds)"
$body = @{ devicetype = "emo_robot#desktop"; generateclientkey = $true } | ConvertTo-Json
$deadline = (Get-Date).AddSeconds(60)
while ($todo.Count -gt 0 -and (Get-Date) -lt $deadline) {
  foreach ($ip in @($todo.Keys)) {
    try {
      $r = @(Invoke-RestMethod -Method Post -Uri "http://$ip/api" -Body $body -ContentType "application/json" -TimeoutSec 5)
      $first = $r | ForEach-Object { $_ } | Select-Object -First 1
      if ($first.success) {
        $b = $todo[$ip]
        $existing[$ip] = "$ip $($first.success.username) $($b.id) $($b.name -replace '\s+', '_')"
        Write-Host "[$ip] paired: '$($b.name)'"
        $todo.Remove($ip)
      } elseif ($first.error -and $first.error.type -ne 101) {
        Write-Host "[$ip] error: $($first.error.description)"; $todo.Remove($ip)
      }
    } catch { Write-Host "[$ip] $($_.Exception.Message)" }
  }
  if ($todo.Count -gt 0) { Start-Sleep -Seconds 1 }
}
foreach ($ip in $todo.Keys) { Write-Host "[$ip] not paired (its link button was not pressed)" }

@($existing.Values) | Set-Content -Encoding ASCII $keyFile
Write-Host ""
Write-Host "Saved $($existing.Count) bridge(s) to $keyFile"
