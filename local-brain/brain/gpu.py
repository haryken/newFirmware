"""GPU memory as Windows sees it.

torch.cuda.mem_get_info() on ROCm/Windows only accounts for memory allocated through HIP, so
llama.cpp's Vulkan buffers are invisible to it. The Windows performance counter
'GPU Adapter Memory(*)\\Dedicated Usage' counts every process and API, so it is used for decisions.
"""
import subprocess


def dedicated_used_gb():
    """VRAM in use on the busiest adapter (all processes), from the Windows counter; None if unavailable."""
    ps = ("$s = (Get-Counter '\\GPU Adapter Memory(*)\\Dedicated Usage' -ErrorAction Stop).CounterSamples; "
          "($s | Measure-Object CookedValue -Maximum).Maximum")
    try:
        out = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", ps],
                             capture_output=True, text=True, timeout=20).stdout.strip()
        return float(out.replace(",", ".")) / 2 ** 30
    except Exception:
        return None


def vram_gb():
    """(used, total, source) in GB for the GPU torch sees."""
    import torch
    total = torch.cuda.get_device_properties(0).total_memory / 2 ** 30
    used = dedicated_used_gb()
    if used is not None:
        return used, total, "windows"
    free, total_b = torch.cuda.mem_get_info()
    return (total_b - free) / 2 ** 30, total_b / 2 ** 30, "torch"
