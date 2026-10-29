"""PortMaster ports in the runtime (docs/PORTMASTER_PLAN.md): our files in PortMaster's folder,
what a port script sees when it sources our control.txt, launch plans, and the engine a port gets."""
import io
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import unittest.mock
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "runtime"))
import emulators
import play_options
import portmaster


class PortMasterTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.base = Path(temp.name)
        self.folders = portmaster.Folders(self.base / "portmaster")
        self.data = self.base / "data"
        control = self.folders.control
        control.mkdir(parents=True)
        (control / "harbourmaster").write_text("#!/usr/bin/env python3\n")
        (control / "version").write_text("2026.10.24-1611\n")
        # Upstream's generic control.txt, as a fresh install or update leaves it.
        (control / "control.txt").write_text('#!/bin/bash\nexport directory="roms"\n')
        # Stand-ins for the upstream files our control.txt sources.
        (control / "device_info.txt").write_text('export DEVICE_ARCH=aarch64 CFW_NAME=Buildroot\n')
        (control / "funcs.txt").write_text('pm_finish() { :; }\n')
        self.port = self.folders.ports / "Cats on Mars.sh"
        self.folders.ports.mkdir(parents=True)
        self.port.write_text("#!/bin/bash\n")

    def test_ensure_writes_our_files_and_keeps_upstreams_control(self):
        self.assertTrue(portmaster.ensure(ROOT, self.folders))
        control = (self.folders.control / "control.txt").read_text()
        self.assertIn(portmaster.MARK, control)
        self.assertIn('export controlfolder="%s"' % self.folders.control, control)
        self.assertIn('export directory="%s"' % str(self.folders.root).lstrip("/"), control)
        self.assertEqual(
            (self.folders.control / "control.upstream.txt").read_text(), '#!/bin/bash\nexport directory="roms"\n'
        )
        self.assertIn("GODOT_OPTS=", (self.folders.control / "mod_Buildroot.txt").read_text())
        self.assertEqual((self.folders.control / "gamecontrollerdb.rgds.txt").read_text(), portmaster.PAD + "\n")
        self.assertEqual(portmaster.read_fields(self.folders.record)["version"], "2026.10.24-1611")
        # Nothing to do the second time; an update that puts upstream's back is noticed again.
        self.assertFalse(portmaster.ensure(ROOT, self.folders))
        (self.folders.control / "control.txt").write_text('#!/bin/bash\nexport directory="roms2"\n')
        self.assertTrue(portmaster.ensure(ROOT, self.folders))
        self.assertIn("roms2", (self.folders.control / "control.upstream.txt").read_text())
        for folder in (self.folders.ports, self.folders.home, self.folders.autoinstall):
            self.assertTrue(folder.is_dir())

    @unittest.skipUnless(shutil.which("bash"), "needs bash")
    def test_a_port_script_sees_our_folders_and_pad(self):
        portmaster.ensure(ROOT, self.folders)
        script = (
            'source "$1/control.txt"\n'
            '[ -f "$1/mod_${CFW_NAME}.txt" ] && source "$1/mod_${CFW_NAME}.txt"\n'
            'get_controls\n'
            'echo "GAMEDIR=/$directory/ports/catsonmars"\n'
            'echo "ESUDO=[$ESUDO]"\n'
            'echo "PAD=$sdl_controllerconfig"\n'
            'echo "GPTOKEYB=$GPTOKEYB"\n'
            'echo "GODOT=$GODOT_OPTS"\n'
        )
        result = subprocess.run(
            ["bash", "-c", script, "port", str(self.folders.control)], capture_output=True, text=True, check=True
        )
        lines = dict(line.split("=", 1) for line in result.stdout.splitlines())
        self.assertEqual(lines["GAMEDIR"], str(self.folders.ports / "catsonmars"))
        self.assertEqual(lines["ESUDO"], "[]")
        self.assertEqual(lines["PAD"], portmaster.PAD)
        self.assertEqual(lines["GPTOKEYB"].split()[0], str(self.folders.control / "gptokeyb"))
        self.assertRegex(lines["GODOT"], r"^--resolution \d+x\d+ -f$")

    @unittest.skipUnless(shutil.which("bash"), "needs bash")
    def test_westonwrap_is_told_rocknix_and_ports_are_not(self):
        # PortMaster's Weston runtime sources control.txt from westonwrap.sh: there (only) it gets ROCKNIX, whose
        # libmali mode draws into the running Weston. A port script keeps the card's own name.
        portmaster.ensure(ROOT, self.folders)
        probe = 'source "$1/control.txt"; echo "$CFW_NAME|${XKB_CONFIG_ROOT:-none}"'
        wrapper = self.base / "westonwrap.sh"
        wrapper.write_text(probe + "\n")
        port = subprocess.run(
            ["bash", "-c", probe, "Port.sh", str(self.folders.control)], capture_output=True, text=True, check=True
        ).stdout.strip()
        weston = subprocess.run(
            ["bash", str(wrapper), str(self.folders.control)], capture_output=True, text=True, check=True
        ).stdout.strip()
        self.assertEqual(port.split("|")[0], "Buildroot")
        self.assertEqual(weston.split("|")[0], "ROCKNIX")
        self.assertEqual(port.split("|")[1], "none")

    @unittest.skipUnless(shutil.which("bash"), "needs bash")
    def test_armhf_capability_only_with_the_32_bit_layer(self):
        portmaster.ensure(ROOT, self.folders)
        (self.folders.control / "device_info.txt").write_text(
            'export DEVICE_ARCH=aarch64 CFW_NAME=Buildroot DEVICE_HAS_ARMHF=N DEVICE_CAPABILITIES="aarch64 power"\n'
        )
        probe = 'source "$1/control.txt"; echo "$DEVICE_HAS_ARMHF|$DEVICE_CAPABILITIES"'
        run = lambda: subprocess.run(
            ["bash", "-c", probe, "Port.sh", str(self.folders.control)], capture_output=True, text=True, check=True
        ).stdout.strip()
        self.assertEqual(run(), "N|aarch64 power")
        loader = self.folders.root / "rgds/armhf/ld-linux-armhf.so.3"
        loader.parent.mkdir(parents=True)
        loader.write_bytes(b"loader")
        self.assertEqual(run(), "Y|aarch64 power armhf")

    def test_port_namespace_binds_point_at_our_portmaster(self):
        import port_runner
        binds = port_runner.prepare_binds(self.folders.root)
        home = self.folders.home / ".local/share/PortMaster"
        self.assertTrue(home.is_dir())  # made inside our own folder
        self.assertIn((str(self.folders.control).encode(), str(home.resolve()).encode()), binds)
        # /roms/ports/PortMaster is bound only where it exists; it is never created on the system disk.
        roms = Path("/roms/ports/PortMaster")
        self.assertEqual(any(target == str(roms.resolve()).encode() for _s, target in binds), roms.is_dir())

    def test_unpack_takes_only_the_portmaster_folder(self):
        good, bad = self.base / "good.zip", self.base / "bad.zip"
        with zipfile.ZipFile(good, "w") as zf:
            zf.writestr("PortMaster/version", "1\n")
        with zipfile.ZipFile(bad, "w") as zf:
            zf.writestr("PortMaster/version", "1\n")
            zf.writestr("../escape", "x")
        target = self.base / "unpacked"
        portmaster.unpack(good, target)
        self.assertEqual((target / "PortMaster/version").read_text(), "1\n")
        with self.assertRaises(ValueError):
            portmaster.unpack(bad, self.base / "other")
        self.assertFalse((self.base / "escape").exists())

    def test_checked_rejects_a_wrong_digest(self):
        archive = self.base / "PortMaster.zip"
        archive.write_bytes(b"portmaster")
        import hashlib
        md5, sha = hashlib.md5(b"portmaster").hexdigest(), hashlib.sha256(b"portmaster").hexdigest()
        portmaster.checked(archive, md5, sha)
        with self.assertRaises(ValueError):
            portmaster.checked(archive, md5, "0" * 64)
        with self.assertRaises(ValueError):
            portmaster.checked(archive, "0" * 32, sha)

    def test_launch_plans(self):
        plan = portmaster.launch_plan(ROOT, self.data, self.port, {"PATH": "/usr/bin"}, folders=self.folders)
        self.assertEqual(plan["command"], ["bash", str(self.port)])
        self.assertEqual(plan["cwd"], str(self.folders.ports))
        env = plan["env"]
        self.assertEqual(env["XDG_DATA_HOME"], str(self.folders.root))
        self.assertEqual(env["HOME"], str(self.folders.home))
        self.assertEqual(env["HM_PORTS_DIR"], str(self.folders.ports))
        self.assertEqual(env["SDL_VIDEODRIVER"], "wayland")
        self.assertEqual(env["SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS"], "1")
        self.assertEqual(env["RGDS_PORT_STATUS"], str(self.data / "session/port-status.ini"))
        self.assertNotIn("LD_PRELOAD", env)
        entry = portmaster.entry_folder(ROOT) / portmaster.ENTRY
        self.assertTrue(entry.is_file())
        self.assertTrue(portmaster.is_entry(ROOT, entry))
        plan = portmaster.launch_plan(ROOT, self.data, entry, {}, folders=self.folders)
        self.assertEqual(plan["command"][2:], [str(ROOT / "runtime/portmaster.py"), "open", "--app", str(ROOT)])
        with self.assertRaises(ValueError):
            portmaster.launch_plan(ROOT, self.data, self.base / "game.zip", {}, folders=self.folders)

    def test_a_port_needs_portmaster_installed(self):
        (self.folders.control / "harbourmaster").unlink()
        with self.assertRaises(ValueError):
            portmaster.launch_plan(ROOT, self.data, self.port, {}, folders=self.folders)
        # The PortMaster entry installs it, so it can always start.
        entry = portmaster.entry_folder(ROOT) / portmaster.ENTRY
        self.assertIn("open", portmaster.launch_plan(ROOT, self.data, entry, {}, folders=self.folders)["command"])

    def test_engine_and_system_of_a_port(self):
        self.assertEqual(play_options.system_of(self.port), "ports")
        self.assertEqual(play_options.engine_for(self.port, "melonds"), "portmaster")
        self.assertEqual(play_options.engine_for(self.base / "game.nds", "melonds"), "rgds-experimental")  # DSCore only
        self.assertEqual(emulators.CORES.index("portmaster"), 6)
        self.assertEqual(emulators.resolve_rom(self.port, self.data, "portmaster"), self.port.resolve())
        with patch.object(portmaster, "Folders", return_value=self.folders):
            plan = emulators.launch_plan("portmaster", ROOT, self.data, self.port)
        self.assertEqual(plan["command"], ["bash", str(self.port)])

    def test_the_launcher_lists_the_entry_and_the_ports(self):
        self.assertEqual(portmaster.roots(ROOT, self.folders), [portmaster.entry_folder(ROOT), self.folders.ports])

    def test_artwork_from_the_ports_own_picture(self):
        self.port.write_text("#!/bin/bash\n# PORTMASTER: catsonmars.zip, Cats on Mars.sh\n")
        folder = self.folders.ports / "catsonmars"
        folder.mkdir()
        self.assertEqual(portmaster.port_folder(self.port, self.folders), folder)
        (self.folders.ports / "Other.sh").write_text("#!/bin/bash\n")
        self.assertIsNone(portmaster.port_folder(self.folders.ports / "Other.sh", self.folders))
        made = []
        from library import game_key
        with patch("scraper.image_to_bmp", lambda content, target: made.append((content, target))):
            # The PortMaster entry has PortMaster's icon before PortMaster is installed (RG DS Core's copy);
            # the port has no picture yet.
            self.assertEqual(portmaster.artwork(ROOT, self.data, self.folders), 1)
            entry = self.data / "library" / game_key(portmaster.entry_folder(ROOT) / portmaster.ENTRY) / "box.bmp"
            self.assertEqual(entry.read_bytes()[:2], b"BM")
            (folder / "screenshot.png").write_bytes(b"shot")
            (folder / "cover.png").write_bytes(b"cover")
            self.assertEqual(portmaster.artwork(ROOT, self.data, self.folders), 1)
        self.assertEqual(made, [(b"cover", self.data / "library" / game_key(self.port) / "box.bmp")])

    def test_panel_size_reads_the_top_panel(self):
        drm = self.base / "drm"
        (drm / "card0-DSI-1").mkdir(parents=True)
        (drm / "card0-DSI-1/modes").write_text("1024x768\n")
        self.assertEqual(portmaster.panel_size(drm), (1024, 768))
        self.assertEqual(portmaster.panel_size(self.base / "none"), (640, 480))

    def test_rgds_os_ports_draw_on_kms_in_their_own_folder(self):
        # RG DS OS (os/buildroot): no compositor, the data partition, the top panel on DSI-2.
        with unittest.mock.patch("linux_platform.rgds_os", return_value=True):
            self.assertEqual(portmaster.default_root(), Path("/data/portmaster"))
            self.assertEqual(portmaster.top_connector(), "DSI-2")
            env = portmaster.port_environment(
                {
                    "WAYLAND_DISPLAY": "wayland-1",
                    "SDL_KMSDRM_FIRST": "DSI-2"
                }, ROOT, self.folders
            )
        self.assertEqual(env["SDL_VIDEODRIVER"], "kmsdrm")
        for key in ("WAYLAND_DISPLAY", "SDL_KMSDRM_FIRST", "SDL_VIDEO_WAYLAND_WMCLASS"):
            self.assertNotIn(key, env)
        with unittest.mock.patch("linux_platform.rgds_os", return_value=False):
            self.assertEqual(portmaster.default_root(), Path("/mnt/mmc/portmaster"))
            self.assertEqual(portmaster.top_connector(), "DSI-1")
        # PortMaster names the mod file after os-release's NAME there.
        self.assertEqual(
            portmaster.overlay(ROOT, self.folders)["mod_RG DS OS.txt"],
            portmaster.overlay(ROOT, self.folders)["mod_Buildroot.txt"]
        )

    def test_status_writes_message_percent_and_detail(self):
        path = self.data / "session/port-status.ini"
        path.parent.mkdir(parents=True)
        with patch("sys.stdout", io.StringIO()):
            portmaster.Status(path)("Downloading PortMaster", 42)
            self.assertEqual(portmaster.read_fields(path)["percent"], "42")
            portmaster.Status(path)("Could not set up PortMaster", -1, "no network")
        self.assertEqual(portmaster.read_fields(path)["detail"], "no network")


if __name__ == "__main__":
    unittest.main()
