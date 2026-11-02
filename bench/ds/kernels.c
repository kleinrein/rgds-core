#include "kernels.h"
#ifndef KERNEL_NAME
#define KERNEL_NAME kernel_arm
#endif
#define CAT2(a, b) a##b
#define CAT(a, b) CAT2(a, b)
#define S(name) CAT(KERNEL_NAME, name)

static u32 S(_data)[128];
static u32 S(_crc)[256];
static u32 S(_crc_ready);

static u32 S(_next)(u32 x) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

/* ALU mix with shifted operands and multiplies. */
static u32 S(_alu)(u32 x) {
    u32 a = x, b = ~x, c = x * 2654435761u;
    for (int i = 0; i < 48; ++i) {
        a = (a << 3) + (b >> 5) + c;
        b ^= a * 33u;
        c = (c >> 7 | c << 25) - (a & b);
    }
    return a ^ b ^ c;
}

/* Block copies through memory: what LDM/STM pairs spend a game's time on. */
static u32 S(_copy)(u32 x) {
    for (int i = 0; i < 64; ++i) S(_data)[i] = x = S(_next)(x);
    for (int pass = 0; pass < 4; ++pass)
        for (int i = 0; i < 56; i += 8) {
            u32* to = S(_data) + 64 + i;
            const u32* from = S(_data) + i;
            to[0] = from[0];
            to[1] = from[1];
            to[2] = from[2];
            to[3] = from[3];
            to[4] = from[4];
            to[5] = from[5];
            to[6] = from[6];
            to[7] = from[7];
        }
    return S(_data)[64 + (x & 31)];
}

/* An insertion sort of noise: branches no predictor could guess. */
static u32 S(_sort)(u32 x) {
    u32 sum = 0;
    for (int i = 0; i < 32; ++i) S(_data)[i] = x = S(_next)(x);
    for (int i = 1; i < 32; ++i) {
        u32 v = S(_data)[i];
        int j = i - 1;
        while (j >= 0 && S(_data)[j] > v) {
            S(_data)[j + 1] = S(_data)[j];
            --j;
        }
        S(_data)[j + 1] = v;
    }
    for (int i = 0; i < 32; ++i) sum = sum * 31 + S(_data)[i];
    return sum;
}

/* Byte and halfword traffic, signed and unsigned. */
static u32 S(_bytes)(u32 x) {
    u8* b = (u8*)S(_data);
    s16* h = (s16*)S(_data);
    u32 sum = 0;
    for (int i = 0; i < 64; ++i) S(_data)[i] = 0;
    for (int i = 0; i < 160; ++i) {
        x = S(_next)(x);
        b[i & 255] = (u8)x;
        h[(i * 3) & 127] = (s16)(x >> 8);
        sum += (u32)(s32)h[i & 127] ^ b[(x >> 4) & 255];
    }
    return sum;
}

/* 4x4 fixed-point matrix multiplies: 64-bit products, the geometry code of every 3D game. */
static u32 S(_matrix)(u32 x) {
    s32 m[16], n[16], out[16];
    for (int i = 0; i < 16; ++i) {
        x = S(_next)(x);
        m[i] = (s32)(x >> 16) - 0x8000;
        n[i] = (s32)(x & 0xFFFF) - 0x8000;
    }
    u32 sum = 0;
    for (int pass = 0; pass < 2; ++pass) {
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) {
                s64 v = 0;
                for (int k = 0; k < 4; ++k) v += (s64)m[r * 4 + k] * n[k * 4 + c];
                out[r * 4 + c] = (s32)(v >> 12);
            }
        for (int i = 0; i < 16; ++i) {
            sum = sum * 17 + (u32)out[i];
            m[i] = out[i];
        }
    }
    return sum;
}

/* Table lookups through a CRC, table built once. */
static u32 S(_crc32)(u32 x) {
    if (!S(_crc_ready)) {
        for (u32 i = 0; i < 256; ++i) {
            u32 c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            S(_crc)[i] = c;
        }
        S(_crc_ready) = 1;
    }
    u32 crc = ~0u;
    for (int i = 0; i < 128; ++i) {
        x = S(_next)(x);
        crc = S(_crc)[(crc ^ x) & 255] ^ (crc >> 8);
    }
    return ~crc;
}

/* A bytecode interpreter: indirect branches on every step, like a game's script engine. */
typedef u32 (*S(_op))(u32, u32);
static u32 S(_add)(u32 a, u32 b) { return a + b; }
static u32 S(_sub)(u32 a, u32 b) { return a - b; }
static u32 S(_mul)(u32 a, u32 b) { return a * (b | 1); }
static u32 S(_xor)(u32 a, u32 b) { return a ^ b; }
static u32 S(_rol)(u32 a, u32 b) { return a << (b & 31) | a >> (-b & 31); }
static u32 S(_not)(u32 a, u32 b) { return ~a + (b >> 3); }
static u32 S(_vm)(u32 x) {
    static const S(_op) ops[6] = {S(_add), S(_sub), S(_mul), S(_xor), S(_rol), S(_not)};
    u32 acc = x;
    for (int i = 0; i < 96; ++i) {
        x = S(_next)(x);
        acc = ops[(x >> 3) % 6](acc, x);
    }
    return acc;
}

u32 KERNEL_NAME(int which, u32 seed) {
    switch (which) {
    case 0: return S(_alu)(seed);
    case 1: return S(_copy)(seed);
    case 2: return S(_sort)(seed);
    case 3: return S(_bytes)(seed);
    case 4: return S(_matrix)(seed);
    case 5: return S(_crc32)(seed);
    default: return S(_vm)(seed);
    }
}
