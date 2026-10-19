"""GitHub release channel: key creation, feed pinning and installer trust upgrades."""
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
installer = load('install_package', ROOT / 'packaging/install_package.py')


@unittest.skipUnless(Path(release.OPENSSL).exists() or sys.platform == 'linux', 'OpenSSL 3 is required')
class ReleaseChannelTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
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

    def test_publish_refuses_dev_versions_and_packages_without_the_channel(self):
        trust, key = self.init()
        (self.root / 'packaging/update-trust.json').write_text(json.dumps(trust))
        dist = self.root / 'dist'; dist.mkdir()
        package = dist / 'RGDS-0.1.0-abc123.zip'
        with release.zipfile.ZipFile(package, 'w') as archive:
            archive.writestr('rgds/state/update-trust.json', json.dumps({'schema': 1, 'channel': 'stable', 'feed': '', 'keys': {}}))
        (dist / 'latest.txt').write_text(package.name + '\n')
        args = mock.Mock(version='0.1.0', repo=None, key=key, channel='stable', notes=None, days=90, upload=False)
        with mock.patch.object(release, 'TRUST', self.root / 'update-trust.json'), mock.patch.object(release, 'ROOT', self.root):
            with self.assertRaisesRegex(SystemExit, 'before this channel was pinned'):
                release.publish(args)
            args.version = '0.2.0'
            with self.assertRaisesRegex(SystemExit, 'set VERSION to 0.2.0'):
                release.publish(args)
            args.version = '0.0.1-dev'
            with self.assertRaisesRegex(SystemExit, 'release version'):
                release.publish(args)


class InstallerTrustTests(unittest.TestCase):
    def test_only_blank_trust_files_may_be_replaced(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'update-trust.json'
            path.write_text(json.dumps({'schema': 1, 'channel': 'stable', 'feed': '', 'keys': {}}))
            self.assertTrue(installer.unconfigured_trust(path))
            path.write_text(json.dumps({'schema': 1, 'channel': 'stable', 'feed': 'https://example.org/stable.json', 'keys': {}}))
            self.assertTrue(installer.unconfigured_trust(path))
            path.write_text(json.dumps({'schema': 1, 'channel': 'stable', 'feed': 'https://example.org/stable.json', 'keys': {'k': 'ab' * 32}}))
            self.assertFalse(installer.unconfigured_trust(path))
            path.write_text('not json')
            self.assertFalse(installer.unconfigured_trust(path))


if __name__ == '__main__':
    unittest.main()
