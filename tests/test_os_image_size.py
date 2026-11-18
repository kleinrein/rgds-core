"""The RG DS OS image stays inside the 768 MiB root slot: the source archives leave it for a release asset of their own
(tools/own_os_build.py move_sources, tools/own_os_release.py sources), and a release that would not fit is refused."""
import hashlib
import json
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import own_os_build  # noqa: E402
import own_os_release  # noqa: E402


class ImageSizeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.app = self.root / 'app'
        self.sources = self.root / 'sources'
        files = {
            'emulators/rgds-experimental/source.tar.gz': b'dscore',
            'emulators/rgds-experimental/panvk-source.tar.gz': b'panvk',
            'emulators/rgds-play/source.tar.gz': b'play',
            'services/samba/source.tar.gz': b'samba',
            'emulators/rgds-play/rgds-play': b'engine',
        }
        for name, data in files.items():
            (self.app / name).parent.mkdir(parents=True, exist_ok=True)
            (self.app / name).write_bytes(data)

    def test_source_archives_leave_the_image_and_are_listed_in_it(self):
        with patch.object(own_os_build, 'SOURCES', self.sources):
            moved = own_os_build.move_sources(self.app)
        self.assertEqual(len(moved), 4)
        self.assertEqual(list(self.app.rglob('*source.tar.gz')), [])
        self.assertTrue((self.app / 'emulators/rgds-play/rgds-play').is_file())
        self.assertEqual((self.sources / 'services/samba/source.tar.gz').read_bytes(), b'samba')
        listing = (self.app / 'SOURCES.md').read_text()
        version = (ROOT / 'VERSION').read_text().strip()
        self.assertIn(f'RG-DS-OS-{version}-sources.tar', listing)
        self.assertIn(hashlib.sha256(b'panvk').hexdigest(), listing)
        self.assertIn('RG-DS-OS-<version>-sources.tar', (self.app / 'THIRD_PARTY.md').read_text())

    def test_the_release_publishes_the_sources_of_this_build_only(self):
        with patch.object(own_os_build, 'SOURCES', self.sources):
            moved = own_os_build.move_sources(self.app)
        (self.sources / 'sources.json').write_text(json.dumps(dict(build='0.9.1-abc-os1', files=moved)))
        out = self.root / 'out'
        out.mkdir()
        with patch.object(own_os_release, 'SOURCES', self.sources):
            with self.assertRaises(SystemExit):
                own_os_release.sources('0.9.1-abc-os2', '0.9.1', out)
            archive = own_os_release.sources('0.9.1-abc-os1', '0.9.1', out)
        with tarfile.open(archive) as tar:
            self.assertEqual(set(tar.getnames()), {*moved, 'sources.json'})

    def test_a_root_filesystem_larger_than_the_slot_is_refused(self):
        rootfs = self.root / 'rootfs.squashfs'
        with rootfs.open('wb') as out:
            out.truncate(own_os_release.SLOT_BYTES)
        self.assertEqual(own_os_release.fits_slot(rootfs), own_os_release.SLOT_BYTES)
        with rootfs.open('ab') as out:
            out.write(b'x')
        with self.assertRaises(SystemExit):
            own_os_release.fits_slot(rootfs)

    def test_the_slot_size_is_the_cards(self):
        layout = (ROOT / 'os/buildroot/board/rgds/genimage.cfg.in').read_text()
        self.assertEqual(layout.count('size = 768M'), 2)
        self.assertEqual(own_os_release.SLOT_BYTES, 768 * 1024 * 1024)


if __name__ == '__main__':
    unittest.main()
