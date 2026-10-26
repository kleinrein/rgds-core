#!/usr/bin/env python3
"""Which shared libraries PortMaster's aarch64 ports need that a handheld lacks.

Reads every port zip in ports.json without downloading it whole: the zip's
central directory and its program and library members are fetched with HTTP
range requests, each aarch64 ELF's DT_NEEDED entries are read, and what the
port bundles itself (libs.aarch64/ and friends) is taken away. What is left,
minus the handheld's own libraries (one name per line, as
`find /lib /usr/lib -name '*.so*' | sed 's|.*/||'` prints them), is what the
compat layer (runtime/portmaster_compat.py) has to provide.

    python3 tools/portmaster_audit.py --device-libs device-libs.txt [--ports ports.json] [--only rtr]

Fetched members are cached in .cache/portmaster-audit/ (keyed by the zip's MD5).
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import re
import struct
import sys
import urllib.request
import zlib

ROOT = Path(__file__).resolve().parents[1]
CACHE = ROOT / '.cache/portmaster-audit'
PORTS_URL = 'https://github.com/PortsMaster/PortMaster-New/releases/latest/download/ports.json'
# Loaded by the dynamic linker itself or always present with glibc.
BASE = {
    'ld-linux-aarch64.so.1', 'libc.so.6', 'libm.so.6', 'libdl.so.2', 'libpthread.so.0', 'librt.so.1', 'libresolv.so.2',
    'libutil.so.1', 'libanl.so.1', 'libcrypt.so.1', 'libgcc_s.so.1', 'libstdc++.so.6'
}
# Android libraries (GameMaker's libyoyo.so and friends) name bionic's libraries; PortMaster's gmloader
# provides those itself.
ANDROID = {
    'libc.so', 'libdl.so', 'liblog.so', 'libm.so', 'libandroid.so', 'libOpenSLES.so', 'libEGL.so', 'libGLESv1_CM.so',
    'libGLESv2.so', 'libGLESv3.so', 'libz.so', 'libstdc++.so', 'libjnigraphics.so', 'libmediandk.so', 'libOpenMAXAL.so',
    'libaaudio.so', 'libvulkan.so'
}
BINARY = re.compile(r'(\.so(\.[0-9.]+)?$|\.aarch64$|(^|/)bin/[^/.]+$|(^|/)[^/.]+$)')


def fetch(url, start, end):
    request = urllib.request.Request(url, headers={'Range': 'bytes=%d-%d' % (start, end), 'User-Agent': 'rgds-audit'})
    with urllib.request.urlopen(request, timeout=60) as response:
        data = response.read()
        if response.status == 200: data = data[start:end + 1]  # server ignored the range
        return data


def total_size(url):
    """(size, final url): GitHub answers with a redirect to a signed address, which is reused for every range."""
    request = urllib.request.Request(url, headers={'Range': 'bytes=0-0', 'User-Agent': 'rgds-audit'})
    with urllib.request.urlopen(request, timeout=60) as response:
        return int(response.headers['Content-Range'].split('/')[1]), response.geturl()


def central_directory(url, size):
    tail = fetch(url, max(0, size - 65558), size - 1)
    at = tail.rfind(b'PK\x05\x06')
    if at < 0: raise ValueError('no end of central directory')
    count, cd_size, cd_offset = struct.unpack_from('<HII', tail, at + 10)
    if cd_offset == 0xFFFFFFFF or count == 0xFFFF:  # zip64
        loc = tail.rfind(b'PK\x06\x07')
        eocd64 = struct.unpack_from('<Q', tail, loc + 8)[0]
        record = fetch(url, eocd64, eocd64 + 55)
        count, cd_size, cd_offset = struct.unpack_from('<QQQ', record, 32)
    data = fetch(url, cd_offset, cd_offset + cd_size - 1)
    members, pos = [], 0
    for _ in range(count):
        (method, csize, usize, name_len, extra_len, comment_len, ext_attr,
         offset) = struct.unpack_from('<10xH8xIIHHH4xII', data, pos)
        name = data[pos + 46:pos + 46 + name_len].decode('utf-8', 'replace')
        extra = data[pos + 46 + name_len:pos + 46 + name_len + extra_len]
        if 0xFFFFFFFF in (csize, usize, offset):  # zip64 sizes in the extra field
            at = 0
            while at + 4 <= len(extra):
                tag, length = struct.unpack_from('<HH', extra, at)
                if tag == 1:
                    values = list(struct.unpack_from('<%dQ' % (length // 8), extra, at + 4))
                    if usize == 0xFFFFFFFF: usize = values.pop(0)
                    if csize == 0xFFFFFFFF: csize = values.pop(0)
                    if offset == 0xFFFFFFFF: offset = values.pop(0)
                at += 4 + length
        crc, = struct.unpack_from('<I', data, pos + 16)
        members.append(
            dict(name=name, method=method, csize=csize, usize=usize, offset=offset, attr=ext_attr >> 16, crc=crc)
        )
        pos += 46 + name_len + extra_len + comment_len
    return members


def member_bytes(url, member):
    # One request: the local header (its name and extra field are rarely longer than 1 KiB) and the data.
    guess = 30 + len(member['name'].encode()) + 1024
    blob = fetch(url, member['offset'], member['offset'] + guess + member['csize'] - 1)
    name_len, extra_len = struct.unpack_from('<HH', blob, 26)
    start = 30 + name_len + extra_len
    if start + member['csize'] > len(blob):
        blob += fetch(url, member['offset'] + len(blob), member['offset'] + start + member['csize'] - 1)
    raw = blob[start:start + member['csize']]
    if member['method'] == 0: return raw
    if member['method'] == 8: return zlib.decompress(raw, -15)
    raise ValueError('compression method %d' % member['method'])


def elf_dynamic(data, machine=183):
    """(soname, [needed]) of a little-endian ELF for machine (183 aarch64, 40 32-bit ARM); None for anything else."""
    if data[:4] != b'\x7fELF' or data[5] != 1: return None
    wide = data[4] == 2
    if struct.unpack_from('<H', data, 18)[0] != machine: return None
    if wide:
        phoff, = struct.unpack_from('<Q', data, 32)
        phentsize, phnum = struct.unpack_from('<HH', data, 54)
    else:
        phoff, = struct.unpack_from('<I', data, 28)
        phentsize, phnum = struct.unpack_from('<HH', data, 42)
    loads, dynamic = [], None
    for i in range(phnum):
        at = phoff + i * phentsize
        if wide: kind, _flags, offset, vaddr, _paddr, filesz = struct.unpack_from('<IIQQQQ', data, at)
        else: kind, offset, vaddr, _paddr, filesz = struct.unpack_from('<IIIII', data, at)
        if kind == 1: loads.append((vaddr, offset, filesz))
        elif kind == 2: dynamic = (offset, filesz)
    if not dynamic: return ('', [])  # static
    entries, strtab, size, form = [], None, 16 if wide else 8, '<qQ' if wide else '<iI'
    for i in range(dynamic[1] // size):
        tag, value = struct.unpack_from(form, data, dynamic[0] + i * size)
        if tag == 0: break
        if tag == 5: strtab = value
        entries.append((tag, value))
    if strtab is None: return ('', [])
    base = next((offset + strtab - vaddr for vaddr, offset, size in loads if vaddr <= strtab < vaddr + size), None)
    if base is None: return ('', [])

    def string(at):
        end = data.index(b'\0', base + at)
        return data[base + at:end].decode('utf-8', 'replace')

    return (next((string(v) for t, v in entries if t == 14), ''), [string(v) for t, v in entries if t == 1])


SEEN = {}  # (crc, size) -> ELF info, shared by ports that bundle the same library


def audit_port(name, port):
    url, md5 = port['source']['url'], port['source']['md5']
    cache = CACHE / (md5 + '.json')
    if cache.is_file(): return json.loads(cache.read_text())
    size, url = total_size(url)
    members = central_directory(url, size)
    elves, scripts, names = [], [], [m['name'] for m in members]
    for member in members:
        if member['name'].endswith('/') or member['usize'] < 1024 or member['usize'] > 64 * 1024 * 1024: continue
        if member['name'].endswith('.sh') and member['name'].count('/') == 0:
            scripts.append(member_bytes(url, member).decode('utf-8', 'replace'))
            continue
        if not BINARY.search(member['name']): continue
        key = (member['crc'], member['usize'])
        if key not in SEEN: SEEN[key] = elf_dynamic(member_bytes(url, member))
        info = SEEN[key]
        if info is not None: elves.append(dict(name=member['name'], soname=info[0], needed=info[1]))
    result = dict(name=name, files=names, elves=elves, scripts=scripts)
    CACHE.mkdir(parents=True, exist_ok=True)
    cache.write_text(json.dumps(result))
    return result


def missing(result, device):
    """Libraries the port's binaries need that neither the port nor the handheld provides."""
    bundled = {Path(f).name for f in result['files']} | {e['soname'] for e in result['elves'] if e['soname']}
    need = {}
    for elf in result['elves']:
        for lib in elf['needed']:
            if lib in BASE or lib in ANDROID or lib in device or lib in bundled: continue
            need.setdefault(lib, []).append(elf['name'])
    return need


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--device-libs', type=Path, required=True)
    parser.add_argument('--ports', type=Path, help='ports.json (downloaded when left out)')
    parser.add_argument('--only', choices=('rtr', 'all'), default='all')
    parser.add_argument('--jobs', type=int, default=12)
    parser.add_argument('--json', type=Path, help='write the per-port result here')
    args = parser.parse_args()
    data = json.loads(args.ports.read_text() if args.ports else urllib.request.urlopen(PORTS_URL).read())
    device = {line.strip() for line in args.device_libs.read_text().splitlines() if line.strip()}
    chosen = {
        name: port
        for name, port in data['ports'].items()
        # An empty arch list is a port with no binaries of its own (a runtime runs it): checked too.
        if (not port['attr']['arch'] or 'aarch64' in port['attr']['arch']) and
        (args.only == 'all' or port['attr']['rtr'])
    }
    results, failures = {}, {}
    with ThreadPoolExecutor(args.jobs) as pool:
        futures = {pool.submit(audit_port, name, port): name for name, port in chosen.items()}
        for done, future in enumerate(futures, 1):
            name = futures[future]
            try:
                results[name] = future.result()
            except Exception as error:  # a broken or moved zip: reported, the rest goes on
                failures[name] = str(error)
            if done % 50 == 0: print('%d/%d' % (done, len(chosen)), file=sys.stderr, flush=True)
    by_lib, broken = {}, {}
    for name, result in results.items():
        need = missing(result, device)
        if need: broken[name] = sorted(need)
        for lib in need:
            by_lib.setdefault(lib, []).append(name)
    print(
        'ports checked: %d, need something the handheld lacks: %d, unreadable: %d' %
        (len(results), len(broken), len(failures))
    )
    for lib, names in sorted(by_lib.items(), key=lambda item: (-len(item[1]), item[0])):
        print('%4d  %-32s %s' % (len(names), lib, ', '.join(sorted(names)[:6]) + (' ...' if len(names) > 6 else '')))
    for name, error in sorted(failures.items()):
        print('unreadable: %s: %s' % (name, error))
    if args.json:
        args.json.write_text(
            json.dumps(dict(broken=broken, by_lib=by_lib, failures=failures, checked=len(results)), indent=1)
        )
    return 0


if __name__ == '__main__':
    sys.exit(main())
