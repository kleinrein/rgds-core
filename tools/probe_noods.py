#!/usr/bin/env python3
"""Build a pinned, headless NooDS experiment; never install or deploy it.

The harness makes its own small cartridge and uses a new private directory for
all states/saves. It can also read an explicitly supplied ROM for throughput
experiments. Numbers exclude physical display and audio output costs.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / '.cache/noods-source'
COMMIT = '4cb1439c330248b26cdf5f8dbcfa4e5ac53dcda0'
REPOSITORY = 'https://github.com/Hydr8gon/NooDS.git'


def run(command, **kwargs):
    return subprocess.run([str(part) for part in command], check=True, **kwargs)


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def source(fetch):
    if not SOURCE.exists():
        if not fetch:
            raise SystemExit('Pinned source is absent; pass --fetch to download the public repository.')
        run(['git', 'clone', '--no-checkout', REPOSITORY, SOURCE])
        run(['git', '-C', SOURCE, 'checkout', '--detach', COMMIT])
    actual = subprocess.check_output(['git', '-C', str(SOURCE), 'rev-parse', 'HEAD'], text=True).strip()
    dirty = subprocess.check_output(['git', '-C', str(SOURCE), 'status', '--porcelain'], text=True).strip()
    if actual != COMMIT or dirty:
        raise SystemExit('NooDS source must be the clean pinned commit; refusing to reset existing work.')


def build(target, fetch=False):
    source(fetch)
    output = ROOT / 'build/noods-probe' / target
    output.mkdir(parents=True, exist_ok=True)
    binary = output / 'noods-probe'
    staged = output / 'source'
    if staged.exists():
        shutil.rmtree(staged)
    shutil.copytree(SOURCE / 'src/core', staged / 'src/core')
    patch = ROOT / 'src/experimental/noods/null-file-close.patch'
    run(['patch', '--batch', '--forward', '-d', staged, '-p1', '-i', patch], stdout=subprocess.DEVNULL)
    compiler = ['clang++'] if target == 'host' else ['zig', 'c++', '-target', 'aarch64-linux-gnu.2.17', '-mcpu=cortex_a55']
    if not shutil.which(compiler[0]):
        raise SystemExit(f'{compiler[0]} is required on PATH')
    flags = ['-O3', '-std=c++11', '-pthread', '-DLOG_LEVEL=0']
    if target == 'host' and platform.system() == 'Darwin':
        flags += ['-DMACOS']
    sources = sorted((staged / 'src/core').rglob('*.cpp'))
    harness = ROOT / 'src/experimental/noods/probe.cpp'
    command = compiler + flags + ['-I', staged / 'src/core', harness] + sources + ['-o', binary]
    environment = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(ROOT / '.cache/zig'),
                       ZIG_LOCAL_CACHE_DIR=str(ROOT / '.cache/zig-noods'))
    run(command, env=environment)
    with (output / 'upstream-source.tar').open('wb') as stream:
        run(['git', '-C', SOURCE, 'archive', '--format=tar', COMMIT], stdout=stream)
    shutil.copy2(SOURCE / 'LICENSE', output / 'LICENSE')
    shutil.copy2(harness, output / 'probe.cpp')
    shutil.copy2(patch, output / patch.name)
    shutil.copy2(__file__, output / 'probe_noods.py')
    metadata = {
        'schema': 1, 'core': 'NooDS', 'repository': REPOSITORY, 'commit': COMMIT,
        'license': 'GPL-3.0-or-later', 'target': target,
        'compiler': subprocess.check_output([compiler[0], '--version' if target == 'host' else 'version'], text=True).strip(),
        'flags': [str(value) for value in compiler[1:] + flags],
        'source_archive_sha256': digest(output / 'upstream-source.tar'),
        'binary_sha256': digest(binary), 'probe_sha256': digest(harness),
        'patches': {patch.name: digest(patch)},
        'build_driver_sha256': digest(__file__),
        'limitations': ['headless', 'no physical audio output', 'no launcher integration',
                        'synthetic results are not commercial-game performance measurements'],
    }
    (output / 'build.json').write_text(json.dumps(metadata, indent=2) + '\n')
    return binary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', choices=('host', 'device'), default='host')
    parser.add_argument('--fetch', action='store_true')
    parser.add_argument('--run', action='store_true', help='Run locally (host only), using disposable save directories')
    parser.add_argument('--frames', type=int, default=180)
    parser.add_argument('--threads', type=int, choices=range(5), default=1)
    parser.add_argument('--rom', type=Path, help='Optional lawfully supplied ROM; all saves stay in the new probe directory')
    args = parser.parse_args()
    if not 1 <= args.frames <= 36000:
        parser.error('--frames must be 1–36000')
    if args.run and args.target != 'host':
        parser.error('--run executes host binaries only; device execution is an explicit separate step')
    binary = build(args.target, args.fetch)
    if args.run:
        work = binary.parent / f'run-{time.time_ns()}'
        command = [binary, work, str(args.frames), str(args.threads)]
        if args.rom:
            command.append(args.rom.resolve(strict=True))
        completed = run(command, capture_output=True, text=True, timeout=180)
        result = json.loads(completed.stdout)
        result.update(binary_sha256=digest(binary), commit=COMMIT, host=platform.platform())
        (work / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps(result, indent=2))
    else:
        print(binary)


if __name__ == '__main__':
    main()
