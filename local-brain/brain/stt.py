"""Speech-to-text for EMO on the desktop GPU: Qwen3-ASR (transformers, ROCm/CUDA) + Silero VAD.

Why Qwen3-ASR: 30 languages incl. English, Italian, German, language ID built in, and it was
trained (with RL) for noisy audio: on Qwen's ExtremeNoise test set it scores 16 % WER where
Whisper-large-v3 scores 63 % (arXiv 2601.21337, table 4). 1.7B params, ~4.5 GB VRAM in bf16;
the 0.6B (~1.8 GB) is the fallback when memory is tight.

Robustness layers on top of the model:
  * Silero VAD decides whether an utterance contains speech at all (a door, a servo, a TV jingle
    produce no transcript and no reply) and trims the non-speech around it, which removes most
    of what ASR models hallucinate on.
  * Language ID is restricted to the languages EMO is expected to hear (default en/it/de): the
    model's free detection is kept when it lands on one of them, otherwise the language with the
    highest score among the allowed ones is forced. Short utterances are where free LID fails
    (Italian heard as Spanish...), and a wrong language ruins the transcript.
  * Hotwords ("EMO", ...) go in the model's context prompt.

Streaming: the robot streams each utterance in chunks. While it arrives, a partial transcript is
recomputed over everything received so far every `partial_every_s` (a 1.7B pass over a few
seconds takes ~0.1-0.3 s on the RX 9070 XT, so partials stay live), and when the stream closes
one final pass runs over the VAD-trimmed audio (or the last partial is reused when only silence
arrived after it). Qwen3-ASR's vLLM-only native streaming is not used: vLLM does not run on
Windows, and full-prefix passes give offline accuracy anyway.
"""
import threading
import time

import numpy as np

SR = 16000
MAX_WINDOW_S = 28.0                   # Qwen3-ASR's feature extractor works in 30 s windows

LANG_NAMES = {"en": "English", "it": "Italian", "de": "German", "fr": "French", "es": "Spanish",
              "pt": "Portuguese", "nl": "Dutch", "pl": "Polish", "ru": "Russian", "ja": "Japanese",
              "zh": "Chinese", "ko": "Korean", "tr": "Turkish", "sv": "Swedish", "da": "Danish",
              "fi": "Finnish", "cs": "Czech", "el": "Greek", "hu": "Hungarian", "ro": "Romanian",
              "ar": "Arabic", "hi": "Hindi", "id": "Indonesian", "ms": "Malay", "th": "Thai",
              "vi": "Vietnamese", "fa": "Persian", "fil": "Filipino", "mk": "Macedonian", "yue": "Cantonese"}
NAME_TO_CODE = {v.lower(): k for k, v in LANG_NAMES.items()}


class VAD:
    """Silero VAD (ONNX on CPU, ~1 ms per 30 ms frame)."""

    def __init__(self, threshold=0.5):
        from silero_vad import get_speech_timestamps, load_silero_vad
        self.model = load_silero_vad(onnx=True)
        self._ts = get_speech_timestamps
        self.threshold = threshold

    def segments(self, audio):
        """[(start, end)] sample ranges that contain speech."""
        import torch
        if len(audio) < SR // 10:
            return []
        ts = self._ts(torch.from_numpy(np.ascontiguousarray(audio, dtype=np.float32)), self.model,
                      sampling_rate=SR, threshold=self.threshold, min_speech_duration_ms=200,
                      min_silence_duration_ms=300, speech_pad_ms=150)
        return [(t["start"], t["end"]) for t in ts]

    def trim(self, audio, pad_s=0.2):
        """(trimmed audio, seconds of speech). Empty array when there is no speech."""
        seg = self.segments(audio)
        if not seg:
            return audio[:0], 0.0
        a = max(0, seg[0][0] - int(pad_s * SR))
        b = min(len(audio), seg[-1][1] + int(pad_s * SR))
        return audio[a:b], sum(e - s for s, e in seg) / SR


class QwenASR:
    def __init__(self, model_id="Qwen/Qwen3-ASR-1.7B-hf", device="cuda", dtype="bfloat16",
                 languages=("en", "it", "de"), hotwords="EMO", features_on_device=True):
        import torch
        from transformers import AutoProcessor, Qwen3ASRForConditionalGeneration
        self.torch = torch
        self.device = device
        self.proc = AutoProcessor.from_pretrained(model_id)
        self.model = Qwen3ASRForConditionalGeneration.from_pretrained(      # no device_map: no accelerate needed
            model_id, dtype=getattr(torch, dtype), attn_implementation="sdpa").to(device).eval()
        self.dtype = getattr(torch, dtype)
        self.languages = [l for l in languages if l in LANG_NAMES]
        self.prompt = hotwords or None
        self.fe_device = device if features_on_device else "cpu"
        tok = self.proc.tokenizer
        self.prefix_ids = tok.encode("language", add_special_tokens=False)
        self.lang_tok = {}
        for code in self.languages:                     # first token of " English", " Italian"...
            ids = tok.encode("language " + LANG_NAMES[code], add_special_tokens=False)
            if ids[:len(self.prefix_ids)] != self.prefix_ids or len(ids) <= len(self.prefix_ids):
                raise RuntimeError(f"unexpected tokenisation of 'language {LANG_NAMES[code]}': {ids}")
            self.lang_tok[code] = ids[len(self.prefix_ids)]
        if len(set(self.lang_tok.values())) != len(self.lang_tok):
            raise RuntimeError(f"language names share a first token: {self.lang_tok}")

    def _inputs(self, audio, language):
        kw = {"processor_kwargs": {"audio_kwargs": {"device": self.fe_device}}} if self.fe_device != "cpu" else {}
        try:
            inp = self.proc.apply_transcription_request(audio, language=LANG_NAMES.get(language), prompt=self.prompt, **kw)
        except TypeError:                               # processor without the device kwarg
            self.fe_device = "cpu"
            inp = self.proc.apply_transcription_request(audio, language=LANG_NAMES.get(language), prompt=self.prompt)
        return inp.to(self.model.device, dtype=self.dtype)

    def detect(self, audio):
        """Constrained language ID: (code, {code: logit}) over self.languages."""
        torch = self.torch
        with torch.inference_mode():
            inp = self._inputs(audio, None)
            extra = torch.tensor([self.prefix_ids], device=inp["input_ids"].device, dtype=inp["input_ids"].dtype)
            ids = torch.cat([inp["input_ids"], extra], dim=1)
            mask = torch.cat([inp["attention_mask"], torch.ones_like(extra)], dim=1)
            out = self.model(input_ids=ids, attention_mask=mask, input_features=inp["input_features"],
                             input_features_mask=inp["input_features_mask"], logits_to_keep=1)
            logits = out.logits[0, -1].float()
        scores = {c: float(logits[t]) for c, t in self.lang_tok.items()}
        return max(scores, key=scores.get), scores

    def _generate(self, audio, language):
        torch = self.torch
        with torch.inference_mode():
            inp = self._inputs(audio, language)
            n_new = int(12 * len(audio) / SR) + 24            # ~3x normal speech rate, stops runaways
            out = self.model.generate(**inp, max_new_tokens=n_new, do_sample=False, repetition_penalty=1.05)
        new = out[0, inp["input_ids"].shape[1]:]
        raw = self.proc.decode(new, skip_special_tokens=True)
        if not language and raw.strip().lower().startswith("language none"):
            return "", "none"                                  # the model's own "no speech" answer
        parsed = self.proc.parse_output(raw)
        lang = language or NAME_TO_CODE.get((parsed.get("language") or "").strip().lower())
        return parsed.get("transcription", "").strip(), lang

    def transcribe(self, audio, language=None):
        """float32 16 kHz mono -> (text, language code). language=None: detect within self.languages."""
        audio = np.asarray(audio, dtype=np.float32)
        if len(audio) > MAX_WINDOW_S * SR:                  # long input: consecutive windows
            parts, lang = [], language
            for i in range(0, len(audio), int(MAX_WINDOW_S * SR)):
                t, lang = self.transcribe(audio[i:i + int(MAX_WINDOW_S * SR)], lang)
                parts.append(t)
            return " ".join(p for p in parts if p), lang
        if language:
            return self._generate(audio, language)
        text, lang = self._generate(audio, None)
        if lang == "none":
            return "", None
        if lang in self.languages or not self.languages:
            return text, lang
        forced, _ = self.detect(audio)                      # free LID left the allowed set
        return self._generate(audio, forced)


class StreamingTranscriber:
    """One utterance, transcribed while it arrives (same interface as the laptop's NPU version).

        st = StreamingTranscriber(asr, vad, gpu_lock)
        st.feed(pcm_f32_16k)            # receiver thread, per chunk
        st.maybe_update()               # side thread: partial pass when enough new audio arrived
        text, lang, info = st.finish()  # after the stream closed; text "" = no speech, ignore
    """

    def __init__(self, asr, vad, lock, language=None, partial_every_s=0.8, min_speech_s=0.3):
        self.asr, self.vad, self.lock, self.language = asr, vad, lock, language
        self.every = int(partial_every_s * SR)
        self.min_speech_s = min_speech_s
        self.buf = np.zeros(0, dtype=np.float32)
        self._buf_lock = threading.Lock()
        self.done = 0                     # samples covered by self.partial
        self.partial, self.lang = "", None
        self.passes = 0

    @property
    def seconds(self):
        return len(self.buf) / SR

    @property
    def text(self):
        return self.partial

    def feed(self, pcm_f32):
        with self._buf_lock:
            self.buf = np.concatenate([self.buf, np.asarray(pcm_f32, dtype=np.float32)])

    def maybe_update(self):
        if len(self.buf) - self.done < self.every or not self.lock.acquire(blocking=False):
            return False
        try:
            with self._buf_lock:
                snap = self.buf.copy()
            speech, sp_s = self.vad.trim(snap)
            if sp_s < self.min_speech_s:
                self.done = len(snap)
                return False
            # keep the language once ~1.5 s of speech decided it; the final pass re-decides
            lang = self.language or (self.lang if sp_s >= 1.5 else None)
            text, lang = self.asr.transcribe(speech, lang)
            self.partial, self.done = text, len(snap)
            if sp_s >= 1.5 or not self.lang:
                self.lang = lang
            self.passes += 1
            return True
        finally:
            self.lock.release()

    def finish(self):
        t0 = time.time()
        with self._buf_lock:
            buf = self.buf.copy()
        speech, sp_s = self.vad.trim(buf)
        info = {"audio_s": round(len(buf) / SR, 2), "speech_s": round(sp_s, 2), "reused": False}
        if sp_s < self.min_speech_s:
            info["final_s"] = round(time.time() - t0, 3)
            return "", None, info
        tail = buf[self.done:]
        if self.done and self.partial and not self.vad.segments(tail):
            info["reused"] = True                          # only silence since the last partial
            text, lang = self.partial, self.lang
        else:
            with self.lock:
                text, lang = self.asr.transcribe(speech, self.language)
            self.passes += 1
        info["final_s"] = round(time.time() - t0, 3)
        return text, lang, info
