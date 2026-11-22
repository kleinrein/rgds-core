"""runtime/mdns.py (finding file servers) and the net_sources command line the launcher drives."""
import io
from pathlib import Path
import socket
import struct
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'runtime'))
import mdns
import net_sources


def record(name, kind, body):
    return mdns.encode_name(name) + struct.pack('>HHIH', kind, 1, 120, len(body)) + body


def response(*records):
    return struct.pack('>HHHHHH', 0, 0x8400, 0, len(records), 0, 0) + b''.join(records)


def server_answer():
    nas = 'Family NAS._smb._tcp.local'
    pi = 'retropi._sftp-ssh._tcp.local'
    pi_ssh = 'retropi._ssh._tcp.local'
    return response(
        record('_smb._tcp.local', mdns.PTR, mdns.encode_name(nas)),
        record(nas, mdns.SRV,
               struct.pack('>HHH', 0, 0, 445) + mdns.encode_name('nas.local')),
        record('nas.local', mdns.A, socket.inet_aton('192.168.1.20')),
        record('_sftp-ssh._tcp.local', mdns.PTR, mdns.encode_name(pi)),
        record('_ssh._tcp.local', mdns.PTR, mdns.encode_name(pi_ssh)),
        record(pi, mdns.SRV,
               struct.pack('>HHH', 0, 0, 22) + mdns.encode_name('retropi.local')),
        record(pi_ssh, mdns.SRV,
               struct.pack('>HHH', 0, 0, 22) + mdns.encode_name('retropi.local')),
        record('retropi.local', mdns.A, socket.inet_aton('192.168.1.30')),
    )


class MdnsTests(unittest.TestCase):
    def test_a_query_asks_for_each_service_by_pointer(self):
        packet = mdns.query(['_smb._tcp.local'], ident=7)
        self.assertEqual(struct.unpack('>HHHHHH', packet[:12]), (7, 0, 1, 0, 0, 0))
        self.assertEqual(mdns.read_name(packet, 12), ('_smb._tcp.local', 12 + 17))

    def test_compressed_names_are_followed(self):
        head = struct.pack('>HHHHHH', 0, 0x8400, 0, 1, 0, 0)
        service = mdns.encode_name('_smb._tcp.local')
        # The instance name is "Box" + a pointer back to the service name at offset 12 + 10.
        body = b'\x03Box' + struct.pack('>H', 0xC000 | 12)
        packet = head + service + struct.pack('>HHIH', mdns.PTR, 1, 120, len(body)) + body
        self.assertEqual(mdns.parse(packet), [('_smb._tcp.local', mdns.PTR, 'Box._smb._tcp.local')])

    def test_a_name_that_points_at_itself_is_refused(self):
        packet = struct.pack('>HHHHHH', 0, 0x8400, 0, 1, 0, 0) + struct.pack('>H', 0xC000 | 12)
        with self.assertRaises(ValueError):
            mdns.parse(packet)

    def test_queries_from_others_are_ignored(self):
        self.assertEqual(mdns.parse(mdns.query(['_smb._tcp.local'])), [])

    def test_servers_with_addresses_and_one_entry_per_ssh_host(self):
        services = mdns.collect(mdns.parse(server_answer()))
        self.assertEqual(
            services, [
                mdns.Service('sftp', 'retropi', 'retropi.local', '192.168.1.30', 22),
                mdns.Service('smb', 'Family NAS', 'nas.local', '192.168.1.20', 445),
            ]
        )

    def test_discover_reads_answers_sent_straight_back(self):
        responder = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        responder.bind(('127.0.0.1', 0))
        responder.settimeout(3)

        def answer():
            try:
                _, sender = responder.recvfrom(9000)
                responder.sendto(b'not dns', sender)
                responder.sendto(server_answer(), sender)
            except OSError:
                pass

        thread = threading.Thread(target=answer)
        thread.start()
        try:
            found = mdns.discover(wait=0.6, target=responder.getsockname())
        finally:
            thread.join()
            responder.close()
        self.assertEqual([s.name for s in found], ['retropi', 'Family NAS'])


class CommandLineTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.data = self.root / 'rgds'
        self.out = self.root / 'answer.tsv'

    def run_cli(self, *args, stdin=''):
        with patch('sys.stdin', io.StringIO(stdin)):
            code = net_sources.main(['--data', str(self.data), '--out', str(self.out), *args])
        return code, self.out.read_text().splitlines()

    def test_add_list_browse_and_remove_a_folder_place(self):
        folder = self.root / 'share'
        (folder / 'GBA').mkdir(parents=True)
        (folder / 'readme.txt').write_text('hi\tthere')
        code, lines = self.run_cli('add', 'local', '--name', 'Stick', '--root', str(folder))
        self.assertEqual((code, lines[0]), (0, 'ok'))
        ident = lines[1]
        self.assertEqual(self.run_cli('sources')[1][1].split('\t')[:3], [ident, 'local', 'Stick'])
        code, lines = self.run_cli('ls', ident)
        self.assertEqual(lines[1].split('\t')[:3], ['GBA', '1', '0'])
        self.assertEqual(lines[2].split('\t')[:3], ['readme.txt', '0', '8'])
        self.run_cli('remove', ident)
        self.assertEqual(self.run_cli('sources')[1], ['ok'])

    def test_errors_come_back_as_the_first_line(self):
        code, lines = self.run_cli('ls', 'smb-gone')
        self.assertEqual((code, lines), (1, ['error\tThat source was removed.']))
        code, lines = self.run_cli('add', 'sftp', '--name', 'pi')
        self.assertEqual(lines, ['error\tAn SFTP source needs a server.'])

    def test_the_password_comes_on_stdin_and_stays_private(self):
        code, lines = self.run_cli(
            'add',
            'smb',
            '--host',
            'nas.local',
            '--share',
            'Games',
            '--user',
            'me',
            '--password-stdin',
            stdin='hunter2\n'
        )
        self.assertEqual(code, 0)
        public, private = net_sources.config_paths(self.data)
        self.assertNotIn('hunter2', public.read_text())
        self.assertIn('password = hunter2', private.read_text())


if __name__ == '__main__':
    unittest.main()
