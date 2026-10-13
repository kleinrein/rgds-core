#!/usr/bin/env python3
"""Generate NNDDSS save-chip metadata from pinned GPL-3.0-or-later melonDS data."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil

ROOT = Path(__file__).resolve().parents[1]
COMMIT = '906e9ebb27da8c6a715cd7abab4abfe8a8d29427'
INPUTS = {
    'src/ROMList.cpp': '181926d537700c58c899a0a931c478db012f31e111132138ce863a0ebdaa0f38',
    'src/NDSCart/CartRetail.cpp': 'bf6f8059e9c5e89ba5b0af8629a796ad23277920cf56c2a1b0aa386d3ec8b8df',
    'LICENSE': '8ceb4b9ee5adedde47b31e975c1d90c73ad27b6b165a1dcd80c7c545eb65b903',
}
OUTPUT = ROOT / 'assets/nnddss/save-types'


def digest(data):
    return hashlib.sha256(data).hexdigest()


def converted(source):
    entries = re.findall(r'\{0x([0-9A-Fa-f]{8}),\s*0x([0-9A-Fa-f]{8}),\s*0x([0-9A-Fa-f]{8})\}', source.decode())
    if len(entries) != 6778 or len({code for code, _, _ in entries}) != len(entries):
        raise ValueError('Pinned melonDS ROM table is incomplete or has duplicate codes')
    table = {code.lower(): [int(size, 16), int(kind, 16)] for code, size, kind in entries}
    return (json.dumps(dict(schema=1, source_commit=COMMIT, entries=table), sort_keys=True, separators=(',', ':')) + '\n').encode()


def validate(output=OUTPUT):
    output = Path(output)
    for name, expected in INPUTS.items():
        path = output / 'source' / name
        if path.is_symlink() or digest(path.read_bytes()) != expected:
            raise ValueError('NNDDSS save-type source changed: ' + name)
    table = output / 'types.json'
    if table.is_symlink() or table.read_bytes() != converted((output / 'source/src/ROMList.cpp').read_bytes()):
        raise ValueError('Generated NNDDSS save types differ from pinned source')
    if (output / 'build.py').read_bytes() != Path(__file__).read_bytes():
        raise ValueError('NNDDSS save-type build driver changed; regenerate its assets')
    return dict(source_commit=COMMIT, entries=6778, table_sha256=digest(table.read_bytes()))


def build(source, output=OUTPUT):
    source, output = Path(source), Path(output)
    for name, expected in INPUTS.items():
        if digest((source / name).read_bytes()) != expected:
            raise ValueError('Unexpected melonDS source bytes: ' + name)
    for name in INPUTS:
        target = output / 'source' / name
        target.parent.mkdir(parents=True, exist_ok=True)
        if (source / name).resolve() != target.resolve():
            shutil.copyfile(source / name, target)
    (output / 'types.json').write_bytes(converted((source / 'src/ROMList.cpp').read_bytes()))
    if Path(__file__).resolve() != (output / 'build.py').resolve():
        shutil.copyfile(__file__, output / 'build.py')
    (output / 'NOTICE.txt').write_text(
        'Save-chip types derived from melonDS src/ROMList.cpp and NDSCart/CartRetail.cpp.\n'
        'Copyright 2016-2026 melonDS team; GPL-3.0-or-later.\n'
        'Source: https://github.com/melonDS-emu/melonDS/tree/' + COMMIT + '\n'
        'Pinned corresponding source and license are included in source/.\n'
        'RGDS converts game-code, ROM-size and save-type facts to JSON and generates\n'
        'a per-ROM compatibility XML at launch. No Nintendo firmware or vendor\n'
        'game database is copied. NAND and unknown chips are not guessed.\n')
    return validate(output)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / '.cache/melonds-source')
    parser.add_argument('--output', type=Path, default=OUTPUT)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    print(json.dumps(validate(args.output) if args.check else build(args.source, args.output), sort_keys=True))
