"""The device's BusyBox tar has no -z/-j/-J (no seamless compression): `tar -tzf` fails, and a script that takes its
empty output as "no files" quietly does the wrong thing. That is how the first boot dropped the flasher's Set up
answers and every Bring over bundle (usr/libexec/rgds/handoff, 2026-11-24). Device scripts pipe gunzip into tar."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ROOT / 'os/buildroot/rootfs'
# a tar invocation whose short options include a compression letter: tar -xzf, tar -tzf, tar czf, tar -xJf ...
COMPRESSED = re.compile(r'\btar\s+-?[a-zA-Z]*[zjJ][a-zA-Z]*\b')


class BusyBoxTar(unittest.TestCase):

    def test_device_scripts_do_not_ask_tar_to_decompress(self):
        found = []
        for path in sorted(SCRIPTS.rglob('*')):
            if not path.is_file() or path.suffix in ('.py', '.json', '.ini', '.conf', '.png', '.bin'):
                continue
            try:
                text = path.read_text()
            except (UnicodeDecodeError, OSError):
                continue
            if not text.startswith('#!'):
                continue
            for number, line in enumerate(text.splitlines(), 1):
                if not line.lstrip().startswith('#') and COMPRESSED.search(line):
                    found.append(f'{path.relative_to(ROOT)}:{number}: {line.strip()}')
        self.assertEqual(found, [], 'BusyBox tar here cannot decompress; pipe gunzip -c into tar -f - instead')

    def test_the_pattern_catches_the_old_handoff(self):
        self.assertTrue(COMPRESSED.search('names=$(tar -tzf $OUT/flasher.tar.gz 2>/dev/null)'))
        self.assertFalse(COMPRESSED.search('gunzip -c $OUT/flasher.tar.gz | tar -xOf - preset.ini'))


if __name__ == '__main__':
    unittest.main()
