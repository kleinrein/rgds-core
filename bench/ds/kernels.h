/* CPU kernels: plain C, so the build also runs them natively and the ROM can hold the emulated results against the
 * right answers (tools/build_impeccable.py writes golden.h). Each takes a seed and returns a checksum; a batch is
 * about one scanline's worth of ARM9 time. Compiled twice, as ARM and as Thumb. */
#pragma once
typedef unsigned int u32;
typedef signed int s32;
typedef unsigned short u16;
typedef signed short s16;
typedef unsigned char u8;
typedef unsigned long long u64;
typedef signed long long s64;
#define KERNELS 7
u32 kernel_arm(int which, u32 seed);
u32 kernel_thumb(int which, u32 seed);
