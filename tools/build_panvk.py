#!/usr/bin/env python3
"""Build a private RG DS PanVK/kbase prototype. Never install or alter a device."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
COMMIT = 'efd07bab401b263a9ac114560ecdb9a2934f5d43'
UPSTREAM = 'https://github.com/FristOneRR-Admin/FristOneRR-Panvk-Source'
PATCH = ROOT/'tools/engine_patches/panvk-kbase-linux.patch'
OPTIONS = ['--buildtype=release', '--prefix=/opt/rgds-panvk', '--libdir=lib',
           '-Dpanfrost-kmds=kbase', '-Dvulkan-drivers=panfrost', '-Dgallium-drivers=',
           '-Dpanfrost-rust=false', '-Dplatforms=', '-Degl=disabled', '-Dglx=disabled',
           '-Dgbm=disabled', '-Dgles1=disabled', '-Dgles2=disabled', '-Dopengl=false',
           '-Dllvm=enabled', '-Dshared-llvm=enabled', '-Dmesa-clc=enabled',
           '-Dbuild-tests=false', '-Dvideo-codecs=', '-Dglvnd=disabled', '-Dvalgrind=disabled']


def run(args, **kwargs):
    return subprocess.run([str(x) for x in args], check=True, **kwargs)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', action='store_true', help=argparse.SUPPRESS)
    parser.add_argument('--source', type=Path, default=ROOT/'.cache/panvk-kbase-source')
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    if args.jobs < 1: parser.error('--jobs must be positive')
    if not args.native and sys.platform == 'darwin':
        env = dict(os.environ, LIMA_HOME=str(ROOT/'.cache/linux-builder/state'))
        run([ROOT/'.cache/linux-builder/lima/bin/limactl', 'shell', 'rgds-image', 'python3',
             Path(__file__).resolve(), '--native', '--source', args.source.resolve(), '--jobs', args.jobs], env=env)
        return
    if sys.platform != 'linux' or platform.machine() not in ('aarch64', 'arm64'):
        raise SystemExit('Use the existing ARM64 Linux builder or an ARM64 Linux host.')
    source = args.source.resolve()
    git = lambda *a: subprocess.check_output(['git', '-C', str(source), *a], text=True)
    if git('rev-parse', 'HEAD').strip() != COMMIT: raise SystemExit('Unexpected PanVK source revision.')
    original = git('show', 'HEAD:meson.build')
    patched = original.replace("add_project_link_arguments(\n  '-llog',\n  language : ['c', 'cpp'],\n)",
                               "if host_machine.system() == 'android'\n  add_project_link_arguments('-llog', language : ['c', 'cpp'])\nendif")
    if patched == original: raise SystemExit('Pinned Android logging block is missing.')
    changes = git('status', '--porcelain', '--untracked-files=all').splitlines()
    if any(line != ' M meson.build' for line in changes): raise SystemExit('Unexpected driver source changes.')
    if (source/'meson.build').read_text() == original:
        run(['patch', '-p1', '--batch', '-i', PATCH], cwd=source)
    if (source/'meson.build').read_text() != patched: raise SystemExit('Linux patch differs from the recorded source.')
    meson = ROOT/'.cache/panvk-build-venv/bin/meson'
    if not meson.is_file(): raise SystemExit('Prepare the pinned Meson 1.7.2 environment; see docs/RGDS_VULKAN.md.')
    if subprocess.check_output([meson, '--version'], text=True).strip() != '1.7.2':
        raise SystemExit('This prototype records Meson 1.7.2.')
    env = dict(os.environ, CC='clang-18', CXX='clang++-18', PATH=str(meson.parent)+os.pathsep+os.environ['PATH'])
    build = ROOT/'build/panvk-kbase'; out = ROOT/'build/panvk-private'
    command = [meson, 'setup']
    if (build/'build.ninja').exists(): command += ['--reconfigure']
    run([*command, build, source, *OPTIONS], env=env)
    run(['ninja', '-C', build, '-j'+str(args.jobs), 'src/panfrost/vulkan/libvulkan_panfrost.so'], env=env)
    out.mkdir(parents=True, exist_ok=True)
    shutil.copy2(build/'src/panfrost/vulkan/libvulkan_panfrost.so', out/'libvulkan_panfrost.so')
    shutil.copy2('/lib/aarch64-linux-gnu/libzstd.so.1', out/'libzstd.so.1')
    shutil.copy2('/usr/share/doc/libzstd1/copyright', out/'libzstd-copyright')
    record = dict(schema=1, prototype=True, upstream=UPSTREAM, commit=COMMIT,
                  patch_sha256=sha(PATCH), recipe_sha256=sha(Path(__file__)), options=OPTIONS,
                  compiler=subprocess.check_output(['clang-18', '--version'], text=True).splitlines()[0],
                  zstd_package=subprocess.check_output(['dpkg-query', '-W', 'libzstd1'], text=True).strip(),
                  files={p.name:sha(p) for p in out.iterdir() if p.is_file() and p.name!='build.json'})
    (out/'build.json').write_text(json.dumps(record, indent=2)+'\n')
    print(out)


if __name__ == '__main__': main()
