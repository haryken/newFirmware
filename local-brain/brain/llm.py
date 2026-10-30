"""Gemma 4 12B through llama.cpp's llama-server (OpenAI-compatible, Vulkan on the RX 9070 XT).

LlamaServer  starts/stops llama-server.exe with the GGUF fully on the GPU.
Brain        one conversation: streams the reply text as it is generated (so speech can start
             after the first sentence) and runs robot tool calls in between (walk, dance...).
"""
import atexit
import json
import os
import subprocess
import threading
import time
import urllib.request

import requests


class LlamaServer:
    def __init__(self, exe, gguf, port=8080, ctx=8192, extra=(), log_path="llama-server.log"):
        self.exe, self.gguf, self.port, self.ctx = exe, gguf, port, ctx
        self.extra = list(extra)
        self.log_path = log_path
        self.proc = None
        self.reused = False
        self.url = f"http://127.0.0.1:{port}"

    def alive(self):
        try:
            with urllib.request.urlopen(self.url + "/health", timeout=2) as r:
                return r.status == 200
        except Exception:
            return False

    def start(self, timeout=240):
        if self.alive():
            print(f"[llm] llama-server already running on {self.url} (left over from an earlier run?) - using it")
            self.reused = True
            return self
        cmd = [self.exe, "-m", self.gguf, "--host", "127.0.0.1", "--port", str(self.port),
               "-ngl", "999",                    # every layer on the GPU
               "-c", str(self.ctx), "-np", "1",  # one conversation, 8k context
               "-fa", "on",                      # flash attention: smaller, faster KV cache
               "--jinja",                        # the model's own chat template (tool calls)
               "--reasoning-budget", "0",        # no thinking phase: EMO answers straight away
               "--no-webui", "-lv", "4"] + self.extra   # -lv 4: log where the buffers went
        print(f"[llm] starting {os.path.basename(self.exe)} ({os.path.basename(self.gguf)}), log: {self.log_path}")
        self._log = open(self.log_path, "w", encoding="utf-8", errors="replace")
        flags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
        self.proc = subprocess.Popen(cmd, stdout=self._log, stderr=subprocess.STDOUT, creationflags=flags)
        atexit.register(self.stop)                 # also when the server crashes during start-up
        t0 = time.time()
        while time.time() - t0 < timeout:
            if self.proc.poll() is not None:
                raise RuntimeError(f"llama-server exited with code {self.proc.returncode}; see {self.log_path}")
            if self.alive():
                print(f"[llm] ready in {time.time() - t0:.0f}s")
                return self
            time.sleep(0.5)
        raise RuntimeError(f"llama-server not ready after {timeout}s; see {self.log_path}")

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                self.proc.kill()


class Tool:
    """A callable the model may use: name, description, JSON-schema parameters, python function."""

    def __init__(self, fn, description, parameters=None):
        self.fn, self.name, self.description = fn, fn.__name__, description
        self.parameters = parameters or {"type": "object", "properties": {}}

    def schema(self):
        return {"type": "function", "function": {"name": self.name, "description": self.description,
                                                 "parameters": self.parameters}}


class Brain:
    FORGET_AFTER_S = 600

    def __init__(self, url, system, tools=(), temperature=1.0, top_p=0.95, top_k=64, max_tokens=400,
                 max_tool_rounds=6, timeout=120, on_tool=None):
        self.url = url.rstrip("/") + "/v1/chat/completions"
        self.system = system
        self.tools = {t.name: t for t in tools}
        self.sampling = {"temperature": temperature, "top_p": top_p, "top_k": top_k, "max_tokens": max_tokens}
        self.max_tool_rounds = max_tool_rounds
        self.timeout = timeout
        self.history = []
        self.last = 0.0
        self.session = requests.Session()
        self.on_tool = on_tool            # called with the tool name before it runs (filler speech)
        self.lang = None                  # language of the turn being answered

    def _stream(self, messages):
        """One streamed completion. Yields ("text", str) deltas; returns via StopIteration the
        (content, tool_calls, finish_reason) of the whole message."""
        body = {"messages": messages, "stream": True, **self.sampling,
                "chat_template_kwargs": {"enable_thinking": False}}
        if self.tools:
            body["tools"] = [t.schema() for t in self.tools.values()]
        content, calls, finish = [], {}, None
        with self.session.post(self.url, json=body, stream=True, timeout=self.timeout) as r:
            if r.status_code != 200:
                raise RuntimeError(f"llama-server HTTP {r.status_code}: {r.text[:300]}")
            for line in r.iter_lines(decode_unicode=True):
                if not line or not line.startswith("data:"):
                    continue
                data = line[5:].strip()
                if data == "[DONE]":
                    break
                ch = json.loads(data)["choices"][0]
                d = ch.get("delta") or {}
                if d.get("content"):
                    content.append(d["content"])
                    yield "text", d["content"]
                for tc in d.get("tool_calls") or []:
                    slot = calls.setdefault(tc.get("index", 0), {"id": None, "name": "", "args": ""})
                    slot["id"] = tc.get("id") or slot["id"]
                    f = tc.get("function") or {}
                    slot["name"] += f.get("name") or ""
                    slot["args"] += f.get("arguments") or ""
                finish = ch.get("finish_reason") or finish
        return "".join(content), [calls[i] for i in sorted(calls)], finish

    def _run_tool(self, call):
        tool = self.tools.get(call["name"])
        if not tool:
            return f"error: unknown tool {call['name']}"
        try:
            args = json.loads(call["args"] or "{}")
            return str(tool.fn(**args))
        except Exception as e:
            return f"error: {type(e).__name__}: {e}"

    def reply_stream(self, text, lang=None):
        """Yields the spoken reply in text fragments as Gemma writes it. Tool calls run in between
        (their results feed the next round). The finished exchange is kept as context."""
        if time.time() - self.last > self.FORGET_AFTER_S:
            self.history = []
        self.last = time.time()
        self.lang = lang
        user = text if not lang else f"[{lang}] {text}"
        messages = [{"role": "system", "content": self.system}] + self.history + [{"role": "user", "content": user}]
        spoken = []
        for _ in range(self.max_tool_rounds):
            gen = self._stream(messages)
            try:
                while True:
                    kind, val = next(gen)
                    spoken.append(val)
                    yield val
            except StopIteration as stop:
                content, calls, finish = stop.value
            if not calls:
                break
            messages.append({"role": "assistant", "content": content or None,
                             "tool_calls": [{"id": c["id"] or f"call_{i}", "type": "function",
                                             "function": {"name": c["name"], "arguments": c["args"] or "{}"}}
                                            for i, c in enumerate(calls)]})
            for i, c in enumerate(calls):
                if self.on_tool:
                    try:
                        self.on_tool(c["name"])
                    except Exception as e:
                        print(f"[tool] on_tool hook failed: {e}")
                t0 = time.time()
                result = self._run_tool(c)
                print(f"[tool] {c['name']}({c['args']}) -> {result[:120]} ({time.time() - t0:.1f}s)")
                messages.append({"role": "tool", "tool_call_id": c["id"] or f"call_{i}", "content": result})
        answer = "".join(spoken).strip()
        if answer:
            self.history += [{"role": "user", "content": user}, {"role": "assistant", "content": answer}]
            self.history = self.history[-24:]
