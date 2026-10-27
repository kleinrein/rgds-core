#!/usr/bin/env python3
"""Build RG DS OS's SD card image (os/buildroot, docs/OWN_OS_PLAN.md) in the ARM64 Lima builder.

On the Mac it stages RG DS Core for the image in build/os-stage/rgds-core/app: a release package's app folder (its
emulators and the Samba bundle for file sharing), with the repository's runtime, themes and assets and the current device
build of the launcher on top. Then it runs Buildroot in the VM, on the VM's own ext4 disk under ~/own-os (Buildroot
does not build on a case-insensitive macOS mount), and copies the image to .cache/own-os/build/rgds-os.img.gz.
The first build takes hours (toolchain, kernel, Mesa); later ones only redo what changed.

  own_os_build.py [--release dist/RGDS-<version>.zip] [--stage-only] [--reconfigure] [--jobs N] [-- make target...]
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
BUILDROOT_URL = 'https://github.com/buildroot/buildroot'
BUILDROOT_VERSION = '2026.02.3'
APT = [
    'build-essential', 'git', 'rsync', 'bc', 'cpio', 'unzip', 'file', 'wget', 'python3', 'perl', 'patch',
    'libncurses-dev', 'xz-utils', 'bzip2'
]
STAGE = ROOT / 'build/os-stage/rgds-core'


def run(*args, **kwargs):
    print('+', ' '.join(str(a) for a in args), flush=True)
    return subprocess.run([str(a) for a in args], check=True, **kwargs)


def stage(release):
    launcher = ROOT / 'build/device/rgds-launcher'
    if not launcher.is_file():
        raise SystemExit('No device build: run tools/build.py device first.')
    # The launcher must match the tree it is staged with: twice on 27 October 2026 another session's shader catalog
    # edit went into an image next to an older launcher, which then refused to start ("Shader catalog/pipeline
    # mismatch") on every boot. build/device/build.json records the sources it was built from.
    record = json.loads((ROOT / 'build/device/build.json').read_text())
    stale = [
        name for name, digest in record['sources'].items()
        if not (ROOT / name).is_file() or hashlib.sha256((ROOT / name).read_bytes()).hexdigest() != digest
    ]
    if stale:
        raise SystemExit(
            'build/device is older than the tree (' + ', '.join(stale[:4]) + '): run tools/build.py device first.'
        )
    app = STAGE / 'app'
    if STAGE.exists():
        shutil.rmtree(STAGE)
    with zipfile.ZipFile(release) as archive:
        prefix = next(n for n in archive.namelist() if n.startswith('rgds/releases/') and n.count('/') == 3)
        prefix = prefix[:prefix.index('/', len('rgds/releases/')) + 1]
        for info in archive.infolist():
            name = info.filename[len(prefix):]
            if not info.filename.startswith(prefix) or not name or info.is_dir():
                continue
            target = app / name
            target.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(info) as source, target.open('wb') as out:
                shutil.copyfileobj(source, out)
            mode = info.external_attr >> 16
            if mode & 0o111:
                target.chmod(0o755)
    # The engines as built from this checkout's dscore/ (tools/build_rgds_experimental.py, build_rgds_vulkan.py +
    # package_rgds_vulkan.py), the way package.py puts them in a release: the image carries this tree's engine, as it
    # carries its runtime, whether or not a release has been packaged since.
    for folder, record in (('rgds-experimental', 'build.json'), ('rgds-vulkan', 'vulkan-build.json')):
        source = ROOT / 'build/engines' / folder
        if not (source / record).is_file(): continue
        for name in (*json.loads((source / record).read_text())['files'], record):
            target = app / 'emulators/rgds-experimental' / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source / name, target)
        print(f'engine from build/engines/{folder}', flush=True)
    for folder in ('runtime', 'themes', 'assets'):
        for source in sorted((ROOT / folder).rglob('*')):
            if source.is_file() and '__pycache__' not in source.parts and source.name != '.DS_Store' \
                    and not source.name.startswith('._'):
                target = app / source.relative_to(ROOT)
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, target)
    # What package.py adds to a release from this tree: emulator profiles, the RGDS shader presets, RG DS Play.
    for name in ('drastic.cfg', 'dsperate.ini'):
        shutil.copy2(ROOT / 'assets/emulator-profiles' / name, app / 'emulators' / name)
    for source in sorted((ROOT / 'assets/shaders').glob('*.dsd')):
        shutil.copy2(source, app / 'emulators/shaders' / source.name)
    # No DraStic on this OS to seed the cheat database from (runtime/emulators.py dscore_cheat_database).
    cheats = ROOT / 'vendor/drastic/usrcheat.dat'
    if cheats.is_file():
        (app / 'emulators/cheats').mkdir(parents=True, exist_ok=True)
        shutil.copy2(cheats, app / 'emulators/cheats/usrcheat.dat')
    else:
        print('warning: vendor/drastic/usrcheat.dat missing, the image will have no cheats', flush=True)
    payload = ROOT / 'build/play/device/payload'
    if not payload.is_dir():
        raise SystemExit('No RG DS Play build: run tools/build_play.py device first.')
    sys.path.insert(0, str(ROOT / 'tools'))
    from build_play import validate_payload
    for name in (*validate_payload(payload)['files'], 'build.json'):
        target = app / 'emulators/rgds-play' / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(payload / name, target)
    shutil.copy2(ROOT / 'build/device/build.json', app / 'build/rgds.json')
    shutil.copy2(launcher, app / 'bin/rgds-launcher')
    shutil.copy2(ROOT / 'build/device/librgdsbridge.so', app / 'emulators/hooks/librgdsbridge.so')
    (app / 'bin/rgds-launcher').chmod(0o755)
    # PanVK with the panfrost kernel backend (tools/build_panvk_rgds_os.py) for DSCore's 2x-5x renderer: the
    # release's own driver only knows the stock kernel's kbase. Installed as /usr/lib/rgds-panvk.
    driver = ROOT / 'build/panvk-rgds-os/libvulkan_panfrost.so'
    if not driver.is_file():
        raise SystemExit('No RG DS OS PanVK build: run tools/build_panvk_rgds_os.py first.')
    (STAGE / 'panvk').mkdir()
    shutil.copy2(driver, STAGE / 'panvk' / driver.name)
    shutil.copy2(app / 'emulators/rgds-experimental/lib/libzstd.so.1', STAGE / 'panvk/libzstd.so.1')
    # A release of its own: emulator launches verify every file against manifest.json, and the folder is named after
    # the build (runtime/activate.py), so the image installs it as /usr/share/rgds/releases/<build>.
    manifest = json.loads((app / 'manifest.json').read_text())
    files = {}
    for path in sorted(app.rglob('*')):
        name = path.relative_to(app).as_posix()
        if path.is_file() and name != 'manifest.json':
            files[name] = hashlib.sha256(path.read_bytes()).hexdigest()
    base = manifest['build'].split('-os')[0]
    build = base + '-os' + hashlib.sha256(json.dumps(sorted(files.items())).encode()).hexdigest()[:8]
    manifest.update(build=build, files=files, cores=sorted(set(manifest.get('cores', [])) | {'rgds-play'}))
    (app / 'manifest.json').write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n')
    (STAGE / 'build-id').write_text(build + '\n')
    print(f'staged {build} in {app} from {release.name} + this checkout', flush=True)


def native(args):
    if sys.platform != 'linux' or platform.machine() not in ('aarch64', 'arm64'):
        raise SystemExit('Use the existing ARM64 Linux builder or an ARM64 Linux host.')
    missing = [p for p in APT if shutil.which(p) is None and p not in ('build-essential', 'libncurses-dev', 'xz-utils')]
    if missing or not Path('/usr/include/ncurses.h').exists():
        run('sudo', 'apt-get', 'update', '-q')
        run('sudo', 'env', 'DEBIAN_FRONTEND=noninteractive', 'apt-get', 'install', '-yq', *APT)
    work = Path.home() / 'own-os'
    buildroot = work / 'buildroot'
    if not (buildroot / 'Makefile').exists():
        run('git', 'clone', '-q', '--depth', '1', '--branch', BUILDROOT_VERSION, BUILDROOT_URL, buildroot)
    out = work / 'output/rgds-os'
    env = dict(os.environ, BR2_DL_DIR=str(work / 'dl'))
    if not (out / '.config').exists() or args.reconfigure:
        run('make', '-C', buildroot, f'O={out}', f'BR2_EXTERNAL={ROOT/"os/buildroot"}', 'rgds_os_defconfig', env=env)
    if not args.targets:
        # Buildroot copies a local package with rsync -u, which keeps a newer leftover over an older staged file
        # (copy2 keeps the device build's dates): start RG DS Core's copy afresh every time.
        run('make', '-C', out, 'rgds-core-dirclean', env=env)
    run('make', '-C', out, f'-j{args.jobs}', *args.targets, env=env)
    if args.targets:
        return
    # The image's RG DS Core must pass the check every game launch makes (runtime/activate.py).
    sys.path.insert(0, str(ROOT / 'runtime'))
    from activate import verify_release
    build = (STAGE / 'build-id').read_text().strip()
    verify_release(out / 'target/usr/share/rgds/releases' / build)
    print(f'{build} verified in the image', flush=True)
    image = out / 'images/rgds-os.img.gz'
    target = ROOT / '.cache/own-os/build'
    target.mkdir(parents=True, exist_ok=True)
    shutil.copy2(image, target / image.name)
    # what tools/own_os_push.py writes into a running handheld's other slot
    for name in ('rootfs.squashfs', 'Image.lz4', 'rk3568-anbernic-rg-ds.dtb', 'extlinux.conf', 'u-boot-rockchip.bin'):
        shutil.copy2(out / 'images' / name, target / name)
    run('sha256sum', target / image.name)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--native', action='store_true', help=argparse.SUPPRESS)
    parser.add_argument('--release', type=Path, help='release package with the emulators (default: dist/latest)')
    parser.add_argument('--stage-only', action='store_true')
    parser.add_argument('--jobs', type=int, default=6)
    parser.add_argument('--reconfigure', action='store_true', help='regenerate output/rgds-os/.config')
    parser.add_argument('targets', nargs='*', help='Buildroot make targets instead of the whole image')
    args = parser.parse_args()
    if args.native:
        return native(args)
    if sys.platform == 'darwin':
        release = args.release or ROOT / 'dist' / (ROOT / 'dist/latest.txt').read_text().strip()
        if release.suffix != '.zip':
            release = release.with_name(release.name + '.zip')
        stage(release)
        if args.stage_only:
            return
        env = dict(os.environ, LIMA_HOME=str(ROOT / '.cache/linux-builder/state'))
        command = [
            ROOT / '.cache/linux-builder/lima/bin/limactl', 'shell', 'rgds-image', 'python3', '-u',
            Path(__file__).resolve(), '--native', '--jobs', args.jobs
        ]
        run(*command, *(['--reconfigure'] if args.reconfigure else []), *args.targets, env=env)
        return
    native(args)


if __name__ == '__main__':
    main()
