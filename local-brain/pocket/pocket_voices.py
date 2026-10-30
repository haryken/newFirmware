"""Render one test line with every built-in English Pocket TTS voice -> pocket/voices/<name>.wav,
and list the downloaded model files in pocket/cache_files.txt."""
import os
import time
import wave

import numpy as np

from pocket_tts import TTSModel
from pocket_tts.utils.utils import _ORIGINS_OF_PREDEFINED_VOICES, download_if_necessary, get_predefined_voice

HERE = os.path.dirname(os.path.abspath(__file__))
TEXT = "Hi! I'm EMO, your little desktop buddy. Want to play a game with me?"
NON_EN = {"giovanni", "lola", "juergen", "rafael", "daan", "estelle"}

out = os.path.join(HERE, "voices")
os.makedirs(out, exist_ok=True)
t0 = time.time()
model = TTSModel.load_model()
print(f"model loaded in {time.time() - t0:.1f}s, sample rate {model.sample_rate}")
files = []
for name in [v for v in _ORIGINS_OF_PREDEFINED_VOICES if v not in NON_EN]:
    t0 = time.time()
    state = model.get_state_for_audio_prompt(name)
    audio = model.generate_audio(state, TEXT).numpy()
    path = os.path.join(out, f"{name}.wav")
    with wave.open(path, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(model.sample_rate)
        w.writeframes((np.clip(audio, -1, 1) * 32767).astype(np.int16).tobytes())
    print(f"{name:15s} {len(audio) / model.sample_rate:4.1f}s audio in {time.time() - t0:4.1f}s")
    files.append(str(download_if_necessary(get_predefined_voice("english", name))))

import yaml  # noqa: E402  (pulled in by pocket-tts' config loading)
import pocket_tts  # noqa: E402
cfg = yaml.safe_load(open(os.path.join(os.path.dirname(pocket_tts.__file__), "config", "english.yaml")))


def walk(d):
    if isinstance(d, dict):
        for v in d.values():
            yield from walk(v)
    elif isinstance(d, str) and d.startswith("hf://"):
        yield d


for p in walk(cfg):
    try:
        files.append(f"{p}\t{download_if_necessary(p)}")
    except Exception as e:  # weights with voice cloning may be gated; we don't need them
        files.append(f"{p}\tNOT AVAILABLE {type(e).__name__}")
open(os.path.join(HERE, "cache_files.txt"), "w").write("\n".join(files) + "\n")
print("done")
