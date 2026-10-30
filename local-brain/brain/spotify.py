"""Spotify for EMO through go-librespot (a Spotify Connect player daemon, Premium account needed).

go-librespot plays on this PC's speakers (WASAPI) and is driven over its local REST API
(localhost:3678). One-time setup and login: spotify\\setup_spotify.bat. EMO starts the daemon itself
when it is set up; it also appears as "EMO" in the Spotify apps, so phones can cast to it.

Searching ("play some Daft Punk") uses the Spotify Web API search endpoint with the access token
of the daemon's own session (POST /token). If Spotify rate-limits that shared token (HTTP 429), put
the client id and secret of your own app from developer.spotify.com in spotify\\spotify_app.txt
(one line: <client id> <client secret>) and searches use that app instead.

While EMO listens and answers, the music is ducked to a fraction of its volume and restored after.
"""
import atexit
import difflib
import os
import re
import subprocess
import threading
import time
import unicodedata

import requests

HERE = os.path.dirname(os.path.abspath(__file__))
SPOTIFY_DIR = os.path.join(HERE, "spotify")
API = "https://api.spotify.com/v1"
KINDS = ("track", "artist", "album", "playlist")


def _norm(s):
    s = unicodedata.normalize("NFKD", s or "").encode("ascii", "ignore").decode().lower()
    return re.sub(r"[^a-z0-9]+", " ", s).strip()


class SpotifyError(RuntimeError):
    pass


class RateLimited(SpotifyError):
    pass


class Spotify:
    def __init__(self, exe=None, config_dir=None, url="http://localhost:3678", app_file=None,
                 duck_factor=0.25, log_path=None):
        self.exe, self.config_dir, self.url = exe, config_dir, url.rstrip("/")
        self.app_file = app_file
        self.duck_factor = duck_factor
        self.log_path = log_path or os.path.join(HERE, "go-librespot.log")
        self.proc = None
        self.local = requests.Session()
        self.local.trust_env = False                  # localhost: never through a proxy
        self.web = requests.Session()
        self._tok = (None, 0.0)
        self._app_tok = (None, 0.0)
        self._duck_lock = threading.Lock()
        self._duck_n = 0
        self._duck_restore = None                     # volume to restore, None = not ducked
        self._playlists = (None, 0.0)
        self._search_blocked = 0.0                    # shared-token search rate-limited until this time

    @classmethod
    def from_dir(cls, folder=SPOTIFY_DIR, **kw):
        """The player set up by setup_spotify.bat, or None."""
        cfg = os.path.join(folder, "config")
        exe = None
        for root, _, files in os.walk(os.path.join(folder, "bin")):
            for f in files:
                if f.lower().startswith("go-librespot") and f.lower().endswith(".exe"):
                    exe = os.path.join(root, f)
        if not exe or not os.path.exists(os.path.join(cfg, "config.yml")):
            return None
        return cls(exe, cfg, app_file=os.path.join(folder, "spotify_app.txt"), **kw)

    # ---------------- daemon ----------------
    def alive(self):
        try:
            return self.local.get(self.url + "/", timeout=1).status_code < 500
        except requests.RequestException:
            return False

    def start(self, wait_s=20):
        if self.alive():
            print("[spotify] reusing the go-librespot already running")
            return self
        if not self.exe:
            raise SpotifyError("go-librespot is not set up (run spotify\\setup_spotify.bat)")
        log = open(self.log_path, "w", encoding="utf-8", errors="replace")
        flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        self.proc = subprocess.Popen([self.exe, "--config_dir", self.config_dir], stdout=log,
                                     stderr=subprocess.STDOUT, creationflags=flags)
        atexit.register(self.stop)
        t0 = time.time()
        while time.time() - t0 < wait_s:
            if self.proc.poll() is not None:
                raise SpotifyError(f"go-librespot exited (code {self.proc.returncode}), see {self.log_path}")
            if self.alive():
                return self
            time.sleep(0.3)
        raise SpotifyError(f"go-librespot did not answer within {wait_s}s, see {self.log_path}")

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.proc = None

    def logged_in(self):
        try:
            return self.local.post(self.url + "/token", timeout=3).status_code == 200
        except requests.RequestException:
            return False

    # ---------------- REST ----------------
    def _get(self, path, **params):
        r = self.local.get(self.url + path, params=params or None, timeout=5)
        if r.status_code == 204:
            return None
        if r.status_code >= 400:
            raise SpotifyError(f"player {path}: HTTP {r.status_code} {r.text[:200]}")
        return r.json() if r.content else None

    def _post(self, path, body=None, timeout=10):
        r = self.local.post(self.url + path, json=body, timeout=timeout)
        if r.status_code >= 400:
            raise SpotifyError(f"player {path}: HTTP {r.status_code} {r.text[:200]}")
        return r.json() if r.content and r.headers.get("content-type", "").startswith("application/json") else None

    def status(self):
        return self._get("/status")

    def volume(self):
        v = self._get("/player/volume") or {}
        mx = v.get("max") or 100
        return round(100 * (v.get("value") or 0) / mx), mx

    def set_volume_pct(self, pct, relative=False):
        _, mx = self.volume()
        self._post("/player/volume", {"volume": round(pct * mx / 100), "relative": relative})

    # ---------------- search ----------------
    def _token(self):
        tok, exp = self._tok
        if tok and time.time() < exp:
            return tok
        r = self.local.post(self.url + "/token", timeout=5)
        if r.status_code != 200:
            raise SpotifyError("Spotify is not logged in (run spotify\\setup_spotify.bat)")
        tok = r.json()["token"]
        self._tok = (tok, time.time() + 600)          # the daemon refreshes it; re-ask every 10 min
        return tok

    def _app_token(self):
        if not self.app_file or not os.path.exists(self.app_file):
            return None
        tok, exp = self._app_tok
        if tok and time.time() < exp:
            return tok
        parts = open(self.app_file, encoding="utf-8").read().split()
        if len(parts) < 2:
            return None
        r = self.web.post("https://accounts.spotify.com/api/token", data={"grant_type": "client_credentials"},
                          auth=(parts[0], parts[1]), timeout=10)
        if r.status_code != 200:
            raise SpotifyError(f"spotify_app.txt rejected: HTTP {r.status_code} {r.text[:200]}")
        j = r.json()
        self._app_tok = (j["access_token"], time.time() + j.get("expires_in", 3600) - 60)
        return self._app_tok[0]

    def search(self, query, kinds=KINDS, limit=5):
        """{kind: [item, ...]} from the Web API (items may be None in Spotify's answers)."""
        app = self._app_token()
        if not app and time.time() < self._search_blocked:
            mins = max(1, round((self._search_blocked - time.time()) / 60))
            raise RateLimited(f"Spotify search is rate-limited for about {mins} more minutes")
        tok = app or self._token()
        params = {"q": query, "type": ",".join(kinds), "limit": limit}
        r = self.web.get(API + "/search", params=params, headers={"Authorization": "Bearer " + tok}, timeout=10)
        if r.status_code == 401 and tok == self._tok[0]:
            self._tok = (None, 0)
            r = self.web.get(API + "/search", params=params,
                             headers={"Authorization": "Bearer " + self._token()}, timeout=10)
        if r.status_code == 429:
            try:
                wait = int(r.headers.get("Retry-After", "60"))
            except ValueError:
                wait = 60
            if not app:
                self._search_blocked = time.time() + wait
            print(f"[spotify] search rate-limited by Spotify for {wait}s"
                  + ("" if app else " (shared token; your own app in spotify\\spotify_app.txt avoids this)"))
            raise RateLimited(f"Spotify search is rate-limited for about {max(1, round(wait / 60))} minutes")
        if r.status_code != 200:
            raise SpotifyError(f"Spotify search: HTTP {r.status_code} {r.text[:200]}")
        j = r.json()
        return {k: [i for i in (j.get(k + "s") or {}).get("items") or [] if i] for k in kinds}

    def my_playlists(self):
        pl, t = self._playlists
        if pl is not None and time.time() - t < 300:
            return pl
        try:
            pl = (self._get("/library/playlists", limit=200) or {}).get("items") or []
        except Exception:
            pl = []
        self._playlists = (pl, time.time())
        return pl

    @staticmethod
    def _label(kind, it):
        if kind == "track":
            return f"'{it['name']}' by {', '.join(a['name'] for a in it.get('artists', []))}"
        if kind == "album":
            return f"the album '{it['name']}' by {', '.join(a['name'] for a in it.get('artists', []))}"
        if kind == "artist":
            return f"music by {it['name']}"
        return f"the playlist '{it['name']}'"

    def resolve(self, query, kind="any"):
        """(uri, context uri or None, label) for a spoken request."""
        q = _norm(query)
        if kind in ("any", "playlist"):
            if q in ("liked songs", "my liked songs", "liked", "brani che ti piacciono", "lieblingssongs",
                     "my music", "la mia musica", "meine musik"):
                liked = self.liked()
                if liked:
                    return liked, None, "your liked songs"
            mine = self.my_playlists()
            names = {_norm(p["name"]): p for p in mine}
            hit = names.get(q) or (names[difflib.get_close_matches(q, list(names), 1, 0.8)[0]]
                                   if difflib.get_close_matches(q, list(names), 1, 0.8) else None)
            if hit:
                return hit["uri"], None, f"your playlist '{hit['name']}'"
        kinds = KINDS if kind == "any" else (kind,)
        res = self.search(query, kinds)
        if kind == "any":                              # a name said on its own: artist > album > playlist > track
            for k in ("artist", "album", "playlist"):
                for it in res.get(k, [])[:2]:
                    if _norm(it["name"]) == q:
                        return it["uri"], None, self._label(k, it)
            kind = "track" if res.get("track") else next((k for k in KINDS if res.get(k)), None)
            if not kind:
                raise SpotifyError(f"nothing found on Spotify for {query!r}")
        items = res.get(kind) or []
        if not items:
            raise SpotifyError(f"no {kind} found on Spotify for {query!r}")
        it = items[0]
        if kind == "track":                            # play inside its album so the music goes on after it
            album = (it.get("album") or {}).get("uri")
            return it["uri"], album, self._label(kind, it)
        return it["uri"], None, self._label(kind, it)

    def liked(self):
        """URI of the user's Liked Songs, or None."""
        user = (self.status() or {}).get("username") or self._state_username()
        return f"spotify:user:{user}:collection" if user else None

    def _state_username(self):
        """The logged-in username from go-librespot's state.json (when /status has no session yet)."""
        try:
            import json
            st = json.load(open(os.path.join(self.config_dir, "state.json"), encoding="utf-8"))
            cred = st.get("credentials") or {}
            return cred.get("username") or st.get("username")
        except Exception:
            return None

    # ---------------- actions (strings for the LLM) ----------------
    def play(self, query=None, kind="any"):
        if not query:
            st = self.status() or {}
            if st.get("track"):
                self._post("/player/resume")
                return "resumed"
            liked = self.liked()
            if not liked:
                raise SpotifyError("nothing to resume; ask what to play")
            self._post("/player/play", {"uri": liked}, timeout=20)
            return "now playing your liked songs"
        try:
            uri, context, label = self.resolve(query, kind)
        except RateLimited as e:                       # search down: still play something
            liked = self.liked()
            if not liked:
                raise
            self._post("/player/play", {"uri": liked}, timeout=20)
            return (f"{e}, so I could not look up {query!r}; playing the user's liked songs instead "
                    "(tell the user, briefly)")
        if context:
            self._post("/player/play", {"uri": context, "skip_to_uri": uri}, timeout=20)
        else:
            self._post("/player/play", {"uri": uri}, timeout=20)
        return f"now playing {label}"

    def queue(self, query):
        uri, _, label = self.resolve(query, "track")
        self._post("/player/add_to_queue", {"uri": uri})
        return f"queued {label}"

    def control(self, action):
        a = _norm(action).replace(" ", "_")
        st = self.status()
        if a in ("pause", "stop"):
            self._post("/player/pause")
            return "paused"
        if a in ("resume", "play", "continue"):
            if not st:
                return "error: nothing to resume, ask what to play"
            self._post("/player/resume")
            return "resumed"
        if a in ("next", "skip"):
            self._post("/player/next")
            return "skipped to the next track"
        if a in ("previous", "prev", "back"):
            self._post("/player/prev")
            return "went back"
        if a in ("restart", "from_start"):
            self._post("/player/seek", {"position": 0})
            return "restarted the track"
        if a in ("shuffle_on", "shuffle_off"):
            self._post("/player/shuffle_context", {"shuffle_context": a == "shuffle_on"})
            return a.replace("_", " ")
        if a in ("repeat_on", "repeat_off"):
            self._post("/player/repeat_context", {"repeat_context": a == "repeat_on"})
            return a.replace("_", " ")
        if a in ("repeat_track", "repeat_one"):
            self._post("/player/repeat_track", {"repeat_track": True})
            return "repeating this track"
        return f"error: unknown action {action!r}"

    def set_volume(self, level=None, change=None):
        with self._duck_lock:
            if self._duck_restore is not None:         # ducked while EMO talks: change what gets restored
                cur = self._duck_restore
                new = max(0, min(100, level if level is not None else cur + (change or 0)))
                self._duck_restore = new
                return f"music volume set to {round(new)}%"
        if level is not None:
            self.set_volume_pct(max(0, min(100, level)))
        elif change:
            cur, _ = self.volume()
            self.set_volume_pct(max(0, min(100, cur + change)))
        cur, _ = self.volume()
        return f"music volume {cur}%"

    def describe(self):
        st = self.status()
        if not st or not st.get("track"):
            return "Spotify: nothing playing"
        t = st["track"]
        state = "paused" if st.get("paused") else ("stopped" if st.get("stopped") else "playing")
        pos, dur = (t.get("position") or 0) // 1000, (t.get("duration") or 0) // 1000
        vol = self._duck_restore if self._duck_restore is not None else round(
            100 * (st.get("volume") or 0) / (st.get("volume_steps") or 100))
        out = (f"Spotify {state}: '{t.get('name')}' by {', '.join(t.get('artist_names') or [])}"
               f" ({t.get('album_name')}), {pos // 60}:{pos % 60:02d} of {dur // 60}:{dur % 60:02d}, volume {vol}%")
        if st.get("context_name"):
            out += f", from '{st['context_name']}'"
        if st.get("shuffle_context"):
            out += ", shuffle on"
        nt = st.get("next_track")
        if nt:
            out += f"; next: '{nt.get('name')}' by {', '.join(nt.get('artist_names') or [])}"
        return out

    # ---------------- ducking while EMO listens / talks ----------------
    def duck(self):
        """Lower the music (nested calls are counted); release() restores it when the last one ends."""
        with self._duck_lock:
            self._duck_n += 1
            if self._duck_n > 1 or self._duck_restore is not None:
                return
            try:
                st = self.local.get(self.url + "/status", timeout=1)
                if st.status_code != 200:
                    return
                st = st.json()
                if st.get("paused") or st.get("stopped") or not st.get("track"):
                    return
                steps = st.get("volume_steps") or 100
                cur = st.get("volume") or 0
                if cur <= 0:
                    return
                self._duck_restore = round(100 * cur / steps)
                self.local.post(self.url + "/player/volume",
                                json={"volume": round(cur * self.duck_factor)}, timeout=1)
            except requests.RequestException:
                pass

    def release(self):
        with self._duck_lock:
            self._duck_n = max(0, self._duck_n - 1)
            if self._duck_n or self._duck_restore is None:
                return
            pct, self._duck_restore = self._duck_restore, None
        try:
            self.set_volume_pct(pct)
        except Exception:
            pass


def make_spotify_tools(sp):
    from llm import Tool

    def guard(fn, *a, **k):
        try:
            return fn(*a, **k)
        except Exception as e:
            return f"error: {e}"

    def spotify_play(query: str = None, kind: str = "any"):
        return guard(sp.play, query, kind or "any")

    def spotify_control(action: str):
        return guard(sp.control, action)

    def spotify_status():
        return guard(sp.describe)

    def spotify_queue(query: str):
        return guard(sp.queue, query)

    def spotify_volume(level: float = None, change: float = None):
        if level is None and change is None:
            return guard(lambda: f"music volume {sp.volume()[0]}%")
        return guard(sp.set_volume, level, change)

    def spotify_playlists():
        def f():
            pl = sp.my_playlists()
            return ("the user's playlists: " + "; ".join(p["name"] for p in pl[:60])) if pl else "no saved playlists"
        return guard(f)

    return [
        Tool(spotify_play, "Spotify: play music on the speakers. query = what was asked (song, artist, album, "
                           "playlist or genre/mood, e.g. 'Bohemian Rhapsody', 'Daft Punk', 'lofi beats', "
                           "'my liked songs', a playlist of the user). Without query: resumes, or plays the user's liked "
                           "songs when nothing was playing; use that for a plain 'play some music'.",
             {"type": "object", "properties": {
                 "query": {"type": "string"},
                 "kind": {"type": "string", "enum": ["any", "track", "artist", "album", "playlist"],
                          "description": "what the query is: playlist for a genre or mood, else what the user said; default any"}}}),
        Tool(spotify_control, "Spotify: pause, resume, next, previous, restart, shuffle_on, shuffle_off, "
                              "repeat_on, repeat_off, repeat_track.",
             {"type": "object", "properties": {"action": {"type": "string", "enum": [
                 "pause", "resume", "next", "previous", "restart", "shuffle_on", "shuffle_off",
                 "repeat_on", "repeat_off", "repeat_track"]}}, "required": ["action"]}),
        Tool(spotify_status, "Spotify: what is playing now (song, artist, album, position, volume, next track)."),
        Tool(spotify_queue, "Spotify: add a song to the queue (plays after the current one).",
             {"type": "object", "properties": {"query": {"type": "string"}}, "required": ["query"]}),
        Tool(spotify_volume, "Spotify music volume (not your own voice): level = 0-100 percent, or change = "
                             "relative step such as +10 / -10.",
             {"type": "object", "properties": {"level": {"type": "number"}, "change": {"type": "number"}}}),
        Tool(spotify_playlists, "Spotify: the names of the user's saved playlists."),
    ]
