#!/usr/bin/env python3
"""Build an original, silent DS homebrew rendering probe (no Nintendo assets).

Top: cycling red/blue intensity. Bottom: solid green. This tests homebrew boot
and rendering, not commercial game compatibility or save restoration.
"""
import os
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def build():
    output = ROOT / 'build/nnddss-probe'; output.mkdir(parents=True, exist_ok=True)
    llvm = Path(os.environ.get('LLVM_BIN', '/opt/homebrew/opt/llvm/bin'))
    payloads = {}
    for arm, cpu, address in ((9, 'arm946e-s', 0x02000000), (7, 'arm7tdmi', 0x03800000)):
        source = ROOT / f'tests/fixtures/nnddss_probe/arm{arm}.s'
        obj = output / f'arm{arm}.o'; elf = output / f'arm{arm}.elf'; binary = output / f'arm{arm}.bin'
        subprocess.run([llvm/'clang', '-target', 'arm-none-eabi', '-mcpu='+cpu, '-c', source, '-o', obj], check=True)
        subprocess.run([llvm/'ld.lld', '-Ttext='+hex(address), '-e', '_start', obj, '-o', elf], check=True)
        subprocess.run([llvm/'llvm-objcopy', '-O', 'binary', elf, binary], check=True)
        payloads[arm] = binary.read_bytes()
    image = bytearray(0x20000)
    image[:12] = b'RGDS PROBE  '
    image[12:16] = b'####'  # Homebrew marker, no commercial game identity.
    image[16:18] = b'00'
    struct.pack_into('<4I', image, 0x20, 0x200, 0x02000000, 0x02000000, len(payloads[9]))
    struct.pack_into('<4I', image, 0x30, 0x1000, 0x03800000, 0x03800000, len(payloads[7]))
    struct.pack_into('<2I', image, 0x80, len(image), 0x200)
    image[0x200:0x200+len(payloads[9])] = payloads[9]
    image[0x1000:0x1000+len(payloads[7])] = payloads[7]
    def crc(data):
        value = 0xffff
        for byte in data:
            value ^= byte
            for _ in range(8): value = (value >> 1) ^ (0xa001 if value & 1 else 0)
        return value
    struct.pack_into('<H', image, 0x15c, crc(image[0xc0:0x15c]))
    struct.pack_into('<H', image, 0x15e, crc(image[:0x15e]))
    path = output / 'RGDS-Probe.nds'; path.write_bytes(image)
    print(path)
    return path


if __name__ == '__main__': build()
