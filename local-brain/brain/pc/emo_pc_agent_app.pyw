"""EMO PC Agent - tray app.

Runs the PC agent (pc_agent.py) in the background with an icon in the notification area:
  * the icon shows the state (eyes open = EMO may control this PC, closed = paused)
  * the menu: what EMO did last, pause/resume control, start with Windows, open the log, quit
  * a small notification when EMO starts doing something on this PC (can be switched off)
Settings live in settings.json next to this file.
"""
import json
import os
import platform
import subprocess
import sys
import threading
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
LOG = os.path.join(HERE, "pc-agent.log")
SETTINGS = os.path.join(HERE, "settings.json")
STARTUP_LNK = os.path.join(os.environ.get("APPDATA", ""), r"Microsoft\Windows\Start Menu\Programs\Startup",
                           "EMO PC Agent.lnk")

sys.stdout = sys.stderr = open(LOG, "a", encoding="utf-8", buffering=1)     # pythonw has no console
print(f"\n===== {time.strftime('%Y-%m-%d %H:%M:%S')} EMO PC Agent starting")


def msgbox(text, title="EMO PC Agent", flags=0x40):
    import ctypes
    ctypes.windll.user32.MessageBoxW(None, text, title, flags)


def load_settings():
    d = {"name": platform.node(), "server": None, "notify": True, "paused": False}
    try:
        d.update(json.load(open(SETTINGS, encoding="utf-8")))
    except Exception:
        pass
    return d


def save_settings(s):
    try:
        json.dump(s, open(SETTINGS, "w", encoding="utf-8"), indent=2)
    except Exception as e:
        print(f"[app] could not save settings: {e}")


def icon_image(paused=False, busy=False):
    """EMO's face: a dark rounded screen with two eyes (cyan; orange while busy; closed lines when paused)."""
    from PIL import Image, ImageDraw
    n = 64
    im = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((2, 6, n - 3, n - 7), radius=14, fill=(24, 28, 36, 255), outline=(90, 100, 115, 255), width=2)
    eye = (255, 166, 0, 255) if busy else (0, 220, 255, 255)
    if paused:
        for x in (15, 37):
            d.rounded_rectangle((x, 30, x + 12, 34), radius=2, fill=(120, 130, 145, 255))
    else:
        for x in (15, 37):
            d.rounded_rectangle((x, 20, x + 12, 42), radius=5, fill=eye)
    return im


def startup_enabled():
    return os.path.exists(STARTUP_LNK)


def set_startup(on):
    if not on:
        try:
            os.remove(STARTUP_LNK)
        except FileNotFoundError:
            pass
        return
    pyw = os.path.join(HERE, ".venv", "Scripts", "pythonw.exe")
    ps = (f"$s=(New-Object -ComObject WScript.Shell).CreateShortcut('{STARTUP_LNK}');"
          f"$s.TargetPath='{pyw}';$s.Arguments='\"{os.path.join(HERE, 'emo_pc_agent_app.pyw')}\"';"
          f"$s.WorkingDirectory='{HERE}';$s.IconLocation='{os.path.join(HERE, 'emo.ico')}';$s.Save()")
    subprocess.run(["powershell", "-NoProfile", "-Command", ps], creationflags=0x08000000)


def main():
    try:
        import pystray
        import pc_agent
    except Exception:
        msgbox("EMO PC Agent could not start:\n\n" + traceback.format_exc(limit=2), flags=0x10)
        return
    st = load_settings()
    if not os.path.exists(os.path.join(HERE, "pc_secret.txt")):
        msgbox("pc_secret.txt is missing.\n\nCopy it from the EMO brain PC (voice\\brain\\pc\\pc_secret.txt) into\n"
               + HERE + "\nand start EMO PC Agent again.", flags=0x30)
        return
    pc_agent.STATE["paused"] = bool(st.get("paused"))
    try:
        pc_agent.start(st["name"], server=st.get("server"))
    except OSError:
        msgbox("EMO PC Agent is already running (port 8765 is in use).", flags=0x30)
        return

    def title():
        if pc_agent.STATE["paused"]:
            return "EMO PC Agent - paused"
        last = pc_agent.STATE["last"]
        if not last:
            return "EMO PC Agent - ready"
        return f"EMO PC Agent - last: {last[1][:60]} ({time.strftime('%H:%M', time.localtime(last[0]))})"

    icon = pystray.Icon("emo_pc_agent", icon_image(st.get("paused")), title())

    def refresh(busy=False):
        icon.icon = icon_image(pc_agent.STATE["paused"], busy)
        icon.title = title()[:127]
        icon.update_menu()

    last_note = {"t": 0.0}

    def on_event(kind, text):
        refresh(busy=True)
        threading.Timer(1.2, refresh).start()
        if st.get("notify") and kind in ("command", "screenshot") and time.time() - last_note["t"] > 20:
            last_note["t"] = time.time()                       # at most one bubble per 20 s
            try:
                icon.notify(text[:200], "EMO is using this PC")
            except Exception:
                pass
    pc_agent.STATE["listeners"].append(on_event)

    def toggle_pause(_icon, _item):
        pc_agent.STATE["paused"] = not pc_agent.STATE["paused"]
        st["paused"] = pc_agent.STATE["paused"]
        save_settings(st)
        pc_agent.STATE["announce_now"].set()
        print(f"[app] control {'paused' if st['paused'] else 'resumed'}")
        refresh()

    def toggle_notify(_icon, _item):
        st["notify"] = not st.get("notify")
        save_settings(st)

    def toggle_startup(_icon, _item):
        set_startup(not startup_enabled())

    def open_log(_icon, _item):
        os.startfile(LOG)

    def open_folder(_icon, _item):
        os.startfile(HERE)

    def quit_app(_icon, _item):
        print("[app] quit")
        icon.stop()

    icon.menu = pystray.Menu(
        pystray.MenuItem(lambda _: title().replace("EMO PC Agent - ", "Status: "), None, enabled=False),
        pystray.MenuItem(lambda _: f"This PC: {st['name']} ({pc_agent.STATE['count']} commands so far)", None,
                         enabled=False),
        pystray.Menu.SEPARATOR,
        pystray.MenuItem("Pause EMO's control", toggle_pause, checked=lambda _: pc_agent.STATE["paused"]),
        pystray.MenuItem("Show notifications", toggle_notify, checked=lambda _: bool(st.get("notify"))),
        pystray.MenuItem("Start with Windows", toggle_startup, checked=lambda _: startup_enabled()),
        pystray.Menu.SEPARATOR,
        pystray.MenuItem("Open log", open_log),
        pystray.MenuItem("Open install folder", open_folder),
        pystray.MenuItem("Quit", quit_app))
    print(f"[app] tray icon up; control {'paused' if pc_agent.STATE['paused'] else 'active'}")
    icon.run()
    os._exit(0)


if __name__ == "__main__":
    main()
