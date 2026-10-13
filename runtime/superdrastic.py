"""Optional pinned SuperDrastic display backend for stock RG DS DraStic.

The native backend owns DRM; RGDS supplies its shared pause menu. This supervisor owns the display
lease, RGDS controls/hardware, and checked checkpoint promotion. No shell runner,
service termination, firmware changes, downloads, or silent fallback are used.
"""
import array
import ctypes
import errno
import fcntl
import hashlib
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time

from game_menu import GameMenu
from controls import JOYSTICK, LOGICAL, load_profile
from emulator_profiles import drastic_home, drastic_native_overrides, game_options, policy
from fields import read_fields, write_fields
from integration import engine_libraries
from library import game_key
from lid_sleep import LidSleep
from system_controls import Hardware

DRASTIC_SHA256 = 'dab3275ef772451b9235b78e64b2d0d041a28ac22937668231088c76b709e218'
MAX_STATE = 64 * 1024 * 1024


def check_sdl(path):
    """Check the private runtime before touching profiles or acquiring the VT."""
    try:
        library = ctypes.CDLL(str(path), mode=ctypes.RTLD_LOCAL)
        for kind, required in (('Video', b'dummy'), ('Audio', b'alsa')):
            count = getattr(library, 'SDL_GetNum' + kind + 'Drivers')
            count.argtypes = []; count.restype = ctypes.c_int
            driver = getattr(library, 'SDL_Get' + kind + 'Driver')
            driver.argtypes = [ctypes.c_int]; driver.restype = ctypes.c_char_p
            if required not in {driver(i) for i in range(count())}:
                raise ValueError('SuperDrastic SDL runtime lacks the ' + required.decode() + ' driver.')
    except (OSError, AttributeError) as error:
        raise ValueError('SuperDrastic SDL runtime cannot load; reinstall the complete RGDS package.') from error


def backend_choice(data, rom):
    global_choice = read_fields(Path(data) / 'ui.ini').get('drastic_backend', '0')
    local = read_fields(Path(data) / 'library' / game_key(rom) / 'game.ini').get('drastic_backend', '-1')
    return 'superdrastic' if (local if local in ('0', '1') else global_choice) == '1' else 'standard'


def fingerprint(path):
    path = Path(path)
    if not path.is_file() or path.is_symlink() or not 0 < path.stat().st_size <= MAX_STATE:
        return None
    with path.open('rb') as stream:
        digest = hashlib.sha256()
        for chunk in iter(lambda: stream.read(256 * 1024), b''): digest.update(chunk)
        return digest.hexdigest()


def copy_state(source, target):
    """Publish only a complete checkpoint, leaving an existing one intact on error."""
    digest = fingerprint(source)
    if digest is None:
        raise ValueError('SuperDrastic did not produce a readable checkpoint; previous save preserved.')
    target.parent.mkdir(parents=True, exist_ok=True)
    fd, pending = tempfile.mkstemp(prefix='.checkpoint-', dir=target.parent)
    try:
        with os.fdopen(fd, 'wb') as out, Path(source).open('rb') as incoming:
            shutil.copyfileobj(incoming, out, 256 * 1024)
            out.flush(); os.fsync(out.fileno())
        if fingerprint(pending) != digest:
            raise ValueError('Checkpoint changed during copy; previous save preserved.')
        os.replace(pending, target)
        # Match ownership/release records: file contents are mandatory-flushed,
        # while directory metadata is best effort on filesystems such as FAT.
        try:
            descriptor = os.open(str(target.parent), os.O_RDONLY)
            try: os.fsync(descriptor)
            finally: os.close(descriptor)
        except OSError:
            pass
    finally:
        Path(pending).unlink(missing_ok=True)


def prepare_profile(app, data, binary, rom, identity):
    home = drastic_home(app, data, binary.parent, mapped_controls=False)
    path = home / 'config/drastic.cfg'
    fields = {k.strip(): v.strip() for k, v in read_fields(path).items()}
    tuning = game_options(data, identity)
    fields.update(threaded_3d=tuning['threaded'], hires_3d=tuning['hires'],
                  frameskip_type=int(tuning['frameskip'] > 0), frameskip_value=max(1, tuning['frameskip']),
                  fast_forward=0, savestate_number=1, screen_wait_for_vsync=0)
    profile = load_profile(data, identity)
    codes = {source: 1024 + number for number, source in JOYSTICK.items()}
    codes.update(up=1089, right=1090, down=1092, left=1096, none=65535)
    for group in ('a', 'b'):
        # Reset old native hotkeys so a previous backend cannot leave duplicates.
        for key in list(fields):
            if key.startswith('controls_' + group + '['): fields[key] = 65535
        for logical in LOGICAL:
            fields[f'controls_{group}[CONTROL_INDEX_{logical.upper()}]'] = codes[profile['bind_' + logical]]
        for name, value in dict(SAVE_STATE=1040, LOAD_STATE=1041, QUIT=1042, FAST_FORWARD=1043, MENU=65535).items():
            fields[f'controls_{group}[CONTROL_INDEX_{name}]'] = value
    # SuperDrastic's control-binding reader requires spaces around '='.
    write_fields(path, {k + ' ': ' ' + str(v) for k, v in fields.items()})
    drastic_native_overrides(home, rom, fields, spaced=True)
    return home


def launch_plan(app, data, binary, rom, env, prepare=True, identity=None):
    identity = identity or rom
    library = app / 'emulators/superdrastic/libsuperdrastic.so'
    sdl = library.parent / 'sdl/libSDL2-2.0.so.0'
    home = data / 'emulators/drastic'
    if not library.is_file() or library.is_symlink():
        raise ValueError('SuperDrastic is missing. Install a complete RGDS package or choose Standard DraStic.')
    if prepare:
        from linux_platform import independent
        if independent():
            raise ValueError('This experimental SuperDrastic profile currently targets Anbernic Linux on standard RG DS.')
        if fingerprint(binary) != DRASTIC_SHA256:
            raise ValueError('SuperDrastic needs the verified RG DS ARM64 DraStic binary. Choose Standard for another build.')
        if not sdl.is_file() or sdl.is_symlink():
            raise ValueError('SuperDrastic private SDL runtime is missing; reinstall the complete RGDS package.')
        check_sdl(sdl)
        home = prepare_profile(app, data, binary, rom, identity)
    session = data / 'session' / ('superdrastic-' + str(os.getpid()) + '-' + str(time.monotonic_ns()))
    auto = data / 'library' / game_key(identity) / 'auto-drastic.dss'
    resume = home / 'savestates' / ('.rgds-resume-' + session.name + '.dss')
    auto_load, auto_save = policy(data)
    load = False
    if prepare:
        session.mkdir(parents=True)
        (data / 'emulators/superdrastic').mkdir(parents=True, exist_ok=True)
        (data / 'logs').mkdir(parents=True, exist_ok=True)
        # Don't let an old checkpoint roll back a newer native in-game save.
        battery = home / 'backup' / (rom.stem + '.dsv')
        if auto_load and fingerprint(auto) and (not battery.exists() or battery.stat().st_mtime_ns <= auto.stat().st_mtime_ns):
            copy_state(auto, resume); load = True
    env['RGDS_SDL_LIBRARY'] = str(sdl)
    tuning = game_options(data, identity)
    gengis_scale = tuning['gengis_scale'] if tuning['hires'] else 0
    engine_libraries(env, private=(sdl.parent, binary.parent / 'libs'), stock_independent=False)
    env.update(HOME=str(home), SDL_VIDEODRIVER='dummy', SDL_RENDER_DRIVER='software',
               LD_PRELOAD=str(library), RGDS_CORE='drastic', DSFLIP_CPUGOV='0',
               DSFLIP_RAST=str(int(gengis_scale > 0)), DSFLIP_RAST_SCALE=str(gengis_scale or 2),
               DSFLIP_RAST_TEXFILTER='0',
               DSFLIP_EXIT_COMBO='0', DSFLIP_MENU='0',
               RGDS_SD_SHARED_MENU='1', RGDS_MENU_LIVE='1',
               RGDS_CONTROL_FILE=str(data / 'session/emulator.control'),
               RGDS_OVERLAY_FILE=env.get('RGDS_OVERLAY_FILE') or str(session / 'overlay.frame'), DSFLIP_TOP='DSI-2',
               DSFLIP_SHADER='none', DSFLIP_FONT=str(app / 'assets/fonts/source/Nunito.ttf'),
               DSFLIP_MENU_FONT=str(app / 'assets/fonts/source/Nunito.ttf'), DSFLIP_SHADER_DIR=str(library.parent / 'shaders'),
               DSFLIP_DATA=str(data / 'emulators/superdrastic'),
               DSFLIP_LOG=str(data / 'logs/superdrastic.log'), DSFLIP_STATE=str(session / 'ready'),
               DSFLIP_RESUME_FILE=str(resume) if auto_save or load else '', DSFLIP_RESUME_LOAD=str(int(load)),
               RGDS_SD_CHECKPOINT=str(auto),
               RGDS_SD_RESUME=str(resume), RGDS_SD_SESSION=str(session))
    for key, name in dict(COMMAND='command', RESULT='result', MENU_STATUS='menu', EXIT='exit', SETTINGS='settings', SYSTEM='system').items():
        env['RGDS_SD_' + key] = str(session / name)
    if any(len(env[key].encode()) > 470 for key in ('DSFLIP_RESUME_FILE', 'RGDS_SD_SESSION')):
        raise ValueError('SuperDrastic data path is too long; use a shorter RGDS data directory.')
    # Autoload and autosave are independent. The native adapter checks this flag.
    env['RGDS_SD_SAVE_ON_EXIT'] = str(int(auto_save))
    return dict(command=[str(binary), str(rom)], cwd=str(home), env=env, backend='superdrastic')


class VTLease:
    """Release Weston through Linux VT switching and restore it on every exit."""
    def __init__(self):
        self.fd = self.tty = None
        self.original = self.allocated = self.mode = None
        self.switched = False
        self.blank = None

    def active(self):
        state = array.array('H', [0, 0, 0]); fcntl.ioctl(self.fd, 0x5603, state)
        return state[0]

    def switch(self, number):
        fcntl.ioctl(self.fd, 0x5606, number)  # VT_ACTIVATE; no unbounded VT_WAITACTIVE.
        deadline = time.monotonic() + 3
        while self.active() != number:
            if time.monotonic() > deadline: raise OSError('Display console did not switch within three seconds')
            time.sleep(.02)

    def acquire(self):
        try:
            self.fd = os.open('/dev/tty0', os.O_RDWR | os.O_CLOEXEC)
            self.original = self.active()
            free = array.array('i', [0]); fcntl.ioctl(self.fd, 0x5600, free)
            if free[0] <= 0 or free[0] == self.original: raise OSError('No free display console for SuperDrastic')
            self.allocated = free[0]
            self.tty = os.open('/dev/tty' + str(self.allocated), os.O_RDWR | os.O_CLOEXEC)
            mode = array.array('i', [0]); fcntl.ioctl(self.tty, 0x4b3b, mode); self.mode = mode[0]
            fcntl.ioctl(self.tty, 0x4b3a, 0)  # KD_TEXT, as required by this KMS backend.
            blank = Path('/sys/class/graphics/fb0/blank')
            if blank.exists(): self.blank = (blank, blank.read_text().strip())
            self.switched = True; self.switch(self.allocated)
            if self.blank: blank.write_text('0')
        except BaseException:
            self.close(); raise

    def close(self):
        if self.fd is None: return
        try:
            if self.switched:
                self.switch(self.original); self.switched = False
            if self.blank and self.blank[1] != '0': self.blank[0].write_text(self.blank[1])
        finally:
            if self.tty is not None:
                try:
                    if self.mode is not None: fcntl.ioctl(self.tty, 0x4b3a, self.mode)
                finally: os.close(self.tty); self.tty = None
            if self.fd is not None:
                try:
                    if self.allocated and not self.switched:
                        try: fcntl.ioctl(self.fd, 0x5608, self.allocated)
                        except OSError as error:
                            # Stock may still hold an inactive VT open. The
                            # original display has already been acknowledged.
                            if error.errno != errno.EBUSY: raise
                finally: os.close(self.fd); self.fd = None


class NativeSupervisor:
    restart = False

    def __init__(self, child, app, data, core, rom, env, inputs, hardware=None):
        self.child, self.data, self.env, self.inputs = child, Path(data), env, inputs
        self.hardware = hardware or Hardware(data)
        self.lid = LidSleep(data, self.hardware)
        self.began = self.last_tick = time.monotonic()
        self.played = 0.0; self.ready = False; self.menu_open = False
        self.queue = []; self.pending = None; self.counter = 0; self.exiting = None
        self.settings = {}; self.hardware_at = float('-inf')
        self.update_hardware()

    def path(self, name): return Path(self.env['RGDS_SD_' + name])

    def played_seconds(self): return int(self.played)

    def update_hardware(self):
        try: self.poll_hardware()
        except (OSError, ValueError, subprocess.SubprocessError) as error:
            # Losing a mixer/backlight is not permission to kill a running game.
            self.settings = read_fields(self.path('SETTINGS'))
            self.hardware_at = time.monotonic()
            print('SuperDrastic hardware control: ' + str(error), file=sys.stderr)

    def poll_hardware(self):
        now = time.monotonic()
        requested = read_fields(self.path('SETTINGS'))
        if requested and requested != self.settings:
            for name in ('volume', 'brightness'):
                try: value = int(requested.get(name, '-1'))
                except ValueError: continue
                if value < (5 if name == 'brightness' else 0) or value > 100 or requested.get(name) == self.settings.get(name): continue
                getattr(self.hardware, name)(value)
                if name == 'brightness':
                    prefs = self.data / 'system.ini'; fields = read_fields(prefs)
                    if fields.get('schema') == '1': fields['brightness'] = value; write_fields(prefs, fields)
            self.settings = requested; self.hardware_at = float('-inf')
        if now - self.hardware_at >= 2:
            status = self.hardware.status()
            write_fields(self.path('SYSTEM'), {'audio.volume': status['volume'], 'display.brightness': status['brightness'],
                         'battery': status['battery'], 'charging': status['charging']}, durable=False)
            self.hardware_at = now

    def request(self, action):
        if action == 'home':
            if self.exiting is None:
                self.child.send_signal(signal.SIGUSR1); self.exiting = time.monotonic()
            return
        if self.exiting is not None: return
        if action == 'menu': self.queue.extend(('speed-off', 'menu'))
        elif action.startswith('fast-forward-'): self.queue.append(action.replace('fast-forward-', 'speed-'))
        if len(self.queue) > 32: raise ValueError('SuperDrastic input queue stopped responding')

    def commands(self):
        now = time.monotonic()
        if self.pending:
            token, since = self.pending
            if read_fields(self.path('RESULT')).get('token') == token: self.pending = None
            elif now - since > 3: raise ValueError('SuperDrastic native menu stopped responding')
        if not self.pending and self.queue:
            action = self.queue.pop(0); self.counter += 1
            token = str(self.counter)
            # The native reader wants a two-word command, not a field record.
            path = self.path('COMMAND'); temporary = path.with_suffix('.pending')
            temporary.write_text(token + ' ' + action + '\n'); temporary.replace(path)
            self.pending = (token, now)

    def poll(self):
        now = time.monotonic()
        if self.ready and not self.menu_open and self.exiting is None: self.played += now - self.last_tick
        self.last_tick = now
        if self.child.poll() is not None: return
        if not self.ready:
            try: verdict = Path(self.env['DSFLIP_STATE']).read_text().strip()
            except OSError: verdict = ''
            if verdict == 'ready': self.ready = True
            elif verdict or now - self.began > 12:
                raise ValueError('SuperDrastic could not start its display: ' + (verdict or 'readiness timeout') + '. Choose Standard DraStic.')
        self.update_hardware()
        if not self.ready: return
        self.menu_open = read_fields(self.path('MENU_STATUS')).get('open') == '1'
        action = self.inputs.poll()
        if action: self.request(action if isinstance(action, str) else 'menu')
        self.commands()
        if self.exiting is not None and now - self.exiting > 8:
            raise ValueError('SuperDrastic exit timed out; the previous checkpoint was preserved.')
        if self.lid.poll():
            self.child.send_signal(signal.SIGSTOP)
            try: self.hardware.sleep_lid()
            except (OSError, ValueError, subprocess.SubprocessError) as error:
                print('SuperDrastic lid sleep failed: ' + str(error), file=sys.stderr)
            finally:
                self.inputs.discard_after_sleep()
                if self.child.poll() is None: self.child.send_signal(signal.SIGCONT)
                self.queue.append('speed-off'); self.last_tick = time.monotonic()

    def finish(self, result):
        receipt = read_fields(self.path('EXIT')).get('state')
        if result != -signal.SIGKILL or receipt not in ('saved', 'no-save'):
            if receipt == 'save-failed': raise ValueError('SuperDrastic checkpoint failed; previous checkpoint retained.')
            if result == 0 and self.exiting is not None and self.env.get('RGDS_SD_SAVE_ON_EXIT') == '1':
                raise ValueError('SuperDrastic exited without confirming the requested checkpoint; previous checkpoint retained.')
            return result or (1 if not self.ready else 0)
        if receipt == 'saved':
            resume, auto = self.path('RESUME'), self.path('CHECKPOINT')
            if not fingerprint(resume): raise ValueError('SuperDrastic reported a save but its checkpoint is missing.')
            previous = fingerprint(auto)
            if previous:
                copy_state(auto, auto.parent / 'checkpoint-backups' / ('drastic-' + previous[:16] + '.dss'))
            copy_state(resume, auto)
        return 0

    def close(self):
        if self.inputs: self.inputs.close(); self.inputs = None
        # Keep receipts and failed saves for diagnosis; remove only consumed
        # staging checkpoints after a confirmed native exit.
        if self.child.poll() is not None and read_fields(self.path('EXIT')).get('state') == 'saved':
            if fingerprint(self.path('RESUME')) == fingerprint(self.path('CHECKPOINT')):
                self.path('RESUME').unlink(missing_ok=True)


class Supervisor(GameMenu):
    """Our shared menu, with verified DraStic save promotion and a live DRM canvas."""
    def __init__(self, child, app, data, core, rom, env, inputs, hardware=None):
        super().__init__(child, app, data, core, rom, env, inputs)
        self.env = env
        self.display_ready = False
        self.exiting = None
        self.lid = LidSleep(data, hardware or Hardware(data))
        self.fps = False  # This backend has no in-game FPS overlay yet.

    path = NativeSupervisor.path
    def switch_checkpoint_saved(self):
        return (self.switch_target is not None and read_fields(self.path('EXIT')).get('state') == 'saved' and self.has_checkpoint()) or super().switch_checkpoint_saved()

    def finish(self, result):
        # The native receipt deliberately uses SIGKILL after the save hook closes.
        receipt = read_fields(self.path('EXIT')).get('state')
        if result == -signal.SIGKILL and receipt in ('saved', 'no-save'):
            if receipt == 'saved':
                resume, auto = self.path('RESUME'), self.path('CHECKPOINT')
                if not fingerprint(resume): raise ValueError('SuperDrastic reported a save but its checkpoint is missing.')
                previous = fingerprint(auto)
                if previous: copy_state(auto, auto.parent / 'checkpoint-backups' / ('drastic-' + previous[:16] + '.dss'))
                copy_state(resume, auto)
            return 0
        if receipt == 'save-failed' or (result == 0 and self.exiting is not None and self.env.get('RGDS_SD_SAVE_ON_EXIT') == '1'):
            raise ValueError('SuperDrastic checkpoint failed; previous checkpoint retained.')
        return result or (0 if self.display_ready else 1)

    def open(self):
        if self.menu is not None or self.snapshot_deadline is not None or self.child.poll() is not None:
            return
        self.stop_speed_for_menu()
        (self.folder / 'menu-background.bmp').unlink(missing_ok=True)
        self.block.touch()
        self.snapshot_deadline = time.monotonic() + .18

    def poll_snapshot(self):
        if self.snapshot_deadline is None:
            return
        if not (self.folder / 'menu-background.bmp').exists() and time.monotonic() < self.snapshot_deadline:
            return
        self.snapshot_deadline = None
        self.open_ready()

    def send(self, action):
        if action.startswith('fps:'):
            self.publish('error', 'The FPS counter is unavailable with SuperDrastic.')
            return
        if action.startswith('shader:'):
            self.publish('error', 'SuperDrastic currently uses native output.')
            return
        if action in ('quit', 'save-quit'):
            self.exiting = time.monotonic()
        super().send(action)

    def poll(self):
        if self.child.poll() is not None:
            return
        if not self.display_ready:
            try: verdict = Path(self.env['DSFLIP_STATE']).read_text().strip()
            except OSError: verdict = ''
            if verdict == 'ready': self.display_ready = True
            elif verdict or time.monotonic() - self.began > 12:
                raise ValueError('SuperDrastic could not start its display: ' + (verdict or 'readiness timeout'))
            if not self.display_ready: return
        if self.pending:
            path = Path(str(self.control) + '.result')
            result = read_fields(path)
            if result.get('token') == self.pending['token'] and result.get('state') == 'native-saved':
                try:
                    copy_state(self.path('RESUME'), self.path('CHECKPOINT'))
                    result['state'] = 'saved'
                except (OSError, ValueError) as error:
                    result['state'] = 'save-failed'
                    print('SuperDrastic checkpoint: ' + str(error), file=sys.stderr)
                write_fields(path, result, durable=False)
        super().poll()

    def close(self):
        try: super().close()
        finally:
            if self.child.poll() is not None and read_fields(self.path('EXIT')).get('state') == 'saved':
                if fingerprint(self.path('RESUME')) == fingerprint(self.path('CHECKPOINT')):
                    self.path('RESUME').unlink(missing_ok=True)
