"""Launch preparation retains saves, switches release code and avoids redundant I/O."""
import errno
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'runtime'))
import emulators
from emulator_profiles import section_config


class LaunchPreparationTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.source = self.root / 'release/libcore.so'
        self.source.parent.mkdir()
        self.source.write_bytes(b'verified code')
        self.destination = self.root / 'home/lib/libcore.so'
        self.save = self.root / 'home/User/backup/game.dsv'
        self.save.parent.mkdir(parents=True)
        self.save.write_bytes(b'progress')

    def test_existing_copy_becomes_link_and_warm_launch_does_not_reread_libraries(self):
        self.destination.parent.mkdir(parents=True)
        self.destination.write_bytes(b'old library')
        emulators.link_component(self.source, self.destination)
        inode = self.destination.lstat().st_ino
        with patch.object(Path, 'open', side_effect=AssertionError('duplicate library read')):
            emulators.link_component(self.source, self.destination)
        self.assertTrue(self.destination.is_symlink())
        self.assertEqual(self.destination.lstat().st_ino, inode)
        self.assertEqual(self.destination.read_bytes(), b'verified code')
        self.assertEqual(self.save.read_bytes(), b'progress')

    def test_release_switch_and_rollback_replace_only_the_owned_link(self):
        second = self.root / 'second/libcore.so'
        second.parent.mkdir()
        second.write_bytes(b'new verified code')
        for source in (self.source, second, self.source):
            emulators.link_component(source, self.destination)
            self.assertEqual(self.destination.resolve(), source.resolve())
        self.assertEqual(second.read_bytes(), b'new verified code')
        self.assertEqual(self.save.read_bytes(), b'progress')

    def test_unsupported_links_copy_and_preserve_unchanged_files(self):
        for reason in (errno.EPERM, errno.ENOSYS, errno.EOPNOTSUPP):
            with self.subTest(reason=reason), patch.object(Path, 'symlink_to', side_effect=OSError(reason, 'no links')):
                emulators.link_component(self.source, self.destination)
                inode = self.destination.stat().st_ino
                emulators.link_component(self.source, self.destination)
                self.assertEqual(self.destination.stat().st_ino, inode)
                self.assertFalse(self.destination.is_symlink())
                self.assertEqual(self.destination.read_bytes(), self.source.read_bytes())
        self.assertEqual(self.save.read_bytes(), b'progress')

    def test_io_errors_do_not_fall_back_or_replace_existing_code(self):
        self.destination.parent.mkdir(parents=True)
        self.destination.write_bytes(b'previous')
        for reason in (errno.EIO, errno.ENOSPC, errno.EROFS):
            with self.subTest(reason=reason
                              ), patch.object(Path, 'symlink_to', side_effect=OSError(reason, 'storage failure')):
                with self.assertRaises(OSError):
                    emulators.link_component(self.source, self.destination)
            self.assertEqual(self.destination.read_bytes(), b'previous')

    def test_rejects_redirected_library_directory(self):
        outside = self.root / 'outside'
        outside.mkdir()
        self.destination.parent.symlink_to(outside)
        with self.assertRaises(ValueError):
            emulators.link_component(self.source, self.destination)
        self.assertEqual(list(outside.iterdir()), [])

    def test_section_updates_keep_unknown_settings_and_skip_unchanged_writes(self):
        path = self.root / 'config.ini'
        path.write_text('[emu]\nframeskip=3\ncustom=keep\n[pad]\na=old\n')
        section_config(path, {'pad': {'a': 'a'}})
        before = path.read_bytes(), path.stat().st_ino, path.stat().st_mtime_ns
        section_config(path, {'pad': {'a': 'a'}})
        self.assertEqual((path.read_bytes(), path.stat().st_ino, path.stat().st_mtime_ns), before)
        self.assertIn('custom = keep', path.read_text())
        self.assertIn('frameskip = 3', path.read_text())

    def test_failed_section_publish_retains_previous_config_and_removes_partial(self):
        path = self.root / 'config.ini'
        path.write_text('[emu]\nframeskip=0\n')
        with patch.object(Path, 'replace', side_effect=OSError('storage failure')):
            with self.assertRaises(OSError):
                section_config(path, {'emu': {'frameskip': 2}})
        self.assertEqual(path.read_text(), '[emu]\nframeskip=0\n')
        self.assertFalse(path.with_name(path.name + '.pending').exists())


if __name__ == '__main__': unittest.main()
