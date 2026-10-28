"""GitHub release channel: key creation and feed pinning."""
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


release = load('release', ROOT / 'tools/release.py')


@unittest.skipUnless(Path(release.OPENSSL).exists() or sys.platform == 'linux', 'OpenSSL 3 is required')
class ReleaseChannelTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def init(self, repo='rgds-dev/rg-ds-core', channel='stable'):
        trust = self.root / 'update-trust.json'
        args = mock.Mock(repo=repo, channel=channel, key=self.root / 'keys/release.pem', key_id='release-2026')
        (self.root / 'packaging').mkdir(exist_ok=True)
        with mock.patch.object(release, 'TRUST', trust), mock.patch.object(release, 'ROOT', self.root):
            release.init(args)
        return json.loads(trust.read_text()), args.key

    def test_init_creates_a_private_key_and_pins_the_github_feed(self):
        trust, key = self.init()
        self.assertEqual(trust['feed'], 'https://github.com/rgds-dev/rg-ds-core/releases/latest/download/stable.json')
        self.assertEqual(trust['keys'], {'release-2026': release.public_key(key)})
        self.assertEqual(len(trust['keys']['release-2026']), 64)
        self.assertEqual(key.stat().st_mode & 0o777, 0o600)
        self.assertEqual((self.root / 'packaging/release-repo.txt').read_text(), 'rgds-dev/rg-ds-core\n')
        # The key is reused, never silently replaced.
        again, _ = self.init()
        self.assertEqual(again['keys'], trust['keys'])

    def test_testing_channel_and_repository_validation(self):
        trust, _ = self.init(channel='testing')
        self.assertTrue(trust['feed'].endswith('/releases/latest/download/testing.json'))
        for bad in ('not a repo', 'owner', 'a/b/c'):
            with self.assertRaises(release.argparse.ArgumentTypeError):
                release.repo_name(bad)


if __name__ == '__main__':
    unittest.main()
