"""The chat pad's Wi-Fi helper: protocol rules, hostile packets and two real processes."""
import os
from pathlib import Path
import random
import select
import socket
import struct
import subprocess
import sys
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "runtime"))
import pictochat_net as net


def pair(**keywords):
    return net.Node(sender=b"A" * 8), net.Node(sender=b"B" * 8)


def hear(node, packets, now=1.0):
    events = []
    for packet in packets:
        events += node.receive(packet, now)
    return events


class PrivilegeTests(unittest.TestCase):
    def test_root_is_given_up_after_binding_and_failure_to_do_so_stops_the_helper(self):
        from unittest import mock
        with mock.patch.object(net.os, "geteuid", return_value=1000), mock.patch.object(net.os, "setuid") as setuid:
            net.drop_privileges()
            setuid.assert_not_called()  # nothing to give up
        calls = []
        ids = iter([0, 0, 65534, 65534])  # root until setuid/setgid run
        state = {"uid": 0, "gid": 0}
        with mock.patch.object(net.os, "geteuid", side_effect=lambda: state["uid"]), \
             mock.patch.object(net.os, "getegid", side_effect=lambda: state["gid"]), \
             mock.patch.object(net.os, "setgroups", side_effect=lambda g: calls.append(("groups", tuple(g)))), \
             mock.patch.object(net.os, "setgid", side_effect=lambda g: (calls.append(("gid", g)), state.update(gid=g))), \
             mock.patch.object(net.os, "setuid", side_effect=lambda u: (calls.append(("uid", u)), state.update(uid=u))):
            net.drop_privileges()
        self.assertEqual(calls, [("groups", ()), ("gid", 65534), ("uid", 65534)])  # groups and group first, user last
        with mock.patch.object(net.os, "geteuid", return_value=0), mock.patch.object(net.os, "setgroups"), \
             mock.patch.object(net.os, "setgid", side_effect=PermissionError("no")), mock.patch.object(net.sys.stderr, "write"):
            with self.assertRaises(SystemExit):
                net.drop_privileges()
        with mock.patch.object(net.os, "geteuid", return_value=0), mock.patch.object(net.os, "getegid", return_value=0), \
             mock.patch.object(net.os, "setgroups"), mock.patch.object(net.os, "setgid"), mock.patch.object(net.os, "setuid"):
            with self.assertRaises(SystemExit):  # still root after "dropping": refuse to run
                net.drop_privileges()


class ProtocolTests(unittest.TestCase):
    def test_hello_names_a_user_once_until_something_changes(self):
        a, b = pair()
        b.join(0, 0, "Me", 0)
        hello = a.join(0, 3, "Anna", 1.0)
        events = hear(b, hello)
        self.assertEqual(events, ["USER %s 0 3 Anna" % (b"A" * 8).hex()])
        self.assertEqual(hear(b, hello), [])
        self.assertEqual(hear(b, a.join(0, 4, "Anna", 2.0))[0].split()[3], "4")

    def test_bye_and_silence_both_remove_a_user(self):
        a, b = pair()
        b.join(0, 0, "Me", 0)
        hear(b, a.join(0, 3, "Anna", 1.0))
        self.assertEqual(hear(b, a.leave()), ["GONE " + (b"A" * 8).hex()])
        hear(b, a.join(0, 3, "Anna", 2.0), now=2.0)
        out, events = b.tick(2.0 + net.USER_TTL + .1)
        self.assertEqual(events, ["GONE " + (b"A" * 8).hex()])

    def test_hellos_repeat_on_a_timer_and_only_while_in_a_room(self):
        a, _ = pair()
        self.assertEqual(a.tick(0)[0], [])
        a.join(1, 2, "Anna", 10.0)
        self.assertEqual(a.tick(10.5)[0], [])
        self.assertEqual(len(a.tick(10.0 + net.HELLO_EVERY)[0]), 1)
        a.leave()
        self.assertEqual(a.tick(100)[0], [])

    def test_changing_room_says_goodbye_in_the_old_one(self):
        a, b = pair()
        b.join(0, 0, "Me", 0)
        hear(b, a.join(0, 3, "Anna", 1.0))
        moved = a.join(2, 3, "Anna", 2.0)
        self.assertEqual(len(moved), 2)
        self.assertEqual(hear(b, moved), ["GONE " + (b"A" * 8).hex(), "USER %s 2 3 Anna" % (b"A" * 8).hex()])

    def test_messages_cross_in_pieces_in_any_order_and_arrive_once(self):
        a, b = pair()
        b.join(0, 0, "Me", 0)
        hear(b, a.join(0, 3, "Anna", 1.0))
        payload = os.urandom(net.CHUNK_BYTES * 2 + 123)
        packets = a.send(payload) if a.room is not None else []
        self.assertEqual(len(packets), 6)  # three pieces, twice
        random.Random(4).shuffle(packets)
        events = hear(b, packets)
        self.assertEqual(events, ["MSG %s %s" % ((b"A" * 8).hex(), payload.hex())])

    def test_a_lost_piece_means_no_message_and_the_buffer_expires(self):
        a, b = pair()
        b.join(0, 0, "Me", 0)
        hear(b, a.join(0, 3, "Anna", 1.0))
        packets = a.send(os.urandom(net.CHUNK_BYTES * 2 + 1))[:2]
        self.assertEqual(hear(b, packets), [])
        self.assertEqual(len(b.pending), 1)
        b.tick(1.0 + net.PENDING_TTL + .5)
        self.assertEqual(len(b.pending), 0)

    def test_other_rooms_strangers_and_garbage_are_ignored(self):
        a, b = pair()
        b.join(0, 0, "Me", 0)
        a.join(1, 3, "Anna", 1.0)
        self.assertEqual(hear(b, a.send(b"hello")), [])  # never said hello to B, and a different room
        a.join(0, 3, "Anna", 2.0)
        self.assertEqual(hear(b, a.send(b"hello")), [])  # said hello to nobody B has heard
        hear(b, a.join(0, 3, "Anna", 3.0))
        self.assertEqual(len(hear(b, a.send(b"hello"))), 1)
        self.assertEqual(hear(b, [b"RGPC"]), [])
        own = net.Node(sender=b"B" * 8)
        own.join(0, 1, "Me", 0)
        self.assertEqual(hear(b, own.send(b"x")), [])  # our own id coming back

    def test_chatty_senders_are_limited_and_recover(self):
        a, b = pair()
        b.join(0, 0, "Me", 0)
        hear(b, a.join(0, 3, "Anna", 1.0))
        heard = sum(len(hear(b, a.send(b"msg %d" % i), now=1.0)) for i in range(20))
        self.assertEqual(heard, int(net.BUCKET_BURST))
        self.assertEqual(len(hear(b, a.send(b"later"), now=5.0)), 1)

    def test_a_room_holds_sixteen(self):
        b = net.Node(sender=b"B" * 8)
        b.join(0, 0, "Me", 0)
        seen = []
        for i in range(20):
            other = net.Node(sender=bytes([i]) * 8)
            seen += hear(b, other.join(0, i % 16, "User%d" % i, 1.0))
        self.assertEqual(sum(1 for e in seen if e.startswith("USER")), net.ROOM_LIMIT)
        self.assertEqual(b.room_users(0), net.ROOM_LIMIT)  # the extra four are not part of the room
        self.assertNotIn("FULL", seen)  # existing members are not told anything
        packets, full = b.try_join(0, 1, "Me", 1.0)  # already inside: not turned away
        self.assertFalse(full)
        late = net.Node(sender=b"L" * 8)
        for other in range(16):
            late.receive(net.pack_hello(bytes([other + 1]) * 8, 2, 3, "U%d" % other), 1.0)
        self.assertEqual(
            late.try_join(2, 0, "Latecomer", 2.0), ([], True)
        )  # a full room turns you away, sending nothing
        self.assertEqual(late.room, None)
        packets, full = late.try_join(3, 0, "Latecomer", 2.0)  # another room is fine
        self.assertFalse(full)
        self.assertEqual(len(packets), 1)

    def test_names_are_cleaned_and_bad_ones_refused(self):
        self.assertEqual(net.clean_name("  Anna \t Lise "), "Anna Lise")
        self.assertEqual(net.clean_name("\x00\x1b[31m"), "[31m")  # controls are dropped, never sent
        self.assertEqual(net.clean_name("\x00\x1b"), "Guest")
        self.assertEqual(net.clean_name(""), "Guest")
        self.assertLessEqual(len(net.clean_name("é" * 40).encode()), net.NAME_BYTES)
        self.assertIsNone(net.printable_name(b"\xff\xfe"))
        self.assertIsNone(net.printable_name(b"a\nb"))
        self.assertIsNone(net.printable_name("a\u0085b".encode()))
        b = net.Node(sender=b"B" * 8)
        b.join(0, 0, "Me", 0)
        for body in (
            struct.pack("<BB", 3, 3) + b"\xff\xfe\xfd", struct.pack("<BB", 99, 1) + b"x",
            struct.pack("<BB", 3, 5) + b"abc"
        ):
            data = net.MAGIC + struct.pack("<BBB8s", 1, net.HELLO, 0, b"Z" * 8) + body
            self.assertEqual(b.receive(data, 1.0), [])

    def test_hostile_datagrams_never_raise_and_only_ever_make_wellformed_events(self):
        a, b = pair()
        b.join(0, 0, "Me", 0)
        valid = a.join(0, 3, "Anna", 1.0) + a.send(os.urandom(2000))
        rng = random.Random(7)
        events = []
        for packet in valid:
            for n in range(len(packet)):
                events += b.receive(packet[:n], 1.0)
        for _ in range(30000):
            packet = bytearray(rng.choice(valid))
            for _k in range(rng.randint(1, 5)):
                packet[rng.randrange(len(packet))] = rng.randrange(256)
            events += b.receive(bytes(packet), rng.uniform(0, 100))
            events += b.receive(os.urandom(rng.randint(0, 1200)), 1.0)
            b.tick(rng.uniform(0, 100))
        events += b.receive(b"x" * 5000, 1.0)
        for event in events:
            word = event.split(" ", 1)[0]
            self.assertIn(word, ("USER", "GONE", "MSG", "FULL"))
            self.assertNotIn("\n", event)
            self.assertTrue(event.isprintable())
        self.assertLessEqual(len(b.pending), net.PENDING_LIMIT)
        self.assertLessEqual(len(b.delivered), 256)

    def test_commands_from_the_launcher_are_checked(self):
        self.assertEqual(net.parse_command("HELLO 2 7 Anna Lise\n"), ("HELLO", 2, 7, "Anna Lise"))
        self.assertEqual(net.parse_command("HELLO 0 0"), ("HELLO", 0, 0, "Guest"))
        for bad in (
            "HELLO 4 0 x", "HELLO 0 16 x", "HELLO x 0 x", "SEND zz", "SEND ", "SEND 00" + "ff" * net.MAX_PAYLOAD,
            "LEAVE now", "RUN", ""
        ):
            self.assertIsNone(net.parse_command(bad), bad)
        self.assertEqual(net.parse_command("SEND 0a0b"), ("SEND", b"\x0a\x0b"))
        self.assertEqual(net.parse_command("QUIT"), ("QUIT",))


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


class Helper:
    """A real helper process wired to a peer's port on loopback."""
    def __init__(self, port, peer):
        self.port = port
        self.process = subprocess.Popen(
            [
                sys.executable, "-B",
                str(ROOT / "runtime/pictochat_net.py"), "--port",
                str(port), "--bind", "127.0.0.1", "--targets",
                "127.0.0.1:%d" % peer
            ],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1
        )
        self.lines = []
        self.partial = b""

    def command(self, line):
        self.process.stdin.write(line + "\n")
        self.process.stdin.flush()

    def wait_for(self, prefix, seconds=6.0):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            for line in self.lines:
                if line.startswith(prefix):
                    self.lines.remove(line)
                    return line
            ready, _, _ = select.select([self.process.stdout.fileno()], [], [], .1)
            if ready:
                chunk = os.read(self.process.stdout.fileno(), 65536)
                if not chunk:
                    break
                self.partial += chunk
                while b"\n" in self.partial:
                    line, self.partial = self.partial.split(b"\n", 1)
                    self.lines.append(line.decode("utf-8"))
        raise AssertionError(
            "no %r from helper %d; heard %r; stderr %r" % (prefix, self.port, self.lines, self.stderr_now())
        )

    def stderr_now(self):
        if self.process.poll() is None:
            return ""
        return self.process.stderr.read()

    def stop(self):
        try:
            self.command("QUIT")
        except (BrokenPipeError, ValueError):
            pass
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
        for stream in (self.process.stdin, self.process.stdout, self.process.stderr):
            try:
                stream.close()
            except (OSError, ValueError, BrokenPipeError):
                pass


class TwoProcessTests(unittest.TestCase):
    def test_two_helpers_meet_chat_and_survive_hostile_packets(self):
        port_a, port_b = free_port(), free_port()
        a, b = Helper(port_a, port_b), Helper(port_b, port_a)
        try:
            sender_a = a.wait_for("READY").split()[1]
            sender_b = b.wait_for("READY").split()[1]
            a.wait_for("ONLINE")
            a.command("HELLO 0 3 Anna")
            b.command("HELLO 0 9 Bjørn")
            self.assertEqual(b.wait_for("USER").split(" ", 4)[1:], [sender_a, "0", "3", "Anna"])
            self.assertEqual(a.wait_for("USER").split(" ", 4)[1:], [sender_b, "0", "9", "Bjørn"])
            payload = os.urandom(3000)
            a.command("SEND " + payload.hex())
            self.assertEqual(b.wait_for("MSG"), "MSG %s %s" % (sender_a, payload.hex()))
            # Garbage from anywhere does not hurt it, and it keeps answering.
            rng = random.Random(3)
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as attacker:
                for _ in range(3000):
                    attacker.sendto(os.urandom(rng.randint(0, 1400)), ("127.0.0.1", port_b))
                    attacker.sendto(net.MAGIC + os.urandom(rng.randint(0, 40)), ("127.0.0.1", port_b))
            self.assertIsNone(b.process.poll())
            a.command("SEND " + b"after".hex())
            self.assertEqual(b.wait_for("MSG"), "MSG %s %s" % (sender_a, b"after".hex()))
            a.command("LEAVE")
            self.assertEqual(b.wait_for("GONE"), "GONE " + sender_a)
            # Bad commands are ignored, not fatal.
            b.command("SEND notahex")
            b.command("\x00\x01 junk")
            b.command("HELLO 0 9 Bjørn")
            self.assertIsNone(b.process.poll())
        finally:
            a.stop()
            b.stop()


if __name__ == "__main__":
    unittest.main()
