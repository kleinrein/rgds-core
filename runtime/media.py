"""RG DS Media: films and books (play/src/media, docs/MEDIA_PLAN.md), started from Home's Media tile.

The launcher hands over both panels the way it does for a game (the display-handoff socket: 'R' when this side is
ready, 'C' once the launcher has let go), then rgds-media runs until Back or Home leaves it, and the launcher
comes back. Nothing of a game's runner is needed: no saves, no playtime, no shared menu. The volume keys keep
working (audio.VolumeWatch) and rgds-media shows their level itself.

rgds-media's touch sliders (a film's Volume and brightness popup) ask for a level through a small file
(RGDS_MEDIA_CONTROL: volume=N, brightness=N); MediaControls applies it the way the launcher's Settings do (audio.Volume,
system_controls.Hardware), keeps brightness in system.ini so the launcher finds it on return, and publishes what is
set in RGDS_MEDIA_LEVELS for the sliders to start from.

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
import threading
import time

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
    env['RGDS_MEDIA_CONTROL'] = str(data / 'session/media-control.ini')
    env['RGDS_MEDIA_LEVELS'] = str(data / 'session/media-levels.ini')
    return {'command': command, 'env': env, 'folders': {k: str(v) for k, v in folders.items()}, 'state': str(state)}


def save_brightness(system_ini, percent):
    """The brightness line of the launcher's system.ini (the rest kept as it is), written atomically."""
    try:
        lines = Path(system_ini).read_text().splitlines()
    except OSError:
        return False
    out, found = [], False
    for line in lines:
        if line.startswith('brightness='):
            line, found = f'brightness={percent}', True
        out.append(line)
    if not found:
        return False  # not a system.ini the launcher wrote: leave it alone
    pending = Path(str(system_ini) + '.media')
    pending.write_text('\n'.join(out) + '\n')
    os.replace(pending, system_ini)
    return True


class MediaControls:
    """Levels asked for by rgds-media's sliders: polled every 40 ms while it runs."""
    def __init__(self, data, volume):
        self.data = Path(data)
        self.volume = volume  # audio.Volume, shared with the volume keys
        self.request = self.data / 'session/media-control.ini'
        self.levels = self.data / 'session/media-levels.ini'
        self.system_ini = self.data / 'system.ini'
        self.stop = threading.Event()
        self.thread = None
        self.brightness = None
        self.brightness_saved = None
        self.brightness_at = 0.0

    def hardware(self):
        from system_controls import Hardware
        return Hardware(self.data)

    def publish(self):
        from fields import write_fields
        fields = dict(schema=1, volume=self.volume.status())
        if self.brightness is not None:
            fields['brightness'] = self.brightness
        write_fields(self.levels, fields, durable=False)

    def start(self):
        self.request.unlink(missing_ok=True)
        try:
            self.brightness = self.hardware().status().get('brightness')
        except (OSError, ValueError, subprocess.SubprocessError):
            self.brightness = None
        try:
            self.publish()
        except (OSError, ValueError, subprocess.SubprocessError) as error:
            print(f'Media controls: {error}', file=sys.stderr, flush=True)
        self.thread = threading.Thread(target=self.watch, name='rgds-media-controls', daemon=True)
        self.thread.start()

    def step(self):
        from fields import read_fields
        try:
            fields = read_fields(self.request) if self.request.exists() else {}
            if fields:
                self.request.unlink(missing_ok=True)
            if 'volume' in fields:
                self.volume.apply(value=max(0, min(100, int(fields['volume']))))
            if 'brightness' in fields and self.brightness is not None:
                self.brightness = max(5, min(100, int(fields['brightness'])))
                self.hardware().brightness(self.brightness)
                self.brightness_at = time.monotonic()
            if fields:
                self.publish()
        except (OSError, ValueError, subprocess.SubprocessError) as error:
            print(f'Media controls: {error}', file=sys.stderr, flush=True)
        # Saved once the slider rests, not on every step of a drag.
        if self.brightness is not None and self.brightness != self.brightness_saved and \
                time.monotonic() - self.brightness_at > .6:
            try:
                save_brightness(self.system_ini, self.brightness)
            except OSError as error:
                print(f'Media controls: {error}', file=sys.stderr, flush=True)
            self.brightness_saved = self.brightness

    def watch(self):
        self.brightness_saved = self.brightness
        while not self.stop.wait(.04):
            self.step()

    def close(self):
        self.stop.set()
        if self.thread:
            self.thread.join(timeout=2)
        self.step()  # a level asked for in the last moment, and an unsaved brightness


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
    controls = MediaControls(data, volume.volume)
    child = None

    def stop(_signum, _frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, stop)
    try:
        volume.start()
        controls.start()
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
        controls.close()
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
