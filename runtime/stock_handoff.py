"""Return frontend ownership to stock without managing network or display services."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import uuid

from records import read_record, write_record

STOCK_SUPERVISOR = Path("/mnt/vendor/ctrl/loadapp.sh")
STOCK_STOP = Path("/tmp/stopAPP.ini")
FRONTENDS = {"dmenu.bin", "muos3.bin", "muos2.bin", "MainUI"}


def clean_environment():
    result = os.environ.copy()
    if result.get('RGDS_SHARED_AUDIO') == '1':
        result.pop('ALSA_CONFIG_PATH', None)
    for key in tuple(result):
        if key.startswith(("RGDS_", "SIMPLEOS_")) or key in (
                "LD_PRELOAD", "LD_LIBRARY_PATH", "SDL_VIDEODRIVER",
                "SDL_TOUCH_MOUSE_EVENTS", "SDL_MOUSE_TOUCH_EVENTS",
                "SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS"):
            result.pop(key, None)
    result["PATH"] = "/usr/sbin:/usr/bin:/sbin:/bin"
    return result


def supervisors(proc=Path("/proc"), script=STOCK_SUPERVISOR):
    """Identify the stock loop by its exact script argument, never a name match."""
    result = []
    for entry in proc.iterdir():
        if not entry.name.isdigit():continue
        try:
            state = (entry / "stat").read_text().rsplit(")", 1)[1].split()
            arguments = (entry / "cmdline").read_bytes().split(b"\0")
            matched = os.fsencode(script) in arguments
            if not matched:
                for argument in arguments:
                    value = Path(os.fsdecode(argument))
                    if argument and not value.is_absolute() and value.name == Path(script).name:
                        matched = ((entry / "cwd").resolve() / value).resolve() == Path(script).resolve()
                        if matched:break
            if state[0] != "Z" and matched:
                result.append({"pid": int(entry.name), "start": state[19]})
        except (OSError, IndexError):continue
    return result


def acquire_stock(owner, marker=STOCK_STOP, script=STOCK_SUPERVISOR):
    """Own only a newly created stop flag; a foreign flag is never replaced."""
    marker, script = Path(marker), Path(script)
    if not script.is_file() or not os.access(script, os.X_OK):
        raise ValueError("Stock loadapp.sh is unavailable; keep the stock frontend active.")
    token = json.dumps(dict(project="rgds", pid=owner["pid"], start=owner["start"], nonce=uuid.uuid4().hex)) + "\n"
    try:
        descriptor = os.open(marker, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o644)
    except FileExistsError:
        raise ValueError("Stock frontend stop flag is already owned; RGDS will not take it over.")
    created = os.fstat(descriptor)
    try:
        with os.fdopen(descriptor, "w") as stream:
            stream.write(token);stream.flush()
            info = os.fstat(stream.fileno())
        return dict(marker=marker, script=script, device=info.st_dev, inode=info.st_ino,
                    mtime=info.st_mtime_ns, ctime=info.st_ctime_ns, token=token)
    except BaseException:
        try:
            current = marker.lstat()
            if (current.st_dev, current.st_ino) == (created.st_dev, created.st_ino):
                marker.unlink()
        except OSError:pass
        raise


def release_stock_flag(lease):
    if lease is None:return
    marker = lease["marker"]
    try:
        descriptor = os.open(marker, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
    except FileNotFoundError:return
    with os.fdopen(descriptor) as stream:
        info = os.fstat(stream.fileno());contents = stream.read(4096)
    if (info.st_dev, info.st_ino, info.st_mtime_ns, info.st_ctime_ns) != \
            (lease["device"], lease["inode"], lease["mtime"], lease["ctime"]) or contents != lease["token"]:
        raise ValueError("Stock stop flag changed owner; the foreign flag was preserved.")
    marker.unlink()


def restore_stock(lease, proc=Path("/proc")):
    """Release the flag first. The stock loop remains the only frontend launcher."""
    if lease is None:return
    release_stock_flag(lease)
    if supervisors(proc, lease["script"]):return
    # A surviving frontend can belong to a stock loop finishing its transition.
    # Starting another loop then could duplicate that frontend.
    for entry in proc.iterdir():
        if not entry.name.isdigit():continue
        try:
            if (entry / "comm").read_text().strip() in FRONTENDS and \
                    (entry / "stat").read_text().rsplit(")", 1)[1].split()[0] != "Z":return
        except (OSError, IndexError):continue
    environment = clean_environment()
    environment["RGDS_SKIP_BOOT_ONCE"] = "1"
    subprocess.Popen([str(lease["script"])], cwd="/", env=environment,
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)


def owner_path(data):return Path(data) / "session/handoff-owner.json"
def request_path(data):return Path(data) / "session/exit.request"


def begin_session(data, owner):
    request_path(data).unlink(missing_ok=True)
    write_record(owner_path(data), owner)


def end_session(data, owner):
    if read_record(owner_path(data)) == owner:owner_path(data).unlink(missing_ok=True)


def _read_exit(path):
    fields = {}
    for line in Path(path).read_text().splitlines():
        key, separator, value = line.partition("=")
        if not separator or key in fields:raise ValueError("Malformed stock exit request")
        fields[key] = value
    if set(fields) != {"schema", "action", "owner_pid", "owner_start", "token", "state"} or fields["schema"] != "2" or \
            fields["action"] not in ("reboot", "poweroff") or fields["state"] not in ("pending", "committed") or \
            len(fields["token"]) != 32 or any(char not in "0123456789abcdef" for char in fields["token"]):
        raise ValueError("Malformed stock exit request")
    return fields


def _write_exit(path, fields):
    """Commit by rename last: cleanup cannot turn a successful commit into error."""
    path = Path(path);path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, pending = tempfile.mkstemp(prefix=".exit-", dir=path.parent)
    try:
        with os.fdopen(descriptor,"w") as stream:
            stream.write("".join(key+"="+value+"\n" for key,value in fields.items()))
            stream.flush()
        os.replace(pending,path)
    except BaseException:
        try:Path(pending).unlink(missing_ok=True)
        except OSError:pass
        raise


def request_exit(data, action):
    """Prepare an ineligible terminal request; publish success before committing."""
    if action not in ("reboot", "poweroff"):raise ValueError("Unknown stock system action")
    from emulators import core_running, process_identity
    if core_running(Path(data)):raise ValueError("Save your game and return home first.")
    owner = read_record(owner_path(data))
    if owner is None or process_identity(owner["pid"]) != owner["start"]:
        raise ValueError("No managed RGDS session is available for a clean stock handoff.")
    receipt = dict(schema="2", action=action, owner_pid=str(owner["pid"]),
                   owner_start=owner["start"], token=uuid.uuid4().hex, state="pending")
    _write_exit(request_path(data),receipt)
    return receipt


def commit_exit(data, receipt):
    """Make only this successfully published request eligible for handoff."""
    current = _read_exit(request_path(data))
    if current != receipt or current["state"] != "pending":
        raise ValueError("Stock exit request changed before it could be committed")
    _write_exit(request_path(data),dict(current,state="committed"))


def cancel_exit(data, receipt):
    """Cancel this exact request without deleting a newer worker's request."""
    path = request_path(data)
    try:current = _read_exit(path)
    except FileNotFoundError:return True
    if any(current[key] != receipt.get(key) for key in ("schema","action","owner_pid","owner_start","token")):
        return False
    path.unlink()
    return True


def consume_exit(data, owner):
    """Capture the current owner's action before recovery releases its lock."""
    path = request_path(data)
    try:
        fields = _read_exit(path)
        if fields["owner_pid"] != str(owner["pid"]) or fields["owner_start"] != owner["start"]:
            raise ValueError("Stock exit request does not belong to this session")
        return fields["action"] if fields["state"] == "committed" else None
    except FileNotFoundError:return None
    finally:path.unlink(missing_ok=True)


def execute_system_action(action, data, *, standalone=False):
    """Use stock init only after recovery, never SysRq or forced reset/halt."""
    if action not in ("reboot", "poweroff"):raise ValueError("Unknown stock system action")
    from emulators import core_running
    if core_running(Path(data)):raise ValueError("An engine is still active; the system action was not dispatched.")
    if standalone:
        from linux_platform import system_command
        command = system_command(action)
    else:
        binary = shutil.which(action, path="/sbin:/bin:/usr/sbin:/usr/bin")
        if not binary:raise ValueError("Stock " + action + " is unavailable")
        command = [binary]
    # Keep the intent and the stock helper's output across the next boot. A
    # successful helper exit only acknowledges a request, not physical power-off.
    from fields import write_fields
    folder = Path(data) / 'logs'
    receipt = dict(schema=1, action=action, command=json.dumps(command),
                   state='dispatching', requested_at=time.time())
    try:receipt['boot_id']=Path('/proc/sys/kernel/random/boot_id').read_text().strip()
    except OSError:receipt['boot_id']='unavailable'
    def record(state, **extra):
        receipt.update(state=state, **extra)
        try:write_fields(folder / 'system-exit.ini', receipt)
        except OSError:pass  # Full storage must not make normal power-off impossible.
    record('dispatching')
    if action == 'poweroff' and not standalone:
        # Lets the final shutdown step power the PMIC off by register so the
        # next start takes one press (system_tuning.request_register_poweroff).
        from system_tuning import request_register_poweroff
        request_register_poweroff()
    try:
        # A real file survives termination by init without holding a pipe open.
        try:
            folder.mkdir(parents=True,exist_ok=True)
            output=(folder / 'system-exit.log').open('w')
        except OSError:output=open(os.devnull,'w')
        with output:
            subprocess.run(command, env=clean_environment(), cwd="/", check=True,
                           timeout=5, start_new_session=True, stdout=output, stderr=subprocess.STDOUT)
        record('request-accepted')
    except (OSError, subprocess.SubprocessError) as error:
        record('failed', error=str(error))
        raise
