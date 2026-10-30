"""Make EMO sound like a small child: raise pitch and formants after synthesis.

Uses Praat's "Change gender" (PSOLA pitch change + formant shift by resampling) through
praat-parselmouth. A child differs from an adult mostly in two numbers: a higher voice pitch
(F0 ~280-330 Hz vs ~180 Hz for the fine-tuned EMO) and a shorter vocal tract, i.e. formants
~15-20 % higher. A slightly wider pitch range makes it bouncier ("cuter").

Presets (F0 median Hz, formant ratio, pitch-range factor):
    light   260, 1.12, 1.20
    medium  300, 1.18, 1.35   default
    strong  340, 1.25, 1.50
"""
import numpy as np

PRESETS = {
    "off": None,
    "light": dict(pitch=260.0, formant=1.12, pitch_range=1.20),
    "medium": dict(pitch=300.0, formant=1.18, pitch_range=1.35),
    "strong": dict(pitch=340.0, formant=1.25, pitch_range=1.50),
}


def resolve(preset="medium", **overrides):
    """Preset name plus any explicit pitch/formant/pitch_range/duration overrides -> params or None."""
    base = PRESETS.get(preset or "off")
    if base is None and not any(v is not None for v in overrides.values()):
        return None
    p = dict(base or PRESETS["medium"])
    p.update({k: float(v) for k, v in overrides.items() if v is not None})
    return p


def childify(x, sr, pitch=300.0, formant=1.18, pitch_range=1.35, duration=1.0):
    """float32 mono -> float32 mono at the same sample rate and (with duration=1) length."""
    import parselmouth
    from parselmouth.praat import call
    x = np.asarray(x, dtype=np.float64)
    if len(x) < sr // 10:
        return x.astype(np.float32)
    peak = np.abs(x).max() or 1.0
    s = parselmouth.Sound(x, sampling_frequency=sr)
    o = call(s, "Change gender", 75, 600, formant, pitch, pitch_range, duration)
    y = o.values[0]
    y = y * (peak / max(np.abs(y).max(), 1e-6))   # keep the original loudness
    return y.astype(np.float32)
