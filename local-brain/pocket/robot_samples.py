"""Render EMO's robot voice: a few lines x built-in voices x robot presets -> samples/robot/.

    pocket\\.venv\\Scripts\\python pocket\\robot_samples.py
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)

import robot_fx  # noqa: E402
from tts_server import PocketEngine, to_wav_bytes  # noqa: E402

LINES = [
    ("en", "Hi! I'm EMO, your little desktop buddy. Want to play a game with me?"),
    ("en", "Oh no, it's raining outside. Don't forget your umbrella!"),
    ("en", "Yay! I won! Again? Okay, okay, one more round."),
    ("it", "Ciao! Sono EMO. Oggi ho voglia di ballare!"),
]
VOICES = ["azelma", "jane", "eve", "cosette"]

out = os.path.join(ROOT, "samples", "robot")
os.makedirs(out, exist_ok=True)
for voice in VOICES:
    eng = PocketEngine({"en": voice})
    for i, (lang, text) in enumerate(LINES, 1):
        if lang != "en" and voice != VOICES[0]:
            continue                     # the Italian model has one built-in voice; render it once
        t0 = time.time()
        x, sr = eng.synth(text, lang)
        tag = voice if lang == "en" else "giovanni"
        with open(os.path.join(out, f"raw_{tag}_{i}_{lang}.wav"), "wb") as f:
            f.write(to_wav_bytes(x * (0.89 / max(abs(x).max(), 1e-6)), sr))
        for preset in ("soft", "pet", "bot"):
            y = robot_fx.robotize(x, sr, **robot_fx.PRESETS[preset])
            with open(os.path.join(out, f"{preset}_{tag}_{i}_{lang}.wav"), "wb") as f:
                f.write(to_wav_bytes(y, sr))
        print(f"{tag:9s} line {i} ({lang}) {len(x) / sr:4.1f}s audio, synth+fx {time.time() - t0:4.1f}s")
print("done ->", out)
