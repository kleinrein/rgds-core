"""Find file servers on the local network by Bonjour/mDNS, in plain Python (docs/NETWORK_PLAN.md section 2).

One legacy unicast query (from an ordinary port, so responders answer us directly) asks 224.0.0.251:5353 for
_smb._tcp, _sftp-ssh._tcp and _ssh._tcp; whatever answers within the wait is returned with its name, host name,
address and port. Macs, Linux boxes with avahi, most NAS and Raspberry Pi OS answer; Windows file sharing usually
does not, so the setup always offers "Type an address" too.
"""
from collections import namedtuple
import random
import socket
import struct
import time

Service = namedtuple('Service', 'kind name host address port')
SERVICES = {'_smb._tcp.local': 'smb', '_sftp-ssh._tcp.local': 'sftp', '_ssh._tcp.local': 'sftp'}
GROUP = ('224.0.0.251', 5353)
PTR, A, SRV, ANY = 12, 1, 33, 255


def encode_name(name):
    out = b''
    for label in name.rstrip('.').split('.'):
        raw = label.encode('utf-8')
        out += bytes([len(raw)]) + raw
    return out + b'\0'


def query(names, ident=None):
    ident = random.randint(0, 0xFFFF) if ident is None else ident
    packet = struct.pack('>HHHHHH', ident, 0, len(names), 0, 0, 0)
    for name in names:
        packet += encode_name(name) + struct.pack('>HH', PTR, 1)
    return packet


def read_name(packet, offset, depth=0):
    """A possibly compressed DNS name at offset; (name, offset after it)."""
    labels, end = [], None
    while True:
        if offset >= len(packet) or depth > 16:
            raise ValueError('bad name')
        length = packet[offset]
        if length & 0xC0 == 0xC0:
            pointer = struct.unpack('>H', packet[offset:offset + 2])[0] & 0x3FFF
            if end is None: end = offset + 2
            offset, depth = pointer, depth + 1
            continue
        offset += 1
        if length == 0:
            break
        labels.append(packet[offset:offset + length].decode('utf-8', 'replace'))
        offset += length
    return '.'.join(labels), end if end is not None else offset


def parse(packet):
    """Every resource record in a response: [(name, type, data)], data decoded for PTR, SRV and A."""
    if len(packet) < 12:
        raise ValueError('short packet')
    _, flags, questions, answers, authority, additional = struct.unpack('>HHHHHH', packet[:12])
    if not flags & 0x8000:
        return []  # a query, not an answer
    offset = 12
    for _ in range(questions):
        _, offset = read_name(packet, offset)
        offset += 4
    records = []
    for _ in range(answers + authority + additional):
        name, offset = read_name(packet, offset)
        kind, _, _, length = struct.unpack('>HHIH', packet[offset:offset + 10])
        offset += 10
        body = packet[offset:offset + length]
        if kind == PTR:
            data = read_name(packet, offset)[0]
        elif kind == SRV:
            port = struct.unpack('>H', body[4:6])[0]
            data = (read_name(packet, offset + 6)[0], port)
        elif kind == A and length == 4:
            data = socket.inet_ntoa(body)
        else:
            data = None
        records.append((name.lower() if kind != PTR else name, kind, data))
        offset += length
    return records


def collect(records):
    """Services from all the records seen, one per instance; SSH only where the same host has no SFTP entry."""
    pointers, targets, addresses = [], {}, {}
    for name, kind, data in records:
        if kind == PTR and name.lower() in SERVICES:
            pointers.append((name.lower(), data))
        elif kind == SRV and data:
            targets[name.lower()] = data
        elif kind == A and data:
            addresses[name.lower()] = data
    found = {}
    for service, instance in pointers:
        host, port = targets.get(instance.lower(), ('', 0))
        label = instance[:-(len(service) + 1)] if instance.lower().endswith('.' + service) else instance
        address = addresses.get(host.lower(), '')
        kind = SERVICES[service]
        key = (kind, (address or host or label).lower())
        if key in found and service == '_ssh._tcp.local':
            continue
        found[key] = Service(kind, label, host.rstrip('.'), address, port or (445 if kind == 'smb' else 22))
    return sorted(found.values(), key=lambda s: (s.kind, s.name.lower()))


def discover(wait=1.5, target=GROUP, names=tuple(SERVICES)):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    try:
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 255)
        sock.bind(('', 0))
        sock.settimeout(0.2)
        packet = query(names)
        sock.sendto(packet, target)
        records, deadline, resent = [], time.monotonic() + wait, False
        while time.monotonic() < deadline:
            if not resent and time.monotonic() > deadline - wait / 2:
                sock.sendto(packet, target)  # a second ask catches a responder that was asleep
                resent = True
            try:
                data, _ = sock.recvfrom(9000)
            except socket.timeout:
                continue
            try:
                records += parse(data)
            except (ValueError, struct.error):
                continue
        return collect(records)
    except OSError:
        return []  # no network, or multicast refused
    finally:
        sock.close()
