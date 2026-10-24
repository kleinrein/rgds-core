"""Explicit standalone Linux runtime; stock sessions remain the default.

The mode marker is set by session.py after display selection, never inferred
from a missing vendor mount. No firmware, BIOS or vendor OS library is imported.
"""
import os
import json
from pathlib import Path
import shutil

MARKER = "RGDS_SESSION_PLATFORM"
LIBRARY_DIRS = (Path("/usr/lib/aarch64-linux-gnu"), Path("/lib/aarch64-linux-gnu"), Path("/usr/lib"), Path("/lib"))
SDL_NAMES = ("libSDL2-2.0.so.0", "libSDL2.so")


def hardware_profile(path=Path('/etc/rgds/hardware.json')):
    try:
        profile = json.loads(Path(path).read_text())
    except FileNotFoundError:
        return None
    if (
        not isinstance(profile, dict) or profile.get('schema') != 1
        or profile.get('model') not in ('rg-ds', 'rg-ds-plus') or not isinstance(profile.get('top_display'), str)
        or not isinstance(profile.get('bottom_display'), str) or profile['top_display'] == profile['bottom_display']
    ):
        raise ValueError('Invalid independent hardware profile')
    return profile


def independent():
    return os.environ.get(MARKER) == "independent"


def vendor_path(path):
    return any(path == prefix or path.startswith(prefix + "/") for prefix in ("/mnt/vendor", "/mnt/ports", "/oem"))


def environment(clean, *, runtime_dirs=None, library_dirs=LIBRARY_DIRS):
    env = dict(clean)
    # Do not inherit a compatibility preload or a vendor library search path.
    for key in ("LD_PRELOAD", "LD_LIBRARY_PATH", "RGDS_SDL_LIBRARY", "SDL_VIDEODRIVER"):
        env.pop(key, None)
    candidates = runtime_dirs if runtime_dirs is not None else [
        Path(clean.get("XDG_RUNTIME_DIR", "/run/rgds")),
        Path("/run/rgds"),
        Path("/run/user") / str(os.getuid())
    ]
    socket = None
    for directory in dict.fromkeys(map(Path, candidates)):
        if vendor_path(str(directory)) or vendor_path(str(directory.resolve())):
            continue
        sockets = sorted(path for path in directory.glob("wayland-*") if path.is_socket())
        preferred = directory / clean.get("WAYLAND_DISPLAY", "wayland-0")
        if preferred in sockets:
            socket = preferred
        elif sockets:
            socket = sockets[0]
        if socket:
            break
    if socket is None:
        raise ValueError("Standalone RGDS needs an active Wayland compositor socket.")
    selected = None
    for directory in map(Path, library_dirs):
        for name in SDL_NAMES:
            path = directory / name
            if (path.is_file() and not vendor_path(str(path)) and not vendor_path(str(path.resolve()))):
                selected = path
                break
        if selected:
            break
    if selected is None:
        raise ValueError("Standalone RGDS needs a system SDL2 runtime with working display/audio backends.")
    env.update(
        {
            MARKER: "independent",
            "PYTHONDONTWRITEBYTECODE": "1",
            "XDG_RUNTIME_DIR": str(socket.parent),
            "WAYLAND_DISPLAY": socket.name,
            "SDL_VIDEODRIVER": "wayland",
            "RGDS_SDL_LIBRARY": str(selected),
            # Desktop GL fallback crashes in the minimal image SDL Wayland backend.
            "SDL_RENDER_DRIVER": "opengles2",
            "SDL_VIDEO_WAYLAND_ALLOW_LIBDECOR": "0",
            "SDL_RENDER_BATCHING": "1",
            "LD_LIBRARY_PATH": str(selected.parent),
            "SDL_VIDEO_DISPLAY_PRIORITY": "DSI-1,DSI-2",
            "SDL_TOUCH_MOUSE_EVENTS": "0",
            "SDL_MOUSE_TOUCH_EVENTS": "0"
        }
    )
    profile = hardware_profile()
    if profile:
        env['RGDS_DEVICE_MODEL'] = profile.get('model', 'rg-ds')
        env['RGDS_TOP_DISPLAY'] = profile['top_display']
        env['RGDS_BOTTOM_DISPLAY'] = profile['bottom_display']
        env['SDL_VIDEO_DISPLAY_PRIORITY'] = profile['top_display'] + ',' + profile['bottom_display']
        from input_devices import discover
        inputs = discover(bottom_hint=profile.get('bottom_touch_path_hint', ''))
        if inputs['touch']: env['RGDS_TOUCH_DEV'] = inputs['touch']
        if inputs['keys']: env['RGDS_KEYS_DEV'] = inputs['keys']
        if inputs.get('profile') == 'mainline-rg-ds': env['RGDS_INPUT_PROFILE'] = 'mainline-rg-ds'
        if inputs['lid'] and inputs['lid_wakeup']:
            env['RGDS_LID_DEV'] = inputs['lid']
            env['RGDS_LID_WAKEUP'] = inputs['lid_wakeup']
        env['RGDS_INPUT_AMBIGUOUS'] = '1' if inputs['touch_ambiguous'] or inputs['keys_ambiguous'] else '0'
    return env


def system_command(action):
    if action not in ("suspend", "reboot", "poweroff"):
        raise ValueError("Unsupported standalone system action")
    binary = shutil.which("systemctl", path="/usr/bin:/bin")
    if not binary:
        raise ValueError("Standalone systemd control is unavailable")
    return [binary, action]
