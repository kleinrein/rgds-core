@ ARM9 boot: TCMs on (DTCM stack, ITCM at 0 holding the exception vectors), then main. Vectors are moved to low memory
@ so hardware IRQs work whichever BIOS the emulator has.
.syntax unified
.arm
.section .text.start
.global _start
_start:
    mrc p15, 0, r0, c1, c0, 0
    bic r0, r0, #(1 << 13)         @ vectors at 0x00000000
    orr r0, r0, #(1 << 18)         @ ITCM on
    orr r0, r0, #(1 << 16)         @ DTCM on
    ldr r1, =0x0B00000A            @ DTCM at 0x0B000000, 16 KB
    mcr p15, 0, r1, c9, c1, 0
    mov r1, #0x20                  @ ITCM at 0, 32 MB mirror
    mcr p15, 0, r1, c9, c1, 1
    @ MPU and caches as a game has them: everything open and uncached, main RAM cached. Timed without them, code in main
    @ RAM runs an order of magnitude slower than any game's does.
    ldr r1, =0x3F                  @ region 0: 4 GB at 0
    mcr p15, 0, r1, c6, c0, 0
    ldr r1, =0x0200002D            @ region 1: 8 MB of main RAM
    mcr p15, 0, r1, c6, c1, 0
    mov r1, #2
    mcr p15, 0, r1, c2, c0, 0      @ data cache: region 1
    mcr p15, 0, r1, c2, c0, 1      @ instruction cache: region 1
    mcr p15, 0, r1, c3, c0, 0      @ write buffer: region 1
    ldr r1, =0x33
    mcr p15, 0, r1, c5, c0, 2      @ data access: read/write
    mcr p15, 0, r1, c5, c0, 3      @ instruction access: read/write
    mov r1, #0
    mcr p15, 0, r1, c7, c5, 0      @ invalidate both
    mcr p15, 0, r1, c7, c6, 0
    orr r0, r0, #1                 @ MPU on
    orr r0, r0, #(1 << 2)          @ data cache
    orr r0, r0, #(1 << 3)          @ write buffer
    orr r0, r0, #(1 << 12)         @ instruction cache
    mcr p15, 0, r0, c1, c0, 0
    msr cpsr_c, #0xD2              @ IRQ mode stack
    ldr sp, =0x0B003C00
    msr cpsr_c, #0xDF              @ system mode stack, IRQs still off
    ldr sp, =0x0B003B00
    ldr r0, =__bss_start
    ldr r1, =__bss_end
    mov r2, #0
1:  cmp r0, r1
    strlo r2, [r0], #4
    blo 1b
    @ IRQ vector -> irq_entry
    ldr r0, =0x00000018
    ldr r1, =0xE51FF004            @ ldr pc, [pc, #-4]
    str r1, [r0]
    ldr r1, =irq_entry
    str r1, [r0, #4]
    bl main
    b .

.global irq_entry
irq_entry:
    push {r0-r3, r12, lr}
    ldr r3, =irq_handler
    blx r3
    pop {r0-r3, r12, lr}
    subs pc, lr, #4
.ltorg

.global cp15_flush_code
cp15_flush_code:
    mov r0, #0
    mcr p15, 0, r0, c7, c5, 0      @ invalidate the instruction cache: what a game's overlay loader does
    bx lr
