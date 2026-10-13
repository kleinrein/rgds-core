#!/usr/bin/env python3
"""Build the optional 1x–5x Vulkan variant of RG DS Experimental.

This never publishes an engine, packages a release, or changes a device. The native
software fork and its build record remain intact. See docs/RGDS_VULKAN.md.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess

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
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    scratch = ROOT/'build/rgds-vulkan-game'
    scratch.mkdir(parents=True, exist_ok=True)
    # This header belongs to the enhanced variant, so remove its previous
    # generated copy before verifying/restoring the native fork source tree.
    (scratch/'source/src/core/gpu/rgds_sample_plan.hpp').unlink(missing_ok=True)
    source, files = fork.prepare_source(ROOT/'.cache/dsperate-source', scratch)
    hires_patch = ROOT/'tools/engine_patches/rgds-vulkan-hires.patch'
    subprocess.run(['patch', '-p1', '--batch', '-i', str(hires_patch)], cwd=source, check=True)
    shutil.copy2(ROOT/'src/experimental/rgds/sample_plan.hpp', source/'src/core/gpu/rgds_sample_plan.hpp')
    files = {p.relative_to(source).as_posix():base.digest(p) for p in source.rglob('*') if p.is_file()}
    shader_record = shaders.prepare(source, scratch/'resolve')
    # Only this private renderer gets the patch and validated generated SPIR-V.
    for name in shader_record['files']:
        path = source/'src/core/gpu/vk/shaders'/name
        shutil.copy2(scratch/'resolve'/name, path)
        files[path.relative_to(source).as_posix()] = base.digest(path)
    headers = ROOT/'.cache/panvk-kbase-source/include'
    if not (headers/'vulkan/vulkan.h').is_file():
        parser.error('Prepare the pinned PanVK checkout first.')
    sdk = ROOT/'build/engine-cross/sdl-sdk'
    if not (sdk/'lib/cmake/SDL2').is_dir():
        parser.error('Build the normal engines first to prepare the SDL link SDK.')
    tool = {name:base.find_tool(name) for name in ('zig', 'cmake', 'ninja')}
    env = dict(os.environ)
    for name in ('CFLAGS', 'CXXFLAGS', 'LDFLAGS', 'CPPFLAGS', 'CPATH',
                 'C_INCLUDE_PATH', 'CPLUS_INCLUDE_PATH', 'LIBRARY_PATH',
                 'SDKROOT', 'MACOSX_DEPLOYMENT_TARGET', 'PKG_CONFIG_PATH', 'SDL2_DIR'):
        env.pop(name, None)
    env.update(ZIG_GLOBAL_CACHE_DIR=str(ROOT/'.cache/zig'),
               ZIG_LOCAL_CACHE_DIR=str(ROOT/'.cache/zig-rgds-vulkan'))
    chain = base.toolchain(scratch/'toolchain', tool['zig'], source,
                           ROOT/'.cache/engine-sdk/SDL2-2.32.10')
    options = {**fork.OPTIONS, 'DSPERATE_VULKAN':'ON',
               'DSPERATE_VULKAN_INCLUDE':str(headers),
               'SDL2_DIR':str(sdk/'lib/cmake/SDL2'),
               'CMAKE_FIND_ROOT_PATH':str(sdk), 'CMAKE_PREFIX_PATH':str(sdk)}
    build = scratch/'build'
    base.configure(tool['cmake'], tool['ninja'], source, build, chain, options, env)
    base.run([tool['cmake'], '--build', build, '--target', 'dsperate',
              '--parallel', args.jobs], env=env)
    actual = {p.relative_to(source).as_posix():base.digest(p)
              for p in source.rglob('*') if p.is_file()}
    if actual != files:
        raise ValueError('Fork source changed during the Vulkan build')
    out = scratch/'rgds-experimental-vulkan'
    shutil.copy2(build/'src/frontend/sdl/dsperate', out)
    header_hashes = {p.relative_to(headers).as_posix():base.digest(p)
                     for name in ('vulkan', 'vk_video')
                     for p in (headers/name).rglob('*') if p.is_file()}
    record = dict(schema=1, prototype=True, resolution=[1, 2, 3, 4, 5],
                  hires_patch_sha256=base.digest(hires_patch),
                  base_source_hash=fork.SOURCE_HASH,
                  source_hash=fork.fingerprint(files), shaders=shader_record, options=options,
                  recipe_sha256=base.digest(Path(__file__)),
                  headers_sha256=fork.fingerprint(header_hashes),
                  binary_sha256=base.digest(out),
                  abi=base.inspect_elf(out, 'private RG DS Vulkan prototype'))
    (scratch/'build.json').write_text(json.dumps(record, indent=2)+'\n')
    print(out)


if __name__ == '__main__':
    main()
