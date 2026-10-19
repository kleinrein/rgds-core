"""DSCore builds straight from dscore/: fingerprint, provenance, source mirroring and archive checks."""
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools import build_rgds_experimental as fork

NOTES = f'Forked from DSperate {fork.base.COMMIT} (tree {fork.base.TREE}).\n'


class SourceTreeTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(); self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name); self.tree = self.root / 'dscore'
        self.write('FORK.md', NOTES); self.write('LICENSE', 'GPL'); self.write('src/a.cpp', 'int a;')
        self.write('src/sub/b.hpp', '#pragma once')

    def write(self, name, text):
        path = self.tree / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_text(text)

    def test_fingerprint_covers_source_but_not_notes_litter_or_generated_spirv(self):
        before = fork.source_hash(self.tree)
        self.write('FORK.md', NOTES + 'More notes.\n'); self.write('src/.DS_Store', 'x')
        self.write(f'{fork.SHADER_DIR}/lean_vert.spv', 'blob')
        self.assertEqual(fork.source_hash(self.tree), before)
        self.write('src/a.cpp', 'int b;')
        self.assertNotEqual(fork.source_hash(self.tree), before)
        self.write('src/new.cpp', ''); self.assertNotIn('FORK.md', fork.tree_files(self.tree))
        self.assertIn('src/new.cpp', fork.tree_files(self.tree))

    def test_links_are_not_source(self):
        (self.tree / 'src/link.cpp').symlink_to(self.tree / 'src/a.cpp')
        with self.assertRaisesRegex(ValueError, 'symlinks'): fork.tree_files(self.tree)
        (self.tree / 'src/link.cpp').unlink(); (self.tree / 'linked').symlink_to(self.tree / 'src')
        with self.assertRaisesRegex(ValueError, 'symlinks'): fork.tree_files(self.tree)
        with self.assertRaisesRegex(ValueError, 'missing'): fork.tree_files(self.root / 'nowhere')

    def test_notes_must_name_the_pinned_upstream(self):
        self.write('FORK.md', f'Forked from {fork.base.COMMIT}.\n')
        with self.assertRaisesRegex(ValueError, 'pinned DSperate commit'): fork.check_provenance(self.tree)
        (self.tree / 'FORK.md').unlink()
        with self.assertRaisesRegex(ValueError, 'pinned DSperate commit'): fork.source_hash(self.tree)

    def test_checked_in_fork_is_complete_and_names_its_upstream(self):
        files = fork.tree_files()
        self.assertEqual(fork.source_hash(), fork.fingerprint(files))
        for name in ('CMakeLists.txt', 'LICENSE', 'pgo/aarch64/MANIFEST', 'src/frontend/sdl/main.cpp'):
            self.assertIn(name, files)
        for name in fork.EXTRAS: self.assertTrue((ROOT / name).is_file(), name)

    def test_build_copy_mirrors_the_tree_and_drops_what_it_no_longer_has(self):
        scratch = self.root / 'scratch'; copy = scratch / 'source'
        for name in ('src/old.cpp', 'gone/deep/old.hpp', f'{fork.SHADER_DIR}/lean_vert.spv'):
            (copy / name).parent.mkdir(parents=True, exist_ok=True); (copy / name).write_text('stale')
        (copy / 'src').mkdir(exist_ok=True); (copy / 'src/a.cpp').write_text('edited in the build copy')
        source, files = fork.prepare_source(self.tree, scratch)
        self.assertEqual(source, copy)
        self.assertEqual(files, fork.tree_files(self.tree))
        self.assertEqual({p.relative_to(copy).as_posix() for p in copy.rglob('*') if p.is_file()}, set(files))
        self.assertFalse((copy / 'gone').exists())
        self.assertEqual((copy / 'src/a.cpp').read_text(), 'int a;')
        # Source edited later reaches the next build; an unchanged tree is left alone.
        kept = (copy / 'LICENSE').stat().st_mtime_ns
        self.write('src/a.cpp', 'int c;')
        fork.prepare_source(self.tree, scratch)
        self.assertEqual((copy / 'src/a.cpp').read_text(), 'int c;')
        self.assertEqual((copy / 'LICENSE').stat().st_mtime_ns, kept)

    def test_build_copy_refuses_links_and_unnamed_upstream(self):
        scratch = self.root / 'scratch'; fork.prepare_source(self.tree, scratch)
        (scratch / 'source/src/a.cpp').unlink(); (scratch / 'source/src/a.cpp').symlink_to(self.tree / 'src/a.cpp')
        with self.assertRaisesRegex(ValueError, 'symlinks'): fork.prepare_source(self.tree, scratch)
        self.write('FORK.md', 'No upstream here.\n')
        with self.assertRaisesRegex(ValueError, 'pinned DSperate commit'): fork.prepare_source(self.tree, self.root / 'other')

    def test_source_archive_must_match_its_fingerprint_and_recipe(self):
        source, files = fork.prepare_source(self.tree, self.root / 'scratch')
        output = self.root / 'source.tar.gz'
        fork.archive_source(source, files, output)
        fork.validate_source_archive(output, fork.fingerprint(files))
        with self.assertRaisesRegex(ValueError, 'source mismatch'):
            fork.validate_source_archive(output, '0' * 64)
        (source / 'src/a.cpp').write_text('changed after hashing')
        fork.archive_source(source, files, output)
        with self.assertRaisesRegex(ValueError, 'source mismatch'):
            fork.validate_source_archive(output, fork.fingerprint(files))
        fork.archive_source(source, files, output)
        with patch.object(fork, 'ROOT', self.root):
            with self.assertRaisesRegex(ValueError, 'recipe changed'):
                fork.validate_source_archive(output, fork.fingerprint(files))


class BuildRecordTests(unittest.TestCase):
    def build_directory(self, source_hash):
        temporary = tempfile.TemporaryDirectory(); self.addCleanup(temporary.cleanup)
        directory = Path(temporary.name)
        for name in fork.FILES: (directory / name).write_bytes(b'fixture ' + name.encode())
        record = dict(schema=2, kind='source-built-rgds-experimental', upstream_commit=fork.base.COMMIT,
                      source_hash=source_hash, options=fork.OPTIONS,
                      driver_sha256=fork.base.digest(Path(fork.__file__)),
                      base_driver_sha256=fork.base.digest(Path(fork.base.__file__)),
                      files={name: fork.base.digest(directory / name) for name in fork.FILES})
        (directory / 'build.json').write_text(json.dumps(record))
        return directory

    def test_build_of_an_older_tree_is_stale(self):
        with self.assertRaisesRegex(ValueError, 'Stale or invalid'):
            fork.validate_build(self.build_directory('0' * 64))

    def test_build_of_the_current_tree_goes_on_to_check_the_binary(self):
        directory = self.build_directory(fork.source_hash())
        with patch.object(fork.base, 'inspect_elf', side_effect=RuntimeError('reached the ELF check')):
            with self.assertRaisesRegex(RuntimeError, 'reached the ELF check'):
                fork.validate_build(directory)

    def test_edited_source_stales_an_otherwise_current_build(self):
        directory = self.build_directory('1' * 64)
        with patch.object(fork, 'source_hash', return_value='1' * 64), \
                patch.object(fork.base, 'inspect_elf', side_effect=RuntimeError('reached the ELF check')):
            with self.assertRaisesRegex(RuntimeError, 'reached the ELF check'): fork.validate_build(directory)
        with patch.object(fork, 'source_hash', return_value='2' * 64):
            with self.assertRaisesRegex(ValueError, 'Stale or invalid'): fork.validate_build(directory)


if __name__ == '__main__':
    unittest.main()
