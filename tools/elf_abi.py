#!/usr/bin/env python3
"""Read ELF64 little-endian required GLIBC versions without executing a binary.

Only GNU version-need records count. Exported versions and strings elsewhere in
the file do not establish a runtime requirement. A missing numeric requirement
is reported as None; this is not a claim that every dependency is available.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys


ELF_HEADER = struct.Struct("<16sHHIQQQIHHHHHH")
SECTION_HEADER = struct.Struct("<IIQQQQIIQQ")
PROGRAM_HEADER = struct.Struct("<IIQQQQQQ")
DYNAMIC_ENTRY = struct.Struct("<qQ")
SYMBOL = struct.Struct("<IBBHQQ")
VERSION_NEED = struct.Struct("<HHIII")
VERSION_AUX = struct.Struct("<IHHII")
SHT_STRTAB = 3
SHT_DYNAMIC = 6
SHT_DYNSYM = 11
SHT_NOBITS = 8
SHT_GNU_VERNEED = 0x6FFFFFFE
MACHINES = {62: "x86_64", 183: "aarch64", 243: "riscv64"}
GLIBC_VERSION = re.compile(r"GLIBC_(\d+\.\d+(?:\.\d+)*)\Z")


def inspect_elf(path, source=None, *, symbols=False):
    """Return ABI/provenance metadata, None for non-ELF, or reject malformed ELF.

    ``source`` is a caller-provided provenance label; by default it is the input
    path. The SHA-256 always covers the actual inspected file. Only ELF64LE is
    supported, including AArch64 payloads and ELF host-build fixtures.
    """
    path = Path(path)
    data = path.read_bytes()
    if not data.startswith(b"\x7fELF"):
        return None

    def invalid(reason):
        raise ValueError(f"Invalid ELF {path}: {reason}")

    def bounds(offset, size, label):
        if offset < 0 or size < 0 or offset > len(data) or size > len(data) - offset:
            invalid(f"{label} is truncated or out of bounds")

    bounds(0, ELF_HEADER.size, "header")
    header = ELF_HEADER.unpack_from(data)
    identity = header[0]
    if identity[4] != 2 or identity[5] != 1:
        invalid("only ELF64 little-endian files are supported")
    if identity[6] != 1 or header[3] != 1:
        invalid("unsupported ELF version")
    if header[8] != ELF_HEADER.size:
        invalid("unexpected ELF64 header size")
    shoff, shentsize, shnum = header[6], header[11], header[12]
    if shnum == 0 and shoff:
        invalid("extended section numbering is unsupported")
    if shnum and shentsize != SECTION_HEADER.size:
        invalid("unexpected ELF64 section-header size")
    bounds(shoff, shentsize * shnum, "section-header table")
    sections = [SECTION_HEADER.unpack_from(data, shoff + i * shentsize) for i in range(shnum)]
    for section in sections:
        if section[1] != SHT_NOBITS:
            bounds(section[4], section[5], "section contents")

    def strings_for(section, label):
        if section[6] >= len(sections) or sections[section[6]][1] != SHT_STRTAB:
            invalid(label + " has no valid linked string table")
        strings = sections[section[6]]
        def string(offset):
            if offset >= strings[5]:
                invalid(label + " string is out of bounds")
            start = strings[4] + offset
            end = data.find(b"\0", start, strings[4] + strings[5])
            if end < 0:
                invalid(label + " string is not terminated")
            try:
                return data[start:end].decode("utf-8")
            except UnicodeDecodeError:
                invalid(label + " string is not valid UTF-8")
        return string

    needed, exports, imports, runpaths = set(), set(), set(), []
    soname = interpreter = None
    phoff, phentsize, phnum = header[5], header[9], header[10]
    if phnum and phentsize != PROGRAM_HEADER.size:
        invalid("unexpected ELF64 program-header size")
    bounds(phoff, phentsize * phnum, "program-header table")
    for index in range(phnum):
        program = PROGRAM_HEADER.unpack_from(data, phoff + index * phentsize)
        if program[0] == 3:  # PT_INTERP
            bounds(program[2], program[5], "interpreter")
            raw = data[program[2]:program[2] + program[5]]
            if not raw.endswith(b"\0") or b"\0" in raw[:-1]:
                invalid("interpreter is not a terminated path")
            try: interpreter = raw[:-1].decode("utf-8")
            except UnicodeDecodeError: invalid("interpreter is not valid UTF-8")
    for section in sections:
        if section[1] == SHT_DYNAMIC:
            if section[9] != DYNAMIC_ENTRY.size or section[5] % DYNAMIC_ENTRY.size:
                invalid("unexpected dynamic-entry size")
            string = strings_for(section, "dynamic")
            terminated = False
            for offset in range(section[4], section[4] + section[5], DYNAMIC_ENTRY.size):
                tag, value = DYNAMIC_ENTRY.unpack_from(data, offset)
                if tag == 0:
                    terminated = True
                    break
                if tag == 1: needed.add(string(value))
                elif tag == 14: soname = string(value)
                elif tag in (15, 29): runpaths.append(string(value))
            if not terminated: invalid("dynamic section is not terminated")
        elif symbols and section[1] == SHT_DYNSYM:
            if section[9] != SYMBOL.size or section[5] % SYMBOL.size:
                invalid("unexpected dynamic-symbol size")
            string = strings_for(section, "dynamic symbol")
            for offset in range(section[4], section[4] + section[5], SYMBOL.size):
                name_at, info, other, index, _value, _size = SYMBOL.unpack_from(data, offset)
                name = string(name_at)
                if not name or info >> 4 not in (1, 2): continue
                if index == 0: imports.add(name)
                elif other & 3 in (0, 3): exports.add(name)

    versions = set()
    for section in sections:
        if section[1] != SHT_GNU_VERNEED:
            continue
        if section[6] >= len(sections) or sections[section[6]][1] != SHT_STRTAB:
            invalid("version-need section has no valid linked string table")
        strings = sections[section[6]]
        begin, size = section[4], section[5]
        records = []

        def record(offset, layout, label):
            if offset < 0 or offset > size or layout.size > size - offset:
                invalid(f"{label} is truncated or out of bounds")
            if any(offset < end and offset + layout.size > start for start, end in records):
                invalid("version-need records overlap")
            records.append((offset, offset + layout.size))
            return layout.unpack_from(data, begin + offset)

        def name(offset):
            if offset >= strings[5]:
                invalid("version string is out of bounds")
            start = strings[4] + offset
            end = data.find(b"\0", start, strings[4] + strings[5])
            if end < 0:
                invalid("version string is not terminated")
            try:
                return data[start:end].decode("utf-8")
            except UnicodeDecodeError:
                invalid("version string is not valid UTF-8")

        position, count = 0, 0
        while size:
            version, auxiliary_count, file_name, auxiliary, following = record(position, VERSION_NEED, "version-need record")
            if version != 1:
                invalid("unsupported GNU version-need format")
            name(file_name)  # Validate the required library name as well.
            if auxiliary_count and auxiliary < VERSION_NEED.size:
                invalid("version-need auxiliary chain overlaps its header")
            auxiliary_position = position + auxiliary
            for index in range(auxiliary_count):
                _hash, _flags, _other, string_offset, next_auxiliary = record(auxiliary_position, VERSION_AUX, "version-need auxiliary record")
                match = GLIBC_VERSION.fullmatch(name(string_offset))
                if match:
                    versions.add(match.group(1))
                if index + 1 < auxiliary_count:
                    if next_auxiliary < VERSION_AUX.size:
                        invalid("version-need auxiliary chain ends early or overlaps")
                    auxiliary_position += next_auxiliary
                elif next_auxiliary:
                    invalid("version-need auxiliary count disagrees with its chain")
            count += 1
            if not following:
                break
            if following < VERSION_NEED.size:
                invalid("version-need chain overlaps an earlier record")
            position += following
        if count != section[7]:
            invalid("version-need record count disagrees with its section header")

    ordered = sorted(versions, key=lambda value: tuple(int(part) for part in value.split(".")))
    result = {
        "machine": MACHINES.get(header[2], f"elf-machine-{header[2]}"),
        "elf_class": 64,
        "endianness": "little",
        "glibc_versions": ordered,
        "glibc_minimum": ordered[-1] if ordered else None,
        "sha256": hashlib.sha256(data).hexdigest(),
        "needed": sorted(needed),
        "interpreter": interpreter,
        "soname": soname,
        "runpaths": runpaths,
        "source": str(path) if source is None else source,
    }
    if symbols:
        result.update(exports=sorted(exports), imports=sorted(imports))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("files", type=Path, nargs="+")
    parser.add_argument("--json", action="store_true", help="Print metadata keyed by input path")
    args = parser.parse_args()
    try:
        results = {str(path): inspect_elf(path) for path in args.files}
    except (OSError, ValueError) as error:
        print(error, file=sys.stderr)
        return 1
    if args.json:
        print(json.dumps(results, indent=2, sort_keys=True))
    else:
        for path, result in results.items():
            print(f"{path}: " + (f"{result['machine']}, required GLIBC {result['glibc_minimum'] or 'none recorded'}" if result else "not ELF"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
