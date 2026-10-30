"""Shared robot access for emo_server.py and emo_mcp.py.

Robot  - thin HTTP client for the firmware's API (API.md): status, motion, K210 controls,
         speaker. Knows how to speak a sentence through Windows TTS onto the robot without
         talking over an utterance that is still streaming (the K210 is half-duplex).
State  - what the voice server knows right now: robot IP, whether an utterance is
         streaming in, the live partial transcript, recent transcripts and replies.
         Threads publish into it; MCP tools and the Brain read from it.
"""
import base64
import collections
import io
import json
import os
import re
import socket
import subprocess
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import wave

PLAY_RATE = 48000             # what the robot's /api/play_pcm expects

# Windows text-to-speech, run in a fresh PowerShell each time so it can never hang the server.
TTS_PS = r"""
Add-Type -AssemblyName System.Speech
$s = New-Object System.Speech.Synthesis.SpeechSynthesizer
$voices = $s.GetInstalledVoices() | Where-Object Enabled | ForEach-Object VoiceInfo
$v = $null
if ($env:EMO_VOICE) { $v = $voices | Where-Object { $_.Name -like "*$env:EMO_VOICE*" } | Select-Object -First 1 }
if (-not $v -and $env:EMO_LANG) { $v = $voices | Where-Object { $_.Culture.TwoLetterISOLanguageName -eq $env:EMO_LANG } | Select-Object -First 1 }
if ($v) { $s.SelectVoice($v.Name) }
if ($env:EMO_RATE) { $s.Rate = [int]$env:EMO_RATE }
$fmt = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo 48000, ([System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen), ([System.Speech.AudioFormat.AudioChannel]::Mono)
$s.SetOutputToWaveFile($env:EMO_WAV, $fmt)
$s.Speak($env:EMO_TEXT)
$s.Dispose()
"""
TTS_CMD = ["powershell", "-NoProfile", "-NonInteractive", "-EncodedCommand",
           base64.b64encode(TTS_PS.encode("utf-16-le")).decode()]


TTS_URL = None                # set by emo_server --tts-url: EMO's cloned voice (host/voice/tts_server.py)


def _resample_to_play_rate(pcm_bytes, sr):
    import numpy as np
    x = np.frombuffer(pcm_bytes, dtype=np.int16)
    if sr == PLAY_RATE:
        return pcm_bytes
    n = int(len(x) * PLAY_RATE / sr)
    y = np.interp(np.arange(n) * sr / PLAY_RATE, np.arange(len(x)), x.astype(np.float32))
    return y.astype(np.int16).tobytes()


def remote_tts_pcm(text, lang=None, timeout=60):
    """POST /tts on the voice service -> 48 kHz mono int16 PCM bytes. Raises on any failure."""
    req = urllib.request.Request(TTS_URL.rstrip("/") + "/tts", data=json.dumps({"text": text, "lang": lang}).encode(),
                                 method="POST", headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        body = r.read()
    with wave.open(io.BytesIO(body), "rb") as w:
        if w.getnchannels() != 1 or w.getsampwidth() != 2:
            raise RuntimeError("voice service returned a non mono-16-bit WAV")
        return _resample_to_play_rate(w.readframes(w.getnframes()), w.getframerate())


def tts_pcm(text, lang=None, voice=None, rate=None):
    """Text -> 48 kHz mono int16 PCM. EMO's cloned voice when --tts-url is set and the
    service answers, otherwise Windows TTS."""
    if TTS_URL:
        try:
            return remote_tts_pcm(text, lang)
        except Exception as e:
            print(f"[tts] voice service {TTS_URL} failed ({type(e).__name__}: {e}); using the Windows voice")
    wav_path = os.path.join(tempfile.gettempdir(), f"emo_tts_{threading.get_ident()}.wav")
    env = dict(os.environ, EMO_TEXT=text, EMO_WAV=wav_path, EMO_LANG=lang or "",
               EMO_VOICE=voice or "", EMO_RATE="" if rate is None else str(rate))
    subprocess.run(TTS_CMD, env=env, timeout=60, check=True, capture_output=True)
    with wave.open(wav_path, "rb") as w:
        return w.readframes(w.getnframes())


class RobotError(RuntimeError):
    pass


class Robot:
    """HTTP client for one EMO-Bridge robot. `ip` may be filled in later (first utterance)."""

    def __init__(self, ip=None, timeout=5):
        self.ip = ip
        self.timeout = timeout
        self.lock = threading.Lock()          # one HTTP request at a time, the ESP32 is small

    def _url(self, path):
        if not self.ip:
            raise RobotError("robot IP unknown: start the server with --robot <ip>, "
                             "or wait for the robot's first utterance")
        return f"http://{self.ip}{path}"

    def _request(self, path, data=None, ctype="application/json", timeout=None):
        req = urllib.request.Request(self._url(path), data=data, method="POST" if data is not None else "GET",
                                     headers={"Content-Type": ctype} if data is not None else {})
        with self.lock:
            try:
                with urllib.request.urlopen(req, timeout=timeout or self.timeout) as r:
                    body = r.read()
            except urllib.error.HTTPError as e:            # the firmware answers 4xx with a JSON body
                body = e.read()
            except (urllib.error.URLError, OSError) as e:
                raise RobotError(f"robot {self.ip} unreachable: {e}") from None
        try:
            return json.loads(body.decode("utf-8", "replace")) if body[:1] in (b"{", b"[") else body.decode("utf-8", "replace")
        except json.JSONDecodeError:
            return body.decode("utf-8", "replace")

    def get(self, path):
        return self._request(path)

    def post(self, path, body):
        return self._request(path, json.dumps(body).encode())

    # ---- firmware endpoints ----
    def status(self):
        return self.get("/api/status")

    def motion_status(self):
        return self.get("/api/motion")

    def motion(self, action, **kw):
        return self.post("/api/motion", {"action": action, **kw})

    def controls(self):
        return self.get("/api/controls")

    def control(self, action, **kw):
        return self.post("/api/controls", {"action": action, **{k: v for k, v in kw.items() if v is not None}})

    def tone(self):
        return self._request("/api/tone", b"", "text/plain")

    def play_pcm(self, pcm):
        """Raw 48 kHz mono int16 LE -> robot speaker. Blocks for about the clip's duration."""
        seconds = len(pcm) / 2 / PLAY_RATE
        self._request("/api/play_pcm", pcm, "application/octet-stream", timeout=seconds + 20)
        return seconds

    def is_listening(self):
        """True while the robot's mic window is open (an utterance is being streamed)."""
        try:
            s = self.status()
            return bool(s.get("session") or s.get("mic_open"))
        except RobotError:
            return False

    # ---- keep the robot pointed at this machine ----
    def sync_host(self, port):
        """Make the robot stream its utterances to this PC: compares the firmware's saved
        voice-server address (/api/config host_ip/host_port) with our own IP as seen from the
        robot and the given port, and updates it when they differ (applies at the next
        utterance). Returns a message when something changed, None when it already matched."""
        me = local_ip_toward(self.ip)
        cfg = self.get("/api/config")
        if not isinstance(cfg, dict):
            raise RobotError(f"unexpected /api/config reply: {cfg!r}")
        if cfg.get("host_ip") == me and int(cfg.get("host_port") or 0) == int(port):
            return None
        r = self.post("/api/config", {"host_ip": me, "host_port": int(port)})
        if not (isinstance(r, dict) and r.get("ok")):
            raise RobotError(f"robot refused the new server address: {r}")
        return f"robot {self.ip} now streams to {me}:{port} (was {cfg.get('host_ip')}:{cfg.get('host_port')})"


def local_ip_toward(ip):
    """This machine's IPv4 address on the interface that reaches `ip` (no packet is sent)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((ip, 9))
        return s.getsockname()[0]
    finally:
        s.close()


def discover_robots(timeout=1.0):
    """IPs of EMO-Bridge robots among this machine's network neighbours (the ARP table:
    `arp -a` on Windows, `ip neigh` elsewhere), found by probing /api/status in parallel."""
    try:
        out = subprocess.run(["arp", "-a"], capture_output=True, text=True, timeout=5).stdout
    except Exception:
        try:
            out = subprocess.run(["ip", "neigh"], capture_output=True, text=True, timeout=5).stdout
        except Exception:
            return []
    cands = sorted({m for m in re.findall(r"\b(\d{1,3}(?:\.\d{1,3}){3})\b", out)
                    if not m.startswith(("224.", "239.", "255.", "127.", "0."))
                    and not m.endswith(".255")})
    found = []

    def probe(ip):
        try:
            with urllib.request.urlopen(f"http://{ip}/api/status", timeout=timeout) as r:
                j = json.loads(r.read().decode("utf-8", "replace"))
            if isinstance(j, dict) and "wake_events" in j and "bat_valid" in j:   # the firmware's signature
                found.append(ip)
        except Exception:
            pass

    threads = [threading.Thread(target=probe, args=(ip,), daemon=True) for ip in cands]
    for t in threads:
        t.start()
    for t in threads:
        t.join(timeout + 1)
    return sorted(found)


class State:
    """Live view of the voice server, shared by the receiver threads, the Brain and the MCP tools."""

    def __init__(self, robot):
        self.robot = robot
        self.cond = threading.Condition()
        self.streaming = False          # an utterance is coming in right now
        self.stream_ip = None
        self.stream_started = 0.0
        self.stream_seconds = 0.0       # audio received so far in this utterance
        self.partial = ""               # live partial transcript of that utterance
        self.transcripts = collections.deque(maxlen=50)   # dicts: time, ip, seconds, text, lang, reply
        self.serial = 0                 # bumps on every finished transcript (for listen())
        self.busy = ""                  # "transcribing" / "thinking" / "speaking" / ""

    # -- stream lifecycle (receiver thread) --
    def stream_begin(self, ip):
        with self.cond:
            self.streaming, self.stream_ip, self.stream_started = True, ip, time.time()
            self.stream_seconds, self.partial = 0.0, ""
            if not self.robot.ip:
                self.robot.ip = ip
            self.cond.notify_all()

    def stream_progress(self, seconds, partial=None):
        with self.cond:
            self.stream_seconds = seconds
            if partial is not None:
                self.partial = partial
            self.cond.notify_all()

    def stream_end(self):
        with self.cond:
            self.streaming = False
            self.cond.notify_all()

    def add_transcript(self, ip, seconds, text, lang):
        entry = {"time": time.strftime("%H:%M:%S"), "ip": ip, "seconds": round(seconds, 1),
                 "text": text, "lang": lang, "reply": None}
        with self.cond:
            self.transcripts.append(entry)
            self.serial += 1
            self.cond.notify_all()
        return entry

    def set_reply(self, entry, reply):
        with self.cond:
            entry["reply"] = reply
            self.cond.notify_all()

    def set_busy(self, what):
        with self.cond:
            self.busy = what
            self.cond.notify_all()

    # -- readers --
    def snapshot(self):
        with self.cond:
            return {
                "robot_ip": self.robot.ip,
                "streaming": self.streaming,
                "stream_seconds": round(self.stream_seconds, 1),
                "partial": self.partial,
                "busy": self.busy,
                "last": self.transcripts[-1] if self.transcripts else None,
            }

    def wait_stream_end(self, timeout):
        """Block while an utterance is streaming in. Returns True if the mic is free."""
        with self.cond:
            return self.cond.wait_for(lambda: not self.streaming, timeout=timeout)

    def wait_transcript(self, after_serial, timeout):
        with self.cond:
            if self.cond.wait_for(lambda: self.serial > after_serial, timeout=timeout):
                return self.transcripts[-1]
            return None

    # -- speaking without talking over the mic --
    def say(self, text, lang=None, voice=None, rate=None, wait_for_mic=True, timeout=20.0):
        """TTS + play on the robot. If an utterance is streaming, wait for it to end first
        (the K210 mutes the mic while the speaker runs). Returns seconds of audio played."""
        pcm = tts_pcm(text, lang, voice, rate)
        if wait_for_mic and not self.wait_stream_end(timeout):
            raise RobotError("the robot is still streaming an utterance; not talking over it")
        self.set_busy("speaking")
        try:
            return self.robot.play_pcm(pcm)
        finally:
            self.set_busy("")
