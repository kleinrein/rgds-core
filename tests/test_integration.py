"""Native bridge selection and DSperate launch behavior, without running cores."""
import configparser
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "runtime"))
from integration import select_hooks, engine_libraries
import emulators
from emulators import launch_plan
from audio import EVENT, Mixer, Volume, VolumeWatch
from library import read_fields, write_fields, game_key


class IntegrationTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.app = self.root / "app"
        self.data = self.root / "userdata"
        self.hooks = self.app / "emulators/hooks"
        self.hooks.mkdir(parents=True)
        (self.hooks / "librgdsbridge.so").write_bytes(b"not executed")
        binary = self.app / "emulators/rgds-experimental/rgds-experimental"
        binary.parent.mkdir()
        binary.write_bytes(b"not executed")
        binary.chmod(0o755)
        profile = self.app / "assets/emulator-profiles/dsperate.ini"
        profile.parent.mkdir(parents=True)
        shutil.copyfile(ROOT / "assets/emulator-profiles/dsperate.ini", profile)
        self.rom = self.root / "fixture.nds"
        content = bytearray(128)
        content[12:16] = b"RGDS"
        self.rom.write_bytes(content)

    def plan(self, **environment):
        with patch.dict(os.environ, environment, clear=True):
            return launch_plan("rgds-experimental", self.app, self.data, self.rom)

    def test_native_bridge_preload_and_shared_menu(self):
        plan = self.plan()
        self.assertEqual(plan["env"]["LD_PRELOAD"], str(self.hooks / "librgdsbridge.so"))
        self.assertEqual(plan["env"]["RGDS_SHARED_MENU"], "1")
        self.assertEqual(plan["env"]["RGDS_STATE_SLOT"], str(self.data / "emulators/rgds-experimental/states/RGDS.0.dss"))

    def test_launch_cleans_stale_environment(self):
        plan = self.plan(LD_PRELOAD="stale.so", LD_LIBRARY_PATH="/old/lib", PATH="/old/bin")
        self.assertEqual(plan["env"]["LD_PRELOAD"], str(self.hooks / "librgdsbridge.so"))
        self.assertNotIn("/old/lib", plan["env"].get("LD_LIBRARY_PATH", ""))
        self.assertEqual(plan["env"]["PATH"], "/usr/sbin:/usr/bin:/sbin:/bin")

    def test_hooks_select_only_the_bridge(self):
        for core in ("rgds-experimental", "rgds-play"):
            with self.subTest(core=core):
                selected = select_hooks(core, self.hooks, {})
                self.assertEqual(selected.bridge, self.hooks / "librgdsbridge.so")
                self.assertEqual(len(selected), 1)
                self.assertIsNone(select_hooks(core, self.hooks, {"RGDS_DISABLE_HOOKS": "1"}).bridge)

    def test_owned_profile_enables_jit_and_has_no_duplicate_managed_hotkeys(self):
        self.plan()
        config = configparser.ConfigParser()
        config.read(self.data / "emulators/rgds-experimental/config.ini")
        self.assertTrue(config.getboolean("emu", "jit"))
        self.assertEqual(config["audio"]["buffer_size"], "auto")
        self.assertEqual(config["video"]["sink"], "renderer")
        self.assertEqual(config["hotkeys"]["fast_forward_toggle"], "Tab")
        for name in (
            "pause", "quit", "fast_forward", "fast_forward_toggle", "save_state", "load_state", "volume_up",
            "volume_down"
        ):
            self.assertEqual(config["padhotkeys"][name], "none")

    def test_existing_tuning_and_save_policy_survive_owned_profile_migration(self):
        path = self.data / "emulators/rgds-experimental/config.ini"
        path.parent.mkdir(parents=True)
        path.write_text("[emu]\njit=false\n[video]\ncustom=preserved\n")
        self.plan()
        config = configparser.ConfigParser()
        config.read(path)
        self.assertFalse(config.getboolean("emu", "jit"))
        self.assertEqual(config["video"]["custom"], "preserved")

    def test_native_per_game_input_overrides_cannot_shadow_canonical_controls(self):
        folder = self.data / 'emulators/rgds-experimental/home/.config/dsperate/games'
        folder.mkdir(parents=True)
        for name in ('RGDS.ini', 'fixture.ini'):
            (folder / name).write_text(
                '[pad]\nup=none\na=b\nstylus_button=a\n[hotkeys]\npause=a\nsave_state=none\n[padhotkeys]\nscreen_swap=a\n[emu]\njit=false\n[video]\ncustom=preserved\n'
            )
        save = self.data / 'emulators/rgds-experimental/saves/game.sav'
        save.parent.mkdir(parents=True)
        save.write_bytes(b'keep this battery save')
        self.plan()
        for name in ('RGDS.ini', 'fixture.ini'):
            config = configparser.ConfigParser()
            config.read(folder / name)
            self.assertEqual(config['pad']['up'], 'dpup')
            self.assertEqual(config['pad']['a'], 'a')
            self.assertEqual(config['pad']['stylus_button'], 'rightstick')
            self.assertEqual(config['hotkeys']['pause'], 'none')
            self.assertEqual(config['hotkeys']['save_state'], 'F5')
            self.assertEqual(config['padhotkeys']['screen_swap'], 'none')
            self.assertFalse(config.getboolean('emu', 'jit'))
            self.assertEqual(config['video']['custom'], 'preserved')
        self.assertEqual(save.read_bytes(), b'keep this battery save')

    def test_engine_and_bridge_resolve_the_same_explicit_sdl_library(self):
        private = self.root / "private"
        private.mkdir()
        stock = self.root / "stock"
        stock.mkdir()
        selected = stock / "libSDL2-2.0.so.0"
        selected.write_bytes(b"one SDL instance")
        env = {"RGDS_SDL_LIBRARY": str(selected)}
        engine_libraries(env, private=(private,))
        self.assertEqual(env["LD_LIBRARY_PATH"].split(":")[:2], [str(stock), str(private)])
        self.assertEqual(env["RGDS_SDL_LIBRARY"], str(selected))

    def test_missing_sdl_pin_is_replaced_with_the_engine_library(self):
        private = self.root / "private"
        private.mkdir()
        selected = private / "libSDL2-2.0.so.0"
        selected.write_bytes(b"engine SDL")
        env = {"RGDS_SDL_LIBRARY": str(self.root / "removed/libSDL2.so")}
        engine_libraries(env, private=(private, private))
        self.assertEqual(env["RGDS_SDL_LIBRARY"], str(selected))
        self.assertEqual(env["LD_LIBRARY_PATH"].split(":").count(str(private)), 1)

    def test_previous_engine_environment_does_not_reuse_touch_or_menu_handles(self):
        plan = self.plan(RGDS_TOUCH_LIBRARY="old-touch.so", RGDS_CORE="drastic", RGDS_OVERLAY_FILE="old.frame")
        self.assertNotIn("RGDS_TOUCH_LIBRARY", plan["env"])
        self.assertNotIn("RGDS_OVERLAY_FILE", plan["env"])
        self.assertEqual(plan["env"]["RGDS_CORE"], "dsperate")

    def test_volume_key_updates_native_hud(self):
        class FakeMixer:
            def query(self):
                return 0

            def set(self, _level):
                pass

        volume = Volume(self.data, FakeMixer())
        watcher = VolumeWatch(self.data, volume)
        watcher.handle(EVENT.pack(0, 0, 1, 115, 1), 1)
        self.assertEqual(read_fields(self.data / "session/volume-hud.ini")["volume"], "5")


if __name__ == "__main__": unittest.main()
