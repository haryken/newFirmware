"""Cute small-robot voice effect (desktop-pet style) for any TTS output.

Chain (each stage has a knob; 0 disables it):
  1. body      Praat "Change gender": pitch -> `pitch` Hz median, formants x`formant`
               (a tiny vocal tract), intonation range x`pitch_range`.
  2. quantize  snap the pitch contour toward semitones (0..1). 1 = stepped, synth-like
               melody; 0.5 = robotic tint that still sounds natural.
  3. tin       short feedback comb filter (`tin_ms` delay, `tin_fb` feedback, `tin_mix` wet):
               the metallic "small hollow shell" resonance.
  4. buzz      ring modulation at `buzz_hz`, mixed at `buzz_mix`: electronic flutter.
  5. crush     sample-and-hold to `crush_hz` + `crush_bits` quantisation, mixed at `crush_mix`.
  6. speaker   high-pass at `hp_hz` + presence boost `presence_db` around 3 kHz: toy speaker.
"""
import numpy as np

PRESETS = {
    "off": None,
    # gentle: clearly a voice, a little robotic sparkle
    "soft": dict(pitch=340, formant=1.20, pitch_range=1.0, quantize=0.5,
                 tin_ms=2.6, tin_fb=0.35, tin_mix=0.18, buzz_hz=55, buzz_mix=0.06,
                 crush_hz=16000, crush_bits=10, crush_mix=0.06, hp_hz=180, presence_db=3.0),
    # default: little desktop pet robot
    "pet": dict(pitch=380, formant=1.25, pitch_range=0.9, quantize=0.8,
                tin_ms=2.4, tin_fb=0.45, tin_mix=0.28, buzz_hz=60, buzz_mix=0.10,
                crush_hz=12000, crush_bits=8, crush_mix=0.10, hp_hz=220, presence_db=4.0),
    # strong: unmistakably a machine, still small and friendly
    "bot": dict(pitch=420, formant=1.30, pitch_range=0.7, quantize=1.0,
                tin_ms=2.2, tin_fb=0.55, tin_mix=0.38, buzz_hz=70, buzz_mix=0.18,
                crush_hz=10000, crush_bits=7, crush_mix=0.16, hp_hz=260, presence_db=5.0),
}


def _body(x, sr, pitch, formant, pitch_range, quantize):
    import parselmouth
    from parselmouth.praat import call
    s = parselmouth.Sound(x.astype(np.float64), sampling_frequency=sr)
    s = call(s, "Change gender", 75, 600, formant, pitch, pitch_range, 1.0)
    if quantize > 0:
        m = call(s, "To Manipulation", 0.01, 75, 900)
        tier = call(m, "Extract pitch tier")
        q = float(quantize)
        call(tier, "Formula", f"440 * 2^(((1-{q})*12*log2(self/440) + {q}*round(12*log2(self/440)))/12)")
        call([tier, m], "Replace pitch tier")
        s = call(m, "Get resynthesis (overlap-add)")
    return s.values[0].astype(np.float32)


def _tin(x, sr, ms, fb, mix):
    from scipy.signal import lfilter
    d = max(1, int(sr * ms / 1000))
    a = np.zeros(d + 1); a[0] = 1.0; a[d] = -fb
    wet = lfilter([1.0 - fb], a, x)
    return (1 - mix) * x + mix * wet


def _buzz(x, sr, hz, mix):
    t = np.arange(len(x)) / sr
    return (1 - mix) * x + mix * x * np.sin(2 * np.pi * hz * t) * 2.0


def _crush(x, sr, hz, bits, mix):
    step = max(1, int(round(sr / hz)))
    held = np.repeat(x[::step], step)[:len(x)]
    levels = 2 ** (bits - 1)
    crushed = np.round(held * levels) / levels
    return (1 - mix) * x + mix * crushed


def _speaker(x, sr, hp_hz, presence_db):
    from scipy.signal import butter, sosfilt, lfilter
    if hp_hz:
        x = sosfilt(butter(2, hp_hz, "highpass", fs=sr, output="sos"), x)
    if presence_db:   # RBJ peaking EQ at 3 kHz, Q 0.9
        A = 10 ** (presence_db / 40); w = 2 * np.pi * 3000 / sr; al = np.sin(w) / (2 * 0.9)
        b = [1 + al * A, -2 * np.cos(w), 1 - al * A]; a = [1 + al / A, -2 * np.cos(w), 1 - al / A]
        x = lfilter(np.array(b) / a[0], np.array(a) / a[0], x)
    return x


def resolve(preset="pet", **overrides):
    base = PRESETS.get(preset or "off")
    over = {k: v for k, v in overrides.items() if v is not None}
    if base is None and not over:
        return None
    p = dict(base or PRESETS["pet"])
    p.update({k: float(v) for k, v in over.items()})
    return p


def robotize(x, sr, pitch=380, formant=1.25, pitch_range=0.9, quantize=0.8, tin_ms=2.4, tin_fb=0.45,
             tin_mix=0.28, buzz_hz=60, buzz_mix=0.10, crush_hz=12000, crush_bits=8, crush_mix=0.10,
             hp_hz=220, presence_db=4.0):
    """float32 mono -> float32 mono, same rate and length, peak-normalised to 0.89."""
    x = np.asarray(x, dtype=np.float32)
    if len(x) < sr // 10:
        return x
    y = _body(x, sr, pitch, formant, pitch_range, quantize) if (pitch or formant != 1) else x.copy()
    y = y.astype(np.float64)
    if tin_mix > 0:
        y = _tin(y, sr, tin_ms, tin_fb, tin_mix)
    if buzz_mix > 0:
        y = _buzz(y, sr, buzz_hz, buzz_mix)
    if crush_mix > 0:
        y = _crush(y, sr, crush_hz, int(crush_bits), crush_mix)
    y = _speaker(y, sr, hp_hz, presence_db)
    y = y * (0.89 / max(np.abs(y).max(), 1e-6))   # consistent loudness (Pocket TTS output is quiet)
    return y.astype(np.float32)
