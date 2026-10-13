#!/bin/sh
# Deploy the verified settings/control rebuild. Does not launch or test RGDS.
set -eu

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo "Usage: $0 root@DEVICE_IP [INSTALL_PARENT]" >&2
    echo "Default installation: /userdata/rgds-device-test/rgds" >&2
    exit 2
fi

RGDS_REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
RGDS_TARGET=$1
RGDS_INSTALL_PARENT=${2:-/userdata/rgds-device-test}

python3 - "$RGDS_REPO" "$RGDS_TARGET" "$RGDS_INSTALL_PARENT" <<'PY'
import hashlib
from pathlib import Path
import shlex
import sys

repo = Path(sys.argv[1])
sys.path.insert(0, str(repo / 'tools'))
import device

destination = device.target(sys.argv[2])
parent = Path(sys.argv[3])
if not parent.is_absolute() or '..' in parent.parts:
    raise SystemExit('Installation parent must be an absolute path without ..')

archive = repo / 'dist/RGDS-0.0.1-dev-c3383ce53ec4.zip'
expected = 'bd3f46ab55432d18e20a91467507f8751ff2154989a915a1efe5ee3c06765af0'
if not archive.is_file():
    raise SystemExit('Missing verified package: ' + str(archive))
if hashlib.sha256(archive.read_bytes()).hexdigest() != expected:
    raise SystemExit('Package checksum mismatch; nothing uploaded.')

root = parent / 'rgds'
print('Deploying verified settings rebuild to ' + str(root), flush=True)
device.deploy(destination, root, archive)
print('Installed. RGDS was not launched or tested.')
print('When ready to launch:')
print(shlex.join(['python3', 'tools/device.py', 'run', '--device', destination, '--sd-root', str(parent)]))
print('To select the previous release:')
print(shlex.join(['python3', 'tools/device.py', 'rollback', '--device', destination, '--sd-root', str(parent)]))
PY
