#!/usr/bin/env python3
"""Build the EMO voice dataset from the robot's own mp3 assets.

Input : the stock /spiffs/mp3 folder (spiffs/spiffs/mp3 in this repo) and clips.tsv
        (the ~115 spoken lines with their transcripts, curated from Whisper output).
Output: dataset/
          wavs/<name>.wav      22.05 kHz mono 16-bit, silence-trimmed, peak-normalised
          metadata.csv         LJSpeech format  name|text|text   (what XTTS fine-tuning reads)
          reference.wav        24 kHz, the "!" lines joined (~25 s): the zero-shot clone prompt
          reference.txt        its transcript (some engines want it)
          reference_6s.wav     the first 6 s of it (XTTS conditioning length)

    python prepare_dataset.py --mp3 ../../spiffs/spiffs/mp3 --out dataset

Decoding uses `miniaudio` (pip install miniaudio, pure wheel) and falls back to ffmpeg on PATH.
"""
import argparse
import csv
import os
import shutil
import subprocess
import sys
import wave

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
TRAIN_SR = 22050          # XTTS dvae / training rate
REF_SR = 24000            # XTTS output / conditioning rate


def decode(path, sr):
    """mp3 -> float32 mono at sr."""
    try:
        import miniaudio
        d = miniaudio.decode_file(path, output_format=miniaudio.SampleFormat.SIGNED16, nchannels=1, sample_rate=sr)
        return np.frombuffer(d.samples, dtype=np.int16).astype(np.float32) / 32768.0
    except ImportError:
        pass
    if not shutil.which("ffmpeg"):
        sys.exit("need `pip install miniaudio` or ffmpeg on PATH to decode mp3")
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-f", "s16le", "-ac", "1", "-ar", str(sr), "-"],
                         capture_output=True, check=True).stdout
    return np.frombuffer(raw, dtype=np.int16).astype(np.float32) / 32768.0


def trim(x, sr, thresh=0.01, pre=0.05, post=0.15):
    nz = np.where(np.abs(x) > thresh)[0]
    if len(nz) == 0:
        return x
    a = max(0, nz[0] - int(pre * sr))
    b = min(len(x), nz[-1] + int(post * sr))
    return x[a:b]


def normalise(x, peak=0.8):
    m = np.abs(x).max()
    return x * (peak / m) if m > 1e-6 else x


def write_wav(path, x, sr):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes((np.clip(x, -1, 1) * 32767).astype(np.int16).tobytes())


def read_clips(tsv):
    rows = []
    with open(tsv, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            name, text = line.split("\t", 1)
            ref = name.startswith("!")
            rows.append((name.lstrip("!"), text.strip(), ref))
    return rows


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--mp3", default=os.path.join(HERE, "..", "..", "spiffs", "spiffs", "mp3"), help="stock mp3 folder")
    p.add_argument("--clips", default=os.path.join(HERE, "clips.tsv"))
    p.add_argument("--out", default=os.path.join(HERE, "dataset"))
    args = p.parse_args()

    rows = read_clips(args.clips)
    wavs = os.path.join(args.out, "wavs")
    os.makedirs(wavs, exist_ok=True)
    meta, total, missing = [], 0.0, []
    ref_parts, ref_text = [], []
    for name, text, is_ref in rows:
        src = os.path.join(args.mp3, name + ".mp3")
        if not os.path.exists(src):
            missing.append(name)
            continue
        x = normalise(trim(decode(src, TRAIN_SR), TRAIN_SR))
        if len(x) < 0.3 * TRAIN_SR:
            continue
        write_wav(os.path.join(wavs, name + ".wav"), x, TRAIN_SR)
        meta.append((name, text))
        total += len(x) / TRAIN_SR
        if is_ref:
            r = normalise(trim(decode(src, REF_SR), REF_SR))
            ref_parts += [r, np.zeros(int(0.3 * REF_SR), np.float32)]
            ref_text.append(text)

    with open(os.path.join(args.out, "metadata.csv"), "w", encoding="utf-8", newline="") as f:
        w = csv.writer(f, delimiter="|", quoting=csv.QUOTE_NONE, escapechar="\\")
        for name, text in meta:
            w.writerow([name, text, text])
    ref = np.concatenate(ref_parts) if ref_parts else np.zeros(REF_SR, np.float32)
    write_wav(os.path.join(args.out, "reference.wav"), ref, REF_SR)
    write_wav(os.path.join(args.out, "reference_6s.wav"), ref[:6 * REF_SR], REF_SR)
    with open(os.path.join(args.out, "reference.txt"), "w", encoding="utf-8") as f:
        f.write(" ".join(ref_text) + "\n")

    print(f"{len(meta)} clips, {total:.0f} s of speech -> {args.out}")
    print(f"reference.wav {len(ref) / REF_SR:.1f} s from {len(ref_text)} lines")
    if missing:
        print(f"missing mp3 ({len(missing)}): {', '.join(missing[:10])}{' ...' if len(missing) > 10 else ''}")


if __name__ == "__main__":
    main()
