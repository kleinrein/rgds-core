@ SPDX-License-Identifier: GPL-3.0-or-later
@ Start of the RG DS Play test cartridge. The header carries no Nintendo logo:
@ mGBA starts the game without the BIOS, and the project ships no Nintendo data.
    .section .text.start, "ax"
    .arm
    .global _start
_start:
    b       boot
    .fill   156, 1, 0               @ logo area, left empty
    .ascii  "RGDS PLAY TS"          @ title, 12 bytes at 0xA0
    .ascii  "ZRDT"                  @ game code at 0xAC
    .ascii  "01"                    @ maker
    .byte   0x96, 0, 0              @ fixed value, unit, device
    .fill   7, 1, 0
    .byte   0                       @ version
    .byte   0                       @ header checksum (not checked without the BIOS)
    .byte   0, 0
boot:
    ldr     sp, =0x03007F00
    ldr     r0, =main
    mov     lr, pc
    bx      r0
hang:
    b       hang
    .ltorg
