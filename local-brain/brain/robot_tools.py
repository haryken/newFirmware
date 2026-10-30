"""The robot as tools for the local LLM (same set the Claude version had, as JSON-schema tools)."""
import time

from emo_catalog import split, turn_plan, walk_plan
from emo_robot import RobotError
from llm import Tool


def make_tools(state, cal, claude=None, brain_ref=None):
    """claude: a ClaudeHelper (adds the ask_claude tool); brain_ref: callable returning the Brain,
    used to pass the turn's language and the recent conversation to Claude."""
    robot = state.robot

    def wait_done(timeout=40.0):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if not robot.motion_status().get("playing"):
                return "done"
            time.sleep(0.25)
        return "still moving"

    def guard(fn):
        try:
            return fn()
        except RobotError as e:
            return f"error: {e}"

    def walk(distance_cm: float):
        plan = walk_plan(float(distance_cm), cal)
        return guard(lambda: (robot.motion("gait", kind="walk", reverse=plan["reverse"], cycles=plan["cycles"]),
                              f"walked {plan['cycles']} cycles (~{plan['planned_cm']} cm): {wait_done()}")[1])

    def turn(degrees: float):
        plan = turn_plan(float(degrees), cal)
        return guard(lambda: (robot.motion("gait", kind="turn", reverse=plan["reverse"], cycles=plan["cycles"]),
                              f"turned {plan['cycles']} cycles (~{plan['planned_deg']} deg): {wait_done()}")[1])

    def names(section, query=""):
        def f():
            found = [n for n in split(robot.motion_status().get("list", []))[section] if query.lower() in n.lower()]
            return ", ".join(found[:80]) or "no match"
        return guard(f)

    def list_movements():
        return names("movements")

    def list_dances():
        return names("dances")

    def list_animations(query: str = ""):
        return names("animations", query or "")

    def play_motion(name: str, loop: bool = False, media: bool = True, speed: float = 0.0):
        kw = {"speed": speed} if speed else {}
        return guard(lambda: str(robot.motion("play", name=name, loop=1 if loop else 0, media=media, **kw)))

    def play_animation(name: str, loop: bool = False):
        return guard(lambda: str(robot.control("anim", name=name, loop=1 if loop else 0)))

    def set_volume(level: int):
        return guard(lambda: str(robot.control("volume", value=max(0, min(255, int(level))))))

    def robot_status():
        def f():
            s, m = robot.status(), robot.motion_status()
            return (f"battery {s.get('bat_pct')}% ({s.get('bat_mv')} mV), pose {m.get('pos')}, "
                    f"motion {'playing ' + m.get('name', '') if m.get('playing') else 'idle'}")
        return guard(f)

    def time_now():
        return time.strftime("%A %d %B %Y, %H:%M")

    def ask_claude(question: str):
        brain = brain_ref() if brain_ref else None
        lang = (brain.lang if brain else None) or "en"
        ctx = [(m["role"], m["content"]) for m in (brain.history if brain else [])]
        try:
            return claude.ask(question, lang, ctx)
        except Exception as e:
            return f"error: Claude is unreachable ({type(e).__name__}); answer as well as you can or say so"

    num = lambda d: {"type": "number", "description": d}
    s = lambda d: {"type": "string", "description": d}
    b = lambda d: {"type": "boolean", "description": d}
    obj = lambda props, req=(): {"type": "object", "properties": props, "required": list(req)}
    return [
        Tool(walk, "Walk straight by about distance_cm using the gait; negative = backward. Blocks until done.",
             obj({"distance_cm": num("centimetres, -30..30; negative walks backward")}, ["distance_cm"])),
        Tool(turn, "Turn on the spot by about `degrees`; positive = left, negative = right. Blocks until done.",
             obj({"degrees": num("-180..180; positive turns left")}, ["degrees"])),
        Tool(list_movements, "List the stock locomotion motion files (steps, turns, edge reactions) for play_motion."),
        Tool(list_dances, "List the dance routines (with their own music) for play_motion."),
        Tool(list_animations, "List every other motion (looks, reactions, games...), filtered by a substring.",
             obj({"query": s("case-insensitive substring; empty = first 80")})),
        Tool(play_motion, "Play a leg motion (.mot) such as a dance or a look-around, with its face and music.",
             obj({"name": s("motion name from a list_* tool, e.g. d1_EmoDance, look_around_1"),
                  "loop": b("repeat until stopped"), "media": b("also play face animation and music (default true)"),
                  "speed": num("0 = default, 1.0 = as authored, 2.0 = twice as fast")}, ["name"])),
        Tool(play_animation, "Show a face animation on the robot's screen.",
             obj({"name": s("e.g. stay_wink_1 (wink), listen_and_think, voice_wake_up"), "loop": b("keep running")}, ["name"])),
        Tool(set_volume, "Set the robot speaker volume.", obj({"level": num("0 (mute) .. 255 (max); 104 is normal")}, ["level"])),
        Tool(robot_status, "Battery level, servo pose and what the robot is doing right now."),
        Tool(time_now, "The current local date and time."),
    ] + ([Tool(ask_claude,
               "Ask Claude, a much more capable AI with live web search. Use it for anything current or online "
               "(news, weather, sports, prices, opening hours, events, facts you are not sure of) and for hard "
               "questions (detailed explanations, maths beyond mental arithmetic, code, advice needing care). "
               "Takes a few seconds; the answer comes back as spoken text in the user's language.",
               obj({"question": s("the full question with all the context Claude needs, self-contained")},
                   ["question"]))] if claude else [])
