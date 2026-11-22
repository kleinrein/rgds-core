"""Apps: programs anyone can package for RG DS Core without us (docs/APPS.md).

An app is a folder in the apps folder (/data/apps on RG DS OS) with an app.ini beside its program:

    /data/apps/hello/app.ini      name=Hello, exec=hello.sh, and the optional keys in KEYS
    /data/apps/hello/hello.sh     started with bash (any other exec is run as it is)
    /data/apps/hello/icon.png     its library cover (or cover.png, screenshot.png)

The launcher lists apps on the Ports shelf and starts them as it starts a port (runtime/port_runner.py): one cgroup
for the whole process tree, the top panel's DRM lease on RG DS OS, the bottom screen's info page, and the same Home
menu, game switcher and quit. A zip dropped into the apps folder is installed at the next start (install_pending).
What an app writes belongs in $RGDS_APP_DATA (also $HOME), which an update of the app keeps.

    python3 runtime/apps.py check FOLDER      what a porter must fix before shipping (exit 1 when anything)
    python3 runtime/apps.py install ZIP       unpack ZIP into the apps folder, replacing an older copy
    python3 runtime/apps.py list              the installed apps, as JSON
    python3 runtime/apps.py artwork --data D  install zips waiting in the folder, then make missing covers
"""
import argparse
import json
import os
from pathlib import Path, PurePosixPath
import re
import shlex
import shutil
import sys
import zipfile

sys.dont_write_bytecode = True
from fields import read_fields

MANIFEST = 'app.ini'
SCHEMA = 1
# Every key app.ini may have; anything else is reported by check() so a typo does not pass silently.
KEYS = (
    'schema', 'name', 'exec', 'args', 'icon', 'cover', 'author', 'version', 'description', 'url', 'arch', 'gl',
    'min_core'
)
ARCHES = ('aarch64', 'armhf')
GLS = ('', 'gles', 'gl4es')
ID = re.compile(r'^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$')
COVERS = ('cover.png', 'cover.jpg', 'icon.png', 'screenshot.png', 'screenshot.jpg')
ZIP_LIMIT = 2 << 30  # unpacked size: a card's free space is checked as well


def default_root():
    from linux_platform import rgds_os
    return Path('/data/apps') if rgds_os() else Path('/mnt/mmc/apps')


def root():
    return Path(os.environ.get('RGDS_APPS_ROOT') or default_root())


def is_app(rom):
    return Path(rom).name == MANIFEST


def _version(text):
    return tuple(int(part) for part in re.findall(r'\d+', text or '')[:3])


def core_version(app):
    try:
        return (Path(app) / 'VERSION').read_text().strip()
    except OSError:
        return ''


def manifest(folder):
    """app.ini with its defaults filled in; {} when the folder has none."""
    folder = Path(folder)
    fields = read_fields(folder / MANIFEST)
    if not fields: return {}
    # A BOM from a Windows editor would otherwise be part of the first key.
    fields = {key.strip().lstrip('\ufeff').strip(): value.strip() for key, value in fields.items()}
    fields.setdefault('name', folder.name)
    fields.setdefault('arch', 'aarch64')
    fields.setdefault('gl', '')
    fields['id'] = folder.name
    return fields


def check(folder, app=None):
    """Everything an author should fix, as sentences; [] when the app is clean. The ones that stop it from running
    are blockers(); the rest (an unknown key, a missing picture, a folder name a zip could not carry) are advice."""
    blocking, advice = review(folder, app)
    return blocking + advice


def blockers(folder, app=None):
    """What stops this app from starting here; [] when it can run. A launch refuses only these."""
    return review(folder, app)[0]


def review(folder, app=None):
    """(blockers, advice) for an app folder."""
    folder = Path(folder)
    if not (folder / MANIFEST).is_file(): return ['There is no app.ini in %s.' % folder], []
    fields = manifest(folder)
    problems, advice = [], []
    if not ID.match(folder.name):
        advice.append(
            'The folder name should be letters, digits, dots, dashes or underscores (at most 64), so a '
            'zip of it installs.'
        )
    if fields.get('schema', str(SCHEMA)) != str(SCHEMA):
        problems.append('schema=%s is not known; this RG DS Core reads schema=%d.' % (fields['schema'], SCHEMA))
    # Keys a later RG DS Core may add are passed over here, so an app that uses one still starts.
    for key in sorted(set(fields) - set(KEYS) - {'id'}):
        advice.append('%s= is not an app.ini key this RG DS Core knows.' % key)
    program = fields.get('exec', '')
    if not program:
        problems.append('exec= is missing: name the program or script to start.')
    else:
        target = _inside(folder, program)
        if target is None: problems.append('exec=%s must be a path inside the app folder.' % program)
        elif not target.is_file(): problems.append('exec=%s is not in the folder.' % program)
        elif target.suffix != '.sh' and not os.access(target, os.X_OK):
            problems.append('exec=%s is not executable (chmod +x, or make it a .sh script).' % program)
    try:
        shlex.split(fields.get('args', ''))
    except ValueError:
        problems.append('args= has an unclosed quote.')
    if fields['arch'] not in ARCHES: problems.append('arch= must be aarch64 or armhf.')
    if fields['gl'] not in GLS: problems.append('gl= must be gles or gl4es, or left out.')
    for key in ('icon', 'cover'):
        if fields.get(key) and (_inside(folder, fields[key]) is None or not _inside(folder, fields[key]).is_file()):
            advice.append('%s=%s is not in the folder.' % (key, fields[key]))
    wanted = fields.get('min_core', '')
    if wanted and app is not None and _version(core_version(app)) and _version(core_version(app)) < _version(wanted):
        problems.append('Needs RG DS Core %s or later; this is %s.' % (wanted, core_version(app)))
    return problems, advice


def _inside(folder, relative):
    """folder/relative when it stays inside folder; None for an absolute path or one that climbs out."""
    path = PurePosixPath(relative)
    if path.is_absolute() or '..' in path.parts or not path.parts: return None
    return Path(folder) / Path(*path.parts)


def installed(apps_root=None):
    apps_root = Path(apps_root) if apps_root else root()
    if not apps_root.is_dir(): return []
    return [
        manifest(folder) for folder in sorted(apps_root.iterdir())
        if folder.is_dir() and not folder.name.startswith('.') and (folder / MANIFEST).is_file()
    ]


# Installing ---------------------------------------------------------------------------------------------------------


def _members(archive):
    """The zip's files under the app's own folder name: (id, [(member, relative path)]). The zip holds either one
    folder with app.ini in it, or app.ini at its top (then the zip's own name is the folder's)."""
    names = [info for info in archive.infolist() if not info.is_dir()]
    paths = []
    for info in names:
        path = PurePosixPath(info.filename.replace('\\', '/'))
        if path.is_absolute() or '..' in path.parts: raise ValueError('The zip names a file outside its folder.')
        if (info.external_attr >> 16) & 0o170000 == 0o120000: raise ValueError('The zip holds a symbolic link.')
        if path.parts and path.parts[0] != '__MACOSX' and not path.name.startswith('._'): paths.append((info, path))
    if any(path == PurePosixPath(MANIFEST) for _, path in paths):
        return None, [(info, path) for info, path in paths]
    tops = {path.parts[0] for _, path in paths}
    if len(tops) == 1 and any(len(path.parts) == 2 and path.name == MANIFEST for _, path in paths):
        top = tops.pop()
        return top, [(info, PurePosixPath(*path.parts[1:])) for info, path in paths]
    raise ValueError('The zip has no app.ini, at its top or in one folder.')


def install(zip_path, apps_root=None, app=None):
    """Unpack an app's zip into the apps folder. An older copy is replaced whole; its data ($RGDS_APP_DATA) lives
    elsewhere and stays. Returns the app's id."""
    zip_path = Path(zip_path)
    apps_root = Path(apps_root) if apps_root else root()
    apps_root.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(zip_path) as archive:
        top, members = _members(archive)
        name = top or zip_path.stem
        if not ID.match(name): raise ValueError('"%s" is not a usable app folder name.' % name)
        size = sum(info.file_size for info, _ in members)
        if size > ZIP_LIMIT: raise ValueError('The app is larger than 2 GiB unpacked.')
        if shutil.disk_usage(apps_root).free < size + (64 << 20): raise ValueError('There is not enough free space.')
        staging = apps_root / ('.install-%s-%d' % (name, os.getpid()))
        if staging.exists(): shutil.rmtree(staging)
        staging.mkdir()
        try:
            for info, path in members:
                target = staging / Path(*path.parts)
                target.parent.mkdir(parents=True, exist_ok=True)
                with archive.open(info) as source, open(target, 'wb') as sink:
                    shutil.copyfileobj(source, sink, 1 << 20)
                mode = (info.external_attr >> 16) & 0o777
                # FAT and exFAT keep no modes; on a card that does, a program packed without one still runs.
                if mode & 0o111 or target.suffix in ('.sh', '') or path.parts[0] == 'bin':
                    try:
                        target.chmod((mode or 0o644) | 0o755)
                    except OSError:
                        pass
            problems = blockers(staging, app)
            if problems: raise ValueError(' '.join(problems))
            final, old = apps_root / name, apps_root / ('.old-%s-%d' % (name, os.getpid()))
            if final.exists(): final.rename(old)
            try:
                staging.rename(final)
            except OSError:
                if old.exists() and not final.exists(): old.rename(final)  # the copy that was there stays
                raise
            if old.exists(): shutil.rmtree(old, ignore_errors=True)
        finally:
            if staging.exists(): shutil.rmtree(staging, ignore_errors=True)
    return name


def recover(apps_root=None):
    """Finish what an install cut short (power lost between its two renames): an app whose folder is missing gets
    its previous copy back from .old-<id>-<pid>; other leftovers are removed. Returns the ids put back."""
    apps_root = Path(apps_root) if apps_root else root()
    if not apps_root.is_dir(): return []
    restored = []
    for leftover in sorted(apps_root.glob('.old-*')) + sorted(apps_root.glob('.install-*')):
        name = re.sub(r'-\d+$', '', leftover.name.split('-', 1)[1]) if '-' in leftover.name else ''
        final = apps_root / name
        try:
            if leftover.name.startswith('.old-') and name and not final.exists() and leftover.is_dir():
                leftover.rename(final)
                restored.append(name)
            else:
                shutil.rmtree(leftover, ignore_errors=True)
        except OSError as error:
            print('Apps: could not tidy %s: %s' % (leftover.name, error), file=sys.stderr, flush=True)
    return restored


def install_pending(apps_root=None, app=None):
    """Each zip waiting in the apps folder (copied there over file sharing or USB): installed, then moved into
    .installed/; one that fails stays, with NAME.zip.txt saying why. Returns the ids installed."""
    apps_root = Path(apps_root) if apps_root else root()
    if not apps_root.is_dir(): return []
    recover(apps_root)
    done = []
    for archive in sorted(apps_root.glob('*.zip')):
        if archive.name.startswith('._'): continue
        note = archive.with_name(archive.name + '.txt')
        try:
            done.append(install(archive, apps_root, app))
            keep = apps_root / '.installed'
            keep.mkdir(exist_ok=True)
            archive.replace(keep / archive.name)
            note.unlink(missing_ok=True)
        except (OSError, ValueError, zipfile.BadZipFile) as error:
            note.write_text('Not installed: %s\n' % error)
            print('Apps: %s: %s' % (archive.name, error), file=sys.stderr, flush=True)
    return done


def artwork(data, apps_root=None):
    """Each app's own picture as its library cover (library/<key>/box.bmp), where it has none yet."""
    from library import game_key
    from portmaster import icon_bmp
    made = 0
    for fields in installed(apps_root):
        folder = (Path(apps_root) if apps_root else root()) / fields['id']
        script = (folder / MANIFEST).resolve()
        target = Path(data) / 'library' / game_key(script) / 'box.bmp'
        named = [(_inside(folder, fields[key]), key == 'icon') for key in ('cover', 'icon') if fields.get(key)]
        found = [(folder / n, n == 'icon.png') for n in COVERS]
        source, icon = next(((p, i) for p, i in named + found if p is not None and p.is_file()), (None, False))
        if target.is_file() or source is None or source.stat().st_size > 8 * 1024 * 1024: continue
        try:
            # An icon is cropped to what it draws; a cover or screenshot keeps its own framing.
            if not (icon and source.suffix.lower() == '.png' and icon_bmp(source, target)):
                from scraper import image_to_bmp
                image_to_bmp(source.read_bytes(), target)
            made += 1
        except Exception as error:  # a picture the decoder refuses keeps the plain cartridge
            print('Apps: cover for %s: %s' % (fields['id'], error), file=sys.stderr)
    return made


# Starting -----------------------------------------------------------------------------------------------------------


def data_folder(data, app_id):
    """Where an app keeps what it writes: in RG DS Core's userdata, so an update of the app never removes it."""
    return Path(data) / 'apps' / app_id


def launch_plan(app, data, rom, env, prepare=True):
    """emulators.launch_plan for an app: its program, in the environment docs/APPS.md promises."""
    folder = Path(rom).parent
    fields = manifest(folder)
    problems = blockers(folder, app)
    if problems: raise ValueError(problems[0])
    program = _inside(folder, fields['exec'])
    own = data_folder(data, fields['id'])
    if prepare:
        for name in ('', 'config', 'share', 'cache'):
            (own / name).mkdir(parents=True, exist_ok=True)
    from portmaster import Folders, pad, panel_size
    from linux_platform import rgds_os
    width, height = panel_size()
    env.update(
        HOME=str(own),
        XDG_CONFIG_HOME=str(own / 'config'),
        XDG_DATA_HOME=str(own / 'share'),
        XDG_CACHE_HOME=str(own / 'cache'),
        RGDS_APP=str(app),
        RGDS_APP_ID=fields['id'],
        RGDS_APP_DIR=str(folder),
        RGDS_APP_DATA=str(own),
        RGDS_PANEL_WIDTH=str(width),
        RGDS_PANEL_HEIGHT=str(height),
        RGDS_CORE_VERSION=core_version(app),
        SDL_AUDIODRIVER='alsa',
        SDL_GAMECONTROLLERCONFIG=pad(),
        SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS='1',
        PYTHONDONTWRITEBYTECODE='1'
    )
    if rgds_os():
        # The top panel's DRM lease (port_runner.PortMenu.port_leases) is the only display the app's SDL sees.
        env['SDL_VIDEODRIVER'] = 'kmsdrm'
        for key in ('SDL_VIDEO_WAYLAND_WMCLASS', 'WAYLAND_DISPLAY', 'SDL_KMSDRM_FIRST'):
            env.pop(key, None)
    else:
        env.update(SDL_VIDEODRIVER='wayland', SDL_VIDEO_WAYLAND_WMCLASS='org.rgds.top')
    for name, key in (('bin', 'PATH'), ('lib', 'LD_LIBRARY_PATH')):
        if (folder / name).is_dir():
            env[key] = ':'.join(
                p for p in (str(folder / name), env.get(key, '/usr/bin:/bin' if key == 'PATH' else '')) if p
            )
    if fields['arch'] == 'armhf' or fields['gl'] == 'gl4es':
        # PortMaster's compat layer (runtime/portmaster_compat.py) has the 32-bit loader and gl4es: an app may use it.
        import portmaster_compat
        compat_root = Folders().root
        if not portmaster_compat.current(compat_root):
            raise ValueError('This app needs the PortMaster libraries. Open PortMaster in Ports once, then try again.')
        if fields['arch'] == 'armhf':
            if prepare: portmaster_compat.prepare_armhf(compat_root, folder)
            portmaster_compat.armhf_environment(env, compat_root)
        if fields['gl'] == 'gl4es': portmaster_compat.gl_environment(env, compat_root)
    command = (['bash'] if program.suffix == '.sh' else []) + [str(program)] + shlex.split(fields.get('args', ''))
    return {'command': command, 'cwd': str(folder), 'env': env, 'port': fields['name']}


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('action', choices=('check', 'install', 'list', 'artwork'))
    parser.add_argument('path', nargs='?', type=Path)
    parser.add_argument('--app', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--data', type=Path, help="RG DS Core's userdata folder (artwork)")
    parser.add_argument('--root', type=Path, help='the apps folder (default: /data/apps on RG DS OS)')
    args = parser.parse_args()
    apps_root = args.root or root()
    if args.action == 'check':
        if args.path is None: parser.error('check needs the app folder')
        problems = check(args.path, args.app)
        print('\n'.join(problems) if problems else '%s is ready.' % manifest(args.path).get('name', args.path.name))
        return 1 if problems else 0
    if args.action == 'install':
        if args.path is None: parser.error('install needs the zip')
        print('Installed ' + install(args.path, apps_root, args.app))
        return 0
    if args.action == 'list':
        print(json.dumps(installed(apps_root), indent=1))
        return 0
    if args.data is None: parser.error('artwork needs --data')
    done = install_pending(apps_root, args.app)
    print('Apps: %d installed, %d covers made' % (len(done), artwork(args.data, apps_root)))
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, zipfile.BadZipFile) as error:
        print('Apps: ' + str(error), file=sys.stderr)
        sys.exit(1)
