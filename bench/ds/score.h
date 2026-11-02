/* The scoring rules, integer only so the ROM and a native test share them (score_test.c).
 *
 * Every test scores 0-10000 inside (the screen shows tenths dropped: 0-1000) and the total is a weighted GEOMETRIC mean
 * scaled to 10000: one weak test sinks the run, and strength elsewhere cannot buy it back. IMPECCABLE is exactly 10000.
 *
 * A paced test is the product of:
 *   smooth    0.90 per missed refresh, normalised to 15 seconds. One miss in 15 s costs a tenth.
 *   hitch     0.95^((g-1)^2) for the longest gap of g refreshes. A single long stall hurts more than scattered ones.
 *   speed     (achieved / 59.83 fps)^4, capped at 1.
 *   headroom  from the same scene run uncapped: 120 fps or more 1.0; 60 fps 0.5; below 60, 0.5 * (fps/60)^2.
 *             Holding 60 with nothing to spare is half marks, so a real optimisation always moves the score.
 *   sharp     (share of frames on the GPU raster)^16 at 2x-5x, so falling back to native is never a pass.
 *   audio     0.97 per audio underrun, normalised to 15 seconds. Crackle is a failure the eye never sees.
 *   latency   by the 99th percentile from swap to flip: half a refresh (8.3 ms) or less 1.0, falling in a line to 0.5 at
 *             two refreshes (33.3 ms) and beyond.
 * Under the product sits the climb: 2% * (fps/60)^2 of full marks. Far below full speed the product is nothing at all,
 * and without this a renderer going from 4 to 8 fps would not move the score.
 * Fast forward scores log2(speed) / log2(8): 8x is full marks; below 1x only the climb is left.
 * A test that gives wrong CPU results or never got its settings scores 0, which the mean floors at 0.01%. */
#pragma once
#include "kernels.h"

#define Q16 65536u
#define SCORE_FULL 10000u
#define SCORE_FLOOR 1u
#define CLIMB_FULL 200u
#define FF_TARGET_LOG2 (3u * Q16)

static inline u32 q_mul(u32 a, u32 b) { return (u32)(((u64)a * b) >> 16); }

static inline u32 div_u32(u32 n, u32 d) {
    if (!d) return 0;
    u32 q = 0, r = 0;
    for (int i = 31; i >= 0; --i) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) {
            r -= d;
            q |= 1u << i;
        }
    }
    return q;
}

/* a * b / c with a 64-bit middle, no libgcc. */
static inline u32 mul_div(u32 a, u32 b, u32 c) {
    if (!c) return 0;
    u64 n = (u64)a * b, q = 0, r = 0;
    for (int i = 63; i >= 0; --i) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= c) {
            r -= c;
            q |= (u64)1 << i;
        }
    }
    return q > 0xFFFFFFFFu ? 0xFFFFFFFFu : (u32)q;
}

/* base^(exp), both Q16, base <= 1; a fractional exponent interpolates between its whole neighbours. */
static inline u32 q_pow(u32 base, u32 exp) {
    u32 whole = exp >> 16, frac = exp & 0xFFFF, out = Q16, b = base;
    if (whole > 1000) return 0;
    for (u32 n = whole; n; n >>= 1) {
        if (n & 1) out = q_mul(out, b);
        b = q_mul(b, b);
    }
    return out - q_mul(out - q_mul(out, base), frac);
}

/* log2 of a Q16 value >= 1/65536, as signed Q16. */
static inline s32 q_log2(u32 x) {
    if (!x) return -16 * (s32)Q16;
    s32 out = 0;
    while (x >= 2 * Q16) {
        x >>= 1;
        out += Q16;
    }
    while (x < Q16) {
        x <<= 1;
        out -= Q16;
    }
    for (u32 bit = Q16 >> 1; bit; bit >>= 1) {
        x = q_mul(x, x);
        if (x >= 2 * Q16) {
            x >>= 1;
            out += (s32)bit;
        }
    }
    return out;
}

/* 2^x for signed Q16 x; a table of 2^(2^-n) squares it up. */
static inline u32 q_exp2(s32 x) {
    static const u32 ROOTS[8] = {92682, 77936, 71468, 68438, 66971, 66250, 65892, 65714};
    s32 whole = x >> 16;
    u32 frac = (u32)x & 0xFFFF, out = Q16;
    for (int n = 0; n < 8; ++n)
        if (frac & (0x8000u >> n)) out = q_mul(out, ROOTS[n]);
    if (whole >= 15) return 0xFFFFFFFFu;
    return whole >= 0 ? out << whole : out >> -whole;
}

typedef struct {
    u32 frames, wall_us, missed, gap_us, period_us, hires, uncapped_frames, uncapped_wall_us, audio_dry, latency_p99_us;
} PacedRun;

/* Q16 factors of a paced test. */
static inline u32 factor_smooth(u32 missed, u32 frames) {
    if (!frames) return 0;
    return q_pow(58982 /* 0.90 */, mul_div(missed, 900u * Q16, frames));
}

static inline u32 factor_hitch(u32 gap_us, u32 period_us) {
    if (!period_us) period_us = 16667;
    u32 g = div_u32(gap_us + period_us / 2, period_us);
    if (g <= 1) return Q16;
    g = g - 1 > 40 ? 40 : g - 1;
    return q_pow(62259 /* 0.95 */, g * g * Q16);
}

static inline u32 factor_speed(u32 frames, u32 wall_us) {
    if (frames < 2 || !wall_us) return 0;
    /* frames - 1 intervals at the DS rate of 59.8261 fps = 16715 us; a lock to a 60 Hz panel runs slightly above */
    u32 s = mul_div(frames - 1, 16715u * Q16, wall_us);
    if (s >= Q16) return Q16;
    return q_mul(q_mul(s, s), q_mul(s, s));
}

static inline u32 uncapped_fps_q16(u32 frames, u32 wall_us) {
    if (frames < 2 || !wall_us) return 0;
    return mul_div((frames - 1) * 1024u, 64000000u, wall_us); /* 1024 * 64000000 = 1000000 * Q16 */
}

static inline u32 factor_headroom(u32 fps_q16) {
    const u32 sixty = 60 * Q16;
    if (fps_q16 >= 2 * sixty) return Q16;
    if (fps_q16 >= sixty) return Q16 / 2 + div_u32((fps_q16 - sixty), 120);
    u32 r = div_u32(fps_q16, 60);
    return q_mul(q_mul(r, r), Q16 / 2);
}

static inline u32 factor_sharp(u32 hires, u32 frames, int scaled) {
    if (!scaled) return Q16;
    if (!frames) return 0;
    return q_pow(mul_div(hires > frames ? frames : hires, Q16, frames), 16 * Q16);
}

/* The climb, from a speed where 1.0 (Q16) is full speed. */
static inline u32 score_climb(u32 speed_q16) {
    if (speed_q16 >= Q16) return CLIMB_FULL;
    return (u32)(((u64)q_mul(speed_q16, speed_q16) * CLIMB_FULL) >> 16);
}

static inline u32 factor_audio(u32 dry, u32 frames) {
    if (!frames) return 0;
    return q_pow(63570 /* 0.97 */, mul_div(dry, 900u * Q16, frames));
}

/* 0: a display that reports no flips, so nothing is known. */
static inline u32 factor_latency(u32 latency_us) {
    if (latency_us <= 8333) return Q16;
    if (latency_us >= 33333) return Q16 / 2;
    return Q16 - mul_div(latency_us - 8333, Q16 / 2, 25000);
}

static inline u32 score_paced(const PacedRun* r, int scaled) {
    u32 v = Q16;
    v = q_mul(v, factor_smooth(r->missed, r->frames));
    v = q_mul(v, factor_hitch(r->gap_us, r->period_us));
    v = q_mul(v, factor_speed(r->frames, r->wall_us));
    v = q_mul(v, factor_headroom(uncapped_fps_q16(r->uncapped_frames, r->uncapped_wall_us)));
    v = q_mul(v, factor_sharp(r->hires, r->frames, scaled));
    v = q_mul(v, factor_audio(r->audio_dry, r->frames));
    v = q_mul(v, factor_latency(r->latency_p99_us));
    const u32 product = (u32)(((u64)v * SCORE_FULL) >> 16);
    const u32 climb = q_mul(score_climb(div_u32(uncapped_fps_q16(r->uncapped_frames, r->uncapped_wall_us), 60)), factor_sharp(r->hires, r->frames, scaled));
    return product > climb ? product : climb;
}

static inline u32 score_ff(u32 frames, u32 wall_us) {
    if (frames < 2 || !wall_us) return 0;
    u32 speed = mul_div(frames - 1, 16715u * Q16, wall_us);
    if (speed <= Q16) return score_climb(speed);
    u32 l = (u32)q_log2(speed);
    if (l >= FF_TARGET_LOG2) return SCORE_FULL;
    const u32 s = mul_div(l, SCORE_FULL, FF_TARGET_LOG2);
    return s > CLIMB_FULL ? s : CLIMB_FULL;
}

/* The total: 10000 * the weighted geometric mean of score/1000. Exactly 10000 only when every test is 1000. */
static inline u32 score_total(const u16* scores, const u8* weights, int count) {
    s64 sum = 0;
    u32 weight = 0;
    int perfect = 1;
    for (int i = 0; i < count; ++i) {
        u32 s = scores[i] < SCORE_FLOOR ? SCORE_FLOOR : scores[i];
        if (s > SCORE_FULL) s = SCORE_FULL;
        if (s != SCORE_FULL) perfect = 0;
        sum += (s64)q_log2(mul_div(s, Q16, SCORE_FULL)) * weights[i];
        weight += weights[i];
    }
    if (!weight) return 0;
    if (perfect) return 10000;
    s64 mean = -(s64)div_u32((u32)(-sum), weight);
    u32 total = (u32)(((u64)q_exp2((s32)mean) * 10000u) >> 16);
    return total >= 10000 ? 9999 : total;
}
