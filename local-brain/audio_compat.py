"""Audio I/O shim so coqui-tts runs without torchcodec (Windows + ROCm torch >= 2.9).

coqui-tts refuses to import on torch >= 2.9 unless torchcodec is installed, because
torchaudio.load now goes through torchcodec. Windows torchcodec wheels need FFmpeg shared
DLLs and must match the torch ABI exactly, which is awkward with AMD's ROCm build. XTTS
only ever calls torchaudio.load (dataset + speaker reference), so this module:
  * replaces torchaudio.load with a soundfile-based loader (same return shape), and
  * imports the TTS package with a placeholder `torchcodec` module in place so coqui's
    one-time presence check passes, then removes the placeholder again (transformers
    also probes torchcodec and must see that it is absent).
Import it before anything from TTS. If real torchcodec is installed it does nothing.
"""
import importlib.machinery
import importlib.util
import sys
import types

if importlib.util.find_spec("torchcodec") is None:
    import soundfile as sf
    import torch
    import torchaudio

    def _load(uri, frame_offset=0, num_frames=-1, normalize=True, channels_first=True, *args, **kwargs):
        x, sr = sf.read(str(uri), dtype="float32", always_2d=True,
                        start=frame_offset, frames=num_frames if num_frames and num_frames > 0 else -1)
        t = torch.from_numpy(x.T.copy() if channels_first else x.copy())
        return t, sr

    torchaudio.load = _load
    stub = types.ModuleType("torchcodec")
    stub.__spec__ = importlib.machinery.ModuleSpec("torchcodec", None)
    sys.modules["torchcodec"] = stub
    try:
        import TTS  # noqa: F401  (runs coqui's check while the placeholder is present)
    finally:
        del sys.modules["torchcodec"]
