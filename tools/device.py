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
REMOTE_INSTALL = r'''
import hashlib, json, os, platform, re, shutil, stat, sys, zipfile
from pathlib import Path
sys.dont_write_bytecode = True
root, archive_path = map(Path, sys.argv[1:])
if sys.version_info < (3,8) or platform.machine() not in ('aarch64', 'arm64'):
    raise SystemExit('Need Linux AArch64 and Python 3.8+; keep stock and capture inventory.')
allowed_apps = {'Try RGDS.sh','RGDS Probe.sh','RGDS Inventory.sh','RGDS Recovery.sh'}
with zipfile.ZipFile(archive_path) as archive:
    entries = archive.infolist()
    if sum(e.file_size for e in entries) > 256*1024*1024:
        raise ValueError('Package exceeds size limit')
    names = [e.filename for e in entries]
    if len(names) != len(set(names)):
        raise ValueError('Duplicate archive entries')
    build = archive.read('rgds/state/active').decode().strip()
    if not re.fullmatch(r'[a-z0-9][a-z0-9.-]{0,100}',build):
        raise ValueError('Invalid build ID')
    prefix = 'rgds/releases/' + build + '/'
    manifest = json.loads(archive.read(prefix+'manifest.json'))
    if manifest.get('build') != build or manifest.get('schema') != 1 or manifest.get('project') != 'rgds' or manifest.get('target') != 'aarch64-linux-gnu':
        raise ValueError('Wrong manifest')
    for e in entries:
        p = Path(e.filename)
        if p.is_absolute() or '..' in p.parts or '\\' in e.filename or stat.S_ISLNK(e.external_attr >> 16):
            raise ValueError('Unsafe archive path')
        permitted = e.filename.startswith(prefix) or e.filename.startswith('rgds/docs/') or e.filename in ('rgds/main.sh','rgds/inventory.sh','rgds/state/active') or (p.parent == Path('Roms/APPS') and p.name in allowed_apps)
        if not permitted or e.is_dir():
            raise ValueError('Unexpected package entry: '+e.filename)
    actual = {n[len(prefix):] for n in names if n.startswith(prefix) and n != prefix+'manifest.json'}
    files = manifest.get('files',{})
    if actual != set(files) or 'bin/rgds-launcher' not in files:
        raise ValueError('Manifest file list mismatch')
    for name, digest in files.items():
        if not re.fullmatch(r'[0-9a-f]{64}',digest) or hashlib.sha256(archive.read(prefix+name)).hexdigest() != digest:
            raise ValueError('SHA-256 mismatch: '+name)
    stage = root/'inbox'/('stage-'+build)
    if stage.exists(): shutil.rmtree(stage)
    stage.mkdir(parents=True)
    for e in entries:
        destination = stage/e.filename
        destination.parent.mkdir(parents=True,exist_ok=True)
        destination.write_bytes(archive.read(e))
        destination.chmod(0o755 if (e.external_attr >> 16)&0o111 else 0o644)
app = stage/'rgds/releases'/build
sys.path.insert(0,str(app/'runtime'))
from activate import activate, stop_session, verify_release
verify_release(app)
stop_session(root/'userdata')
destination = root/'releases'/build
destination.parent.mkdir(parents=True,exist_ok=True)
if destination.exists():
    verify_release(destination)
else:
    app.rename(destination)
# Only private development deployment updates these tiny bootstrap files.
for name in ('main.sh','inventory.sh'):
    if (root/name).exists():
        (root/'state').mkdir(exist_ok=True)
        shutil.copy2(root/name,root/'state'/(name+'.previous'))
    shutil.copy2(stage/'rgds'/name,root/name)
apps = root.parent/'Roms/APPS'
apps.mkdir(parents=True,exist_ok=True)
for source in (stage/'Roms/APPS').iterdir(): shutil.copy2(source,apps/source.name)
activate(root,build,stop=False)
shutil.rmtree(stage)
archive_path.unlink()
print('Activated '+build+'. Run make run or APPS > RGDS Probe / Try RGDS.')
'''

def target(value):
    if not value or value.startswith("-") or not re.fullmatch(r"[A-Za-z0-9_.:@\[\]-]+", value):
        raise ValueError("Set DEVICE to the RG DS SSH address, for example root@192.168.1.42.")
    return value if "@" in value else "root@" + value

def ssh(device, command, batch=False, **kwargs):
    options = ["-o", "ConnectTimeout=5", "-o", "ServerAliveInterval=5", "-o", "ServerAliveCountMax=2"]
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
        result = ssh(device, "tail -n 30 " + Q(str(root / "userdata/runner.log")) + " " + Q(str(root / "userdata/session.log")) + " 2>&1 || :", text=True, capture_output=True, batch=True)
        raise RuntimeError("Device launcher did not stay running.\n" + result.stdout)

def active_command(root, action):
    return "RGDS_ID=$(cat " + Q(str(root / "state/active")) + "); case \"$RGDS_ID\" in ''|*[!a-z0-9.-]*|.*) exit 1;; esac; python3 -B " + Q(str(root / "releases")) + '/"$RGDS_ID"/runtime/activate.py ' + action + " --root " + Q(str(root))

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("inventory", "deploy", "run", "logs", "rollback", "status", "doctor"))
    parser.add_argument("--device", required=True)
    parser.add_argument("--sd-root", default="/mnt/mmc", type=Path)
    parser.add_argument("--dev", action="store_true", help="Start with the live theme override")
    args = parser.parse_args()
    device = target(args.device)
    if not args.sd_root.is_absolute() or ".." in args.sd_root.parts:
        raise ValueError("--sd-root must be an absolute mount path without ..")
    root = args.sd_root / "rgds"
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
