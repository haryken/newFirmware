#!/usr/bin/env python3
"""Render the same sentences with stock XTTS v2 (zero-shot) and the fine-tuned EMO model, for A/B listening.

    python make_samples.py                      # newest run/xtts_emo-* vs stock
    python make_samples.py --run run\\xtts_emo-...

Writes samples/<stock|finetuned>_<n>_<lang>.wav (24 kHz) with the same settings tts_server.py uses.
"""
import argparse
import glob
import os
import time

import audio_compat  # noqa: F401  (torchcodec-free audio loading)

HERE = os.path.dirname(os.path.abspath(__file__))

SENTENCES = [
    ("en", "Hi, I'm EMO. It's nice to finally talk to you."),
    ("en", "Hmm, I think it's going to rain this afternoon. Don't forget your umbrella!"),
    ("en", "Want to play rock, paper, scissors? I'm feeling lucky today."),
    ("it", "Ciao, sono EMO. Oggi ho voglia di ballare."),
    ("it", "Che ore sono? Sono le tre e mezza, è ora della merenda!"),
]


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--run", help="fine-tuned run folder (default: newest run/xtts_emo-*)")
    p.add_argument("--stock", default=os.path.join(HERE, "run", "XTTS_v2.0_original_model_files"))
    p.add_argument("--reference", default=os.path.join(HERE, "dataset", "reference_6s.wav"))
    p.add_argument("--out", default=os.path.join(HERE, "samples"))
    args = p.parse_args()

    import gc
    import torch
    from tts_server import XttsEngine, to_wav_bytes

    run = args.run
    if not run:
        runs = [r for r in sorted(glob.glob(os.path.join(HERE, "run", "xtts_emo*")), key=os.path.getmtime)
                if glob.glob(os.path.join(r, "best_model*.pth"))]
        if not runs:
            raise SystemExit("no fine-tuned run with a best_model*.pth under run/")
        run = runs[-1]
    os.makedirs(args.out, exist_ok=True)
    for tag, folder in (("stock", args.stock), ("finetuned", run)):
        print(f"[{tag}] loading {folder}")
        eng = XttsEngine(args.reference, folder)
        for i, (lang, text) in enumerate(SENTENCES, 1):
            t0 = time.time()
            x, sr = eng.synth(text, lang)
            path = os.path.join(args.out, f"{tag}_{i}_{lang}.wav")
            with open(path, "wb") as f:
                f.write(to_wav_bytes(x, sr))
            print(f"  {path}  {len(x) / sr:.1f}s audio in {time.time() - t0:.1f}s")
        del eng
        gc.collect()
        if torch.cuda.is_available():
            torch.cuda.empty_cache()
    print(f"done -> {args.out}")


if __name__ == "__main__":
    main()
