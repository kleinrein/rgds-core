#!/usr/bin/env python3
"""SSH tools for a stock Linux development card. A device address is always explicit."""
import argparse
import json
from datetime import datetime, timezone
from pathlib import Path
import re
import shlex
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
Q = shlex.quote

# The uploader verifies archive structure and every release hash before executing
# uploaded Python code. Updates stage into our folder and retain the old build.
REMOTE_INSTALL = (ROOT / "packaging/install_package.py").read_text()


def target(value):
    if not value or value.startswith("-") or not re.fullmatch(r"[A-Za-z0-9_.:@\[\]-]+", value):
        raise ValueError("Set DEVICE to the RG DS SSH address, for example root@192.168.1.42.")
    return value if "@" in value else "root@" + value


def ssh(device, command, batch=False, **kwargs):
    options = [
        "-o", "IPQoS=none", "-o", "ConnectTimeout=10", "-o", "ServerAliveInterval=5", "-o", "ServerAliveCountMax=2"
    ]
    if batch:
        options += ["-o", "BatchMode=yes"]
    return subprocess.run(["ssh", *options, device, command], check=True, **kwargs)


REMOTE_STATUS = r'''
import json, platform, sys, time
from pathlib import Path
root = Path(sys.argv[1])
if sys.platform != 'linux' or platform.machine() not in ('aarch64','arm64') or sys.version_info < (3,8):
    raise ValueError('Need the stock Linux AArch64 card with Python 3.8+.')
if not root.parent.is_dir():
    raise ValueError('SD mount is missing: '+str(root.parent))
record_errors = []
def alive(path, unknown_busy=False):
    record = None
    for attempt in range(5):
        try:
            record = json.loads(path.read_text())
            if not isinstance(record,dict) or type(record.get('pid')) is not int or record['pid'] <= 1 or not isinstance(record.get('start'),str) or not record['start'].isdigit():
                raise ValueError('Invalid ownership')
            break
        except FileNotFoundError: return False
        except (OSError,ValueError,TypeError):
            record = None
            if attempt < 4: time.sleep(0.05)
    if record is None:
        record_errors.append(str(path))
        return unknown_busy
    pid = record['pid']
    try:
        fields = Path('/proc',str(pid),'stat').read_text().rsplit(')',1)[1].split()
        return fields[0] != 'Z' and fields[19] == str(record['start'])
    except FileNotFoundError: return False
    except (OSError,IndexError):
        record_errors.append(str(path))
        return unknown_busy
lock = Path('/tmp/rgds-session.lock')
pointer = root/'state/active'
result = {'game':alive(root/'userdata/session/core.json',True),
    'launcher':alive(root/'userdata/session/launcher.json'),
    'session':alive(lock/'owner.json'), 'lock':lock.exists(),
    'recovery':(root/'userdata/boot_stock').exists(),
    'active':pointer.read_text().strip() if pointer.exists() else None}
result['record_errors'] = record_errors
print(json.dumps(result))
'''


def status(device, root, batch=False):
    result = ssh(device, "python3 - " + Q(str(root)), input=REMOTE_STATUS, text=True, capture_output=True, batch=batch)
    return json.loads(result.stdout)


REMOTE_DOCTOR = r'''
import json,sys,time
from pathlib import Path
root = Path(sys.argv[1])
if Path('/tmp/rgds-session.lock').exists():
    raise SystemExit('An RGDS session/recovery lock exists. Exit to stock or reboot before repairing activity records.')
for entry in Path('/proc').iterdir():
    if not entry.name.isdigit(): continue
    try:
        if int(entry.name) == __import__('os').getpid(): continue
        name=(entry/'comm').read_text().strip().lower()
        args=(entry/'cmdline').read_bytes().split(b'\x00')
        fields=(entry/'stat').read_text().rsplit(')',1)[1].split()
        if fields[0] == 'Z': continue
        owned = any(b'/rgds/releases/' in arg and (arg.endswith(b'/runtime/emulators.py') or arg.endswith(b'/runtime/session.py') or arg.endswith(b'/bin/rgds-launcher')) for arg in args)
        if owned or name in ('rgds-launcher','drastic','dsperate','nnddss'):
            raise SystemExit('A launcher or emulator is still running. Exit it normally before repairing activity records.')
    except (FileNotFoundError,ProcessLookupError): pass
for name in ('core.json','launcher.json'):
    path=root/'userdata/session'/name
    if not path.exists(): continue
    try:
        record=json.loads(path.read_text())
        pid=record['pid']
        fields=Path('/proc',str(pid),'stat').read_text().rsplit(')',1)[1].split()
        if fields[0] != 'Z' and fields[19] == str(record['start']):
            raise SystemExit('An ownership record still points to a live process. Leaving it untouched.')
    except (FileNotFoundError,ValueError,KeyError,TypeError,IndexError): pass
    backup=path.with_name(path.stem+'.stale-'+str(time.time_ns())+'.json')
    path.rename(backup)
    print('Preserved stale '+name+' as '+backup.name)
print('Activity records repaired. Run make dev-device again.')
'''


def doctor(device, root):
    ssh(device, "python3 -B - " + Q(str(root)), input=REMOTE_DOCTOR, text=True, batch=True)


def latest_package():
    name = (ROOT / "dist/latest.txt").read_text().strip()
    if Path(name).name != name:
        raise ValueError("Invalid latest package filename")
    return ROOT / "dist" / name


def deploy(device, root, archive, batch=False):
    upload = root / "inbox" / archive.name
    ssh(device, "test -d " + Q(str(root.parent)) + " && mkdir -p " + Q(str(root / "inbox")), batch=batch)
    with archive.open("rb") as stream:
        ssh(device, "cat > " + Q(str(upload)), stdin=stream, batch=batch)
    ssh(device, "python3 -B - " + Q(str(root)) + " " + Q(str(upload)), input=REMOTE_INSTALL, text=True, batch=batch)


REMOTE_THEME = r'''
import os, sys
from pathlib import Path
path = Path(sys.argv[1])/'userdata/dev/theme.ini'
content = sys.stdin.buffer.read(65537)
if len(content) > 65536: raise ValueError('Theme exceeds 64 KiB')
path.parent.mkdir(parents=True,exist_ok=True)
temporary = path.with_name('theme.ini.upload')
with temporary.open('wb') as stream:
    stream.write(content); stream.flush(); os.fsync(stream.fileno())
temporary.replace(path)
'''


def sync_theme(device, root, theme, batch=False):
    # A writable override keeps the hashed release itself intact.
    content = theme.read_bytes()
    validate_theme(content)
    command = "python3 -c " + Q(REMOTE_THEME) + " " + Q(str(root))
    ssh(device, command, input=content, batch=batch)


def validate_theme(content):
    if len(content) > 65536:
        raise ValueError("Theme exceeds 64 KiB")
    schema = False
    keys = {"background", "panel", "ink", "muted", "accent", "line", "slot", "on_accent", "focus"}
    for line in content.decode("utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        key, separator, value = line.partition("=")
        if not separator:
            raise ValueError("Theme line lacks =")
        if key == "schema" and value == "1":
            schema = True
        elif key == "mode" and value in ("light", "dark"):
            continue
        elif key not in keys or not re.fullmatch(r"[0-9a-fA-F]{6}", value):
            raise ValueError("Invalid theme field: " + key)
    if not schema:
        raise ValueError("Theme schema is missing")


def run_session(device, root, dev=False, batch=False):
    log = root / "userdata/runner.log"
    command = "mkdir -p " + Q(str(log.parent)) + "; nohup sh " + Q(str(root / "main.sh"))
    if dev:
        command += " --dev"
    command += " > " + Q(str(log)) + " 2>&1 < /dev/null &"
    ssh(device, command, batch=batch)


def start_dev_session(device, root):
    run_session(device, root, dev=True, batch=True)
    time.sleep(2)
    if not status(device, root, batch=True)["launcher"]:
        result = ssh(
            device,
            "tail -n 30 " + Q(str(root / "userdata/runner.log")) + " " + Q(str(root / "userdata/session.log")) +
            " 2>&1 || :",
            text=True,
            capture_output=True,
            batch=True
        )
        raise RuntimeError("Device launcher did not stay running.\n" + result.stdout)


def active_command(root, action):
    return "RGDS_ID=$(cat " + Q(str(root / "state/active")
                                ) + "); case \"$RGDS_ID\" in ''|*[!a-z0-9.-]*|.*) exit 1;; esac; python3 -B " + Q(
                                    str(root / "releases")
                                ) + '/"$RGDS_ID"/runtime/activate.py ' + action + " --root " + Q(str(root))


def configured_root(device, sd_root=None):
    """An explicit mount wins; otherwise follow the configured installation, even in stock mode."""
    if sd_root is not None:
        if not sd_root.is_absolute() or ".." in sd_root.parts:
            raise ValueError("--sd-root must be an absolute mount path without ..")
        return sd_root / "rgds"
    script = r"""
import json
from pathlib import Path
root=Path('/mnt/mmc/rgds')
try:
 c=Path('/userdata/rgds-boot');v=json.loads((c/'config.json').read_text())
 if not isinstance(v,dict):raise ValueError('Invalid configuration')
 p=Path(v.get('root',''))
 if v.get('mode') in ('rgds','stock') and p.is_absolute() and '..' not in p.parts and '\n' not in str(p) and (p/'main.sh').is_file():root=p
except (OSError,ValueError,TypeError):pass
print(root)
"""
    result = ssh(device, "python3 -B -", batch=True, input=script, text=True, capture_output=True)
    root = Path(result.stdout.strip())
    if not root.is_absolute() or ".." in root.parts or "\n" in str(root):
        raise ValueError("Invalid configured RGDS root")
    print("Using RGDS installation: " + str(root))
    return root


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("inventory", "deploy", "run", "logs", "rollback", "status", "doctor"))
    parser.add_argument("--device", required=True)
    parser.add_argument("--sd-root", default=None, type=Path)
    parser.add_argument("--dev", action="store_true", help="Start with the live theme override")
    args = parser.parse_args()
    device = target(args.device)
    root = configured_root(device, args.sd_root)
    if args.action == "inventory":
        result = ssh(device, "sh -s", input=(ROOT / "runtime/inventory.sh").read_text(), text=True, capture_output=True)
        path = ROOT / "docs" / ("device-inventory-" + datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S") + ".txt")
        path.write_text(result.stdout)
        print("Saved " + str(path))
    elif args.action == "deploy":
        deploy(device, root, latest_package())
    elif args.action == "run":
        run_session(device, root, dev=args.dev)
        print("Started temporary RGDS session. make logs shows its output.")
    elif args.action == "status":
        print(json.dumps(status(device, root), indent=2))
    elif args.action == "doctor":
        doctor(device, root)
    elif args.action == "logs":
        ssh(device, "tail -n 80 -f " + Q(str(root / "userdata/session.log")))
    else:
        ssh(device, active_command(root, "rollback"))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print("RGDS device: " + str(error), file=sys.stderr)
        sys.exit(1)
