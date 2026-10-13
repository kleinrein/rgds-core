#!/usr/bin/env python3
"""Build the vendored BSD replacement DS BIOS with LLVM; no stock files needed."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / 'assets/nnddss/freebios'
if (Path(__file__).resolve().parent / 'freebios/source.json').is_file():
    ASSETS = Path(__file__).resolve().parent / 'freebios'
COMMIT = '1124603369ce72986b7469bb6fa3520bcb58ab5e'
EDITS = (('.section .text, "axR"', '.section .text, "ax"'),
         ('ldreqb', 'ldrbeq'), ('ldrneb', 'ldrbne'), ('strneh', 'strhne'), ('ldmeqia', 'ldmiaeq'))


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate(directory=ASSETS):
    directory = Path(directory)
    source = json.loads((directory / 'source.json').read_text())
    record = json.loads((directory / 'build.json').read_text())
    if source['commit'] != COMMIT or record['source'] != source or record['driver_sha256'] != sha(Path(__file__)):
        raise ValueError('FreeBIOS build/source record changed; run tools/build_freebios.py')
    for name, expected in source['files'].items():
        if sha(directory / 'source' / name) != expected:
            raise ValueError('FreeBIOS source changed: ' + name)
    if set(record['files']) != {'drastic_bios_arm7.bin', 'drastic_bios_arm9.bin'}:
        raise ValueError('Incomplete FreeBIOS build')
    for name, expected in record['files'].items():
        if sha(directory / name) != expected:
            raise ValueError('FreeBIOS output changed: ' + name)
    return record


def build():
    source = json.loads((ASSETS / 'source.json').read_text())
    if source['commit'] != COMMIT:
        raise ValueError('Unexpected FreeBIOS source revision')
    for name, expected in source['files'].items():
        if sha(ASSETS / 'source' / name) != expected:
            raise ValueError('FreeBIOS source changed: ' + name)
    directory = os.environ.get('LLVM_BIN', '/opt/homebrew/opt/llvm/bin')
    binaries = {name: shutil.which(name) or str(Path(directory) / name)
                for name in ('clang', 'ld.lld', 'llvm-objcopy')}
    # Apple clang cannot assemble this ELF source. Prefer one coherent LLVM set.
    if (Path(directory) / 'clang').is_file():
        binaries = {name: str(Path(directory) / name) for name in binaries}
    assembly = (ASSETS / 'source/src/bios_common.s').read_text()
    for original, replacement in EDITS:
        assembly = assembly.replace(original, replacement)
    record = dict(schema=1, source=source, driver_sha256=sha(Path(__file__)),
                  assembler_edits=list(EDITS), files={}, commands={},
                  compiler=subprocess.check_output([binaries['clang'], '--version'], text=True).splitlines()[0])
    with tempfile.TemporaryDirectory(prefix='rgds-freebios-') as temporary:
        work = Path(temporary)
        (work / 'bios.s').write_text(assembly)
        for arm, cpu, size in ((7, 'arm7tdmi', 0x4000), (9, 'arm946e-s', 0x1000)):
            commands = [
                [binaries['clang'], '-target', 'arm-none-eabi', '-mcpu=' + cpu,
                 '-x', 'assembler-with-cpp', '-DPLATFORM_NTR', '-DTARGET_ARM' + str(arm),
                 '-c', 'bios.s', '-o', 'bios.o'],
                [binaries['ld.lld'], '-Ttext=0', '-e', '0', 'bios.o', '-o', 'bios.elf'],
                [binaries['llvm-objcopy'], '-O', 'binary', 'bios.elf', 'bios.bin']]
            for command in commands:
                subprocess.run(command, cwd=work, check=True)
            data = (work / 'bios.bin').read_bytes()
            if not 32 < len(data) <= size:
                raise ValueError('Unexpected replacement BIOS size')
            name = 'drastic_bios_arm' + str(arm) + '.bin'
            # Full address-space images; no Nintendo logo or encryption tables.
            (work / name).write_bytes(data.ljust(size, b'\0'))
            record['files'][name] = sha(work / name)
            record['commands'][name] = [[Path(c[0]).name, *c[1:]] for c in commands]
        for name in record['files']:
            shutil.copyfile(work / name, ASSETS / name)
    (ASSETS / 'build.json').write_text(json.dumps(record, indent=2, sort_keys=True) + '\n')
    validate()
    print('Built and verified replacement ARM7/ARM9 BIOS without stock files.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    if parser.parse_args().check:
        validate()
        print('FreeBIOS source and outputs verified.')
    else:
        build()
