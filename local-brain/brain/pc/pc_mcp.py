"""EMO PC-control MCP server.

Keeps the list of computers that announce themselves (pc_agent.py on each PC: signed UDP broadcast
on port 8799, or HTTP POST to port 8798) and offers three MCP tools:

    computers()                                  -> the computers + the commands they accept
    computer_command(computer, command, args)    -> runs one command on a computer
    computer_screenshot(computer, window, ...)   -> a screenshot; the last resort

MCP endpoint (streamable HTTP, this PC only): http://127.0.0.1:8790/mcp  - EMO's brain connects to it,
and any other MCP client (Claude Desktop, ...) can too.

    python pc_mcp.py [--port 8790]
"""
import argparse
import json
import os
import socket
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pc_common import ANNOUNCE_PORT, SECRET_FILE, load_secret, signed_headers, unpack_announcement  # noqa: E402

HTTP_ANNOUNCE_PORT = 8798
ONLINE_S = 70                       # agents announce every 20 s


class Registry:
    def __init__(self, secret):
        self.secret = secret
        self.pcs = {}               # name.lower() -> info
        self.lock = threading.Lock()
        self._cat = {}              # name -> (time, commands)
        self.local_ips = _local_ips()

    def announce(self, data, ip):
        info = unpack_announcement(self.secret, data)
        if not info or not info.get("name"):
            return False
        key = info["name"].lower()
        with self.lock:
            old = self.pcs.get(key)
            if ip == "127.0.0.1" and old and old["ip"] in self.local_ips:
                ip = old["ip"]                      # the same PC also announces over loopback
            info = dict(info, ip=ip, last_seen=time.time())
            self.pcs[key] = info
        if not old or old.get("ip") != ip or time.time() - old["last_seen"] > ONLINE_S:
            print(f"[pc] {info['name']} at {ip}:{info.get('port')} (user {info.get('user')}) is online")
        return True

    def online(self):
        now = time.time()
        with self.lock:
            return [p for p in self.pcs.values() if now - p["last_seen"] < ONLINE_S]

    def find(self, name):
        pcs = self.online()
        if not pcs:
            raise RuntimeError("no computer is online (is pc_agent running on it?)")
        if not name:
            here = [p for p in pcs if p["ip"] in self.local_ips]
            if len(pcs) == 1:
                return pcs[0]
            if here:
                return here[0]
            raise RuntimeError("several computers are online; say which: " + ", ".join(p["name"] for p in pcs))
        n = name.lower().strip()
        for p in pcs:
            if p["name"].lower() == n:
                return p
        part = [p for p in pcs if n in p["name"].lower() or p["name"].lower() in n]
        if len(part) == 1:
            return part[0]
        if n in ("this", "this pc", "here", "local", "my pc", "desktop", "computer", "pc"):
            here = [p for p in pcs if p["ip"] in self.local_ips]
            if here:
                return here[0]
        raise RuntimeError(f"no online computer called {name!r}; online: " + ", ".join(p["name"] for p in pcs))

    # ---------------- talking to agents ----------------
    def _call(self, pc, path, body=None, timeout=70):
        url = f"http://{pc['ip']}:{pc.get('port', 8765)}{path}"
        if body is None:
            u = urllib.parse.urlparse(path)
            sig_body = u.path.encode() + b"?" + u.query.encode()
            req = urllib.request.Request(url, headers=signed_headers(self.secret, sig_body))
        else:
            data = json.dumps(body).encode()
            req = urllib.request.Request(url, data=data, method="POST",
                                         headers={"Content-Type": "application/json", **signed_headers(self.secret, data)})
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                return r.read(), r.headers.get("Content-Type", "")
        except urllib.error.HTTPError as e:
            raise RuntimeError(f"{pc['name']}: HTTP {e.code} {e.read()[:200].decode(errors='replace')}")
        except OSError as e:
            raise RuntimeError(f"{pc['name']} did not answer ({e}); is its firewall letting port {pc.get('port')} in?")

    def commands(self, pc):
        t, cmds = self._cat.get(pc["name"], (0, None))
        if cmds is None or time.time() - t > 300:
            data, _ = self._call(pc, "/info", timeout=10)
            cmds = json.loads(data)["commands"]
            self._cat[pc["name"]] = (time.time(), cmds)
        return cmds

    def run(self, pc, command, args):
        data, _ = self._call(pc, "/cmd", {"command": command, "args": args or {}})
        return json.loads(data)

    def screenshot(self, pc, window=None):
        q = "?" + urllib.parse.urlencode({"window": window}) if window else ""
        data, ctype = self._call(pc, "/screenshot" + q, timeout=40)
        if not ctype.startswith("image/"):
            raise RuntimeError(json.loads(data).get("error", "screenshot failed"))
        return data


def _local_ips():
    ips = {"127.0.0.1"}
    try:
        for res in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ips.add(res[4][0])
    except OSError:
        pass
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("192.0.2.1", 9))                 # no packet is sent; picks the LAN interface
        ips.add(s.getsockname()[0])
        s.close()
    except OSError:
        pass
    return ips


def udp_listener(reg, port=ANNOUNCE_PORT):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", port))
    while True:
        data, (ip, _) = s.recvfrom(65535)
        if not reg.announce(data, ip):
            print(f"[pc] ignored an announcement from {ip} with a bad signature (different pc_secret.txt?)")


def http_listener(reg, port=HTTP_ANNOUNCE_PORT):
    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def do_POST(self):
            ok = self.path == "/announce" and reg.announce(
                self.rfile.read(int(self.headers.get("Content-Length") or 0)), self.client_address[0])
            self.send_response(200 if ok else 403)
            self.end_headers()
    ThreadingHTTPServer(("0.0.0.0", port), H).serve_forever()


def describe_commands(cmds):
    lines, group = [], None
    for c in cmds:
        if c["group"] != group:
            group = c["group"]
            lines.append(f"[{group}]")
        args = ", ".join(a + ("?" if "(optional)" in d else "") for a, d in c["args"].items())
        detail = "; ".join(f"{a}: {d.replace(' (optional)', '')}" for a, d in c["args"].items())
        lines.append(f"- {c['command']}({args}): {c['description']}" + (f" [{detail}]" if detail else ""))
    return "\n".join(lines)


def build(reg, port):
    from mcp.server.fastmcp import FastMCP, Image
    mcp = FastMCP("emo-pc", host="127.0.0.1", port=port, log_level="WARNING", instructions=(
        "Control the user's Windows computers with commands. Call computers() first: it lists the computers "
        "and the commands they accept. Then use computer_command. Use computer_screenshot only when no "
        "command can tell you what you need."))

    @mcp.tool()
    def computers() -> str:
        """List the computers you can control (online/offline) and the commands they accept. Call this first
        whenever you are asked to do something on a computer, then use computer_command."""
        now = time.time()
        with reg.lock:
            pcs = list(reg.pcs.values())
        if not pcs:
            return "No computer has announced itself yet: pc_agent must run on the PCs to control."
        out = ["Computers:"]
        for p in sorted(pcs, key=lambda p: p["name"]):
            on = now - p["last_seen"] < ONLINE_S
            here = " - the PC EMO runs on" if p["ip"] in reg.local_ips else ""
            st = ("online" if on else "offline") + (", control paused by its user" if on and p.get("paused") else "")
            out.append(f"- {p['name']} ({st}, user {p.get('user') or '?'}{here})")
        online = reg.online()
        if online:
            try:
                out += ["", "Commands for computer_command(computer, command, args) - args is an object, "
                        "? marks optional ones:", describe_commands(reg.commands(online[0]))]
            except Exception as e:
                out.append(f"(could not read the command list: {e})")
        return "\n".join(out)

    @mcp.tool()
    def computer_command(computer: str, command: str, args: dict | None = None) -> str:
        """Run one command (from computers()) on a computer. computer: its name ('' when only one is online
        or for the PC EMO runs on). args: the command's arguments as an object, e.g. {"target": "weather forecast"}."""
        try:
            pc = reg.find(computer)
            r = reg.run(pc, command, args or {})
        except Exception as e:
            return f"error: {e}"
        if not r.get("ok"):
            return f"error on {pc['name']}: {r.get('error')}"
        res = r.get("result")
        return res if isinstance(res, str) else json.dumps(res, ensure_ascii=False)

    @mcp.tool()
    def computer_screenshot(computer: str = "", window: str | None = None):
        """LAST RESORT: a picture of a computer's screen (or of one window). Only when the commands
        (list_windows, browser_read, media_status...) cannot tell you what you need."""
        try:
            pc = reg.find(computer)
            jpg = reg.screenshot(pc, window)
        except Exception as e:
            return f"error: {e}"
        return [f"Screenshot of {pc['name']}" + (f", window {window!r}" if window else ""),
                Image(data=jpg, format="jpeg")]

    return mcp


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--port", type=int, default=8790, help="MCP port (bound to 127.0.0.1)")
    p.add_argument("--secret-file", default=SECRET_FILE)
    args = p.parse_args()
    created = not os.path.exists(args.secret_file)
    secret = load_secret(args.secret_file, create=True)
    if created:
        print(f"[pc] created {args.secret_file}: copy it next to pc_agent.py on every other PC")
    reg = Registry(secret)
    threading.Thread(target=udp_listener, args=(reg,), daemon=True).start()
    threading.Thread(target=http_listener, args=(reg,), daemon=True).start()
    print(f"[pc] MCP on http://127.0.0.1:{args.port}/mcp; listening for PCs on UDP {ANNOUNCE_PORT} and "
          f"HTTP {HTTP_ANNOUNCE_PORT}", flush=True)
    build(reg, args.port).run(transport="streamable-http")


if __name__ == "__main__":
    main()
