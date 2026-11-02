#pragma once
#include "kernels.h"
#define REG32(a) (*(volatile u32*)(a))
#define REG16(a) (*(volatile u16*)(a))
#define REG8(a) (*(volatile u8*)(a))

#define DISPCNT_A REG32(0x04000000)
#define DISPCNT_B REG32(0x04001000)
#define DISPSTAT REG16(0x04000004)
#define VCOUNT REG16(0x04000006)
#define DISP3DCNT REG16(0x04000060)
#define DISPCAPCNT REG32(0x04000064)
#define KEYINPUT REG16(0x04000130)
#define IPCSYNC REG16(0x04000180)
#define IME REG32(0x04000208)
#define IE REG32(0x04000210)
#define IF REG32(0x04000214)
#define POWCNT1 REG16(0x04000304)
#define VRAMCNT(bank) REG8(0x04000240 + (bank))
enum { BANK_A, BANK_B, BANK_C, BANK_D, BANK_E, BANK_F, BANK_G, WRAMCNT_, BANK_H, BANK_I };
#define LCDC_A ((volatile u16*)0x06800000)
#define LCDC_B ((volatile u16*)0x06820000)
#define LCDC_C ((volatile u16*)0x06840000)
#define LCDC_D ((volatile u16*)0x06860000)
#define LCDC_E ((volatile u16*)0x06880000)
#define BG_A ((volatile u16*)0x06000000)
#define BG_B ((volatile u16*)0x06200000)
#define OBJ_A ((volatile u16*)0x06400000)
#define OBJ_B ((volatile u16*)0x06600000)
#define OAM_A ((volatile u16*)0x07000000)
#define OAM_B ((volatile u16*)0x07000400)
#define PAL_BG_A ((volatile u16*)0x05000000)
#define PAL_OBJ_A ((volatile u16*)0x05000200)
#define PAL_BG_B ((volatile u16*)0x05000400)
#define PAL_OBJ_B ((volatile u16*)0x05000600)
#define BG_CNT(e, n) REG16(0x04000008 + (e) * 0x1000 + (n) * 2)
#define BG_HOFS(e, n) REG16(0x04000010 + (e) * 0x1000 + (n) * 4)
#define BG_VOFS(e, n) REG16(0x04000012 + (e) * 0x1000 + (n) * 4)
#define BG_PA(e, n) REG16(0x04000020 + (e) * 0x1000 + ((n) - 2) * 16)
#define BG_PB(e, n) REG16(0x04000022 + (e) * 0x1000 + ((n) - 2) * 16)
#define BG_PC(e, n) REG16(0x04000024 + (e) * 0x1000 + ((n) - 2) * 16)
#define BG_PD(e, n) REG16(0x04000026 + (e) * 0x1000 + ((n) - 2) * 16)
#define BG_X(e, n) REG32(0x04000028 + (e) * 0x1000 + ((n) - 2) * 16)
#define BG_Y(e, n) REG32(0x0400002C + (e) * 0x1000 + ((n) - 2) * 16)
#define WIN0H(e) REG16(0x04000040 + (e) * 0x1000)
#define WIN1H(e) REG16(0x04000042 + (e) * 0x1000)
#define WIN0V(e) REG16(0x04000044 + (e) * 0x1000)
#define WIN1V(e) REG16(0x04000046 + (e) * 0x1000)
#define WININ(e) REG16(0x04000048 + (e) * 0x1000)
#define WINOUT(e) REG16(0x0400004A + (e) * 0x1000)
#define MOSAIC(e) REG16(0x0400004C + (e) * 0x1000)
#define BLDCNT(e) REG16(0x04000050 + (e) * 0x1000)
#define BLDALPHA(e) REG16(0x04000052 + (e) * 0x1000)
#define DMA_SRC(n) REG32(0x040000B0 + (n) * 12)
#define DMA_DST(n) REG32(0x040000B4 + (n) * 12)
#define DMA_CTL(n) REG32(0x040000B8 + (n) * 12)
#define DMA_ON (1u << 31)
#define DMA_32 (1u << 26)
#define DMA_REPEAT (1u << 25)
#define DMA_DST_FIX (2u << 21)
#define DMA_DST_RELOAD (3u << 21)
#define DMA_HBLANK (2u << 27)
#define DMA_GX (7u << 27)

/* Geometry */
#define GX(cmd) REG32(0x04000400 + (cmd) * 4)
#define GXFIFO REG32(0x04000400)
#define GXSTAT REG32(0x04000600)
#define CLEAR_COLOR REG32(0x04000350)
#define CLEAR_DEPTH REG16(0x04000354)
#define FOG_COLOR REG32(0x04000358)
#define FOG_OFFSET REG16(0x0400035C)
#define ALPHA_TEST REG8(0x04000340)
enum {
    MTX_MODE = 0x10, MTX_PUSH, MTX_POP, MTX_STORE, MTX_RESTORE, MTX_IDENTITY, MTX_LOAD44, MTX_LOAD43, MTX_MULT44,
    MTX_MULT43, MTX_MULT33, MTX_SCALE, MTX_TRANS,
    GX_COLOR = 0x20, GX_NORMAL, GX_TEXCOORD, GX_VTX16, GX_VTX10, GX_VTXXY, GX_VTXXZ, GX_VTXYZ, GX_VTXDIFF, GX_POLY_ATTR,
    GX_TEX_PARAM, GX_PAL_BASE,
    GX_DIF_AMB = 0x30, GX_SPE_EMI, GX_LIGHT_VEC, GX_LIGHT_COL, GX_SHININESS,
    GX_BEGIN = 0x40, GX_END, GX_SWAP = 0x50, GX_VIEWPORT = 0x60, GX_BOX = 0x70, GX_POS, GX_VEC
};
#define TRIS 0
#define QUADS 1
#define TRI_STRIP 2
#define QUAD_STRIP 3

#define RGB5(r, g, b) ((u16)((r) | ((g) << 5) | ((b) << 10)))
#define RGB1(r, g, b) ((u16)(0x8000 | RGB5(r, g, b)))

#define KEY_A 1
#define KEY_B 2
#define KEY_SELECT 4
#define KEY_START 8
#define KEY_RIGHT 16
#define KEY_LEFT 32
#define KEY_UP 64
#define KEY_DOWN 128
#define KEY_R 256
#define KEY_L 512

static inline u32 keys_down(void) { return ~KEYINPUT & 0x3FF; }
static inline void irq_on(void) { __asm__ volatile("msr cpsr_c, #0x1F"); }
/* Asleep until the VBlank interrupt, as games wait. A loop reading VCOUNT would be charged to the emulator: it runs every
 * instruction of it, since the idle-loop skip can't take a register the scheduler has to work out. */
extern volatile u32 g_vblanks;
static inline void wait_vblank(void) {
    const u32 seen = g_vblanks;
    DISPSTAT = (u16)(DISPSTAT | 1u << 3);
    IE |= 1;
    IME = 1;
    irq_on();
    while (g_vblanks == seen) __asm__ volatile("mcr p15, 0, %0, c7, c0, 4" : : "r"(0));
}
