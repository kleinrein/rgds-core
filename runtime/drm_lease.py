"""DRM leases for the two panels on RG DS OS, which has no compositor (docs/PORTMASTER_PLAN.md).

A port and its menu helper both need a panel of their own at the same time: the port the top one, the helper the
bottom one (its info screen, and the menu while the port is frozen). Only one process can be DRM master, so the
runner becomes master once the launcher has let go of the display, and leases each panel (its connector, CRTC and
primary plane) to one of them. A lessee is a master of its own objects only: the port's SDL, whichever copy, sees
the top panel as the only display there is. The leases end when the runner closes its descriptor.

Programs open /dev/dri/card0 themselves; librgds-drm-lease.so (tools/build_compat_stubs.py), preloaded, hands
them a lease instead (RGDS_DRM_CARD names the card). The helper is one process and inherits its lease
(RGDS_DRM_LEASE_FD). A port is a tree of processes one after another (a splash, PortMaster's GUI, the game), and
each needs a lease of its own: a DRM file's events, buffers and framebuffers are that file's, and a process reading
another's queued page-flip events jumps through pointers into the other's memory (PortMaster's GUI crashed in
SDL's flip handler that way, on its splash's events). So the port's processes ask the runner for theirs
(RGDS_DRM_LEASE_SOCKET): each new process gets a new lease of the top panel, and the one before it loses the panel.
"""
import array
import ctypes
import fcntl
import os
import socket
import struct
import threading

CARDS = ('/dev/dri/card0', '/dev/dri/card1')
DSI = 16  # DRM_MODE_CONNECTOR_DSI
OBJECT_PLANE = 0xeeeeeeee
PLANE_PRIMARY = 1


def _iowr(number, size):
    return (3 << 30) | (size << 16) | (ord('d') << 8) | number


SET_CLIENT_CAP = (1 << 30) | (16 << 16) | (ord('d') << 8) | 0x0d
SET_MASTER = (ord('d') << 8) | 0x1e
GETRESOURCES = _iowr(0xA0, 64)
GETENCODER = _iowr(0xA6, 20)
GETCONNECTOR = _iowr(0xA7, 80)
GETPROPERTY = _iowr(0xAA, 64)
GETPLANERESOURCES = _iowr(0xB5, 16)
GETPLANE = _iowr(0xB6, 32)
OBJ_GETPROPERTIES = _iowr(0xB9, 32)
CREATE_LEASE = _iowr(0xC6, 24)
REVOKE_LEASE = _iowr(0xC9, 4)
CAP_UNIVERSAL_PLANES = 2


def _ioctl(fd, request, buffer):
    fcntl.ioctl(fd, request, buffer, True)
    return buffer


def _ids(count):
    array = (ctypes.c_uint32 * max(count, 1))()
    return array, ctypes.addressof(array)


class Card:
    """The display card, opened as master. Raises OSError when it is not ours to lease (another master holds it)."""
    def __init__(self, path=None):
        self.fd = -1
        for candidate in ([path] if path else CARDS):
            try:
                fd = os.open(candidate, os.O_RDWR | os.O_CLOEXEC)
            except OSError:
                continue
            try:
                resources = self._resources(fd)
            except OSError:
                os.close(fd)
                continue
            if resources[2]:  # panfrost's card node has no connectors
                self.fd, self.path = fd, candidate
                break
            os.close(fd)
        if self.fd < 0: raise OSError('no display card')
        try:
            fcntl.ioctl(self.fd, SET_MASTER)  # the first opener is master already; this fails if someone else is
            _ioctl(self.fd, SET_CLIENT_CAP, bytearray(struct.pack('<QQ', CAP_UNIVERSAL_PLANES, 1)))
        except OSError:
            self.close()
            raise

    @staticmethod
    def _resources(fd):
        head = _ioctl(fd, GETRESOURCES, bytearray(64))
        counts = struct.unpack_from('<4I', head, 32)
        arrays = [_ids(n) for n in counts]
        body = bytearray(struct.pack('<4Q4I', *(a[1] for a in arrays), *counts)) + bytearray(16)
        _ioctl(fd, GETRESOURCES, body)
        counts = struct.unpack_from('<4I', body, 32)
        fbs, crtcs, connectors, encoders = ([a[0][i] for i in range(n)] for a, n in zip(arrays, counts))
        return fbs, crtcs, connectors, encoders

    def connectors(self):
        """{name: (connector id, [possible CRTC ids], current CRTC id or 0)}"""
        _fbs, crtcs, connectors, _encoders = self._resources(self.fd)
        found = {}
        for connector in connectors:
            # Counts first (no modes asked for: the arrays stay unwritten), then the encoders.
            query = lambda pointer, count: bytearray(
                struct.pack('<4Q12I', pointer, 0, 0, 0, 0, 0, count, 0, connector,
                            *(0,) * 7)
            )
            count_encoders = struct.unpack_from('<I', _ioctl(self.fd, GETCONNECTOR, query(0, 0)), 40)[0]
            encoders, pointer = _ids(count_encoders)
            body = _ioctl(self.fd, GETCONNECTOR, query(pointer, count_encoders))
            current, _id, kind, kind_id = struct.unpack_from('<4I', body, 44)
            if kind != DSI: continue
            possible, active = [], 0
            for encoder in list(encoders)[:struct.unpack_from('<I', body, 40)[0]]:
                record = _ioctl(self.fd, GETENCODER, bytearray(struct.pack('<5I', encoder, 0, 0, 0, 0)))
                _e, _t, crtc, mask, _c = struct.unpack('<5I', record)
                possible += [crtcs[i] for i in range(len(crtcs)) if mask & (1 << i) and crtcs[i] not in possible]
                if encoder == current and crtc: active = crtc
            found['DSI-%d' % kind_id] = (connector, possible, active)
        return found

    def primary_planes(self):
        """{CRTC id: its primary plane}: VOP2 gives each video port one primary plane that only it may use."""
        _fbs, crtcs, _connectors, _encoders = self._resources(self.fd)
        head = _ioctl(self.fd, GETPLANERESOURCES, bytearray(16))
        count = struct.unpack_from('<I', head, 8)[0]
        planes, pointer = _ids(count)
        _ioctl(self.fd, GETPLANERESOURCES, bytearray(struct.pack('<QI4x', pointer, count)))
        found = {}
        for plane in list(planes)[:count]:
            record = _ioctl(self.fd, GETPLANE, bytearray(struct.pack('<6IQ', plane, 0, 0, 0, 0, 0, 0)))
            mask = struct.unpack_from('<I', record, 12)[0]
            if self._plane_type(plane) != PLANE_PRIMARY: continue
            owners = [crtcs[i] for i in range(len(crtcs)) if mask & (1 << i)]
            if len(owners) == 1: found.setdefault(owners[0], plane)
        return found

    def _plane_type(self, plane):
        head = _ioctl(self.fd, OBJ_GETPROPERTIES, bytearray(struct.pack('<QQIII4x', 0, 0, 0, plane, OBJECT_PLANE)))
        count = struct.unpack_from('<I', head, 16)[0]
        props, props_pointer = _ids(count)
        values = (ctypes.c_uint64 * max(count, 1))()
        _ioctl(
            self.fd, OBJ_GETPROPERTIES,
            bytearray(struct.pack('<QQIII4x', props_pointer, ctypes.addressof(values), count, plane, OBJECT_PLANE))
        )
        for i in range(count):
            record = _ioctl(self.fd, GETPROPERTY, bytearray(struct.pack('<QQII32sII', 0, 0, props[i], 0, b'', 0, 0)))
            if record[24:56].split(b'\0')[0] == b'type': return values[i]
        return None

    def lease(self, objects):
        """(lessee descriptor, lessee id) holding objects; the descriptor is inheritable (it is handed to a child)."""
        ids = (ctypes.c_uint32 * len(objects))(*objects)
        record = _ioctl(
            self.fd, CREATE_LEASE, bytearray(struct.pack('<QIIII', ctypes.addressof(ids), len(objects), 0, 0, 0))
        )
        lessee, fd = struct.unpack_from('<II', record, 16)
        return fd, lessee

    def revoke(self, lessee):
        """The lessee loses its objects at once (they can be leased again); False when it was gone already."""
        try:
            _ioctl(self.fd, REVOKE_LEASE, bytearray(struct.pack('<I', lessee)))
            return True
        except OSError:
            return False

    def close(self):
        if self.fd >= 0: os.close(self.fd)
        self.fd = -1


class PanelLeases:
    """The top and bottom panels leased apart. The helper inherits fds['bottom']; the port's processes ask the socket
    for a lease of the top panel each (serve). close() ends them all."""
    def __init__(self, top='DSI-2', bottom='DSI-1', socket_path=None):
        self.card = Card()
        self.fds, self.objects, self.current = {}, {}, {}
        self.lock = threading.Lock()
        self.server = None
        self.socket_path = socket_path or '/run/rgds/drm-lease-%d.sock' % os.getpid()
        try:
            connectors, planes = self.card.connectors(), self.card.primary_planes()
            taken = set()
            for role, name in (('top', top), ('bottom', bottom)):
                if name not in connectors: raise OSError('no %s connector' % name)
                connector, possible, active = connectors[name]
                choices = ([active] if active else []) + possible
                crtc = next((c for c in choices if c not in taken and c in planes), None)
                if crtc is None: raise OSError('no CRTC with a primary plane for ' + name)
                taken.add(crtc)
                self.objects[role] = [connector, crtc, planes[crtc]]
            self.fds['bottom'] = self.lease('bottom')
            self.serve('top')
        except OSError:
            self.close()
            raise

    @property
    def path(self):
        return self.card.path

    def lease(self, role):
        """A new lease of role's panel; the previous holder's is revoked first (one lessee per object)."""
        with self.lock:
            if role in self.current: self.card.revoke(self.current.pop(role))
            fd, self.current[role] = self.card.lease(self.objects[role])
            return fd

    def serve(self, role):
        """Hand out leases of role's panel on socket_path: one per connection, as SCM_RIGHTS."""
        try:
            os.unlink(self.socket_path)
        except FileNotFoundError:
            pass
        self.server = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        self.server.bind(self.socket_path)
        os.chmod(self.socket_path, 0o600)
        self.server.listen(4)
        server = self.server

        def answer():
            while True:
                try:
                    client, _address = server.accept()
                except OSError:
                    return  # closed
                with client:
                    try:
                        client.recv(16)
                        fd = self.lease(role)
                    except OSError as error:
                        print('RGDS port: no lease for a port process: %s' % error, flush=True)
                        continue
                    try:
                        client.sendmsg([b'L'], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', [fd]))])
                    except OSError:
                        pass
                    finally:
                        os.close(fd)  # the process holds it now; its lease ends when it exits

        threading.Thread(target=answer, name='drm-lease', daemon=True).start()

    def environment(self, env, role):
        env['RGDS_DRM_CARD'] = self.card.path
        if role == 'bottom': env['RGDS_DRM_LEASE_FD'] = str(self.fds['bottom'])
        else: env['RGDS_DRM_LEASE_SOCKET'] = self.socket_path
        return env

    def close(self):
        if self.server is not None:
            try:
                self.server.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self.server.close()
            self.server = None
            try:
                os.unlink(self.socket_path)
            except OSError:
                pass
        for fd in self.fds.values():
            try:
                os.close(fd)
            except OSError:
                pass
        self.fds = {}
        with self.lock:
            for lessee in self.current.values():
                self.card.revoke(lessee)
            self.current = {}
        self.card.close()
