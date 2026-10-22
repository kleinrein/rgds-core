"""Canonical DS controls and shared menu shortcuts, independent of emulator."""
import argparse
from pathlib import Path
import sys

from library import game_key, read_fields, write_fields

LOGICAL = ("a", "b", "x", "y", "l", "r", "start", "select", "up", "down", "left", "right")
SOURCES = (*LOGICAL, "l2", "r2", "l3", "r3", "none")
JOYSTICK = {
    0: "a",
    1: "b",
    2: "y",
    3: "x",
    4: "l",
    5: "r",
    6: "select",
    7: "start",
    9: "l3",
    10: "l2",
    11: "r2",
    12: "r3"
}
MODES = ("anbernic", "start+menu", "startthenmenu")
ACTIONS = ("menu", "home")


def captured_source(button=None, hat=None):
    if button is not None: return JOYSTICK.get(button)
    return {1: "up", 2: "right", 4: "down", 8: "left"}.get(hat)


def evdev_source(kind, code, value=1, *, mainline=False):
    if mainline and kind == 1:
        return {
            305: "a",
            304: "b",
            307: "x",
            308: "y",
            310: "l",
            311: "r",
            315: "start",
            314: "select",
            544: "up",
            545: "down",
            546: "left",
            547: "right",
            312: "l2",
            313: "r2",
            317: "l3",
            318: "r3"
        }.get(code)
    if kind == 1: return JOYSTICK.get(code - 304)
    if kind == 3 and code == 16: return "left" if value < 0 else "right" if value > 0 else None
    if kind == 3 and code == 17: return "up" if value < 0 else "down" if value > 0 else None
    return None


def paths(data, rom=None):
    global_path = Path(data) / "controls.ini"
    return global_path, Path(data) / "library" / game_key(rom) / "controls.ini" if rom else None


def preset(name="default"):
    mapping = dict(zip(LOGICAL, LOGICAL))
    if name == "ab-swap": mapping.update(a="b", b="a")
    elif name == "left-handed":
        mapping.update(a="right", b="down", x="up", y="left", up="x", down="b", left="y", right="a", l="r", r="l")
    return dict(
        schema="1",
        preset=name,
        shortcut="start+menu",
        shortcut_action="home",
        fast_forward_button="r2",
        fast_forward_mode="hold",
        **{
            "bind_" + key: value
            for key, value in mapping.items()
        }
    )


def merge(profile, fields):
    if fields.get("schema", "1") != "1": return profile
    name = fields.get("preset")
    if name in ("default", "ab-swap", "left-handed"):
        profile.update({key: value for key, value in preset(name).items() if key.startswith("bind_")})
        profile["preset"] = name
    elif name == "custom":
        profile["preset"] = name
    for logical in LOGICAL:
        key = "bind_" + logical
        if fields.get(key) in SOURCES: profile[key] = fields[key]
    if fields.get("shortcut") in MODES: profile["shortcut"] = fields["shortcut"]
    if fields.get("shortcut_action") in ACTIONS: profile["shortcut_action"] = fields["shortcut_action"]
    if fields.get("fast_forward_button") in SOURCES: profile["fast_forward_button"] = fields["fast_forward_button"]
    if fields.get("fast_forward_mode") in ("hold", "toggle"): profile["fast_forward_mode"] = fields["fast_forward_mode"]
    if name in ("default", "ab-swap", "left-handed") and "fast_forward_button" not in fields:
        if profile["fast_forward_button"] in [profile["bind_" + key] for key in LOGICAL]:
            profile["fast_forward_button"] = "none"
    return profile


def load_profile(data, rom=None):
    global_path, local = paths(data, rom)
    result = merge(preset(), read_fields(global_path))
    if local:
        fields = read_fields(local)
        if fields.get("inherit", "0") != "1": merge(result, fields)
    return result


def save_profile(data, fields, rom=None):
    profile = merge(load_profile(data, rom), {str(k): str(v) for k, v in fields.items()})
    if profile.get("preset") not in ("default", "ab-swap", "left-handed", "custom"):
        raise ValueError("Unknown control preset")
    for key, value in fields.items():
        if key == "preset" and value not in ("default", "ab-swap", "left-handed", "custom"):
            raise ValueError("Unknown control preset")
        if key.startswith("bind_") and (key[5:] not in LOGICAL or value not in SOURCES):
            raise ValueError("Unknown control binding")
        if key == "shortcut" and value not in MODES: raise ValueError("Unknown menu shortcut")
        if key == "shortcut_action" and value not in ACTIONS: raise ValueError("Unknown shortcut action")
        if key == "fast_forward_button" and value not in SOURCES: raise ValueError("Unknown fast-forward button")
        if key == "fast_forward_mode" and value not in ("hold", "toggle"): raise ValueError("Unknown fast-forward mode")
    assigned = [profile["bind_" + logical] for logical in LOGICAL if profile["bind_" + logical] != "none"]
    if profile["fast_forward_button"] != "none": assigned.append(profile["fast_forward_button"])
    if len(assigned) != len(set(assigned)): raise ValueError("Assign each physical button only once")
    global_path, local = paths(data, rom)
    if local: profile["inherit"] = str(fields.get("inherit", "0"))
    write_fields(local or global_path, profile)
    return profile


def set_binding(data, logical, source, rom=None):
    if logical not in LOGICAL or source not in SOURCES: raise ValueError("Unknown control binding")
    profile = load_profile(data, rom)
    previous = profile["bind_" + logical]
    if source != "none":
        if profile["fast_forward_button"] == source: profile["fast_forward_button"] = previous
        for other in LOGICAL:
            if other != logical and profile["bind_" + other] == source: profile["bind_" + other] = previous
    profile.update(preset="custom", **{"bind_" + logical: source})
    return save_profile(data, profile, rom)


def set_fast_forward(data, source, rom=None):
    if source not in SOURCES: raise ValueError("Unknown fast-forward button")
    profile = load_profile(data, rom)
    previous = profile['fast_forward_button']
    if source != 'none':
        for key in LOGICAL:
            if profile['bind_' + key] == source: profile['bind_' + key] = previous
    profile.update(fast_forward_button=source, preset='custom')
    return save_profile(data, profile, rom)


class FastForwardButton:
    """One action per press; hold mode always releases on menu/profile changes."""
    def __init__(self):
        self.source = 'none'
        self.mode = 'hold'
        self.held = set()
        self.suppressed = False

    def configure(self, profile):
        source, mode = profile.get('fast_forward_button', 'none'), profile.get('fast_forward_mode', 'hold')
        changed = (source, mode) != (self.source, self.mode)
        stop = changed and self.source != 'none'
        if changed:
            self.held.clear()
            self.suppressed = True
        self.source, self.mode = source, mode
        return 'fast-forward-off' if stop else None

    def feed(self, source, down, device=0):
        if source != self.source or source == 'none': return None
        before = bool(self.held)
        if down: self.held.add(device)
        else: self.held.discard(device)
        after = bool(self.held)
        if self.suppressed:
            # An initial configuration should still accept the first fresh
            # press; menu suppression is applied only while actually held.
            if not before: self.suppressed = False
            elif not after:
                self.suppressed = False
                return None
            else:
                return None
        if before == after: return None
        if self.mode == 'toggle': return 'fast-forward-toggle' if after else None
        return 'fast-forward-on' if after else 'fast-forward-off'

    def pause(self):
        if self.mode == 'hold' and self.held:
            self.suppressed = True
            return 'fast-forward-off'
        return None


class Shortcut:
    """Delay lone Menu briefly, recognizing either order of a physical chord."""
    def __init__(self, mode="start+menu", action="home"):
        self.mode, self.action = mode, action
        self.start = self.menu = self.latched = False
        self.pending = None
        self.started = -100
        self.last = -100

    def feed(self, source, down, now):
        if source not in ("start", "menu"): return None
        before = getattr(self, source)
        setattr(self, source, down)
        if not self.start and not self.menu: self.latched = False
        if before == down or not down: return None
        if source == "start": self.started = now
        if self.latched: return None
        if self.mode == "anbernic" and source == "menu": return self.fire(self.action, now)
        if self.mode == "start+menu" and self.start and self.menu:
            self.pending = None
            self.latched = True
            return self.fire(self.action, now)
        if self.mode == "startthenmenu" and source == "menu" and 0 <= now - self.started <= .65:
            self.pending = None
            self.latched = True
            return self.fire(self.action, now)
        if source == "menu": self.pending = now
        return None

    def fire(self, action, now):
        if now - self.last < .25: return None
        self.last = now
        return action

    def poll(self, now):
        if self.pending is not None and now - self.pending >= .15:
            self.pending = None
            return self.fire("menu", now)
        return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("get", "preset", "bind", "shortcut", "inherit"))
    parser.add_argument("value", nargs="?")
    parser.add_argument("source", nargs="?")
    parser.add_argument("--data", type=Path, required=True)
    parser.add_argument("--rom", type=Path)
    args = parser.parse_args()
    if args.command == "get": result = load_profile(args.data, args.rom)
    elif args.command == "preset": result = save_profile(args.data, {"preset": args.value}, args.rom)
    elif args.command == "bind": result = set_binding(args.data, args.value, args.source, args.rom)
    elif args.command == "shortcut":
        result = save_profile(args.data, {"shortcut": args.value, "shortcut_action": args.source or "home"}, args.rom)
    else:
        if not args.rom: raise ValueError("Choose a game before setting inheritance")
        result = save_profile(args.data, {"inherit": "1"}, args.rom)
    print("".join(key + "=" + value + "\n" for key, value in result.items()), end="")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, OSError) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
