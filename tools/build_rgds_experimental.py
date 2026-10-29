#!/usr/bin/env python3
"""Build DSCore (the RG DS fork of DSperate) from dscore/; never deploy it."""
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
    from . import engine_toolchain as base
    from . import build_rgds_vulkan_shaders as shaders
except ImportError:
    import engine_toolchain as base
    import build_rgds_vulkan_shaders as shaders

ROOT = base.ROOT
TREE = ROOT / 'dscore'
NOTES = 'FORK.md'
SHADER_DIR = 'src/core/gpu/vk/shaders'
# Compiled by tools/build_rgds_vulkan_shaders.py into a build tree only.
GENERATED = frozenset(f'{SHADER_DIR}/{name}' for name in shaders.SHADERS)
# What the corresponding-source archive carries beside the DSperate/ tree.
EXTRAS = (
    'tools/build_rgds_experimental.py', 'tools/build_rgds_vulkan_shaders.py', 'tools/engine_toolchain.py',
    'tools/build_engines_gcc.py', 'tools/elf_abi.py', 'dscore/FORK.md', 'src/experimental/rgds/README.md'
)
OPTIONS = {
    **base.ENGINE_OPTIONS, 'DSPERATE_VERSION': '3.0.0-rgds-experimental.1',
    'DSPERATE_TUNE': '-mcpu=cortex_a55',
    'DSPERATE_CHEEVOS': 'OFF',
    'DSPERATE_NET': 'OFF'
}
FILES = {'rgds-experimental', 'LICENSE', 'NOTICE.txt', 'source.tar.gz'}
GCC_OPTIONS = base.gcc.gcc_options(OPTIONS)


def fingerprint(files):
    return hashlib.sha256(json.dumps(files, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def tree_files(tree=None):
    """Digest every file a build compiles: the tree without its notes, Finder litter and generated SPIR-V."""
    tree = Path(tree or TREE)
    if tree.is_symlink() or not tree.is_dir():
        raise ValueError('DSCore source tree is missing: ' + str(tree))
    files = {}
    for folder, folders, names in os.walk(tree):
        for entry in (*folders, *names):
            if (Path(folder) / entry).is_symlink():
                raise ValueError('DSCore source must not contain symlinks: ' + str(Path(folder) / entry))
        for name in names:
            path = Path(folder) / name
            relative = path.relative_to(tree).as_posix()
            if relative == NOTES or name == '.DS_Store' or relative in GENERATED:
                continue
            if not path.is_file():
                raise ValueError('Unsupported DSCore source file: ' + relative)
            files[relative] = base.digest(path)
    return files


def check_provenance(tree=None):
    """The tree's notes must name the pinned upstream it was forked from."""
    notes = Path(tree or TREE) / NOTES
    text = notes.read_text() if notes.is_file() else ''
    if base.COMMIT not in text or base.TREE not in text:
        raise ValueError(f'{NOTES} does not record the pinned DSperate commit {base.COMMIT}')


def source_hash(tree=None):
    """Fingerprint of the checked-in fork; builds record it and packaging rejects a stale one."""
    check_provenance(tree)
    return fingerprint(tree_files(tree))


def prepare_source(source, scratch):
    """Verify the DSCore tree and mirror it into this build's private source copy."""
    source = Path(source)
    check_provenance(source)
    files = tree_files(source)
    destination = scratch / 'source'
    destination.mkdir(parents=True, exist_ok=True)
    existing = {p.relative_to(destination).as_posix() for p in destination.rglob('*') if p.is_file() or p.is_symlink()}
    for name in existing - files.keys():
        (destination / name).unlink()
    for folder in sorted((p for p in destination.rglob('*') if p.is_dir()), reverse=True):
        if not any(folder.iterdir()):
            folder.rmdir()
    for name, checksum in files.items():
        target = destination / name
        if target.is_symlink():
            raise ValueError('DSCore build source must not contain symlinks')
        if not target.is_file() or base.digest(target) != checksum:
            target.parent.mkdir(parents=True, exist_ok=True)
            # A fresh mtime, not the edit time, so incremental builds see the change.
            shutil.copy(source / name, target)
            if base.digest(target) != checksum:
                raise ValueError('DSCore source changed while it was being copied: ' + name)
    return destination, files


def archive_source(source, files, output):
    with output.open('wb') as stream, gzip.GzipFile(fileobj=stream, mode='wb', filename='', mtime=0) as zipped:
        with tarfile.open(fileobj=zipped, mode='w') as archive:
            entries = [('DSperate/' + name, source / name) for name in sorted(files)]
            entries += [('rgds/' + name, ROOT / name) for name in EXTRAS]
            for name, path in entries:
                content = path.read_bytes()
                item = tarfile.TarInfo(name)
                item.size = len(content)
                item.mode = path.stat().st_mode & 0o777
                archive.addfile(item, io.BytesIO(content))


def validate_source_archive(path, expected):
    """The archive holds exactly the fingerprinted tree and the unchanged build recipe."""
    files = {}
    seen = set()
    with tarfile.open(path) as archive:
        for member in archive:
            name = Path(member.name)
            if (
                not member.isfile() or name.is_absolute() or '..' in name.parts or member.name in seen
                or not member.name.startswith(('DSperate/', 'rgds/'))
            ):
                raise ValueError('Unsafe experimental source archive')
            seen.add(member.name)
            content = archive.extractfile(member).read()
            if member.name.startswith('DSperate/'):
                files[member.name[len('DSperate/'):]] = hashlib.sha256(content).hexdigest()
            else:
                original = ROOT / member.name[len('rgds/'):]
                if not original.is_file() or original.read_bytes() != content:
                    raise ValueError('Experimental archive build recipe changed')
    if fingerprint(files) != expected or not {'rgds/' + name for name in EXTRAS} <= seen:
        raise ValueError('Experimental archive source mismatch')


def validate_build(directory):
    directory = Path(directory)
    record = json.loads((directory / 'build.json').read_text())
    if (
        directory.is_symlink() or {p.name
                                   for p in directory.iterdir()} != FILES | {'build.json'}
        or record.get('kind') != 'source-built-rgds-experimental' or record.get('schema') != 2
        or record.get('options') != (GCC_OPTIONS if record.get('compiler') == base.gcc.COMPILER else OPTIONS)
        or record.get('source_hash') != source_hash() or record.get('upstream_commit') != base.COMMIT
        or record.get('driver_sha256') != base.digest(Path(__file__))
        or record.get('base_driver_sha256') != base.digest(Path(base.__file__)) or set(record.get('files', {})) != FILES
    ):
        raise ValueError('Stale or invalid RG DS experimental build; run make build-rgds-experimental')
    for name, checksum in record['files'].items():
        path = directory / name
        if path.is_symlink() or not path.is_file() or base.digest(path) != checksum:
            raise ValueError('Experimental payload hash mismatch: ' + name)
    abi = base.inspect_elf(directory / 'rgds-experimental', 'RG DS experimental DSperate fork')
    if (
        abi != record.get('binary') or abi['machine'] != 'aarch64' or abi['interpreter'] != '/lib/ld-linux-aarch64.so.1'
        or abi.get('runpaths') or not base.gcc.glibc_allowed(abi, record.get('compiler', 'zig'))
    ):
        raise ValueError('Experimental engine ABI mismatch')
    validate_source_archive(directory / 'source.tar.gz', record['source_hash'])
    return record


def build(args):
    scratch = ROOT / 'build/rgds-experimental-cross'
    scratch.mkdir(parents=True, exist_ok=True)
    source, files = prepare_source(TREE, scratch)
    gcc_build = getattr(args, 'compiler', 'zig') == base.gcc.COMPILER
    if gcc_build:
        # The Lima VM links against its own SDL2 development files: no link SDK needed.
        return publish(source, files, args, *base.gcc.build(source, 'rgds-experimental', GCC_OPTIONS))
    sdk = base.sdl_sdk(getattr(args, 'fetch', False), args.jobs)
    tool = {name: base.find_tool(name) for name in ('zig', 'cmake', 'ninja')}
    env = base.cross_environment('zig-rgds-experimental')
    chain = base.toolchain(scratch / 'toolchain', tool['zig'], source, base.SDL_SOURCE)
    build_dir = scratch / 'build'
    base.configure(
        tool['cmake'], tool['ninja'], source, build_dir, chain, {
            **OPTIONS, 'SDL2_DIR': str(sdk / 'lib/cmake/SDL2'),
            'CMAKE_FIND_ROOT_PATH': str(sdk),
            'CMAKE_PREFIX_PATH': str(sdk)
        }, env
    )
    base.run([tool['cmake'], '--build', build_dir, '--target', 'dsperate', '--parallel', args.jobs], env=env)
    return publish(source, files, args, build_dir / 'src/frontend/sdl/dsperate', None)


def publish(source, files, args, binary, gcc_record):
    actual = {p.relative_to(source).as_posix(): base.digest(p) for p in source.rglob('*') if p.is_file()}
    if actual != files: raise ValueError('Experimental source changed during compilation')
    if source_hash() != fingerprint(files): raise ValueError('dscore/ was edited during the build; build again')
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    pending = Path(tempfile.mkdtemp(prefix='.rgds-experimental-', dir=output.parent))
    try:
        shutil.copy2(binary, pending / 'rgds-experimental')
        shutil.copy2(source / 'LICENSE', pending / 'LICENSE')
        (pending / 'NOTICE.txt').write_text(
            'RG DS Experimental 1: modified DSperate, GPL-3.0-or-later.\n'
            f'Upstream: {base.UPSTREAM}, commit {base.COMMIT}\n'
            'Changes: the DSCore fork (rgds/dscore/FORK.md in source.tar.gz): portability and frame counter; single-stylus\n'
            'ownership, edge clamping, mouse/touch arbitration; capture-safe fast-forward raster skipping; lid sleep;\n'
            'RGDS-owned menus and HUD; high-resolution 3D compositor.\n' + (
                'GCC 13.3 with upstream\'s committed PGO profile, ARM64 JIT, NEON (tools/build_engines_gcc.py); network and achievements omitted.\n'
                if gcc_record else 'Cortex-A55 CPU target, ARM64 JIT, NEON, LTO; network and achievements omitted.\n'
            ) + 'Performance and Zelda drawing acceptance require physical RG DS testing.\n'
            'Complete modified source and build recipe: source.tar.gz.\n'
        )
        archive_source(source, files, pending / 'source.tar.gz')
        record = dict(
            schema=2,
            kind='source-built-rgds-experimental',
            upstream_commit=base.COMMIT,
            source_hash=fingerprint(files),
            options=GCC_OPTIONS if gcc_record else OPTIONS,
            driver_sha256=base.digest(Path(__file__)),
            base_driver_sha256=base.digest(Path(base.__file__)),
            files={name: base.digest(pending / name)
                   for name in sorted(FILES)},
            binary=base.inspect_elf(pending / 'rgds-experimental', 'RG DS experimental DSperate fork')
        )
        if gcc_record: record.update(compiler=base.gcc.COMPILER, gcc=gcc_record)
        (pending / 'build.json').write_text(json.dumps(record, indent=2, sort_keys=True) + '\n')
        validate_build(pending)
        base.publish_build(pending, output)
    finally:
        if pending.exists(): shutil.rmtree(pending)
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/engines/rgds-experimental')
    parser.add_argument('--jobs', type=int, default=min(os.cpu_count() or 1, 8))
    parser.add_argument(
        '--compiler',
        choices=('zig', base.gcc.COMPILER),
        default='zig',
        help='gcc-pgo: GCC 13.3 + upstream PGO profile in the Lima ARM64 VM'
    )
    parser.add_argument('--fetch', action='store_true', help='zig: download the pinned SDL source for the link SDK')
    args = parser.parse_args()
    if args.jobs < 1: parser.error('--jobs must be positive')
    try:
        print(build(args))
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, str(error) + '\n')


if __name__ == '__main__': main()
