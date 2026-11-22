"""Network places in Files (docs/NETWORK_PLAN.md phase 3), driven by keys: browsing a place, Add to library through
the transfer queue, and adding an SFTP place step by step against a real sshd."""
import getpass
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'runtime'))
from net_sources import add_source, sources, ssh_key

BINARY = Path(os.environ.get('RGDS_LAUNCHER', ROOT / 'build/host/rgds-launcher'))
SSHD = shutil.which('sshd') or '/usr/sbin/sshd'
SFTP_SERVER = next((p for p in ('/usr/libexec/sftp-server', '/usr/lib/openssh/sftp-server') if Path(p).exists()), None)


def free_port():
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        return probe.getsockname()[1]


@unittest.skipUnless(BINARY.exists(), 'Build the host launcher first')
class NetworkPlacesTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.app = self.root / 'app'
        self.app.mkdir()
        for name in ('assets', 'themes', 'runtime'):
            (self.app / name).symlink_to(ROOT / name, target_is_directory=True)
        self.data = self.root / 'data'
        self.data.mkdir()
        self.games = self.root / 'roms'
        self.inbox = self.root / 'inbox'
        self.games.mkdir()
        self.inbox.mkdir()
        self.state = self.root / 'state.txt'
        self.state.write_text('1 0 0 ""\n')

    def run_ui(self, script, seconds=4, environment=None):
        path = self.root / 'input.txt'
        path.write_text(script)
        result = subprocess.run(
            [
                str(BINARY), '--preview', '--app',
                str(self.app), '--data',
                str(self.data), '--preferences',
                str(self.root / 'preferences.ini'), '--state',
                str(self.state), '--input-script',
                str(path), '--seconds',
                str(seconds), '--settings', '--section', 'files'
            ],
            cwd=ROOT,
            env=dict(
                os.environ,
                SDL_VIDEODRIVER='dummy',
                SDL_AUDIODRIVER='dummy',
                RGDS_FILES_ROOTS=f'Games={self.games};Inbox={self.inbox}',
                **(environment or {})
            ),
            capture_output=True,
            text=True,
            timeout=60
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_a_game_from_a_place_goes_into_its_system_folder(self):
        shelf = self.root / 'stick'
        (shelf / 'Game Boy Advance').mkdir(parents=True)
        (shelf / 'Game Boy Advance/Golden Sun.gba').write_bytes(b'g' * 70_000)
        add_source(self.data, 'local', name='Stick', root=str(shelf))
        # Roots: Games, Inbox, Stick, Trash. R R opens Stick; A opens the folder; Y > Add to library.
        self.run_ui('300 key ]\n600 key ]\n1400 key A\n2200 key Y\n2400 key A\n', seconds=5)
        self.assertEqual((self.games / 'gba/Golden Sun.gba').read_bytes(), b'g' * 70_000)
        self.assertTrue((shelf / 'Game Boy Advance/Golden Sun.gba').exists())

    def test_download_to_inbox_and_copy_into_a_chosen_folder(self):
        shelf = self.root / 'stick'
        shelf.mkdir()
        (shelf / 'manual.pdf').write_bytes(b'%PDF')
        (shelf / 'notes.txt').write_bytes(b'notes')
        add_source(self.data, 'local', name='Stick', root=str(shelf))
        (self.games / 'Extras').mkdir()
        # Stick: manual.pdf first. Y > Download to Inbox. Down to notes.txt, Y > Copy; L L to Games, Extras is the
        # first row: A opens it, Y > Paste here.
        self.run_ui(
            '300 key ]\n600 key ]\n1400 key Y\n1500 key Down\n1600 key A\n2400 key Down\n2600 key Y\n'
            '2700 key Down\n2800 key Down\n2900 key A\n3200 key [\n3400 key [\n3800 key A\n4200 key Y\n'
            '4400 key A\n',
            seconds=6
        )
        self.assertEqual((self.inbox / 'manual.pdf').read_bytes(), b'%PDF')
        self.assertEqual((self.games / 'Extras/notes.txt').read_bytes(), b'notes')

    def test_removing_a_place(self):
        add_source(self.data, 'local', name='Stick', root=str(self.root))
        # On Stick, Y: Add to library, Download to Inbox, Copy, Refresh, Add network place, Remove this place.
        self.run_ui(
            '300 key ]\n600 key ]\n1400 key Y\n' + ''.join(f'{1500 + i * 60} key Down\n' for i in range(5)) +
            '2000 key A\n2300 key Down\n2400 key A\n',
            seconds=4
        )
        self.assertEqual(sources(self.data), {})

    @unittest.skipUnless(Path(SSHD).exists() and SFTP_SERVER and shutil.which('sftp'), 'OpenSSH server not installed')
    def test_adding_an_sftp_place_with_the_handhelds_key(self):
        server = self.root / 'pi'
        (server / 'roms').mkdir(parents=True)
        (server / 'roms/Tetris.gb').write_bytes(b't' * 32768)
        port = free_port()
        host_key = self.root / 'host_key'
        subprocess.run(['ssh-keygen', '-q', '-t', 'ed25519', '-N', '', '-f', str(host_key)], check=True)
        authorized = self.root / 'authorized_keys'
        # The key the handheld shows during the setup, put on the server as the owner would.
        authorized.write_text(ssh_key(self.data)[1] + '\n')
        config = self.root / 'sshd_config'
        config.write_text(
            f'Port {port}\nListenAddress 127.0.0.1\nHostKey {host_key}\nAuthorizedKeysFile {authorized}\n'
            f'StrictModes no\nUsePAM no\nPasswordAuthentication no\nPidFile none\n'
            f'Subsystem sftp {SFTP_SERVER}\n'
        )
        sshd = subprocess.Popen(
            [SSHD, '-f', str(config), '-D', '-e'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        )
        self.addCleanup(sshd.wait, 5)
        self.addCleanup(sshd.terminate)
        for _ in range(100):
            with socket.socket() as probe:
                if probe.connect_ex(('127.0.0.1', port)) == 0:
                    break
            time.sleep(0.05)
        folder = str(server)
        typed = ''.join(f'{6200 + n * 50} text {folder[i:i + 30]}\n' for n, i in enumerate(range(0, len(folder), 30)))
        # R R: the Network root. Y > Add network place > SFTP; nothing is found (the query goes to a closed port),
        # so "Type an address"; the address, the user, "This handheld's key", A past the key, the folder.
        script = (
            '300 key ]\n600 key ]\n1000 key Y\n1200 key A\n1500 key Down\n1600 key A\n'
            f'3600 key A\n4000 text 127.0.0.1:{port}\n4300 key Return\n4700 text {getpass.getuser()}\n'
            '5000 key Return\n5400 key Down\n5500 key A\n5900 key A\n' + typed + '8200 key Return\n'
        )
        closed = free_port()
        self.run_ui(script, seconds=12, environment={'RGDS_MDNS_TARGET': f'127.0.0.1:{closed}'})
        places = list(sources(self.data).values())
        self.assertEqual(len(places), 1, places)
        self.assertEqual(
            (places[0]['type'], places[0]['host'], places[0]['port'], places[0]['path']),
            ('sftp', '127.0.0.1', str(port), folder)
        )
        self.assertIn('127.0.0.1', (self.data / 'network/known_hosts').read_text())


if __name__ == '__main__':
    unittest.main()
