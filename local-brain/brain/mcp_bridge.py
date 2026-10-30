"""Gives Gemma the tools of an MCP server (here: pc/pc_mcp.py, the PC control server).

Each MCP tool becomes an llm.Tool with the server's own name, description and JSON schema. Calls run
over streamable HTTP in a fresh short session (local, a few tens of ms). Images in a result (the
screenshot tool) cannot go to Gemma as text, so `on_image(jpeg_bytes, tool_args)` turns them into a
description (Claude's vision when a key is set) and that text is returned instead.
"""
import asyncio
import base64
import json
import os
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))


def _port_open(port, host="127.0.0.1"):
    try:
        with socket.create_connection((host, port), timeout=0.5):
            return True
    except OSError:
        return False


class PcServer:
    """Starts pc/pc_mcp.py (unless one already runs) and stops it at exit."""

    def __init__(self, port=8790, log_path=os.path.join(HERE, "pc-mcp.log")):
        self.port, self.log_path, self.proc, self.reused = port, log_path, None, False
        self.url = f"http://127.0.0.1:{port}/mcp"

    def start(self, wait_s=30):
        if _port_open(self.port):
            self.reused = True
            print(f"[pc] using the PC-control MCP server already running on port {self.port}")
            return self
        log = open(self.log_path, "w", encoding="utf-8", errors="replace")
        env = dict(os.environ, PYTHONUNBUFFERED="1")
        self.proc = subprocess.Popen([sys.executable, os.path.join(HERE, "pc", "pc_mcp.py"), "--port", str(self.port)],
                                     stdout=log, stderr=subprocess.STDOUT, env=env,
                                     creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        t0 = time.time()
        while time.time() - t0 < wait_s:
            if self.proc.poll() is not None:
                raise RuntimeError(f"pc_mcp.py exited (code {self.proc.returncode}), see {self.log_path}")
            if _port_open(self.port):
                return self
            time.sleep(0.3)
        raise RuntimeError(f"pc_mcp.py did not start within {wait_s}s, see {self.log_path}")

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()


class McpBridge:
    def __init__(self, url, on_image=None, timeout=90):
        self.url, self.on_image, self.timeout = url, on_image, timeout

    async def _session(self, fn):
        from mcp import ClientSession
        from mcp.client.streamable_http import streamablehttp_client
        async with streamablehttp_client(self.url, timeout=self.timeout) as (r, w, _):
            async with ClientSession(r, w) as s:
                await s.initialize()
                return await fn(s)

    def list_tools(self):
        async def f(s):
            return (await s.list_tools()).tools
        return asyncio.run(self._session(f))

    def call(self, name, args):
        async def f(s):
            return await s.call_tool(name, args)
        res = asyncio.run(self._session(f))
        out = []
        for c in res.content:
            if c.type == "text":
                out.append(c.text)
            elif c.type == "image":
                img = base64.b64decode(c.data)
                if self.on_image:
                    out.append(self.on_image(img, args))
                else:
                    out.append("(a screenshot was taken, but no vision model is available to look at it)")
        text = "\n".join(out)
        return ("error: " + text) if res.isError and not text.startswith("error") else text

    def tools(self):
        """MCP tools as llm.Tool objects."""
        from llm import Tool
        tools = []
        for t in self.list_tools():
            def fn(_name=t.name, **kw):
                return self.call(_name, {k: v for k, v in kw.items() if v is not None})
            fn.__name__ = t.name
            schema = dict(t.inputSchema or {"type": "object", "properties": {}})
            schema.pop("title", None)
            for prop in schema.get("properties", {}).values():
                prop.pop("title", None)
            tools.append(Tool(fn, (t.description or "").strip(), schema))
        return tools
