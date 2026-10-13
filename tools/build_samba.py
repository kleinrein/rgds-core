#!/usr/bin/env python3
"""Build a relocatable ARM64 SMB service in the existing local Linux builder."""
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
import tarfile
import urllib.request

try:
    from .elf_abi import inspect_elf
except ImportError:
    from elf_abi import inspect_elf

ROOT = Path(__file__).resolve().parents[1]
VERSION = '4.25.0'
URL = 'https://codeload.github.com/samba-team/samba/tar.gz/refs/tags/samba-' + VERSION
SOURCE_SHA256 = 'd524c6ebfaf59a11d160f1d374bed687b3643d1e3c7005f99a4119c870233d67'
ARCHIVE = ROOT / '.cache/samba' / ('samba-' + VERSION + '-git.tar.gz')
OUTPUT = ROOT / 'build/samba'
TARGETS = ('smbd/smbd', 'smbpasswd', 'client/smbclient', 'samba-dcerpcd', 'rpcd_classic', 'rpcd_winreg', 'rpcd_lsad')
OPTIONS = ['--prefix=/opt/rgds-samba', '--without-ad-dc', '--without-ads',
           '--without-ldap', '--without-winbind', '--disable-python', '--without-pam', '--without-ldb-lmdb',
           '--disable-cups', '--disable-iprint', '--disable-avahi', '--without-systemd',
           '--without-systemd-userdb', '--without-libarchive', '--without-regedit',
           '--without-quotas', '--without-utmp', '--without-automount', '--without-fam',
           '--disable-cephfs', '--disable-cephrgw', '--disable-glusterfs',
           '--disable-spotlight', '--disable-wsp', '--without-json', '--without-gpgme',
           '--without-smb1-server', '--without-acl-support', '--without-cluster-support',
           '--without-lttng', '--without-libunwind', '--without-prometheus-exporter',
           '--nonshared-binary=smbd/smbd',
           '--with-shared-modules=!DEFAULT,!FORCED',
           '--disable-rpath-install', '--disable-rpath-private-install']
# Only this private bundle recognizes these environment overrides. Normal Samba
# defaults stay intact; RGDS passes absolute paths within its verified release.
PATCH_FROM = 'const char *get_dyn_##name(void) \\\n{\\\n\treturn dyn_##name;\\\n}'
PATCH_TO = 'const char *get_dyn_##name(void) \\\n{\\\n\tconst char *rgds_path = getenv("RGDS_SAMBA_" #name);\\\n\treturn (rgds_path != NULL && rgds_path[0] == \'/\') ? rgds_path : dyn_##name;\\\n}'
SYSTEM_LIBRARIES = {'libc.so.6', 'libm.so.6', 'libpthread.so.0', 'libdl.so.2',
                    'librt.so.1', 'ld-linux-aarch64.so.1', 'libresolv.so.2'}


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def validate_build(output=OUTPUT):
    output = Path(output)
    record = json.loads((output / 'build.json').read_text())
    if record.get('schema') != 1 or record.get('source') != dict(url=URL, sha256=SOURCE_SHA256, version=VERSION):
        raise ValueError('Wrong bundled Samba source record; run make build-samba.')
    if record.get('driver_sha256') != sha(__file__) or record.get('options') != OPTIONS:
        raise ValueError('Samba build recipe changed; run make build-samba.')
    files = record.get('files', {})
    required = {'bin/' + Path(name).name for name in TARGETS} | {'COPYING', 'source.tar.gz', 'NOTICE.txt', 'rgds-build/build_samba.py'}
    if not required <= files.keys():
        raise ValueError('Incomplete bundled Samba service.')
    for name, digest in files.items():
        path = Path(name)
        if path.is_absolute() or '..' in path.parts or (output / path).is_symlink() or sha(output / path) != digest:
            raise ValueError('Bundled Samba payload changed: ' + name)
    for name, abi in record.get('abi', {}).items():
        current = inspect_elf(output / name, 'Pinned Samba SMB service and private libraries')
        if current != abi or current['machine'] != 'aarch64':
            raise ValueError('Unsupported Samba payload ABI: ' + name)
        if current['glibc_minimum'] and tuple(map(int, current['glibc_minimum'].split('.'))) > (2, 39):
            raise ValueError('Samba exceeds the GLIBC 2.39 baseline: ' + name)
        missing = set(current['needed']) - SYSTEM_LIBRARIES - {Path(p).name for p in files if p.startswith('lib/')}
        if missing:
            raise ValueError('Missing private Samba dependencies: ' + ', '.join(sorted(missing)))
    if not {'bin/' + Path(t).name for t in TARGETS} <= record.get('abi', {}).keys():
        raise ValueError('Samba executable ABI records are missing.')
    with tarfile.open(output / 'source.tar.gz') as source:
        if hashlib.sha256(source.extractfile('rgds/build_samba.py').read()).hexdigest() != record['driver_sha256']:
            raise ValueError('Samba corresponding build source differs.')
        if PATCH_TO not in source.extractfile('samba/dynconfig/dynconfig.c').read().decode():
            raise ValueError('Samba relocation patch is missing from corresponding source.')
    return record


def dependencies(binary):
    result = subprocess.check_output(['ldd', str(binary)], text=True)
    if 'not found' in result:
        raise ValueError('An SMB build dependency is missing: ' + result)
    return {name: Path(path) for name, path in re.findall(r'^\s*(\S+) => (/\S+) ', result, re.MULTILINE)
            if name not in SYSTEM_LIBRARIES}


def build(jobs):
    try:
        validate_build(); print('Bundled Samba build is current.', flush=True); return
    except (OSError, ValueError, KeyError, TypeError):
        pass
    if sha(ARCHIVE) != SOURCE_SHA256:
        raise ValueError('Pinned Samba source archive checksum mismatch.')
    work = ROOT / 'build/samba-work'
    if work.is_symlink():
        raise ValueError('Samba staging cannot be a symlink.')
    work.mkdir(parents=True, exist_ok=True)
    source = work / ('samba-samba-' + VERSION)
    if not source.exists():
        with tarfile.open(ARCHIVE) as archive:
            archive.extractall(work, filter='data')
    config = source / 'dynconfig/dynconfig.c'
    original = config.read_text()
    if original.count(PATCH_FROM) != 1 and original.count(PATCH_TO) != 1:
        raise ValueError('Pinned Samba relocation patch does not match.')
    config.write_text(original.replace(PATCH_FROM, PATCH_TO))
    env = dict(os.environ, CFLAGS='-O2 -mtune=cortex-a55', PYTHONDONTWRITEBYTECODE='1', PYTHONHASHSEED='1')
    settings = json.dumps(dict(options=OPTIONS, flags=env['CFLAGS'], patch=PATCH_TO), sort_keys=True)
    configured = work / 'configured.json'
    if not configured.exists() or configured.read_text() != settings:
        with (work / 'configure.log').open('w') as log:
            subprocess.run(['./configure', *OPTIONS], cwd=source, env=env, check=True, stdout=log, stderr=log)
        configured.write_text(settings)
    print('Samba configured. Building SMB server, authentication, RPC and client.', flush=True)
    with (work / 'compile.log').open('w') as log:
        subprocess.run(['./buildtools/bin/waf', 'build', '-j' + str(jobs), '--targets=' + ','.join(TARGETS)],
                       cwd=source, env=env, check=True, stdout=log, stderr=log)
    candidate = work / 'bundle'
    if candidate.exists():
        shutil.rmtree(candidate)
    (candidate / 'bin').mkdir(parents=True)
    libraries = {}
    for name in TARGETS:
        binary = source / 'bin/default/source3' / name
        if not binary.is_file():
            binary = source / 'bin/default/source3/rpc_server' / Path(name).name
        if not binary.is_file():
            binary = source / 'bin/default/source3/utils' / Path(name).name
        out = candidate / 'bin' / Path(name).name
        shutil.copy2(binary, out)
        subprocess.run(['strip', '--strip-unneeded', str(out)], check=True)
        subprocess.run(['patchelf', '--remove-rpath', str(out)], check=True)
        libraries.update(dependencies(binary))
    licenses = candidate / 'licenses'; licenses.mkdir()
    packages = {}
    (candidate / 'lib').mkdir()
    for name, path in sorted(libraries.items()):
        shutil.copy2(path.resolve(), candidate / 'lib' / name)
        subprocess.run(['patchelf', '--remove-rpath', str(candidate / 'lib' / name)], check=True)
        if path.resolve().is_relative_to(source):
            continue  # Included in Samba's full corresponding source below.
        resolved = str(path.resolve())
        found = subprocess.run(['dpkg-query', '-S', resolved], capture_output=True, text=True)
        if found.returncode and resolved.startswith('/usr/lib/'):
            found = subprocess.run(['dpkg-query', '-S', resolved[4:]], capture_output=True, text=True)
        if found.returncode:
            raise ValueError('Cannot identify private dependency package: ' + resolved)
        owner = found.stdout.split(': ')[0]
        metadata = subprocess.check_output(['dpkg-query', '-W', '-f=${Package}\t${Version}\t${source:Package}\t${source:Version}', owner], text=True)
        package = metadata.split('\t')[0]
        packages[package] = metadata
        copyright = Path('/usr/share/doc') / package / 'copyright'
        if copyright.is_file():
            shutil.copy2(copyright, licenses / (package + '-copyright'))
    shutil.copy2(source / 'COPYING', candidate / 'COPYING')
    (candidate / 'rgds-build').mkdir()
    shutil.copy2(__file__, candidate / 'rgds-build/build_samba.py')
    with tarfile.open(candidate / 'source.tar.gz', 'w:gz') as archive:
        for path in sorted(source.rglob('*')):
            if path.is_file() and path.relative_to(source).parts[0] != 'bin' and '__pycache__' not in path.parts:
                archive.add(path, arcname='samba/' + path.relative_to(source).as_posix(), recursive=False)
        archive.add(__file__, arcname='rgds/build_samba.py')
    (candidate / 'NOTICE.txt').write_text('Samba ' + VERSION + ' (GPL-3.0-or-later), built from pinned upstream source.\n'
        'Full corresponding Samba source and relocation patch: source.tar.gz; recipe: rgds-build/build_samba.py.\n'
        'External library copyright notices: licenses/. Ubuntu ARM64 package/source versions:\n' + '\n'.join(packages.values()) + '\n')
    files = {p.relative_to(candidate).as_posix(): sha(p) for p in candidate.rglob('*') if p.is_file()}
    record = dict(schema=1, source=dict(url=URL, sha256=SOURCE_SHA256, version=VERSION), options=OPTIONS,
                  driver_sha256=sha(__file__), compiler=subprocess.check_output(['gcc', '--version'], text=True).splitlines()[0],
                  dependency_packages=packages, files=files,
                  abi={name: inspect_elf(candidate / name, 'Pinned Samba SMB service and private libraries')
                       for name in files if name.startswith(('bin/', 'lib/'))})
    (candidate / 'build.json').write_text(json.dumps(record, indent=2, sort_keys=True) + '\n')
    validate_build(candidate)
    if OUTPUT.exists():
        shutil.rmtree(OUTPUT)
    candidate.rename(OUTPUT)
    print('Bundled Samba ARM64 service verified: ' + str(OUTPUT), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fetch', action='store_true'); parser.add_argument('--check', action='store_true')
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    if args.check:
        validate_build(); return
    if not ARCHIVE.is_file() and args.fetch:
        ARCHIVE.parent.mkdir(parents=True, exist_ok=True)
        pending = ARCHIVE.with_suffix('.pending')
        urllib.request.urlretrieve(URL, pending)
        if sha(pending) != SOURCE_SHA256:
            pending.unlink(); raise ValueError('Pinned Samba download checksum mismatch.')
        pending.replace(ARCHIVE)
    if platform.system() == 'Darwin':
        env = dict(os.environ, LIMA_HOME=str(ROOT / '.cache/linux-builder/state'))
        subprocess.run([str(ROOT / '.cache/linux-builder/lima/bin/limactl'), 'shell', 'rgds-image',
                        'python3', '-B', str(Path(__file__).resolve()), '--jobs', str(args.jobs)], env=env, check=True)
        validate_build()
    elif platform.system() == 'Linux' and platform.machine() in ('aarch64', 'arm64'):
        build(args.jobs)
    else:
        raise ValueError('Build Samba in the existing ARM64 Linux builder.')


if __name__ == '__main__':
    main()
