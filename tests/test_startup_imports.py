"""Boot verification and session setup do not load unused game/archive code."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'runtime'))
import records


class StartupImportTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.app = self.root / 'releases/test-build'
        header = bytearray(20)
        header[:6] = b'\x7fELF\x02\x01'
        header[18:20] = b'\xb7\x00'
        payload = {'bin/rgds-launcher': bytes(header), 'runtime/session.py': b'owned startup'}
        for name, content in payload.items():
            path = self.app / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
        manifest = dict(
            schema=1,
            project='rgds',
            target='aarch64-linux-gnu',
            build='test-build',
            files={
                name: hashlib.sha256(content).hexdigest()
                for name, content in payload.items()
            }
        )
        (self.app / 'manifest.json').write_text(json.dumps(manifest))
        pointer = self.root / 'state/active'
        pointer.parent.mkdir()
        pointer.write_text('test-build\n')

    def test_fresh_boot_imports_and_verification_do_not_load_game_or_archive_modules(self):
        prohibited = [
            'emulators', 'library', 'zipfile', 'game_menu', 'emulator_profiles', 'shaders', 'integration', 'engine_log'
        ]
        for module in ('activate',):
            with self.subTest(module=module):
                code = '''import importlib,json,sys
from pathlib import Path
sys.path.insert(0,sys.argv[1])
module=importlib.import_module(sys.argv[2])
if sys.argv[2]=='activate':module.verify_startup(Path(sys.argv[3])/'releases/test-build')
print(json.dumps([name for name in json.loads(sys.argv[4]) if name in sys.modules]))
'''
                result = subprocess.run(
                    [
                        sys.executable, '-B', '-c', code,
                        str(ROOT / 'runtime'), module,
                        str(self.root),
                        json.dumps(prohibited)
                    ],
                    capture_output=True,
                    text=True,
                    timeout=5
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(json.loads(result.stdout), [])

    def test_shared_process_identity_handles_spaces_and_parentheses_in_comm(self):
        suffix = ['S'] + ['0'] * 18 + ['987654'] + ['0'] * 3
        content = '123 (worker (render) child) ' + ' '.join(suffix) + '\n'
        with patch.object(records.Path, 'read_text', return_value=content) as read:
            self.assertEqual(records.process_identity(123), '987654')
        self.assertEqual(read.call_count, 1)

    def test_missing_or_malformed_process_identity_never_proves_ownership(self):
        for content in ('', '123 (unterminated comm', '123 (short) S 0 0'):
            with self.subTest(content=content), patch.object(records.Path, 'read_text', return_value=content):
                self.assertIsNone(records.process_identity(123))
        with patch.object(records.Path, 'read_text', side_effect=FileNotFoundError):
            self.assertIsNone(records.process_identity(123))

    def test_legacy_modules_reexport_the_shared_helpers(self):
        import emulators
        import fields
        import library
        self.assertIs(emulators.process_identity, records.process_identity)
        self.assertIs(library.read_fields, fields.read_fields)
        self.assertIs(library.write_fields, fields.write_fields)


if __name__ == '__main__': unittest.main()
