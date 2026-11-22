"""RG DS Media: films and books (play/src/media, docs/MEDIA_PLAN.md), started from Home's Media tile.

The launcher hands over both panels the way it does for a game (the display-handoff socket: 'R' when this side is
ready, 'C' once the launcher has let go), then rgds-media runs until Back or Home leaves it, and the launcher
comes back. Nothing of a game's runner is needed: no saves, no playtime, no shared menu. The volume keys keep
working (audio.VolumeWatch) and rgds-media shows their level itself.

    python3 runtime/media.py launch --app APP --data USERDATA [--open FILE] [--display-handoff-fd N]
    python3 runtime/media.py plan --app APP --data USERDATA      the command and folders, as JSON
"""
import argparse
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys

sys.dont_write_bytecode = True

BINARY = 'emulators/rgds-media/rgds-media'
SYSTEM_BINARY = Path('/usr/bin/rgds-media')


def roots():
    """The videos, books and music folders: beside the games on the data partition."""
    from linux_platform import rgds_os
    base = Path('/data') if rgds_os() else Path('/mnt/mmc')
    return {'videos': base / 'videos', 'books': base / 'books', 'music': base / 'music'}


def find_binary(app):
    for candidate in (Path(app) / BINARY, SYSTEM_BINARY):
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    return None


def plan(app, data, open_file=None):
    app, data = Path(app), Path(data)
    binary = find_binary(app)
    if binary is None:
        raise OSError('RG DS Media is not installed')
    folders = roots()
    # The launcher passes the profile's own folder and look (a Kid profile keeps its own places).
    state = Path(os.environ.get('RGDS_MEDIA_STATE') or data / 'media')
    preferences = os.environ.get('RGDS_MEDIA_PREFERENCES') or str(data / 'preferences.ini')
    command = [
        str(binary), '--videos',
        str(folders['videos']), '--books',
        str(folders['books']), '--state',
        str(state), '--music',
        str(folders['music']), '--fonts',
        str(app / 'assets/fonts'), '--locales',
        str(app / 'assets/locales'), '--preferences', preferences
    ]
    if os.environ.get('RGDS_MEDIA_MUSIC_TAB') == '1':
        command.append('--music-tab')
    if open_file:
        command += ['--open', str(open_file)]
    env = dict(os.environ)
    from linux_platform import rgds_os
    if rgds_os():
        # No compositor on RG DS OS: rgds-media opens both panels on KMS, the top one first (the session's
        # SDL_KMSDRM_FIRST), and reads the touch panels itself.
        env['SDL_VIDEODRIVER'] = 'KMSDRM'
        try:
            from input_devices import discover
            found = discover()
            if found.get('touch'):
                env['RGDS_TOUCH_DEV'] = found['touch']
        except (OSError, ValueError):
            pass
    env['RGDS_MEDIA_VOLUME_HUD'] = str(data / 'session/volume-hud.ini')
    return {'command': command, 'env': env, 'folders': {k: str(v) for k, v in folders.items()}, 'state': str(state)}


def release_launcher_display(channel):
    if channel is None:
        return
    channel.settimeout(10)
    channel.sendall(b'R')
    if channel.recv(1) != b'C':
        raise OSError('Launcher closed before display handoff')


def launch(app, data, open_file=None, channel=None):
    p = plan(app, data, open_file)
    for folder in p['folders'].values():
        try:
            Path(folder).mkdir(parents=True, exist_ok=True)
        except OSError:
            pass  # a read-only card: the shelves say they are empty
    Path(p['state']).mkdir(parents=True, exist_ok=True)
    from audio import VolumeWatch
    volume = VolumeWatch(Path(data))
    child = None

    def stop(_signum, _frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, stop)
    try:
        volume.start()
        release_launcher_display(channel)
        log = open(Path(data) / 'logs/rgds-media.log', 'w') if (Path(data) / 'logs').is_dir() else None
        child = subprocess.Popen(p['command'], env=p['env'], stdout=log, stderr=log or None)
        return child.wait()
    except KeyboardInterrupt:
        if child and child.poll() is None:
            child.terminate()
            try:
                child.wait(5)
            except subprocess.TimeoutExpired:
                child.kill()
        return 0
    finally:
        volume.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('launch', 'plan'))
    parser.add_argument('--app', type=Path, required=True)
    parser.add_argument('--data', type=Path, required=True)
    parser.add_argument('--open', type=Path)
    parser.add_argument('--display-handoff-fd', type=int, default=-1, help=argparse.SUPPRESS)
    args = parser.parse_args()
    try:
        if args.action == 'plan':
            p = plan(args.app.resolve(), args.data.resolve(), args.open)
            print(json.dumps({'command': p['command'], 'folders': p['folders']}, indent=2))
            return 0
        if args.display_handoff_fd >= 0:
            with socket.socket(fileno=args.display_handoff_fd) as channel:
                return launch(args.app.resolve(), args.data.resolve(), args.open, channel)
        return launch(args.app.resolve(), args.data.resolve(), args.open)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print('RG DS Media: ' + str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
