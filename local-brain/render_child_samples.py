"""Apply each voice_fx preset to samples/finetuned_*.wav -> samples/child/<preset>_<n>_<lang>.wav."""
import glob
import os
import wave

import numpy as np

import voice_fx

HERE = os.path.dirname(os.path.abspath(__file__))
out_dir = os.path.join(HERE, "samples", "child")
os.makedirs(out_dir, exist_ok=True)
for f in sorted(glob.glob(os.path.join(HERE, "samples", "finetuned_*.wav"))):
    with wave.open(f) as w:
        sr = w.getframerate()
        x = np.frombuffer(w.readframes(w.getnframes()), np.int16).astype(np.float32) / 32768
    for name in ("light", "medium", "strong"):
        y = voice_fx.childify(x, sr, **voice_fx.PRESETS[name])
        path = os.path.join(out_dir, f"{name}_" + os.path.basename(f).replace("finetuned_", ""))
        with wave.open(path, "wb") as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
            w.writeframes((np.clip(y, -1, 1) * 32767).astype(np.int16).tobytes())
        print(path)
