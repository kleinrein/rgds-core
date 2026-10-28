#!/usr/bin/env python3
"""The signed update channel on GitHub: the signing key and the pinned feed.

One-time setup, once the repository exists:

    python3 tools/release.py init --repo OWNER/REPO

creates an Ed25519 signing key outside the project (default
~/.config/rgds/release-key.pem; keep it private and backed up) and pins the
update channel in packaging/update-trust.json: the feed
https://github.com/OWNER/REPO/releases/latest/download/stable.json and the
public key. RG DS OS images carry that file (app/os/) and read the OS feed
beside it, stable-os.json. Each release is then published with
tools/own_os_release.py (docs/OTA.md).
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
TRUST = ROOT / 'packaging/update-trust.json'
DEFAULT_KEY = Path.home() / '.config/rgds/release-key.pem'
OPENSSL = next(
    (
        p for p in ('/opt/homebrew/opt/openssl@3/bin/openssl', '/usr/local/opt/openssl@3/bin/openssl')
        if sys.platform == 'darwin' and Path(p).exists()
    ), 'openssl'
)


def repo_name(value):
    owner, _, name = value.partition('/')
    if not owner or not name or '/' in name or any(
        c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.' for c in owner + name
    ):
        raise argparse.ArgumentTypeError('Use OWNER/REPO, as on github.com')
    return value


def feed_url(repo, channel):
    return f'https://github.com/{repo}/releases/latest/download/{channel}.json'


def public_key(key):
    der = subprocess.run(
        [OPENSSL, 'pkey', '-in', str(key), '-pubout', '-outform', 'DER'], check=True, capture_output=True
    ).stdout
    return der[-32:].hex()


def init(args):
    key = args.key.expanduser()
    if not key.exists():
        key.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([OPENSSL, 'genpkey', '-algorithm', 'ED25519', '-out', str(key)], check=True, capture_output=True)
        key.chmod(0o600)
        print(f'Created signing key {key}. Back it up; without it no further update can be signed.')
    trust = dict(
        schema=1, channel=args.channel, feed=feed_url(args.repo, args.channel), keys={args.key_id: public_key(key)}
    )
    TRUST.write_text(json.dumps(trust, indent=2) + '\n')
    (ROOT / 'packaging/release-repo.txt').write_text(args.repo + '\n')
    print(f'Pinned {trust["feed"]} and key {args.key_id} in {TRUST.relative_to(ROOT)}.')
    print('Build the image again (tools/own_os_build.py) so new cards follow this channel.')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    first = sub.add_parser('init', help='create the signing key and pin the GitHub update channel')
    first.add_argument('--repo', type=repo_name, required=True)
    first.add_argument('--key', type=Path, default=DEFAULT_KEY)
    first.add_argument('--key-id', default='release-' + time.strftime('%Y'))
    first.add_argument('--channel', choices=('stable', 'testing'), default='stable')
    args = parser.parse_args()
    init(args)


if __name__ == '__main__':
    main()
