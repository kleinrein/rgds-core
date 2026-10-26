#!/usr/bin/env python3
"""PortMaster ports in RG DS Core (docs/PORTMASTER_PLAN.md).

PortMaster is upstream's and updates itself; RG DS Core installs its latest
release on first use and never edits its files. What we add are files
PortMaster already knows how to use, written again after every PortMaster
update: control.txt (as the CFW variants have), mod_Buildroot.txt (the stock
card reports CFW_NAME=Buildroot) and our pad's controller line.

Everything lives in one folder on the games card, outside rgds/, so updates,
rollbacks and uninstalling RG DS Core never touch it:

    /mnt/mmc/portmaster/PortMaster   the control folder (XDG_DATA_HOME/PortMaster)
    /mnt/mmc/portmaster/ports        port scripts and game folders
    /mnt/mmc/portmaster/home         the ports' $HOME

The card is FAT32 and case-insensitive, so /mnt/mmc/ports would be the stock
menu's Ports folder; that one, the stock PortMaster on /mnt/ports and /roms are
left alone.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import urllib.request
import zipfile

sys.dont_write_bytecode = True
from fields import read_fields, write_fields

ROOT = Path(os.environ.get('RGDS_PORTMASTER_ROOT', '/mnt/mmc/portmaster'))
RELEASE = 'https://api.github.com/repos/PortsMaster/PortMaster-GUI/releases/latest'
CORE = 'portmaster'
ENTRY = 'PortMaster.sh'  # the PortMaster entry in Ports (runtime/portmaster_files/entry)
MARK = '# RG DS Core\'s control.txt'
# RG DS Core's pad (runtime/emulators.py): labelled A is SDL a, Select is back, Home is guide.
PAD = (
    '1900b655010000000100000000010000,ANBERNIC-rk3568-keys,a:b0,b:b1,x:b3,y:b2,back:b6,guide:b8,start:b7,'
    'leftstick:b9,rightstick:b12,leftshoulder:b4,rightshoulder:b5,dpup:h0.1,dpleft:h0.8,dpdown:h0.4,'
    'dpright:h0.2,leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:b10,righttrigger:b11,platform:Linux,'
)


class Folders:
    def __init__(self, root=None):
        self.root = Path(root) if root else ROOT
        self.control = self.root / 'PortMaster'
        self.ports = self.root / 'ports'
        self.home = self.root / 'home'
        self.autoinstall = self.root / 'autoinstall'
        self.download = self.root / '.download'
        self.record = self.root / 'rgds.ini'


def files(app):
    return Path(app) / 'runtime/portmaster_files'


def entry_folder(app):
    return files(app) / 'entry'


def is_entry(app, rom):
    try:
        return Path(rom).resolve() == (entry_folder(app) / ENTRY).resolve()
    except OSError:
        return False


def installed(folders=None):
    folders = folders or Folders()
    return (folders.control / 'harbourmaster').is_file() and (folders.control / 'version').is_file()


def version(folders=None):
    folders = folders or Folders()
    try:
        return (folders.control / 'version').read_text().strip()
    except OSError:
        return ''


def panel_size(drm=Path('/sys/class/drm')):
    """The top panel's mode (DSI-1): 640x480 on the RG DS, 1024x768 on the Plus."""
    for name in ('card0-DSI-1', 'card1-DSI-1'):
        try:
            mode = (drm / name / 'modes').read_text().split()[0]
            width, height = (int(v) for v in mode.split('x')[:2])
            if 0 < width <= 4096 and 0 < height <= 4096: return max(width, height), min(width, height)
        except (OSError, IndexError, ValueError):
            continue
    return 640, 480


def overlay(app, folders):
    """Our files, rendered: {name in the control folder: text}."""
    width, height = panel_size()
    control = (files(app) / 'control.txt').read_text()
    for key, value in (
        ('@CONTROL@', str(folders.control)), ('@DIRECTORY@', str(folders.root).lstrip('/')), ('@PAD@', PAD),
        ('@WIDTH@', str(width)), ('@HEIGHT@', str(height))
    ):
        control = control.replace(key, value)
    return {
        'control.txt': control,
        'mod_Buildroot.txt': (files(app) / 'mod_Buildroot.txt').read_text(),
        'gamecontrollerdb.rgds.txt': PAD + '\n',
    }


def replace_text(path, text):
    pending = path.with_name(path.name + '.rgds-pending')
    pending.write_text(text)
    pending.replace(path)


def ensure(app, folders=None):
    """Write our files into PortMaster's folder if an update replaced them. Returns True when it wrote."""
    folders = folders or Folders()
    if not installed(folders):
        raise ValueError('PortMaster is not installed.')
    for folder in (folders.ports, folders.home, folders.autoinstall):
        folder.mkdir(parents=True, exist_ok=True)
    wrote = False
    for name, text in overlay(app, folders).items():
        path = folders.control / name
        try:
            current = path.read_text()
        except OSError:
            current = None
        if current == text:
            continue
        if name == 'control.txt' and current is not None and MARK not in current:
            # A PortMaster install or update put upstream's own back: keep it for reference.
            replace_text(folders.control / 'control.upstream.txt', current)
        replace_text(path, text)
        wrote = True
    record = read_fields(folders.record)
    if wrote or record.get('version') != version(folders):
        write_fields(folders.record, dict(schema=1, version=version(folders), applied=int(time.time())))
    return wrote


def hm_environment(env, folders=None):
    """What harbourmaster and the port scripts need to find our folders."""
    folders = folders or Folders()
    env.update(
        XDG_DATA_HOME=str(folders.root),
        HOME=str(folders.home),
        HM_TOOLS_DIR=str(folders.root),
        HM_PORTS_DIR=str(folders.ports),
        HM_SCRIPTS_DIR=str(folders.ports),
        PYTHONDONTWRITEBYTECODE='1'
    )
    return env


class Status:
    """Progress for the bottom screen while PortMaster installs (session/port-status.ini)."""
    def __init__(self, path):
        self.path = Path(path) if path else None
        self.shown = (None, -1, '')

    def __call__(self, message, percent=-1, detail=''):
        # message is fixed English the launcher translates; detail is shown as it is.
        print(
            'PortMaster: ' + message + ('' if percent < 0 else ' %d%%' % percent) + (': ' + detail if detail else ''),
            flush=True
        )
        if not self.path or (message, percent, detail) == self.shown: return
        self.shown = (message, percent, detail)
        try:
            write_fields(self.path, dict(message=message, percent=percent, detail=detail), durable=False)
        except OSError:
            pass


def _get(url, accept='application/octet-stream'):
    request = urllib.request.Request(url, headers={'User-Agent': 'RG-DS-Core', 'Accept': accept})
    return urllib.request.urlopen(request, timeout=30)


def latest_release():
    with _get(RELEASE, 'application/vnd.github+json') as response:
        release = json.load(response)
    assets = {asset['name']: asset for asset in release.get('assets', [])}
    if 'PortMaster.zip' not in assets or 'PortMaster.zip.md5' not in assets:
        raise ValueError('The latest PortMaster release has no PortMaster.zip.')
    return release['tag_name'], assets


def download(url, target, size, status):
    """Resumable download into target (a .partial file is kept across attempts)."""
    partial = target.with_name(target.name + '.partial')
    have = partial.stat().st_size if partial.is_file() else 0
    if have > size: partial.unlink()
    have = partial.stat().st_size if partial.is_file() else 0
    request = urllib.request.Request(url, headers={'User-Agent': 'RG-DS-Core', 'Range': 'bytes=%d-' % have})
    with urllib.request.urlopen(request, timeout=30) as response:
        if have and response.status != 206: have = 0
        with partial.open('ab' if have else 'wb') as out:
            while True:
                chunk = response.read(256 * 1024)
                if not chunk: break
                out.write(chunk)
                have += len(chunk)
                status('Downloading PortMaster', min(99, have * 100 // max(1, size)))
            out.flush()
            os.fsync(out.fileno())
    partial.replace(target)


def checked(path, md5, sha256):
    digest_md5, digest_sha = hashlib.md5(), hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest_md5.update(chunk)
            digest_sha.update(chunk)
    if digest_md5.hexdigest() != md5:
        raise ValueError('PortMaster.zip did not match its MD5; try again.')
    if sha256 and digest_sha.hexdigest() != sha256:
        raise ValueError('PortMaster.zip did not match GitHub\'s SHA-256; try again.')


def unpack(archive, root):
    """PortMaster.zip holds one PortMaster/ folder; nothing else is written."""
    with zipfile.ZipFile(archive) as zf:
        names = zf.namelist()
        bad = [n for n in names if n.startswith('/') or '..' in n.split('/') or not n.startswith('PortMaster/')]
        if bad: raise ValueError('PortMaster.zip has unexpected entries: ' + ', '.join(bad[:3]))
        zf.extractall(root)


def bootstrap(folders):
    """harbourmaster unpacks its Python libraries on its first run."""
    env = hm_environment(dict(os.environ), folders)
    subprocess.run(
        [sys.executable, '-B',
         str(folders.control / 'harbourmaster'), '--quiet', '--no-check', 'list'],
        cwd=str(folders.control),
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.STDOUT,
        timeout=180,
        check=False
    )


def install(app, folders=None, status=None):
    """Download, check and unpack the latest PortMaster, then add our files."""
    folders = folders or Folders()
    status = status or Status(None)
    folders.root.mkdir(parents=True, exist_ok=True)
    folders.download.mkdir(exist_ok=True)
    offline = folders.autoinstall / 'PortMaster.zip'
    if offline.is_file():
        status('Installing PortMaster from the card', 10)
        archive = offline
    else:
        status('Looking for the latest PortMaster', 0)
        tag, assets = latest_release()
        with _get(assets['PortMaster.zip.md5']['browser_download_url']) as response:
            md5 = response.read().decode().split()[0].strip().lower()
        digest = (assets['PortMaster.zip'].get('digest') or '')
        sha256 = digest.split(':', 1)[1].lower() if digest.startswith('sha256:') else ''
        archive = folders.download / ('PortMaster-' + tag + '.zip')
        if not archive.is_file():
            download(
                assets['PortMaster.zip']['browser_download_url'], archive, assets['PortMaster.zip']['size'], status
            )
        status('Checking PortMaster', 99, tag)
        try:
            checked(archive, md5, sha256)
        except ValueError:
            archive.unlink(missing_ok=True)
            raise
    status('Unpacking PortMaster', 99)
    unpack(archive, folders.root)
    status('Setting up PortMaster', 99)
    bootstrap(folders)
    if not installed(folders):
        raise ValueError('PortMaster did not finish setting up.')
    ensure(app, folders)
    if archive == offline:
        offline.rename(offline.with_name('PortMaster.zip.installed'))
    else:
        shutil.rmtree(folders.download, ignore_errors=True)
    compat(folders, status)
    status('PortMaster is ready', 100, version(folders))


def port_environment(env, app, folders=None):
    """A port's environment: the session's display and sound, our folders, no engine hooks."""
    folders = folders or Folders()
    hm_environment(env, folders)
    env.update(
        SDL_VIDEODRIVER='wayland',
        SDL_AUDIODRIVER='alsa',
        # Our top-panel role; and pad input keeps working whatever has keyboard focus.
        SDL_VIDEO_WAYLAND_WMCLASS='org.rgds.top',
        SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS='1',
        PM_CUR_TTY='/dev/null',
        RGDS_APP=str(app)
    )
    from portmaster_compat import environment
    return environment(env, folders.root)


def compat(folders=None, status=None):
    """The libraries and GNU tools ports expect (runtime/portmaster_compat.py), installed or brought up to date.
    False when that failed: ports still start, some of them without what they need."""
    import portmaster_compat
    folders = folders or Folders()
    if portmaster_compat.current(folders.root): return True
    try:
        portmaster_compat.install(folders.root, status)
        return True
    except Exception as error:  # offline, or a mirror gone: the next PortMaster open or launch tries again
        print('PortMaster compat: ' + str(error), file=sys.stderr, flush=True)
        return False


def launch_plan(app, data, rom, env, prepare=True, folders=None):
    """emulators.launch_plan for the portmaster engine: a port script, or the PortMaster entry."""
    folders = folders or Folders()
    app, rom = Path(app), Path(rom)
    status_path = Path(data) / 'session/port-status.ini'
    if prepare and not is_entry(app, rom) and rom.suffix.lower() == '.sh' and installed(folders):
        compat(folders, Status(status_path))
        status_path.unlink(missing_ok=True)  # the companion shows the info screen, not a finished step
        folder = port_folder(rom, folders)
        if folder is not None and armhf_only(folder):
            import portmaster_compat
            try:
                portmaster_compat.prepare_armhf(folders.root, folder)
            except OSError as error:  # the port then fails as it would have without the layer
                print('PortMaster armhf: ' + str(error), file=sys.stderr, flush=True)
    else:
        folder = None
    env = port_environment(env, app, folders)
    if folder is not None:
        import portmaster_compat
        if portmaster_compat.gl_port(folder, port_json(folder).get('attr') or {}):
            portmaster_compat.gl_environment(env, folders.root)
    env['RGDS_PORT_STATUS'] = str(status_path)
    if is_entry(app, rom):
        command = [sys.executable, '-B', str(app / 'runtime/portmaster.py'), 'open', '--app', str(app)]
        return {'command': command, 'cwd': str(app), 'env': env, 'port': 'PortMaster'}
    if rom.suffix.lower() != '.sh':
        raise ValueError('A port is started from its .sh script.')
    if prepare:
        if not installed(folders):
            raise ValueError('PortMaster is not installed. Open PortMaster in Ports first.')
        ensure(app, folders)
    return {'command': ['bash', str(rom)], 'cwd': str(rom.parent), 'env': env, 'port': rom.stem}


def port_folder(script, folders=None):
    """A port's own folder: its script's second line names the port zip ("# PORTMASTER: catsonmars.zip, ...")."""
    folders = folders or Folders()
    try:
        with Path(script).open(errors='replace') as stream:
            head = [stream.readline() for _ in range(3)]
    except OSError:
        return None
    for line in head:
        if line.startswith('# PORTMASTER:'):
            name = line.split(':', 1)[1].split(',')[0].strip()
            folder = folders.ports / Path(name).stem
            if name.endswith('.zip') and '/' not in name and folder.is_dir(): return folder
    return None


# Where a port's own files go when the owner brings them, in the order PortMaster's porters use them.
FILE_FOLDERS = ('gamedata', 'assets', 'gamefiles', 'installer', 'game', 'data')


def port_json(folder):
    """The port.json harbourmaster keeps in a port's folder; {} when it is missing or unreadable."""
    try:
        value = json.loads((Path(folder) / 'port.json').read_text(errors='replace'))
        return value if isinstance(value, dict) else {}
    except (OSError, ValueError):
        return {}


def file_target(folder, instructions=''):
    """The folder (relative to the port's) the owner's game files go into: the one the instructions name, else the
    first of FILE_FOLDERS the port ships, else the port's own folder ('')."""
    folder = Path(folder)
    shipped = [name for name in FILE_FOLDERS if (folder / name).is_dir()]
    text = (instructions or '').lower().replace('\\', '/')
    named = [name for name in FILE_FOLDERS if '/' + name in text or ' ' + name + ' folder' in text]
    for name in named:
        if name in shipped: return name
    return shipped[0] if shipped else (named[0] if named else '')


def details(script, folders=None):
    """What a port needs from its owner: its title, whether it is ready to run, the instructions, the stores that sell
    it and where its files go. None for a script that is not an installed port."""
    folders = folders or Folders()
    folder = port_folder(script, folders)
    if folder is None: return None
    attr = port_json(folder).get('attr') or {}
    stores = [
        dict(name=str(store.get('name') or '').strip(), url=str(store.get('gameurl') or '').strip())
        for store in attr.get('store') or [] if isinstance(store, dict)
    ]
    instructions = str(attr.get('inst') or '').strip()
    return dict(
        id=folder.name,
        script=str(script),
        folder=str(folder),
        title=str(attr.get('title') or Path(script).stem),
        ready=bool(attr.get('rtr', True)),
        instructions=instructions,
        stores=[store for store in stores if store['url'].startswith(('https://', 'http://'))],
        target=file_target(folder, instructions)
    )


def armhf_only(folder):
    """A port built for 32-bit ARM only (port.json arch lists armhf and not aarch64)."""
    arch = (port_json(folder).get('attr') or {}).get('arch') or []
    return 'armhf' in arch and 'aarch64' not in arch


def notes(data, folders=None):
    """library/<key>/port.ini for each port, which the launcher reads: needs_files=1 when the port is not ready to
    run, and the folder its files go into."""
    from library import game_key
    folders = folders or Folders()
    written = 0
    for script in sorted(folders.ports.glob('*.sh')) if folders.ports.is_dir() else []:
        port = details(script, folders)
        if port is None: continue
        record = dict(
            id=port['id'],
            needs_files=int(not port['ready']),
            target=port['target'],
            instructions=' '.join(port['instructions'].split())
        )
        path = Path(data) / 'library' / game_key(script) / 'port.ini'
        if read_fields(path) != {key: str(value) for key, value in record.items()}:
            path.parent.mkdir(parents=True, exist_ok=True)
            write_fields(path, record)
            written += 1
    return written


def _png_rgba(path):
    """(width, height, rows of RGBA bytes) of an 8-bit RGBA, non-interlaced PNG; None for anything else."""
    import struct
    import zlib
    data = Path(path).read_bytes()
    if data[:8] != b'\x89PNG\r\n\x1a\n': return None
    pos, idat, header = 8, b'', None
    while pos + 8 <= len(data):
        size, kind = struct.unpack('>I4s', data[pos:pos + 8])
        chunk = data[pos + 8:pos + 8 + size]
        if kind == b'IHDR': header = struct.unpack('>IIBBBBB', chunk)
        elif kind == b'IDAT': idat += chunk
        pos += 12 + size
    if not header or header[2:5] != (8, 6, 0) or header[6] != 0: return None
    width, height = header[:2]
    if width * height > 1024 * 1024: return None
    raw, stride, rows, previous, at = zlib.decompress(idat), width * 4, [], bytearray(width * 4), 0
    for _ in range(height):
        kind, line = raw[at], bytearray(raw[at + 1:at + 1 + stride])
        at += 1 + stride
        for x in range(stride):
            a, b = line[x - 4] if x >= 4 else 0, previous[x]
            c = previous[x - 4] if x >= 4 else 0
            if kind == 1: line[x] = (line[x] + a) & 255
            elif kind == 2: line[x] = (line[x] + b) & 255
            elif kind == 3: line[x] = (line[x] + (a + b) // 2) & 255
            elif kind == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        previous = line
    return width, height, rows


def icon_bmp(source, target):
    """An app icon with transparent padding, cropped to what is drawn, as a 32-bit BMP with alpha. False if not RGBA."""
    import struct
    decoded = _png_rgba(source)
    if not decoded: return False
    width, height, rows = decoded
    xs = [x for x in range(width) if any(row[x * 4 + 3] > 16 for row in rows)]
    ys = [y for y in range(height) if any(rows[y][x * 4 + 3] > 16 for x in range(width))]
    if not xs or not ys: return False
    left, right, top, bottom = xs[0], xs[-1] + 1, ys[0], ys[-1] + 1
    w, h = right - left, bottom - top
    pixels = bytearray()
    for row in reversed(rows[top:bottom]):  # bottom-up
        for x in range(left, right):
            r, g, b, a = row[x * 4:x * 4 + 4]
            pixels += bytes((b, g, r, a))
    info = struct.pack('<IiiHHIIiiII', 108, w, h, 1, 32, 3, len(pixels), 2835, 2835, 0, 0)
    info += struct.pack('<IIII', 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000) + b'BGRs' + bytes(48)
    target.parent.mkdir(parents=True, exist_ok=True)
    pending = target.with_suffix('.pending.bmp')
    pending.write_bytes(
        b'BM' + struct.pack('<IHHI', 14 + len(info) + len(pixels), 0, 0, 14 + len(info)) + info + pixels
    )
    pending.replace(target)
    return True


def artwork(app, data, folders=None):
    """Each port's own picture as its library cover (library/<key>/box.bmp), where it has none yet."""
    from library import game_key
    from scraper import image_to_bmp
    folders = folders or Folders()
    # PortMaster's own app icon (the one CFW menus use), else its screenshot.
    pictures = {(entry_folder(app) / ENTRY): [folders.control / 'trimui/icon.png', folders.control / 'screenshot.png']}
    if folders.ports.is_dir():
        for script in sorted(folders.ports.glob('*.sh')):
            folder = port_folder(script, folders)
            if folder:
                pictures[script] = [
                    folder / name for name in ('cover.png', 'cover.jpg', 'screenshot.png', 'screenshot.jpg')
                ]
    made = 0
    for script, candidates in pictures.items():
        target = Path(data) / 'library' / game_key(script) / 'box.bmp'
        source = next((path for path in candidates if path.is_file()), None)
        if target.is_file() or source is None or source.stat().st_size > 8 * 1024 * 1024: continue
        try:
            if not (source.name == 'icon.png' and icon_bmp(source, target)):
                image_to_bmp(source.read_bytes(), target)
            made += 1
        except Exception as error:  # a picture the decoder refuses keeps the plain cartridge
            print('PortMaster artwork for %s: %s' % (script.name, error), file=sys.stderr)
    return made


def roots(app, folders=None):
    """The folders the launcher lists as the Ports system: the PortMaster entry and the ports."""
    folders = folders or Folders()
    return [entry_folder(app), folders.ports]


def open_portmaster(app, folders=None):
    """The PortMaster entry: install on first use, add our files, then hand over to PortMaster's GUI."""
    folders = folders or Folders()
    status = Status(os.environ.get('RGDS_PORT_STATUS'))
    try:
        if not installed(folders): install(app, folders, status)
        else:
            ensure(app, folders)
            compat(folders, status)
    except Exception as error:  # shown on the bottom screen, then the launcher comes back
        status('Could not set up PortMaster', -1, str(error))
        time.sleep(6)
        return 1
    env = port_environment(hm_environment(dict(os.environ), folders), app, folders)
    os.chdir(folders.control)
    os.execvpe('bash', ['bash', str(folders.control / 'PortMaster.sh')], env)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('action', choices=('status', 'install', 'ensure', 'open'))
    parser.add_argument('--app', type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    folders = Folders()
    if args.action == 'status':
        print(json.dumps({'root': str(folders.root), 'installed': installed(folders), 'version': version(folders)}))
        return 0
    if args.action == 'install':
        install(args.app, folders, Status(os.environ.get('RGDS_PORT_STATUS')))
        return 0
    if args.action == 'ensure':
        print('wrote' if ensure(args.app, folders) else 'unchanged')
        return 0
    return open_portmaster(args.app, folders)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print('PortMaster: ' + str(error), file=sys.stderr)
        sys.exit(1)
