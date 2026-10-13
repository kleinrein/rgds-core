/* Original RGDS 3D workload, no game assets or SDK dependency.
 * Fixed opaque triangles cover the screen in several layers. This exercises
 * geometry, rasterization and scaling, not texture/shadow compatibility.
 */
typedef unsigned int u32;
typedef unsigned short u16;
#define REG32(a) (*(volatile u32 *)(a))
#define REG16(a) (*(volatile u16 *)(a))
#ifndef TRIANGLES
#define TRIANGLES 128
#endif

static void vertex(int x, int y, int z) {
    REG32(0x0400048c) = (u16)x | ((u32)(u16)y << 16);
    REG32(0x0400048c) = (u16)z;
}

void main(void) {
    REG16(0x04000304) = 0x820f;
    REG32(0x04000000) = 0x10108; /* Display mode 1, BG0 enabled and 3D. */
    REG16(0x04000008) = 0;
    REG16(0x04000060) = 0;
    REG32(0x04000350) = (31u << 16) | (3u << 10);
    REG16(0x04000354) = 0x7fff;
    REG32(0x04000580) = 0xbfff0000;
    REG32(0x04000440) = 0; REG32(0x04000454) = 0;
    REG32(0x04000440) = 1; REG32(0x04000454) = 0;
    REG32(0x040004a4) = (31u << 16) | 0xc0;
    volatile u32 *clock = (volatile u32 *)0x02010000;
    clock[0]=0x53444752; clock[1]=0x4d415246;
    clock[2]=0x4f525045; clock[3]=0x31304542;
    u32 low=0, high=0;
    for (;;) {
        while (REG16(0x04000006) >= 192) {}
        while (REG16(0x04000006) < 192) {}
        if (++low == 0) ++high;
        clock[4]=low; clock[5]=high; clock[6]=~low; clock[7]=~high;
        for (u32 i=0; i<TRIANGLES; ++i) {
            /* 8 by 8 cells, with successive layers nearer the camera. */
            int x=-4096+(int)(i&7)*1024;
            int y=-4096+(int)((i>>3)&7)*1024;
            int z=2048-(int)(i>>6)*128;
            REG32(0x04000480) = 4+(i&15) | ((8+(i&7))<<5) | ((24-(i&15))<<10);
            REG32(0x04000500) = 0;
            vertex(x+32,y+32,z); vertex(x+960,y+128,z); vertex(x+128,y+960,z);
            REG32(0x04000504) = 0;
        }
        REG32(0x04000540) = 0;
    }
}
