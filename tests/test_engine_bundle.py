"""Offline provisioning verifies bytes, inventory and paths without replacing bundles."""
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools.provision_engines import digest, provision, validate_bundle


class EngineBundleTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.source = self.root / 'source'
        self.bundle = self.root / 'bundle'
        for name, content in {
            'bin/dsperate': b'\x7fELF dsperate',
            'dsperate/LICENSE': b'upstream license',
            'dsperate/NOTICE.txt': b'upstream notice',
            'nnddss_payload/nnddss': b'\x7fELF nnddss',
            'nnddss_payload/lib/libbionic_shim.so': b'\x7fELF shim',
            'nnddss_payload/lib/LICENSE.txt': b'shim license',
            'nnddss_payload/gamecontrollerdb.txt': b'controller mapping',
            'nnddss_payload/ui/icon.png': b'PNG pixels',
            'nnddss_payload/shaders/Dot.dfx': b'supplied shader data',
            'nnddss_payload/COPYING': b'engine notice',
            'shaders/lcd1x.dsd': b'<vertex>owned shader path</vertex>',
            'shaders/README.txt': b'community shader attribution',
        }.items():
            self.put(name, content)

    def put(self, name, content):
        path = self.source / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        return path

    def manifest(self):
        return json.loads((self.bundle / 'payloads.json').read_text())

    def change_manifest(self, update):
        manifest = self.manifest()
        update(manifest)
        (self.bundle / 'payloads.json').write_text(json.dumps(manifest))

    def snapshot(self):
        result = {}
        for path in sorted(self.bundle.rglob('*')):
            if path.is_symlink(): result[path.relative_to(self.bundle).as_posix()] = ('link', str(path.readlink()))
            elif path.is_file():
                result[path.relative_to(self.bundle).as_posix()] = ('file', path.read_bytes(), path.stat().st_mtime_ns)
            else:
                result[path.relative_to(self.bundle).as_posix()] = ('directory',)
        return result

    def rejected_without_mutation(self):
        before = self.snapshot()
        with self.assertRaises(ValueError):
            provision(self.source, self.bundle)
        self.assertEqual(self.snapshot(), before)
        self.assertFalse(list(self.root.glob('.engine-import-*')))

    def test_verified_idempotence_and_notices_without_legacy_payloads(self):
        for name in (
            'lib/libhook.so', 'lib/libdrafix.so', 'bin/menu', 'dsperate/start.sh', 'dsperate/old.py',
            'nnddss_payload/lib/libhook.so', 'nnddss_payload/lib/libdrafix.so', 'nnddss_payload/tools/old.py',
            'nnddss_payload/start.sh', 'nnddss_payload/bios/bios7.bin', 'nnddss_payload/bios7.bin',
            'nnddss_payload/firmware.bin', 'nnddss_payload/game.nds', 'shaders/old.dfx'
        ):
            self.put(name, b'old or restricted data')
        self.assertEqual(provision(self.source, self.bundle), self.bundle.resolve())
        manifest = validate_bundle(self.bundle)
        files = manifest['files']
        self.assertEqual(
            set(files), {
                'dsperate/dsperate', 'dsperate/LICENSE', 'dsperate/NOTICE.txt', 'nnddss/lib/libbionic_shim.so',
                'nnddss/lib/LICENSE.txt', 'nnddss/gamecontrollerdb.txt', 'nnddss/ui/icon.png', 'nnddss/shaders/Dot.dfx',
                'nnddss/COPYING', 'shaders/lcd1x.dsd', 'shaders/README.txt'
            }
        )
        for name, expected in files.items():
            self.assertEqual(digest(self.bundle / name), expected)
        before = self.snapshot()
        self.assertEqual(provision(self.source, self.bundle), self.bundle.resolve())
        self.assertEqual(self.snapshot(), before)

    def test_identical_manifest_cannot_hide_altered_existing_bytes(self):
        provision(self.source, self.bundle)
        (self.bundle / 'nnddss/nnddss').write_bytes(b'\x7fELF altered')
        self.rejected_without_mutation()

    def test_unlisted_resource_and_removed_file_are_rejected(self):
        provision(self.source, self.bundle)
        extra = self.bundle / 'nnddss/ui/unlisted.png'
        extra.write_bytes(b'new pixels')
        self.rejected_without_mutation()
        extra.unlink()
        (self.bundle / 'dsperate/LICENSE').unlink()
        self.rejected_without_mutation()

    def test_unsupported_paths_even_when_hashed_are_rejected(self):
        provision(self.source, self.bundle)
        forbidden = self.bundle / 'nnddss/start.sh'
        forbidden.write_bytes(b'#!/bin/sh\nexit 0\n')
        self.change_manifest(lambda m: m['files'].update({'nnddss/start.sh': digest(forbidden)}))
        self.rejected_without_mutation()

    def test_unsafe_manifest_names_are_rejected(self):
        provision(self.source, self.bundle)
        original = (self.bundle / 'payloads.json').read_bytes()
        for name in (
            '../escape', '/absolute', 'nnddss/../escape', 'nnddss\\lib\\x.so', 'nnddss//nnddss', 'nnddss/./nnddss'
        ):
            with self.subTest(name=name):
                (self.bundle / 'payloads.json').write_bytes(original)
                self.change_manifest(lambda m: m['files'].update({name: '0' * 64}))
                self.rejected_without_mutation()

    def test_missing_required_manifest_entry_and_invalid_hash_types_fail(self):
        provision(self.source, self.bundle)
        original = (self.bundle / 'payloads.json').read_bytes()
        variants = [
            lambda m: m['files'].pop('dsperate/dsperate'), lambda m: m.update(files=[]),
            lambda m: m.update(schema=True), lambda m: m['files'].update({'dsperate/dsperate': None}),
            lambda m: m['files'].update({'dsperate/dsperate': 'bad-hash'})
        ]
        for update in variants:
            (self.bundle / 'payloads.json').write_bytes(original)
            self.change_manifest(update)
            self.rejected_without_mutation()

    def test_changed_source_or_manifest_metadata_never_replaces_bundle(self):
        provision(self.source, self.bundle)
        self.put('bin/dsperate', b'\x7fELF new source')
        self.rejected_without_mutation()
        self.put('bin/dsperate', b'\x7fELF dsperate')
        self.change_manifest(lambda m: m.update(excluded=[]))
        self.rejected_without_mutation()

    def test_source_file_or_directory_symlinks_are_refused_before_copy(self):
        original = self.source / 'nnddss_payload/lib/libbionic_shim.so'
        original.unlink()
        original.symlink_to(self.source / 'bin/dsperate')
        with self.assertRaises(ValueError):
            provision(self.source, self.bundle)
        self.assertFalse(self.bundle.exists())
        original.unlink()
        directory = self.source / 'nnddss_payload/lib'
        (directory / 'LICENSE.txt').unlink()
        directory.rmdir()
        directory.symlink_to(self.source / 'dsperate', target_is_directory=True)
        with self.assertRaises(ValueError):
            provision(self.source, self.bundle)
        self.assertFalse(self.bundle.exists())

    def test_existing_ancestor_manifest_and_root_symlinks_are_refused(self):
        provision(self.source, self.bundle)
        directory = self.bundle / 'nnddss/ui'
        real = self.root / 'real-ui'
        directory.rename(real)
        directory.symlink_to(real, target_is_directory=True)
        self.rejected_without_mutation()
        directory.unlink()
        real.rename(directory)
        manifest = self.bundle / 'payloads.json'
        real_manifest = self.root / 'real-manifest.json'
        manifest.rename(real_manifest)
        manifest.symlink_to(real_manifest)
        self.rejected_without_mutation()
        manifest.unlink()
        real_manifest.rename(manifest)
        alias = self.root / 'alias'
        alias.symlink_to(self.bundle, target_is_directory=True)
        before = self.snapshot()
        with self.assertRaises(ValueError):
            provision(self.source, alias)
        with self.assertRaises(ValueError):
            validate_bundle(alias)
        self.assertEqual(self.snapshot(), before)

    def test_shell_program_cannot_masquerade_as_an_engine_binary(self):
        self.put('bin/dsperate', b'#!/bin/sh\nexec old-menu\n')
        with self.assertRaisesRegex(ValueError, 'not ELF'):
            provision(self.source, self.bundle)
        self.assertFalse(self.bundle.exists())


if __name__ == '__main__': unittest.main()
