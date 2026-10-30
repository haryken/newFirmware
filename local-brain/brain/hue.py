"""Philips Hue control for EMO's LLM, over the bridges' local CLIP v2 API (HTTPS, LAN only).

Keys come from hue_key.txt (hue_pair.bat writes it): one line per bridge
    <ip> <application key> [<bridge id>] [<name>]
Several bridges are merged: lights, rooms, zones and scenes from all of them, addressed by name.
"""
import colorsys
import difflib
import os
import re
import threading
import time
import unicodedata

import requests
import urllib3

HERE = os.path.dirname(os.path.abspath(__file__))
urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)   # bridges use a self-signed cert

# colour words (en/it/de) -> RGB; the LLM usually passes English, but the user's words may come through
COLORS = {
    "red": (255, 0, 0), "rosso": (255, 0, 0), "rot": (255, 0, 0),
    "orange": (255, 110, 0), "arancione": (255, 110, 0),
    "yellow": (255, 210, 0), "giallo": (255, 210, 0), "gelb": (255, 210, 0),
    "green": (0, 255, 0), "verde": (0, 255, 0), "grün": (0, 255, 0), "grun": (0, 255, 0),
    "cyan": (0, 255, 255), "turquoise": (64, 224, 208), "turchese": (64, 224, 208), "türkis": (64, 224, 208),
    "blue": (0, 60, 255), "blu": (0, 60, 255), "azzurro": (80, 170, 255), "blau": (0, 60, 255),
    "light blue": (80, 170, 255), "hellblau": (80, 170, 255),
    "purple": (140, 0, 255), "viola": (140, 0, 255), "lila": (160, 60, 255), "violett": (140, 0, 255),
    "magenta": (255, 0, 200), "pink": (255, 80, 160), "rosa": (255, 80, 160), "fucsia": (255, 0, 200),
    "white": (255, 255, 255), "bianco": (255, 255, 255), "weiß": (255, 255, 255), "weiss": (255, 255, 255),
}
WHITES = {"warm": 2700, "warm white": 2700, "caldo": 2700, "warmweiß": 2700, "candle": 2200, "candela": 2200,
          "neutral": 4000, "neutro": 4000, "cool": 5500, "cool white": 5500, "freddo": 5500, "kalt": 5500,
          "daylight": 6500, "luce diurna": 6500, "tageslicht": 6500}


def _norm(s):
    s = unicodedata.normalize("NFKD", s or "").encode("ascii", "ignore").decode().lower()
    return re.sub(r"[^a-z0-9]+", " ", s).strip()


def rgb_to_xy(r, g, b):
    """sRGB 0-255 -> CIE 1931 xy (wide gamut D65, as in Philips' own guide)."""
    def lin(c):
        c /= 255.0
        return ((c + 0.055) / 1.055) ** 2.4 if c > 0.04045 else c / 12.92
    r, g, b = lin(r), lin(g), lin(b)
    X = r * 0.4124 + g * 0.3576 + b * 0.1805
    Y = r * 0.2126 + g * 0.7152 + b * 0.0722
    Z = r * 0.0193 + g * 0.1192 + b * 0.9505
    s = X + Y + Z
    return (round(X / s, 4), round(Y / s, 4)) if s else (0.3227, 0.329)


def parse_color(color):
    """'red' / 'rosso' / '#ff8800' / 'warm' / '3000K' -> ('xy', (x, y)) or ('mirek', m)."""
    c = _norm(color)
    m = re.fullmatch(r"(\d{4,5}) ?k?", c)
    if m:
        return "mirek", max(153, min(500, round(1e6 / int(m.group(1)))))
    if c in WHITES:
        return "mirek", round(1e6 / WHITES[c])
    if c in COLORS or color.lower() in COLORS:
        rgb = COLORS.get(c) or COLORS[color.lower()]
        if rgb == (255, 255, 255):
            return "mirek", 233                        # "white": neutral white is nicer than xy white
        return "xy", rgb_to_xy(*rgb)
    m = re.fullmatch(r"#?([0-9a-fA-F]{6})", color.strip())
    if m:
        h = m.group(1)
        return "xy", rgb_to_xy(int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16))
    close = difflib.get_close_matches(c, list(COLORS) + list(WHITES), n=1, cutoff=0.75)
    if close:
        return parse_color(close[0])
    raise ValueError(f"unknown colour {color!r}; use a colour name (red, blue, warm, cool...), #rrggbb or e.g. 2700K")


class Bridge:
    def __init__(self, ip, key, bid="", name="", scheme="https"):
        self.ip, self.key, self.id, self.name = ip, key, bid, name.replace("_", " ") or ip
        self.base = f"{scheme}://{ip}/clip/v2/resource"
        self.s = requests.Session()
        self.s.headers["hue-application-key"] = key
        self.s.trust_env = False          # LAN device: no proxies, and REQUESTS_CA_BUNDLE must not re-enable checks

    def get(self, rtype):
        r = self.s.get(f"{self.base}/{rtype}", timeout=5, verify=False)
        r.raise_for_status()
        return r.json().get("data", [])

    def put(self, rtype, rid, body):
        r = self.s.put(f"{self.base}/{rtype}/{rid}", json=body, timeout=5, verify=False)
        if r.status_code >= 400:
            errs = "; ".join(e.get("description", "") for e in (r.json().get("errors") or [])) if r.content else ""
            raise RuntimeError(f"bridge {self.name}: HTTP {r.status_code} {errs}")
        return r.json()


class Hue:
    """All paired bridges as one set of named targets: lights, rooms, zones, scenes."""

    def __init__(self, bridges, refresh_s=60):
        self.bridges = bridges
        self.refresh_s = refresh_s
        self.lock = threading.Lock()
        self.loaded = 0.0
        self.targets = []        # dicts: kind, name, bridge, rtype, rid, lights(list of light dicts)
        self.scenes = []         # dicts: name, group name, bridge, rid

    @classmethod
    def from_file(cls, path=os.path.join(HERE, "hue_key.txt")):
        if not os.path.exists(path):
            return None
        bridges = []
        for line in open(path, encoding="utf-8-sig"):
            p = line.split()
            if len(p) >= 2 and not line.lstrip().startswith("#"):
                bridges.append(Bridge(p[0], p[1], p[2] if len(p) > 2 else "", p[3] if len(p) > 3 else ""))
        return cls(bridges) if bridges else None

    # ---------------- inventory ----------------
    def refresh(self, force=False):
        with self.lock:
            if not force and time.time() - self.loaded < self.refresh_s:
                return
            targets, scenes = [], []
            for b in self.bridges:
                try:
                    lights = {l["id"]: l for l in b.get("light")}
                    rooms, zones = b.get("room"), b.get("zone")
                    devices = {d["id"]: d for d in b.get("device")}
                    sc = b.get("scene")
                except Exception as e:
                    print(f"[hue] bridge {b.name} ({b.ip}) unreachable: {type(e).__name__}: {e}")
                    continue
                for l in lights.values():
                    targets.append({"kind": "light", "name": l["metadata"]["name"], "bridge": b,
                                    "rtype": "light", "rid": l["id"], "lights": [l]})
                groups = {}
                for kind, items in (("room", rooms), ("zone", zones)):
                    for g in items:
                        ls = []
                        for ch in g.get("children", []):
                            if ch["rtype"] == "light" and ch["rid"] in lights:
                                ls.append(lights[ch["rid"]])
                            elif ch["rtype"] == "device" and ch["rid"] in devices:
                                ls += [lights[s["rid"]] for s in devices[ch["rid"]].get("services", [])
                                       if s["rtype"] == "light" and s["rid"] in lights]
                        gl = next((s["rid"] for s in g.get("services", []) if s["rtype"] == "grouped_light"), None)
                        if gl:
                            groups[g["id"]] = g["metadata"]["name"]
                            targets.append({"kind": kind, "name": g["metadata"]["name"], "bridge": b,
                                            "rtype": "grouped_light", "rid": gl, "lights": ls})
                for s in sc:
                    scenes.append({"name": s["metadata"]["name"], "group": groups.get(s.get("group", {}).get("rid"), ""),
                                   "bridge": b, "rid": s["id"]})
            names = [t["name"] for t in targets]
            for t in targets:                       # same name on two bridges: add the bridge name
                t["base"] = t["name"]
                if names.count(t["name"]) > 1 and len(self.bridges) > 1:
                    t["name"] = f"{t['name']} ({t['bridge'].name})"
            self.targets, self.scenes, self.loaded = targets, scenes, time.time()

    def find(self, name):
        self.refresh()
        if not self.targets:
            raise ValueError("no Hue bridge is reachable right now")
        n = _norm(name)
        if n in ("all", "tutte", "tutto", "alle", "everything", "tutte le luci", "alle lichter", "all lights"):
            return [t for t in self.targets if t["kind"] == "room"] or [t for t in self.targets if t["kind"] == "light"]
        exact = [t for t in self.targets if _norm(t["name"]) == n]
        if not exact:                                # "Soggiorno" = the Soggiorno of every bridge
            exact = [t for t in self.targets if _norm(t["base"]) == n]
        if exact:
            order = {"room": 0, "zone": 1, "light": 2}
            best = min(order[t["kind"]] for t in exact)
            return [t for t in exact if order[t["kind"]] == best]
        names = {_norm(t["name"]): t for t in self.targets}
        part = [t for k, t in names.items() if n and (n in k or k in n)]
        if len(part) == 1:
            return part
        close = difflib.get_close_matches(n, list(names), n=1, cutoff=0.6)
        if close:
            return [names[close[0]]]
        raise ValueError(f"no light, room or zone called {name!r}. Known: " + ", ".join(sorted(set(t['name'] for t in self.targets))))

    # ---------------- actions ----------------
    @staticmethod
    def _state(l):
        on = l.get("on", {}).get("on")
        bri = l.get("dimming", {}).get("brightness")
        return f"{'on' if on else 'off'}" + (f" {round(bri)}%" if on and bri is not None else "")

    def list(self):
        self.refresh(force=True)
        if not self.targets:
            return "no Hue bridge reachable"
        out = []
        for kind in ("room", "zone"):
            for t in [t for t in self.targets if t["kind"] == kind]:
                on = sum(1 for l in t["lights"] if l.get("on", {}).get("on"))
                out.append(f"{kind} {t['name']}: {on}/{len(t['lights'])} on ("
                           + ", ".join(f"{l['metadata']['name']} {self._state(l)}" for l in t["lights"]) + ")")
        loose = [t for t in self.targets if t["kind"] == "light" and
                 not any(t["rid"] == l["id"] for g in self.targets if g["kind"] != "light" for l in g["lights"])]
        if loose:
            out.append("other lights: " + ", ".join(f"{t['name']} {self._state(t['lights'][0])}" for t in loose))
        if self.scenes:
            out.append("scenes: " + ", ".join(sorted(set(f"{s['name']}" + (f" [{s['group']}]" if s['group'] else "")
                                                         for s in self.scenes))))
        return "\n".join(out)

    def set(self, target, on=None, brightness=None, color=None, transition_s=0.4):
        body = {}
        if on is not None:
            body["on"] = {"on": bool(on)}
        if brightness is not None:
            body["dimming"] = {"brightness": max(1.0, min(100.0, float(brightness)))}
            body.setdefault("on", {"on": True})
        if color:
            kind, val = parse_color(color)
            body["color_temperature" if kind == "mirek" else "color"] = {"mirek": val} if kind == "mirek" else {"xy": {"x": val[0], "y": val[1]}}
            body.setdefault("on", {"on": True})
        if not body:
            raise ValueError("nothing to change: give on, brightness and/or color")
        body["dynamics"] = {"duration": int(transition_s * 1000)}
        done = []
        for t in self.find(target):
            b = dict(body)
            note = ""
            if t["rtype"] == "light":                    # drop what this lamp cannot do
                l = t["lights"][0]
                if "color" in b and "color" not in l:
                    b.pop("color"); note = " (white-only lamp: colour not possible)"
                if "color_temperature" in b and "color_temperature" not in l:
                    b.pop("color_temperature"); note = " (no white-tone control)"
            elif "color" in b and not any("color" in l for l in t["lights"]):
                note = " (no colour lamps there)"
            t["bridge"].put(t["rtype"], t["rid"], b)
            done.append(f"{t['kind']} {t['name']}{note}")
        self.loaded = 0.0                                 # state changed: re-read on the next query
        return "done: " + ", ".join(done)

    def scene(self, name, room=None):
        self.refresh()
        n, r = _norm(name), _norm(room or "")
        cands = [s for s in self.scenes if (not r or r in _norm(s["group"]))]
        exact = [s for s in cands if _norm(s["name"]) == n]
        pick = exact[0] if exact else None
        if not pick:
            close = difflib.get_close_matches(n, [_norm(s["name"]) for s in cands], n=1, cutoff=0.6)
            pick = next((s for s in cands if close and _norm(s["name"]) == close[0]), None)
        if not pick:
            raise ValueError(f"no scene {name!r}" + (f" in {room!r}" if room else "") + ". Known: "
                             + ", ".join(sorted(set(s['name'] for s in self.scenes))))
        pick["bridge"].put("scene", pick["rid"], {"recall": {"action": "active"}})
        self.loaded = 0.0
        return f"scene {pick['name']}" + (f" in {pick['group']}" if pick["group"] else "") + " activated"


def make_hue_tools(hue):
    from llm import Tool

    def guard(fn, *a, **k):
        try:
            return fn(*a, **k)
        except Exception as e:
            return f"error: {e}"

    def lights_status():
        return guard(hue.list)

    def set_lights(target: str, on: bool = None, brightness: float = None, color: str = None):
        return guard(hue.set, target, on, brightness, color)

    def light_scene(scene: str, room: str = None):
        return guard(hue.scene, scene, room)

    return [
        Tool(lights_status, "Philips Hue: list rooms, zones, lights (on/off, brightness) and scenes. Call it first "
                            "when you do not know the names."),
        Tool(set_lights, "Philips Hue: switch, dim or colour a room, zone or single light by name ('all' = every room).",
             {"type": "object", "properties": {
                 "target": {"type": "string", "description": "room, zone or light name as listed, or 'all'"},
                 "on": {"type": "boolean", "description": "true = on, false = off"},
                 "brightness": {"type": "number", "description": "1-100 percent"},
                 "color": {"type": "string", "description": "English colour name (red, blue, purple...), 'warm', "
                                                            "'cool', 'daylight', a Kelvin value like '2700K', or #rrggbb"}},
              "required": ["target"]}),
        Tool(light_scene, "Philips Hue: activate a saved scene (e.g. Relax, Concentrate, Nightlight, Energize).",
             {"type": "object", "properties": {"scene": {"type": "string"},
                                               "room": {"type": "string", "description": "optional room/zone the scene belongs to"}},
              "required": ["scene"]}),
    ]
