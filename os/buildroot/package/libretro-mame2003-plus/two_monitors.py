#!/usr/bin/env python3
"""Cut mame2003-plus down to Nintendo's two-monitor cabinets, a core of a few megabytes instead of forty.

Run in the unpacked source (a post-patch hook): the drivers' sources in Makefile.common and the list in src/driver.c
keep only Punch-Out!!/Super Punch-Out!!/Arm Wrestling and the Vs. dual system's games, and only the CPU and sound
cores those boards carry are built. Every anchor must be found exactly once, or nothing is written.
"""
import re
import sys
from pathlib import Path

# The sets the library shows (src/launcher/model.hpp arcade_sets), bring-over copies (runtime/play_options.py
# ARCADE_SETS) and play/profiles/arcade names: tests/test_play_runtime.py keeps the four the same.
GAMES = [
    "punchout", "spnchout", "spnchotj", "armwrest", "vstennis", "wrecking", "balonfgt", "vsmahjng", "vsbball",
    "vsbballj", "vsbbalja", "iceclmrj"
]
DRIVER_SOURCES = [
    "vidhrdw/punchout_vidhrdw.c",
    "drivers/punchout.c",
    "machine/rp5h01.c",
    "vidhrdw/ppu2c03b.c",
    "machine/vsnes_machine.c",
    "vidhrdw/vsnes_vidhrdw.c",
    "drivers/vsnes.c",
]
CPUS = ["Z80", "N2A03"]  # Punch-Out's Z80 and the sound/Vs. 2A03s
# Boards the core's own files name (info.c's BIOS sets, the content flags in core_options.c).
LEFT_OUT_DRIVERS = [
    "coh1000a", "coh1000c", "coh1000t", "coh1000w", "coh1001l", "coh1002e", "coh1002m", "coh1002t", "coh1002v",
    "coh3002c", "coh3002t", "cvs", "decocass", "konamigx", "megaplay", "megatech", "namcoc76", "neogeo", "nss", "pgm",
    "playch10", "skns", "stvbios"
]
SOUNDS = ["CUSTOM", "SAMPLES", "DAC", "NES", "VLM5030"]  # core files use the first two; the 2A03 APU, speech, DAC


def main(root: Path) -> None:
    make = root / "Makefile.common"
    text = make.read_text()
    # The drivers' block: the SOURCES_C after the sound list, up to its first empty line, before the CPU cores.
    start = text.index("\nSOURCES_C += \\\n", text.index("SOUNDS+=ICS2115@\n"))
    end = text.index("\n\n", start + 1)
    assert end < text.index("# List of CPU core") and "drivers/punchout.c" in text[start:end]
    lines = [f"\t$(CORE_DIR)/{s}" for s in DRIVER_SOURCES]
    text = text[:start] + "\nSOURCES_C += \\\n" + " \\\n".join(lines) + text[end:]
    for kind, keep in (("CPUS", CPUS), ("SOUNDS", SOUNDS)):
        pattern = re.compile(rf"^#?{kind}\+=([A-Z0-9_]+)@$", re.M)
        assert len(pattern.findall(text)) > 10, kind
        text = pattern.sub(
            lambda m: f"{kind}+={m.group(1)}@" if m.group(1) in keep else f"#{kind}+={m.group(1)}@", text
        )
        for name in keep:
            assert re.search(rf"^{kind}\+={name}@$", text, re.M), (kind, name)

    driver = root / "src" / "driver.c"
    listing = driver.read_text()
    head = "#else\t/* DRIVER_RECURSIVE */\n"
    tail = "#endif\t/* DRIVER_RECURSIVE */"
    assert listing.count(head) == 1 and listing.count(tail) == 1
    a = listing.index(head) + len(head)
    b = listing.index(tail)
    for game in GAMES:
        assert re.search(rf"DRIVER\(\s*{game}\s*\)", listing[a:b]), game
    listing = listing[:a] + "\n" + "".join(f"\tDRIVER( {g} )\n" for g in GAMES) + "\n" + listing[b:]

    # The core's own files name boards left out (the BIOS sets info.c lists, Neo Geo's memory card menu, Sega's
    # FD1094 CPU on the 68000): stand-ins in a file of ours, and FD1089/FD1094 not built.
    for gone in ("machine/fd1089.c", "machine/fd1094.c"):
        line = f"\t$(CORE_DIR)/{gone} \\\n"
        assert text.count(line) == 1, gone
        text = text.replace(line, "")
    anchor = "\t$(CORE_DIR)/driver.c \\\n"
    assert text.count(anchor) == 1
    text = text.replace(anchor, anchor + "\t$(CORE_DIR)/rgds_left_out.c \\\n")
    stubs = [
        '/* Written by RG DS (two_monitors.py): what the core names of the boards left out of this build. */',
        '#include "driver.h"'
    ]
    stubs += [f"struct GameDriver driver_{name};" for name in LEFT_OUT_DRIVERS]
    stubs += [
        "int mcd_action, mcd_number, memcard_status, memcard_number;",
        "int neogeo_memcard_load(int number) { (void)number; return 0; }", "void neogeo_memcard_save(void) {}",
        "void neogeo_memcard_eject(void) {}", "int neogeo_memcard_create(int number) { (void)number; return 0; }", ""
    ]

    make.write_text(text)
    driver.write_text(listing)
    (root / "src" / "rgds_left_out.c").write_text("\n".join(stubs))


if __name__ == "__main__":
    main(Path(sys.argv[1] if len(sys.argv) > 1 else "."))
