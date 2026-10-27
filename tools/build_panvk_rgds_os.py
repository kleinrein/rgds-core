#!/usr/bin/env python3
"""Build PanVK for RG DS OS: upstream Mesa's Vulkan driver for the panfrost kernel driver.

RG DS OS (os/buildroot) runs the open panfrost kernel driver. The release's PanVK (tools/build_panvk.py, a fork)
drives the stock kernel's kbase only, and its Bifrost job queue was rewritten for kbase: on panfrost it retries a
kbase submit forever and DSCore's 2x-5x renderer freezes after its first frames. This builds upstream PanVK from a
Mesa release recent enough to offer HOST_CACHED memory on panfrost (WB_MMAP, kernel driver 1.6), which DSCore's
readback needs (Mesa 25.0, the image's GL driver, has none), in the Lima builder, into build/panvk-rgds-os.
tools/own_os_build.py puts it in the image as /usr/lib/rgds-panvk, and runtime/emulators.py uses it on RG DS OS. Bifrost is not a conformant PanVK target, so it needs PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1.

  build_panvk_rgds_os.py [--jobs N]
"""
import argparse
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
MESA = 'mesa-26.2.5'
URL = 'https://archive.mesa3d.org/' + MESA + '.tar.xz'
SHA256 = '794b3d1729075dae7ebd47c7283b7ca0cdb5dec668f7f11cc0a5d82f6138a0a5'  # docs.mesa3d.org/relnotes/26.2.5.html
TARBALL = Path.home() / 'own-os/dl/panvk' / (MESA + '.tar.xz')
OPTIONS = [
    '--buildtype=release', '-Dvulkan-drivers=panfrost', '-Dgallium-drivers=', '-Dplatforms=', '-Degl=disabled',
    '-Dglx=disabled', '-Dgbm=disabled', '-Dgles1=disabled', '-Dgles2=disabled', '-Dopengl=false', '-Dllvm=enabled',
    '-Dshared-llvm=enabled', '-Dmesa-clc=enabled', '-Dbuild-tests=false', '-Dvideo-codecs=', '-Dglvnd=disabled',
    '-Dvalgrind=disabled', '-Dzstd=disabled', '-Dxmlconfig=disabled', '-Dlibunwind=disabled'
]
OUT = ROOT / 'build/panvk-rgds-os'


def run(args, **kwargs):
    print('+', ' '.join(str(a) for a in args), flush=True)
    return subprocess.run([str(a) for a in args], check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--native', action='store_true', help=argparse.SUPPRESS)
    parser.add_argument('--jobs', type=int, default=6)
    args = parser.parse_args()
    if not args.native and sys.platform == 'darwin':
        env = dict(os.environ, LIMA_HOME=str(ROOT / '.cache/linux-builder/state'))
        run(
            [
                ROOT / '.cache/linux-builder/lima/bin/limactl', 'shell', 'rgds-image', 'python3', '-u',
                Path(__file__).resolve(), '--native', '--jobs', args.jobs
            ],
            env=env
        )
        return
    if sys.platform != 'linux' or platform.machine() not in ('aarch64', 'arm64'):
        raise SystemExit('Use the existing ARM64 Linux builder or an ARM64 Linux host.')
    if not TARBALL.is_file():
        TARBALL.parent.mkdir(parents=True, exist_ok=True)
        run(['curl', '-fsSL', '-o', TARBALL.with_suffix('.part'), URL])
        actual = subprocess.check_output(['sha256sum', TARBALL.with_suffix('.part')], text=True).split()[0]
        if actual != SHA256: raise SystemExit(f'{MESA}.tar.xz does not match its published SHA-256.')
        TARBALL.with_suffix('.part').rename(TARBALL)
    work = Path.home() / 'own-os/panvk' / MESA
    source, build = work / MESA, work / 'build'
    if not source.is_dir():
        work.mkdir(parents=True, exist_ok=True)
        run(['tar', '-xf', TARBALL, '-C', work])
    meson = ROOT / '.cache/panvk-build-venv/bin/meson'
    if not meson.is_file(): raise SystemExit('Prepare the pinned Meson environment; see docs/RGDS_VULKAN.md.')
    env = dict(os.environ, CC='clang-18', CXX='clang++-18', PATH=str(meson.parent) + os.pathsep + os.environ['PATH'])
    command = [meson, 'setup'] + (['--reconfigure'] if (build / 'build.ninja').exists() else [])
    run([*command, build, source, *OPTIONS], env=env)
    run(['ninja', '-C', build, '-j' + str(args.jobs), 'src/panfrost/vulkan/libvulkan_panfrost.so'], env=env)
    OUT.mkdir(parents=True, exist_ok=True)
    shutil.copy2(build / 'src/panfrost/vulkan/libvulkan_panfrost.so', OUT / 'libvulkan_panfrost.so')
    run(['sha256sum', OUT / 'libvulkan_panfrost.so'])
    run(['readelf', '-d', OUT / 'libvulkan_panfrost.so'])


if __name__ == '__main__':
    main()
