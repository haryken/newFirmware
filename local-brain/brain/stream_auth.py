"""Who may stream audio to EMO's port 7007 (safe to expose to the internet).

Rule: a connection is accepted only if it proves it holds the stream key, or comes from a trusted
address (this PC, the robot's LAN address, --trust ...). Everything else is closed before a single
byte of audio is processed, and an address that fails 5 times is blocked for 10 minutes.

Handshake (backward compatible with the current firmware):
    robot  -> server   hello, 19 bytes: "EMOA", version, <IBBQ rate, channels, bits, offset>
    version 1 (today's firmware): accepted from trusted addresses only.
    version 2:
    server -> robot    "EMOC" + 16 random bytes (nonce)
    robot  -> server   32 bytes HMAC-SHA256(key, nonce + hello)
    then the usual chunks / ACK0 replies.
A recorded handshake is useless to an attacker: every connection gets a new nonce.

The key is brain\\stream_key.txt (64 hex characters, created on first start); the robot firmware
needs the same 32 bytes. Note: the audio itself is not encrypted.
"""
import hashlib
import hmac
import os
import secrets
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
KEY_FILE = os.path.join(HERE, "stream_key.txt")
PRIVATE = ("10.", "192.168.", "127.") + tuple(f"172.{i}." for i in range(16, 32))


def load_key(path=KEY_FILE):
    if not os.path.exists(path):
        with open(path, "w", encoding="ascii") as f:
            f.write(secrets.token_hex(32) + "\n")
        print(f"[auth] created {path}: put the same key in the robot firmware")
    key = bytes.fromhex(open(path, encoding="ascii").read().strip())
    if len(key) != 32:
        raise SystemExit(f"{path} must hold 64 hex characters")
    return key


def _read_exact(conn, n):
    buf = bytearray()
    while len(buf) < n:
        part = conn.recv(n - len(buf))
        if not part:
            return None
        buf += part
    return bytes(buf)


class Gate:
    def __init__(self, key, trusted=(), trust_lan=False, robot_ip=lambda: None, max_fail=5, ban_s=600):
        self.key, self.trusted, self.trust_lan, self.robot_ip = key, set(trusted) | {"127.0.0.1"}, trust_lan, robot_ip
        self.max_fail, self.ban_s = max_fail, ban_s
        self.fails, self.banned = {}, {}
        self.lock = threading.Lock()

    def is_trusted(self, ip):
        return ip in self.trusted or ip == self.robot_ip() or (self.trust_lan and ip.startswith(PRIVATE))

    def _fail(self, ip, why):
        with self.lock:
            n = self.fails.get(ip, 0) + 1
            self.fails[ip] = n
            if n >= self.max_fail:
                self.banned[ip] = time.time() + self.ban_s
                self.fails.pop(ip, None)
                why += f"; blocked for {self.ban_s // 60} min"
        print(f"[auth] refused {ip}: {why}")
        return None

    def admit(self, conn, ip):
        """Runs the handshake. Returns the 19-byte hello for an accepted stream, None (connection to be
        closed) otherwise."""
        with self.lock:
            until = self.banned.get(ip, 0)
        if until > time.time():
            return None                                            # silently: no log flooding
        conn.settimeout(5)
        try:
            hello = _read_exact(conn, 19)
            if not hello or hello[:4] != b"EMOA":
                return self._fail(ip, "not an EMO audio stream")
            version = hello[4]
            if version == 2:
                nonce = secrets.token_bytes(16)
                conn.sendall(b"EMOC" + nonce)
                mac = _read_exact(conn, 32)
                want = hmac.new(self.key, nonce + hello, hashlib.sha256).digest()
                if not mac or not hmac.compare_digest(mac, want):
                    return self._fail(ip, "wrong key")
            elif not self.is_trusted(ip):
                return self._fail(ip, f"protocol v{version} without a key from an untrusted address")
        except OSError as e:
            return self._fail(ip, f"handshake failed ({e})")
        with self.lock:
            self.fails.pop(ip, None)
        return hello
