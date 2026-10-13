"""Visible clock/status changes repaint; identical polling results do not."""
import datetime
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from zoneinfo import ZoneInfo

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "build/host/rgds-launcher"


@unittest.skipUnless(BINARY.exists(), "Build the host launcher first")
class IdlePreviewTests(unittest.TestCase):
    def test_unchanged_clock_and_optional_status_fields_do_not_redraw_each_poll(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); data = root / "data"; data.mkdir()
            (data / "preferences.ini").write_text("schema=1\nmode=light\nfont=rounded\nanimations=0\n")
            (data / "system.ini").write_text("schema=1\npower=0\nbrightness=60\nidle=0\nlid=0\nbattery_percent=1\ntimezone=Europe/Oslo\n")
            report = root / "report.json"
            before = datetime.datetime.now(ZoneInfo("Europe/Oslo")).strftime("%d/%m  %H:%M")
            result = subprocess.run([str(BINARY), "--preview", "--home", "--data", str(data),
                                     "--seconds", "3", "--cache-report", str(report)], cwd=ROOT,
                                    env=dict(os.environ, SDL_VIDEODRIVER="dummy", LC_ALL="C"),
                                    capture_output=True, text=True, timeout=8)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            counts = json.loads(report.read_text())
            after = datetime.datetime.now(ZoneInfo("Europe/Oslo")).strftime("%d/%m  %H:%M")
            self.assertIn(counts["status_stamp"], (before, after), "Date is locale-stable DD/MM, time is HH:MM")
            self.assertLessEqual(counts["frames"], 4, "Stable status must not redraw every quarter second")
            self.assertEqual(counts["render_file_reads"], 0)


@unittest.skipUnless(BINARY.exists() and shutil.which("clang++") and shutil.which("sdl2-config"), "Host SDL/C++ required")
class IdleHardwareTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(); root = Path(cls.temp.name)
        source = root / "display.cpp"; cls.library = root / "display.dylib"
        source.write_text(r'''#include <SDL.h>
#include <dlfcn.h>
extern "C" Uint32 SDL_GetTicks(){auto fn=reinterpret_cast<decltype(&SDL_GetTicks)>(dlsym(RTLD_NEXT,"SDL_GetTicks"));return fn()*4;}
extern "C" int SDL_GetNumVideoDisplays(){return 2;}
extern "C" const char* SDL_GetDisplayName(int index){return index?"DSI-2":"DSI-1";}
extern "C" int SDL_GetDisplayBounds(int index,SDL_Rect* rect){*rect={0,index*480,640,480};return 0;}
extern "C" SDL_Window* SDL_CreateWindow(const char* name,int,int,int width,int height,Uint32){auto fn=reinterpret_cast<decltype(&SDL_CreateWindow)>(dlsym(RTLD_NEXT,"SDL_CreateWindow"));return fn(name,SDL_WINDOWPOS_UNDEFINED,SDL_WINDOWPOS_UNDEFINED,width,height,SDL_WINDOW_HIDDEN);}
extern "C" SDL_Renderer* SDL_CreateRenderer(SDL_Window* window,int,Uint32){auto fn=reinterpret_cast<decltype(&SDL_CreateRenderer)>(dlsym(RTLD_NEXT,"SDL_CreateRenderer"));return fn(window,-1,SDL_RENDERER_SOFTWARE);}
extern "C" int SDL_PollEvent(SDL_Event*){return 0;}
extern "C" int SDL_WaitEventTimeout(SDL_Event*,int){SDL_Delay(5);return 0;}
''')
        flags = subprocess.check_output(["sdl2-config", "--cflags", "--libs"], text=True).split()
        subprocess.run(["clang++", "-std=c++17", "-shared", "-fPIC", str(source), *flags,
                        "-o", str(cls.library)], check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def exercise(self, charging=False, game_menu=False):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); app = root / "app"; data = root / "data"
            runtime = app / "runtime"; runtime.mkdir(parents=True); data.mkdir()
            (app / "assets").symlink_to(ROOT / "assets", target_is_directory=True)
            (app / "themes").symlink_to(ROOT / "themes", target_is_directory=True)
            (data / "preferences.ini").write_text("schema=1\nmode=light\nfont=rounded\nanimations=0\n")
            (runtime / "system_controls.py").write_text('''import sys
from pathlib import Path
data=Path(sys.argv[sys.argv.index('--data')+1])
with (data/'status-calls').open('a') as f:f.write('status\\n')
print('brightness_supported=0\\npower_supported=0\\nsleep_supported=0\\nlid_supported=0\\nbrightness=60\\nbattery=82\\ncharging=' + ''' + repr("1" if charging else "0") + ''' + '\\nvolume_supported=0\\nvolume=25\\npoweroff_supported=0')
''')
            # Intentionally omit optional fields that status-bar/UI reads use.
            (runtime / "device_services.py").write_text('''import sys
from pathlib import Path
data=Path(sys.argv[sys.argv.index('--data')+1]);(data/'device').mkdir(exist_ok=True)
(data/'device/status.ini').write_text('state=ready\\nwifi=1\\nwifi_signal=90\\n')
''')
            report = root / "report.json"
            if game_menu:
                (data / "session").mkdir()
                (data / "session/game-menu.status").write_text("state=ready\nlive=1\n")
            result = subprocess.run([str(BINARY), "--game-menu" if game_menu else "--home", "--app", str(app), "--data", str(data),
                                     "--seconds", "3" if charging else "12", "--cache-report", str(report)],
                                    cwd=ROOT, env=dict(os.environ, RGDS_SDL_LIBRARY=str(self.library),
                                                      SDL_VIDEODRIVER="dummy", RGDS_SYSTEM_MANAGED="1"),
                                    capture_output=True, text=True, timeout=8)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            counts = json.loads(report.read_text())
            if not charging:
                self.assertGreaterEqual(len((data / "status-calls").read_text().splitlines()), 2,
                                        "Exercise a real asynchronous repeated hardware status result")
            return counts

    def test_identical_async_hardware_status_does_not_trigger_redraw(self):
        self.assertLessEqual(self.exercise()["frames"], 4)

    def test_charging_repaints_only_changed_bar_phases(self):
        frames = self.exercise(True)["frames"]
        self.assertGreaterEqual(frames, 6, "Charging animation must remain visible")
        self.assertLessEqual(frames, 10, "Only the four-phase 420 ms animation needs redraws")

    def test_unchanged_native_game_menu_status_does_not_redraw_each_poll(self):
        self.assertLessEqual(self.exercise(game_menu=True)["frames"], 4)


if __name__ == "__main__":
    unittest.main()
