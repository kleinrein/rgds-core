"""Run the actual ARM64 shared library in isolated Linux helper processes.

No DRM initialization, handheld access, or game is needed. These checks exercise
the real patched config parser, signal handler, exit receipt and loader symbols.
"""
import json
import os
from pathlib import Path
import platform
import signal
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
LIB = ROOT / 'build/superdrastic/libsuperdrastic.so'


@unittest.skipUnless(
    platform.system() == 'Linux' and platform.machine() in ('aarch64', 'arm64') and LIB.is_file(),
    'Requires the local Linux ARM64 build'
)
class NativeArm64Tests(unittest.TestCase):
    def run_native(self, code, save='0'):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'config').mkdir()
            (root / 'config/drastic.cfg').write_text(
                'controls_a[CONTROL_INDEX_SAVE_STATE] = 1040\ncontrols_a[CONTROL_INDEX_LOAD_STATE] = 1041\ncontrols_a[CONTROL_INDEX_MENU] = 1044\n'
            )
            env = dict(
                os.environ,
                LD_PRELOAD=str(LIB),
                LD_BIND_NOW='1',
                DSFLIP_CPUGOV='0',
                DSFLIP_EXIT_COMBO='0',
                DSFLIP_RAST='0',
                DSFLIP_RESUME_FILE=str(root / 'resume.dss'),
                RGDS_SD_SAVE_ON_EXIT=save,
                RGDS_SD_EXIT=str(root / 'exit')
            )
            env.pop('RGDS_CORE', None)
            result = subprocess.run(
                [sys.executable, '-c', 'import ctypes,os,signal; lib=ctypes.CDLL(None); lib.resume_start(); ' + code],
                cwd=root,
                env=env,
                capture_output=True,
                text=True,
                timeout=8
            )
            receipt = (root / 'exit').read_text() if (root / 'exit').exists() else ''
            return result, receipt

    def test_real_pinned_library_loads_with_all_symbols_and_reads_spaced_controls(self):
        result, _ = self.run_native(
            'print([lib.resume_control_button(x) for x in (b"SAVE_STATE",b"LOAD_STATE",b"MENU")])'
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout), [16, 17, 20])

    def test_device_discovery_handles_long_paths_and_missing_devices(self):
        result, _ = self.run_native(
            'from pathlib import Path; p=Path.cwd().joinpath(*(["segment"*12]*8)); p.mkdir(parents=True); p=p/"goodix"; p.touch(); f=lib.rgds_sd_device_matches; f.argtypes=[ctypes.c_char_p,ctypes.c_char_p]; assert f(str(p).encode(),b"goodix")==1; assert f(str(p).encode(),b"absent")==0; assert f(b"/missing-device",b"goodix")==0'
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_signal_exit_with_autosave_disabled_has_receipt_even_with_resume_path(self):
        result, receipt = self.run_native('os.kill(os.getpid(),signal.SIGUSR1); lib.resume_frame()')
        self.assertEqual(result.returncode, -signal.SIGKILL, result.stderr)
        self.assertEqual(receipt, 'state=no-save\n')

    def test_fast_forward_release_uses_time_even_when_presents_stop(self):
        result, _ = self.run_native(
            '''
os.environ['RGDS_SD_SESSION'] = os.getcwd()
event = ctypes.create_string_buffer(56)
lib.resume_press(19)
assert lib.resume_poll(event) == 1 and event.raw[12:14] == bytes([19, 1])
for _ in range(40): lib.resume_frame()
assert lib.resume_poll(event) == 0
import time
time.sleep(.18)
assert lib.resume_poll(event) == 1 and event.raw[12:14] == bytes([19, 0])
assert lib.rgds_sd_idle() == 1
'''
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_save_control_retains_frame_held_release(self):
        result, _ = self.run_native(
            '''
os.environ['RGDS_SD_SESSION'] = os.getcwd()
event = ctypes.create_string_buffer(56)
lib.resume_press(16)
assert lib.resume_poll(event) == 1 and event.raw[12:14] == bytes([16, 1])
import time
time.sleep(.18)
assert lib.resume_poll(event) == 0
for _ in range(40): lib.resume_frame()
assert lib.resume_poll(event) == 1 and event.raw[12:14] == bytes([16, 0])
'''
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_missing_emulator_save_produces_failure_receipt_and_keeps_second_signal_safe(self):
        result, receipt = self.run_native(
            'os.kill(os.getpid(),signal.SIGUSR1); lib.resume_frame(); os.kill(os.getpid(),signal.SIGUSR1); import time; time.sleep(6)',
            save='1'
        )
        self.assertEqual(result.returncode, -signal.SIGKILL, result.stderr)
        self.assertEqual(receipt, 'state=save-failed\n')


if __name__ == '__main__': unittest.main()
