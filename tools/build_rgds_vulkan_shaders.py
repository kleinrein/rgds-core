#!/usr/bin/env python3
"""Compile the private RGDS Vulkan resolve optimization; never install it."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT/'tools/engine_patches/rgds-vulkan-resolve.patch'
FLAGS = ['-V', '--target-env', 'vulkan1.1', '-Os', '-DDS_SAMPLES=1']


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inputs(source):
    vk = source/'src/core/gpu/vk'
    return {'shader': sha(vk/'shaders/lean_resolve.frag'),
            'layout': sha(vk/'vk_layout.h'), 'patch': sha(PATCH),
            'recipe': sha(Path(__file__))}


def validate(source, output):
    record = json.loads((output/'build.json').read_text())
    if record['inputs'] != inputs(source) or record['flags'] != FLAGS:
        raise ValueError('Private Vulkan shader inputs changed')
    for name, digest in record['files'].items():
        if name not in ('lean_resolve.frag', 'lean_resolve_1x_frag.spv') or sha(output/name) != digest:
            raise ValueError('Private Vulkan shader output mismatch')
    if set(record['files']) != {'lean_resolve.frag', 'lean_resolve_1x_frag.spv'}:
        raise ValueError('Incomplete private Vulkan shaders')
    return record


def compile_resolve(source, output):
    expected = inputs(source)
    tree = output/'source'
    vk = tree/'src/core/gpu/vk'
    (vk/'shaders').mkdir(parents=True, exist_ok=True)
    for name in ('shaders/lean_resolve.frag', 'vk_layout.h'):
        shutil.copy2(source/'src/core/gpu/vk'/name, vk/name)
    subprocess.run(['patch', '-p1', '--batch', '-i', str(PATCH)], cwd=tree, check=True)
    shader = vk/'shaders/lean_resolve.frag'
    blob = output/'lean_resolve_1x_frag.spv'
    subprocess.run(['glslangValidator', *FLAGS, '-I'+str(vk), str(shader), '-o', str(blob)], check=True)
    subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', str(blob)], check=True)
    shutil.copy2(shader, output/'lean_resolve.frag')
    if inputs(source) != expected:
        raise ValueError('Shader inputs changed during compilation')
    record = dict(schema=1, inputs=expected, flags=FLAGS,
                  compiler=subprocess.check_output(['glslangValidator', '--version'], text=True),
                  files={name:sha(output/name) for name in ('lean_resolve.frag', 'lean_resolve_1x_frag.spv')})
    (output/'build.json').write_text(json.dumps(record, indent=2)+'\n')


def prepare(source, output):
    source, output = source.resolve(), output.resolve()
    if sys.platform == 'darwin':
        env = dict(os.environ, LIMA_HOME=str(ROOT/'.cache/linux-builder/state'))
        subprocess.run([str(ROOT/'.cache/linux-builder/lima/bin/limactl'), 'shell', 'rgds-image',
                        'python3', str(Path(__file__).resolve()), '--source', str(source),
                        '--output', str(output)], env=env, check=True)
    else:
        compile_resolve(source, output)
    return validate(source, output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    prepare(args.source, args.output)


if __name__ == '__main__':
    main()
