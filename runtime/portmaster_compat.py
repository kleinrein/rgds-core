#!/usr/bin/env python3
"""The compat layer PortMaster ports need on the stock RG DS card (docs/PORTMASTER_PLAN.md).

Ports are built for Ubuntu-style handheld systems. On this card they miss a few
shared libraries (libjpeg.so.8, libzstd.so.1...) and GNU tools where the card
has BusyBox (tar, grep -P, gawk...). runtime/portmaster_files/compat.json pins
the Ubuntu 20.04 arm64 packages that provide them, by version and SHA-256
(tools/portmaster_compat_manifest.py makes it). This downloads those packages
from Ubuntu's archive, checks them and copies the listed files into

    /mnt/mmc/portmaster/rgds/bin   first in PATH for port scripts only
    /mnt/mmc/portmaster/rgds/lib   last in LD_LIBRARY_PATH for ports only

Only libraries the card lacks are listed, so nothing the system provides is
ever shadowed. Nothing is shipped in the RG DS Core package; installing again
after compat.json changes replaces the folder as a whole.
"""
import hashlib
import io
import json
import lzma
import os
from pathlib import Path
import re
import shutil
import sys
import tarfile
import urllib.request

sys.dont_write_bytecode = True
from fields import read_fields, write_fields

FILES = Path(__file__).resolve().parent / 'portmaster_files'
# compat.json: the 64-bit libraries and GNU tools; compat-armhf.json: the 32-bit layer for armhf-only ports.
MANIFESTS = (FILES / 'compat.json', FILES / 'compat-armhf.json')
# armhf programs name /lib/ld-linux-armhf.so.3, which the card does not have (and which is not ours to add);
# prepare_armhf points them at this copy instead. It must not be longer than the name it replaces.
ARMHF_INTERP = b'/lib/ld-linux-armhf.so.3'
ARMHF_LOADER = '/tmp/rgds-ld-armhf.so.3'


def manifests(path=None):
    if path is None: return [p for p in MANIFESTS if p.is_file()]
    return list(path) if isinstance(path, (list, tuple)) else [Path(path)]


def manifest_hash(path=None):
    digest = hashlib.sha256()
    for item in manifests(path):
        digest.update(item.read_bytes())
    return digest.hexdigest()[:16]


def folder(root):
    return Path(root) / 'rgds'


def current(root, path=None):
    """True when the compat folder matches the manifests."""
    record = read_fields(folder(root) / 'compat.ini')
    return record.get('manifest') == manifest_hash(path) and (folder(root) / 'bin').is_dir()


def ar_members(data):
    if data[:8] != b'!<arch>\n': raise ValueError('not a Debian package')
    pos = 8
    while pos + 60 <= len(data):
        name = data[pos:pos + 16].decode().strip().rstrip('/')
        size = int(data[pos + 48:pos + 58].decode().strip())
        yield name, data[pos + 60:pos + 60 + size]
        pos += 60 + size + (size & 1)


def package_files(data):
    """{path: bytes or ('link', target)} of a package's data.tar.xz."""
    for name, body in ar_members(data):
        if not name.startswith('data.tar'): continue
        if not name.endswith('.xz'): raise ValueError('unexpected ' + name)
        members = {}
        with tarfile.open(fileobj=io.BytesIO(lzma.decompress(body))) as tar:
            for info in tar:
                path = info.name.lstrip('./')
                if info.issym(): members[path] = ('link', info.linkname)
                elif info.isfile(): members[path] = tar.extractfile(info).read()
        return members
    raise ValueError('no data.tar in the package')


def resolve(members, path):
    for _ in range(8):
        value = members.get(path)
        if not isinstance(value, tuple): return value
        target = value[1]
        path = target.lstrip('/') if target.startswith('/') else str(Path(path).parent / target)
        path = re.sub(r'[^/]+/\.\./', '', path)
    return None


def download(mirrors, filename, sha256, cache):
    """A package from the first mirror that has it, checked against its pinned SHA-256."""
    cached = cache / Path(filename).name
    if cached.is_file() and hashlib.sha256(cached.read_bytes()).hexdigest() == sha256:
        return cached.read_bytes()
    errors = []
    for mirror in mirrors:
        try:
            request = urllib.request.Request(mirror + '/' + filename, headers={'User-Agent': 'RG-DS-Core'})
            with urllib.request.urlopen(request, timeout=60) as response:
                data = response.read()
        except OSError as error:
            errors.append('%s: %s' % (mirror, error))
            continue
        if hashlib.sha256(data).hexdigest() != sha256:
            errors.append('%s: checksum mismatch' % mirror)
            continue
        cache.mkdir(parents=True, exist_ok=True)
        cached.write_bytes(data)
        return data
    raise ValueError('Could not download %s (%s)' % (Path(filename).name, '; '.join(errors)))


def place(staging, target, data):
    out = staging / target
    if staging.resolve() not in out.resolve().parents: raise ValueError('bad target ' + target)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(data)
    out.chmod(0o755)


def install(root, status=None, path=None):
    """Build rgds/ next to it and swap it in, so a failed install leaves the previous one working."""
    import zipfile
    status = status or (lambda message, percent=-1, detail='': None)
    loaded = [(item, json.loads(item.read_text())) for item in manifests(path)]
    base = folder(root)
    staging, cache = base.with_name('rgds.new'), Path(root) / '.download/compat'
    shutil.rmtree(staging, ignore_errors=True)
    (staging / 'bin').mkdir(parents=True)
    (staging / 'lib').mkdir()
    total = sum(p['size'] for _i, m in loaded for p in m['packages'] + m.get('extras', [])) or 1
    done = count = 0
    for item, manifest in loaded:
        for package in manifest['packages']:
            status('Adding compatibility files', done * 100 // total, package['name'])
            mirrors = package.get('mirrors', manifest['mirrors'])  # bionic and buster packages name their own
            members = package_files(download(mirrors, package['filename'], package['sha256'], cache))
            for source, target in package['files']:
                data = resolve(members, source)
                if data is None: raise ValueError('%s has no %s' % (package['name'], source))
                place(staging, target, data)
            done += package['size']
            count += 1
        for extra in manifest.get('extras', []):
            url = extra.get('zip') or extra['url']
            status('Adding compatibility files', done * 100 // total, Path(url).name)
            blob = download([url.rsplit('/', 1)[0]], url.rsplit('/', 1)[1], extra['sha256'], cache)
            if 'zip' in extra:
                # Files taken from a pinned PortMaster port zip (gl4es's libGL.so.1).
                with zipfile.ZipFile(io.BytesIO(blob)) as zf:
                    for member, target, sha256 in extra['files']:
                        data = zf.read(member)
                        if hashlib.sha256(data).hexdigest() != sha256: raise ValueError('%s does not match' % member)
                        place(staging, target, data)
            else:
                place(staging, extra['target'], blob)  # one pinned file (Rockchip's 32-bit libmali)
            done += extra['size']
        for source, target in manifest.get('local', []):
            # Files RG DS Core ships beside the manifest (the armhf stubs).
            place(staging, target, (item.parent / source).read_bytes())
        for tool, script in manifest.get('wrappers', {}).items():
            (staging / 'bin' / tool).write_text(script)
            (staging / 'bin' / tool).chmod(0o755)
    write_fields(staging / 'compat.ini', dict(schema=1, manifest=manifest_hash(path), packages=count))
    # The card is FAT: no atomic directory swap over an existing folder, so the old one moves aside first.
    old = base.with_name('rgds.old')
    shutil.rmtree(old, ignore_errors=True)
    if base.exists(): base.rename(old)
    staging.rename(base)
    shutil.rmtree(old, ignore_errors=True)
    shutil.rmtree(cache, ignore_errors=True)
    status('Compatibility files ready', 100)


def armhf_ready(root):
    return (folder(root) / 'armhf/ld-linux-armhf.so.3').is_file()


def patch_interp(path, new=ARMHF_LOADER.encode()):
    """Point a 32-bit ARM program's PT_INTERP from /lib/ld-linux-armhf.so.3 at new. True when it changed."""
    import struct
    try:
        with open(path, 'r+b') as stream:
            head = stream.read(52)
            if len(head) < 52 or head[:4] != b'\x7fELF' or head[4] != 1 or head[5] != 1: return False
            if struct.unpack_from('<H', head, 18)[0] != 40: return False
            phoff, = struct.unpack_from('<I', head, 28)
            phentsize, phnum = struct.unpack_from('<HH', head, 42)
            for i in range(phnum):
                stream.seek(phoff + i * phentsize)
                kind, offset, _vaddr, _paddr, filesz = struct.unpack('<IIIII', stream.read(20))
                if kind != 3: continue  # PT_INTERP
                stream.seek(offset)
                current = stream.read(filesz).rstrip(b'\0')
                if current == new: return False
                if current != ARMHF_INTERP or len(new) >= filesz: return False
                stream.seek(offset)
                stream.write(new + b'\0' * (filesz - len(new)))
                return True
    except OSError:
        pass
    return False


def prepare_armhf(root, port_folder):
    """Before an armhf-only port starts: the loader in /tmp, and the port's programs pointed at it."""
    source = folder(root) / 'armhf/ld-linux-armhf.so.3'
    if not source.is_file(): return 0
    loader = Path(ARMHF_LOADER)
    data = source.read_bytes()
    if not loader.is_file() or loader.read_bytes() != data:
        pending = loader.with_name(loader.name + '.pending')
        pending.write_bytes(data)
        pending.chmod(0o755)
        pending.replace(loader)
    changed = 0
    for path in Path(port_folder).rglob('*'):
        if path.is_file() and not path.is_symlink() and path.stat().st_size > 1024: changed += patch_interp(path)
    return changed


# Desktop-GL libraries a port may need; gl4es (rgds/gl4es) provides them over the card's GLES.
GL_NAMES = {b'libGL.so.1', b'libOpenGL.so.0', b'libGLX.so.0'}


def elf_needs(path, names):
    """True when a 64-bit ARM ELF names one of names as DT_NEEDED. Reads only what it needs (mmap)."""
    import mmap
    import struct
    try:
        with open(path, 'rb') as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
            if data[:4] != b'\x7fELF' or data[4] != 2 or data[5] != 1 or struct.unpack_from('<H', data, 18)[0] != 183:
                return False
            phoff, = struct.unpack_from('<Q', data, 32)
            phentsize, phnum = struct.unpack_from('<HH', data, 54)
            loads, dynamic = [], None
            for i in range(phnum):
                kind, _f, offset, vaddr, _p, filesz = struct.unpack_from('<IIQQQQ', data, phoff + i * phentsize)
                if kind == 1: loads.append((vaddr, offset, filesz))
                elif kind == 2: dynamic = (offset, filesz)
            if not dynamic: return False
            entries = [struct.unpack_from('<qQ', data, dynamic[0] + i * 16) for i in range(dynamic[1] // 16)]
            strtab = next((v for t, v in entries if t == 5), None)
            base = next(
                (o + strtab - va for va, o, size in loads if strtab is not None and va <= strtab < va + size), None
            )
            if base is None: return False
            for tag, value in entries:
                if tag == 0: break
                if tag == 1 and data[base + value:data.find(b'\0', base + value)] in names: return True
    except (OSError, ValueError, struct.error):
        pass
    return False


def gl_port(port_folder, attr):
    """A port that needs desktop GL from the system and does not bring gl4es: its binaries name libGL, libOpenGL
    or libGLX (Bugdom, Nanosaur, MiceAmaze), or it runs on the Ren'Py runtime. Ports on PortMaster's Weston runtime
    are left alone: it brings its own GL, and probes for a system libGL."""
    runtime = ' '.join(attr.get('runtime') or [])
    if 'weston' in runtime: return False
    if 'renpy' in runtime: return True
    files = [path for path in Path(port_folder).rglob('*') if path.is_file() and not path.is_symlink()]
    if any(path.name == 'libGL.so.1' for path in files): return False
    binary = re.compile(r'(\.so(\.[0-9.]+)?$|\.aarch64$|^[^.]+$)')  # game data has extensions; programs rarely do
    return any(binary.search(path.name) and path.stat().st_size > 1024 and elf_needs(path, GL_NAMES) for path in files)


def gl_environment(env, root):
    """gl4es, GLU and the glvnd stubs (rgds/gl4es) after a GL port's other libraries, the SDL attribute shim preloaded, and
    gl4es's usual settings."""
    gl4es = folder(root) / 'gl4es'
    if not (gl4es / 'libGL.so.1').is_file(): return env
    env['LD_LIBRARY_PATH'] = ':'.join(p for p in (env.get('LD_LIBRARY_PATH', ''), str(gl4es)) if p)
    # SDL GL requests Mali's Wayland EGL can meet (tools/build_compat_stubs.py): a compatibility or core profile becomes
    # ES, so SDL makes the GLES context gl4es works on, and the depth buffer is at most 24 bits.
    attrs = gl4es / 'librgds-gl-attrs.so'
    if attrs.is_file(): env['LD_PRELOAD'] = ':'.join(p for p in (str(attrs), env.get('LD_PRELOAD', '')) if p)
    env.setdefault('LIBGL_ES', '2')
    env.setdefault('LIBGL_GL', '21')
    return env


def environment(env, root):
    """PATH and LD_LIBRARY_PATH for a port: our GNU tools first, our libraries (64-bit, then 32-bit) last."""
    base = folder(root)
    if (base / 'bin').is_dir(): env['PATH'] = str(base / 'bin') + ':' + env.get('PATH', '/usr/bin:/bin')
    # 64-bit programs skip the 32-bit libraries (a wrong ELF class is passed over) and the other way round.
    libraries = [str(base / name) for name in ('lib', 'armhf/lib') if (base / name).is_dir()]
    if libraries: env['LD_LIBRARY_PATH'] = ':'.join(p for p in [env.get('LD_LIBRARY_PATH', '')] + libraries if p)
    return env


if __name__ == '__main__':
    root = Path(os.environ.get('RGDS_PORTMASTER_ROOT', '/mnt/mmc/portmaster'))
    if len(sys.argv) > 1 and sys.argv[1] == 'status':
        print(json.dumps(dict(current=current(root), manifest=manifest_hash())))
    else:
        install(root, lambda message, percent=-1, detail='': print(message, percent, detail, flush=True))
