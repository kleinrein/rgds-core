"""Optional startup warning, genuine fingers/raw captures and deferred boot work."""
import json
import os
import shutil
import socket
from pathlib import Path
import struct
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "build/host/rgds-launcher"


@unittest.skipUnless(BINARY.exists(), "Build host launcher first")
class BootWarningTests(unittest.TestCase):
    def run_boot(self, script="", events=None, frames=None, extra=(), helper=None, warning=True):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder).resolve()
            state = root / "state.txt";state.write_text('1 3 2 ""\n')
            (root / "ui.ini").write_text("schema=1\nhome_layout=carousel\nbackground=plain\n")   # the carousel home these scripts drive
            inputs = root / "input.txt";inputs.write_text(script)
            report = root / "report.json"
            screenshot = root / "warning.bmp"
            command = [str(helper) if helper else str(BINARY), "--preview", "--boot-warning" if warning else "--boot-startup", "--state", str(state),
                       "--preferences", str(root / "preferences.ini"), "--input-script", str(inputs),
                       "--cache-report", str(report), "--screenshot", str(screenshot)]
            command += ["--frames", str(frames)] if frames else ["--seconds", "1"]
            if events is not None:
                recording = root / "touch.events"
                recording.write_bytes(b"".join(struct.pack("<qqHHi", ms // 1000, (ms % 1000) * 1000, kind, code, value)
                                             for ms, kind, code, value in events))
                command += ["--touch-recording", str(recording)]
            result = subprocess.run([*command, *extra], executable=str(BINARY) if helper else None,
                                    cwd=ROOT, env=dict(os.environ, SDL_VIDEODRIVER="dummy"),
                                    capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertNotIn("Font fallback", result.stderr)
            return json.loads(report.read_text()), state.read_text(), screenshot.read_bytes()

    @staticmethod
    def raw_tap(ms, hold=20):
        return [(ms, 1, 330, 1), (ms, 3, 57, 0), (ms, 3, 53, 320), (ms, 3, 54, 250),
                (ms, 0, 2, 0), (ms, 0, 0, 0), (ms + hold, 1, 330, 0), (ms + hold, 0, 2, 0), (ms + hold, 0, 0, 0)]

    def test_first_warning_frame_defers_scan_and_preserves_saved_selection(self):
        for mode, font in (("light", "rounded"), ("dark", "pixel")):
            with self.subTest(mode=mode):
                report, state, image = self.run_boot(frames=1, extra=("--rom-dir", "/missing/cold-card", "--mode", mode, "--font", font))
                self.assertEqual(report["boot_pending"], 1)
                self.assertEqual(report["library_scan_started"], 0)
                self.assertEqual(report["library_ready"], 0)
                self.assertEqual(report["metadata_reads"], 0)
                self.assertEqual(state, '1 3 2 ""\n')
                offset = struct.unpack_from("<I", image, 10)[0]
                width, height = struct.unpack_from("<ii", image, 18)
                row = height - 1 - 94 if height > 0 else 94
                self.assertEqual(image[offset + (row * width + 320) * 4:offset + (row * width + 320) * 4 + 3], bytes((65, 178, 239)))

    def test_welcome_animates_without_scanning_or_accepting_for_the_user(self):
        report, _, _ = self.run_boot()
        self.assertGreater(report["frames"], 8)
        self.assertLess(report["frames"], 45, "Welcome motion is capped at 30 fps")
        self.assertEqual(report["boot_pending"], 1)
        self.assertEqual(report["library_scan_started"], 0)
        self.assertEqual(report["render_file_reads"], 0)

    def test_reduced_motion_welcome_stays_idle(self):
        with tempfile.TemporaryDirectory() as folder:
            preferences = Path(folder) / "preferences.ini"
            preferences.write_text("schema=1\nmode=light\nfont=rounded\nanimations=0\n")
            report, _, _ = self.run_boot(extra=("--preferences", str(preferences)))
            self.assertEqual(report["frames"], 1)
            self.assertEqual(report["boot_pending"], 1)

    def test_warning_has_no_timed_auto_continue(self):
        report, _, _ = self.run_boot()
        self.assertEqual(report["boot_pending"], 1)
        self.assertEqual(report["library_scan_started"], 0)

    def test_back_cannot_exit_warning_and_continue_still_works(self):
        report, _, _ = self.run_boot('20 key Escape\n40 key B\n60 joy-down 1\n80 joy-up 1\n120 key A\n140 key-up A\n')
        self.assertEqual(report['boot_pending'], 0)

    def test_warning_disabled_home_first_frame_defers_all_scan_work(self):
        report, state, _ = self.run_boot(frames=1, warning=False, extra=("--rom-dir", "/missing/cold-card"))
        self.assertEqual(report["page"], 0)
        self.assertEqual(report["boot_pending"], 0)
        self.assertEqual(report["library_scan_started"], 0)
        self.assertEqual(report["library_ready"], 0)
        self.assertEqual(report["metadata_reads"], 0)
        self.assertEqual(state, '1 3 2 ""\n')

    def test_pixel_first_frame_needs_no_font_files_or_shader_catalog(self):
        with tempfile.TemporaryDirectory() as folder:
            for warning in (True, False):
                report, _, _ = self.run_boot(frames=1, warning=warning,
                    extra=("--app", folder, "--font", "pixel"))
                self.assertEqual(report["font_atlases"], 0)
                self.assertEqual(report["font_bytes"], 0)
                self.assertEqual(report["shader_catalog_loaded"], 0)

    def test_rounded_first_frame_only_needs_its_visible_font_sizes(self):
        for warning, sizes in ((True, (16, 20, 28, 36)), (False, (16, 20, 28))):
            with self.subTest(warning=warning), tempfile.TemporaryDirectory() as folder:
                app = Path(folder);fonts = app / "assets/fonts";fonts.mkdir(parents=True)
                total = 0
                for size in sizes:
                    source = ROOT / "assets/fonts" / ("rounded-%d.rgf" % size)
                    shutil.copy2(source, fonts / source.name);total += source.stat().st_size
                # The other font sizes and shader catalog deliberately do not exist.
                report, _, _ = self.run_boot(frames=1, warning=warning,
                    extra=("--app", folder, "--font", "rounded"))
                self.assertEqual(report["font_atlases"], len(sizes))
                self.assertEqual(report["font_bytes"], total)
                self.assertEqual(report["shader_catalog_loaded"], 0)

    def test_continue_prepares_remaining_rounded_fonts_and_shader_catalog(self):
        report, _, _ = self.run_boot("20 key A\n40 key-up A\n", extra=("--font", "rounded"))
        self.assertEqual(report["font_atlases"], 5)
        self.assertEqual(report["shader_catalog_loaded"], 1)

    def test_warning_does_not_read_system_preferences_before_continue(self):
        with tempfile.TemporaryDirectory() as folder:
            preferences = Path(folder) / "system.ini";os.mkfifo(preferences)
            report, _, _ = self.run_boot(frames=1, extra=("--system-preferences", str(preferences)))
            self.assertEqual(report["system_configuration_loaded"], 0)

    def test_saved_system_preferences_apply_before_home_clock_is_shown(self):
        with tempfile.TemporaryDirectory() as folder:
            preferences = Path(folder) / "system.ini"
            preferences.write_text("schema=1\npower=0\nbrightness=75\nidle=0\nlid=0\n"
                                   "battery_percent=1\ntimezone=Asia/Tokyo\n")
            for warning in (True, False):
                report, _, _ = self.run_boot("20 key A\n40 key-up A\n" if warning else "",
                    warning=warning, extra=("--system-preferences", str(preferences)))
                self.assertEqual(report["system_configuration_loaded"], 1)
                self.assertEqual(report["timezone"], "Asia/Tokyo")

    def test_first_present_notifies_guardian_before_scan_and_survives_closed_guardian(self):
        for receiver_closed in (False, True):
            with self.subTest(receiver_closed=receiver_closed), tempfile.TemporaryDirectory() as folder:
                root = Path(folder);report = root / "report.json"
                reader, writer = socket.socketpair()
                try:
                    if receiver_closed:reader.close()
                    result = subprocess.run([str(BINARY), "--preview", "--boot-startup", "--frames", "1",
                        "--boot-ready-fd", str(writer.fileno()), "--cache-report", str(report),
                        "--preferences", str(root / "preferences.ini"), "--rom-dir", "/missing/cold-card"],
                        pass_fds=(writer.fileno(),), cwd=ROOT, env=dict(os.environ, SDL_VIDEODRIVER="dummy"),
                        capture_output=True, text=True, timeout=5)
                    writer.close()
                    self.assertEqual(result.returncode, 0, result.stderr)
                    if not receiver_closed:
                        reader.settimeout(1);self.assertEqual(reader.recv(16), b"1")
                    self.assertEqual(json.loads(report.read_text())["library_scan_started"], 0)
                finally:
                    reader.close();writer.close()

    def test_warning_disabled_startup_scans_without_an_acceptance_press(self):
        with tempfile.TemporaryDirectory() as folder:
            roms=Path(folder).resolve();(roms/"one.nds").write_bytes(b"local fixture")
            report, state, _ = self.run_boot(warning=False, extra=("--rom-dir", str(roms)))
            self.assertEqual(report["library_scan_started"], 1)
            self.assertEqual(report["library_ready"], 1)
            self.assertIn(str(roms/"one.nds"), state)

    def test_warning_disabled_startup_keeps_home_responsive_during_slow_scan(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);helper=root/"slow-scan"
            helper.write_text("#!/usr/bin/env python3\nimport time\ntime.sleep(2)\n");helper.chmod(0o755)
            report, state, _=self.run_boot("100 key Left\n120 key-up Left\n160 key A\n180 key-up A\n", warning=False, extra=("--rom-dir",str(root)),helper=helper)
            self.assertEqual(report["library_ready"], 0)
            self.assertEqual(report["page"], 1)
            self.assertEqual(state, '1 3 2 ""\n')

    def test_fresh_a_and_start_continue_without_activating_a_root_card(self):
        for script in ("20 key A\n40 key-up A\n", "20 joy-down 7\n40 joy-up 7\n"):
            report, state, _ = self.run_boot(script)
            self.assertEqual(report["boot_pending"], 0)
            self.assertEqual(report["library_ready"], 1)
            self.assertEqual(state.splitlines(), ['1 3 2 ""', "home 1"])

    def test_queued_or_held_a_start_wait_for_release_then_a_fresh_press(self):
        for held in ("0 key A\n80 key A\n100 key-up A\n", "0 joy-down 7\n80 joy-down 7\n100 joy-up 7\n"):
            report, _, _ = self.run_boot(held)
            self.assertEqual(report["boot_pending"], 1)
            fresh = "160 key A\n180 key-up A\n"
            report, _, _ = self.run_boot(held + fresh)
            self.assertEqual(report["boot_pending"], 0)

    def test_fresh_sdl_finger_and_goodix_raw_touch_continue(self):
        report, _, _ = self.run_boot("20 finger-down 0.5 0.9375\n40 finger-up 0.5 0.9375\n")
        self.assertEqual(report["boot_pending"], 0)
        report, _, _ = self.run_boot(events=[(0, 0, 0, 0)] + self.raw_tap(20))
        self.assertEqual(report["boot_pending"], 0)

    def test_pre_display_touch_cannot_accept_on_release(self):
        report, _, _ = self.run_boot("0 finger-down 0.5 0.9375\n100 finger-up 0.5 0.9375\n")
        self.assertEqual(report["boot_pending"], 1)
        report, _, _ = self.run_boot(events=self.raw_tap(0, hold=100))
        self.assertEqual(report["boot_pending"], 1)
        report, _, _ = self.run_boot(events=self.raw_tap(0, hold=100) + self.raw_tap(160))
        self.assertEqual(report["boot_pending"], 0)

    def test_background_scan_keeps_category_and_settings_input_responsive(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            helper = root / "slow-scan"
            helper.write_text("#!/usr/bin/env python3\nimport time\ntime.sleep(2)\n")
            helper.chmod(0o755)
            began = time.monotonic()
            report, state, _ = self.run_boot("20 key A\n40 key-up A\n100 key Left\n120 key-up Left\n160 key A\n180 key-up A\n",
                                             extra=("--rom-dir", str(root)), helper=helper)
            self.assertLess(time.monotonic() - began, 2)
            self.assertEqual(report["boot_pending"], 0)
            self.assertEqual(report["library_ready"], 0)
            self.assertEqual(report["page"], 1)
            self.assertEqual(state, '1 3 2 ""\n')

    def test_real_background_scan_finishes_after_continue(self):
        with tempfile.TemporaryDirectory() as folder:
            roms = Path(folder).resolve()
            (roms / "one.nds").write_bytes(b"local fixture")
            report, state, _ = self.run_boot("20 key A\n40 key-up A\n", extra=("--rom-dir", str(roms)))
            self.assertEqual(report["library_scan_started"], 1)
            self.assertEqual(report["library_ready"], 1)
            self.assertIn(str(roms / "one.nds"), state)


if __name__ == "__main__":
    unittest.main()
