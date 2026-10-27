"""RG DS OS's panel leases (runtime/drm_lease.py) and the shim that hands them out (tools/build_compat_stubs.py)."""
import os
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'runtime'))
import drm_lease
import port_runner


class DrmLeaseTests(unittest.TestCase):
    def test_ioctl_numbers_match_the_kernel_headers(self):
        # drm.h / drm_mode.h on 64-bit ARM: _IOWR('d', nr, struct size).
        self.assertEqual(drm_lease.GETRESOURCES, 0xC04064A0)
        self.assertEqual(drm_lease.GETENCODER, 0xC01464A6)
        self.assertEqual(drm_lease.GETCONNECTOR, 0xC05064A7)
        self.assertEqual(drm_lease.GETPROPERTY, 0xC04064AA)
        self.assertEqual(drm_lease.GETPLANERESOURCES, 0xC01064B5)
        self.assertEqual(drm_lease.GETPLANE, 0xC02064B6)
        self.assertEqual(drm_lease.OBJ_GETPROPERTIES, 0xC02064B9)
        self.assertEqual(drm_lease.CREATE_LEASE, 0xC01864C6)
        self.assertEqual(drm_lease.SET_CLIENT_CAP, 0x4010640D)
        self.assertEqual(drm_lease.SET_MASTER, 0x641E)

    def test_the_shims_ship_where_each_loader_looks(self):
        # One LD_PRELOAD entry: $LIB is lib64 for RG DS OS's loader, lib/arm-linux-gnueabihf for the armhf layer's.
        shim = port_runner.lease_shims(ROOT)
        self.assertIn('/$LIB/', shim)
        aarch64 = Path(shim.replace('$LIB', 'lib64'))
        armhf = Path(shim.replace('$LIB', 'lib/arm-linux-gnueabihf'))
        self.assertEqual(aarch64.read_bytes()[:5], b'\x7fELF\x02')
        self.assertEqual(armhf.read_bytes()[:5], b'\x7fELF\x01')
        for path in (aarch64, armhf):
            # only the open calls are exported, so nothing else of the port's is interposed
            self.assertIn(b'openat64', path.read_bytes())

    def test_lease_environment(self):
        # The helper inherits its lease; the port's processes ask the runner's socket for theirs.
        leases = drm_lease.PanelLeases.__new__(drm_lease.PanelLeases)
        leases.fds = {'bottom': 8}
        leases.socket_path = '/run/rgds/drm-lease-1.sock'
        leases.card = type('Card', (), {'path': '/dev/dri/card1'})()
        self.assertEqual(
            leases.environment({}, 'bottom'), {
                'RGDS_DRM_LEASE_FD': '8',
                'RGDS_DRM_CARD': '/dev/dri/card1'
            }
        )
        self.assertEqual(
            leases.environment({}, 'top'), {
                'RGDS_DRM_LEASE_SOCKET': '/run/rgds/drm-lease-1.sock',
                'RGDS_DRM_CARD': '/dev/dri/card1'
            }
        )

    @unittest.skipUnless(sys.platform.startswith('linux'), 'SOCK_SEQPACKET Unix sockets')
    def test_each_request_gets_a_new_lease_and_revokes_the_last(self):
        import array
        import socket
        import tempfile
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        calls = []

        class Card:
            path = '/dev/dri/card0'

            def lease(self, objects):
                calls.append(('lease', tuple(objects)))
                return os.open(temp.name, os.O_RDONLY), len(calls)

            def revoke(self, lessee):
                calls.append(('revoke', lessee))

            def close(self):
                pass

        leases = drm_lease.PanelLeases.__new__(drm_lease.PanelLeases)
        leases.card, leases.fds, leases.current, leases.server = Card(), {}, {}, None
        leases.objects = {'top': [1, 2, 3]}
        leases.lock = __import__('threading').Lock()
        leases.socket_path = temp.name + '/lease.sock'
        leases.serve('top')
        self.addCleanup(leases.close)

        def ask():
            with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as client:
                client.connect(leases.socket_path)
                client.send(b'L')
                fds = array.array('i')
                _data, ancillary, _flags, _address = client.recvmsg(1, socket.CMSG_SPACE(fds.itemsize))
                fds.frombytes(ancillary[0][2])
                os.close(fds[0])

        ask()
        ask()
        self.assertEqual(calls, [('lease', (1, 2, 3)), ('revoke', 1), ('lease', (1, 2, 3))])


if __name__ == '__main__':
    unittest.main()
