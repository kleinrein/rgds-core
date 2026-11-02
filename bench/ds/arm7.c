/* ARM7 side: idle until the ARM9 asks for load through IPCSYNC, then all 16 sound channels and a busy CPU. */
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;
#define REG32(a) (*(volatile u32*)(a))
#define REG16(a) (*(volatile u16*)(a))
#define REG8(a) (*(volatile u8*)(a))
#define IPCSYNC REG16(0x04000180)
#define POWCNT2 REG16(0x04000304)
#define SOUNDCNT REG16(0x04000500)
#define CH_CNT(n) REG32(0x04000400 + (n) * 16)
#define CH_SAD(n) REG32(0x04000404 + (n) * 16)
#define CH_TMR(n) REG16(0x04000408 + (n) * 16)
#define CH_PNT(n) REG16(0x0400040A + (n) * 16)
#define CH_LEN(n) REG32(0x0400040C + (n) * 16)
#define SAMPLES 0x02300000 /* the ARM9 fills this: a PCM16 tone per channel (arm9 main.c) */

static void sound_on(void) {
    POWCNT2 = 3; /* sound + wifi clock */
    SOUNDCNT = 0x8000 | 30; /* quiet: the mixer still runs for every channel */
    for (int n = 0; n < 16; ++n) {
        CH_TMR(n) = (u16)(0x10000 - 0x200 + n * 8);
        if (n < 8) {
            CH_SAD(n) = SAMPLES + n * 0x1000;
            CH_PNT(n) = 0;
            CH_LEN(n) = 0x400;
            CH_CNT(n) = 0x80000000u | (1u << 27) | (1u << 29) | (0x40u << 16) | 40; /* PCM16, loop */
        } else {
            CH_LEN(n) = 0;
            CH_CNT(n) = 0x80000000u | (3u << 29) | (3u << 24) | (0x40u << 16) | 16; /* PSG (8-13) and noise (14-15) */
        }
    }
}

static void sound_off(void) {
    for (int n = 0; n < 16; ++n) CH_CNT(n) = 0;
    SOUNDCNT = 0;
}

static u32 table[64];

static u32 burn(u32 x) {
    for (int i = 0; i < 64; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        table[i & 63] += x;
        if (x & 1) x += table[(i * 7) & 63];
        else x -= table[(i * 3) & 63];
    }
    return x;
}

void main7(void) {
    u32 x = 0x12345678, mode = 0;
    for (;;) {
        u32 want = IPCSYNC & 15;
        if (want != mode) {
            if (want & 1) sound_on();
            else sound_off();
            mode = want;
        }
        if (mode & 1) x = burn(x);
    }
}
