#!/usr/bin/env python3
"""Add the verified SMB service to an existing, fully verified release ZIP.

Keeps its launcher, emulator binaries and their source/build records intact.
The normal package build also requires and includes the same SMB bundle.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import zipfile

try:
    from .build_samba import validate_build
    from .package import component_abi, glibc_floor
except ImportError:
    from build_samba import validate_build
    from package import component_abi, glibc_floor

ROOT = Path(__file__).resolve().parents[1]


def build(base, legacy_details=False):
    subprocess.run([sys.executable, '-B', str(ROOT / 'packaging/install_package.py'),
                    str(ROOT / 'build/service-update-check'), str(base), '--check'], check=True)
    samba = ROOT / 'build/samba'
    record = validate_build(samba)
    stage = ROOT / 'build/service-update-abi'
    stage.mkdir(parents=True, exist_ok=True)
    replacements = {'runtime/file_sharing.py': (ROOT / 'runtime/file_sharing.py').read_bytes(),
                    'THIRD_PARTY.md': (ROOT / 'THIRD_PARTY.md').read_bytes()}
    documents = {'rgds/docs/' + name: (ROOT / name).read_bytes()
                 for name in ('docs/DEVICE_SETUP.md', 'docs/DEVELOPMENT.md', 'THIRD_PARTY.md')}
    for name in (*sorted(record['files']), 'build.json'):
        replacements['services/samba/' + name] = (samba / name).read_bytes()
    with zipfile.ZipFile(base) as source:
        old = source.read('rgds/state/active').decode().strip()
        prefix = 'rgds/releases/' + old + '/'
        manifest = json.loads(source.read(prefix + 'manifest.json'))
        files = dict(manifest['files'])
        files.update({name: hashlib.sha256(content).hexdigest() for name, content in replacements.items()})
        for name, content in replacements.items():
            path = stage / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(content)
        abi = dict(manifest.get('component_abi', {}))
        abi.update(component_abi(stage, {name: files[name] for name in replacements}))
        update = dict(base_build=old, legacy_sharing_details=legacy_details,
                      recipe_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
        identity = hashlib.sha256(json.dumps(dict(files=files, abi=abi, update=update), sort_keys=True).encode()).hexdigest()[:12]
        build_id = manifest['version'] + '-smb-' + identity
        manifest.update(build=build_id, files=files, component_abi=abi,
                        minimum_glibc=glibc_floor(abi), service_update=update)
        manifest.setdefault('source_builds', {})['samba'] = 'services/samba/build.json'
        target = 'rgds/releases/' + build_id + '/'
        output = ROOT / 'dist' / ('RGDS-' + build_id + '.zip')
        pending = output.with_suffix('.zip.pending')
        with zipfile.ZipFile(pending, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as destination:
            for entry in source.infolist():
                if entry.filename.startswith(prefix):
                    name = entry.filename[len(prefix):]
                    if name in replacements or name == 'manifest.json':
                        continue
                    new_entry = zipfile.ZipInfo(target + name, entry.date_time)
                    new_entry.external_attr = entry.external_attr; new_entry.compress_type = zipfile.ZIP_DEFLATED
                    destination.writestr(new_entry, source.read(entry))
                else:
                    content = build_id.encode() + b'\n' if entry.filename == 'rgds/state/active' else documents.get(entry.filename, source.read(entry))
                    destination.writestr(entry, content)
            for name, content in {**replacements, 'manifest.json': (json.dumps(manifest, indent=2, sort_keys=True) + '\n').encode()}.items():
                entry = zipfile.ZipInfo(target + name, (2026, 10, 13, 0, 0, 0))
                entry.compress_type = zipfile.ZIP_DEFLATED
                entry.external_attr = (0o100755 if name.startswith('services/samba/bin/') else 0o100644) << 16
                destination.writestr(entry, content)
    subprocess.run([sys.executable, '-B', str(ROOT / 'packaging/install_package.py'),
                    str(ROOT / 'build/service-update-check'), str(pending), '--check'], check=True)
    pending.replace(output)
    print(output)
    return output


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', type=Path, required=True)
    parser.add_argument('--legacy-details', action='store_true', help='Compatibility for the first sharing UI')
    args = parser.parse_args()
    build(args.base, args.legacy_details)
