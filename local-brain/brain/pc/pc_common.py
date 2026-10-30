"""Shared bits of EMO's PC control: the shared secret and request signing.

Every computer that EMO may control runs pc_agent.py; the brain runs pc_mcp.py. Both hold the same
secret (pc_secret.txt, created by pc_mcp.py on first start; copy it next to pc_agent.py on the other
PCs). Announcements and commands carry an HMAC-SHA256 signature over a timestamp and the body, so
nobody else on the network can announce a fake PC or send commands, and the secret never travels.
"""
import hashlib
import hmac
import json
import os
import secrets
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SECRET_FILE = os.path.join(HERE, "pc_secret.txt")
ANNOUNCE_PORT = 8799          # UDP broadcast: agents -> MCP server
AGENT_PORT = 8765             # HTTP: MCP server -> agent
MAX_SKEW_S = 120              # accepted clock difference between machines


def load_secret(path=SECRET_FILE, create=False):
    if os.path.exists(path):
        s = open(path, encoding="utf-8").read().strip()
        if s:
            return s.encode()
    if not create:
        raise SystemExit(f"no shared secret: copy pc_secret.txt from the EMO brain PC to {path}")
    s = secrets.token_urlsafe(32)
    with open(path, "w", encoding="utf-8") as f:
        f.write(s + "\n")
    return s.encode()


def sign(secret, ts, body: bytes):
    return hmac.new(secret, str(ts).encode() + b"." + body, hashlib.sha256).hexdigest()


def verify(secret, ts, body: bytes, sig):
    try:
        if abs(time.time() - float(ts)) > MAX_SKEW_S:
            return False
    except (TypeError, ValueError):
        return False
    return hmac.compare_digest(sign(secret, ts, body), sig or "")


def signed_headers(secret, body: bytes):
    ts = str(int(time.time()))
    return {"X-EMO-Ts": ts, "X-EMO-Sig": sign(secret, ts, body)}


def pack_announcement(secret, info: dict) -> bytes:
    body = json.dumps(info, separators=(",", ":"), sort_keys=True).encode()
    ts = str(int(time.time()))
    return json.dumps({"ts": ts, "sig": sign(secret, ts, body), "info": info}).encode()


def unpack_announcement(secret, data: bytes):
    """The announced info dict, or None when the signature or timestamp is wrong."""
    try:
        msg = json.loads(data)
        info = msg["info"]
        body = json.dumps(info, separators=(",", ":"), sort_keys=True).encode()
        return info if verify(secret, msg["ts"], body, msg["sig"]) else None
    except Exception:
        return None
