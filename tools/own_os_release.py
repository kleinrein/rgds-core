#!/usr/bin/env python3
"""Publish an RG DS OS release: the signed image the handheld installs into its other slot (runtime/os_update.py).

    python3 tools/own_os_build.py                      # the image, .cache/own-os/build/
    python3 tools/own_os_release.py --version 0.8.0 [--notes notes.txt] [--channel stable] [--upload]

writes dist/release-os/<version>/: the card image for new installs (RG-DS-OS-<version>.img.gz and its .sha256),
RG-DS-OS-Plus-<version>.img.gz (the card fixed to the RG DS Plus, should its detection pick wrong),
the update RG-DS-OS-<build>.zip (rootfs.squashfs, Image.lz4, the device trees, extlinux.conf and manifest.json under
<build>/) and <channel>-os.json, the feed signed with the key tools/release.py init made. Handhelds read the feed
from the latest GitHub release (packaging/update-trust.json's stable.json -> stable-os.json), so a release is only
offered once it is published and marked latest. Without --upload it prints the `gh release create --draft` command;
with it, it runs that command: the release is created as a draft, and nothing is public until it is published on
GitHub.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import time
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'runtime'))
sys.path.insert(0, str(ROOT / 'tools'))
from os_update import DTBS, PAYLOAD, TARGET, BASE  # noqa: E402
from ota import MAX_EXPANDED, MAX_PACKAGE  # noqa: E402
from publish_update import sign  # noqa: E402
from release import DEFAULT_KEY, TRUST, feed_url, public_key, repo_name  # noqa: E402

BUILD = ROOT / '.cache/own-os/build'
IMAGE = 'rgds-os.img.gz'
PLUS_IMAGE = 'rgds-os-plus.img.gz'


def package(build, out):
    """RG-DS-OS-<build>.zip from the last own_os_build; the squashfs is stored (it is compressed already)."""
    archive = out / f'RG-DS-OS-{build}.zip'
    files = {}
    with zipfile.ZipFile(archive, 'w') as z:
        for name in PAYLOAD:
            source = BUILD / name
            files[name] = hashlib.sha256(source.read_bytes()).hexdigest()
            z.write(
                source, f'{build}/{name}', zipfile.ZIP_STORED if name == 'rootfs.squashfs' else zipfile.ZIP_DEFLATED
            )
        manifest = dict(schema=1, build=build, target=TARGET, dtbs=list(DTBS), files=files)
        z.writestr(f'{build}/manifest.json', json.dumps(manifest, indent=2, sort_keys=True) + '\n')
    with zipfile.ZipFile(archive) as z:
        unpacked = sum(e.file_size for e in z.infolist())
    size = archive.stat().st_size
    if size > MAX_PACKAGE or unpacked > MAX_EXPANDED:
        raise SystemExit(f'{archive.name} is larger than handhelds accept ({size} bytes; limit {MAX_PACKAGE}).')
    return archive, unpacked


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--version', required=True)
    parser.add_argument('--repo', type=repo_name)
    parser.add_argument('--key', type=Path, default=DEFAULT_KEY)
    parser.add_argument('--channel', choices=('stable', 'testing'), default='stable')
    parser.add_argument('--notes', type=Path)
    parser.add_argument('--days', type=int, default=90)
    parser.add_argument(
        '--upload', action='store_true', help='create the draft release instead of printing the command'
    )
    args = parser.parse_args()
    if not re.fullmatch(r'\d+\.\d+\.\d+([-.][0-9A-Za-z.]+)?', args.version) or 'dev' in args.version:
        raise SystemExit(f'Use a release version such as 0.1.0, not {args.version}.')
    missing = [n for n in (*PAYLOAD, 'build-id', IMAGE, PLUS_IMAGE) if not (BUILD / n).is_file()]
    if missing:
        raise SystemExit('Missing in .cache/own-os/build: ' + ', '.join(missing) + '. Run tools/own_os_build.py.')
    build = (BUILD / 'build-id').read_text().strip()
    if not build.startswith(args.version + '-'):
        raise SystemExit(f'The last build is {build}: set VERSION to {args.version} and build the image again.')
    repo = args.repo or (ROOT / 'packaging/release-repo.txt').read_text().strip()
    trust = json.loads(TRUST.read_text())
    if trust.get('feed') != feed_url(repo, args.channel):
        raise SystemExit(
            f'packaging/update-trust.json follows {trust.get("feed")}, not {feed_url(repo, args.channel)}.'
        )
    key = args.key.expanduser()
    key_id = next((k for k, v in trust['keys'].items() if v == public_key(key)), None) if key.is_file() else None
    if not key_id: raise SystemExit(f'{key} is not one of the keys pinned in packaging/update-trust.json.')
    out = ROOT / 'dist/release-os' / args.version
    out.mkdir(parents=True, exist_ok=True)
    archive, unpacked = package(build, out)
    images = []
    for source, name in ((IMAGE, f'RG-DS-OS-{args.version}.img.gz'), (PLUS_IMAGE, f'RG-DS-OS-Plus-{args.version}.img.gz')):
        image = out / name
        shutil.copyfile(BUILD / source, image)
        digest = hashlib.sha256(image.read_bytes()).hexdigest()
        image.with_name(image.name + '.sha256').write_text(f'{digest}  {image.name}\n')
        images += [image, image.with_name(image.name + '.sha256')]
    image = images[0]
    tag = 'v' + args.version
    now = int(time.time())
    metadata = dict(
        schema=1,
        target=TARGET,
        base=BASE,
        minimum_updater=1,
        channel=args.channel,
        version=args.version,
        build=build,
        sequence=now,
        issued=now,
        expires=now + args.days * 86400,
        url=f'https://github.com/{repo}/releases/download/{tag}/{archive.name}',
        size=archive.stat().st_size,
        unpacked_size=unpacked,
        sha256=hashlib.sha256(archive.read_bytes()).hexdigest(),
        notes=args.notes.read_text() if args.notes else f'RG DS OS {args.version}\n'
    )
    feed = out / f'{args.channel}-os.json'
    sign(metadata, key, key_id, feed)
    print(f'{archive.relative_to(ROOT)} ({metadata["size"] / 1048576:.0f} MiB), feed {feed.relative_to(ROOT)}')
    print(f'{image.relative_to(ROOT)} ({image.stat().st_size / 1048576:.0f} MiB), the card image (both handhelds)')
    notes = out / 'notes.md'
    notes.write_text(metadata['notes'])
    files = [*images, archive, feed]
    command = [
        'gh', 'release', 'create', tag, '--repo', repo, '--draft', '--title', f'RG DS OS {args.version}',
        '--notes-file',
        str(notes), *map(str, files)
    ]
    if args.upload:
        subprocess.run(command, check=True)
        print(f'\nDraft {tag} created. Publish it on GitHub (as the latest release) to offer the update.')
    else:
        print('\nCreate the draft release (nothing is public until it is published):\n' + shlex.join(command))


if __name__ == '__main__':
    main()
