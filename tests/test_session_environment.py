"""Opt-in benchmark diagnostics survive the session boundary, not stock restore."""
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "runtime"))
import emulators
import session
import stock_handoff

DIAGNOSTICS = (
    "RGDS_LAUNCH_PROFILE", "RGDS_MELONDS_PROFILE", "RGDS_MELONDS_PRESENT_EVERY_FRAME", "RGDS_MELONDS_POSIX_SEMAPHORE"
)


class SessionEnvironmentTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="rgds-env-", dir="/tmp")
        self.addCleanup(temporary.cleanup)
        self.runtime = Path(temporary.name)
        self.compositor = self.runtime / "wayland-0"
        self.compositor.touch()
        self.env = {"XDG_RUNTIME_DIR": str(self.runtime), "PATH": "/usr/bin:/bin"}

    def stock_environment(self, additional):
        supplied = dict(self.env, **additional)
        vendor_sdl = Path("/mnt/vendor/deep/drastic_aarch64/libs/libSDL2-2.0.so.0")
        with patch.dict(os.environ, supplied, clear=True), \
             patch.object(Path, "is_socket", autospec=True, side_effect=lambda path:path == self.compositor), \
             patch.object(Path, "is_file", autospec=True, side_effect=lambda path:path == vendor_sdl):
            return session.environment(self.runtime)

    def test_runtime_bytecode_is_read_from_ram_and_compiled_there_at_idle_priority(self):
        environment = self.stock_environment({})
        self.assertEqual(environment["PYTHONDONTWRITEBYTECODE"], "1")
        self.assertEqual(environment["PYTHONPYCACHEPREFIX"], session.PYCACHE)
        self.assertTrue(session.PYCACHE.startswith("/tmp/"))
        app = Path("/release")
        with patch.object(session.subprocess, "Popen") as spawn:
            session.precompile(app, {"PATH": "/usr/bin"})
        command, options = spawn.call_args.args[0], spawn.call_args.kwargs
        self.assertEqual(command[1:], ["-m", "compileall", "-q", str(app / "runtime")])
        self.assertEqual(options["env"]["PYTHONPYCACHEPREFIX"], session.PYCACHE)
        self.assertTrue(options["start_new_session"])
        with patch.object(session.os, "nice") as nice:
            options["preexec_fn"]()
        nice.assert_called_once_with(19)

    def test_explicit_diagnostics_reach_engine_without_forwarding_stale_controls(self):
        supplied = {name: "1" for name in DIAGNOSTICS}
        supplied.update(
            RGDS_OVERLAY_FILE="/old/overlay",
            RGDS_SESSION_PLATFORM="independent",
            RGDS_INPUT_PROFILE="stale",
            RGDS_UNRECOGNIZED_DIAGNOSTIC="1",
            LD_PRELOAD="/old/hook.so"
        )
        environment = self.stock_environment(supplied)
        with patch.dict(os.environ, environment, clear=True):
            engine = emulators.base_environment()
        for name in DIAGNOSTICS:
            self.assertEqual(engine[name], "1")
        for name in (
            "RGDS_OVERLAY_FILE", "RGDS_SESSION_PLATFORM", "RGDS_INPUT_PROFILE", "RGDS_UNRECOGNIZED_DIAGNOSTIC",
            "LD_PRELOAD"
        ):
            self.assertNotIn(name, engine)
        self.assertEqual(engine["WAYLAND_DISPLAY"], "wayland-0")

    def test_default_and_nonexact_values_do_not_enable_diagnostics(self):
        for value in (None, "0", "true", "yes", "1 ", "01"):
            with self.subTest(value=value):
                supplied = {} if value is None else {name: value for name in DIAGNOSTICS}
                environment = self.stock_environment(supplied)
                for name in DIAGNOSTICS:
                    self.assertNotIn(name, environment)

    def test_standalone_handoff_receives_the_same_narrow_opt_in(self):
        supplied = dict(self.env, **{name: "1" for name in DIAGNOSTICS}, RGDS_OVERLAY_FILE="/old/overlay")
        with patch.dict(os.environ, supplied, clear=True), \
             patch("linux_platform.environment", side_effect=lambda clean:dict(clean)) as standalone:
            environment = session.environment(self.runtime, standalone=True)
        standalone.assert_called_once()
        for name in DIAGNOSTICS:
            self.assertEqual(environment[name], "1")
        self.assertNotIn("RGDS_OVERLAY_FILE", environment)

    def test_stock_restore_still_removes_all_rgds_diagnostics(self):
        with patch.dict(os.environ, dict(self.env, **{name: "1" for name in DIAGNOSTICS}), clear=True):
            environment = stock_handoff.clean_environment()
        for name in DIAGNOSTICS:
            self.assertNotIn(name, environment)


if __name__ == "__main__":
    unittest.main()
