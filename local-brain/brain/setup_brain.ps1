# EMO local brain: downloads llama.cpp (Vulkan build for the RX 9070 XT), Gemma 4 12B QAT GGUF,
# Qwen3-ASR (1.7B + 0.6B) and Whisper-large-v3-turbo (for comparison), and installs the
# Python extras into ..\.venv. Safe to re-run: finished downloads are skipped/resumed.
$ErrorActionPreference = "Continue"   # native tools write to stderr; failures are checked explicitly
$ProgressPreference = "SilentlyContinue"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$voice = Split-Path -Parent $here
$py = Join-Path $voice ".venv\Scripts\python.exe"
function Log($m) { Write-Output ("[{0}] {1}" -f (Get-Date -Format HH:mm:ss), $m) }

# ---- where the big files go: C: only if it has room, else the fixed drive with the most space
$paths = Join-Path $here "paths.json"
if (Test-Path $paths) {
  $models = (Get-Content $paths | ConvertFrom-Json).models
} else {
  $drives = Get-CimInstance Win32_LogicalDisk -Filter "DriveType=3" | Sort-Object FreeSpace -Descending
  foreach ($d in $drives) { Log ("drive {0} {1:N1} GB free of {2:N0} GB" -f $d.DeviceID, ($d.FreeSpace/1GB), ($d.Size/1GB)) }
  $c = $drives | Where-Object DeviceID -eq "C:"
  if ($c -and $c.FreeSpace -gt 30GB) { $models = Join-Path $here "models" }
  else { $models = "{0}\emo-models" -f $drives[0].DeviceID }
  @{ models = $models } | ConvertTo-Json | Set-Content $paths
}
New-Item -ItemType Directory -Force -Path $models | Out-Null
Log "models folder: $models"

# ---- llama.cpp, Vulkan build (works on RDNA4 without the ROCm SDK)
$llama = Join-Path $here "llama"
if (-not (Test-Path (Join-Path $llama "llama-server.exe"))) {
  $rels = Invoke-RestMethod "https://api.github.com/repos/ggml-org/llama.cpp/releases?per_page=8" -Headers @{ "User-Agent" = "emo-setup" }
  $asset = $null
  foreach ($r in $rels) {
    $asset = $r.assets | Where-Object { $_.name -match "win.*vulkan.*x64.*\.zip$" } | Select-Object -First 1
    if ($asset) { Log "llama.cpp release $($r.tag_name): $($asset.name)"; break }
    Log ("release {0} has no Vulkan Windows zip; assets: {1}" -f $r.tag_name, (($r.assets | ForEach-Object name) -join ", "))
  }
  if (-not $asset) { throw "no llama.cpp Windows Vulkan build found" }
  $zip = Join-Path $env:TEMP $asset.name
  & curl.exe -L --fail --retry 5 -s -S -o $zip $asset.browser_download_url
  if ($LASTEXITCODE) { throw "llama.cpp download failed" }
  Expand-Archive -Force $zip $llama
  Remove-Item $zip
  $exe = Get-ChildItem $llama -Recurse -Filter llama-server.exe | Select-Object -First 1
  if ($exe.DirectoryName -ne $llama) { Get-ChildItem $exe.DirectoryName | Move-Item -Destination $llama -Force }
}
Log ("llama-server: " + (Join-Path $llama "llama-server.exe"))
& (Join-Path $llama "llama-server.exe") --version 2>&1 | Select-Object -First 3 | ForEach-Object { Log $_ }
& (Join-Path $llama "llama-server.exe") --list-devices 2>&1 | Select-Object -First 8 | ForEach-Object { Log $_ }

# ---- Gemma 4 12B instruct, QAT 4-bit GGUF (Unsloth UD-Q4_K_XL, 6.7 GB)
$gguf = Join-Path $models "gemma-4-12B-it-qat-UD-Q4_K_XL.gguf"
if (-not (Test-Path $gguf) -or (Get-Item $gguf).Length -lt 6GB) {
  Log "downloading Gemma 4 12B QAT GGUF (6.7 GB)..."
  & curl.exe -L --fail --retry 10 --retry-delay 5 -C - -s -S -o $gguf "https://huggingface.co/unsloth/gemma-4-12B-it-qat-GGUF/resolve/main/gemma-4-12B-it-qat-UD-Q4_K_XL.gguf"
  if ($LASTEXITCODE) { throw "GGUF download failed ($LASTEXITCODE)" }
}
Log ("gguf {0:N2} GB" -f ((Get-Item $gguf).Length / 1GB))

# ---- Python extras + speech models (HF cache inside the models folder)
$env:HF_HOME = Join-Path $models "hf"
$env:PIP_NO_CACHE_DIR = "1"
& $py -m pip install silero-vad onnxruntime jiwer anthropic 2>&1 | Select-Object -Last 2 | ForEach-Object { Log $_ }
& $py -c @"
from huggingface_hub import snapshot_download
for rid in ('Qwen/Qwen3-ASR-1.7B-hf', 'Qwen/Qwen3-ASR-0.6B-hf', 'openai/whisper-large-v3-turbo'):
    p = snapshot_download(rid, allow_patterns=['*.json', '*.safetensors', '*.jinja', '*.txt', '*.model'])
    print('ok', rid, p, flush=True)
"@ 2>&1 | Where-Object { $_ -notmatch "it/s\]|B/s\]" } | ForEach-Object { Log $_ }
Get-CimInstance Win32_LogicalDisk -Filter "DriveType=3" | ForEach-Object { Log ("after: {0} {1:N1} GB free" -f $_.DeviceID, ($_.FreeSpace/1GB)) }
Log "SETUP DONE"
