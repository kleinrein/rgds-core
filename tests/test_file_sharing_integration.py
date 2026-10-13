"""Opt-in ARM64 Linux protocol test for the actual bundled SMB service."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'runtime'))
from file_sharing import Sharing
from fields import read_fields, write_fields


@unittest.skipUnless(sys.platform == 'linux' and os.geteuid() == 0 and os.environ.get('RGDS_SMB_INTEGRATION_BUNDLE'),
                     'Explicit bundled SMB integration test requires ARM64 Linux and root')
class SmbProtocolTests(unittest.TestCase):
    def test_authenticated_listing_upload_download_and_stop(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            data = root / 'data'; data.mkdir()
            games = root / 'games'; games.mkdir()
            sharing = Sharing(data, runtime_root=root / 'run')
            sharing.bundle = Path(os.environ['RGDS_SMB_INTEGRATION_BUNDLE'])
            client = sharing.bundle / 'bin/smbclient'
            try:
                sharing.start('127.0.0.1', [games])
                self.assertEqual(sharing.status('127.0.0.1')['sharing'], 1)
                auth = sharing.folder / 'login.ini'
                options = ['-s', str(sharing.folder / 'smb.conf'), '-A', str(auth)]
                def request(arguments):
                    result = subprocess.run([str(client), *arguments, *options], env=sharing.environment(),
                                            capture_output=True, text=True, timeout=10)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    return result.stdout
                listing = request(['-L', '127.0.0.1', '-g'])
                for name in ('Games', 'Imports', 'Exports'):
                    self.assertIn(name, listing)
                original = root / 'original.txt'
                payload = b'RGDS authenticated SMB transfer\n' * 100
                original.write_bytes(payload)
                downloaded = root / 'downloaded.txt'
                request(['//127.0.0.1/Imports', '-c', 'put ' + str(original) + ' transfer.txt; get transfer.txt ' + str(downloaded) + '; del transfer.txt'])
                self.assertEqual(downloaded.read_bytes(), payload)
                self.assertFalse((data / 'imports/transfer.txt').exists())
                incorrect = root / 'wrong-login.ini'
                write_fields(incorrect, dict(username=read_fields(auth)['username'], password='incorrect'))
                rejected = subprocess.run([str(client), '//127.0.0.1/Imports', '-s', str(sharing.folder / 'smb.conf'),
                                           '-A', str(incorrect), '-c', 'quit'], env=sharing.environment(),
                                          capture_output=True, text=True, timeout=10)
                self.assertNotEqual(rejected.returncode, 0)
            finally:
                sharing.stop()
            self.assertFalse(sharing.reachable('127.0.0.1'))
            self.assertFalse((sharing.folder / 'login.ini').exists())
            self.assertEqual(sharing.status('127.0.0.1')['sharing'], 0)


if __name__ == '__main__':
    unittest.main()
