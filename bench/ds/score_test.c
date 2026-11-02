/* Native check of the scoring rules; tools/build_impeccable.py runs it before it builds the ROM. */
#include <stdio.h>
#include <stdlib.h>
#include "score.h"

#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("score_test.c:%d: %s\n", __LINE__, #c);                                                             \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)
#define NEAR(a, b, tolerance) CHECK(abs((int)(a) - (int)(b)) <= (tolerance))

static PacedRun run(u32 fps_uncapped) {
    PacedRun r = {900, 15000000, 0, 16667, 16667, 900, (u32)fps_uncapped * 4 + 1, 4000000};
    return r;
}

int main(void) {
    NEAR(q_exp2(0), 65536, 1);
    NEAR(q_exp2(65536), 131072, 2);
    NEAR(q_exp2(-65536), 32768, 1);
    NEAR(q_log2(131072), 65536, 8);
    NEAR(q_log2(32768), -65536, 8);
    NEAR(q_exp2(q_log2(50000)), 50000, 80);
    NEAR(q_pow(58982, 65536), 58982, 1);
    NEAR(q_pow(58982, 2 * 65536), 53084, 2);

    PacedRun r = run(120);
    CHECK(score_paced(&r, 0) == 10000); /* flawless, with a whole frame to spare */
    r = run(60);
    NEAR(score_paced(&r, 0), 5000, 20); /* flawless, nothing to spare: half */
    r = run(90);
    NEAR(score_paced(&r, 0), 7500, 20);
    r = run(120);
    r.missed = 1;
    NEAR(score_paced(&r, 0), 9000, 20); /* one miss in 15 s */
    r.missed = 10;
    NEAR(score_paced(&r, 0), 3487, 30);
    r.missed = 0;
    r.gap_us = 16667 * 3; /* a stall of three refreshes */
    NEAR(score_paced(&r, 0), 8145, 30);
    r = run(30);
    r.wall_us = 30083000; /* 30 fps */
    r.missed = 900;
    NEAR(score_paced(&r, 0), 50, 1); /* only the climb is left */
    r = run(8);
    r.missed = 900;
    u32 slow = score_paced(&r, 0);
    r = run(4);
    r.missed = 900;
    CHECK(slow > score_paced(&r, 0)); /* twice the frame rate moves the score, however far from full speed */
    r = run(120);
    r.hires = 800; /* adaptive fell back on a ninth of the frames */
    CHECK(score_paced(&r, 1) < 2000);
    CHECK(score_paced(&r, 0) == 10000);

    NEAR(score_ff(600, 10000000), 200, 2);    /* 1x: the climb's top */
    CHECK(score_ff(300, 10000000) < 100);     /* half speed */
    NEAR(score_ff(1200, 10000000), 3333, 30); /* 2x */
    CHECK(score_ff(4800, 10000000) == 10000);

    r = run(120);
    r.audio_dry = 1;
    NEAR(score_paced(&r, 0), 9700, 20); /* one underrun in 15 s */
    r.audio_dry = 10;
    NEAR(score_paced(&r, 0), 7374, 40);
    r = run(120);
    r.latency_p99_us = 8000;
    CHECK(score_paced(&r, 0) == 10000); /* within half a refresh */
    r.latency_p99_us = 20833;
    NEAR(score_paced(&r, 0), 7500, 20); /* halfway down the line */
    r.latency_p99_us = 90000;
    NEAR(score_paced(&r, 0), 5000, 20); /* and no further */

    u16 all[4] = {10000, 10000, 10000, 10000};
    u8 equal[4] = {1, 1, 1, 1};
    CHECK(score_total(all, equal, 4) == 10000);
    all[3] = 9999;
    CHECK(score_total(all, equal, 4) < 10000); /* never rounds up to impeccable */
    u16 half[4] = {5000, 5000, 5000, 5000};
    NEAR(score_total(half, equal, 4), 5000, 12);
    u16 one_bad[4] = {10000, 10000, 10000, 0};
    NEAR(score_total(one_bad, equal, 4), 1000, 40); /* one dead test out of four */
    puts("score ok");
    return 0;
}
