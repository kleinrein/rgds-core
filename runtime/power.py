"""Power off and restart through BusyBox init (RG DS OS), with the request and its outcome kept across the next boot."""
import json
import os
from pathlib import Path
import subprocess
import time


def clean_environment():
    result = os.environ.copy()
    if result.get('RGDS_SHARED_AUDIO') == '1':
        result.pop('ALSA_CONFIG_PATH', None)
    for key in tuple(result):
        if key.startswith("RGDS_") or key in (
            "LD_PRELOAD", "LD_LIBRARY_PATH", "SDL_VIDEODRIVER", "SDL_TOUCH_MOUSE_EVENTS", "SDL_MOUSE_TOUCH_EVENTS",
            "SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS"
        ):
            result.pop(key, None)
    result["PATH"] = "/usr/sbin:/usr/bin:/sbin:/bin"
    return result


def execute_system_action(action, data):
    """Ask init to reboot or power off, never SysRq or a forced reset/halt."""
    if action not in ("reboot", "poweroff"): raise ValueError("Unknown system action")
    from emulators import core_running
    if core_running(Path(data)): raise ValueError("An engine is still active; the system action was not dispatched.")
    from linux_platform import system_command
    command = system_command(action)
    # Keep the intent and the helper's output across the next boot. A successful helper exit only acknowledges a
    # request, not physical power-off.
    from fields import write_fields
    folder = Path(data) / 'logs'
    receipt = dict(schema=1, action=action, command=json.dumps(command), state='dispatching', requested_at=time.time())
    try:
        receipt['boot_id'] = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
    except OSError:
        receipt['boot_id'] = 'unavailable'

    def record(state, **extra):
        receipt.update(state=state, **extra)
        try:
            write_fields(folder / 'system-exit.ini', receipt)
        except OSError:
            pass  # Full storage must not make normal power-off impossible.

    record('dispatching')
    try:
        # A real file survives termination by init without holding a pipe open.
        try:
            folder.mkdir(parents=True, exist_ok=True)
            output = (folder / 'system-exit.log').open('w')
        except OSError:
            output = open(os.devnull, 'w')
        with output:
            subprocess.run(
                command,
                env=clean_environment(),
                cwd="/",
                check=True,
                timeout=5,
                start_new_session=True,
                stdout=output,
                stderr=subprocess.STDOUT
            )
        record('request-accepted')
    except (OSError, subprocess.SubprocessError) as error:
        record('failed', error=str(error))
        raise
