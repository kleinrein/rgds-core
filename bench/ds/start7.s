.syntax unified
.arm
.section .text.start
.global _start
_start:
    ldr sp, =0x0380FE00
    bl main7
    b .
