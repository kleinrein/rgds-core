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
        binary = self.app / "emulators/dsperate/dsperate"
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
            return launch_plan("dsperate", self.app, self.data, self.rom)

    def test_native_bridge_preload_and_shared_menu(self):
        plan = self.plan()
        self.assertEqual(plan["env"]["LD_PRELOAD"], str(self.hooks / "librgdsbridge.so"))
        self.assertEqual(plan["env"]["RGDS_SHARED_MENU"], "1")
        self.assertEqual(plan["env"]["RGDS_STATE_SLOT"], str(self.data / "emulators/dsperate/states/RGDS.0.dss"))

    def test_launch_cleans_stale_environment(self):
        plan = self.plan(LD_PRELOAD="stale.so", LD_LIBRARY_PATH="/old/lib", PATH="/old/bin")
        self.assertEqual(plan["env"]["LD_PRELOAD"], str(self.hooks / "librgdsbridge.so"))
        self.assertNotIn("/old/lib", plan["env"].get("LD_LIBRARY_PATH", ""))
        self.assertEqual(plan["env"]["PATH"], "/usr/sbin:/usr/bin:/sbin:/bin")

    def test_hooks_select_only_the_bridge(self):
        for core in ("drastic", "dsperate"):
            with self.subTest(core=core):
                selected = select_hooks(core, self.hooks, {})
                self.assertEqual(selected.bridge, self.hooks / "librgdsbridge.so")
                self.assertEqual(len(selected), 1)
                self.assertIsNone(select_hooks(core, self.hooks, {"RGDS_DISABLE_HOOKS": "1"}).bridge)

    def test_owned_profile_enables_jit_and_has_no_duplicate_managed_hotkeys(self):
        self.plan()
        config = configparser.ConfigParser()
        config.read(self.data / "emulators/dsperate/config.ini")
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
        path = self.data / "emulators/dsperate/config.ini"
        path.parent.mkdir(parents=True)
        path.write_text("[emu]\njit=false\n[video]\ncustom=preserved\n")
        self.plan()
        config = configparser.ConfigParser()
        config.read(path)
        self.assertFalse(config.getboolean("emu", "jit"))
        self.assertEqual(config["video"]["custom"], "preserved")

    def test_native_per_game_input_overrides_cannot_shadow_canonical_controls(self):
        folder = self.data / 'emulators/dsperate/home/.config/dsperate/games'
        folder.mkdir(parents=True)
        for name in ('RGDS.ini', 'fixture.ini'):
            (folder / name).write_text(
                '[pad]\nup=none\na=b\nstylus_button=a\n[hotkeys]\npause=a\nsave_state=none\n[padhotkeys]\nscreen_swap=a\n[emu]\njit=false\n[video]\ncustom=preserved\n'
            )
        save = self.data / 'emulators/dsperate/saves/game.sav'
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

    def test_drastic_resets_inherited_screen_swap_and_both_hotkey_banks(self):
        binary = self.app / 'emulators/drastic/drastic'
        binary.parent.mkdir(parents=True)
        binary.write_text('#!/bin/sh\nexit 0\n')
        binary.chmod(0o755)
        config = binary.parent / 'config/drastic.cfg'
        config.parent.mkdir()
        bindings = {
            f'controls_{bank}[CONTROL_INDEX_{action}]': '1024'
            for bank in ('a', 'b')
            for action in ('SWAP_SCREENS', 'SWAP_ORIENTATION_A', 'SWAP_ORIENTATION_B')
        }
        config.write_text('screen_swap=1\ncustom_option=keep\n' + ''.join(f'{k}={v}\n' for k, v in bindings.items()))
        save = self.data / 'emulators/drastic/backup/game.dsv'
        save.parent.mkdir(parents=True)
        save.write_bytes(b'existing save')
        with patch.dict(os.environ, {}, clear=True):
            emulators.launch_plan('drastic', self.app, self.data, self.rom)
        fields = read_fields(self.data / 'emulators/drastic/config/drastic.cfg')
        self.assertEqual(fields['screen_swap'], '0')
        self.assertEqual(fields['custom_option'], 'keep')
        for key in bindings:
            self.assertEqual(fields[key], '65535')
        self.assertEqual(save.read_bytes(), b'existing save')

    def test_hooks_disabled_drastic_restores_stock_dpad_after_a_mapped_launch(self):
        binary = self.app / 'emulators/drastic/drastic'
        binary.parent.mkdir(parents=True)
        binary.write_text('#!/bin/sh\nexit 0\n')
        binary.chmod(0o755)
        config = binary.parent / 'config/drastic.cfg'
        config.parent.mkdir()
        config.write_text('controls_a[CONTROL_INDEX_UP]=1537\n')
        with patch.dict(os.environ, {}, clear=True):
            emulators.launch_plan('drastic', self.app, self.data, self.rom)
        home = self.data / 'emulators/drastic/config/drastic.cfg'
        self.assertEqual(read_fields(home)['controls_a[CONTROL_INDEX_UP]'], '1044')
        with patch.dict(os.environ, {'RGDS_DISABLE_HOOKS': '1'}, clear=True):
            emulators.launch_plan('drastic', self.app, self.data, self.rom)
        self.assertEqual(read_fields(home)['controls_a[CONTROL_INDEX_UP]'], '1537')
        self.assertNotIn('controls_b[CONTROL_INDEX_RIGHT]', read_fields(home))

    def test_drastic_screen_choice_overrides_native_game_profile_without_touching_saves(self):
        binary = self.app / 'emulators/drastic/drastic'
        binary.parent.mkdir(parents=True)
        binary.write_text('#!/bin/sh\nexit 0\n')
        binary.chmod(0o755)
        folder = self.data / 'emulators/drastic/config'
        folder.mkdir(parents=True)
        native = folder / (self.rom.stem + '.cfg')
        native.write_text('screen_swap = 1\ncustom_option = keep\ncontrols_b[CONTROL_INDEX_SWAP_SCREENS] = 1024\n')
        unrelated = folder / 'other.cfg'
        unrelated.write_text('screen_swap=1\ncustom_option=other\n')
        save = self.data / 'emulators/drastic/savestates/fixture_1.dss'
        save.parent.mkdir()
        save.write_bytes(b'keep checkpoint')
        for selected in (None, 1, 0):
            with self.subTest(selected=selected):
                if selected is not None:
                    write_fields(
                        self.data / 'library' / game_key(self.rom) / 'game.ini', {'drastic_screen_swap': selected}
                    )
                with patch.dict(os.environ, {}, clear=True):
                    emulators.launch_plan('drastic', self.app, self.data, self.rom)
                for path in (native, folder / 'drastic.cfg'):
                    values = read_fields(path)
                    self.assertEqual(values['screen_swap'], str(selected or 0))
                    self.assertEqual(values['controls_b[CONTROL_INDEX_SWAP_SCREENS]'], '65535')
                self.assertEqual(read_fields(native)['custom_option'], 'keep')
                self.assertEqual(save.read_bytes(), b'keep checkpoint')
                self.assertEqual(unrelated.read_text(), 'screen_swap=1\ncustom_option=other\n')

    def test_drastic_refuses_to_destroy_binary_per_game_configuration(self):
        from emulator_profiles import drastic_game_profile
        home = self.data / 'emulators/drastic'
        folder = home / 'config'
        folder.mkdir(parents=True)
        native = folder / (self.rom.stem + '.cfg')
        original = b'DSCF\x00\xffbinary options'
        native.write_bytes(original)
        with self.assertRaisesRegex(ValueError, 'binary per-game config'):
            drastic_game_profile(home, self.rom, dict(threaded=1, hires=0, frameskip=0, drastic_screen_swap=0))
        self.assertEqual(native.read_bytes(), original)

    def test_drastic_invalid_screen_choice_defaults_to_normal(self):
        from emulator_profiles import game_options
        path = self.data / 'library' / game_key(self.rom) / 'game.ini'
        for value in ('invalid', '2', '-1'):
            write_fields(path, {'drastic_screen_swap': value})
            self.assertEqual(game_options(self.data, self.rom)['drastic_screen_swap'], 0)

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

    def test_dsperate_never_launches_an_nnddss_binary_when_missing(self):
        (self.app / "emulators/dsperate/dsperate").unlink()
        other = self.root / "installed-nnddss"
        other.mkdir()
        (other / "nnddss").write_bytes(b"fixture")
        (other / "nnddss").chmod(0o755)
        # A missing DSperate used to use the NNDDSS-only fallback branch.
        with patch.object(emulators, "Path", wraps=Path) as paths:
            paths.side_effect = lambda value: other / "nnddss" if str(value).endswith("/nnddss/nnddss") else Path(value)
            self.assertIsNone(emulators.find_binary("dsperate", self.app))

    def test_drastic_skips_a_stock_arm32_binary_for_aarch64_oga(self):
        stock = self.root / "drastic64"
        stock.mkdir()
        for name, identity in (("drastic", b"\x01\x01"), ("drastic_oga", b"\x02\x01")):
            binary = stock / name
            machine = b"\x28\x00" if name == "drastic" else b"\xb7\x00"
            binary.write_bytes(b"\x7fELF" + identity + bytes(12) + machine)
            binary.chmod(0o755)
        with patch.object(emulators, "STOCK_DIRS", (stock,)), patch.dict(os.environ, {}, clear=True):
            self.assertEqual(emulators.find_binary("drastic", self.app), (stock / "drastic_oga").resolve())

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
