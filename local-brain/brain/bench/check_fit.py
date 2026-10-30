"""Do Gemma 4 12B + Qwen3-ASR fit together in 16 GB, and how fast is the loop? (run with ..\\.venv)

Starts llama-server like emo_local.py does, loads Qwen3-ASR next to it, then measures:
VRAM, ASR pass time on the real robot recordings, Gemma's time to first token and speed in
English / Italian / German, a tool call and the <skip> answer for background speech.
The robot tools are simulated (no robot needed).  -> bench\\results\\fit.md
"""
import glob
import json
import os
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
BRAIN = os.path.dirname(HERE)
sys.path.insert(0, BRAIN)


class FakeRobot:
    ip = "simulated"

    def motion_status(self):
        return {"playing": False, "pos": [90, 90, 90, 90],
                "list": ["d1_EmoDance", "d2_Robot", "d3_Happy", "look_around_1", "explore_foward1", "Byebye", "stay_wink_1"]}

    def motion(self, action, **kw):
        return {"ok": True, "action": action, **kw}

    def control(self, action, **kw):
        return {"ok": True, "action": action, **kw}

    def status(self):
        return {"bat_pct": 76, "bat_mv": 3950}


class FakeState:
    robot = FakeRobot()


def main():
    paths = json.load(open(os.path.join(BRAIN, "paths.json")))
    os.environ.setdefault("HF_HOME", os.path.join(paths["models"], "hf"))
    os.environ.setdefault("HF_HUB_OFFLINE", "1")
    import torch
    from emo_catalog import Calibration, move_help
    from emo_local import CLAUDE_PROMPT, SYSTEM_PROMPT, SKIP
    from claude_helper import ClaudeHelper, load_key
    from llm import Brain, LlamaServer
    from robot_tools import make_tools
    from stt import QwenASR, VAD
    from bench_stt import read16k

    out = ["# Fit and speed check", ""]
    gb = lambda b: b / 2 ** 30
    from gpu import vram_gb
    def free_b():
        used, tot, src = vram_gb()
        return (tot - used) * 2 ** 30, tot * 2 ** 30, src
    free0, total, src = free_b()
    out.append(f"GPU: {torch.cuda.get_device_name(0)}, {gb(total):.1f} GB; free before loading: {gb(free0):.1f} GB (measured via {src})")
    srv = LlamaServer(os.path.join(BRAIN, "llama", "llama-server.exe"),
                      os.path.join(paths["models"], "gemma-4-12B-it-qat-UD-Q4_K_XL.gguf"),
                      log_path=os.path.join(HERE, "results", "llama-server.log"))
    os.makedirs(os.path.join(HERE, "results"), exist_ok=True)
    t = time.time()
    srv.start()
    free1 = free_b()[0]
    out.append(f"Gemma 4 12B (QAT Q4_K_XL, 8k context) loaded in {time.time() - t:.0f}s: {gb(free0 - free1):.2f} GB")
    t = time.time()
    asr = QwenASR("Qwen/Qwen3-ASR-1.7B-hf", languages=("en", "it", "de"))
    vad = VAD()
    asr.transcribe(np.zeros(16000, np.float32), "en")
    free2 = free_b()[0]
    out.append(f"Qwen3-ASR-1.7B loaded in {time.time() - t:.0f}s: {gb(free1 - free2):.2f} GB "
               f"(peak allocated by torch {gb(torch.cuda.max_memory_allocated()):.2f} GB)")
    out.append(f"**Free VRAM with both resident: {gb(free2):.2f} GB of {gb(total):.1f} GB**")
    out.append("")

    # ---- ASR speed on the real recordings
    times = []
    for f in sorted(glob.glob(os.path.join(HERE, "real", "*.wav")))[:20]:
        x = read16k(f)
        sp, _ = vad.trim(x)
        t = time.time()
        text, lang = asr.transcribe(sp)
        times.append((time.time() - t, len(sp) / 16000))
    tt = np.array(times)
    out.append(f"ASR pass on 20 real robot clips ({tt[:, 1].mean():.1f}s speech on average): mean "
               f"{tt[:, 0].mean() * 1000:.0f} ms, max {tt[:, 0].max() * 1000:.0f} ms")
    out.append("")

    # ---- Gemma
    key = load_key()
    claude = ClaudeHelper(key) if key else None
    holder = {}
    tools = make_tools(FakeState(), Calibration(), claude, lambda: holder.get("b"))
    brain = Brain(srv.url, SYSTEM_PROMPT + move_help(Calibration()) + (CLAUDE_PROMPT if claude else ""), tools)
    holder["b"] = brain
    out.append("ask_claude: " + ("enabled (real Claude call, web search)" if claude else "no API key, not tested"))
    tests = [("en", "Hi EMO! How are you today?"), ("it", "Ciao EMO, che cosa ti piace fare?"),
             ("de", "Hallo EMO, erzähl mir etwas über dich."), ("en", "EMO, can you do a little dance for me?"),
             ("it", "e adesso le previsioni del tempo per domani sulle regioni del nord"),
             ("de", "Wie spät ist es eigentlich?")]
    if claude:
        tests += [("it", "EMO, che tempo farà domani a Zurigo?"), ("en", "Who won the last Formula 1 race?")]
    out += ["| lang | user | first token | total | tokens/s | reply |", "|---|---|---|---|---|---|"]
    for lang, text in tests:
        brain.history = []
        t = time.time()
        first, n, parts = None, 0, []
        for frag in brain.reply_stream(text, lang):
            first = first or time.time() - t
            n += 1
            parts.append(frag)
        total_s = time.time() - t
        reply = "".join(parts).strip().replace("|", "/")
        rate = n / max(total_s - (first or 0), 1e-3)
        out.append(f"| {lang} | {text} | {first or 0:.2f}s | {total_s:.2f}s | {rate:.0f} | {reply} |")
        print(out[-1], flush=True)
    srv.stop()
    out.append("")
    out.append("(tokens/s counts streamed fragments, about one token each; tool calls were simulated)")
    open(os.path.join(HERE, "results", "fit.md"), "w", encoding="utf-8").write("\n".join(out) + "\n")
    print("\n".join(out))


if __name__ == "__main__":
    sys.path.insert(0, HERE)
    main()
