#!/usr/bin/env python3
"""Build the optional 1x–5x Vulkan variant of RG DS Experimental.

It compiles the same dscore/ tree as the native fork with Vulkan on, plus SPIR-V
generated from that tree's shaders. This never publishes an engine, packages a
release, or changes a device. See docs/RGDS_VULKAN.md.
"""
import argparse
import json
from pathlib import Path
import shutil

try:
    from . import build_rgds_experimental as fork
    from . import build_rgds_vulkan_shaders as shaders
except ImportError:
    import build_rgds_experimental as fork
    import build_rgds_vulkan_shaders as shaders

base = fork.base
ROOT = base.ROOT


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument(
        '--compiler',
        choices=('zig', base.gcc.COMPILER),
        default='zig',
        help='gcc-pgo: GCC 13.3 + upstream PGO in the Lima ARM64 VM'
    )
    parser.add_argument('--fetch', action='store_true', help='zig: download the pinned SDL source for the link SDK')
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    scratch = ROOT / 'build/rgds-vulkan-game'
    scratch.mkdir(parents=True, exist_ok=True)
    # The tree carries no SPIR-V; syncing it also drops the last build's blobs.
    source, files = fork.prepare_source(fork.TREE, scratch)
    base_hash = fork.fingerprint(files)
    shader_record = shaders.prepare(source, scratch / 'shaders')
    # Only this private renderer gets the validated SPIR-V built from its sources.
    for name in shader_record['files']:
        path = source / fork.SHADER_DIR / name
        shutil.copy2(scratch / 'shaders' / name, path)
        files[path.relative_to(source).as_posix()] = base.digest(path)
    headers = ROOT / '.cache/panvk-kbase-source/include'
    if not (headers / 'vulkan/vulkan.h').is_file():
        parser.error('Prepare the pinned PanVK checkout first.')
    sdk = base.SDL_SDK
    if args.compiler == base.gcc.COMPILER:
        # Same verified tree, headers and shaders; GCC + PGO in the Lima VM,
        # which sees this checkout at the same path.
        options = {
            **base.gcc.gcc_options(fork.OPTIONS), 'DSPERATE_VULKAN': 'ON',
            'DSPERATE_VULKAN_INCLUDE': str(headers)
        }
        binary, gcc_record = base.gcc.build(source, 'rgds-vulkan', options)
        build = None
    else:
        gcc_record = None
        sdk = base.sdl_sdk(args.fetch, args.jobs)
        binary = configure_zig(scratch, source, sdk, headers, args)
    actual = {p.relative_to(source).as_posix(): base.digest(p) for p in source.rglob('*') if p.is_file()}
    if actual != files:
        raise ValueError('DSCore source changed during the Vulkan build')
    if fork.source_hash() != base_hash:
        raise ValueError('dscore/ was edited during the Vulkan build; build again')
    out = scratch / 'rgds-experimental-vulkan'
    shutil.copy2(binary, out)
    header_hashes = {
        p.relative_to(headers).as_posix(): base.digest(p)
        for name in ('vulkan', 'vk_video')
        for p in (headers / name).rglob('*') if p.is_file()
    }
    record = dict(
        schema=2,
        prototype=True,
        resolution=[1, 2, 3, 4, 5],
        base_source_hash=base_hash,
        source_hash=fork.fingerprint(files),
        shaders=shader_record,
        options=options if gcc_record else zig_options(sdk, headers),
        recipe_sha256=base.digest(Path(__file__)),
        headers_sha256=fork.fingerprint(header_hashes),
        binary_sha256=base.digest(out),
        abi=base.inspect_elf(out, 'private RG DS Vulkan prototype')
    )
    if gcc_record: record.update(compiler=base.gcc.COMPILER, gcc=gcc_record)
    (scratch / 'build.json').write_text(json.dumps(record, indent=2) + '\n')
    print(out)


def zig_options(sdk, headers):
    return {
        **fork.OPTIONS, 'DSPERATE_VULKAN': 'ON',
        'DSPERATE_VULKAN_INCLUDE': str(headers),
        'SDL2_DIR': str(sdk / 'lib/cmake/SDL2'),
        'CMAKE_FIND_ROOT_PATH': str(sdk),
        'CMAKE_PREFIX_PATH': str(sdk)
    }


def configure_zig(scratch, source, sdk, headers, args):
    tool = {name: base.find_tool(name) for name in ('zig', 'cmake', 'ninja')}
    env = base.cross_environment('zig-rgds-vulkan')
    chain = base.toolchain(scratch / 'toolchain', tool['zig'], source, base.SDL_SOURCE)
    build = scratch / 'build'
    base.configure(tool['cmake'], tool['ninja'], source, build, chain, zig_options(sdk, headers), env)
    base.run([tool['cmake'], '--build', build, '--target', 'dsperate', '--parallel', args.jobs], env=env)
    return build / 'src/frontend/sdl/dsperate'


if __name__ == '__main__':
    main()
