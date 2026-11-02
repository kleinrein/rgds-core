#!/usr/bin/env python3
"""Build RG DS Impeccable, the DS benchmark ROM (bench/ds, docs/IMPECCABLE.md), with the Homebrew LLVM cross compiler,
as the raster-rules and graphics-probe ROMs are: -> build/impeccable/Impeccable DS.nds. Checks the scoring rules
natively and writes the CPU kernels' right answers for the ROM to hold the emulated ones against.
"""
import os
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / 'bench/ds'
OUT = ROOT / 'build/impeccable'
LLVM = Path(os.environ.get('LLVM_BIN', '/opt/homebrew/opt/llvm/bin'))
TITLE, CODE = b'RGDS IMPECC ', b'RGIM'
ARM9_AT, ARM9_OFFSET, ARM7_AT = 0x02000000, 0x8000, 0x03800000


def run(*command):
    subprocess.run([str(part) for part in command], check=True)


def native():
    run('clang', '-std=c11', '-O1', '-Wall', SRC / 'score_test.c', '-o', OUT / 'score_test')
    run(OUT / 'score_test')
    run('clang', '-std=c11', '-O1', '-Wall', SRC / 'golden_gen.c', SRC / 'kernels.c', '-o', OUT / 'golden_gen')
    (OUT / 'golden.h').write_text(subprocess.run([OUT / 'golden_gen'], check=True, capture_output=True, text=True).stdout)


def cross(flags, source, target, *extra):
    run(LLVM / 'clang', *flags, *extra, f'-I{OUT}', f'-I{SRC}', '-c', source, '-o', OUT / target)


def link(script, objects, output):
    (OUT / 'link.ld').write_text(script)
    run(LLVM / 'ld.lld', '-T', OUT / 'link.ld', *[OUT / o for o in objects], '-o', OUT / f'{output}.elf')
    run(LLVM / 'llvm-objcopy', '-O', 'binary', OUT / f'{output}.elf', OUT / f'{output}.bin')
    return (OUT / f'{output}.bin').read_bytes()


def script(address):
    return (
        f'ENTRY(_start)\nSECTIONS {{ . = {address:#x}; .text : {{ *(.text.start) *(.text*) }} .rodata : {{ *(.rodata*) }}'
        ' .data : { *(.data*) } . = ALIGN(4); __bss_start = .; .bss : { *(.bss*) *(COMMON) } . = ALIGN(4); __bss_end = .;'
        ' /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) *(.comment) } }\n'
    )


def crc16(data):
    value = 0xffff
    for byte in data:
        value ^= byte
        for _ in range(8):
            value = (value >> 1) ^ (0xa001 if value & 1 else 0)
    return value


def build():
    OUT.mkdir(parents=True, exist_ok=True)
    native()
    # -O2 with armv5te, as commercial DS code is built: LDRD, CLZ, the DSP multiplies, Thumb interworking.
    arm9 = ['-target', 'arm-none-eabi', '-mcpu=arm946e-s', '-mfloat-abi=soft', '-O2', '-std=gnu11', '-ffreestanding', '-Wall',
            '-Wextra', '-Wno-unused-parameter', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '-fno-exceptions']
    run(LLVM / 'clang', '-target', 'arm-none-eabi', '-mcpu=arm946e-s', '-c', SRC / 'start9.s', '-o', OUT / 'start9.o')
    cross(arm9 + ['-marm'], SRC / 'main.c', 'main.o')
    cross(arm9 + ['-marm'], SRC / 'scenes.c', 'scenes.o')
    cross(arm9 + ['-marm'], SRC / 'kernels.c', 'kernels_arm.o')
    cross(arm9 + ['-mthumb'], SRC / 'kernels.c', 'kernels_thumb.o', '-DKERNEL_NAME=kernel_thumb')
    code9 = link(script(ARM9_AT), ['start9.o', 'main.o', 'scenes.o', 'kernels_arm.o', 'kernels_thumb.o'], 'arm9')
    arm7 = ['-target', 'arm-none-eabi', '-mcpu=arm7tdmi', '-mfloat-abi=soft', '-marm', '-O2', '-std=gnu11', '-ffreestanding',
            '-Wall', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '-fno-exceptions']
    run(LLVM / 'clang', '-target', 'arm-none-eabi', '-mcpu=arm7tdmi', '-c', SRC / 'start7.s', '-o', OUT / 'start7.o')
    cross(arm7, SRC / 'arm7.c', 'arm7.o')
    code7 = link(script(ARM7_AT), ['start7.o', 'arm7.o'], 'arm7')

    arm7_offset = (ARM9_OFFSET + len(code9) + 0xfff) & ~0xfff
    image = bytearray(arm7_offset + len(code7))
    image[:12] = TITLE
    image[12:16] = CODE
    image[16:18] = b'00'
    struct.pack_into('<4I', image, 0x20, ARM9_OFFSET, ARM9_AT, ARM9_AT, len(code9))
    struct.pack_into('<4I', image, 0x30, arm7_offset, ARM7_AT, ARM7_AT, len(code7))
    image[ARM9_OFFSET:ARM9_OFFSET + len(code9)] = code9
    image[arm7_offset:arm7_offset + len(code7)] = code7
    struct.pack_into('<2I', image, 0x80, len(image), 0x4000)
    struct.pack_into('<H', image, 0x15c, crc16(image[0xc0:0x15c]))
    struct.pack_into('<H', image, 0x15e, crc16(image[:0x15e]))
    rom = OUT / 'Impeccable DS.nds'
    rom.write_bytes(image)
    print(f'{rom}: {len(image)} bytes, ARM9 {len(code9)}, ARM7 {len(code7)}')
    return rom


if __name__ == '__main__':
    build()
