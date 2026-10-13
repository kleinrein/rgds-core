.syntax unified
.arm
.section .text
.global _start
_start:
    ldr sp, =0x023ff000
    ldr r0, =0x04000304
    ldr r1, =0x820f
    strh r1, [r0]
    ldr r0, =0x04000240
    mov r1, #0x80
    strb r1, [r0]              // VRAM A: LCD framebuffer.
    mov r1, #0x84
    strb r1, [r0, #2]          // VRAM C: sub-engine background.
    ldr r0, =0x04000000
    mov r1, #0x20000
    str r1, [r0]               // Main engine displays VRAM A directly.
    ldr r0, =0x04001000
    ldr r1, =0x10100
    str r1, [r0]
    ldr r1, =0x1080
    strh r1, [r0, #8]          // Sub BG0: 8bpp tile, map at 0x8000.
    ldr r0, =0x06200000
    ldr r1, =0x01010101
    mov r2, #16
1:  str r1, [r0], #4
    subs r2, r2, #1
    bne 1b
    ldr r0, =0x06208000
    mov r1, #0
    mov r2, #512
2:  str r1, [r0], #4
    subs r2, r2, #1
    bne 2b
    ldr r0, =0x05000402
    ldr r1, =0x83e0
    strh r1, [r0]              // Bottom screen green.
    // Original workload telemetry in main RAM, not a display FPS estimate.
    // 16-byte magic, uint64 vblank count and its uint64 complement.
    // Checkpoint readers reject partial updates and code-literal matches.
    ldr r10, =0x02001000
    ldr r0, =0x53444752
    str r0, [r10]
    ldr r0, =0x4d415246
    str r0, [r10, #4]
    ldr r0, =0x4f525045
    str r0, [r10, #8]
    ldr r0, =0x31304542
    str r0, [r10, #12]
    mov r8, #0
    mov r9, #0
    str r8, [r10, #16]
    str r9, [r10, #20]
    mvn r0, #0
    str r0, [r10, #24]
    str r0, [r10, #28]
3:  ldr r0, =0x04000006
4:  ldrh r1, [r0]
    cmp r1, #192
    blo 4b
5:  ldrh r1, [r0]
    cmp r1, #192
    bhs 5b
    adds r8, r8, #1
    adc r9, r9, #0
    str r8, [r10, #16]
    str r9, [r10, #20]
    mvn r0, r8
    str r0, [r10, #24]
    mvn r0, r9
    str r0, [r10, #28]
    and r2, r8, #31
    orr r2, r2, #0x8000
    orr r2, r2, #0x400
    ldr r0, =0x06800000
    ldr r1, =49152
6:  strh r2, [r0], #2
    subs r1, r1, #1
    bne 6b
    b 3b
.ltorg
