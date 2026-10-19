#!/usr/bin/env python3
"""Compile the private RGDS Vulkan raster shaders from source; never install them.

Upstream DSperate tracks prebuilt SPIR-V beside its lean raster shaders. The
RG DS renderer changes those sources (polygon constants as flat varyings, a
single-sample resolve that writes the CPU readback buffer directly), so every
lean blob is rebuilt here from the patched tree, validated with spirv-val and
recorded with its compiler and input hashes. Only the private Vulkan build
uses the result; the native engine never reads SPIR-V.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
FLAGS = ['-V', '--target-env', 'vulkan1.1', '-Os']
# Output blob -> (source shader, extra defines). The fragment variants are
# upstream's: plain (late depth test), early (no discard), and the two halves
# of a translucent run (opaque pixels, then translucent ones).
SHADERS = {
    'lean_vert.spv': ('lean.vert', []),
    'lean_frag.spv': ('lean.frag', []),
    'lean_frag_early.spv': ('lean.frag', ['-DDS_EARLY']),
    'lean_frag_opq.spv': ('lean.frag', ['-DDS_ALPHA_SEL=1']),
    'lean_frag_trans.spv': ('lean.frag', ['-DDS_ALPHA_SEL=2']),
    'lean_frag_trans_nd.spv': ('lean.frag', ['-DDS_ALPHA_SEL=2', '-DDS_NODISCARD']),   # pass B without discard (DS_LEAN_NODISCARD)
    'lean_resolve_frag.spv': ('lean_resolve.frag', []),
    'lean_resolve_1x_frag.spv': ('lean_resolve.frag', ['-DDS_SAMPLES=1']),
    'lean_resolve_store_1x_frag.spv': ('lean_resolve.frag', ['-DDS_SAMPLES=1', '-DDS_STORE_OUT']),
}
SOURCES = ('shaders/lean.vert', 'shaders/lean.frag', 'shaders/lean_resolve.frag', 'ds_shade.glsl', 'vk_layout.h')


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def inputs(source):
    vk = source/'src/core/gpu/vk'
    record = {name: sha(vk/name) for name in SOURCES}
    record['recipe'] = sha(Path(__file__))
    return record


def validate(source, output):
    record = json.loads((output/'build.json').read_text())
    if record['inputs'] != inputs(source) or record['flags'] != FLAGS or record['shaders'] != {k: list(v) for k, v in SHADERS.items()}:
        raise ValueError('Private Vulkan shader inputs changed')
    if set(record['files']) != set(SHADERS):
        raise ValueError('Incomplete private Vulkan shaders')
    for name, digest in record['files'].items():
        if sha(output/name) != digest:
            raise ValueError('Private Vulkan shader output mismatch')
    return record


def compile_shaders(source, output):
    expected = inputs(source)
    vk = source/'src/core/gpu/vk'
    output.mkdir(parents=True, exist_ok=True)
    for blob, (shader, defines) in SHADERS.items():
        target = output/blob
        subprocess.run(['glslangValidator', *FLAGS, *defines, '-I'+str(vk), '-I'+str(vk/'shaders'),
                        str(vk/'shaders'/shader), '-o', str(target)], check=True, stdout=subprocess.DEVNULL)
        subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', str(target)], check=True)
    if inputs(source) != expected:
        raise ValueError('Shader inputs changed during compilation')
    record = dict(schema=2, inputs=expected, flags=FLAGS, shaders={k: list(v) for k, v in SHADERS.items()},
                  compiler=subprocess.check_output(['glslangValidator', '--version'], text=True),
                  files={name: sha(output/name) for name in SHADERS})
    (output/'build.json').write_text(json.dumps(record, indent=2)+'\n')


def prepare(source, output):
    source, output = source.resolve(), output.resolve()
    if sys.platform == 'darwin':
        env = dict(os.environ, LIMA_HOME=str(ROOT/'.cache/linux-builder/state'))
        subprocess.run([str(ROOT/'.cache/linux-builder/lima/bin/limactl'), 'shell', 'rgds-image',
                        'python3', str(Path(__file__).resolve()), '--source', str(source),
                        '--output', str(output)], env=env, check=True)
    else:
        if output.exists():
            shutil.rmtree(output)
        compile_shaders(source, output)
    return validate(source, output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    prepare(args.source, args.output)


if __name__ == '__main__':
    main()
