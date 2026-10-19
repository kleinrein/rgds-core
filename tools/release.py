#!/usr/bin/env python3
"""Publish RG DS Core releases on GitHub, with signed automatic updates.

One-time setup, once the repository exists:

    python3 tools/release.py init --repo OWNER/REPO

creates an Ed25519 signing key outside the project (default
~/.config/rgds/release-key.pem; keep it private and backed up) and pins the
update channel in packaging/update-trust.json: the feed
https://github.com/OWNER/REPO/releases/latest/download/stable.json and the
public key. Rebuild the installer afterwards; installs made from it check that
feed. Devices installed earlier need the new trust file (see docs/OTA.md).

Each release, after `make installer`:

    python3 tools/release.py publish --version 0.1.0 --notes notes.txt

signs an update for the latest package (dist/latest.txt) and writes
dist/release/<version>/ with the update archive, the signed feed
(stable.json) and the installer. Without --upload it uploads nothing and
prints the `gh release create` command; with --upload it runs it (needs
`gh auth login`). The feed's sequence is the publish time, so every release
supersedes the last.
"""
import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
import zipfile

ROOT = Path(__file__).resolve().parents[1]
TRUST = ROOT / 'packaging/update-trust.json'
DEFAULT_KEY = Path.home() / '.config/rgds/release-key.pem'
OPENSSL = next((p for p in ('/opt/homebrew/opt/openssl@3/bin/openssl', '/usr/local/opt/openssl@3/bin/openssl')
                if sys.platform == 'darwin' and Path(p).exists()), 'openssl')


def repo_name(value):
    owner, _, name = value.partition('/')
    if not owner or not name or '/' in name or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.' for c in owner + name):
        raise argparse.ArgumentTypeError('Use OWNER/REPO, as on github.com')
    return value


def feed_url(repo, channel):
    return f'https://github.com/{repo}/releases/latest/download/{channel}.json'


def public_key(key):
    der = subprocess.run([OPENSSL, 'pkey', '-in', str(key), '-pubout', '-outform', 'DER'], check=True, capture_output=True).stdout
    return der[-32:].hex()


def init(args):
    key = args.key.expanduser()
    if not key.exists():
        key.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([OPENSSL, 'genpkey', '-algorithm', 'ED25519', '-out', str(key)], check=True, capture_output=True)
        key.chmod(0o600)
        print(f'Created signing key {key}. Back it up; without it no further update can be signed.')
    trust = dict(schema=1, channel=args.channel, feed=feed_url(args.repo, args.channel), keys={args.key_id: public_key(key)})
    TRUST.write_text(json.dumps(trust, indent=2) + '\n')
    (ROOT / 'packaging/release-repo.txt').write_text(args.repo + '\n')
    print(f'Pinned {trust["feed"]} and key {args.key_id} in {TRUST.relative_to(ROOT)}.')
    print('Rebuild the installer (make installer) so new installs follow this channel.')


def packaged_trust(package):
    with zipfile.ZipFile(package) as archive:
        name = next((n for n in archive.namelist() if n.endswith('rgds/state/update-trust.json')), None)
        return json.loads(archive.read(name)) if name else None


def publish(args):
    if not re.fullmatch(r'\d+\.\d+\.\d+([-.][0-9A-Za-z.]+)?', args.version) or 'dev' in args.version:
        raise SystemExit(f'Use a release version such as 0.1.0 or 0.1.0-beta.1, not {args.version}.')
    repo = args.repo or (ROOT / 'packaging/release-repo.txt').read_text().strip()
    trust = json.loads(TRUST.read_text())
    if not trust.get('feed') or not trust.get('keys'):
        raise SystemExit('Run `tools/release.py init --repo OWNER/REPO` first.')
    if trust['feed'] != feed_url(repo, args.channel):
        raise SystemExit(f'packaging/update-trust.json follows {trust["feed"]}, not {feed_url(repo, args.channel)}.')
    key = args.key.expanduser()
    if not key.is_file():
        raise SystemExit(f'No signing key at {key}; pass --key with the key `init` created.')
    key_id = next((k for k, v in trust['keys'].items() if v == public_key(key)), None)
    if not key_id:
        raise SystemExit(f'{key} is not one of the keys pinned in packaging/update-trust.json.')
    package = ROOT / 'dist' / (ROOT / 'dist/latest.txt').read_text().strip()
    build = package.stem.split('-')[-1]
    # The package carries VERSION; a feed for another version would show testers the wrong number.
    packaged = package.stem.removeprefix('RGDS-').rsplit('-', 1)[0]
    if packaged != args.version:
        raise SystemExit(f'{package.name} is version {packaged}: set VERSION to {args.version} and run `make installer` again.')
    # An installer built before `init` ships a blank channel: its installs
    # would never see this release or any later one.
    if packaged_trust(package) != trust:
        raise SystemExit(f'{package.name} was built before this channel was pinned; run `make installer` again.')
    tag = 'v' + args.version
    out = ROOT / 'dist/release' / args.version
    out.mkdir(parents=True, exist_ok=True)
    archive = out / f'RG-DS-Core-{build}.ota.zip'
    notes = args.notes or out / 'notes.txt'
    if not args.notes:
        notes.write_text(f'RG DS Core {args.version}\n')
    subprocess.run([sys.executable, str(ROOT / 'tools/publish_update.py'), '--package', str(package), '--output', str(archive),
                    '--url', f'https://github.com/{repo}/releases/download/{tag}/{archive.name}',
                    '--key', str(key), '--key-id', key_id, '--sequence', str(int(time.time())),
                    '--version', args.version, '--channel', args.channel, '--days', str(args.days),
                    '--notes', str(notes), '--feed', str(out / f'{args.channel}.json')], check=True)
    installers = sorted((ROOT / 'dist').glob(f'RG-DS-Core-Installer-*{build}.zip'))
    assets = [archive, out / f'{args.channel}.json']
    if installers:
        shutil.copy2(installers[-1], out / installers[-1].name)
        assets.append(out / installers[-1].name)
    else:
        print('No installer for this build in dist/; run `make installer` to attach one.')
    if args.upload:
        command = ['gh', 'release', 'create', tag, '--repo', repo, '--title', f'RG DS Core {args.version}',
                   '--notes-file', str(notes), *map(str, assets)]
        if args.channel == 'testing':
            command.append('--prerelease')
        subprocess.run(command, check=True)
        return
    files = ' '.join(f'"{p.relative_to(ROOT)}"' for p in assets)
    print(f'\nPublish (uploads the archive, the signed feed and the installer):\n'
          f'gh release create {tag} --repo {repo} --title "RG DS Core {args.version}" --notes-file "{notes}" {files}'
          + (' --prerelease' if args.channel == 'testing' else ''))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    first = sub.add_parser('init', help='create the signing key and pin the GitHub update channel')
    first.add_argument('--repo', type=repo_name, required=True)
    first.add_argument('--key', type=Path, default=DEFAULT_KEY)
    first.add_argument('--key-id', default='release-' + time.strftime('%Y'))
    first.add_argument('--channel', choices=('stable', 'testing'), default='stable')
    release = sub.add_parser('publish', help='sign an update for the latest package and print the upload command')
    release.add_argument('--version', required=True)
    release.add_argument('--repo', type=repo_name)
    release.add_argument('--key', type=Path, default=DEFAULT_KEY)
    release.add_argument('--channel', choices=('stable', 'testing'), default='stable')
    release.add_argument('--notes', type=Path)
    release.add_argument('--days', type=int, default=90)
    release.add_argument('--upload', action='store_true', help='run `gh release create` instead of printing it')
    args = parser.parse_args()
    (init if args.command == 'init' else publish)(args)


if __name__ == '__main__':
    main()
