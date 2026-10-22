#!/usr/bin/env python3
"""Build an original DS triangle workload with an emulated frame clock."""
import argparse
import os
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def build(triangles):
    output = ROOT / 'build/graphics-probe' / str(triangles)
    output.mkdir(parents=True, exist_ok=True)
    llvm = Path(os.environ.get('LLVM_BIN', '/opt/homebrew/opt/llvm/bin'))
    start = output / 'start.s'
    start.write_text(
        '.syntax unified\n.arm\n.section .text.start\n.global _start\n_start:\n ldr sp, =0x023ff000\n bl main\n b .\n'
    )
    linker = output / 'arm9.ld'
    linker.write_text(
        'ENTRY(_start)\nSECTIONS { . = 0x02000000; .text : { *(.text.start) *(.text*) } .rodata : { *(.rodata*) } .data : { *(.data*) } .bss : { *(.bss*) } }\n'
    )
    flags = ['-target', 'arm-none-eabi', '-mcpu=arm946e-s', '-marm', '-O2', '-ffreestanding', '-fno-builtin']
    subprocess.run([llvm / 'clang', *flags, '-c', start, '-o', output / 'start.o'], check=True)
    subprocess.run(
        [
            llvm / 'clang', *flags, '-DTRIANGLES=' + str(triangles), '-c',
            ROOT / 'tests/fixtures/graphics_probe/arm9.c', '-o', output / 'main.o'
        ],
        check=True
    )
    subprocess.run(
        [llvm / 'ld.lld', '-T', linker, output / 'start.o', output / 'main.o', '-o', output / 'arm9.elf'], check=True
    )
    subprocess.run([llvm / 'llvm-objcopy', '-O', 'binary', output / 'arm9.elf', output / 'arm9.bin'], check=True)
    subprocess.run(
        [
            llvm / 'clang', '-target', 'arm-none-eabi', '-mcpu=arm7tdmi', '-c',
            ROOT / 'tests/fixtures/nnddss_probe/arm7.s', '-o', output / 'arm7.o'
        ],
        check=True
    )
    subprocess.run(
        [llvm / 'ld.lld', '-Ttext=0x03800000', '-e', '_start', output / 'arm7.o', '-o', output / 'arm7.elf'],
        check=True
    )
    subprocess.run([llvm / 'llvm-objcopy', '-O', 'binary', output / 'arm7.elf', output / 'arm7.bin'], check=True)
    image = bytearray(0x20000)
    image[:12] = b'RGDS 3D TEST'
    image[12:16] = b'####'
    image[16:18] = b'00'
    for arm, offset, address, header in ((9, 0x200, 0x02000000, 0x20), (7, 0x8000, 0x03800000, 0x30)):
        code = (output / ('arm' + str(arm) + '.bin')).read_bytes()
        assert len(code) < 0x7e00
        struct.pack_into('<4I', image, header, offset, address, address, len(code))
        image[offset:offset + len(code)] = code
    struct.pack_into('<2I', image, 0x80, len(image), 0x200)

    def crc(data):
        value = 0xffff
        for byte in data:
            value ^= byte
            for _ in range(8):
                value = (value >> 1) ^ (0xa001 if value & 1 else 0)
        return value

    struct.pack_into('<H', image, 0x15c, crc(image[0xc0:0x15c]))
    struct.pack_into('<H', image, 0x15e, crc(image[:0x15e]))
    result = output / ('RGDS-3D-' + str(triangles) + '.nds')
    result.write_bytes(image)
    print(result)


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--triangles', type=int, choices=(64, 128, 512, 1024, 1536), default=128)
    build(p.parse_args().triangles)
