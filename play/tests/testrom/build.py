#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the RG DS Play test cartridge with Zig (no devkitARM needed)."""
import argparse
from pathlib import Path
import struct
import subprocess

HERE = Path(__file__).resolve().parent
ROM_BASE = 0x08000000


def elf_to_rom(elf):
    """Lay the ELF's loadable segments out as a cartridge image."""
    data = elf.read_bytes()
    if data[:4] != b"\x7fELF" or data[4] != 1:
        raise ValueError("expected a 32-bit ELF")
    phoff, = struct.unpack_from("<I", data, 28)
    phentsize, phnum = struct.unpack_from("<HH", data, 42)
    image = bytearray()
    for i in range(phnum):
        kind, offset, _, paddr, filesz, _, _, _ = struct.unpack_from("<8I", data, phoff + i * phentsize)
        if kind != 1 or filesz == 0:
            continue
        if paddr < ROM_BASE:
            raise ValueError("a segment lies outside the cartridge (initialised RAM data is not supported)")
        start = paddr - ROM_BASE
        if len(image) < start + filesz:
            image.extend(b"\0" * (start + filesz - len(image)))
        image[start:start + filesz] = data[offset:offset + filesz]
    image.extend(b"\0" * (-len(image) % 4))
    return bytes(image)


def build(output):
    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    elf = output.with_suffix(".elf")
    subprocess.run(
        [
            "zig", "cc", "-target", "arm-freestanding-eabi", "-mcpu=arm7tdmi", "-marm", "-O2", "-nostdlib", "-Wall",
            "-Wextra", "-Wl,-T," + str(HERE / "gba.ld"),
            str(HERE / "crt0.s"),
            str(HERE / "main.c"), "-o",
            str(elf)
        ],
        check=True
    )
    output.write_bytes(elf_to_rom(elf))
    return output


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", nargs="?", default=str(HERE.parents[2] / "build/play/testrom/rgds-play-test.gba"))
    print(build(parser.parse_args().output))
