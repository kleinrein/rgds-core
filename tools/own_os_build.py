#!/usr/bin/env python3
"""Build RG DS OS's SD card image (os/buildroot, docs/OWN_OS_PLAN.md) in the ARM64 Lima builder.

On the Mac it stages RG DS Core for the image in build/os-stage/rgds-core/app: a release package's app folder (its
emulators and the Samba bundle for file sharing), with the repository's runtime, themes and assets and the current device
build of the launcher on top. Then it runs Buildroot in the VM, on the VM's own ext4 disk under ~/own-os (Buildroot
does not build on a case-insensitive macOS mount), and copies the image to .cache/own-os/build/rgds-os.img.gz.
The first build takes hours (toolchain, kernel, Mesa); later ones only redo what changed.

  own_os_build.py [--release dist/RGDS-<version>.zip] [--stage-only] [--slot-only] [--reconfigure] [--jobs N]
                  [--no-3ds] [-- make target...]

The 3DS engine (Tanuki3DS) comes from build/engines/tanuki3ds, which tools/tanuki3ds/stage_engine.py fills; the
image goes to /usr/share/rgds/engines/tanuki3ds. Without it the build stops: 0.9.0 went out with no 3DS engine
because it was only ever on the owner's data partition. --no-3ds builds an image without 3DS games on purpose.

--slot-only builds what tools/own_os_push.py writes (root filesystem, kernel, device trees) without the two SD card
images, and removes the cached ones so own_os_release.py cannot pick up a card from an older build.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
BUILDROOT_URL = 'https://github.com/buildroot/buildroot'
BUILDROOT_VERSION = '2026.02.3'
APT = [
    'build-essential', 'git', 'rsync', 'bc', 'cpio', 'unzip', 'file', 'wget', 'python3', 'perl', 'patch',
    'libncurses-dev', 'xz-utils', 'bzip2', 'zip'
]
STAGE = ROOT / 'build/os-stage/rgds-core'


def run(*args, **kwargs):
    print('+', ' '.join(str(a) for a in args), flush=True)
    return subprocess.run([str(a) for a in args], check=True, **kwargs)


TANUKI = ROOT / 'build/engines/tanuki3ds'


def stage_tanuki(stage_dir, skip):
    """The 3DS engine for /usr/share/rgds/engines/tanuki3ds, exactly the files its engine.json lists."""
    record = TANUKI / 'engine.json'
    if skip:
        print('warning: --no-3ds, the image has no 3DS engine and shows no 3DS games', flush=True)
        return
    if not record.is_file():
        raise SystemExit(
            'No 3DS engine staged: run tools/tanuki3ds/stage_engine.py HOST first (or --no-3ds on purpose).'
        )
    files = json.loads(record.read_text())['files']
    for name, digest in files.items():
        source = TANUKI / name
        if not source.is_file() or hashlib.sha256(source.read_bytes()).hexdigest() != digest:
            raise SystemExit(f'build/engines/tanuki3ds/{name} does not match engine.json: stage it again.')
    target = stage_dir / 'engines/tanuki3ds'
    for name in (*files, 'engine.json'):
        (target / name).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(TANUKI / name, target / name)
    for name in ('run.sh', 'ctremu'):
        (target / name).chmod(0o755)
    size = sum((TANUKI / name).stat().st_size for name in files)
    print(f'3DS engine from build/engines/tanuki3ds ({len(files)} files, {size >> 20} MB)', flush=True)


SOURCES = ROOT / 'build/os-stage/sources'


def move_sources(app):
    """The engines' and Samba's corresponding source archives (about 190 MB, already compressed) leave the image:
    own_os_release.py publishes them as RG-DS-OS-<version>-sources.tar beside the image and the update on the same
    release, and SOURCES.md in the release says so, with each archive's SHA-256. The root slot is 768 MiB."""
    if SOURCES.exists():
        shutil.rmtree(SOURCES)
    SOURCES.mkdir(parents=True)
    shutil.copy2(ROOT / 'THIRD_PARTY.md', app / 'THIRD_PARTY.md')  # this tree's, which says where the sources went
    moved = {}
    for path in sorted(app.rglob('*source.tar.gz')):
        name = path.relative_to(app).as_posix()
        target = SOURCES / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.move(path, target)
        moved[name] = hashlib.sha256(target.read_bytes()).hexdigest()
    version = (ROOT / 'VERSION').read_text().strip()
    repo = (ROOT / 'packaging/release-repo.txt').read_text().strip()
    (app / 'SOURCES.md').write_text(
        '# Corresponding source\n\n'
        'The complete source of the GPL, LGPL and MPL programs in this release (DSCore, RG DS Play with mGBA and its\n'
        'libretro front end, the PanVK Vulkan driver from Mesa, Samba), with the build recipes and patches, is\n'
        f'published with it as `RG-DS-OS-{version}-sources.tar` on https://github.com/{repo}/releases/tag/v{version}\n'
        '(the same place as the image and the update). It holds these archives, at these paths:\n\n' +
        ''.join(f'- `{name}` sha256 `{digest}`\n' for name, digest in moved.items()) +
        '\nThird-party notices: THIRD_PARTY.md and each component\'s LICENSE, COPYING and NOTICE.txt.\n'
    )
    size = sum((SOURCES / name).stat().st_size for name in moved)
    print(f'{len(moved)} source archives ({size >> 20} MB) moved out of the image to {SOURCES}', flush=True)
    return moved


def stage(release, no_3ds=False):
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
    # What package.py adds to a release from this tree: DSCore's profile, the RGDS shader presets, RG DS Play.
    shutil.copy2(ROOT / 'assets/emulator-profiles/dsperate.ini', app / 'emulators/dsperate.ini')
    for source in sorted((ROOT / 'assets/shaders').glob('*.dsd')):
        shutil.copy2(source, app / 'emulators/shaders' / source.name)
    # The Action Replay database DSCore seeds its cheats from (runtime/emulators.py dscore_cheat_database).
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
    # OS updates (runtime/os_update.py): the update channel and the guard ship with the image, read-only
    (app / 'os').mkdir(exist_ok=True)
    shutil.copy2(ROOT / 'packaging/update_guard.py', app / 'os/update_guard.py')
    shutil.copy2(ROOT / 'packaging/update-trust.json', app / 'os/update-trust.json')
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
    sources = move_sources(app)
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
    (SOURCES / 'sources.json').write_text(json.dumps(dict(build=build, files=sources), indent=2, sort_keys=True) + '\n')
    stage_tanuki(STAGE, no_3ds)
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
    env = dict(os.environ, BR2_DL_DIR=str(work / 'dl'), RGDS_SLOT_ONLY='1' if args.slot_only else '0')
    if not (out / '.config').exists() or args.reconfigure:
        run('make', '-C', buildroot, f'O={out}', f'BR2_EXTERNAL={ROOT/"os/buildroot"}', 'rgds_os_defconfig', env=env)
    if not args.targets:
        # Buildroot copies a local package with rsync -u, which keeps a newer leftover over an older staged file
        # (copy2 keeps the device build's dates): start RG DS Core's copy afresh every time.
        # The music engine is a local package too (os/buildroot/package/rgds-music, from src/music).
        # So is the pad daemon (os/buildroot/package/rgds-pad, from src/pad).
        run('make', '-C', out, 'rgds-core-dirclean', 'rgds-music-dirclean', 'rgds-pad-dirclean', env=env)
        if kernel_config_drift(out):
            # a changed kernel fragment (board/rgds/linux-*.fragment): Buildroot only merges them when the kernel
            # is configured, so an existing kernel build would keep the old settings without this
            run('make', '-C', out, f'-j{args.jobs}', 'linux-reconfigure', env=env)
    run('make', '-C', out, f'-j{args.jobs}', *args.targets, env=env)
    if args.targets:
        return
    drift = kernel_config_drift(out)
    if drift:
        raise SystemExit('The kernel was built without these fragment settings: ' + ', '.join(drift[:12]))
    # The image's RG DS Core must pass the check every game launch makes (runtime/activate.py).
    sys.path.insert(0, str(ROOT / 'runtime'))
    from activate import verify_release
    build = (STAGE / 'build-id').read_text().strip()
    verify_release(out / 'target/usr/share/rgds/releases' / build)
    print(f'{build} verified in the image', flush=True)
    check_cores(out)
    target = ROOT / '.cache/own-os/build'
    target.mkdir(parents=True, exist_ok=True)
    # the 3DS engine's record beside the image, which own_os_release.py asks for (0.9.0 shipped without the engine)
    engine = out / 'target/usr/share/rgds/engines/tanuki3ds'
    (target / 'tanuki3ds.json').unlink(missing_ok=True)
    if not args.no_3ds:
        if not (engine / 'run.sh').is_file(): raise SystemExit('The image has no 3DS engine.')
        shutil.copy2(engine / 'engine.json', target / 'tanuki3ds.json')
    # the card for both handhelds, and the one fixed to the RG DS Plus (board/rgds/post-image.sh)
    images = [out / 'images/rgds-os.img.gz', out / 'images/rgds-os-plus.img.gz']
    for image in images:
        if args.slot_only: (target / image.name).unlink(missing_ok=True)
        else: shutil.copy2(image, target / image.name)
    # what tools/own_os_push.py writes into a running handheld's other slot
    for name in (
        'rootfs.squashfs', 'Image.lz4', 'rk3568-anbernic-rg-ds.dtb', 'rk3568-anbernic-rg-ds-plus.dtb', 'extlinux.conf',
        'u-boot-rockchip.bin'
    ):
        shutil.copy2(out / 'images' / name, target / name)
    (target / 'build-id').write_text(build + '\n')  # what own_os_push.py and own_os_release.py call this build
    if not args.slot_only: run('sha256sum', *(target / image.name for image in images))


def kernel_fragments(text):
    """The kernel config fragments rgds_os_defconfig (or an output .config) names, as paths in this checkout."""
    found = re.search(r'^BR2_LINUX_KERNEL_CONFIG_FRAGMENT_FILES="([^"]*)"', text, re.M)
    if not found:
        return []
    return [
        Path(name.replace('$(BR2_EXTERNAL_RGDS_PATH)', str(ROOT / 'os/buildroot'))) for name in found.group(1).split()
    ]


def kernel_config_drift(out, kernel_config=None):
    """Fragment settings the kernel's .config does not carry (empty when it matches, or before the first build).

    Buildroot keeps output/.config until --reconfigure (a fragment newly listed in the defconfig is ignored, so
    that stops the build), and merges the fragments into the kernel's .config only when the kernel is configured
    (native() then runs linux-reconfigure). A setting Kconfig itself overrides shows up here after the build too.
    """
    defconfig = (ROOT / 'os/buildroot/configs/rgds_os_defconfig').read_text()
    if kernel_fragments(defconfig) != kernel_fragments((out / '.config').read_text()):
        raise SystemExit(
            'output/rgds-os/.config names other kernel fragments than the defconfig: run with --reconfigure.'
        )
    kernel_config = kernel_config or out / 'build/linux-custom/.config'
    if not kernel_config.is_file():
        return []
    have = {}
    for line in kernel_config.read_text().splitlines():
        if line.startswith('CONFIG_') and '=' in line:
            name, value = line.split('=', 1)
            have[name] = value
    drift = []
    for fragment in kernel_fragments(defconfig):
        for line in fragment.read_text().splitlines():
            if not line.startswith('CONFIG_') or '=' not in line:
                continue
            name, value = line.split('=', 1)
            # "=n" and "is not set" are the same thing, as is a symbol this kernel does not have
            if have.get(name, 'n') != value:
                drift.append(f'{name}={value} (built: {have.get(name, "n")})')
    return drift


def check_cores(out):
    """Every libretro core the defconfig enables must be in the image.

    Buildroot keeps an old output/.config (new defconfig lines are ignored without --reconfigure) and stale package
    stamps (a core can be built yet never installed), and either way the image ships without a system's core.
    """
    defconfig = (ROOT / 'os/buildroot/configs/rgds_os_defconfig').read_text()
    wanted = set(re.findall(r'^(BR2_PACKAGE_LIBRETRO_\w+)=y', defconfig, re.M))
    config = (out / '.config').read_text()
    stale = sorted(w for w in wanted if f'{w}=y' not in config)
    if stale:
        raise SystemExit(f'output/rgds-os/.config lacks {", ".join(stale)}: run with --reconfigure.')
    absent = []
    for option in sorted(wanted):
        package = 'libretro-' + option.removeprefix('BR2_PACKAGE_LIBRETRO_').lower().replace('_', '-')
        recipe = (ROOT / 'os/buildroot/package' / package / f'{package}.mk').read_text()
        for name in re.findall(r'/usr/lib/libretro/(\w+_libretro\.so)', recipe):
            if not (out / 'target/usr/lib/libretro' / name).is_file(): absent.append(f'{name} ({package})')
    if absent:
        raise SystemExit(
            'The image lacks cores the defconfig enables: ' + ', '.join(absent) +
            '. Run "make <package>-dirclean && make <package>" in output/rgds-os, then build again.'
        )
    print(f'{len(wanted)} libretro cores present in the image', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--native', action='store_true', help=argparse.SUPPRESS)
    parser.add_argument('--release', type=Path, help='release package with the emulators (default: dist/latest)')
    parser.add_argument('--stage-only', action='store_true')
    parser.add_argument('--slot-only', action='store_true', help='no SD card images: what make push needs')
    parser.add_argument('--jobs', type=int, default=6)
    parser.add_argument('--reconfigure', action='store_true', help='regenerate output/rgds-os/.config')
    parser.add_argument('--no-3ds', action='store_true', help='build without the 3DS engine on purpose')
    parser.add_argument('targets', nargs='*', help='Buildroot make targets instead of the whole image')
    args = parser.parse_args()
    if args.native:
        return native(args)
    if sys.platform == 'darwin':
        release = args.release or ROOT / 'dist' / (ROOT / 'dist/latest.txt').read_text().strip()
        if release.suffix != '.zip':
            release = release.with_name(release.name + '.zip')
        stage(release, args.no_3ds)
        if args.stage_only:
            return
        env = dict(os.environ, LIMA_HOME=str(ROOT / '.cache/linux-builder/state'))
        command = [
            ROOT / '.cache/linux-builder/lima/bin/limactl', 'shell', 'rgds-image', 'python3', '-u',
            Path(__file__).resolve(), '--native', '--jobs', args.jobs
        ]
        run(
            *command,
            *(['--reconfigure'] if args.reconfigure else []),
            *(['--slot-only'] if args.slot_only else []),
            *(['--no-3ds'] if args.no_3ds else []),
            *args.targets,
            env=env
        )
        return
    native(args)


if __name__ == '__main__':
    main()
