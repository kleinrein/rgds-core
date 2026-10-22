#!/usr/bin/env python3
"""Wi-Fi for the chat pad: rooms A-D over UDP broadcast, one process per session.

The launcher runs as root and must not parse packets from a network, so this
helper does: it checks every datagram, reassembles messages and hands the
launcher only whole payloads (still untrusted bytes, decoded strictly there).

Launcher -> helper, one command per line on stdin:
    HELLO <room 0-3> <colour 0-15> <name>      join a room, or change name/colour
    SEND <hex payload>                         a message for the current room
    LEAVE | QUIT
Helper -> launcher, one event per line on stdout:
    READY <sender id>   ONLINE <ip> | OFFLINE   USER <id> <room> <colour> <name>
    GONE <id>   MSG <id> <hex payload>
    FULL   (the room you asked to join already has sixteen people)
"""
import argparse
import binascii
import collections
import fcntl
import os
import random
import selectors
import socket
import struct
import sys
import time

MAGIC = b"RGPC"
VERSION = 1
HELLO, BYE, CHUNK = 1, 2, 3
PORT = 49320
ROOMS = 4
ROOM_LIMIT = 16  # users in one room, as on the DS
MAX_DATAGRAM = 1100
CHUNK_BYTES = 960
MAX_PAYLOAD = 8192  # must match pictochat_core.hpp
MAX_CHUNKS = (MAX_PAYLOAD + CHUNK_BYTES - 1) // CHUNK_BYTES
NAME_BYTES = 24
HELLO_EVERY = 2.0
USER_TTL = 7.0
PENDING_TTL = 3.0
PENDING_LIMIT = 32
BUCKET_BURST, BUCKET_RATE = 6.0, 3.0  # messages: a burst, then three a second
SIOCGIFADDR, SIOCGIFBRDADDR = 0x8915, 0x8919


def printable_name(raw):
    """A name that is valid UTF-8, printable, trimmed and short; else None."""
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError:
        return None
    if any(ord(c) < 0x20 or ord(c) == 0x7f or 0x80 <= ord(c) < 0xa0 or 0xd800 <= ord(c) <= 0xdfff for c in text):
        return None
    text = " ".join(text.split())
    while len(text.encode("utf-8")) > NAME_BYTES:
        text = text[:-1]
    return text or None


def clean_name(text):
    """A name the user typed, made safe to send: controls dropped, tabs and newlines as spaces."""
    text = "".join(
        " " if c in "\t\n\r" else c for c in text if c in "\t\n\r"
        or not (ord(c) < 0x20 or ord(c) == 0x7f or 0x80 <= ord(c) < 0xa0 or 0xd800 <= ord(c) <= 0xdfff)
    )
    return printable_name(text.encode("utf-8", "replace")) or "Guest"


def pack_hello(sender, room, colour, name):
    raw = clean_name(name).encode("utf-8")
    return MAGIC + struct.pack("<BBB8sBB", VERSION, HELLO, room, sender, colour, len(raw)) + raw


def pack_bye(sender, room):
    return MAGIC + struct.pack("<BBB8s", VERSION, BYE, room, sender)


def pack_chunks(sender, room, message_id, payload):
    count = max(1, (len(payload) + CHUNK_BYTES - 1) // CHUNK_BYTES)
    return [
        MAGIC + struct.pack("<BBB8sIBB", VERSION, CHUNK, room, sender, message_id, index, count) +
        payload[index * CHUNK_BYTES:(index + 1) * CHUNK_BYTES] for index in range(count)
    ]


class Bucket:
    def __init__(self):
        self.tokens, self.at = BUCKET_BURST, None

    def take(self, now):
        if self.at is not None:
            self.tokens = min(BUCKET_BURST, self.tokens + (now - self.at) * BUCKET_RATE)
        self.at = now
        if self.tokens >= 1:
            self.tokens -= 1
            return True
        return False


class Node:
    """The protocol, free of sockets so it can be tested with plain bytes."""
    def __init__(self, sender=None):
        self.sender = sender or os.urandom(8)
        self.room, self.colour, self.name = None, 0, "Guest"
        self.users = {}  # sender -> [room, colour, name, last seen]
        self.pending = collections.OrderedDict()  # (sender, id) -> [room, count, {index: bytes}, started]
        self.delivered = collections.OrderedDict()
        self.buckets = {}
        self.next_id = random.getrandbits(32)
        self.last_hello = None

    # -- what we say ---------------------------------------------------------
    def room_users(self, room):
        return sum(1 for u in self.users.values() if u[0] == room)

    def try_join(self, room, colour, name, now):
        """(datagrams, full): a room that already holds sixteen people turns you away."""
        if room != self.room and self.room_users(room) >= ROOM_LIMIT:
            return [], True
        return self.join(room, colour, name, now), False

    def join(self, room, colour, name, now):
        out = []
        if self.room is not None and room != self.room:
            out.append(pack_bye(self.sender, self.room))
        self.room, self.colour, self.name = room, colour, clean_name(name)
        self.last_hello = now
        out.append(pack_hello(self.sender, self.room, self.colour, self.name))
        return out

    def leave(self):
        out = [pack_bye(self.sender, self.room)] if self.room is not None else []
        self.room = None
        return out

    def send(self, payload):
        if self.room is None or not payload or len(payload) > MAX_PAYLOAD:
            return []
        self.next_id = (self.next_id + 1) & 0xffffffff
        packets = pack_chunks(self.sender, self.room, self.next_id, payload)
        return packets + packets  # twice: a lost datagram would drop the message

    def tick(self, now):
        """Datagrams due now, and events for users who stopped answering."""
        out, events = [], []
        if self.room is not None and (self.last_hello is None or now - self.last_hello >= HELLO_EVERY):
            self.last_hello = now
            out.append(pack_hello(self.sender, self.room, self.colour, self.name))
        for sender in [s for s, u in self.users.items() if now - u[3] > USER_TTL]:
            del self.users[sender]
            events.append("GONE " + sender.hex())
        for key in [k for k, v in self.pending.items() if now - v[3] > PENDING_TTL]:
            del self.pending[key]
        for sender in [
            s for s, b in self.buckets.items() if b.at is not None and now - b.at > 60 and s not in self.users
        ]:
            del self.buckets[sender]
        return out, events

    # -- what we hear --------------------------------------------------------
    def receive(self, data, now):
        """Events for one datagram; anything malformed is silently ignored."""
        events = []
        if len(data) < 15 or len(data) > MAX_DATAGRAM or data[:4] != MAGIC or data[4] != VERSION:
            return events
        kind, room, sender = data[5], data[6], data[7:15]
        if room >= ROOMS or sender == self.sender or kind not in (HELLO, BYE, CHUNK):
            return events
        body = data[15:]
        if kind == HELLO:
            if len(body) < 2:
                return events
            colour, length = body[0], body[1]
            name = printable_name(body[2:2 + length]) if len(body) == 2 + length else None
            if colour > 15 or name is None:
                return events
            known = self.users.get(sender)
            if known is None and self.room_users(room) >= ROOM_LIMIT:
                return events  # a seventeenth person is not part of the room
            changed = known is None or known[:3] != [room, colour, name]
            self.users[sender] = [room, colour, name, now]
            if changed:
                events.append("USER %s %d %d %s" % (sender.hex(), room, colour, name))
        elif kind == BYE:
            if len(body) == 0 and self.users.pop(sender, None) is not None:
                events.append("GONE " + sender.hex())
        else:
            if room != self.room or len(body) < 6 or sender not in self.users:
                return events
            message_id, index, count = struct.unpack_from("<IBB", body)
            data = body[6:]
            if count == 0 or count > MAX_CHUNKS or index >= count or len(data) > CHUNK_BYTES or (
                index + 1 < count and len(data) != CHUNK_BYTES
            ):
                return events
            key = (sender, message_id)
            if key in self.delivered:
                return events
            entry = self.pending.get(key)
            if entry is None:
                if not self.buckets.setdefault(sender, Bucket()).take(now):
                    return events
                while len(self.pending) >= PENDING_LIMIT:
                    self.pending.popitem(last=False)
                entry = self.pending[key] = [room, count, {}, now]
            if entry[1] != count or entry[0] != room:
                return events
            entry[2][index] = data
            if len(entry[2]) == count:
                del self.pending[key]
                payload = b"".join(entry[2][i] for i in range(count))
                if 0 < len(payload) <= MAX_PAYLOAD:
                    self.delivered[key] = now
                    while len(self.delivered) > 256:
                        self.delivered.popitem(last=False)
                    events.append("MSG %s %s" % (sender.hex(), payload.hex()))
        return events


# -- the network ------------------------------------------------------------------
def interface_broadcasts():
    """(ip, directed broadcast) for each up, non-loopback IPv4 interface (Linux)."""
    found = []
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        for _index, name in socket.if_nameindex():
            if name == "lo":
                continue
            request = struct.pack("256s", name.encode()[:15])
            try:
                ip = socket.inet_ntoa(fcntl.ioctl(probe.fileno(), SIOCGIFADDR, request)[20:24])
                broadcast = socket.inet_ntoa(fcntl.ioctl(probe.fileno(), SIOCGIFBRDADDR, request)[20:24])
            except OSError:
                continue
            if not ip.startswith("169.254.") and broadcast != "0.0.0.0":
                found.append((ip, broadcast))
    except (OSError, AttributeError):
        pass
    finally:
        probe.close()
    return found


def drop_privileges():
    """The launcher runs as root; this helper reads strangers' packets, so it gives root up
    as soon as its socket is bound. If root cannot be given up, it does not run."""
    if os.geteuid() != 0:
        return
    try:
        os.setgroups([])
        os.setgid(65534)
        os.setuid(65534)
    except OSError as error:
        sys.stderr.write("pictochat_net: cannot drop root: %s\n" % error)
        sys.exit(1)
    if os.geteuid() == 0 or os.getegid() == 0:
        sys.exit(1)


def say(line):
    sys.stdout.write(line + "\n")
    sys.stdout.flush()


def parse_command(line):
    """A launcher command as a tuple, or None if it is not one."""
    parts = line.rstrip("\n").split(" ", 3)
    if parts[0] == "HELLO" and len(parts) >= 3:
        try:
            room, colour = int(parts[1]), int(parts[2])
        except ValueError:
            return None
        if 0 <= room < ROOMS and 0 <= colour <= 15:
            return ("HELLO", room, colour, parts[3] if len(parts) > 3 else "Guest")
    elif parts[0] == "SEND" and len(parts) == 2:
        try:
            payload = binascii.unhexlify(parts[1])
        except (binascii.Error, ValueError):
            return None
        return ("SEND", payload) if 0 < len(payload) <= MAX_PAYLOAD else None
    elif parts[0] in ("LEAVE", "QUIT") and len(parts) == 1:
        return (parts[0],)
    return None


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", type=int, default=PORT)
    parser.add_argument("--targets", default="", help="host:port,... instead of broadcasting (testing)")
    parser.add_argument("--bind", default="0.0.0.0")
    args = parser.parse_args(argv)
    node = Node()
    targets = [(t.rsplit(":", 1)[0], int(t.rsplit(":", 1)[1])) for t in args.targets.split(",") if t]
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if hasattr(socket, "SO_REUSEPORT"):
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.bind((args.bind, args.port))
    sock.setblocking(False)
    drop_privileges()
    selector = selectors.DefaultSelector()
    selector.register(sock, selectors.EVENT_READ, "net")
    os.set_blocking(sys.stdin.fileno(), False)
    selector.register(sys.stdin, selectors.EVENT_READ, "cmd")
    addresses, scanned, online = [], -10.0, None
    stdin_buffer = ""

    def transmit(packets):
        for packet in packets:
            for target in (
                targets
                or [(b, args.port) for _ip, b in addresses] + ([("255.255.255.255", args.port)] if addresses else [])
            ):
                try:
                    sock.sendto(packet, target)
                except OSError:
                    pass

    say("READY " + node.sender.hex())
    running = True
    while running:
        now = time.monotonic()
        if not targets and now - scanned >= 3:
            scanned = now
            addresses = interface_broadcasts()
            state = addresses[0][0] if addresses else None
            if state != online:
                online = state
                say("ONLINE " + state if state else "OFFLINE")
        if targets and online is None:
            online = "test"
            say("ONLINE test")
        out, events = node.tick(now)
        transmit(out)
        for event in events:
            say(event)
        for key, _mask in selector.select(timeout=0.2):
            if key.data == "net":
                for _ in range(64):
                    try:
                        data, _address = sock.recvfrom(2048)
                    except (BlockingIOError, InterruptedError):
                        break
                    except OSError:
                        break
                    for event in node.receive(data, time.monotonic()):
                        say(event)
            else:
                try:
                    chunk = os.read(sys.stdin.fileno(), 65536)
                except (BlockingIOError, InterruptedError):
                    continue
                if not chunk:
                    running = False
                    break
                stdin_buffer += chunk.decode("utf-8", "replace")
                if len(stdin_buffer) > 4 * MAX_PAYLOAD * 2 + 1024 and "\n" not in stdin_buffer:
                    stdin_buffer = ""
                while "\n" in stdin_buffer:
                    line, stdin_buffer = stdin_buffer.split("\n", 1)
                    command = parse_command(line)
                    if command is None:
                        continue
                    now = time.monotonic()
                    if command[0] == "HELLO":
                        packets, full = node.try_join(command[1], command[2], command[3], now)
                        if full:
                            say("FULL")
                        transmit(packets)
                    elif command[0] == "SEND":
                        transmit(node.send(command[1]))
                    elif command[0] == "LEAVE":
                        transmit(node.leave())
                    else:
                        running = False
    transmit(node.leave())
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (BrokenPipeError, KeyboardInterrupt):
        sys.exit(0)  # the launcher went away
