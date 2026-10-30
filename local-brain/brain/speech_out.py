"""Streamed speech output: the reply is spoken sentence by sentence while Gemma is still writing.

    sp = Speaker(robot_state, tts_url, style="06")
    turn = sp.turn(lang)            # one reply
    for frag in brain.reply_stream(...):
        turn.push(frag)             # fragments in, whole sentences out to the TTS
    turn.close(); turn.wait()

Three stages run in parallel: the splitter cuts complete sentences out of the token stream,
a synth thread turns each into 48 kHz PCM with Pocket TTS (CPU, separate process, ~4x real time),
and a play thread sends them to the robot one after the other (play_pcm blocks for the clip's
duration), so sentence 2 is synthesised while sentence 1 plays.
"""
import io
import json
import queue
import re
import threading
import time
import urllib.request
import wave

import numpy as np

PLAY_RATE = 48000

_EMOJI = re.compile("[\U0001F000-\U0001FAFF☀-➿️‍]")
_END = re.compile(r"([.!?…]+[\"')\]]*)\s+")      # only once the next word started: "3." may be "3.5"


def clean(text):
    text = _EMOJI.sub("", text)
    text = re.sub(r"[*_#`>|~]+", "", text)            # markdown that TTS would read out
    text = re.sub(r"\[[a-z]{2}\]\s*", "", text)        # a stray language tag
    return re.sub(r"\s+", " ", text).strip()


class SentenceSplitter:
    """Feed text fragments; get back speakable chunks as soon as they are complete. The first chunk
    may be cut at a comma once it is long enough, to start speaking sooner."""

    def __init__(self, first_min=24, max_len=160):
        self.buf, self.first, self.first_min, self.max_len = "", True, first_min, max_len

    def push(self, frag):
        self.buf += frag
        out = []
        while True:
            m = _END.search(self.buf)
            if m and m.end(1) >= 2:
                cut = m.end(1)
            elif self.first and len(self.buf) >= self.first_min and re.search(r"[,;:]\s", self.buf[self.first_min // 2:]):
                cut = self.first_min // 2 + re.search(r"[,;:]\s", self.buf[self.first_min // 2:]).end()
            elif len(self.buf) > self.max_len:
                sp = self.buf.rfind(" ", 0, self.max_len)
                cut = sp if sp > 40 else self.max_len
            else:
                break
            chunk, self.buf = clean(self.buf[:cut]), self.buf[cut:]
            if chunk:
                out.append(chunk)
                self.first = False
        return out

    def flush(self):
        chunk, self.buf = clean(self.buf), ""
        return [chunk] if chunk else []


def tts_pcm48(url, text, lang=None, style=None, timeout=60):
    body = {"text": text, "lang": lang}
    if style:
        body["style"] = style
    req = urllib.request.Request(url.rstrip("/") + "/tts", data=json.dumps(body).encode(), method="POST",
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        data = r.read()
    with wave.open(io.BytesIO(data), "rb") as w:
        sr, x = w.getframerate(), np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16)
    if sr != PLAY_RATE:
        n = int(len(x) * PLAY_RATE / sr)
        x = np.interp(np.arange(n) * sr / PLAY_RATE, np.arange(len(x)), x.astype(np.float32)).astype(np.int16)
    return x.tobytes()


class Turn:
    def __init__(self, speaker, lang):
        self.sp, self.lang = speaker, lang
        self.split = SentenceSplitter()
        self.texts = queue.Queue()
        self.pcms = queue.Queue(maxsize=3)
        self.t0 = time.time()
        self.first_audio_s = None
        self.spoken = []
        self.audio_s = 0.0
        self.queued = 0
        self._synth = threading.Thread(target=self._synth_loop, daemon=True)
        self._play = threading.Thread(target=self._play_loop, daemon=True)
        self._synth.start()
        self._play.start()

    def push(self, frag):
        for s in self.split.push(frag):
            self.texts.put(s)
            self.queued += 1

    def flush(self):
        """Speak what is buffered now (e.g. "Let me check" right before a slow tool call)."""
        for s in self.split.flush():
            self.texts.put(s)
            self.queued += 1

    def close(self):
        self.flush()
        self.texts.put(None)

    def _synth_loop(self):
        while True:
            s = self.texts.get()
            if s is None:
                self.pcms.put(None)
                return
            try:
                t = time.time()
                pcm = tts_pcm48(self.sp.tts_url, s, self.lang, self.sp.style)
                print(f"[tts] {len(pcm) / 2 / PLAY_RATE:4.1f}s in {time.time() - t:4.2f}s: {s!r}")
                self.pcms.put((s, pcm))
            except Exception as e:
                print(f"[tts] failed for {s!r}: {type(e).__name__}: {e}")

    def _play_loop(self):
        first = True
        while True:
            item = self.pcms.get()
            if item is None:
                return
            s, pcm = item
            if first:
                self.sp.state.wait_stream_end(20)        # the K210 is half-duplex: never talk over the mic
                self.first_audio_s = time.time() - self.t0
                first = False
            self.sp.state.set_busy("speaking")
            try:
                self.audio_s += self.sp.play(pcm)
                self.spoken.append(s)
            except Exception as e:
                print(f"[play] {type(e).__name__}: {e}")
            finally:
                self.sp.state.set_busy("")

    def wait(self, timeout=180):
        self._synth.join(timeout)
        self._play.join(timeout)


class Speaker:
    def __init__(self, state, tts_url, style=None):
        self.state, self.tts_url, self.style = state, tts_url, style
        self.play = state.robot.play_pcm

    def turn(self, lang):
        return Turn(self, lang)
