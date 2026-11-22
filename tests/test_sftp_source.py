"""SFTP sources (runtime/net_sources.py SftpSource) against a real OpenSSH sshd on a loopback port: listing, resumed
downloads, the handheld's own key, trust on first use, and the transfer queue end to end."""
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
import net_sources
from net_sources import SourceError, add_source, open_source, ssh_key
from transfers import Queue, Runner, add

SSHD = shutil.which('sshd') or '/usr/sbin/sshd'
SFTP_SERVER = next(
    (
        p for p in ('/usr/libexec/sftp-server', '/usr/lib/openssh/sftp-server', '/usr/lib/ssh/sftp-server')
        if Path(p).exists()
    ), None
)


def free_port():
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        return probe.getsockname()[1]


@unittest.skipUnless(Path(SSHD).exists() and SFTP_SERVER and shutil.which('sftp'), 'OpenSSH server not installed')
class SftpTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.data = self.root / 'rgds'
        self.data.mkdir()
        self.server = self.root / 'server'
        self.server.mkdir()
        self.authorized = self.root / 'authorized_keys'
        self.authorized.write_text('')
        self.port = free_port()
        self.make_host_key()
        self.start_sshd()
        work = patch_work = self.root / 'work'
        self.work = work
        self.original_work = net_sources.work_dir
        net_sources.work_dir = lambda: patch_work
        self.addCleanup(setattr, net_sources, 'work_dir', self.original_work)

    def make_host_key(self):
        key = self.root / 'host_key'
        for path in (key, key.with_suffix('.pub')):
            path.unlink(missing_ok=True)
        subprocess.run(['ssh-keygen', '-q', '-t', 'ed25519', '-N', '', '-f', str(key)], check=True)

    def start_sshd(self):
        config = self.root / 'sshd_config'
        config.write_text(
            f'Port {self.port}\nListenAddress 127.0.0.1\nHostKey {self.root / "host_key"}\n'
            f'AuthorizedKeysFile {self.authorized}\nStrictModes no\nUsePAM no\n'
            f'PasswordAuthentication no\nPidFile none\nSubsystem sftp {SFTP_SERVER}\n'
        )
        self.sshd = subprocess.Popen(
            [SSHD, '-f', str(config), '-D', '-e'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        )
        self.addCleanup(self.stop_sshd)
        for _ in range(100):
            with socket.socket() as probe:
                if probe.connect_ex(('127.0.0.1', self.port)) == 0:
                    return
            time.sleep(0.05)
        self.fail('sshd did not start')

    def stop_sshd(self):
        if self.sshd.poll() is None:
            self.sshd.terminate()
            self.sshd.wait(5)

    def trust_handheld(self):
        _, public = ssh_key(self.data)
        self.authorized.write_text(public + '\n')

    def place(self):
        self.trust_handheld()
        return add_source(
            self.data,
            'sftp',
            name='pi',
            host='127.0.0.1',
            port=self.port,
            user=getpass.getuser(),
            path=str(self.server)
        )

    def serve(self, relative, content):
        path = self.server / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        return path

    def test_lists_folders_first_with_spaces_and_hides_dot_files(self):
        self.serve('Game Boy/Link\'s Awakening (EU).gb', b'l' * 3000)
        self.serve('Game Boy/.DS_Store', b'junk')
        (self.server / 'Game Boy/Saves').mkdir()
        source = open_source(self.data, self.place())
        entries = source.list('Game Boy')
        self.assertEqual(
            [(e.name, e.is_dir, e.size) for e in entries],
            [('Saves', True, 0), ("Link's Awakening (EU).gb", False, 3000)]
        )
        self.assertGreater(entries[1].mtime, time.time() - 3600)
        self.assertEqual(source.stat("Game Boy/Link's Awakening (EU).gb").size, 3000)

    def test_a_partial_download_resumes(self):
        content = os.urandom(300_000)
        self.serve('Zelda.gba', content)
        source = open_source(self.data, self.place())
        part = self.root / 'Zelda.gba.part'
        part.write_bytes(content[:100_000])
        source.fetch('Zelda.gba', part)
        self.assertEqual(part.read_bytes(), content)

    def test_the_handheld_key_is_made_once_and_kept_off_the_card_in_use(self):
        key, public = ssh_key(self.data)
        self.assertTrue(public.startswith('ssh-ed25519 '))
        self.assertEqual(ssh_key(self.data)[1], public)
        self.serve('a.gba', b'a')
        open_source(self.data, self.place()).list()
        # Each command copies the key into the private work folder and removes it afterwards.
        self.assertEqual(list(self.work.iterdir()), [])

    def test_an_unknown_key_reads_as_a_sign_in_problem(self):
        ident = add_source(
            self.data,
            'sftp',
            name='pi',
            host='127.0.0.1',
            port=self.port,
            user=getpass.getuser(),
            path=str(self.server)
        )
        ssh_key(self.data)  # made, but never added to authorized_keys
        with self.assertRaisesRegex(SourceError, 'Wrong user name, password or key for pi') as caught:
            open_source(self.data, ident).list()
        self.assertFalse(caught.exception.transient)

    def test_a_server_whose_key_changed_is_refused(self):
        ident = self.place()
        open_source(self.data, ident).list()
        self.assertTrue((self.data / 'network/known_hosts').read_text())
        self.stop_sshd()
        self.make_host_key()
        self.start_sshd()
        with self.assertRaisesRegex(SourceError, 'not the server it was before'):
            open_source(self.data, ident).list()

    def test_nothing_listening_is_worth_retrying(self):
        ident = self.place()
        self.stop_sshd()
        with self.assertRaises(SourceError) as caught:
            open_source(self.data, ident).list()
        self.assertTrue(caught.exception.transient)
        self.assertIn('pi', str(caught.exception))

    def test_queue_fetches_a_disc_folder_over_sftp_into_the_library(self):
        self.serve('PlayStation/Crash/Crash.cue', b'FILE "Crash.bin" BINARY')
        self.serve('PlayStation/Crash/Crash.bin', b'c' * 50_000)
        ident = self.place()
        job = add(self.data, ident, 'PlayStation/Crash')
        roms, inbox = self.root / 'roms', self.root / 'inbox'
        Runner(self.data, roms, inbox, self.root / 'run/transfers', sleep=lambda s: None).run()
        done = Queue(self.data).job(job['id'])
        self.assertEqual(done['state'], 'done', done['error'])
        self.assertEqual((roms / 'psx/Crash/Crash.bin').read_bytes(), b'c' * 50_000)


if __name__ == '__main__':
    unittest.main()
