/* SPDX-License-Identifier: GPL-3.0-or-later */
ENTRY(_start)
MEMORY { rom (rx) : ORIGIN = 0x08000000, LENGTH = 32M }
SECTIONS {
    .text : { KEEP(*(.text.start)) KEEP(*(.rodata.keep)) *(.text*) *(.rodata*) } > rom
    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes*) *(.comment*) }
}
