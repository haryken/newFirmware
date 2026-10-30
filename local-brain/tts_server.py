#!/usr/bin/env python3
r"""EMO voice TTS service: runs on the GPU desktop, called by emo_server.py on the laptop.

    POST /tts   {"text": "...", "lang": "en"}   -> audio/wav (mono 16-bit, 24 kHz)
                optional "fx": {"preset": "off|light|medium|strong", "pitch": 300, "formant": 1.18,
                                "pitch_range": 1.35}  overrides the --child default per request
                optional "robot": "off|soft|pet|bot" or {"preset": "pet", "pitch": 400, ...}
                                overrides the --robot default (robot_fx.py) per request
                optional "style": "01".."47" (robot_styles.py, sets the voice too) instead of "robot"; "off" for none
    GET  /health                                -> {"ok": true, "engine": "...", "device": "..."}

Engines (--engine):
    xtts        Coqui XTTS v2, zero-shot from --reference (dataset/reference_6s.wav), or the
                fine-tuned model from finetune_xtts.py with --xtts-dir. 17 languages incl. it/en.
    chatterbox  Resemble Chatterbox (multilingual), zero-shot from --reference. Alternative if
                XTTS's licence (non-commercial) or quality does not suit.
    pocket      Kyutai Pocket TTS (100M, CPU, built-in voices, MIT/CC-BY), shaped into a cute
                little robot by robot_fx.py. Runs from its own venv (pocket\.venv, plain CPU
                torch; the ROCm build's CPU path is ~100x too slow for it). en/it/fr/de/es/pt/nl.

    python tts_server.py --engine xtts --reference dataset/reference_6s.wav            # zero-shot
    python tts_server.py --engine xtts --xtts-dir run/xtts_emo-... --reference dataset/reference_6s.wav
    python tts_server.py --engine chatterbox --reference dataset/reference.wav
    pocket\.venv\Scripts\python tts_server.py --engine pocket                 # robot "pet" voice
    pocket\.venv\Scripts\python tts_server.py --engine pocket --robot bot --voices en=jane
    pocket\.venv\Scripts\python tts_server.py --engine pocket --style 07      # one of robot_styles.py

Then on the laptop:  python emo_server.py --tts-url http://<desktop-ip>:8777
"""
import argparse
import io
import json
import os
import threading
import time
import wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import numpy as np

import robot_fx
import robot_styles
import voice_fx

HERE = os.path.dirname(os.path.abspath(__file__))


def to_wav_bytes(x, sr):
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes((np.clip(np.asarray(x, dtype=np.float32), -1, 1) * 32767).astype(np.int16).tobytes())
    return buf.getvalue()


# --------------------------------------------------------------------------- #
# Engines: synth(text, lang) -> (float32 mono, sample_rate)
# --------------------------------------------------------------------------- #
class XttsEngine:
    name = "xtts"

    def __init__(self, reference, xtts_dir=None, device=None):
        import audio_compat  # noqa: F401  (torchcodec-free audio loading; must precede the TTS imports)
        import torch
        from TTS.tts.configs.xtts_config import XttsConfig
        from TTS.tts.models.xtts import Xtts
        self.device = device or ("cuda" if torch.cuda.is_available() else "cpu")
        if xtts_dir:
            cfg_path = os.path.join(xtts_dir, "config.json")
            ckpt = os.path.join(xtts_dir, "best_model.pth")
            if not os.path.exists(ckpt):            # stock files folder, or a run without best_model.pth
                import glob
                aux = {"dvae.pth", "mel_stats.pth", "speakers_xtts.pth"}
                cands = [c for c in sorted(glob.glob(os.path.join(xtts_dir, "*.pth")), key=os.path.getmtime)
                         if os.path.basename(c) not in aux]
                stock = os.path.join(xtts_dir, "model.pth")
                ckpt = stock if os.path.exists(stock) else cands[-1]
            vocab = os.path.join(xtts_dir, "vocab.json")
        else:                                   # stock XTTS v2 via the TTS model manager
            from TTS.utils.manage import ModelManager
            mm = ModelManager()
            model_dir, _, _ = mm.download_model("tts_models/multilingual/multi-dataset/xtts_v2")
            cfg_path = os.path.join(model_dir, "config.json")
            ckpt = os.path.join(model_dir, "model.pth")
            vocab = os.path.join(model_dir, "vocab.json")
        self.config = XttsConfig()
        self.config.load_json(cfg_path)
        self.model = Xtts.init_from_config(self.config)
        self.model.load_checkpoint(self.config, checkpoint_path=ckpt, vocab_path=vocab, use_deepspeed=False)
        self.model.to(self.device)
        self.model.eval()
        print(f"[xtts] computing speaker latents from {reference}")
        self.gpt_latent, self.spk_emb = self.model.get_conditioning_latents(
            audio_path=[reference], gpt_cond_len=6, gpt_cond_chunk_len=3, max_ref_length=20)
        self.sr = self.config.audio.output_sample_rate     # 24000
        self.lock = threading.Lock()

    def synth(self, text, lang):
        lang = (lang or "en")[:2]
        if lang not in self.config.languages:
            lang = "en"
        with self.lock:
            out = self.model.inference(text, lang, self.gpt_latent, self.spk_emb,
                                       temperature=0.65, repetition_penalty=5.0, top_k=50, top_p=0.85,
                                       speed=1.05, enable_text_splitting=True)
        return np.asarray(out["wav"], dtype=np.float32), self.sr


class ChatterboxEngine:
    name = "chatterbox"

    def __init__(self, reference, device=None):
        import torch
        self.device = device or ("cuda" if torch.cuda.is_available() else "cpu")
        try:
            from chatterbox.mtl_tts import ChatterboxMultilingualTTS as C
            self.multilingual = True
        except ImportError:
            from chatterbox.tts import ChatterboxTTS as C
            self.multilingual = False
        self.model = C.from_pretrained(device=self.device)
        self.reference = reference
        self.sr = self.model.sr
        self.lock = threading.Lock()

    def synth(self, text, lang):
        kw = {"audio_prompt_path": self.reference, "exaggeration": 0.6, "cfg_weight": 0.5}
        if self.multilingual:
            kw["language_id"] = (lang or "en")[:2]
        with self.lock:
            wav = self.model.generate(text, **kw)
        return wav.squeeze().cpu().numpy().astype(np.float32), self.sr


class PocketEngine:
    """Kyutai Pocket TTS with built-in voices. One model per language, loaded on first use."""
    name = "pocket"
    LANGS = {"en": "english", "it": "italian", "fr": "french", "de": "german",
             "es": "spanish", "pt": "portuguese", "nl": "dutch"}

    def __init__(self, voices=None, quantize=False, device="cpu"):
        from pocket_tts import TTSModel
        from pocket_tts.default_parameters import DEFAULT_VOICE_FOR_LANGUAGE
        self._TTSModel = TTSModel
        self.voices = {"en": "azelma"}
        self.voices.update({k: v for k, v in (voices or {}).items()})
        self._defaults = {k: DEFAULT_VOICE_FOR_LANGUAGE.get(v) for k, v in self.LANGS.items()}
        self.device = device
        self.quantize = quantize and device == "cpu"      # pocket's int8 path is CPU-only
        self.models = {}                         # lang -> model
        self.states = {}                         # (lang, voice) -> voice state
        self.lock = threading.Lock()
        self._get("en")
        self.sr = self.models["en"].sample_rate

    def _get(self, lang, voice=None):
        if lang not in self.models:
            t0 = time.time()
            model = self._TTSModel.load_model(language=self.LANGS[lang], quantize=self.quantize)
            if self.device != "cpu":
                model = model.to(self.device)
            self.models[lang] = model
            print(f"[pocket] {self.LANGS[lang]} model loaded on {self.device} in {time.time() - t0:.1f}s")
        model = self.models[lang]
        voice = voice or self.voices.get(lang) or self._defaults.get(lang) or "alba"
        if (lang, voice) not in self.states:
            try:
                self.states[(lang, voice)] = model.get_state_for_audio_prompt(voice)
                print(f"[pocket] {lang} voice '{voice}' ready")
            except Exception as e:                # e.g. an English voice asked of the Italian model
                print(f"[pocket] no voice '{voice}' for {lang} ({type(e).__name__}); using the default")
                fallback = self.voices.get(lang) or self._defaults.get(lang) or "alba"
                if fallback == voice:
                    raise
                self.states[(lang, voice)] = self._get(lang, fallback)[1]
        return model, self.states[(lang, voice)]

    def synth(self, text, lang, voice=None):
        """voice: a built-in voice name for this request (English styles pick their own voice)."""
        lang = (lang or "en")[:2]
        if lang not in self.LANGS:
            lang = "en"
        with self.lock:
            model, state = self._get(lang, voice if lang == "en" else None)
            audio = model.generate_audio(state, text)
        return audio.detach().float().cpu().numpy(), model.sample_rate


# --------------------------------------------------------------------------- #
# HTTP
# --------------------------------------------------------------------------- #
def make_handler(engine, device, child_default, robot_default=None, style_default=None):
    robot_default = robot_default or {"preset": "off"}

    class H(BaseHTTPRequestHandler):
        def log_message(self, fmt, *a):            # one line per request, no noise
            pass

        def _send(self, code, body, ctype):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            if self.path.startswith("/health"):
                self._send(200, json.dumps({"ok": True, "engine": engine.name, "device": device,
                                            "sample_rate": engine.sr, "child": child_default,
                                            "robot": robot_default, "style": style_default}).encode(),
                           "application/json")
            else:
                self._send(404, b"not found", "text/plain")

        def do_POST(self):
            if not self.path.startswith("/tts"):
                self._send(404, b"not found", "text/plain")
                return
            n = int(self.headers.get("Content-Length") or 0)
            try:
                req = json.loads(self.rfile.read(n) or b"{}")
                text = (req.get("text") or "").strip()
                if not text:
                    raise ValueError("empty text")
                t0 = time.time()
                style = req.get("style", style_default)
                sid = str(style).zfill(2) if style and style != "off" else None
                if sid and sid not in robot_styles.STYLES:
                    raise ValueError(f"unknown style {style!r}")
                if isinstance(engine, PocketEngine):          # the style's own base voice
                    x, sr = engine.synth(text, req.get("lang"), robot_styles.STYLES[sid][1] if sid else None)
                else:
                    x, sr = engine.synth(text, req.get("lang"))
                fx = req.get("fx") or {}
                params = voice_fx.resolve(fx.get("preset", child_default["preset"]),
                                          **{k: fx.get(k, child_default.get(k)) for k in ("pitch", "formant", "pitch_range")})
                if params:
                    x = voice_fx.childify(x, sr, **params)
                style = req.get("style", style_default)
                if style and style != "off":
                    x = robot_styles.render(str(style).zfill(2), x, sr)
                    rb = "off" if "robot" not in req else req["robot"]
                else:
                    rb = req.get("robot", robot_default)
                rb = {"preset": rb} if isinstance(rb, str) else dict(rb or {})
                rparams = robot_fx.resolve(rb.pop("preset", robot_default.get("preset")),
                                           **{k: v for k, v in rb.items() if k in robot_fx.PRESETS["pet"]})
                if rparams:
                    x = robot_fx.robotize(x, sr, **rparams)
                print(f"[tts] {len(x) / sr:4.1f}s audio in {time.time() - t0:4.1f}s  ({req.get('lang')}) {text[:70]!r}")
                self._send(200, to_wav_bytes(x, sr), "audio/wav")
            except Exception as e:
                print(f"[tts] error: {type(e).__name__}: {e}")
                self._send(500, json.dumps({"ok": False, "error": str(e)}).encode(), "application/json")
    return H


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--engine", choices=["xtts", "chatterbox", "pocket"], default="xtts")
    p.add_argument("--reference", default=os.path.join(HERE, "dataset", "reference_6s.wav"))
    p.add_argument("--xtts-dir", help="fine-tuned run folder from finetune_xtts.py (default: stock XTTS v2)")
    p.add_argument("--device", help="cuda / cpu (default: cuda if available; ROCm torch also reports cuda)")
    p.add_argument("--child", choices=list(voice_fx.PRESETS),
                   help="child-voice preset (voice_fx.py); default medium for xtts, off for pocket")
    p.add_argument("--pitch", type=float, help="override the preset's target median pitch in Hz")
    p.add_argument("--formant", type=float, help="override the preset's formant shift ratio")
    p.add_argument("--pitch-range", type=float, help="override the preset's pitch range factor")
    p.add_argument("--robot", choices=list(robot_fx.PRESETS),
                   help="small-robot preset (robot_fx.py); default pet for pocket, off otherwise")
    p.add_argument("--style", choices=list(robot_styles.STYLES),
                   help="pocket: use one of the 20 styles in robot_styles.py (voice + effect chain) instead of --robot")
    p.add_argument("--voices", default="",
                   help="pocket: built-in voice per language, e.g. en=azelma,it=giovanni (default en=azelma)")
    p.add_argument("--quantize", action="store_true", help="pocket: int8 quantisation (~27%% faster on x86)")
    p.add_argument("--host", default="0.0.0.0")
    p.add_argument("--port", type=int, default=8777)
    args = p.parse_args()

    import torch
    device = args.device or ("cuda" if torch.cuda.is_available() else "cpu")
    print(f"torch {torch.__version__} on {device}" + (f" ({torch.cuda.get_device_name(0)})" if device == "cuda" else ""))
    t0 = time.time()
    if args.engine == "pocket":
        device = args.device or "cpu"               # Pocket TTS is CPU-first; --device cuda moves it to the GPU
        voices = dict(kv.split("=", 1) for kv in args.voices.split(",") if "=" in kv)
        if args.style and "en" not in voices:
            voices["en"] = robot_styles.STYLES[args.style][1]
        engine = PocketEngine(voices, quantize=args.quantize, device=device)
    elif args.engine == "xtts":
        engine = XttsEngine(args.reference, args.xtts_dir, device)
    else:
        engine = ChatterboxEngine(args.reference, device)
    if args.child is None:
        args.child = "medium" if args.engine == "xtts" else "off"
    if args.robot is None:
        args.robot = "pet" if args.engine == "pocket" and not args.style else "off"
    print(f"[{engine.name}] ready in {time.time() - t0:.0f}s, warming up...")
    engine.synth("Hello, I'm EMO.", "en")
    child_default = {"preset": args.child, "pitch": args.pitch, "formant": args.formant, "pitch_range": args.pitch_range}
    print(f"[fx] child voice: {voice_fx.resolve(args.child, pitch=args.pitch, formant=args.formant, pitch_range=args.pitch_range) or 'off'}")
    print(f"[fx] robot voice: {robot_fx.resolve(args.robot) or 'off'}"
          + (f", style {args.style} ({robot_styles.STYLES[args.style][0]})" if args.style else ""))
    srv = ThreadingHTTPServer((args.host, args.port),
                              make_handler(engine, device, child_default, {"preset": args.robot}, args.style))
    print(f"[tts] listening on http://{args.host}:{args.port}/tts  (laptop: python emo_server.py --tts-url http://<this-ip>:{args.port})")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nbye")


if __name__ == "__main__":
    main()
