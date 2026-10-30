"""The robot's motions in three sections, and the walk/turn primitives an LLM can use.

  movements   locomotion: the procedural walk/turn gait (what walk() and turn() use) and the
              stock locomotion files (explore_foward*, explore_back_away*, explore_turn_*,
              cliff_*, react_back_*). Sent to the model only when it asks for movement.
  dances      the dance routines with their music (d1_EmoDance ... d11_OceanBlue, dance_miss,
              Christmas_song, santasong, DJ1, Daily_Boxing_loop*, listen_to_music_end*).
  animations  everything else (looks, reactions, games, daily routines, pettings...).

The gait itself is the stock firmware's oscillator (see main/legs.c): one *cycle* is one
period of the leg/foot sine motion. How far one cycle moves the robot depends on the floor,
so the two constants below are calibration values, adjustable from the command line
(--walk-cm-per-cycle, --turn-deg-per-cycle). Measure once: walk(20) and see how far it
went; turn(90) and see how far it turned; scale the constants accordingly.
"""
import re
from dataclasses import dataclass

DANCE_RE = re.compile(r"^(d\d+_|dance|DJ1_|Christmas_song|santasong|listen_to_music_end|Daily_Boxing_loop)", re.I)
MOVE_RE = re.compile(r"^(explore_(foward|back_away|turn_left|turn_right)\d|cliff_|react_back_\d|zombie_loop_walk)", re.I)

MOVE_HINTS = {
    "explore_foward": "a few steps forward (1 = short, 3 = long)",
    "explore_back_away": "steps backward",
    "explore_turn_left": "turn on the spot to the left",
    "explore_turn_right": "turn on the spot to the right",
    "cliff_": "edge reaction: back off from a table edge (L/R/B = side)",
    "react_back_": "startled: jump back",
    "zombie_loop_walk": "slow zombie walk",
}


def section(name):
    if DANCE_RE.match(name):
        return "dances"
    if MOVE_RE.match(name):
        return "movements"
    return "animations"


def split(names):
    out = {"movements": [], "dances": [], "animations": []}
    for n in names:
        out[section(n)].append(n)
    return out


def hint(name):
    for k, v in MOVE_HINTS.items():
        if name.lower().startswith(k.lower()):
            return v
    return ""


@dataclass
class Calibration:
    walk_cm_per_cycle: float = 2.0     # forward travel of one walk cycle (stock explore_foward gait)
    turn_deg_per_cycle: float = 15.0   # rotation of one turn cycle (stock explore_turn gait)
    max_cycles: float = 30.0


def walk_plan(distance_cm, cal):
    """distance_cm > 0 forward, < 0 backward -> gait request for the firmware."""
    cycles = min(cal.max_cycles, max(0.5, round(abs(distance_cm) / cal.walk_cm_per_cycle * 2) / 2))
    return {"action": "gait", "kind": "walk", "reverse": distance_cm < 0, "cycles": cycles,
            "planned_cm": round(cycles * cal.walk_cm_per_cycle, 1)}


def turn_plan(degrees, cal):
    """degrees > 0 left (counter-clockwise), < 0 right -> gait request for the firmware."""
    cycles = min(cal.max_cycles, max(0.5, round(abs(degrees) / cal.turn_deg_per_cycle * 2) / 2))
    return {"action": "gait", "kind": "turn", "reverse": degrees < 0, "cycles": cycles,
            "planned_deg": round(cycles * cal.turn_deg_per_cycle, 1)}


def move_help(cal):
    """System-prompt text with the basic movement commands."""
    return (
        "Moving the robot (it has two legs with a hip and a foot servo each, no wheels):\n"
        f"- walk(distance_cm): walks straight; negative = backward. One gait cycle is about {cal.walk_cm_per_cycle:g} cm.\n"
        f"- turn(degrees): turns on the spot; positive = left (counter-clockwise), negative = right. One cycle is about {cal.turn_deg_per_cycle:g} degrees, so turn(90) is roughly {90 / cal.turn_deg_per_cycle:.1f} cycles.\n"
        "Both block until the move is finished and return the cycles actually planned; amounts are rounded to half cycles.\n"
        "Keep single moves short (up to 30 cm or 180 degrees), split longer paths into steps, and remember the robot "
        "lives on a desk: never walk more than a few centimetres without being told the way is clear.\n"
        "list_movements gives the stock locomotion files (explore_foward1, explore_turn_left2 ...) playable with "
        "play_motion; list_dances the dances (play_dance plays one with its music); list_animations the rest "
        "(play_motion). Dances and animations also show their face animation and sound."
    )
