"""97 "cute little desktop robot" voice styles for Pocket TTS output.

Each style = a built-in Pocket TTS voice + a chain of effects. Render them all with
    python robot_styles.py <folder with pocket voice wavs> <out folder>
or use one in the server:  tts_server.py --engine pocket --style 07
(the style's voice becomes the English voice unless --voices says otherwise).
"""
import numpy as np

# --------------------------------------------------------------------------- #
# building blocks: f(x: float64 mono, sr, **kw) -> float64 mono
# --------------------------------------------------------------------------- #


def _snd(x, sr):
    import parselmouth
    return parselmouth.Sound(np.asarray(x, dtype=np.float64), sampling_frequency=sr)


def body(x, sr, pitch=350, formant=1.2, pitch_range=1.0, duration=1.0):
    """Praat Change gender: target median pitch (Hz), formant ratio, intonation range, tempo."""
    from parselmouth.praat import call
    return call(_snd(x, sr), "Change gender", 75, 600, formant, pitch, pitch_range, duration).values[0]


def _pitch_formula(x, sr, formula):
    from parselmouth.praat import call
    m = call(_snd(x, sr), "To Manipulation", 0.01, 75, 900)
    tier = call(m, "Extract pitch tier")
    call(tier, "Formula", formula)
    call([tier, m], "Replace pitch tier")
    return call(m, "Get resynthesis (overlap-add)").values[0]


def quantize(x, sr, amount=1.0):
    """Snap intonation toward semitones: stepped, synth-like melody."""
    q = float(amount)
    return _pitch_formula(x, sr, f"440 * 2^(((1-{q})*12*log2(self/440) + {q}*round(12*log2(self/440)))/12)")


def mono(x, sr, pitch=330):
    """One fixed note, Speak & Spell style."""
    return _pitch_formula(x, sr, f"{float(pitch)}")


def vibrato(x, sr, rate=7.0, depth=0.6):
    """Pitch wobble: `depth` semitones at `rate` Hz."""
    return _pitch_formula(x, sr, f"self * 2^({depth} * sin(2*pi*{rate}*x) / 12)")


def chipmunk(x, sr, factor=1.35):
    """Play faster: pitch, formants and tempo all up by `factor` (tape-speed trick)."""
    from scipy.signal import resample_poly
    up, down = 100, int(round(100 * factor))
    return resample_poly(x, up, down)


def harmonize(x, sr, intervals=(12,), mix=0.3):
    """Add pitch-shifted copies (semitones), formants kept: chirpy layered voice."""
    import parselmouth  # noqa: F401
    from parselmouth.praat import call
    s = _snd(x, sr)
    f0 = s.to_pitch(pitch_floor=75, pitch_ceiling=900).selected_array["frequency"]
    med = float(np.median(f0[f0 > 0])) if np.any(f0 > 0) else 300.0
    y = np.array(x, dtype=np.float64)
    for st in intervals:
        layer = call(s, "Change gender", 75, 900, 1.0, med * 2 ** (st / 12), 1.0, 1.0).values[0]
        n = min(len(y), len(layer))
        y[:n] += mix * layer[:n]
    return y


def chorus(x, sr, delay_ms=18, depth_ms=3, rate=0.8, mix=0.5):
    """Modulated delay: a doubled, slightly detuned voice."""
    n = np.arange(len(x))
    d = (delay_ms + depth_ms * np.sin(2 * np.pi * rate * n / sr)) * sr / 1000
    wet = np.interp(n - d, n, x, left=0.0)
    return (1 - mix * 0.5) * x + mix * wet


def tin(x, sr, ms=2.4, fb=0.45, mix=0.3):
    """Short feedback comb: metallic, hollow little shell."""
    from scipy.signal import lfilter
    k = max(1, int(sr * ms / 1000))
    a = np.zeros(k + 1); a[0] = 1.0; a[k] = -fb
    return (1 - mix) * x + mix * lfilter([1.0 - fb], a, x)


def ring(x, sr, hz=60, mix=0.1):
    """Ring modulation: buzz (low hz) or glassy shimmer (high hz)."""
    t = np.arange(len(x)) / sr
    return (1 - mix) * x + mix * 2.0 * x * np.sin(2 * np.pi * hz * t)


def crush(x, sr, hz=12000, bits=8, mix=0.1):
    """Sample-and-hold + bit reduction: digital grit / 8-bit console."""
    step = max(1, int(round(sr / hz)))
    held = np.repeat(x[::step], step)[:len(x)]
    lv = 2 ** (int(bits) - 1)
    return (1 - mix) * x + mix * np.round(held * lv) / lv


def gate(x, sr, rate=12, depth=0.5):
    """Square-wave amplitude chop: glitchy, stuttering circuit."""
    t = np.arange(len(x)) / sr
    sq = (np.sin(2 * np.pi * rate * t) > 0).astype(float)
    from scipy.signal import lfilter
    sq = lfilter([0.02], [1, -0.98], sq) / 1.0           # soften the edges (no clicks)
    sq = sq / max(sq.max(), 1e-6)
    return x * (1 - depth + depth * sq)


def drive(x, sr, amount=2.0):
    """Soft saturation: a tiny overdriven speaker."""
    return np.tanh(amount * x / max(np.abs(x).max(), 1e-6))


def band(x, sr, lo=None, hi=None, order=2):
    """High-pass `lo`, low-pass `hi`, or band-pass with both."""
    from scipy.signal import butter, sosfilt
    if lo and hi:
        return sosfilt(butter(order, [lo, hi], "bandpass", fs=sr, output="sos"), x)
    if lo:
        return sosfilt(butter(order, lo, "highpass", fs=sr, output="sos"), x)
    if hi:
        return sosfilt(butter(order, hi, "lowpass", fs=sr, output="sos"), x)
    return x


def presence(x, sr, db=4.0, hz=3000, q=0.9):
    """Peaking EQ (RBJ)."""
    from scipy.signal import lfilter
    A = 10 ** (db / 40); w = 2 * np.pi * hz / sr; al = np.sin(w) / (2 * q)
    b = np.array([1 + al * A, -2 * np.cos(w), 1 - al * A]); a = np.array([1 + al / A, -2 * np.cos(w), 1 - al / A])
    return lfilter(b / a[0], a / a[0], x)


def vocoder(x, sr, mix=0.8, carrier="saw", fixed=None, quant=1.0, transpose=0, nfft=1024, lifter=28, noise=0.3):
    """Channel-vocoder-like cross-synthesis: a synth (saw/square/pulse) following the voice's pitch
    (snapped to semitones by `quant`, or a `fixed` Hz note) shaped by the voice's spectral envelope."""
    from scipy.signal import istft, stft
    hop = nfft // 4
    s = _snd(x, sr)
    p = s.to_pitch(time_step=hop / sr, pitch_floor=75, pitch_ceiling=900)
    f0 = p.selected_array["frequency"]; tt = p.xs()
    n = np.arange(len(x)) / sr
    voiced = np.interp(n, tt, (f0 > 0).astype(float), left=0, right=0)
    f = np.where(f0 > 0, f0, np.nan)
    good = ~np.isnan(f)
    f = np.interp(np.arange(len(f)), np.flatnonzero(good), f[good]) if good.any() else np.full(len(f), 300.0)
    if fixed:
        f = np.full_like(f, float(fixed))
    else:
        c = 12 * np.log2(f / 440) + transpose
        c = (1 - quant) * c + quant * np.round(c)
        f = 440 * 2 ** (c / 12)
    fs = np.interp(n, tt, f)
    phase = np.cumsum(fs / sr)
    car = np.zeros(len(x))
    rng = np.random.default_rng(0)
    kmax = 1 if carrier == "sine" else int(min(40, (sr / 2 - 500) // max(fs.max(), 1)))
    if carrier == "noise":                                         # whisper: breath instead of a tone
        car = rng.standard_normal(len(x))
        kmax = 0
    for k in range(1, kmax + 1):
        if carrier in ("square", "triangle") and k % 2 == 0:
            continue
        if carrier == "triangle":
            amp = (-1) ** ((k - 1) // 2) / k ** 2
        else:
            amp = 1.0 / k if carrier in ("saw", "square") else 1.0  # "pulse": flat harmonics
        car += amp * np.sin(2 * np.pi * k * phase)
    if carrier == "noise":
        car = car / max(np.abs(car).max(), 1e-6)
        y = _cross(x, car, sr, nfft, lifter)
        return (1 - mix) * np.asarray(x) + mix * y
    car = voiced * car / max(np.abs(car).max(), 1e-6) + (1 - voiced * (1 - noise)) * 0.3 * rng.standard_normal(len(x))
    y = _cross(x, car, sr, nfft, lifter)
    return (1 - mix) * np.asarray(x) + mix * y


def _cross(x, car, sr, nfft=1024, lifter=28):
    """Impose x's spectral envelope (cepstrally smoothed) on carrier `car` (same length)."""
    from scipy.signal import istft, stft
    hop = nfft // 4

    def env(Z):
        logm = np.log(np.abs(Z) + 1e-7)
        cep = np.fft.irfft(logm, axis=0)
        cep[lifter:-lifter] = 0
        return np.exp(np.fft.rfft(cep, axis=0).real[: Z.shape[0]])

    _, _, X = stft(x, sr, nperseg=nfft, noverlap=nfft - hop)
    _, _, C = stft(car, sr, nperseg=nfft, noverlap=nfft - hop)
    Y = C / env(C) * env(X)
    _, y = istft(Y, sr, nperseg=nfft, noverlap=nfft - hop)
    y = y[: len(x)]
    if len(y) < len(x):
        y = np.pad(y, (0, len(x) - len(y)))
    return y / max(np.abs(y).max(), 1e-6) * np.abs(x).max()


def _envelope(x, sr, ms=12):
    from scipy.signal import lfilter
    a = np.exp(-1.0 / (sr * ms / 1000))
    return lfilter([1 - a], [1, -a], np.abs(x))


def varispeed(x, sr, factor=1.6, wander=2.0, rate=2.5, rise=3.0, seed=1):
    """Sampler/tape playback whose speed is automated: pitch, size and tempo move together.
    `factor` base speed, `wander` semitones of smooth random drift at ~`rate` Hz, `rise` semitones
    of upward bend over the last 15 % (a question-like lift)."""
    rng = np.random.default_rng(seed)
    n = len(x)
    knots = max(4, int(n / sr * rate) + 2)
    curve = np.interp(np.linspace(0, knots - 1, n), np.arange(knots), rng.uniform(-1, 1, knots)) * wander
    tail = np.clip((np.arange(n) / n - 0.85) / 0.15, 0, 1)
    curve += rise * tail ** 2
    speed = factor * 2 ** (curve / 12)
    pos = np.cumsum(speed)
    pos = pos[pos < n - 1]
    return np.interp(pos, np.arange(n), x)


def smooth_pitch(x, sr, semitones=2.0):
    """Praat pitch stylization: replaces the natural pitch micro-movement with straight glides
    ("pitch modulation suppressed")."""
    from parselmouth.praat import call
    m = call(_snd(x, sr), "To Manipulation", 0.01, 75, 900)
    tier = call(m, "Extract pitch tier")
    call(tier, "Stylize", float(semitones), "Semitones")
    call([tier, m], "Replace pitch tier")
    return call(m, "Get resynthesis (overlap-add)").values[0]


def lpc(x, sr, rate=8000, order=10, frame_ms=25, pitch_scale=1.0, quant=0.0, noise_mix=0.0, bits=8, jitter=0.0):
    """Linear-predictive (LPC) resynthesis, the way 1970s-80s speech chips (TI's LPC-10 Speak & Spell)
    stored speech: per frame, an all-pole filter of `order` + a pitch-pulse or noise source,
    at `rate` Hz with `bits`-bit output. pitch_scale/quant reshape the melody; noise_mix adds hiss
    to voiced frames; jitter randomises frame pitch (imperfect, "broken" resynthesis)."""
    from scipy.linalg import solve_toeplitz
    from scipy.signal import lfilter, resample_poly
    from math import gcd
    g = gcd(int(sr), int(rate))
    y8 = resample_poly(x, rate // g, sr // g)
    N = int(rate * frame_ms / 1000)
    s = _snd(y8, rate)
    p = s.to_pitch(time_step=frame_ms / 1000, pitch_floor=75, pitch_ceiling=600)
    f0 = p.selected_array["frequency"]; tt = p.xs()
    rng = np.random.default_rng(2)
    pre = lfilter([1, -0.9], [1], y8)
    out = np.zeros(len(y8)); zi = np.zeros(order); phase = 0.0
    win = np.hanning(2 * N)
    for i in range(0, len(y8) - 2 * N, N):
        seg = pre[i:i + 2 * N] * win
        r = np.correlate(seg, seg, "full")[2 * N - 1:2 * N + order]
        if r[0] < 1e-9:
            out[i:i + N] = 0; continue
        r[0] *= 1.0001
        a = solve_toeplitz(r[:order], -r[1:order + 1])
        err = r[0] + np.dot(a, r[1:order + 1])
        gain = np.sqrt(max(err, 1e-12) / (2 * N))
        t = (i + N) / rate
        f = np.interp(t, tt, f0, left=0, right=0)
        if f > 0:
            c = 12 * np.log2(f * pitch_scale / 440)
            c = (1 - quant) * c + quant * np.round(c) + jitter * rng.standard_normal()
            f = 440 * 2 ** (c / 12)
            ex = np.zeros(N)
            for k in range(N):
                phase += f / rate
                if phase >= 1:
                    phase -= 1; ex[k] = np.sqrt(rate / f)
            if noise_mix:
                ex = (1 - noise_mix) * ex + noise_mix * rng.standard_normal(N)
        else:
            ex = rng.standard_normal(N)
        seg_out, zi = lfilter([gain], np.r_[1, a], ex, zi=zi)
        out[i:i + N] = seg_out
    out = lfilter([1], [1, -0.9], out)                               # de-emphasis
    out = out / max(np.abs(out).max(), 1e-6)
    lv = 2 ** (bits - 1); out = np.round(out * lv) / lv
    y = resample_poly(out, sr // g, rate // g)[: len(x)]
    return np.pad(y, (0, max(0, len(x) - len(y))))


def talkbox(x, sr, notes=(0, 7, 12, 4), step_ms=110, root=660, glide=0.35, mix=0.85, seed=3):
    """Talkbox: a synth's sound shaped by the voice's mouth (formant envelope). The synth plays quick
    gliding 'bloop' notes (semitones from `root`) instead of the voice's own melody."""
    rng = np.random.default_rng(seed)
    n = len(x); step = int(sr * step_ms / 1000)
    seq = np.repeat(rng.choice(notes, n // step + 2), step)[:n].astype(float)
    a = np.exp(-1.0 / (sr * glide * step_ms / 1000))
    from scipy.signal import lfilter
    seq = lfilter([1 - a], [1, -a], seq, zi=[a * seq[0]])[0]
    f = root * 2 ** (seq / 12)
    ph = np.cumsum(f / sr)
    car = sum(np.sin(2 * np.pi * k * ph) / k for k in range(1, int((sr / 2 - 500) // max(f.max(), 1)) + 1))
    env = _envelope(x, sr)
    car = car / max(np.abs(car).max(), 1e-6) * (env / max(env.max(), 1e-6) > 0.02)
    car += 0.05 * np.random.default_rng(seed).standard_normal(n)
    y = _cross(x, car, sr)
    return (1 - mix) * x + mix * y


def beeps(x, sr, lo=900, hi=2600, hold_ms=70, mix=0.75, voice_mix=0.35, seed=4):
    """Astromech-style layer: two oscillators whose pitches jump by sample-and-hold random values,
    ring-modulated together and gated by the voice's syllables, plus noise on unvoiced sounds;
    the (already squeaky) voice stays underneath at `voice_mix`."""
    rng = np.random.default_rng(seed)
    n = len(x); step = int(sr * hold_ms / 1000)
    sh1 = np.repeat(rng.uniform(np.log(lo), np.log(hi), n // step + 2), step)[:n]
    sh2 = np.repeat(rng.uniform(np.log(lo / 4), np.log(hi / 4), n // step + 2), step)[:n]
    from scipy.signal import lfilter
    a = np.exp(-1.0 / (sr * 0.004))
    sh1 = lfilter([1 - a], [1, -a], sh1, zi=[a * sh1[0]])[0]
    o1 = np.sin(2 * np.pi * np.cumsum(np.exp(sh1) / sr))
    o2 = np.sin(2 * np.pi * np.cumsum(np.exp(sh2) / sr))
    env = _envelope(x, sr, 8); env = env / max(env.max(), 1e-6)
    s = _snd(x, sr)
    p = s.to_pitch(time_step=0.01, pitch_floor=75, pitch_ceiling=900)
    voiced = np.interp(np.arange(n) / sr, p.xs(), (p.selected_array["frequency"] > 0).astype(float), left=0, right=0)
    tone = (0.65 * o1 + 0.35 * o1 * o2) * env * voiced
    hiss = band(rng.standard_normal(n), sr, lo=3000, hi=9000) * env * (1 - voiced) * 0.6
    layer = tone + hiss
    layer = layer / max(np.abs(layer).max(), 1e-6) * np.abs(x).max()
    return voice_mix * x + mix * layer


def reverb(x, sr, room=0.35, decay=0.3, mix=0.25, seed=5):
    """Small space (a helmet, a box, a room): convolution with a synthetic decaying-noise impulse
    response of `room` seconds whose level falls 60 dB in `decay` s. The tail is kept."""
    from scipy.signal import fftconvolve
    rng = np.random.default_rng(seed)
    n = int(sr * room)
    t = np.arange(n) / sr
    ir = rng.standard_normal(n) * np.exp(-t * 6.9 / decay)
    ir = band(ir, sr, lo=200, hi=7000)
    ir[: int(sr * 0.004)] = 0                                   # 4 ms pre-delay
    xp = np.pad(np.asarray(x, dtype=np.float64), (0, n))
    wet = fftconvolve(xp, ir)[: len(xp)]
    wet *= np.abs(x).max() / max(np.abs(wet).max(), 1e-6)
    return (1 - mix) * xp + mix * wet


def echo(x, sr, ms=140, fb=0.4, mix=0.35, taps=4):
    """Slapback / repeating echo."""
    d = int(sr * ms / 1000)
    y = np.pad(np.asarray(x, dtype=np.float64), (0, d * taps))
    out = y.copy()
    for k in range(1, taps + 1):
        out[k * d:] += mix * fb ** (k - 1) * y[: len(y) - k * d]
    return out


def flanger(x, sr, delay_ms=2.5, depth_ms=2.0, rate=0.4, mix=0.7, passes=2):
    """Very short swept delay mixed back in (twice for a stronger 'jet' resonance)."""
    n = np.arange(len(x))
    d = (delay_ms + depth_ms * (0.5 + 0.5 * np.sin(2 * np.pi * rate * n / sr))) * sr / 1000
    y = np.asarray(x, dtype=np.float64)
    for _ in range(passes):
        y = y + mix * np.interp(n - d, n, y, left=0.0)
    return y


def tremolo(x, sr, rate=8.0, depth=0.4):
    """Volume wobble."""
    t = np.arange(len(x)) / sr
    return x * (1 - depth * (0.5 + 0.5 * np.sin(2 * np.pi * rate * t)))


def stutter(x, sr, every_s=1.1, ms=80, repeats=2, seed=6):
    """Glitchy repeat of the start of some syllables ('b-b-buddy'): a short slice is played
    `repeats` extra times at onsets roughly every `every_s` seconds."""
    rng = np.random.default_rng(seed)
    env = _envelope(x, sr, 10)
    env = env / max(env.max(), 1e-6)
    on = np.flatnonzero((env[1:] > 0.25) & (env[:-1] <= 0.25))
    L = int(sr * ms / 1000)
    fade = np.ones(L); f = max(1, int(sr * 0.004)); fade[:f] = np.linspace(0, 1, f); fade[-f:] = np.linspace(1, 0, f)
    out, last, prev = [], -10.0, 0
    for o in on:
        if o / sr - last < every_s * rng.uniform(0.7, 1.3) or o + L > len(x):
            continue
        out.append(x[prev:o])
        for _ in range(repeats):
            out.append(x[o:o + L] * fade)
            out.append(np.zeros(int(sr * 0.012)))
        prev, last = o, o / sr
    out.append(x[prev:])
    return np.concatenate(out)


def flutter(x, sr, wow=0.3, wow_rate=0.6, flut=0.08, flut_rate=9.0, hiss=0.012, seed=7):
    """Old tape/cassette: slow pitch wow + fast flutter (in semitones), soft top end and hiss."""
    n = len(x)
    t = np.arange(n) / sr
    semis = wow * np.sin(2 * np.pi * wow_rate * t) + flut * np.sin(2 * np.pi * flut_rate * t)
    pos = np.cumsum(2 ** (semis / 12))
    pos = pos[pos < n - 1]
    y = band(np.interp(pos, np.arange(n), x), sr, lo=60, hi=6500)
    rng = np.random.default_rng(seed)
    return y + hiss * np.abs(x).max() * band(rng.standard_normal(len(y)), sr, lo=2000)


def static(x, sr, level=0.03, lo=800, hi=6000, crackle=0.0, seed=8):
    """Radio hiss (band-limited noise) and optional crackle pops under the voice."""
    rng = np.random.default_rng(seed)
    nz = band(rng.standard_normal(len(x)), sr, lo=lo, hi=hi)
    nz = nz / max(np.abs(nz).max(), 1e-6) * level * np.abs(x).max()
    if crackle:
        pops = (rng.random(len(x)) < crackle / sr * 40) * rng.standard_normal(len(x)) * 0.3 * np.abs(x).max()
        nz = nz + pops
    return x + nz


def hardclip(x, sr, gain=6.0):
    """Drive the input until the waveform squares off."""
    return np.clip(gain * x / max(np.abs(x).max(), 1e-6), -1, 1)


OPS = {f.__name__: f for f in (body, quantize, mono, vibrato, chipmunk, harmonize, chorus, tin, ring,
                               crush, gate, drive, band, presence, vocoder, varispeed, smooth_pitch, lpc,
                               talkbox, beeps, hardclip, reverb, echo, flanger, tremolo, stutter, flutter,
                               static)}

# --------------------------------------------------------------------------- #
# the 20 styles: id -> (name, built-in voice, chain)
# --------------------------------------------------------------------------- #
STYLES = {
    "01": ("Cozmo-ish: light touch", "azelma",
           [("body", dict(pitch=330, formant=1.15, pitch_range=1.1)), ("crush", dict(hz=16000, bits=9, mix=0.08)),
            ("band", dict(lo=300)), ("presence", dict(db=3))]),
    "02": ("Chipmunk critter", "eve",
           [("chipmunk", dict(factor=1.35)), ("band", dict(lo=250)), ("presence", dict(db=2))]),
    "03": ("Speak & Spell: one note, gritty", "azelma",
           [("body", dict(pitch=300, formant=1.15)), ("mono", dict(pitch=300)), ("crush", dict(hz=8000, bits=6, mix=0.3)),
            ("band", dict(lo=200, hi=3800))]),
    "04": ("Vocoder pet: synth follows the melody", "azelma",
           [("body", dict(pitch=340, formant=1.2, pitch_range=1.1)), ("vocoder", dict(mix=0.75, carrier="saw", quant=1.0)),
            ("band", dict(lo=200))]),
    "05": ("Vocoder monotone", "jane",
           [("body", dict(pitch=330, formant=1.2)), ("vocoder", dict(mix=0.85, carrier="saw", fixed=440)), ("band", dict(lo=200))]),
    "06": ("Sing-song autotune duo", "jane",
           [("body", dict(pitch=360, formant=1.2, pitch_range=1.3)), ("quantize", dict(amount=1.0)),
            ("chorus", dict(delay_ms=14, depth_ms=2, mix=0.4)), ("presence", dict(db=3))]),
    "07": ("Warble bot: wobbly pitch", "cosette",
           [("body", dict(pitch=380, formant=1.25)), ("vibrato", dict(rate=7, depth=0.7)), ("tin", dict(ms=2.0, fb=0.4, mix=0.25)),
            ("band", dict(lo=220))]),
    "08": ("Harmonized chirp: octave sparkle", "eve",
           [("body", dict(pitch=340, formant=1.2, pitch_range=1.2)), ("harmonize", dict(intervals=(12, 19), mix=0.22)),
            ("band", dict(lo=250)), ("presence", dict(db=3))]),
    "09": ("Tiny speaker", "anna",
           [("body", dict(pitch=320, formant=1.22, pitch_range=1.1)), ("band", dict(lo=500, hi=4500, order=3)),
            ("drive", dict(amount=1.8))]),
    "10": ("Glass shell: high ring shimmer", "azelma",
           [("body", dict(pitch=380, formant=1.25)), ("ring", dict(hz=600, mix=0.18)), ("tin", dict(ms=1.4, fb=0.5, mix=0.3)),
            ("band", dict(lo=250))]),
    "11": ("Retro 8-bit console", "jane",
           [("body", dict(pitch=400, formant=1.2)), ("quantize", dict(amount=0.8)), ("crush", dict(hz=8000, bits=5, mix=0.5)),
            ("band", dict(lo=200))]),
    "12": ("Bubbly & fast", "eve",
           [("body", dict(pitch=370, formant=1.2, pitch_range=1.4, duration=0.85)), ("band", dict(lo=200)),
            ("presence", dict(db=3))]),
    "13": ("Sleepy soft robot", "fantine",
           [("body", dict(pitch=300, formant=1.15, pitch_range=0.6, duration=1.1)), ("quantize", dict(amount=0.6)),
            ("tin", dict(ms=3.0, fb=0.3, mix=0.2)), ("band", dict(lo=150, hi=5000))]),
    "14": ("Squeaky toy", "caro_davy",
           [("chipmunk", dict(factor=1.5)), ("band", dict(lo=300)), ("presence", dict(db=2))]),
    "15": ("Robot duet: doubled & stepped", "azelma",
           [("body", dict(pitch=360, formant=1.22)), ("quantize", dict(amount=0.7)),
            ("chorus", dict(delay_ms=20, depth_ms=4, rate=1.2, mix=0.6)), ("ring", dict(hz=45, mix=0.08))]),
    "16": ("Metallic small", "mary",
           [("body", dict(pitch=360, formant=1.3)), ("tin", dict(ms=1.2, fb=0.6, mix=0.4)), ("band", dict(lo=250))]),
    "17": ("Vocoder square + voice", "cosette",
           [("body", dict(pitch=350, formant=1.2, pitch_range=1.2)), ("vocoder", dict(mix=0.5, carrier="square", quant=0.8)),
            ("band", dict(lo=200))]),
    "18": ("Monotone high + buzz", "vera",
           [("body", dict(pitch=420, formant=1.25)), ("mono", dict(pitch=420)), ("ring", dict(hz=40, mix=0.12)),
            ("band", dict(lo=250))]),
    "19": ("Just small & cute (no robot fx)", "azelma",
           [("body", dict(pitch=300, formant=1.3, pitch_range=1.3)), ("band", dict(lo=150))]),
    "20": ("Glitch bot: chopped & stepped", "jane",
           [("body", dict(pitch=370, formant=1.2)), ("quantize", dict(amount=1.0)), ("gate", dict(rate=11, depth=0.35)),
            ("crush", dict(hz=11000, bits=7, mix=0.15)), ("band", dict(lo=200))]),
}


# Round 2: variations on the user's picks 06 (autotune duo), 09 (tiny speaker), 14 (squeaky toy).
_DUO = [("quantize", dict(amount=1.0)), ("chorus", dict(delay_ms=14, depth_ms=2, mix=0.4)), ("presence", dict(db=3))]
_SPK = [("band", dict(lo=500, hi=4500, order=3)), ("drive", dict(amount=1.8))]
STYLES.update({
    "21": ("Autotune duo in a tiny speaker", "jane", [("body", dict(pitch=360, formant=1.2, pitch_range=1.3))] + _DUO + _SPK),
    "22": ("Squeaky toy in a tiny speaker", "caro_davy",
           [("chipmunk", dict(factor=1.5)), ("band", dict(lo=500, hi=5000, order=3)), ("drive", dict(amount=1.6))]),
    "23": ("Squeaky autotune duo", "jane", [("chipmunk", dict(factor=1.35))] + _DUO),
    "24": ("Higher autotune duo", "jane", [("body", dict(pitch=420, formant=1.28, pitch_range=1.4))] + _DUO),
    "25": ("Autotune duo: eve", "eve", [("body", dict(pitch=360, formant=1.2, pitch_range=1.3))] + _DUO),
    "26": ("Autotune duo: azelma", "azelma", [("body", dict(pitch=360, formant=1.2, pitch_range=1.3))] + _DUO),
    "27": ("Sing-song tiny speaker", "anna",
           [("body", dict(pitch=330, formant=1.22, pitch_range=1.3)), ("quantize", dict(amount=0.9))] + _SPK),
    "28": ("Even tinier speaker", "anna",
           [("body", dict(pitch=370, formant=1.3, pitch_range=1.1)), ("band", dict(lo=700, hi=5000, order=4)),
            ("drive", dict(amount=2.5))]),
    "29": ("Squeaky toy, a bit lower", "caro_davy", [("chipmunk", dict(factor=1.35)), ("band", dict(lo=300)), ("presence", dict(db=2))]),
    "30": ("Squeaky toy, extra squeak", "caro_davy", [("chipmunk", dict(factor=1.65)), ("band", dict(lo=300)), ("presence", dict(db=2))]),
    "31": ("Squeaky toy: mary", "mary", [("chipmunk", dict(factor=1.5)), ("band", dict(lo=300)), ("presence", dict(db=2))]),
    "32": ("Squeaky toy: anna", "anna", [("chipmunk", dict(factor=1.45)), ("band", dict(lo=300)), ("presence", dict(db=2))]),
    "33": ("Squeaky & stepped", "caro_davy", [("chipmunk", dict(factor=1.4)), ("quantize", dict(amount=1.0)), ("band", dict(lo=300))]),
    "34": ("Squeaky duo", "caro_davy",
           [("chipmunk", dict(factor=1.45)), ("chorus", dict(delay_ms=14, depth_ms=2, mix=0.5)), ("band", dict(lo=300))]),
    "35": ("All three picks in one", "jane",
           [("chipmunk", dict(factor=1.3)), ("quantize", dict(amount=1.0)), ("chorus", dict(delay_ms=14, depth_ms=2, mix=0.4)),
            ("band", dict(lo=500, hi=4500, order=3)), ("drive", dict(amount=1.6))]),
    "36": ("Harmony duo: a third above", "jane",
           [("body", dict(pitch=360, formant=1.2, pitch_range=1.3)), ("quantize", dict(amount=1.0)),
            ("harmonize", dict(intervals=(4,), mix=0.35)), ("presence", dict(db=3))]),
    "37": ("Walkie-talkie pet", "anna",
           [("body", dict(pitch=340, formant=1.25)), ("band", dict(lo=800, hi=3500, order=4)), ("drive", dict(amount=3.0)),
            ("crush", dict(hz=11000, bits=8, mix=0.15))]),
    "38": ("Music-box sing-song", "jane",
           [("body", dict(pitch=380, formant=1.22, pitch_range=1.3)), ("quantize", dict(amount=1.0)),
            ("vibrato", dict(rate=5, depth=0.2)), ("tin", dict(ms=1.8, fb=0.35, mix=0.2))]),
    "39": ("Bouncy tape", "eve", [("body", dict(pitch=300, formant=1.0, pitch_range=1.45)), ("chipmunk", dict(factor=1.3))]),
    "40": ("Fast tiny duo", "anna",
           [("body", dict(pitch=360, formant=1.25, pitch_range=1.3, duration=0.9)),
            ("chorus", dict(delay_ms=14, depth_ms=2, mix=0.45)), ("band", dict(lo=500, hi=4500, order=3))]),
})

# Sound-alikes of famous robots, built from how their voices were documented to be made.
# (Our approximations with a TTS voice; not the characters' actual recordings.)
ROBOTS = {
    "41": ("WALL-E", "Ben Burtt's own voice, resynthesized imperfectly (artifacts, noise) and re-performed with stretching and repitching"),
    "42": ("Cozmo", "the sound designer's own voice, sampled and played back with the pitch automated in a sampler"),
    "43": ("GLaDOS", "pitch constrained to notes, pitch modulation suppressed, formant moved up"),
    "44": ("Dalek", "a 30 Hz ring modulator on a deliberately clipped voice"),
    "45": ("R2-D2", "ARP 2600 oscillators and noise through a ring modulator, sample-and-hold pitch, layered with baby-like vocal sounds"),
    "46": ("BB-8", "performers' voices routed through a talkbox driven by a sound app"),
    "47": ("Speak & Spell", "LPC-10 speech synthesis on TI's TMS5100 chip"),
}
STYLES.update({
    "41": ("Inspired by WALL-E", "paul",
           [("body", dict(pitch=220, formant=1.25)),
            ("lpc", dict(rate=16000, order=18, frame_ms=20, pitch_scale=1.3, noise_mix=0.35, bits=10, jitter=0.35)),
            ("varispeed", dict(factor=1.12, wander=3.0, rate=1.5, rise=2.0)), ("band", dict(lo=200))]),
    "42": ("Inspired by Cozmo", "stuart_bell",
           [("varispeed", dict(factor=1.9, wander=3.0, rate=3.0, rise=4.0)), ("band", dict(lo=250)), ("presence", dict(db=3))]),
    "43": ("Inspired by GLaDOS", "jane",
           [("body", dict(pitch=220, formant=1.12, pitch_range=1.0)), ("smooth_pitch", dict(semitones=2.0)),
            ("quantize", dict(amount=1.0))]),
    "44": ("Inspired by the Daleks", "michael",
           [("hardclip", dict(gain=6.0)), ("ring", dict(hz=30, mix=1.0)), ("band", dict(lo=250, hi=4000))]),
    "45": ("Inspired by R2-D2", "eve", [("chipmunk", dict(factor=1.6)), ("beeps", dict(mix=0.8, voice_mix=0.35))]),
    "46": ("Inspired by BB-8", "azelma", [("talkbox", dict(root=700, mix=0.85)), ("band", dict(lo=250))]),
    "47": ("Inspired by Speak & Spell", "paul", [("lpc", dict(rate=8000, order=10, frame_ms=25, bits=8)), ("band", dict(hi=3800))]),
})


# Round 3 (50 more): 48-57 more of 28 (the voice in use), 58-77 new textures, 78-82 more robots
# (our impressions, not documented techniques), 83-97 mixes of the favourites.
_T28 = [("band", dict(lo=700, hi=5000, order=4)), ("drive", dict(amount=2.5))]


def _tiny(pitch=370, formant=1.3, pitch_range=1.1, duration=1.0):
    return [("body", dict(pitch=pitch, formant=formant, pitch_range=pitch_range, duration=duration))]


STYLES.update({
    # --- more "even tinier speaker" (28)
    "48": ("Tinier speaker: eve", "eve", _tiny() + _T28),
    "49": ("Tinier speaker: azelma", "azelma", _tiny() + _T28),
    "50": ("Tinier speaker: cosette", "cosette", _tiny() + _T28),
    "51": ("Tinier speaker: vera, higher", "vera", _tiny(pitch=410, formant=1.32) + _T28),
    "52": ("Tinier speaker, cleaner", "anna", _tiny() + [("band", dict(lo=600, hi=6000, order=3)), ("drive", dict(amount=1.4))]),
    "53": ("Tinier speaker, gently tuned", "anna", _tiny(pitch_range=1.25) + [("quantize", dict(amount=0.6))] + _T28),
    "54": ("Tinier speaker in a little box", "anna", _tiny() + _T28 + [("reverb", dict(room=0.15, decay=0.12, mix=0.3))]),
    "55": ("Tinier speaker, chattier", "anna", _tiny(duration=0.88, pitch_range=1.3) + _T28),
    "56": ("Pocket radio", "anna",
           _tiny() + [("band", dict(lo=600, hi=3200, order=4)), ("drive", dict(amount=2.2)),
                      ("static", dict(level=0.05, crackle=0.5))]),
    "57": ("Tinier speaker, tape-sped", "anna", [("chipmunk", dict(factor=1.18))] + _tiny(pitch=380, formant=1.15) + _T28),
    # --- new textures
    "58": ("Jet flanger bot", "azelma", _tiny(pitch=360, formant=1.22) + [("flanger", dict(rate=0.35)), ("band", dict(lo=250))]),
    "59": ("Space helmet", "jane",
           _tiny(pitch=360, formant=1.22) + [("band", dict(lo=300, hi=6000)), ("reverb", dict(room=0.25, decay=0.18, mix=0.35)),
                                             ("presence", dict(db=4, hz=2500))]),
    "60": ("Echo pet", "eve", _tiny(pitch=360, formant=1.2) + [("echo", dict(ms=130, fb=0.35, mix=0.3, taps=3)), ("band", dict(lo=250))]),
    "61": ("Tremolo toy", "cosette", _tiny(pitch=380, formant=1.25) + [("tremolo", dict(rate=9, depth=0.45)), ("band", dict(lo=250))]),
    "62": ("Whisper bot", "azelma",
           _tiny(pitch=340, formant=1.2) + [("vocoder", dict(mix=0.75, carrier="noise")), ("band", dict(lo=400))]),
    "63": ("Theremin pet", "jane",
           _tiny(pitch=360, formant=1.2, pitch_range=1.4) + [("vocoder", dict(mix=0.55, carrier="sine", quant=0.0, transpose=12)),
                                                             ("vibrato", dict(rate=6, depth=0.4)), ("band", dict(lo=250))]),
    "64": ("Soft synth (triangle)", "azelma",
           _tiny(pitch=350, formant=1.2, pitch_range=1.2) + [("vocoder", dict(mix=0.7, carrier="triangle", quant=1.0)),
                                                             ("band", dict(lo=200))]),
    "65": ("Little voice, big shadow", "eve",
           _tiny(pitch=380, formant=1.25) + [("harmonize", dict(intervals=(-12,), mix=0.35)), ("band", dict(lo=120))]),
    "66": ("Tiny choir", "jane",
           _tiny(pitch=360, formant=1.22, pitch_range=1.3) + [("quantize", dict(amount=1.0)),
                                                              ("harmonize", dict(intervals=(7, 12), mix=0.25)),
                                                              ("reverb", dict(room=0.4, decay=0.35, mix=0.2))]),
    "67": ("Stutter glitch", "anna", _tiny(pitch=360, formant=1.25) + [("stutter", dict(every_s=0.9, ms=75, repeats=2)),
                                                                     ("crush", dict(hz=11000, bits=7, mix=0.15)), ("band", dict(lo=250))]),
    "68": ("Underwater bubble", "cosette",
           _tiny(pitch=370, formant=1.25) + [("vibrato", dict(rate=11, depth=0.5)), ("band", dict(lo=200, hi=2200, order=3)),
                                             ("reverb", dict(room=0.2, decay=0.15, mix=0.25))]),
    "69": ("Music box", "jane",
           _tiny(pitch=400, formant=1.25, pitch_range=1.3) + [("quantize", dict(amount=1.0)),
                                                              ("harmonize", dict(intervals=(12, 24), mix=0.18)),
                                                              ("tin", dict(ms=1.0, fb=0.5, mix=0.25)), ("band", dict(lo=300))]),
    "70": ("Toy keyboard", "azelma",
           _tiny(pitch=360, formant=1.2, pitch_range=1.3) + [("vocoder", dict(mix=0.55, carrier="square", quant=1.0)),
                                                             ("crush", dict(hz=11000, bits=6, mix=0.2)), ("band", dict(lo=300))]),
    "71": ("Kazoo buddy", "eve",
           _tiny(pitch=360, formant=1.2) + [("ring", dict(hz=90, mix=0.15)), ("band", dict(lo=700, hi=3500, order=3)),
                                            ("drive", dict(amount=3.0))]),
    "72": ("Hologram", "vera",
           _tiny(pitch=380, formant=1.25) + [("chorus", dict(delay_ms=12, depth_ms=3, rate=1.5, mix=0.5)),
                                             ("flanger", dict(rate=0.25, mix=0.4, passes=1)),
                                             ("reverb", dict(room=0.3, decay=0.25, mix=0.25)), ("band", dict(lo=300))]),
    "73": ("Old cassette pet", "anna", _tiny(pitch=350, formant=1.25) + [("flutter", dict(wow=0.35, hiss=0.015))]),
    "74": ("Game Boy", "jane",
           _tiny(pitch=390, formant=1.2) + [("quantize", dict(amount=1.0)), ("crush", dict(hz=6000, bits=4, mix=0.55)),
                                            ("band", dict(lo=300, hi=5000))]),
    "75": ("Arcade cabinet", "eve",
           _tiny(pitch=380, formant=1.2) + [("crush", dict(hz=8000, bits=6, mix=0.4)),
                                            ("reverb", dict(room=0.35, decay=0.3, mix=0.25)), ("band", dict(lo=250))]),
    "76": ("Own melody, tiny throat", "eve", [("body", dict(pitch=0, formant=1.35)), ("band", dict(lo=200)), ("presence", dict(db=3))]),
    "77": ("Sparkle dust", "azelma",
           _tiny(pitch=380, formant=1.25) + [("harmonize", dict(intervals=(24,), mix=0.15)), ("ring", dict(hz=1300, mix=0.1)),
                                             ("reverb", dict(room=0.3, decay=0.25, mix=0.15)), ("band", dict(lo=300))]),
    # --- more robots: our impressions (no documented technique)
    "78": ("Baymax-ish: calm & soft", "paul",
           [("body", dict(pitch=180, formant=1.08, pitch_range=0.5, duration=1.12)), ("smooth_pitch", dict(semitones=3.0)),
            ("vocoder", dict(mix=0.18, carrier="sine", quant=0.0)), ("band", dict(lo=120, hi=6000)),
            ("reverb", dict(room=0.2, decay=0.15, mix=0.15))]),
    "79": ("EVE-ish: airy & bright", "vera",
           [("body", dict(pitch=420, formant=1.25, pitch_range=1.2, duration=0.95)), ("smooth_pitch", dict(semitones=2.0)),
            ("vocoder", dict(mix=0.3, carrier="sine", transpose=12, quant=0.5)), ("band", dict(lo=400)),
            ("reverb", dict(room=0.3, decay=0.25, mix=0.2))]),
    "80": ("Furby-ish: furry gremlin", "caro_davy",
           [("chipmunk", dict(factor=1.45)), ("vibrato", dict(rate=9, depth=0.5)), ("drive", dict(amount=2.0)),
            ("band", dict(lo=400, hi=5000, order=3))]),
    "81": ("Johnny-5-ish: excited circuitry", "peter_yearsley",
           [("body", dict(pitch=260, formant=1.15, pitch_range=1.4, duration=0.9)), ("ring", dict(hz=55, mix=0.12)),
            ("crush", dict(hz=12000, bits=8, mix=0.12)), ("band", dict(lo=250))]),
    "82": ("Vector-ish: quick little bot", "stuart_bell",
           [("varispeed", dict(factor=1.55, wander=1.5, rate=3.0, rise=3.0)), ("crush", dict(hz=11000, bits=7, mix=0.2)),
            ("band", dict(lo=300)), ("presence", dict(db=3))]),
    # --- mixes of the favourites (06 duo, 09/28 speaker, 14 squeaky)
    "83": ("Autotune duo in the tinier speaker", "jane", _tiny(pitch=370, pitch_range=1.3) + _DUO + _T28),
    "84": ("Squeaky toy in the tinier speaker", "caro_davy", [("chipmunk", dict(factor=1.45))] + _T28),
    "85": ("Soft duo: eve", "eve", _tiny(pitch=360, formant=1.22, pitch_range=1.3)
           + [("quantize", dict(amount=0.8)), ("chorus", dict(delay_ms=16, depth_ms=2, mix=0.3)), ("presence", dict(db=2))]),
    "86": ("Third-harmony duo, tinier speaker", "jane", _tiny(pitch=370, pitch_range=1.3)
           + [("quantize", dict(amount=1.0)), ("harmonize", dict(intervals=(4,), mix=0.3))] + _T28),
    "87": ("Tinier speaker with a wobble", "anna", _tiny() + [("vibrato", dict(rate=6, depth=0.35))] + _T28),
    "88": ("Squeaky echo", "caro_davy", [("chipmunk", dict(factor=1.45)), ("echo", dict(ms=110, fb=0.3, mix=0.25, taps=3)),
                                          ("band", dict(lo=300))]),
    "89": ("Squeaky in a tin can", "caro_davy", [("chipmunk", dict(factor=1.45)), ("tin", dict(ms=1.6, fb=0.5, mix=0.3)),
                                                  ("reverb", dict(room=0.12, decay=0.1, mix=0.25)), ("band", dict(lo=300))]),
    "90": ("Autotune duo in a room", "jane", _tiny(pitch=360, formant=1.2, pitch_range=1.3) + _DUO
           + [("reverb", dict(room=0.35, decay=0.3, mix=0.2))]),
    "91": ("Sing-song tinier speaker", "anna", _tiny(pitch_range=1.35) + [("quantize", dict(amount=1.0))] + _T28),
    "92": ("Tinier speaker, a bit lower", "anna", _tiny(pitch=320, formant=1.25) + _T28),
    "93": ("Tiniest speaker", "anna", _tiny(pitch=420, formant=1.38) + [("band", dict(lo=900, hi=5000, order=4)),
                                                                      ("drive", dict(amount=3.0))]),
    "94": ("Tinier speaker, flutter", "anna", _tiny() + _T28 + [("tremolo", dict(rate=6, depth=0.2))]),
    "95": ("Squeaky autotune, tinier speaker", "caro_davy", [("chipmunk", dict(factor=1.35)), ("quantize", dict(amount=1.0))] + _T28),
    "96": ("Glitchy duo", "jane", _tiny(pitch=370, pitch_range=1.3) + _DUO + [("stutter", dict(every_s=1.2, ms=70, repeats=1))]),
    "97": ("Everything cute", "anna",
           [("chipmunk", dict(factor=1.18))] + _tiny(pitch=380, formant=1.2, pitch_range=1.3)
           + [("quantize", dict(amount=0.7)), ("chorus", dict(delay_ms=14, depth_ms=2, mix=0.35))] + _T28
           + [("reverb", dict(room=0.15, decay=0.12, mix=0.2))]),
})


def apply(x, sr, chain):
    """Run an effect chain; returns float32 peak-normalised to 0.89. Length may change (tempo ops)."""
    y = np.asarray(x, dtype=np.float64)
    for op, kw in chain:
        y = OPS[op](y, sr, **kw)
    y = y * (0.89 / max(np.abs(y).max(), 1e-6))
    return y.astype(np.float32)


def render(style_id, x, sr):
    return apply(x, sr, STYLES[style_id][2])


if __name__ == "__main__":
    import os
    import re
    import sys
    import time

    import soundfile as sf
    src, out, only = sys.argv[1], sys.argv[2], set(sys.argv[3:])      # optional: style ids to render
    os.makedirs(out, exist_ok=True)
    for sid, (name, voice, chain) in STYLES.items():
        if only and sid not in only:
            continue
        x, sr = sf.read(os.path.join(src, f"{voice}.wav"), dtype="float32")
        t0 = time.time()
        y = apply(x, sr, chain)
        slug = re.sub(r"[^a-z0-9]+", "-", name.lower().replace("inspired by the ", "").replace("inspired by ", "")).strip("-")
        sf.write(os.path.join(out, f"{sid}_{slug}.wav"), y, sr)
        print(f"{sid} {name:40s} {voice:9s} {len(y) / sr:4.1f}s  fx {1000 * (time.time() - t0):4.0f} ms")
