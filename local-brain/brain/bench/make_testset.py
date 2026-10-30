"""Build a noisy speech test set with known transcripts (run with pocket\\.venv's python).

Clean speech: Pocket TTS, 12 typical robot sentences in each of English, Italian, German.
Noise, the kinds a desk robot's always-open mic picks up:
  room    the quiet parts of the 47 real robot recordings (room tone, fans, the robot's own servos)
  babble  three other people talking (other TTS sentences in other voices / languages)
  tv      quieter babble plus music (synthetic chords with a beat)
Mixed at SNR 20 / 10 / 5 / 0 dB (speech RMS over its active part vs noise RMS), with a short
desk-room reverb on the speech, plus noise-only clips that must produce no transcript.

    pocket\\.venv\\Scripts\\python bench\\make_testset.py  ->  bench\\testset\\*.wav + manifest.json
"""
import glob
import json
import os
import sys
import wave

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "testset")
SR = 16000
SNRS = (20, 10, 5, 0)

SENTENCES = {
    "en": ["EMO, can you dance for me?", "What's the weather like today?",
           "Turn left and walk a little bit forward.", "Please set the volume a bit lower.",
           "Tell me a joke about cats.", "How much battery do you have left?",
           "Good morning, did you sleep well?", "Can you tell me what time it is?",
           "Show me a happy face.", "Let's play rock paper scissors.",
           "I'm going to the kitchen, wait for me here.", "Do you like listening to music?"],
    "it": ["EMO, puoi ballare per me?", "Che tempo fa oggi a Zurigo?",
           "Gira a sinistra e cammina un po' in avanti.", "Abbassa un po' il volume, per favore.",
           "Raccontami una barzelletta sui gatti.", "Quanta batteria ti rimane?",
           "Buongiorno, hai dormito bene?", "Mi dici che ore sono?",
           "Fammi vedere una faccia felice.", "Giochiamo a sasso, carta, forbici.",
           "Vado in cucina, aspettami qui.", "Ti piace ascoltare la musica?"],
    "de": ["EMO, kannst du für mich tanzen?", "Wie ist das Wetter heute in Zürich?",
           "Dreh dich nach links und geh ein bisschen nach vorne.", "Mach die Lautstärke bitte etwas leiser.",
           "Erzähl mir einen Witz über Katzen.", "Wie viel Akku hast du noch?",
           "Guten Morgen, hast du gut geschlafen?", "Kannst du mir sagen, wie spät es ist?",
           "Zeig mir ein fröhliches Gesicht.", "Lass uns Schere, Stein, Papier spielen.",
           "Ich gehe in die Küche, warte hier auf mich.", "Hörst du gern Musik?"],
}
EN_VOICES = ["azelma", "jean", "michael", "eve", "paul", "jane"]
LANG_MODEL = {"en": "english", "it": "italian", "de": "german"}


def write(path, x):
    with wave.open(path, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR)
        w.writeframes((np.clip(x, -1, 1) * 32767).astype(np.int16).tobytes())


def read(path):
    with wave.open(path, "rb") as w:
        x = np.frombuffer(w.readframes(w.getnframes()), np.int16).astype(np.float32) / 32768
        return x, w.getframerate()


def resample(x, sr):
    from scipy.signal import resample_poly
    from math import gcd
    g = gcd(sr, SR)
    return resample_poly(x, SR // g, sr // g).astype(np.float32)


def active_rms(x):
    fr = x[: len(x) // 320 * 320].reshape(-1, 320)
    e = np.sqrt(np.mean(fr ** 2, 1))
    return float(np.sqrt(np.mean(e[e > 0.1 * e.max()] ** 2)))


def reverb(x, rng, rt60=0.25):
    n = int(rt60 * SR)
    ir = rng.standard_normal(n) * np.exp(-6.9 * np.arange(n) / n)
    ir[0] = 1.0 / 0.35
    y = np.convolve(x, ir * 0.35)[: len(x)]
    return (y / max(np.abs(y).max(), 1e-6) * np.abs(x).max()).astype(np.float32)


def room_noise(n_total):
    """Concatenate the quietest frames of the real robot recordings (no speech)."""
    real = sorted(glob.glob(os.path.join(HERE, "real", "*.wav")))
    parts = []
    for f in real:
        x, sr = read(f)
        x = resample(x, sr) if sr != SR else x
        fr = x[: len(x) // 1600 * 1600].reshape(-1, 1600)
        e = np.sqrt(np.mean(fr ** 2, 1))
        parts.append(fr[e <= np.percentile(e, 30)].reshape(-1))
    if not parts:
        raise SystemExit("bench\\real\\*.wav (the robot recordings) not found")
    noise = np.concatenate(parts)
    reps = int(np.ceil(n_total / len(noise)))
    return np.tile(noise, reps)[:n_total]


def music(n, rng):
    t = np.arange(n) / SR
    y = np.zeros(n)
    beat = int(0.5 * SR)
    roots = rng.choice([220, 247, 262, 294, 330], size=n // beat + 1)
    for i, r in enumerate(roots):
        a, b = i * beat, min(n, (i + 1) * beat)
        for k, m in enumerate((1, 1.26, 1.5, 2)):
            y[a:b] += np.sin(2 * np.pi * r * m * t[a:b]) / (k + 1)
        y[a:a + 800] += rng.standard_normal(min(800, b - a)) * 0.5 * np.exp(-np.arange(min(800, b - a)) / 150)
    return y.astype(np.float32)


def main():
    from pocket_tts import TTSModel
    rng = np.random.default_rng(7)
    os.makedirs(OUT, exist_ok=True)
    clean = []                                                     # (lang, text, audio)
    for lang, sents in SENTENCES.items():
        model = TTSModel.load_model(language=LANG_MODEL[lang])
        voices = EN_VOICES if lang == "en" else [None]
        states = {}
        for i, text in enumerate(sents):
            v = voices[i % len(voices)]
            if v not in states:
                from pocket_tts.default_parameters import DEFAULT_VOICE_FOR_LANGUAGE
                states[v] = model.get_state_for_audio_prompt(v or DEFAULT_VOICE_FOR_LANGUAGE[LANG_MODEL[lang]])
            x = model.generate_audio(states[v], text).numpy().astype(np.float32)
            x = resample(x, model.sample_rate)
            x = x / max(np.abs(x).max(), 1e-6) * 0.5
            clean.append((lang, text, x))
            print(f"tts {lang} {i:2d} {len(x) / SR:4.1f}s {text}", flush=True)

    total = sum(len(c[2]) for c in clean) + SR
    room = room_noise(total * 2)
    manifest, k = [], 0
    for idx, (lang, text, x) in enumerate(clean):
        x = np.concatenate([np.zeros(int(0.4 * SR), np.float32), reverb(x, rng), np.zeros(int(0.5 * SR), np.float32)])
        n = len(x)
        others = [c for j, c in enumerate(clean) if j != idx]
        pick = [others[j] for j in rng.choice(len(others), 3, replace=False)]
        bab = np.zeros(n, np.float32)
        for _, _, o in pick:                                     # three overlapping talkers
            o = np.tile(o, int(np.ceil(n / len(o))) + 1)
            s = rng.integers(0, len(o) - n)
            bab += o[s:s + n] * rng.uniform(0.6, 1.0)
        start = rng.integers(0, len(room) - n)
        noises = {"room": room[start:start + n], "babble": bab,
                  "tv": 0.6 * bab / max(active_rms(bab), 1e-6) + music(n, rng) / max(active_rms(music(n, rng)), 1e-6)}
        sp = active_rms(x)
        name = f"{k:04d}_{lang}_clean.wav"; k += 1
        write(os.path.join(OUT, name), x)
        manifest.append({"file": name, "lang": lang, "text": text, "noise": "clean", "snr": None})
        for nt, nz in noises.items():
            nz = nz / max(np.sqrt(np.mean(nz ** 2)), 1e-9)
            for snr in SNRS:
                y = x + nz * sp / (10 ** (snr / 20))
                y = y / max(np.abs(y).max(), 1e-6) * 0.7
                name = f"{k:04d}_{lang}_{nt}_{snr}dB.wav"; k += 1
                write(os.path.join(OUT, name), y)
                manifest.append({"file": name, "lang": lang, "text": text, "noise": nt, "snr": snr})
    for i in range(10):                                          # must give no transcript
        n = int(rng.uniform(2, 5) * SR)
        start = rng.integers(0, len(room) - n)
        y = room[start:start + n] * rng.uniform(1, 8)
        name = f"{k:04d}_none_room.wav"; k += 1
        write(os.path.join(OUT, name), np.clip(y, -1, 1))
        manifest.append({"file": name, "lang": None, "text": "", "noise": "room-only", "snr": None})
        y = music(n, rng) * 0.2
        name = f"{k:04d}_none_music.wav"; k += 1
        write(os.path.join(OUT, name), y)
        manifest.append({"file": name, "lang": None, "text": "", "noise": "music-only", "snr": None})
    json.dump(manifest, open(os.path.join(OUT, "manifest.json"), "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    print(f"{len(manifest)} clips -> {OUT}")


if __name__ == "__main__":
    sys.exit(main())
