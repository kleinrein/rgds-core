#!/usr/bin/env python3
"""Stage the 3DS engine for the OS image: build/engines/tanuki3ds, which tools/own_os_build.py puts in the image as
/usr/share/rgds/engines/tanuki3ds (runtime/emulators.py tanuki_engine).

  stage_engine.py HOST            the engine installed on a handheld (/data/rgds/engines/tanuki3ds), the one played
  stage_engine.py --from FOLDER   a folder or tanuki-bundle tar of your own
  ... --mesa DIR                  lib2's Mesa replaced by build-mesa.sh's install (dbg/mesa-inst/usr/lib, copied out of
                                  the VM): the build without LLVM, which is what makes the engine fit an OS update

LLVM is left out (libLLVM*, with every library only it needed, such as ICU's 31 MB of data), as are lib's Ubuntu Mesa
files that lib2 shadows (run.sh puts lib2 first; MESA_STOCK=1 is a test switch). A lib2 Mesa that still needs LLVM
stops the staging. The OS update is limited to 512 MiB (runtime/ota.py MAX_PACKAGE, read by handhelds already out).

Only what a game launch needs is taken: run.sh, ctremu, ctremu.ini and the egl/, lib/ and lib2/ folders. The test
binaries beside the installed engine (ctremu.ptN, ctremu.installed-*, ctremu.stable ...) stay out. engine.json lists
every file with its SHA-256; own_os_build.py refuses files that do not match it.

A 3DS resume point belongs to the engine build that wrote it: stage the build that is installed and played, not a
newer one nobody has state-checked (README, state-check.sh).
"""
import argparse
import re
import struct
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TARGET = ROOT / 'build/engines/tanuki3ds'
DEVICE = '/data/rgds/engines/tanuki3ds'
FILES = ('run.sh', 'ctremu', 'ctremu.ini')
FOLDERS = ('egl', 'lib', 'lib2')
JUNK = ('.DS_Store',)

# Ubuntu's Mesa in lib, under lib2's copies (run.sh: LD_LIBRARY_PATH=lib2:lib, GBM_BACKENDS_PATH=lib2/gbm)
SHADOWED = ('lib/libEGL_mesa.so.0', 'lib/gbm/dri_gbm.so')
# what build-mesa.sh installs and run.sh loads from lib2, by the name it has there
MESA = ('libEGL_mesa.so.0', 'libgbm.so.1', 'gbm/dri_gbm.so', 'libgallium-25.2.8.so')


def needed(path):
    """The DT_NEEDED names of a 64-bit little-endian ELF file (None for anything else)."""
    data = path.read_bytes()
    if data[:4] != b'\x7fELF' or data[4] != 2 or data[5] != 1: return None
    phoff, = struct.unpack_from('<Q', data, 0x20)
    phentsize, phnum = struct.unpack_from('<HH', data, 0x36)
    loads, dynamic = [], None
    for i in range(phnum):
        kind, _, offset, vaddr, _, filesz, _, _ = struct.unpack_from('<IIQQQQQQ', data, phoff + i * phentsize)
        if kind == 1: loads.append((vaddr, offset, filesz))
        if kind == 2: dynamic = (offset, filesz)
    if dynamic is None: return []
    entries = [struct.unpack_from('<qQ', data, dynamic[0] + n) for n in range(0, dynamic[1], 16)]
    strtab = next(v for t, v in entries if t == 5)
    strtab = next(o + strtab - a for a, o, z in loads if a <= strtab < a + z)
    names = []
    for tag, value in entries:
        if tag == 1: names.append(data[strtab + value:data.index(b'\0', strtab + value)].decode())
    return names


def prune(folder, keep):
    """keep without LLVM, lib's shadowed Mesa, and libraries that only those needed. A library nothing names is
    kept: SDL and glvnd open theirs by name at run time."""
    dropped = {n for n in keep if Path(n).name.startswith('libLLVM') or n in SHADOWED}
    dropped |= {
        n
        for n in keep if n.startswith('lib/libgallium-') and any(k.startswith('lib2/libgallium-') for k in keep)
    }
    graph = {n: needed(folder / n) or [] for n in keep}
    users = {}
    for name, libs in graph.items():
        for lib in libs:
            users.setdefault(lib, set()).add(name)
    while True:
        more = {
            n
            for n in keep if n not in dropped and Path(n).parts[0] in ('lib', 'lib2') and users.get(Path(n).name)
            and users[Path(n).name] <= dropped
        }
        if not more: break
        dropped |= more
    for name in keep:
        if name not in dropped and any(lib.startswith('libLLVM') for lib in graph[name]):
            raise SystemExit(f'{name} still needs LLVM: stage a Mesa built without it (--mesa).')
    return [n for n in keep if n not in dropped], sorted(dropped)


def wanted(name):
    parts = Path(name).parts
    if not parts or any(p.startswith('._') or p in JUNK for p in parts): return False
    if name in FILES: return True
    if len(parts) < 2 or parts[0] not in FOLDERS: return False
    # libraries and the EGL vendor file only, not backups beside them (libgallium-25.2.8.so.nov1, .pre-range)
    return parts[0] == 'egl' and name.endswith('.json') or re.search(r'\.so(\.\d+)*$', name) is not None


def pull(host, into):
    """The handheld's engine folder as it is, through ssh and tar (scp of 400 files is slow)."""
    archive = into / 'engine.tar'
    with archive.open('wb') as out:
        subprocess.run(
            [
                'ssh', '-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=no', 'root@' + host,
                f'cd {DEVICE} && tar -cf - .'
            ],
            stdout=out,
            check=True
        )
    return archive


def unpack(source, into):
    if source.is_dir(): return source
    folder = into / 'unpacked'
    with tarfile.open(source) as archive:
        members = [
            m for m in archive.getmembers()
            if not Path(m.name).is_absolute() and '..' not in Path(m.name).parts and (m.isfile() or m.isdir())
        ]
        archive.extractall(folder, members)
    # a tanuki-bundle.tar has its folder inside
    inner = [p for p in folder.iterdir() if p.is_dir()]
    return inner[0] if len(inner) == 1 and not (folder / 'run.sh').exists() else folder


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('host', nargs='?')
    parser.add_argument('--from', dest='source', type=Path)
    parser.add_argument('--mesa', type=Path, help="build-mesa.sh's usr/lib, put into lib2")
    args = parser.parse_args()
    if bool(args.host) == bool(args.source): parser.error('give a HOST or --from FOLDER')
    with tempfile.TemporaryDirectory(dir=ROOT / 'build') as tmp:
        tmp = Path(tmp)
        folder = unpack(pull(args.host, tmp) if args.host else args.source.resolve(), tmp)
        if args.mesa and folder == args.source.resolve():
            folder = Path(shutil.copytree(folder, tmp / 'copy'))
        names = sorted(p.relative_to(folder).as_posix() for p in folder.rglob('*') if p.is_file())
        keep = [n for n in names if wanted(n)]
        missing = [n for n in FILES if n not in keep]
        if missing: raise SystemExit('Not an engine folder, missing ' + ', '.join(missing))
        left = [n for n in names if n not in keep]
        if args.mesa:
            for name in MESA:
                source = args.mesa / name
                if not source.is_file(): raise SystemExit(f'{source} missing')
                (folder / 'lib2' / name).parent.mkdir(parents=True, exist_ok=True)
                (folder / 'lib2' / name).unlink(missing_ok=True)
                shutil.copyfile(source, folder / 'lib2' / name)
                if 'lib2/' + name not in keep: keep.append('lib2/' + name)
        keep, pruned = prune(folder, keep)
        if TARGET.exists(): shutil.rmtree(TARGET)
        files = {}
        for name in keep:
            (TARGET / name).parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(folder / name, TARGET / name)
            files[name] = hashlib.sha256((TARGET / name).read_bytes()).hexdigest()
        for name in ('run.sh', 'ctremu'):
            (TARGET / name).chmod(0o755)
        engine = hashlib.md5((TARGET / 'ctremu').read_bytes()).hexdigest()
        record = {
            'source': args.host and f'{args.host}:{DEVICE}' or str(args.source),
            'ctremu_md5': engine,
            'mesa': str(args.mesa) if args.mesa else 'as installed',
            'pruned': pruned,
            'files': files
        }
        (TARGET / 'engine.json').write_text(json.dumps(record, indent=2, sort_keys=True) + '\n')
    size = sum((TARGET / n).stat().st_size for n in files)
    print(f'staged {len(files)} files, {size >> 20} MB, ctremu md5 {engine[:8]} in {TARGET.relative_to(ROOT)}')
    if pruned: print('pruned:', ', '.join(pruned))
    if left: print('left out:', ', '.join(left[:20]) + (' ...' if len(left) > 20 else ''))
    if (TARGET / 'run.sh').read_bytes() != (ROOT / 'tools/tanuki3ds/run.sh').read_bytes():
        print('note: run.sh differs from tools/tanuki3ds/run.sh (the staged one is what the image gets)')


if __name__ == '__main__':
    main()
