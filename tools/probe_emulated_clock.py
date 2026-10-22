"""Read only the original RGDS homebrew clock from an owned benchmark engine.

No general memory dump is exposed. Discovery is bounded and sampling reads
only the validated 32-byte magic/count/inverse record. Device mappings are
excluded; a caller may explicitly allow DraStic's literal /dev/zero RAM map
or its exact shared-RAM file after matching an owned regular-file descriptor.
"""
import json
import os
from pathlib import Path
import re
import stat
import struct
import time

MAGIC = b'RGDSFRAMEPROBE01'
MASK = (1 << 64) - 1
MAP_LIMIT = 64 * 1024 * 1024
SCAN_LIMIT = 128 * 1024 * 1024
CHUNK = 1024 * 1024


def _read_small(path, limit):
    with Path(path).open('rb') as source:
        value = source.read(limit + 1)
    if len(value) > limit:
        raise ValueError('Benchmark metadata exceeds its size limit')
    return value


def _identity(proc, pid):
    fields = _read_small(proc / str(pid) / 'stat', 4096).decode().rsplit(')', 1)[1].split()
    if len(fields) < 20 or fields[0] in ('Z', 'X'):
        raise ValueError('Benchmark process is no longer running')
    return dict(pid=int(pid), parent=int(fields[1]), start=fields[19])


def _mapping(line, allow_zero, drastic_identity=None):
    fields = line.split(None, 5)
    if len(fields) < 5 or not fields[1].startswith('rw'):
        return None
    name = fields[5].strip() if len(fields) == 6 else ''
    anonymous = not name or name == '[heap]' or name.startswith('[anon:')
    zero = allow_zero and name in ('/dev/zero', '/dev/zero (deleted)')
    drastic = (
        drastic_identity is not None
        and name in ('/dev/shm/drastic_mapped_memory.dat', '/dev/shm/drastic_mapped_memory.dat (deleted)')
        and (fields[3], fields[4]) == drastic_identity and int(fields[2], 16) == 0
    )
    if not (anonymous or zero or drastic):
        return None
    start, end = (int(part, 16) for part in fields[0].split('-'))
    if end - start < 32 or end - start > MAP_LIMIT:
        return None
    if drastic and end - start != 4 * 1024 * 1024:
        return None  # One complete main-RAM alias; skip WRAM and repeated views.
    return dict(start=start, end=end, zero=bool(zero), drastic_ram=bool(drastic))


def _count(data):
    if len(data) != 32 or data[:16] != MAGIC:
        return None
    count, inverse = struct.unpack_from('<QQ', data, 16)
    return count if count ^ inverse == MASK else None


class EmulatedClock:
    def __init__(
        self,
        pid,
        benchmark_root,
        rom_path,
        *,
        allow_zero_mapping=False,
        allow_drastic_mapping=False,
        _proc_root='/proc'
    ):
        self.proc = Path(_proc_root)
        self.pid = int(pid)
        self.root = Path(benchmark_root).resolve(strict=True)
        rom_argument = os.fsencode(os.path.abspath(rom_path))
        self.rom = Path(rom_path).resolve(strict=True)
        self.allow_zero = bool(allow_zero_mapping)
        self.allow_drastic = bool(allow_drastic_mapping)
        self.fd = None
        self.address = None
        self.last_count = None
        self.candidates = []
        self.discovery_status = {}
        containers = [p for p in self.root.parents if re.fullmatch(r'benchmark-[a-zA-Z0-9_-]+', p.name)]
        if self.root.name != 'rgds' or not containers or not self.rom.is_relative_to(containers[0]):
            raise ValueError('Clock probe requires an isolated benchmark root and ROM')
        with self.rom.open('rb') as rom:
            header = rom.read(18)
        if header[:16] not in (b'RGDS PROBE  ####', b'RGDS 3D TEST####'):
            raise ValueError('Clock probe is restricted to the original RGDS homebrew ROM')
        self.owner_path = self.root / 'userdata/session/core.json'
        self.owner = json.loads(_read_small(self.owner_path, 4096))
        self.owner = dict(pid=int(self.owner['pid']), start=str(self.owner['start']))
        if self.pid <= 1 or self.pid == self.owner['pid']:
            raise ValueError('Clock target must be an owned emulator child')
        self.start = _identity(self.proc, self.pid)['start']
        self._check_owner()
        args = _read_small(self.proc / str(self.pid) / 'cmdline', 65536).split(b'\0')
        if not {os.fsencode(self.rom), rom_argument}.intersection(args):
            raise ValueError('Target command does not contain the isolated probe ROM')

    def _check_owner(self):
        current = json.loads(_read_small(self.owner_path, 4096))
        if int(current.get('pid', 0)) != self.owner['pid'] or str(current.get('start')) != self.owner['start']:
            raise ValueError('Benchmark ownership changed')
        if _identity(self.proc, self.owner['pid'])['start'] != self.owner['start']:
            raise ValueError('Benchmark owner identity changed')
        process = _identity(self.proc, self.pid)
        if process['start'] != self.start:
            raise ValueError('Target process identity changed')
        seen = {self.pid}
        for _ in range(16):
            parent = process['parent']
            if parent == self.owner['pid']:
                return
            if parent <= 1 or parent in seen:
                break
            seen.add(parent)
            process = _identity(self.proc, parent)
        raise ValueError('Target process is not descended from the benchmark owner')

    def _drastic_identity(self):
        if not self.allow_drastic:
            return None
        self._check_owner()
        identities = set()
        # Inspect metadata only: never open a backing /dev/shm or device path.
        directory = self.proc / str(self.pid) / 'fd'
        entries = list(directory.iterdir())
        if len(entries) > 256:
            raise ValueError('Benchmark file-descriptor count exceeds probe limit')
        for entry in entries:
            if not entry.name.isdigit():
                continue
            try:
                target = os.readlink(entry)
                if target not in ('/dev/shm/drastic_mapped_memory.dat', '/dev/shm/drastic_mapped_memory.dat (deleted)'):
                    continue
                info = entry.stat()
            except FileNotFoundError:
                continue
            if stat.S_ISREG(info.st_mode) and 4 * 1024 * 1024 <= info.st_size <= MAP_LIMIT:
                identities.add((f'{os.major(info.st_dev):02x}:{os.minor(info.st_dev):02x}', str(info.st_ino)))
        self._check_owner()
        if len(identities) != 1:
            raise ValueError('Expected one owned DraStic shared-RAM file')
        return identities.pop()

    def _maps(self):
        result = []
        identity = self._drastic_identity()
        seen_drastic = False
        for line in _read_small(self.proc / str(self.pid) / 'maps', 1024 * 1024).decode().splitlines():
            mapping = _mapping(line, self.allow_zero, identity)
            if mapping:
                if mapping['drastic_ram']:
                    if seen_drastic:
                        continue
                    seen_drastic = True
                result.append(mapping)
        if identity is not None:
            result = [mapping for mapping in result if mapping['drastic_ram']]
            if not result:
                raise ValueError('Owned DraStic main-RAM mapping is absent')
        # DraStic's canonical DS RAM is an exact 4 MiB map. Scan smaller maps
        # before large allocator areas, with a total cap independent of RSS.
        return sorted(
            result, key=lambda m: (m['end'] - m['start'] != 4 * 1024 * 1024, m['end'] - m['start'], m['start'])
        )

    def _read(self, address, size):
        self._check_owner()
        if self.fd is None:
            self.fd = os.open(self.proc / str(self.pid) / 'mem', os.O_RDONLY | os.O_CLOEXEC)
            self._check_owner()
        data = os.pread(self.fd, size, address)
        self._check_owner()
        return data

    def find(self, settle_seconds=.08):
        if not .02 <= settle_seconds <= .5:
            raise ValueError('Discovery observation must be between 20 and 500 milliseconds')
        candidates = []
        scanned = 0
        exhausted = False
        mappings = self._maps()
        unreadable = 0
        for mapping in mappings:
            position = mapping['start']
            while position < mapping['end']:
                amount = min(CHUNK, mapping['end'] - position, SCAN_LIMIT - scanned)
                if amount <= 0:
                    exhausted = True
                    break
                try:
                    data = self._read(position, amount)
                except OSError:
                    unreadable += 1
                    break
                scanned += len(data)
                offset = data.find(MAGIC)
                while offset >= 0:
                    count = _count(data[offset:offset + 32])
                    if count is not None:
                        candidates.append(dict(address=position + offset, count=count, mapping=mapping))
                        if len(candidates) > 64:
                            raise ValueError('Too many candidate probe clocks')
                    offset = data.find(MAGIC, offset + 16)
                if len(data) < amount or amount < 32:
                    break
                position += amount - 31 if position + amount < mapping['end'] else amount
            if exhausted:
                break
        time.sleep(settle_seconds)
        advancing = []
        seen = set()
        for candidate in candidates:
            address = candidate['address']
            if address in seen:
                continue
            seen.add(address)
            count = _count(self._read(address, 32))
            if count is not None and 0 < count - candidate['count'] <= 1000000:
                advancing.append(dict(candidate, count=count))
        self.discovery_status = dict(
            eligible_maps=len(mappings),
            unreadable_maps=unreadable,
            scanned_bytes=scanned,
            candidates=len(candidates),
            advancing=len(advancing),
            scan_limit_reached=exhausted
        )
        if not advancing:
            raise ValueError(
                'No advancing homebrew clock found in allowed benchmark memory: ' +
                json.dumps(self.discovery_status, sort_keys=True)
            )
        advancing.sort(
            key=lambda p: (
                not (
                    p['mapping']['end'] - p['mapping']['start'] == 4 * 1024 * 1024 and p['address'] - p['mapping'][
                        'start'] == 0x1000
                ), p['address']
            )
        )
        # Cross-check mirrors inside two reads of the chosen clock. Their reads
        # are sequential, so equality or a fixed small tolerance would reject
        # correct mirrors when fast-forward advances during /proc metadata I/O.
        reference = advancing[0]['address']
        before = _count(self._read(reference, 32))
        observed = [_count(self._read(p['address'], 32)) for p in advancing]
        after = _count(self._read(reference, 32))
        if before is None or after is None or after < before or any(
            v is None or not before <= v <= after for v in observed
        ):
            raise ValueError('Different advancing homebrew clocks found; refusing an ambiguous sample')
        self.address = advancing[0]['address']
        self.candidates = [p['address'] for p in advancing]
        self.last_count = after
        return dict(
            pid=self.pid,
            start=self.start,
            address=hex(self.address),
            aliases=[hex(p) for p in self.candidates],
            scanned_bytes=scanned,
            scan_limit_reached=exhausted,
            zero_mapping=advancing[0]['mapping']['zero'],
            drastic_shared_ram=advancing[0]['mapping']['drastic_ram'],
            **self.sample()
        )

    def sample(self):
        if self.address is None:
            raise ValueError('Discover the benchmark clock before sampling')
        if not any(m['start'] <= self.address and self.address + 32 <= m['end'] for m in self._maps()):
            raise ValueError('Homebrew clock mapping changed')
        before = time.monotonic_ns()
        count = _count(self._read(self.address, 32))
        after = time.monotonic_ns()
        if count is None:
            raise ValueError('Homebrew clock was changed or sampled during an incomplete update')
        if self.last_count is not None and count < self.last_count:
            raise ValueError('Homebrew clock moved backwards; start a new measurement after restoring state')
        self.last_count = count
        return dict(vblank_count=count, monotonic_ns=(before + after) // 2, read_duration_ns=after - before)

    def close(self):
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()
