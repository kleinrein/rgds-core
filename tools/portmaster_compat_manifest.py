#!/usr/bin/env python3
"""Pin the Ubuntu 20.04 (focal) arm64 packages behind RG DS Core's PortMaster compat layer.

Ports are built for Ubuntu-style handheld systems (ArkOS and friends) and link
against libraries the stock RG DS card does not have (tools/portmaster_audit.py
lists them), and their scripts expect GNU tools where the card has BusyBox. The
handheld downloads these from Ubuntu's own archive at install time
(runtime/portmaster_compat.py); this tool decides exactly which packages and
files, and pins each package by version and SHA-256:

- every requested library, and then every library those need, by DT_NEEDED,
  until nothing is missing (glibc, libgcc and libstdc++ always come from the
  handheld, which has newer ones);
- nothing the handheld already has (--device-libs), so a port never gets a
  second copy of a library the system provides;
- the requested GNU tools as bin/<name>.

    python3 tools/portmaster_compat_manifest.py --device-libs device-libs.txt \\
        --libs libjpeg.so.8,libzstd.so.1 [--audit audit.json --min-ports 1] \\
        --out runtime/portmaster_files/compat.json

Downloads are cached in .cache/portmaster-compat/.
"""
import argparse
import gzip
import hashlib
import io
import json
import lzma
from pathlib import Path
import re
import sys
import tarfile
import urllib.request

sys.path.insert(0, str(Path(__file__).resolve().parent))
from portmaster_audit import BASE, elf_dynamic  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
CACHE = ROOT / '.cache/portmaster-compat'
MIRRORS = ['http://ports.ubuntu.com/ubuntu-ports', 'http://old-releases.ubuntu.com/ubuntu']
SUITES = ['focal', 'focal-updates', 'focal-security']
COMPONENTS = ['main', 'universe']
# Where a library focal does not have comes from instead, tried in this order (Wizznic and Yatka want bionic's
# libreadline.so.7, VCMI Debian buster's ICU 63). Package keys from these carry the source: 'bionic/libreadline7'.
FALLBACKS = {
    'bionic': dict(
        mirrors=MIRRORS,
        suites=['bionic', 'bionic-updates', 'bionic-security'],
        components=COMPONENTS,
        contents=['dists/bionic/Contents-%s.gz', 'dists/bionic-updates/Contents-%s.gz']
    ),
    'buster': dict(
        mirrors=['http://archive.debian.org/debian'],
        suites=['buster'],
        components=['main'],
        contents=['dists/buster/main/Contents-%s.gz']
    ),
}
LIBDIRS = ('usr/lib/aarch64-linux-gnu/', 'lib/aarch64-linux-gnu/')
# --arch armhf: the 32-bit layer for armhf-only ports (the card has no 32-bit userland at all).
ARCH = dict(name='arm64', machine=183, prefix='lib/', base=True)
ARMHF = dict(name='armhf', machine=40, prefix='armhf/lib/', base=False)
ARMHF_LIBDIRS = ('usr/lib/arm-linux-gnueabihf/', 'lib/arm-linux-gnueabihf/', 'usr/lib/arm-linux-gnueabihf/pulseaudio/')
# SDL2 opens these with dlopen, so no DT_NEEDED names them.
ARMHF_DLOPEN = ['libwayland-client.so.0', 'libwayland-cursor.so.0', 'libxkbcommon.so.0', 'libasound.so.2']
# GNU tools port scripts and PortMaster's funcs.txt expect where the card has BusyBox (or nothing):
# name in bin/ -> (package, path in the package).
TOOLS = {
    'tar': ('tar', 'bin/tar'),
    'sed': ('sed', 'bin/sed'),
    'grep': ('grep', 'bin/grep'),
    'egrep': ('grep', 'bin/egrep'),
    'fgrep': ('grep', 'bin/fgrep'),
    'gawk': ('gawk', 'usr/bin/gawk'),
    'awk': ('gawk', 'usr/bin/gawk'),
    'find': ('findutils', 'usr/bin/find'),
    'xargs': ('findutils', 'usr/bin/xargs'),
    'gzip': ('gzip', 'bin/gzip'),
    'gunzip': ('gzip', 'bin/gzip'),
    'unzip': ('unzip', 'usr/bin/unzip'),
    'killall': ('psmisc', 'usr/bin/killall'),
    'zstd': ('zstd', 'usr/bin/zstd'),
    'rsync': ('rsync', 'usr/bin/rsync'),
    'bc': ('bc', 'usr/bin/bc'),
    'xxd': ('xxd', 'usr/bin/xxd'),
    'cpio': ('cpio', 'bin/cpio'),
    'zramctl': ('util-linux', 'sbin/zramctl'),
}
# Wrapper scripts, not binaries: gunzip and the grep aliases call the real tool with a flag.
WRAPPERS = {
    'gunzip': '#!/bin/sh\nexec "$(dirname "$0")/gzip" -d "$@"\n',
    'egrep': '#!/bin/sh\nexec "$(dirname "$0")/grep" -E "$@"\n',
    'fgrep': '#!/bin/sh\nexec "$(dirname "$0")/grep" -F "$@"\n',
}
# Data files a tool needs beside it: tool -> [(package, path in the package, target)]. The card's own zip and
# file are real (not BusyBox), so neither is listed above.
DATA = {}


def get(url):
    request = urllib.request.Request(url, headers={'User-Agent': 'rgds-compat'})
    with urllib.request.urlopen(request, timeout=120) as response:
        return response.read()


def cached(name, url):
    path = CACHE / name
    if not path.is_file():
        CACHE.mkdir(parents=True, exist_ok=True)
        path.write_bytes(get(url))
    return path.read_bytes()


def version_key(version):
    """dpkg's ordering: epoch, then upstream and revision compared part by part (~ sorts first)."""
    epoch, _, rest = version.rpartition(':')
    upstream, _, revision = rest.rpartition('-') if '-' in rest else (rest, '', '0')

    def part(text):
        out = []
        for chunk in re.findall(r'\d+|\D+', text):
            if chunk.isdigit(): out.append((1, int(chunk), ''))
            else:
                out.append(
                    (
                        0, 0, ''.join(chr(1) if c == '~' else c if c.isalpha() else chr(ord(c) + 256)
                                      for c in chunk) + chr(2)
                    )
                )
        return out + [(0, 0, chr(2))]

    return (int(epoch or 0), part(upstream), part(revision))


def packages(mirror=None, suites=None, components=None):
    """The newest record of every package for ARCH (focal unless told otherwise): name -> fields."""
    best = {}
    mirror = mirror or MIRRORS[0]
    for suite in suites or SUITES:
        for component in components or COMPONENTS:
            raw = cached(
                '%s-%s-%s-Packages.xz' % (suite, component, ARCH['name']),
                '%s/dists/%s/%s/binary-%s/Packages.xz' % (mirror, suite, component, ARCH['name'])
            )
            for block in lzma.decompress(raw).decode('utf-8', 'replace').split('\n\n'):
                fields = dict(line.split(': ', 1) for line in block.splitlines() if ': ' in line and line[0] != ' ')
                name = fields.get('Package')
                if not name: continue
                if name not in best or version_key(fields['Version']) > version_key(best[name]['Version']):
                    best[name] = fields
    return best


def contents(mirror=None, paths=('dists/focal/Contents-%s.gz', 'dists/focal-updates/Contents-%s.gz')):
    """Path in a package -> package name, for the library folders only."""
    owner = {}
    for path in paths:
        path = path % ARCH['name']
        raw = cached(path.replace('dists/', '').replace('/', '-'), '%s/%s' % (mirror or MIRRORS[0], path))
        for line in gzip.decompress(raw).decode('utf-8', 'replace').splitlines():
            path, _, location = line.rpartition(' ')
            path = path.strip()
            if path.startswith(LIBDIRS) or path.startswith(('bin/', 'usr/bin/')):
                owner[path] = location.split(',')[0].split('/')[-1]
    return owner


def deb_members(package, mirror=None):
    """(sha256, size, {path: bytes or ('link', target)}) of a package's data.tar.xz."""
    data = cached(Path(package['Filename']).name, (mirror or MIRRORS[0]) + '/' + package['Filename'])
    digest = hashlib.sha256(data).hexdigest()
    if digest != package['SHA256']: raise ValueError('%s: SHA-256 mismatch' % package['Package'])
    pos, members = 8, {}
    while pos < len(data):
        name = data[pos:pos + 16].decode().strip().rstrip('/')
        size = int(data[pos + 48:pos + 58].decode().strip())
        body = data[pos + 60:pos + 60 + size]
        pos += 60 + size + (size & 1)
        if not name.startswith('data.tar'): continue
        if not name.endswith('.xz'): raise ValueError('%s: %s is not xz' % (package['Package'], name))
        with tarfile.open(fileobj=io.BytesIO(lzma.decompress(body))) as tar:
            for info in tar:
                path = info.name.lstrip('./')
                if info.issym(): members[path] = ('link', info.linkname)
                elif info.isfile(): members[path] = tar.extractfile(info).read()
    return digest, len(data), members


def resolve(members, path):
    """Follow symlinks inside one package."""
    for _ in range(8):
        value = members.get(path)
        if not isinstance(value, tuple): return path, value
        target = value[1]
        path = target.lstrip('/') if target.startswith('/') else str(Path(path).parent / target)
        path = re.sub(r'[^/]+/\.\./', '', path)
    return path, None


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--device-libs', type=Path, required=True)
    parser.add_argument('--libs', default='', help='comma-separated sonames to provide')
    parser.add_argument('--audit', type=Path, help="tools/portmaster_audit.py --json output: its libraries too")
    parser.add_argument(
        '--min-ports', type=int, default=1, help='with --audit: libraries at least this many ports need'
    )
    parser.add_argument('--tools', default=','.join(TOOLS), help='comma-separated tools for bin/')
    parser.add_argument('--arch', choices=('arm64', 'armhf'), default='arm64')
    parser.add_argument('--mali', default='', help='with --arch armhf: URL of the 32-bit libmali to pin')
    parser.add_argument('--gl-libs', default='libGLU.so.1', help='with --gl4es: libraries that go beside it')
    parser.add_argument('--gl4es', default='', help='URL#member of a PortMaster port zip carrying gl4es libGL.so.1')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    global LIBDIRS
    if args.arch == 'armhf':
        ARCH.update(ARMHF)
        LIBDIRS = ARMHF_LIBDIRS
    device = {line.strip() for line in args.device_libs.read_text().splitlines() if line.strip()}
    wanted = [lib for lib in args.libs.split(',') if lib]
    if args.audit:
        audit = json.loads(args.audit.read_text())
        wanted += [lib for lib, names in audit['by_lib'].items() if len(names) >= args.min_ports]
    index, owner = packages(), contents()
    mirrors_of, fallback_owners = {}, {}
    plan, provided, unresolved, cache = {}, {}, [], {}

    def members_of(name):
        if name not in cache: cache[name] = deb_members(index[name], mirrors_of.get(name, MIRRORS)[0])
        return cache[name]

    def owner_of(soname):
        """The package key with soname in a library folder: focal first, then the fallbacks."""
        found = next((owner[d + soname] for d in LIBDIRS if d + soname in owner), None)
        if found in index: return found
        for source, spec in FALLBACKS.items():
            if source not in fallback_owners:
                fallback_owners[source] = (
                    packages(spec['mirrors'][0], spec['suites'],
                             spec['components']), contents(spec['mirrors'][0], spec['contents'])
                )
            records, owners = fallback_owners[source]
            found = next((owners[d + soname] for d in LIBDIRS if d + soname in owners), None)
            if found in records:
                key = source + '/' + found
                index[key] = records[found]
                mirrors_of[key] = spec['mirrors']
                return key
        return None

    def take(name, source, target):
        plan.setdefault(name, {})[target] = source

    queue = list(dict.fromkeys(wanted + (ARMHF_DLOPEN if args.arch == 'armhf' else [])))
    mali = None
    if args.mali:
        # Rockchip's 32-bit libmali for this GPU (g24p0, Wayland + GBM), the 32-bit twin of the card's own driver.
        mali = cached('libmali-armhf-' + Path(args.mali).name, args.mali)
        queue += elf_dynamic(mali, ARCH['machine'])[1]
    for tool in [t for t in args.tools.split(',') if t]:
        package, path = TOOLS[tool]
        if tool in WRAPPERS: continue
        _digest, _size, members = members_of(package)
        real, data = resolve(members, path)
        if data is None: raise ValueError('%s: %s missing' % (package, path))
        take(package, real, 'bin/' + tool)
        queue += [lib for lib in elf_dynamic(data, ARCH['machine'])[1]]
        for data_package, source, target in DATA.get(tool, []):
            members_of(data_package)
            take(data_package, source, target)

    def close(queue, prefix, present):
        """Every library queue names, and theirs in turn, into prefix; present ones are skipped."""
        while queue:
            soname = queue.pop(0)
            if (ARCH['base'] and soname in BASE) or soname in present or soname in provided: continue
            package = owner_of(soname)
            if package is None:
                unresolved.append(soname)
                provided[soname] = None
                continue
            _digest, _size, members = members_of(package)
            path = next((d + soname for d in LIBDIRS if d + soname in members), None)
            real, data = resolve(members, path) if path else (None, None)
            if data is None:
                unresolved.append(soname)
                provided[soname] = None
                continue
            provided[soname] = package
            take(package, real, prefix + soname)
            queue += elf_dynamic(data, ARCH['machine'])[1]

    close(queue, ARCH['prefix'], device)
    if args.gl4es:
        # Libraries for desktop-GL ports that sit with gl4es (gl4es/, which only such ports get on their path):
        # GLU names libGL.so.1, and a libGL.so.1 every port sees changes what PortMaster's Weston runtime does.
        close([lib for lib in args.gl_libs.split(',') if lib], 'gl4es/', device | {'libGL.so.1'})
    if args.arch == 'armhf':
        # The 32-bit dynamic loader, which armhf programs name in PT_INTERP (runtime/portmaster_compat.py points
        # them at this copy).
        _digest, _size, members = members_of('libc6')
        real, data = resolve(members, 'lib/ld-linux-armhf.so.3')
        if data is None: raise ValueError('libc6 has no ld-linux-armhf.so.3')
        take('libc6', real, 'armhf/ld-linux-armhf.so.3')
    extras, local = [], []
    if mali is not None:
        extras.append(
            dict(
                url=args.mali, sha256=hashlib.sha256(mali).hexdigest(), size=len(mali), target='armhf/lib/libmali.so.1'
            )
        )
        # tools/build_compat_stubs.py: libEGL.so.1 and friends, each only naming libmali.so.1.
        stubs = ROOT / 'runtime/portmaster_files/armhf'
        local = [['armhf/' + stub.name, 'armhf/lib/' + stub.name] for stub in sorted(stubs.glob('lib*.so.*'))]
    if args.gl4es:
        # PortMaster's own gl4es build (desktop OpenGL over GLES), taken from a port zip that carries it, for ports
        # that need libGL.so.1 and do not bring gl4es themselves (Ren'Py, MiceAmaze).
        url, member = args.gl4es.split('#', 1)
        blob = cached(Path(url).name, url)
        import zipfile
        with zipfile.ZipFile(io.BytesIO(blob)) as zf:
            data = zf.read(member)
        extras.append(
            dict(
                zip=url,
                sha256=hashlib.sha256(blob).hexdigest(),
                size=len(blob),
                # Not in lib/: a libGL.so.1 every port sees changes what PortMaster's Weston runtime picks.
                files=[[member, 'gl4es/libGL.so.1', hashlib.sha256(data).hexdigest()]]
            )
        )
        queue_gl = [lib for lib in elf_dynamic(data)[1] if lib not in BASE and lib not in device]
        if queue_gl: print('gl4es also needs: ' + ', '.join(queue_gl))
        # tools/build_compat_stubs.py: libOpenGL.so.0 and libGLX.so.0 for glvnd ports, each naming libGL.so.1.
        local += [
            ['gl4es/' + stub.name, 'gl4es/' + stub.name]
            for stub in sorted((ROOT / 'runtime/portmaster_files/gl4es').glob('lib*.so*'))
        ]
    manifest = dict(
        schema=1,
        about='Ubuntu 20.04 (focal) arm64 packages for PortMaster ports on RG DS Core. Generated by '
        'tools/portmaster_compat_manifest.py; installed by runtime/portmaster_compat.py.',
        mirrors=MIRRORS,
        wrappers={tool: WRAPPERS[tool]
                  for tool in args.tools.split(',') if tool in WRAPPERS},
        packages=[
            dict(
                name=name.split('/')[-1],
                version=index[name]['Version'],
                filename=index[name]['Filename'],
                sha256=members_of(name)[0],
                size=members_of(name)[1],
                files=sorted([source, target] for target, source in files.items()),
                **({
                    'mirrors': mirrors_of[name]
                } if name in mirrors_of else {})
            ) for name, files in sorted(plan.items())
        ],
        extras=extras,
        local=local
    )
    args.out.write_text(json.dumps(manifest, indent=1) + '\n')
    total = sum(p['size'] for p in manifest['packages'])
    print(
        '%d packages, %.1f MB to download, %d libraries, %d tools' % (
            len(plan), total / 1e6, sum(1 for v in provided.values() if v), len(manifest['wrappers']) +
            sum(1 for files in plan.values() for target in files if target.startswith('bin/'))
        )
    )
    if unresolved: print('not in focal: ' + ', '.join(sorted(set(unresolved))))
    return 0


if __name__ == '__main__':
    sys.exit(main())
