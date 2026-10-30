"""STT accuracy in noise: Qwen3-ASR 1.7B / 0.6B vs Whisper-large-v3-turbo (run with ..\\.venv).

Uses bench\\testset (make_testset.py) for WER per language x noise x SNR, language-ID accuracy and
false transcripts on noise-only clips, and transcribes the real robot recordings in bench\\real
side by side. All models get the same front end as the server: Silero VAD trim, language ID
limited to en/it/de.

    ..\\.venv\\Scripts\\python bench\\bench_stt.py  ->  bench\\results\\{summary.md, wer.csv, real.csv}
"""
import csv
import glob
import json
import os
import re
import sys
import time
import unicodedata
import wave

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
BRAIN = os.path.dirname(HERE)
sys.path.insert(0, BRAIN)
LANGS = ("en", "it", "de")


def read16k(path):
    with wave.open(path, "rb") as w:
        x = np.frombuffer(w.readframes(w.getnframes()), np.int16).astype(np.float32) / 32768
        sr = w.getframerate()
    if sr != 16000:
        n = int(len(x) * 16000 / sr)
        x = np.interp(np.arange(n) * sr / 16000, np.arange(len(x)), x).astype(np.float32)
    return x


def norm(s):
    s = unicodedata.normalize("NFKC", s or "").lower()
    s = s.replace("’", "'")
    s = re.sub(r"[^\w\s']", " ", s)
    s = re.sub(r"\bemo\b|\bemmo\b|\bemö\b", "emo", s)
    return re.sub(r"\s+", " ", s).strip()


class Whisper:
    """Whisper-large-v3-turbo (transformers, GPU) with the same constrained language ID."""

    def __init__(self, model_id="openai/whisper-large-v3-turbo"):
        import torch
        from transformers import WhisperForConditionalGeneration, WhisperProcessor
        self.torch = torch
        self.proc = WhisperProcessor.from_pretrained(model_id)
        self.model = WhisperForConditionalGeneration.from_pretrained(model_id, dtype=torch.float16).to("cuda").eval()
        tok = self.proc.tokenizer
        self.lang_ids = {l: tok.convert_tokens_to_ids(f"<|{l}|>") for l in LANGS}
        self.sot = tok.convert_tokens_to_ids("<|startoftranscript|>")

    def transcribe(self, audio, language=None):
        torch = self.torch
        with torch.inference_mode():
            feats = self.proc(audio, sampling_rate=16000, return_tensors="pt").input_features.to("cuda", torch.float16)
            if not language:
                logits = self.model(input_features=feats, decoder_input_ids=torch.tensor([[self.sot]], device="cuda")).logits[0, -1]
                language = max(self.lang_ids, key=lambda l: float(logits[self.lang_ids[l]]))
            out = self.model.generate(feats, language=language, task="transcribe", max_new_tokens=200)
        return self.proc.batch_decode(out, skip_special_tokens=True)[0].strip(), language


def wer(ref, hyp):
    import jiwer
    r, h = norm(ref), norm(hyp)
    if not r:
        return 0.0 if not h else 1.0
    return jiwer.wer(r, h)


def main():
    paths = json.load(open(os.path.join(BRAIN, "paths.json")))
    os.environ.setdefault("HF_HOME", os.path.join(paths["models"], "hf"))
    os.environ.setdefault("HF_HUB_OFFLINE", "1")
    import torch
    from stt import QwenASR, VAD
    vad = VAD()
    manifest = json.load(open(os.path.join(HERE, "testset", "manifest.json"), encoding="utf-8"))
    real = sorted(glob.glob(os.path.join(HERE, "real", "*.wav")))
    out_dir = os.path.join(HERE, "results")
    os.makedirs(out_dir, exist_ok=True)
    only = set(sys.argv[1:])
    models = [("qwen3-asr-1.7b", lambda: QwenASR("Qwen/Qwen3-ASR-1.7B-hf", languages=LANGS)),
              ("qwen3-asr-0.6b", lambda: QwenASR("Qwen/Qwen3-ASR-0.6B-hf", languages=LANGS)),
              ("whisper-large-v3-turbo", lambda: Whisper())]
    rows, real_rows, summary = [], {os.path.basename(f): {"file": os.path.basename(f)} for f in real}, {}
    for name, make in models:
        if only and name not in only:
            continue
        torch.cuda.empty_cache()
        free0 = torch.cuda.mem_get_info()[0]
        t0 = time.time()
        m = make()
        load_s = time.time() - t0
        vram = (free0 - torch.cuda.mem_get_info()[0]) / 2 ** 30
        m.transcribe(np.zeros(16000, np.float32), "en")
        print(f"== {name}: loaded in {load_s:.0f}s, {vram:.2f} GB VRAM", flush=True)
        lat = []
        for i, it in enumerate(manifest):
            x = read16k(os.path.join(HERE, "testset", it["file"]))
            t = time.time()
            speech, sp_s = vad.trim(x)
            if sp_s < 0.3:
                hyp, lang = "", None
            else:
                hyp, lang = m.transcribe(speech)
            lat.append((time.time() - t, len(x) / 16000))
            rows.append({"model": name, **it, "hyp": hyp, "hyp_lang": lang, "wer": wer(it["text"], hyp)})
            if i % 50 == 0:
                print(f"  {i}/{len(manifest)} {it['file']}: {hyp!r}", flush=True)
        for f in real:
            x = read16k(f)
            speech, sp_s = vad.trim(x)
            hyp, lang = (m.transcribe(speech) if sp_s >= 0.3 else ("", None))
            real_rows[os.path.basename(f)][name] = f"({lang}) {hyp}" if hyp else "(no speech)"
        lt = np.array(lat)
        summary[name] = {"vram_gb": round(vram, 2), "load_s": round(load_s, 1),
                         "rtf": round(float(lt[:, 0].sum() / lt[:, 1].sum()), 4),
                         "mean_latency_s": round(float(lt[:, 0].mean()), 3)}
        del m
    # ---- report
    with open(os.path.join(out_dir, "wer.csv"), "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader(); w.writerows(rows)
    with open(os.path.join(out_dir, "real.csv"), "w", newline="", encoding="utf-8") as f:
        keys = ["file"] + [n for n, _ in models if not only or n in only]
        w = csv.DictWriter(f, fieldnames=keys)
        w.writeheader(); w.writerows(real_rows.values())
    names = list(summary)
    conds = ["clean"] + [f"{n} {s}dB" for n in ("room", "babble", "tv") for s in (20, 10, 5, 0)]
    lines = ["# STT benchmark", "", "Word error rate in % (lower is better), all three languages pooled.", "",
             "| condition | " + " | ".join(names) + " |", "|---|" + "---|" * len(names)]
    for c in conds:
        cells = []
        for n in names:
            sel = [r for r in rows if r["model"] == n and r["lang"] and
                   ((r["noise"] == "clean") if c == "clean" else (f"{r['noise']} {r['snr']}dB" == c))]
            cells.append(f"{100 * np.mean([r['wer'] for r in sel]):.1f}" if sel else "-")
        lines.append(f"| {c} | " + " | ".join(cells) + " |")
    lines += ["", "| per language (all conditions) | " + " | ".join(names) + " |", "|---|" + "---|" * len(names)]
    for l in LANGS:
        lines.append(f"| {l} | " + " | ".join(
            f"{100 * np.mean([r['wer'] for r in rows if r['model'] == n and r['lang'] == l]):.1f}" for n in names) + " |")
    lines += ["", "| other | " + " | ".join(names) + " |", "|---|" + "---|" * len(names)]
    lines.append("| language ID correct, % | " + " | ".join(
        f"{100 * np.mean([r['hyp_lang'] == r['lang'] for r in rows if r['model'] == n and r['lang'] and r['hyp']]):.1f}" for n in names) + " |")
    lines.append("| noise-only clips with a transcript | " + " | ".join(
        f"{sum(1 for r in rows if r['model'] == n and not r['lang'] and r['hyp'])}/{sum(1 for r in rows if r['model'] == n and not r['lang'])}" for n in names) + " |")
    for key, label in (("vram_gb", "VRAM, GB"), ("rtf", "real-time factor"), ("mean_latency_s", "mean latency per clip, s")):
        lines.append(f"| {label} | " + " | ".join(str(summary[n][key]) for n in names) + " |")
    open(os.path.join(out_dir, "summary.md"), "w", encoding="utf-8").write("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
