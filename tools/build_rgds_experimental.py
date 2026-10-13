#!/usr/bin/env python3
"""Build the isolated RG DS experimental DSperate fork; never deploy it."""
import argparse
import gzip
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile

try:
    from . import build_engines as base
except ImportError:
    import build_engines as base

ROOT = base.ROOT
PATCH = ROOT / 'tools/engine_patches/rgds-experimental.patch'
SLEEP_PATCH = ROOT / 'tools/engine_patches/rgds-sleep-ui.patch'
UI_PATCH = ROOT / 'tools/engine_patches/rgds-owned-ui.patch'
STYLUS = ROOT / 'src/experimental/rgds/stylus.hpp'
FRONTEND_UI = ROOT / 'src/experimental/rgds/frontend_ui.hpp'
SOURCE_HASH = 'e2b42f9e803d1b8ca0a6922336c5e883d65c781e22359a279446c82dcbe9901f'
OPTIONS = {**base.ENGINE_OPTIONS, 'DSPERATE_VERSION': '3.0.0-rgds-experimental.1',
           'DSPERATE_TUNE': '-mcpu=cortex_a55',
           'DSPERATE_CHEEVOS': 'OFF', 'DSPERATE_NET': 'OFF'}
FILES = {'rgds-experimental', 'LICENSE', 'NOTICE.txt', 'source.tar.gz'}


def fingerprint(files):
    return hashlib.sha256(json.dumps(files, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def prepare_source(source, scratch):
    _, inputs = base.source_inputs(source)
    portable = ROOT / 'build/engine-cross/source'
    base.prepare_build_source(source, portable, inputs)
    # Build in a dedicated tree. Standard DSperate never receives fork patches.
    pending = Path(tempfile.mkdtemp(prefix='source-', dir=scratch))
    try:
        for name in inputs:
            target = pending / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(portable / name, target)
        subprocess.run(['patch', '-p1', '--batch', '-i', str(PATCH)], cwd=pending, check=True)
        subprocess.run(['patch', '-p1', '--batch', '-i', str(SLEEP_PATCH)], cwd=pending, check=True)
        subprocess.run(['patch', '-E', '-p1', '--batch', '-i', str(UI_PATCH)], cwd=pending, check=True)
        shutil.copy2(STYLUS, pending / 'src/frontend/sdl/rgds_stylus.hpp')
        shutil.copy2(FRONTEND_UI, pending / 'src/frontend/sdl/rgds_frontend_ui.hpp')
        files = {p.relative_to(pending).as_posix(): base.digest(p) for p in pending.rglob('*') if p.is_file()}
        if fingerprint(files) != SOURCE_HASH:
            raise ValueError('Experimental source differs from its recorded fingerprint: ' + fingerprint(files))
        destination = scratch / 'source'
        destination.mkdir(exist_ok=True)
        existing = {p.relative_to(destination).as_posix() for p in destination.rglob('*') if p.is_file()}
        retired = {f'src/frontend/sdl/{name}' for name in ('menu.cpp', 'menu.h', 'settings.cpp', 'settings.h')}
        if existing - files.keys() - retired:
            raise ValueError('Unexpected files in experimental build source')
        for name in (existing - files.keys()) & retired:
            (destination / name).unlink()
        for name, checksum in files.items():
            target = destination / name
            if target.is_symlink(): raise ValueError('Experimental source must not contain symlinks')
            if not target.is_file() or base.digest(target) != checksum:
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(pending / name, target)
        return destination, files
    finally:
        shutil.rmtree(pending)


def archive_source(source, files, output):
    extras = [Path(__file__), ROOT / 'tools/build_engines.py', ROOT / 'tools/elf_abi.py',
              PATCH, SLEEP_PATCH, UI_PATCH, base.PATCH, STYLUS, FRONTEND_UI, ROOT / 'src/experimental/rgds/README.md']
    with output.open('wb') as stream, gzip.GzipFile(fileobj=stream, mode='wb', filename='', mtime=0) as zipped:
        with tarfile.open(fileobj=zipped, mode='w') as archive:
            entries = [('DSperate/' + name, source / name) for name in sorted(files)]
            entries += [('rgds/' + p.relative_to(ROOT).as_posix(), p) for p in extras]
            for name, path in entries:
                content = path.read_bytes(); item = tarfile.TarInfo(name)
                item.size = len(content); item.mode = path.stat().st_mode & 0o777
                archive.addfile(item, io.BytesIO(content))


def validate_build(directory):
    directory = Path(directory)
    record = json.loads((directory / 'build.json').read_text())
    if (directory.is_symlink() or {p.name for p in directory.iterdir()} != FILES | {'build.json'}
            or record.get('kind') != 'source-built-rgds-experimental' or record.get('schema') != 1
            or record.get('options') != OPTIONS or record.get('source_hash') != SOURCE_HASH
            or record.get('upstream_commit') != base.COMMIT
            or record.get('driver_sha256') != base.digest(Path(__file__))
            or record.get('base_driver_sha256') != base.digest(Path(base.__file__))
            or record.get('patch_sha256') != base.digest(PATCH)
            or record.get('stylus_sha256') != base.digest(STYLUS)
            or record.get('frontend_ui_sha256') != base.digest(FRONTEND_UI)
            or set(record.get('files', {})) != FILES):
        raise ValueError('Stale or invalid RG DS experimental build; run make build-rgds-experimental')
    for name, checksum in record['files'].items():
        path = directory / name
        if path.is_symlink() or not path.is_file() or base.digest(path) != checksum:
            raise ValueError('Experimental payload hash mismatch: ' + name)
    abi = base.inspect_elf(directory / 'rgds-experimental', 'RG DS experimental DSperate fork')
    if (abi != record.get('binary') or abi['machine'] != 'aarch64'
            or abi['interpreter'] != '/lib/ld-linux-aarch64.so.1' or abi.get('runpaths')
            or not abi['glibc_minimum'] or tuple(map(int, abi['glibc_minimum'].split('.'))) > (2, 17)):
        raise ValueError('Experimental engine ABI mismatch')
    files = {}; seen = set()
    with tarfile.open(directory / 'source.tar.gz') as archive:
        for member in archive:
            path = Path(member.name)
            if (not member.isfile() or path.is_absolute() or '..' in path.parts or member.name in seen
                    or not member.name.startswith(('DSperate/', 'rgds/'))):
                raise ValueError('Unsafe experimental source archive')
            seen.add(member.name)
            content = archive.extractfile(member).read()
            if member.name.startswith('DSperate/'):
                files[member.name[len('DSperate/'):]] = hashlib.sha256(content).hexdigest()
            else:
                original = ROOT / member.name[len('rgds/'):]
                if not original.is_file() or original.read_bytes() != content:
                    raise ValueError('Experimental archive build recipe changed')
    if fingerprint(files) != SOURCE_HASH or 'rgds/tools/build_rgds_experimental.py' not in seen:
        raise ValueError('Experimental archive source mismatch')
    return record


def build(args):
    scratch = ROOT / 'build/rgds-experimental-cross'; scratch.mkdir(parents=True, exist_ok=True)
    sdk = ROOT / 'build/engine-cross/sdl-sdk'
    if not (sdk / 'lib/cmake/SDL2').is_dir():
        raise ValueError('Run make build-engines first to prepare the SDL link SDK')
    source, files = prepare_source(args.source.resolve(), scratch)
    tool = {name: base.find_tool(name) for name in ('zig', 'cmake', 'ninja')}
    env = os.environ.copy()
    for name in ('CFLAGS', 'CXXFLAGS', 'LDFLAGS', 'CPPFLAGS', 'CPATH', 'C_INCLUDE_PATH',
                 'CPLUS_INCLUDE_PATH', 'LIBRARY_PATH', 'SDKROOT', 'MACOSX_DEPLOYMENT_TARGET', 'PKG_CONFIG_PATH', 'SDL2_DIR'):
        env.pop(name, None)
    env.update(ZIG_GLOBAL_CACHE_DIR=str(ROOT / '.cache/zig'), ZIG_LOCAL_CACHE_DIR=str(ROOT / '.cache/zig-rgds-experimental'))
    chain = base.toolchain(scratch / 'toolchain', tool['zig'], source, ROOT / '.cache/engine-sdk/SDL2-2.32.10')
    build_dir = scratch / 'build'
    base.configure(tool['cmake'], tool['ninja'], source, build_dir, chain,
                   {**OPTIONS, 'SDL2_DIR': str(sdk / 'lib/cmake/SDL2'),
                    'CMAKE_FIND_ROOT_PATH': str(sdk), 'CMAKE_PREFIX_PATH': str(sdk)}, env)
    base.run([tool['cmake'], '--build', build_dir, '--target', 'dsperate', '--parallel', args.jobs], env=env)
    actual = {p.relative_to(source).as_posix(): base.digest(p) for p in source.rglob('*') if p.is_file()}
    if actual != files: raise ValueError('Experimental source changed during compilation')
    output = args.output.resolve(); output.parent.mkdir(parents=True, exist_ok=True)
    pending = Path(tempfile.mkdtemp(prefix='.rgds-experimental-', dir=output.parent))
    try:
        shutil.copy2(build_dir / 'src/frontend/sdl/dsperate', pending / 'rgds-experimental')
        shutil.copy2(source / 'LICENSE', pending / 'LICENSE')
        (pending / 'NOTICE.txt').write_text(
            'RG DS Experimental 1: modified DSperate, GPL-3.0-or-later.\n'
            f'Upstream: {base.UPSTREAM}, commit {base.COMMIT}\n'
            'Changes: RGDS portability/frame counter patches; single-stylus ownership, edge clamping, mouse/touch arbitration.\n'
            'Capture-safe fast-forward raster skipping and late-capture raster recovery.\n'
            'Cortex-A55 CPU target, ARM64 JIT, NEON, LTO; network and achievements omitted.\n'
            'Performance and Zelda drawing acceptance require physical RG DS testing.\n'
            'Complete modified source and build recipe: source.tar.gz.\n')
        archive_source(source, files, pending / 'source.tar.gz')
        record = dict(schema=1, kind='source-built-rgds-experimental', upstream_commit=base.COMMIT,
                      source_hash=SOURCE_HASH, options=OPTIONS, driver_sha256=base.digest(Path(__file__)),
                      base_driver_sha256=base.digest(Path(base.__file__)), patch_sha256=base.digest(PATCH),
                      stylus_sha256=base.digest(STYLUS), frontend_ui_sha256=base.digest(FRONTEND_UI),
                      files={name: base.digest(pending / name) for name in sorted(FILES)},
                      binary=base.inspect_elf(pending / 'rgds-experimental', 'RG DS experimental DSperate fork'))
        (pending / 'build.json').write_text(json.dumps(record, indent=2, sort_keys=True) + '\n')
        validate_build(pending)
        base.publish_build(pending, output)
    finally:
        if pending.exists(): shutil.rmtree(pending)
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / '.cache/dsperate-source')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/engines/rgds-experimental')
    parser.add_argument('--jobs', type=int, default=min(os.cpu_count() or 1, 8))
    args = parser.parse_args()
    if args.jobs < 1: parser.error('--jobs must be positive')
    try: print(build(args))
    except (OSError, ValueError, subprocess.CalledProcessError) as error: parser.exit(1, str(error) + '\n')


if __name__ == '__main__': main()
