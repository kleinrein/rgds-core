"""One RGDS pause menu and bounded engine commands, owned by the game runner."""
import fcntl
import json
import os
from pathlib import Path
import select
import signal
import struct
import subprocess
import sys
import time

from audio import EVENT, clean_environment
from shared_audio import stop_outside_mixer
from emulator_profiles import policy
from engines import CORES
from shaders import supported
from library import read_fields, write_fields, game_key
from controls import load_profile, paths as control_paths, Shortcut, FastForwardButton, evdev_source
from lid_sleep import LidSleep
from game_clocks import GameClocks
from collections import deque
from typing import NamedTuple


class SwitchGame(NamedTuple):
    core: str
    rom: Path

MENU_CODES = (102, 139, 158, 172, 316)
ANBERNIC_MENU_CODES = (102, 139, 158, 172, 312, 354)

def menu_button_action(code, anbernic=False):
    """The logo/Guide button and the separate Home key have distinct routes."""
    if code in ((312, 354) if anbernic else (316,)):
        return 'menu'
    if code in (102, 139, 158, 172):
        return 'game-menu'
    return None


def menu_environment(environment):
    """Resolve a unique touch node once, without opening unrelated drivers."""
    result = dict(environment)
    if result.get('RGDS_TOUCH_DEV'):
        return result
    from input_devices import discover
    try:
        inputs = discover()
    except OSError:
        return result
    # Ambiguous/missing capability data keeps the native discovery fallback.
    # The native reader still validates axes, touch capabilities and ranges.
    if inputs.get('touch') and not inputs.get('touch_ambiguous'):
        result['RGDS_TOUCH_DEV'] = inputs['touch']
    return result


class MenuInput:
    def __init__(self, data=None, rom=None):
        self.descriptors = []
        self.codes = {}
        self.starts = {}
        self.last_press = -10
        self.data, self.rom = data, rom
        self.profile = load_profile(data, rom) if data is not None else {"shortcut":"start+menu","shortcut_action":"home"}
        self.shortcut = Shortcut(self.profile["shortcut"], self.profile["shortcut_action"])
        self.profile_at = 0
        self.profile_stamp = None
        self.suppressed_keys = set()
        self.speed=FastForwardButton();self.speed.configure(self.profile);self.actions=deque();self.calibrated={};self.mainline={}
        for path in sorted(Path("/dev/input").glob("event*")):
            fd = -1
            try:
                fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC)
                bits = bytearray(96)
                fcntl.ioctl(fd, 0x80604521, bits)
                name = bytearray(128)
                fcntl.ioctl(fd, 0x80804506, name)
                calibrated=b"ANBERNIC" in name or b"retrogame_joypad" in name
                codes=ANBERNIC_MENU_CODES if calibrated else MENU_CODES
                if any(bits[code // 8] & (1 << (code % 8)) for code in codes):
                    self.descriptors.append(fd)
                    self.codes[fd]=codes
                    self.starts[fd]=311 if calibrated else 315
                    self.calibrated[fd]=calibrated or b"gpio-keys-control" in name
                    self.mainline[fd]=b"gpio-keys-control" in name
                else:
                    os.close(fd)
            except OSError:
                if fd >= 0:
                    os.close(fd)

    def poll(self):
        now = time.monotonic()
        if self.data is not None and now-self.profile_at >= .25:
            self.profile_at = now
            stamps=[]
            for path in control_paths(self.data,self.rom):
                try:
                    info=path.stat() if path else None
                    stamps.append((info.st_ino,info.st_size,info.st_mtime_ns,info.st_ctime_ns) if info else None)
                except OSError: stamps.append(None)
            if stamps != self.profile_stamp:
                self.profile_stamp=stamps
                profile=load_profile(self.data,self.rom)
                self.shortcut.mode,self.shortcut.action=profile['shortcut'],profile['shortcut_action']
                self.profile=profile
                change=self.speed.configure(profile)
                if change:self.actions.append(change)
        readable, _, _ = select.select(self.descriptors, [], [], 0)
        for fd in readable:
            try:
                data = os.read(fd, EVENT.size * 64)
            except BlockingIOError:
                continue
            except OSError:
                change=self.speed.feed(self.speed.source,False,fd)
                if change:self.actions.append(change)
                self.descriptors.remove(fd);os.close(fd)
                continue
            for offset in range(0, len(data) - EVENT.size + 1, EVENT.size):
                _sec, _usec, kind, code, value = EVENT.unpack_from(data, offset)
                held = (fd, kind, code)
                if held in self.suppressed_keys:
                    if value == 0:
                        self.suppressed_keys.discard(held)
                    continue
                if kind==0 and code==3:
                    change=self.speed.feed(self.speed.source,False,fd)
                    if change:self.actions.append(change)
                if value in (0,1,-1) and kind in (1,3) and self.calibrated.get(fd):
                    if kind==3 and code in (16,17):
                        for source in (('left','right') if code==16 else ('up','down')):
                            change=self.speed.feed(source,source==evdev_source(kind,code,value),fd)
                            if change:self.actions.append(change)
                    elif kind==1:
                        change=self.speed.feed(evdev_source(kind,code,value,mainline=self.mainline.get(fd,False)),value==1,fd)
                        if change:self.actions.append(change)
                if kind == 1 and value in (0,1):
                    button=menu_button_action(code, 312 in self.codes[fd]) if code in self.codes[fd] else None
                    if button == 'game-menu':
                        if value == 1:self.actions.append(button)
                        continue
                    source=button or ('start' if code==self.starts[fd] else None)
                    result=self.shortcut.feed(source,value==1,now)
                    if result:self.actions.append(result)
        action=self.shortcut.poll(now)
        if action:self.actions.append(action)
        return self.actions.popleft() if self.actions else False

    def discard_after_sleep(self):
        # Read-and-discard, never replay presses accumulated around suspend.
        self.actions.clear()
        self.shortcut = Shortcut(self.profile['shortcut'], self.profile['shortcut_action'])
        self.speed.held.clear()
        self.suppressed_keys.clear()
        for fd in list(self.descriptors):
            try:
                while os.read(fd, EVENT.size * 64):
                    pass
            except OSError:
                pass
            try:
                bits = bytearray(96)
                fcntl.ioctl(fd, 0x80604518, bits)  # EVIOCGKEY(96)
                for code in range(len(bits) * 8):
                    if bits[code // 8] & (1 << (code % 8)):
                        self.suppressed_keys.add((fd, 1, code))
                for code in (16, 17):
                    axis = bytearray(24)
                    fcntl.ioctl(fd, 0x80184540 + code, axis)  # EVIOCGABS
                    if struct.unpack_from('i', axis)[0]:
                        self.suppressed_keys.add((fd, 3, code))
            except OSError:
                pass

    def close(self):
        for fd in self.descriptors:
            os.close(fd)
        self.descriptors = []


class GameMenu:
    def __init__(self, child, app, data, core, rom, environment, inputs=None):
        self.child, self.app, self.data, self.core, self.rom = child, Path(app), Path(data), core, Path(rom)
        self.environment = menu_environment(environment)
        self.live = environment.get('RGDS_MENU_LIVE') == '1'
        self.restart = False
        self.restart_same_core = False
        self.switch_target = None
        self.switcher_requested = False
        self.view_request = 0
        self.reset_state = False
        self.slot_operation = None
        self.fast_forward = False
        self.threaded = environment.get("RGDS_THREADED_3D", "1") != "0"
        self.speed_state=Path(str(self.data / 'session/emulator.control')+'.speed-state')
        self.fps = environment.get('RGDS_FPS') == '1'
        self.active_scale = environment.get('RGDS_ACTIVE_SCALE', '1')
        # The native and high-resolution renderers are separate binaries.
        self.launch_hires = self.active_scale not in ('', '1')
        # GPU-rendered games hold the GPU and memory clock floors (game_clocks).
        self.clocks = None
        if GameClocks.wanted(environment, self.data):
            try:
                self.clocks = GameClocks(self.data)
                held = self.clocks.hold()
                if held: print('Clock floor held for ' + ', '.join(held), flush=True)
            except (OSError, ValueError) as error:
                self.clocks = None
                print('Clock floor not held: ' + str(error), file=sys.stderr, flush=True)
        self.active_smooth = environment.get('RGDS_3D_SMOOTH', '1')
        self.active_filter2d = environment.get('RGDS_2D_FILTER', '0')
        self.stats_path = Path(environment['RGDS_FRAME_STATS']) if environment.get('RGDS_FRAME_STATS') else None
        self.overlay_path = Path(environment['RGDS_OVERLAY_FILE']) if environment.get('RGDS_OVERLAY_FILE') else None
        self.overlay_ready = False
        self.direct_ready = False
        self.inputs = inputs if inputs is not None else MenuInput()
        self.lid = LidSleep(self.data)
        self.folder = self.data / "session"
        self.control = self.folder / "emulator.control"
        self.request = self.folder / "game-menu.request"
        self.status = self.folder / "game-menu.status"
        self.ready = self.folder / "game-menu.ready"
        self.trigger = Path(str(self.control) + ".menu")
        self.home_trigger = Path(str(self.control) + ".home")
        self.game_menu_trigger = Path(str(self.control) + ".game-menu")
        self.block = Path(str(self.control) + ".blocked")
        self.menu = None
        self.snapshot_deadline = None
        self.paused = False
        self.pending = None
        self.sequence = 0
        self.last_request = ""
        self.accepted_request = ""
        self.last_toggle = -10
        self.began = time.monotonic()
        self.menu_began = None
        self.menu_seconds = 0
        self.folder.mkdir(parents=True, exist_ok=True)
        for path in (self.request, self.status, self.trigger, self.home_trigger, self.game_menu_trigger, self.block, self.folder / 'game-menu.wake', self.folder / 'game-menu.audio-sleep', self.folder / 'game-menu.audio-sleep.ready'):
            path.unlink(missing_ok=True)
        # Load the pause path's modules while the engine boots, not on the
        # first menu press.
        try:
            import menu_snapshot, save_library  # noqa: F401
        except ImportError:
            pass
        # Monotonic stages of the current menu opening, reported once it shows.
        self.open_stages = None

    def publish(self, state, message="", **fields):
        write_fields(self.status, dict(state=state, message=message, core=self.core,
                    request_token=self.accepted_request, active_scale=self.active_scale, active_smooth=self.active_smooth, active_filter2d=self.active_filter2d,
                    checkpoint=int(self.has_checkpoint()), live=int(self.live and not self.paused), fast_forward=int(self.fast_forward), fps=int(self.fps), threaded=int(self.threaded), view_request=self.view_request, view='switcher' if self.switcher_requested else 'game-menu', **fields), durable=False)

    def checkpoint(self):
        return self.data / "library" / game_key(self.rom) / ("auto-" + self.core + ".dss")

    def has_checkpoint(self):
        try:
            return self.checkpoint().is_file() and self.checkpoint().stat().st_size > 0
        except OSError:
            return False

    def prepare_switcher(self):
        """A bounded snapshot from the launcher's already-scanned library."""
        source = read_fields(self.folder / 'switcher-library.ini')
        self.switch_games = [(self.rom.resolve(), self.rom.stem)]
        for index in range(9):
            name = source.get(f'game_{index}_rom', '')
            if not name or '\n' in name or '\r' in name:
                continue
            path = Path(name).resolve()
            if path.suffix.lower() not in ('.nds', '.zip') or not path.is_file() or any(path == game[0] for game in self.switch_games):
                continue
            self.switch_games.append((path, source.get(f'game_{index}_title', path.stem)))
            if len(self.switch_games) == 9:
                break
        values = {'count': len(self.switch_games)}
        for index, (rom, title) in enumerate(self.switch_games):
            values[f'game_{index}_rom'] = str(rom)
            values[f'game_{index}_title'] = title
        write_fields(self.folder / 'game-switcher.ini', values, durable=False)

    def switch_game(self, index):
        if self.pending:
            return
        try:
            rom, _ = self.switch_games[int(index)]
            if int(index) < 0 or not rom.is_file():
                raise ValueError('Game unavailable')
            if rom == self.rom.resolve():
                self.close_menu()
                return
        except (ValueError, IndexError, AttributeError, OSError):
            self.publish('error', 'This game is no longer available.')
            return
        options = read_fields(self.data / 'library' / game_key(rom) / 'game.ini')
        defaults = read_fields(self.data / 'preferences.ini')
        try:
            core = int(options.get('core', '-1'))
            if core < 0:
                core = int(defaults.get('core', '1'))
            if not 0 <= core < len(CORES):
                raise ValueError('Invalid emulator')
        except ValueError:
            self.publish('error', 'Choose an emulator for this game in the library.')
            return
        self.restart = False
        self.switch_target = SwitchGame('rgds-experimental' if CORES[core] == 'nnddss' else CORES[core], rom)   # NNDDSS is retired
        # Always require a successful checkpoint, even with auto-save disabled.
        self.send('save-quit')

    def switch_checkpoint_saved(self):
        result = read_fields(Path(str(self.control) + '.result'))
        return bool(self.pending and self.pending['action'] == 'save-quit' and
                    result.get('token') == self.pending['token'] and result.get('state') == 'saved' and self.has_checkpoint())

    def completed_switch(self):
        return self.switch_target if self.child.poll() is not None and self.switch_target and self.switch_checkpoint_saved() else None

    def pause(self):
        if not self.paused and self.child.poll() is None:
            # Never stop the engine inside the shared mixer's critical section:
            # the menu's cue sink would block on its semaphore and freeze.
            if not stop_outside_mixer(self.child):
                print('RGDS menu: the engine still holds the shared mixer while stopped; menu sounds may wait for it.', file=sys.stderr, flush=True)
            self.paused = True

    def resume_engine(self):
        if self.paused and self.child.poll() is None:
            self.child.send_signal(signal.SIGCONT)
        self.paused = False

    def stop_speed_for_menu(self):
        stop = self.inputs.speed.pause() if hasattr(self.inputs, 'speed') else None
        # Speed may have been enabled by the menu, not the physical button.
        # Stop it before the capture/pause handoff, while the guest still polls.
        if stop or read_fields(self.speed_state).get('active') == '1':
            self.request_speed('fast-forward-off')
        self.fast_forward = False

    def open(self):
        if self.menu is not None or self.snapshot_deadline is not None or self.child.poll() is not None:
            return
        self.open_stages = dict(pressed=time.monotonic())
        self.stop_speed_for_menu()
        backdrop = self.folder / 'menu-background.bmp'
        backdrop.unlink(missing_ok=True)
        # Request just one frame before pausing. The main loop keeps servicing
        # input and exit commands during this bounded handoff; no blocking sleep.
        # A live engine (DSCore on direct display) holds the game once input
        # is blocked and keeps presenting that frame, so it is captured too.
        if self.environment.get('RGDS_CONTROL_FILE'):
            for suffix in ('.capture-top.bmp', '.capture-bottom.bmp', '.capture-combined.bmp'):
                Path(str(self.control)+suffix).unlink(missing_ok=True)
            self.block.touch()
            Path(str(self.control)+'.capture').write_text(f'menu-{os.getpid()}-{time.monotonic_ns()}\n')
            self.snapshot_deadline = time.monotonic() + (.25 if self.live else .18)
            return
        self.open_ready()

    def poll_snapshot(self):
        if self.snapshot_deadline is None:
            return
        sources = [Path(str(self.control)+suffix) for suffix in ('.capture-top.bmp', '.capture-combined.bmp')]
        source = next((path for path in sources if path.is_file()), None)
        if source is None and time.monotonic() < self.snapshot_deadline and self.child.poll() is None:
            return
        self.snapshot_deadline = None
        Path(str(self.control)+'.capture').unlink(missing_ok=True)
        if self.open_stages is not None:
            self.open_stages['captured' if source is not None else 'capture_missed'] = time.monotonic()
        if source is not None:
            try:
                from menu_snapshot import top_panel
                top_panel(source, self.folder / 'menu-background.bmp')
            except (OSError, ValueError):
                pass  # A missing frame must never prevent pause or exit.
        self.open_ready()

    def open_ready(self):
        if self.menu is not None or self.child.poll() is not None:
            return
        self.view_request = 0
        stop=self.inputs.speed.pause() if hasattr(self.inputs,'speed') else None
        if stop:self.request_speed(stop)
        self.fast_forward=read_fields(self.speed_state).get('active','0')=='1' and not stop
        if not self.live and not self.pending:self.pause()
        self.last_toggle = time.monotonic()
        self.block.touch()
        self.ready.unlink(missing_ok=True)
        self.direct_ready = False
        try:self.vault().publish()
        except (OSError,ValueError) as error:print('Save gallery: '+str(error),file=sys.stderr)
        if not self.pending:self.publish("paused")
        env = clean_environment()
        # Keep the session's known vendor SDL path/Wayland socket, drop core hooks.
        for key in ("XDG_RUNTIME_DIR", "WAYLAND_DISPLAY", "RGDS_SDL_LIBRARY", "SDL_VIDEODRIVER", "SDL_RENDER_DRIVER", "SDL_RENDER_BATCHING", "RGDS_LAUNCH_PROFILE", "SDL_VIDEO_WAYLAND_ALLOW_LIBDECOR", "TZ", 'RGDS_MENU_EXTERNAL','RGDS_LEGACY_ACTIVE',
                    'RGDS_SD_SHARED_MENU','RGDS_INPUT_PROFILE','RGDS_SESSION_PLATFORM','RGDS_TOP_DISPLAY','RGDS_BOTTOM_DISPLAY','RGDS_LID_DEV','RGDS_LID_WAKEUP','RGDS_TOUCH_DEV','RGDS_INPUT_AMBIGUOUS'):
            if key in self.environment:
                env[key] = self.environment[key]
        env["RGDS_SYSTEM_MANAGED"] = "1"
        env["RGDS_VOLUME_EXTERNAL"] = "1"
        if not self.live:env["RGDS_MENU_READY_FILE"]=str(self.ready)
        # A stopped emulator cannot present shared frames. The paused menu owns
        # its GPU windows, avoiding a software render/readback/copy per frame.
        if self.live:
            env['RGDS_MENU_LIVE']='1'
            if self.overlay_path:env['RGDS_OVERLAY_FILE']=str(self.overlay_path)
        self.overlay_ready = False
        if self.overlay_path and self.overlay_path.exists():
            with self.overlay_path.open('r+b') as stream:
                stream.seek(16);stream.write(struct.pack('I',0))
                stream.seek(24);stream.write(struct.pack('I',0))
        switcher_ready = self.switcher_requested
        try:self.prepare_switcher()
        except OSError as error:
            switcher_ready = False
            self.switch_games = []
            print('Game switcher unavailable: ' + str(error), file=sys.stderr)
        command = [str(self.app / "bin/rgds-launcher"), "--game-menu", "--app", str(self.app),
                   "--data", str(self.data), "--game-core", self.core, "--game-rom", str(self.rom)]
        if switcher_ready:command.append('--game-switcher')
        if env.get('RGDS_SESSION_PLATFORM')=='independent' and env.get('RGDS_TOP_DISPLAY') and env.get('RGDS_BOTTOM_DISPLAY'):
            command+=['--top-display',env['RGDS_TOP_DISPLAY'],'--bottom-display',env['RGDS_BOTTOM_DISPLAY']]
        try:
            self.menu = subprocess.Popen(command, env=env)
            self.menu_began = time.monotonic()
            if self.open_stages is not None:
                self.open_stages['spawned'] = self.menu_began
        except OSError:
            self.close_menu()
            raise

    def report_open(self):
        """One log line per opening: where the press-to-picture time went."""
        stages, self.open_stages = self.open_stages, None
        if not stages or 'spawned' not in stages:
            return
        shown = time.monotonic()
        def ms(start, end):
            return round((stages[end] if end in stages else shown) - stages[start], 3) * 1000 if start in stages else None
        capture = 'captured' if 'captured' in stages else 'capture_missed' if 'capture_missed' in stages else None
        parts = dict(total=ms('pressed', 'shown'), capture=ms('pressed', capture) if capture else 0,
                     handoff=ms(capture, 'spawned') if capture else ms('pressed', 'spawned'), helper=ms('spawned', 'shown'))
        print('RGDS menu: shown in {total:.0f} ms (capture {capture:.0f}, handoff {handoff:.0f}, helper {helper:.0f})'.format(**parts)
              + (' without a backdrop' if capture == 'capture_missed' else ''), flush=True)

    def close_menu(self):
        self.snapshot_deadline = None
        Path(str(self.control)+'.capture').unlink(missing_ok=True)
        closing = self.menu
        began = time.monotonic()
        was_paused = self.paused
        # Keep game input blocked while releasing the menu's GPU/Wayland
        # windows. A SIGSTOPped engine cannot service its pending display work
        # while another client tears down; resume it before waiting for SDL.
        self.resume_engine()
        resumed = time.monotonic()
        terminated = None
        killed = False
        if self.menu is not None:
            if self.menu.poll() is None:
                terminated = time.monotonic()
                self.menu.terminate()
                try:
                    self.menu.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    killed = True
                    print(f'RGDS menu: helper {self.menu.pid} did not exit within 2 seconds; killing it.', file=sys.stderr, flush=True)
                    self.menu.kill();self.menu.wait()
            self.menu = None
        self.ready.unlink(missing_ok=True)
        if self.overlay_path and self.overlay_path.exists():
            # SIGTERM may end the helper before C++ destructors hide the frame.
            try:
                with self.overlay_path.open('r+b') as stream:
                    stream.seek(16);stream.write(struct.pack('I',0))
            except OSError:
                pass
        if self.menu_began is not None:
            self.menu_seconds += time.monotonic() - self.menu_began
            self.menu_began = None
        self.trigger.unlink(missing_ok=True)
        self.home_trigger.unlink(missing_ok=True)
        self.game_menu_trigger.unlink(missing_ok=True)
        # The bridge discards queued menu presses before the game receives input.
        try:
            write_fields(Path(str(self.control) + ".resume"),
                         dict(monotonic=time.monotonic(), realtime=time.time()), durable=False)
        finally:
            self.block.unlink(missing_ok=True)
            self.resume_engine()
        if closing is not None and self.environment.get('RGDS_LAUNCH_PROFILE') == '1':
            print(json.dumps(dict(type='rgds-menu-close-profile', pid=closing.pid,
                  was_paused=was_paused, started_monotonic=began,
                  resumed_monotonic=resumed, terminated_monotonic=terminated,
                  ended_monotonic=time.monotonic(), exit_code=closing.poll(), killed=killed)), flush=True)

    def played_seconds(self):
        current = time.monotonic() - self.menu_began if self.menu_began is not None else 0
        return max(0, int(time.monotonic() - self.began - self.menu_seconds - current))

    def vault(self):
        from save_library import SaveLibrary
        return SaveLibrary(self.data,self.rom,self.core,self.app.name)

    def slot_request(self, action):
        try:
            pieces=action.split(':');operation=pieces[0]
            if operation=='slot-undo':self.vault().undo_write();self.publish('paused','Save overwrite undone.');return
            if operation=='slot-export':self.vault().export(int(pieces[1]));self.publish('paused','Slot exported to userdata/exports.');return
            if operation=='slot-save':
                slot=int(pieces[1]);overwrite=len(pieces)>2 and pieces[2]=='yes'
                if str(slot) in self.vault().catalog()['slots'] and not overwrite:raise ValueError('Confirm overwriting this slot.')
            else:slot=int(pieces[1]) if len(pieces)>1 else 0
            if operation not in ('slot-save','slot-load','slot-undo-load') or not 0<=slot<9:raise ValueError('Invalid save action.')
            if operation=='slot-load':self.vault().checked(self.vault().catalog()['slots'].get(str(slot),{}))
            self.slot_operation=dict(operation=operation,slot=slot,overwrite=operation=='slot-save' and overwrite)
            self.send('save')
        except (OSError,ValueError,KeyError,IndexError) as error:self.slot_operation=None;self.publish('error',str(error))

    def send(self, action):
        if action.startswith('slot-'):
            if not self.pending:self.slot_request(action)
            return
        if self.pending:
            return
        if action.startswith('threaded:') and (self.core != 'melonds' or action not in ('threaded:0','threaded:1')):
            self.publish('error', 'This engine has no live threading control.')
            return
        if action.startswith('graphics:'):
            try:
                scale,smooth,skip=map(int,action.split(':')[1:])
                if self.core!='rgds-experimental' or not (1<=scale<=5 and 0<=smooth<=1 and 0<=skip<=3):raise ValueError()
            except ValueError:
                self.publish('error','Invalid DSCore graphics setting.')
                return
        if action.startswith('filter2d:'):
            try:
                mode=int(action.split(':')[1])
                if self.core!='rgds-experimental' or action.count(':')!=1 or not 0<=mode<=3:raise ValueError()
            except ValueError:
                self.publish('error','Invalid DSCore 2D filter.')
                return
        if action.startswith('adaptive:') and (self.core != 'rgds-experimental' or action not in ('adaptive:0','adaptive:1')):
            self.publish('error', 'This engine has no live heavy-scene control.')
            return
        if action.startswith('shader:'):
            try:choice=int(action.split(':',1)[1])
            except ValueError:choice=-1
            if not supported(self.core,choice):
                self.publish('error','This shader is unavailable for the current emulator.')
                return
        if action == "quit" and self.restart:
            action = "save-quit"
        if action == "fast-forward" and self.core not in CORES:
            self.publish("paused", "Fast-forward is unavailable for this emulator.")
            return
        if action == "load" and not self.has_checkpoint():
            self.publish("error", "No checkpoint saved for this emulator yet.")
            return
        self.sequence += 1
        token = f"{os.getpid()}-{self.sequence}"
        pending = self.control.with_suffix(".pending")
        pending.write_text(token + " " + action + "\n")
        pending.replace(self.control)
        self.pending = dict(token=token, action=action, began=time.monotonic())
        print(f'RGDS menu: {token} {action}', flush=True)
        self.publish("busy", "Saving before switching games…" if self.switch_target else 'Changing 3D threading…' if action.startswith('threaded:') else 'Applying graphics…' if action.startswith(('graphics:','filter2d:','adaptive:')) else 'Applying shader…' if action.startswith('shader:') else ("Saving checkpoint before restarting…" if self.restart else "Saving before returning home…") if action == "save-quit" else 'Changing speed…' if action == 'fast-forward' else "Saving before returning home…" if action == "quit" and policy(self.data)[1]
                     else "Returning home…" if action == "quit" else "Saving checkpoint…" if action == "save" else "Loading checkpoint…")
        self.resume_engine()

    def request_speed(self, action):
        if action not in ('fast-forward-on','fast-forward-off','fast-forward-toggle'):return
        self.sequence+=1
        write_fields(Path(str(self.control)+'.speed-request'),dict(token=f'{os.getpid()}-speed-{self.sequence}',action=action),durable=False)

    def force_quit(self):
        self.pending = None
        self.resume_engine()
        self.child.terminate()
        try:
            self.child.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.child.kill();self.child.wait()
        self.publish("home")

    def poll(self):
        self.poll_snapshot()
        if self.lid.poll(ready=self.pending is None and self.child.poll() is None):
            self.sleep_for_lid()
            return
        key = self.inputs.poll()
        if isinstance(key,str) and key.startswith('fast-forward-'):
            self.request_speed(key);key=False
        home = self.home_trigger.exists() or key == 'home'
        if self.home_trigger.exists(): self.home_trigger.unlink(missing_ok=True)
        game_menu = self.game_menu_trigger.exists() or key == 'game-menu'
        self.game_menu_trigger.unlink(missing_ok=True)
        triggered = self.trigger.exists()
        if triggered:
            self.trigger.unlink(missing_ok=True)
        # Independent evdev and SDL paths can observe the same physical press
        # on adjacent supervisor polls. One press must toggle the menu once.
        if home and not self.pending and self.child.poll() is None:
            self.last_toggle=time.monotonic()
            if self.menu is None:self.open()
            # The shortcut is an explicit save-before-exit action. Save failure
            # remains visible in the shared menu; it never kills a live game.
            self.restart=False
            self.switch_target=None
            self.send('save-quit')
        elif game_menu and not self.pending and self.child.poll() is None:
            self.last_toggle = time.monotonic()
            self.switcher_requested = False
            if self.menu is None:self.open()
            else:
                self.view_request += 1
                self.publish('paused')
        elif (key or triggered) and time.monotonic() - self.last_toggle >= .25:
            self.last_toggle = time.monotonic()
            if self.menu is None:
                self.switcher_requested = True
                self.open()
            elif not self.pending:
                self.switcher_requested = True
                self.view_request += 1
                self.publish('paused')
        if self.menu is None:
            return
        speed=read_fields(self.speed_state)
        if speed.get('active') in ('0','1') and self.fast_forward!=(speed['active']=='1'):
            self.fast_forward=speed['active']=='1'
            if not self.pending:self.publish('paused')
        if self.live and self.overlay_path and not self.overlay_ready:
            try:
                with self.overlay_path.open('rb') as stream:header=struct.unpack('7I',stream.read(28))
                self.overlay_ready = header[0] == 0x52474453 and header[6] > 0
            except (OSError,struct.error):pass
            if self.overlay_ready:self.report_open()
            if not self.overlay_ready and self.menu_began is not None and time.monotonic()-self.menu_began > 3:
                self.close_menu()
                self.publish('error','The overlay did not render. Game controls restored.')
                return
        if not self.live and not self.direct_ready:
            try:self.direct_ready = self.ready.read_text().strip() == str(self.menu.pid)
            except OSError:pass
            if self.direct_ready:self.report_open()
            if not self.direct_ready and self.menu_began is not None and time.monotonic()-self.menu_began > 5:
                print(f'RGDS menu: helper {self.menu.pid} did not present within 5 seconds; exit={self.menu.poll()}.', file=sys.stderr, flush=True)
                self.close_menu()
                self.publish('error','The game menu did not render. Game controls restored.')
                return
        request = read_fields(self.request)
        token, action = request.get("token", ""), request.get("action", "")
        if token and token != self.last_request:
            self.last_request = token
            if not self.pending or action == 'force-quit':
                self.accepted_request = token
            if action == "resume":
                if not self.pending:
                    print(f'RGDS menu: {token} resume', flush=True)
                    self.switch_target = None
                    self.restart = False
                    self.close_menu()
                return
            if action.startswith('switch:'):
                self.switch_game(action.split(':', 1)[1])
            elif action.startswith('graphics:') and not self.pending and self.renderer_restart(action):
                pass
            elif action in ('restart', 'reset-state') and not self.pending:
                self.switch_target = None
                self.reset_state = action == 'reset-state'
                self.restart = True;self.restart_same_core = False
                self.send('save-quit')
            elif action in ("ds:blow","ds:lid") or action.startswith("slot-") or action in ("save", "load", "quit", "fast-forward") or action.startswith(('shader:','graphics:','filter2d:')) or action in ('fps:0','fps:1','threaded:0','threaded:1','adaptive:0','adaptive:1'):
                self.send(action)
            elif action == "force-quit":
                self.switch_target = None
                self.restart = False
                self.force_quit()
        if self.child.poll() is not None:
            self.publish("home")
            self.close_menu()
            return
        if self.pending:
            result = read_fields(Path(str(self.control) + ".result"))
            if result.get("token") == self.pending["token"]:
                state = result.get("state", "")
                if state in ('ds-applied','ds-unavailable'):
                    self.pending=None
                    if state=='ds-applied':self.close_menu()
                    else:
                        if not self.live:self.pause()
                        self.publish('error','DS controls are unavailable for this engine.')
                    return
                if state == 'saved' and not self.has_checkpoint():
                    state = 'save-failed'
                if state in ("saved", "loaded", "load-dispatched", "save-timeout", "save-failed", "load-failed", 'fast-forward-on', 'fast-forward-off', 'fast-forward-unavailable','shader-applied','shader-failed','shader-restart-required','fps-on','fps-off','threaded-on','threaded-off','threaded-failed','graphics-applied','graphics-failed','filter2d-applied','filter2d-failed','adaptive-on','adaptive-off','adaptive-failed'):
                    if state == "saved" and self.pending["action"] in ("quit", "save-quit"):
                        pass  # Still wait for the actual engine exit.
                    else:
                        action = self.pending["action"]
                        if self.switch_target and state in ('save-timeout', 'save-failed'):
                            self.switch_target = None
                        if self.reset_state and state in ("save-timeout","save-failed"):
                            self.restart = False;self.reset_state = False
                        self.pending = None
                        if self.slot_operation:
                            operation=self.slot_operation;self.slot_operation=None
                            try:
                                if state!='saved':raise ValueError('Current progress could not be saved. No slot was changed.')
                                vault=self.vault()
                                if operation['operation']=='slot-save':vault.store(operation['slot'],operation['overwrite'],self.folder/'menu-background.bmp')
                                else:
                                    if operation['operation']=='slot-load':vault.stage_load(operation['slot'])
                                    else:vault.undo_load()
                                    self.send('load');return
                            except (OSError,ValueError) as error:
                                if not self.live:self.pause()
                                self.publish('error',str(error));return
                        if not self.live:self.pause()
                        if state.startswith('fast-forward-'):self.fast_forward=state.endswith('-on')
                        if state in ('fps-on','fps-off'):
                            self.fps=state=='fps-on'
                            try:write_fields(self.data/'performance.ini',dict(schema=1,fps=int(self.fps)))
                            except OSError:state='fps-unsaved'
                        if state in ('threaded-on','threaded-off'):
                            self.threaded = state == 'threaded-on'
                            try:
                                path=self.data/'library'/game_key(self.rom)/'game.ini'
                                settings=read_fields(path);settings['threaded_3d']=int(self.threaded)
                                write_fields(path,settings)
                            except OSError:state='threaded-unsaved'
                        if state in ('adaptive-on','adaptive-off'):
                            try:
                                path=self.data/'library'/game_key(self.rom)/'game.ini'
                                settings=read_fields(path);settings.update(rgds_adaptive=int(state=='adaptive-on'))
                                write_fields(path,settings)
                            except OSError:state='adaptive-unsaved'
                        if state=='filter2d-applied':
                            mode=int(action.split(':')[1]);self.active_filter2d=str(mode)
                            try:
                                path=self.data/'library'/game_key(self.rom)/'game.ini'
                                settings=read_fields(path);settings.update(rgds_2d=mode)
                                write_fields(path,settings)
                            except OSError:state='filter2d-unsaved'
                        if state=='graphics-applied':
                            scale,smooth,skip=map(int,action.split(':')[1:])
                            self.active_scale=str(scale);self.active_smooth=str(smooth)
                            try:
                                path=self.data/'library'/game_key(self.rom)/'game.ini'
                                settings=read_fields(path);settings.update(rgds_scale=scale,rgds_smooth=smooth,frameskip=skip)
                                write_fields(path,settings)
                            except OSError:state='graphics-unsaved'
                        ok = state in ("saved", "loaded", "load-dispatched", 'fast-forward-on', 'fast-forward-off','shader-applied','fps-on','fps-off','fps-unsaved','threaded-on','threaded-off','threaded-unsaved','graphics-applied','graphics-unsaved','filter2d-applied','filter2d-unsaved','adaptive-on','adaptive-off','adaptive-unsaved')
                        self.publish("paused" if ok or state in ('fast-forward-unavailable','shader-restart-required') else "error", "Checkpoint saved." if state == "saved" else "Checkpoint loaded." if state == "loaded"
                                     else "Load requested. Resume to check your game." if state == "load-dispatched"
                                     else 'Fast-forward enabled.' if state == 'fast-forward-on' else 'Normal speed.' if state == 'fast-forward-off'
                                     else 'Fast-forward is unavailable for this emulator.' if state == 'fast-forward-unavailable'
                                     else 'Graphics applied. Your game keeps running.' if state=='graphics-applied'
                                     else 'Graphics applied for this session. Settings storage is unavailable.' if state=='graphics-unsaved'
                                     else 'Could not apply graphics. The previous renderer is still active.' if state=='graphics-failed'
                                     else '2D filter applied. Your game keeps running.' if state=='filter2d-applied'
                                     else '2D filter applied for this session. Settings storage is unavailable.' if state=='filter2d-unsaved'
                                     else 'Could not apply the 2D filter. Your game is unchanged.' if state=='filter2d-failed'
                                     else 'Heavy 3D scenes changed. Your game keeps running.' if state in ('adaptive-on','adaptive-off')
                                     else 'Heavy 3D scenes changed for this session. Settings storage is unavailable.' if state=='adaptive-unsaved'
                                     else 'Could not change heavy 3D scenes. Your game is unchanged.' if state=='adaptive-failed'
                                     else '3D threading changed. No restart needed.' if state in ('threaded-on','threaded-off')
                                     else '3D threading changed for this session. Settings storage is unavailable.' if state=='threaded-unsaved'
                                     else 'Could not change 3D threading. Your game is unchanged.' if state=='threaded-failed'
                                     else 'FPS changed for this session. Settings storage is unavailable.' if state=='fps-unsaved'
                                     else 'FPS counter enabled.' if state=='fps-on' else 'FPS counter hidden.' if state=='fps-off'
                                     else 'Shader applied.' if state=='shader-applied' else 'Could not apply this shader.' if state=='shader-failed'
                                     else 'Shader saved. Restart this game to apply it.' if state=='shader-restart-required'
                                     else "Could not " + ("load" if action == "load" else "save") + ". Resume, retry, or return without saving.")
            if self.pending and time.monotonic() - self.pending["began"] > (45 if self.pending["action"].startswith("graphics:") else 12):
                self.pending = None;self.slot_operation=None;self.switch_target=None
                if self.reset_state:self.restart=False;self.reset_state=False
                if not self.live:self.pause()
                self.publish("error", "The emulator did not finish. Resume, retry, or return without saving.")
        elif self.menu.poll() is not None:
            print(f'RGDS menu: helper {self.menu.pid} exited with code {self.menu.poll()}; presented={int(self.direct_ready if not self.live else self.overlay_ready)}.', file=sys.stderr, flush=True)
            self.restart = False
            self.close_menu()

    def park_menu_audio(self):
        """Release the UI sink before SIGSTOP, so the sleep owner can use it."""
        from sleep_sounds import effects_volume
        if self.menu is None or self.menu.poll() is not None or not effects_volume(self.data):
            return False
        ready = self.folder / 'game-menu.audio-sleep.ready'
        ready.unlink(missing_ok=True)
        (self.folder / 'game-menu.wake').unlink(missing_ok=True)
        (self.folder / 'game-menu.audio-sleep').touch()
        deadline = time.monotonic() + .35
        while not ready.exists() and self.menu.poll() is None and time.monotonic() < deadline:
            time.sleep(.005)
        return True

    def renderer_restart(self, action):
        """Native <-> 2x-5x needs the other binary: save the choice, checkpoint, relaunch."""
        try:
            scale, smooth, skip = map(int, action.split(':')[1:])
        except ValueError:
            return False
        if self.core != 'rgds-experimental' or not (1 <= scale <= 5 and 0 <= smooth <= 1 and 0 <= skip <= 3):
            return False
        if (scale > 1) == self.launch_hires:
            return False
        try:
            path = self.data/'library'/game_key(self.rom)/'game.ini'
            settings = read_fields(path);settings.update(rgds_scale=scale, rgds_smooth=smooth, frameskip=skip)
            write_fields(path, settings)
        except OSError:
            self.publish('error', 'Could not save the new resolution. Check storage.')
            return True
        self.switch_target = None;self.reset_state = False;self.restart = True
        # Same engine, other renderer binary: never re-resolve the emulator
        # from the per-game/default choice (that is for "Next emulator").
        self.restart_same_core = True
        self.send('save-quit')
        return True

    def sleep_for_lid(self):
        was_paused, was_blocked = self.paused, self.block.exists()
        paused_menu = None
        audio_requested = False
        try:
            self.block.touch()
            stop = self.inputs.speed.pause() if hasattr(self.inputs, 'speed') else None
            if stop:
                self.request_speed(stop)
            self.pause()
            if self.menu is not None and self.menu.poll() is None:
                audio_requested = self.park_menu_audio()
                self.menu.send_signal(signal.SIGSTOP)
                paused_menu = self.menu
            self.lid.hardware.sleep_lid()
        except (OSError, ValueError, subprocess.SubprocessError) as error:
            print('Lid sleep failed: ' + str(error), flush=True)
            if self.menu is not None:
                self.publish('error', 'Could not sleep. Reopen the lid and try again.')
        finally:
            try:
                if hasattr(self.inputs, 'discard_after_sleep'):
                    self.inputs.discard_after_sleep()
                self.trigger.unlink(missing_ok=True)
                self.home_trigger.unlink(missing_ok=True)
                self.game_menu_trigger.unlink(missing_ok=True)
                write_fields(Path(str(self.control) + '.resume'),
                             dict(monotonic=time.monotonic(), realtime=time.time()), durable=False)
            except OSError as error:
                print('Could not clear sleep input: ' + str(error), flush=True)
            finally:
                try:
                    (self.folder / 'game-menu.audio-sleep').unlink(missing_ok=True)
                    (self.folder / 'game-menu.audio-sleep.ready').unlink(missing_ok=True)
                    if paused_menu is not None or audio_requested:
                        (self.folder / 'game-menu.wake').touch()
                except OSError as error:
                    print('Could not restore menu audio: ' + str(error), flush=True)
                try:
                    if not was_blocked:
                        self.block.unlink(missing_ok=True)
                except OSError as error:
                    print('Could not restore sleep input gate: ' + str(error), flush=True)
                finally:
                    try:
                        if paused_menu is not None and paused_menu.poll() is None:
                            paused_menu.send_signal(signal.SIGCONT)
                    finally:
                        if not was_paused:
                            self.resume_engine()
            self.last_toggle = time.monotonic()

    def release_clocks(self):
        clocks, self.clocks = self.clocks, None
        if clocks:
            try:clocks.release()
            except (OSError, ValueError) as error:print('Clock floor not released: ' + str(error), file=sys.stderr, flush=True)

    def close(self):
        try:self.close_menu()
        finally:
            self.release_clocks()
            self.inputs.close()
            if self.overlay_path:self.overlay_path.unlink(missing_ok=True)
            if self.stats_path and self.stats_path.is_file():
                try:
                    stats=read_fields(self.stats_path)
                    stats.update(core=self.core,rom=str(self.rom))
                    write_fields(self.data/'logs'/(self.core+'-performance.ini'),stats)
                finally:self.stats_path.unlink(missing_ok=True)
