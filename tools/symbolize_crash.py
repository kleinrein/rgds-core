#!/usr/bin/env python3
"""Turn the addresses in a crashes.log record into source lines.

Usage: tools/symbolize_crash.py crashes.log [--symbols dist/symbols]

tools/build.py keeps each device build's symbols as dist/symbols/rgds-launcher-<build id>.debug, and a record names
its build id. Addresses in other libraries are printed as file + offset, from the record's memory map.
"""
import argparse
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
FRAME = re.compile(r"#(\d+) 0x([0-9a-f]+)$")
MAP = re.compile(r"([0-9a-f]+)-([0-9a-f]+) \S+ ([0-9a-f]+) \S+ \S+\s+(\S+)$")
BASE = re.compile(r"base=0x([0-9a-f]+).*build_id=(\S*)")


def symbolizer():
    for name in ("llvm-symbolizer", "/opt/homebrew/opt/llvm/bin/llvm-symbolizer"):
        if shutil.which(name): return name
    return None


def parse(text):
    """Each record as {base, build_id, frames, maps, lines}; a torn record at the end still counts."""
    records = []
    for block in text.split("=== crash ===\n")[1:]:
        record = dict(base=0, build_id="", frames=[], maps=[], lines=[])
        for line in block.split("=== end ===")[0].splitlines():
            base = BASE.search(line)
            frame = FRAME.match(line)
            mapping = MAP.match(line)
            if base:
                record.update(base=int(base.group(1), 16), build_id=base.group(2))
            if frame:
                record["frames"].append(int(frame.group(2), 16))
            elif mapping:
                record["maps"].append(
                    (int(mapping.group(1), 16), int(mapping.group(2), 16), int(mapping.group(3), 16), mapping.group(4))
                )
                continue
            if line != "maps:": record["lines"].append(line)
        records.append(record)
    return records


def owner(record, address):
    """(file, offset in it) when the memory map knows the address."""
    for start, end, offset, path in record["maps"]:
        if start <= address < end: return path, address - start + offset
    return None, None


def locate(symbols, offset, tool):
    if not tool or not symbols: return None
    run = subprocess.run(
        [tool, "--inlining=false", "--obj=" + str(symbols),
         hex(offset)], capture_output=True, text=True, timeout=30
    )
    lines = [line for line in run.stdout.splitlines() if line.strip()]
    return " at ".join(lines[:2]) if lines and lines[0] != "??" else None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--symbols", type=Path, default=ROOT / "dist/symbols")
    args = parser.parse_args()
    tool = symbolizer()
    if not tool: print("llvm-symbolizer not found: printing offsets only (brew install llvm).")
    for record in parse(args.log.read_text(errors="replace")):
        symbols = args.symbols / f"rgds-launcher-{record['build_id']}.debug" if record["build_id"] else None
        if symbols and not symbols.is_file():
            print(f"No symbols for build {record['build_id']} in {args.symbols}.")
            symbols = None
        print("=== crash ===")
        for line in record["lines"]:
            frame = FRAME.match(line)
            if not frame:
                print(line)
                continue
            address = int(frame.group(2), 16)
            path, _ = owner(record, address)
            if path and not path.endswith("rgds-launcher"):
                _, offset = owner(record, address)
                print(f"{line}  {path}+{offset:#x}")
                continue
            # a position-independent program's addresses, less where it was loaded, are its symbol addresses
            offset = address - record["base"]
            print(f"{line}  {locate(symbols, offset, tool) or f'rgds-launcher+{offset:#x}'}")


if __name__ == "__main__":
    main()
