"""The in-game menu for a PortMaster port (docs/PORTMASTER_PLAN.md, sections 7 and 8).

A port is a script with a tree of processes (bash, the game, gptokeyb), not one
engine with our bridge inside it, so the menu works from outside:

* The tree runs in its own cgroup2 leaf and a private mount namespace: the menu
  freezes and thaws the whole tree at once, quitting ends every process it
  started, and runtime squashfs mounts disappear with it.
* The stock Weston (desktop-shell) gives keyboard focus to every new window and
  never gives it back, and gptokeyb's keys follow that focus. So the menu never
  opens a window over the port: the menu helper (rgds-launcher --port-menu) maps
  its windows before the port starts and stays for the whole session. The port
  covers its top window; its bottom window is the info screen RG DS Play shows
  beside its games (PortMaster itself: its install progress) and turns into the
  menu on Home, or the game switcher on the Anbernic button, as for every game.
* Presses made in the menu queue up in the frozen port's input devices. Before
  thawing, the runner borrows the port's evdev descriptors (pidfd_getfd) and
  drains them, and the keyboard-like key device is grabbed while the menu is
  open so Weston queues nothing for the port either.
* RG DS OS has no compositor: the runner leases the top panel to the port and the
  bottom one to the helper (drm_lease.PanelLeases), so both draw on KMS at once.
"""
import ctypes
import ctypes.util
import fcntl
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import time

from audio import clean_environment
from fields import write_fields
from game_menu import GameMenu
from shared_audio import mixer_holder

CGROUPS = Path('/run/rgds-cgroup')
LEAF = CGROUPS / 'rgds-port'
EVIOCGRAB = 0x40044590
EVIOCGBIT_KEY = 0x80604521  # EVIOCGBIT(EV_KEY, 96)
PIDFD_OPEN, PIDFD_GETFD = 434, 438
_libc = ctypes.CDLL(ctypes.util.find_library('c') or None, use_errno=True)


def _bit(bits, code):
    return bool(bits[code // 8] & (1 << (code % 8)))


def mount_cgroups():
    """cgroup2 at /run/rgds-cgroup (the stock card mounts no cgroup filesystem)."""
    if (CGROUPS / 'cgroup.procs').is_file(): return
    CGROUPS.mkdir(parents=True, exist_ok=True)
    subprocess.run(['mount', '-t', 'cgroup2', 'none', str(CGROUPS)], check=True, timeout=10)


def members():
    try:
        return [int(p) for p in (LEAF / 'cgroup.procs').read_text().split()]
    except (OSError, ValueError):
        return []


def kill_leaf(timeout=3.0):
    """End every process in the leaf: SIGKILL through cgroup.kill, then wait until it is empty."""
    if not LEAF.is_dir(): return True
    try:
        (LEAF / 'cgroup.freeze').write_text('0')
        (LEAF / 'cgroup.kill').write_text('1')
    except OSError:
        for pid in members():
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
    deadline = time.monotonic() + timeout
    while members() and time.monotonic() < deadline:
        time.sleep(.05)
    return not members()


# (source, target) bind mounts made inside each port's namespace (prepare_binds); nothing outside it changes.
_BINDS = []


def prepare_binds(root=None):
    """Our PortMaster wherever a script might look for one. PortMaster's Weston runtime (westonwrap.sh) looks in
    $HOME/.local/share/PortMaster and otherwise falls back to /roms/ports/PortMaster, which on the stock card is
    the 2022 PortMaster on /mnt/ports, whose control.txt starts Anbernic's portsCtrl.dge."""
    from portmaster import Folders
    folders = Folders(root)
    targets = [folders.home / '.local/share/PortMaster', Path('/roms/ports/PortMaster')]
    del _BINDS[:]
    for target in targets:
        try:
            # A mount point is made only inside our own folder on the games card, never on the system's disk.
            if not target.exists() and folders.root in target.parents: target.mkdir(parents=True)
            if target.is_dir(): _BINDS.append((str(folders.control).encode(), str(target.resolve()).encode()))
        except OSError:
            pass
    return list(_BINDS)


def lease_shims(app):
    """librgds-drm-lease.so for LD_PRELOAD (tools/build_compat_stubs.py): $LIB is lib64 for RG DS OS's own loader and
    lib/arm-linux-gnueabihf for the armhf layer's, so each program loads its own build and no warning is printed."""
    return str(Path(app) / 'runtime/portmaster_files/kms/$LIB/librgds-drm-lease.so')


def enter_port():
    """preexec_fn of the port's bash: its own session, mount namespace and cgroup leaf."""
    os.setsid()
    if _libc.unshare(0x00020000) != 0:  # CLONE_NEWNS
        raise OSError(ctypes.get_errno(), 'unshare')
    if _libc.mount(None, b'/', None, 0x4000 | (1 << 18), None) != 0:  # MS_REC | MS_PRIVATE
        raise OSError(ctypes.get_errno(), 'private mounts')
    for source, target in _BINDS:
        _libc.mount(source, target, None, 0x1000, None)  # MS_BIND; a failure leaves that path as it was
    (LEAF / 'cgroup.procs').write_text(str(os.getpid()))


def drain_inputs(pids):
    """Read away what queued in the port's evdev descriptors while it was frozen."""
    drained = 0
    for pid in pids:
        try:
            descriptors = os.listdir('/proc/%d/fd' % pid)
        except OSError:
            continue
        pidfd = -1
        for name in descriptors:
            try:
                if not os.readlink('/proc/%d/fd/%s' % (pid, name)).startswith('/dev/input/event'): continue
            except OSError:
                continue
            if pidfd < 0:
                pidfd = _libc.syscall(PIDFD_OPEN, pid, 0)
                if pidfd < 0: break
            copy = _libc.syscall(PIDFD_GETFD, pidfd, int(name), 0)
            if copy < 0: continue
            try:
                # The copy shares the port's open file, so this empties the port's queue.
                for _ in range(64):
                    if not select.select([copy], [], [], 0)[0]: break
                    if not os.read(copy, 24 * 64): break
                    drained += 1
            except OSError:
                pass
            finally:
                os.close(copy)
        if pidfd >= 0: os.close(pidfd)
    return drained


class KeyGrab:
    """While the menu is open: the key devices Weston reads (the RG DS's arrow/A/Home keys) go to nobody."""
    def __init__(self):
        self.held = []

    def grab(self):
        if self.held: return
        for path in sorted(Path('/dev/input').glob('event*')):
            try:
                fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC)
            except OSError:
                continue
            bits = bytearray(96)
            try:
                fcntl.ioctl(fd, EVIOCGBIT_KEY, bits)
                # Arrow keys, but not a pad (BTN_SOUTH) or a touch panel (BTN_TOUCH).
                if _bit(bits, 103) and _bit(bits, 108) and not _bit(bits, 304) and not _bit(bits, 330):
                    fcntl.ioctl(fd, EVIOCGRAB, 1)
                    self.held.append(fd)
                    continue
            except OSError:
                pass
            os.close(fd)

    def release(self):
        for fd in self.held:
            try:
                fcntl.ioctl(fd, EVIOCGRAB, 0)
            except OSError:
                pass
            os.close(fd)
        self.held = []


class PortMenu(GameMenu):
    def __init__(self, child, app, data, core, rom, environment, inputs=None):
        super().__init__(child, app, data, core, rom, environment, inputs=inputs)
        self.companion = None
        self.leases = None  # RG DS OS: the panels' DRM leases (start_companion)
        self.state_file = self.folder / 'port-menu.state'
        self.port_status = self.folder / 'port-status.ini'
        self.state_token = 0
        self.keys = KeyGrab()
        self.user_stopped = False
        self.manager = self.rom.match('*/entry/PortMaster.sh')  # PortMaster itself, not a port
        for path in (self.state_file, self.port_status):
            path.unlink(missing_ok=True)
        mount_cgroups()
        if LEAF.is_dir():
            kill_leaf()
            LEAF.rmdir()
        LEAF.mkdir()

    # The helper ----------------------------------------------------------------------
    def companion_environment(self):
        env = clean_environment()
        for key in (
            "XDG_RUNTIME_DIR", "WAYLAND_DISPLAY", "RGDS_SDL_LIBRARY", "SDL_VIDEODRIVER", "SDL_RENDER_DRIVER",
            "SDL_RENDER_BATCHING", "TZ", 'RGDS_MENU_EXTERNAL', 'RGDS_INPUT_PROFILE', 'RGDS_SESSION_PLATFORM',
            'RGDS_TOP_DISPLAY', 'RGDS_BOTTOM_DISPLAY', 'RGDS_LID_DEV', 'RGDS_LID_WAKEUP', 'RGDS_TOUCH_DEV',
            'RGDS_INPUT_AMBIGUOUS', 'LD_LIBRARY_PATH'
        ):
            if key in self.environment:
                env[key] = self.environment[key]
        env.update(
            RGDS_SYSTEM_MANAGED="1",
            RGDS_VOLUME_EXTERNAL="1",
            RGDS_MENU_READY_FILE=str(self.ready),
            RGDS_TOUCH_GRAB="1",
            SDL_VIDEODRIVER="wayland"
        )
        if self.leases is not None:
            # The bottom panel is the only display the helper sees; it reads the port's picture for the touch pad
            # from the top panel's connector, which on RG DS OS is DSI-2.
            env.update(SDL_VIDEODRIVER='kmsdrm', RGDS_PORT_LEASE='1', RGDS_SCANOUT_CONNECTOR='DSI-2')
            env.pop('SDL_KMSDRM_FIRST', None)
            env['LD_PRELOAD'] = lease_shims(self.app)
            self.leases.environment(env, 'bottom')
        return env

    def port_leases(self, env):
        """The port's half of the leases (RG DS OS): each of its processes asks the runner for a lease of the top
        panel when it opens the card (drm_lease.PanelLeases.serve). Returns the descriptors the port inherits: none."""
        if self.leases is None: return ()
        self.leases.environment(env, 'top')
        env['LD_PRELOAD'] = ' '.join(p for p in (lease_shims(self.app), env.get('LD_PRELOAD', '')) if p)
        return ()

    def write_state(self, state):
        self.state_token += 1
        write_fields(self.state_file, dict(state=state, token=self.state_token), durable=False)

    def start_companion(self, timeout=6.0):
        """The helper maps its windows now, before the port, so the port is the last window and keeps focus."""
        self.ready.unlink(missing_ok=True)
        self.write_state('idle')
        from linux_platform import rgds_os
        if rgds_os():
            from drm_lease import PanelLeases
            try:
                self.leases = PanelLeases(top='DSI-2', bottom='DSI-1')
            except OSError as error:
                # The port then opens the card itself and has both panels; Home returns home, with no menu.
                print(
                    'RGDS port: no panel leases (%s); the port runs without the menu' % error,
                    file=sys.stderr,
                    flush=True
                )
                return False
        command = [
            str(self.app / "bin/rgds-launcher"), "--port-menu", "--app",
            str(self.app), "--data",
            str(self.data), "--game-core", self.core, "--game-rom",
            str(self.rom)
        ]
        env = self.companion_environment()
        if env.get('RGDS_SESSION_PLATFORM') == 'independent' and env.get('RGDS_TOP_DISPLAY'
                                                                         ) and env.get('RGDS_BOTTOM_DISPLAY'):
            command += ['--top-display', env['RGDS_TOP_DISPLAY'], '--bottom-display', env['RGDS_BOTTOM_DISPLAY']]
        self.companion = subprocess.Popen(
            command, env=env, pass_fds=(self.leases.fds['bottom'],) if self.leases is not None else ()
        )
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline and self.companion.poll() is None:
            try:
                if self.ready.read_text().strip() == str(self.companion.pid): return True
            except OSError:
                pass
            time.sleep(.05)
        print(
            'RGDS port: the menu helper did not show within %.0f s; exit=%s' % (timeout, self.companion.poll()),
            file=sys.stderr,
            flush=True
        )
        return False

    def attach(self, child):
        self.child = child
        self.began = time.monotonic()

    # Freeze and thaw -------------------------------------------------------------------
    def _freeze(self, frozen):
        try:
            (LEAF / 'cgroup.freeze').write_text('1' if frozen else '0')
        except OSError as error:
            print('RGDS port: cgroup freeze: ' + str(error), file=sys.stderr, flush=True)
            for pid in members():
                try:
                    os.kill(pid, signal.SIGSTOP if frozen else signal.SIGCONT)
                except ProcessLookupError:
                    pass
            return
        deadline = time.monotonic() + .2
        while time.monotonic() < deadline:
            try:
                if ('frozen 1' in (LEAF / 'cgroup.events').read_text()) == frozen: return
            except OSError:
                return
            time.sleep(.002)

    def pause(self):
        if self.paused or self.child is None or self.child.poll() is not None: return
        tree = set(members())
        for _ in range(50):
            self._freeze(True)
            # Never leave the port frozen inside the shared mixer's critical section (stop_outside_mixer).
            if mixer_holder() not in tree: break
            self._freeze(False)
            time.sleep(.002)
        else:
            print('RGDS port: the port still held the shared mixer while frozen.', file=sys.stderr, flush=True)
        self.paused = True

    def resume_engine(self):
        if self.paused and self.child is not None and self.child.poll() is None:
            drain_inputs(members())
            self._freeze(False)
        self.paused = False

    # Opening and closing -----------------------------------------------------------------
    def open(self):
        self.open_ready()

    def open_ready(self, switcher=False):
        if self.menu is not None or self.child is None or self.child.poll() is not None: return
        # The helper reads which view to show from the status once it turns into the menu.
        self.view_request = 1 if switcher else 0
        self.switcher_requested = switcher
        try:
            self.prepare_switcher()
        except OSError as error:
            self.switch_games = []
            print('Game switcher unavailable: ' + str(error), file=sys.stderr, flush=True)
        self.pause()
        self.keys.grab()
        self.block.touch()
        self.last_toggle = time.monotonic()
        self.publish('paused')
        self.write_state('menu')
        self.menu = self.companion
        self.direct_ready = True
        self.menu_began = time.monotonic()

    def close_menu(self):
        if self.menu is None and not self.paused: return
        self.write_state('idle')
        if self.menu_began is not None:
            self.menu_seconds += time.monotonic() - self.menu_began
            self.menu_began = None
        self.menu = None
        for path in (self.trigger, self.home_trigger, self.game_menu_trigger):
            path.unlink(missing_ok=True)
        try:
            self.keys.release()
            self.resume_engine()
        finally:
            self.block.unlink(missing_ok=True)

    def stop_port(self):
        """Return home, restart or switch games: SIGTERM to every process, a moment to save, then cgroup.kill."""
        self.user_stopped = True
        self.publish('busy', 'Closing PortMaster…' if self.manager else 'Closing the port…')
        for pid in members():
            try:
                os.kill(pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
        self.keys.release()
        self.resume_engine()
        deadline = time.monotonic() + 3
        while self.child.poll() is None and time.monotonic() < deadline:
            time.sleep(.05)
        kill_leaf()

    def force_quit(self):
        self.stop_port()

    def port_result(self, result):
        """A port the player closed (menu, Select+Start) came home normally."""
        return 0 if self.user_stopped else result

    def switch_game(self, index):
        """A port has no checkpoint to save first: it saves itself as it closes, then the next game starts."""
        target = self.switch_choice(index)
        if target is None: return
        self.restart = False
        self.switch_target = target
        self.stop_port()

    def completed_switch(self):
        return self.switch_target if self.child is not None and self.child.poll() is not None else None

    # The supervisor loop -----------------------------------------------------------------
    def poll(self):
        if self.child is None: return
        if self.lid.poll(ready=self.child.poll() is None):
            self.sleep_for_lid()
            return
        key = self.inputs.poll()
        if isinstance(key, str) and key.startswith('fast-forward-'): key = False
        # As for every game: the Anbernic button (or the info screen's Game switcher, .menu) opens the
        # switcher, the home shortcut (or the info screen's held Return home, .home) goes home, Home opens
        # the game menu and Home again resumes. PortMaster has only its two-choice sheet.
        home, switcher, game_menu = (
            path.exists() for path in (self.home_trigger, self.trigger, self.game_menu_trigger)
        )
        for path in (self.trigger, self.home_trigger, self.game_menu_trigger):
            path.unlink(missing_ok=True)
        wanted = 'home' if home or key == 'home' else 'switcher' if switcher or key == 'menu' else 'menu' if (
            game_menu or key
        ) else None
        if self.manager and wanted: wanted = 'menu'
        if wanted and self.child.poll() is None and time.monotonic() - self.last_toggle >= .3:
            self.last_toggle = time.monotonic()
            if self.companion is None:
                self.stop_port()  # no helper to show a menu in (RG DS OS without panel leases): home
            elif wanted == 'home':
                if self.menu is None: self.open_ready()
                self.stop_port()
            elif wanted == 'switcher':
                if self.menu is None: self.open_ready(switcher=True)
                else:
                    self.switcher_requested = True
                    self.view_request += 1
                    self.publish('paused')
            elif self.menu is None:
                self.open()
            else:
                self.close_menu()
        if self.child.poll() is not None:
            self.close_menu()
            self.publish('home')
            return
        if self.menu is None: return
        if self.companion is not None and self.companion.poll() is not None:
            print('RGDS port: the menu helper exited; resuming the port.', file=sys.stderr, flush=True)
            self.close_menu()
            return
        from library import read_fields
        request = read_fields(self.request)
        token, action = request.get('token', ''), request.get('action', '')
        if not token or token == self.last_request: return
        self.last_request = self.accepted_request = token
        print('RGDS port menu: %s %s' % (token, action), flush=True)
        if action == 'resume':
            self.close_menu()  # the helper went back to its companion view itself
        elif action in ('quit', 'save-quit', 'force-quit'):
            self.stop_port()
        elif action in ('restart', 'reset-state'):
            self.restart = True
            self.stop_port()
        elif action.startswith('switch:'):
            self.switch_game(action[len('switch:'):])
        else:
            self.publish('error', 'This is not available for ports.')

    def close(self):
        try:
            self.close_menu()
        finally:
            try:
                kill_leaf()
                try:
                    LEAF.rmdir()
                except OSError:
                    pass
            finally:
                self.keys.release()
                if self.companion is not None and self.companion.poll() is None:
                    self.companion.terminate()
                    try:
                        self.companion.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        self.companion.kill()
                        self.companion.wait()
                if self.leases is not None:
                    self.leases.close()
                    self.leases = None
                self.release_clocks()
                self.inputs.close()
                try:
                    # Ports PortMaster just installed get their own pictures before the launcher lists them.
                    from portmaster import artwork, notes
                    artwork(self.app, self.data)
                    notes(self.data)
                except Exception as error:
                    print('RGDS port: artwork: ' + str(error), file=sys.stderr, flush=True)
                for path in (self.state_file, self.port_status, self.ready):
                    path.unlink(missing_ok=True)
                if self.overlay_path: self.overlay_path.unlink(missing_ok=True)
