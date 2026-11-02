/* RG DS Impeccable: the DS performance benchmark of RG DS OS. docs/IMPECCABLE.md has the why and the rules.
 *
 * The ROM runs the list below in order, one settings change at a time through the engine's bench port. Its progress
 * lives host-side in the port's file, so the restart a settings change needs picks up where it left off, whether the
 * engine came back through a saved state or from a cold boot. */
#include "port.h"
#include "scenes.h"
#include "score.h"

#define BUILD_VERSION 3 /* scores are only comparable within one version of the tests and rules */

/* Runtime for clang's helpers: no libgcc on this target. */
void* memset(void* to, int value, unsigned long n) {
    u8* p = to;
    while (n--) *p++ = (u8)value;
    return to;
}
void* memcpy(void* to, const void* from, unsigned long n) {
    u8* p = to;
    const u8* q = from;
    while (n--) *p++ = *q++;
    return to;
}
void __aeabi_memclr(void* to, unsigned long n) { memset(to, 0, n); }
void __aeabi_memclr4(void* to, unsigned long n) { memset(to, 0, n); }
void __aeabi_memclr8(void* to, unsigned long n) { memset(to, 0, n); }
void __aeabi_memset(void* to, unsigned long n, int v) { memset(to, v, n); }
void __aeabi_memcpy(void* to, const void* from, unsigned long n) { memcpy(to, from, n); }
void __aeabi_memcpy4(void* to, const void* from, unsigned long n) { memcpy(to, from, n); }
void __aeabi_memcpy8(void* to, const void* from, unsigned long n) { memcpy(to, from, n); }
u64 __aeabi_uidivmod(u32 n, u32 d) {
    u32 q = 0, r = 0;
    for (int i = 31; i >= 0; --i) {
        r = r << 1 | (n >> i & 1);
        if (r >= d) {
            r -= d;
            q |= 1u << i;
        }
    }
    return (u64)r << 32 | q;
}
u32 __aeabi_uidiv(u32 n, u32 d) { return (u32)__aeabi_uidivmod(n, d); }
u64 __aeabi_idivmod(s32 n, s32 d) {
    const u64 u = __aeabi_uidivmod(n < 0 ? -(u32)n : (u32)n, d < 0 ? -(u32)d : (u32)d);
    u32 q = (u32)u, r = (u32)(u >> 32);
    if ((n < 0) != (d < 0)) q = -q;
    if (n < 0) r = -r;
    return (u64)r << 32 | q;
}
s32 __aeabi_idiv(s32 n, s32 d) { return (s32)(u32)__aeabi_idivmod(n, d); }
u64 __aeabi_lmul(u64 a, u64 b) {
    const u32 al = (u32)a, ah = (u32)(a >> 32), bl = (u32)b, bh = (u32)(b >> 32);
    const u64 low = (u64)al * bl;
    return low + ((u64)(al * bh + ah * bl) << 32);
}

/* --- The tests --------------------------------------------------------------------------------------------------- */

enum { CAT_CORE, CAT_SCALE, CAT_FILTER, CAT_SHADER, CAT_SPEED, CAT_COUNT };
static const char* const CATEGORY[CAT_COUNT] = {"CORE 1X", "3D 2X-5X", "HD FILTERS", "SHADERS", "FAST FORWARD"};
enum { KIND_PACED, KIND_FF };
typedef struct {
    u8 scene, scale, filter, shader, category, weight, seconds, kind;
    const char* name;
} Test;
#define CORE(sc, secs, w, nm) {sc, 1, 0, 0, CAT_CORE, w, secs, KIND_PACED, nm}
#define SCALED(sc, x, nm) {sc, x, 0, 0, CAT_SCALE, 3, 10, KIND_PACED, nm}
#define FILTERED(sc, x, f, nm) {sc, x, f, 0, CAT_FILTER, 3, 10, KIND_PACED, nm}
#define SHADED(id, nm) {SC_SHOWCASE, 1, 0, id, CAT_SHADER, 2, 8, KIND_PACED, nm}
#define FF(sc, x, nm) {sc, x, 0, 0, CAT_SPEED, 6, 20, KIND_FF, nm}
static const Test TESTS[] = {
    CORE(SC_PACE, 30, 12, "PACE"),
    CORE(SC_ARM9, 12, 6, "ARM9"),
    CORE(SC_JIT, 12, 6, "JIT CHURN"),
    CORE(SC_ARM7, 12, 4, "ARM7 + SPU"),
    CORE(SC_STORM, 12, 8, "2D STORM"),
    CORE(SC_GEOMETRY, 12, 8, "3D GEOMETRY"),
    CORE(SC_FILL, 12, 8, "3D FILL"),
    CORE(SC_CHURN, 12, 6, "TEXTURE CHURN"),
    CORE(SC_SPIKE, 12, 6, "STATE SPIKES"),
    CORE(SC_DUAL, 12, 6, "DUAL 3D"),
    CORE(SC_FEEDBACK, 12, 6, "CAPTURE LOOP"),
    CORE(SC_GAUNTLET, 15, 12, "GAUNTLET"),
    CORE(SC_SHOWCASE, 12, 4, "2D SHOWCASE"),
    FF(SC_ARM9, 1, "FF ARM9"),
    FF(SC_STORM, 1, "FF 2D STORM"),
    FF(SC_GAUNTLET, 1, "FF GAUNTLET"),
#define QUICK_TESTS 16 /* everything above runs at 1x with no restart */
    SHADED(1, "SHARP BILINEAR"),
    SHADED(8, "LCD + COLOURS"),
    SHADED(14, "ZFAST + COLOURS"),
    SHADED(20, "SOFT LCD GRID"),
    SHADED(23, "NATURAL GLOW"),
    SHADED(25, "NATURAL SMOOTH"),
    SHADED(26, "VIVID SMOOTH"),
    SHADED(27, "PIXEL BEADS"),
    SHADED(28, "GLOSSY TILES"),
    FILTERED(SC_SHOWCASE, 1, 4, "HD SHOWCASE"),
    FILTERED(SC_GAUNTLET, 1, 4, "HD GAUNTLET"),
    FILTERED(SC_SHOWCASE, 1, 5, "SMOOTH SHOWCASE"),
    FILTERED(SC_GAUNTLET, 1, 5, "SMOOTH GAUNTLET"),
    FF(SC_GAUNTLET, 2, "FF GAUNTLET 2X"),
    SCALED(SC_GEOMETRY, 2, "GEOMETRY 2X"),
    SCALED(SC_FILL, 2, "FILL 2X"),
    SCALED(SC_GAUNTLET, 2, "GAUNTLET 2X"),
    FILTERED(SC_GAUNTLET, 2, 5, "SMOOTH GAUNT 2X"),
    SCALED(SC_GEOMETRY, 3, "GEOMETRY 3X"),
    SCALED(SC_FILL, 3, "FILL 3X"),
    SCALED(SC_GAUNTLET, 3, "GAUNTLET 3X"),
    SCALED(SC_GEOMETRY, 4, "GEOMETRY 4X"),
    SCALED(SC_FILL, 4, "FILL 4X"),
    SCALED(SC_GAUNTLET, 4, "GAUNTLET 4X"),
    SCALED(SC_GEOMETRY, 5, "GEOMETRY 5X"),
    SCALED(SC_FILL, 5, "FILL 5X"),
    SCALED(SC_GAUNTLET, 5, "GAUNTLET 5X"),
};
#define TEST_COUNT ((int)(sizeof TESTS / sizeof TESTS[0]))

/* What survives a restart, in the port's file. */
typedef struct {
    u16 score;
    u16 work_p99_ms10; /* tenths of a millisecond */
    u16 missed;
    u16 fps10; /* uncapped frames per second, or fast-forward speed x 100 */
    u16 gap_ms;
    u16 wrong;
} Result;
typedef struct {
    u32 magic, version;
    u32 running, quick, next, runs, best, last, prior;
    Result results[TEST_COUNT];
} Saved;
#define SAVED_MAGIC 0x4D434550u
_Static_assert(sizeof(Saved) <= 4 * 1024, "the port's buffer holds 1024 words");
static Saved saved;

static void saved_load(void) {
    const u32 words = port_cmd(CMD_LOAD, 0, 0, 0);
    if (words * 4 == sizeof saved) {
        port_seek(0);
        u32* to = (u32*)&saved;
        for (u32 i = 0; i < words; ++i) to[i] = port_get();
    }
    if (saved.magic != SAVED_MAGIC || saved.version != BUILD_VERSION) {
        memset(&saved, 0, sizeof saved);
        saved.magic = SAVED_MAGIC;
        saved.version = BUILD_VERSION;
    }
}

static void saved_write(void) {
    port_seek(0);
    const u32* from = (const u32*)&saved;
    for (u32 i = 0; i < sizeof saved / 4; ++i) port_put(from[i]);
    port_cmd(CMD_SAVE, sizeof saved / 4, 0, 0);
}

/* --- Text on the bitmap screens ---------------------------------------------------------------------------------- */

static const u8 FONT[][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x5F, 0x00, 0x00}, {0x00, 0x07, 0x00, 0x07, 0x00}, {0x14, 0x7F, 0x14, 0x7F, 0x14},
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, {0x23, 0x13, 0x08, 0x64, 0x62}, {0x36, 0x49, 0x55, 0x22, 0x50}, {0x00, 0x05, 0x03, 0x00, 0x00},
    {0x00, 0x1C, 0x22, 0x41, 0x00}, {0x00, 0x41, 0x22, 0x1C, 0x00}, {0x14, 0x08, 0x3E, 0x08, 0x14}, {0x08, 0x08, 0x3E, 0x08, 0x08},
    {0x00, 0x50, 0x30, 0x00, 0x00}, {0x08, 0x08, 0x08, 0x08, 0x08}, {0x00, 0x60, 0x60, 0x00, 0x00}, {0x20, 0x10, 0x08, 0x04, 0x02},
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00}, {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4B, 0x31},
    {0x18, 0x14, 0x12, 0x7F, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39}, {0x3C, 0x4A, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03},
    {0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1E}, {0x00, 0x36, 0x36, 0x00, 0x00}, {0x00, 0x56, 0x36, 0x00, 0x00},
    {0x08, 0x14, 0x22, 0x41, 0x00}, {0x14, 0x14, 0x14, 0x14, 0x14}, {0x00, 0x41, 0x22, 0x14, 0x08}, {0x02, 0x01, 0x51, 0x09, 0x06},
    {0x32, 0x49, 0x79, 0x41, 0x3E}, {0x7E, 0x11, 0x11, 0x11, 0x7E}, {0x7F, 0x49, 0x49, 0x49, 0x36}, {0x3E, 0x41, 0x41, 0x41, 0x22},
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x09, 0x01}, {0x3E, 0x41, 0x49, 0x49, 0x7A},
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, {0x00, 0x41, 0x7F, 0x41, 0x00}, {0x20, 0x40, 0x41, 0x3F, 0x01}, {0x7F, 0x08, 0x14, 0x22, 0x41},
    {0x7F, 0x40, 0x40, 0x40, 0x40}, {0x7F, 0x02, 0x0C, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F}, {0x3E, 0x41, 0x41, 0x41, 0x3E},
    {0x7F, 0x09, 0x09, 0x09, 0x06}, {0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46}, {0x46, 0x49, 0x49, 0x49, 0x31},
    {0x01, 0x01, 0x7F, 0x01, 0x01}, {0x3F, 0x40, 0x40, 0x40, 0x3F}, {0x1F, 0x20, 0x40, 0x20, 0x1F}, {0x3F, 0x40, 0x38, 0x40, 0x3F},
    {0x63, 0x14, 0x08, 0x14, 0x63}, {0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43},
};

static volatile u16* screen(int s) { return s ? BG_B : BG_A; }

static void rect(int s, int x, int y, int w, int h, u16 c) {
    volatile u16* v = screen(s);
    for (int j = y; j < y + h; ++j)
        for (int i = x; i < x + w; ++i)
            if (i >= 0 && i < 256 && j >= 0 && j < 192) v[j * 256 + i] = c;
}

static int text(int s, int x, int y, int size, u16 c, const char* t) {
    for (; *t; ++t) {
        const int ch = *t >= 'a' && *t <= 'z' ? *t - 32 : *t;
        if (ch >= 32 && ch <= 90) {
            const u8* g = FONT[ch - 32];
            for (int col = 0; col < 5; ++col)
                for (int row = 0; row < 7; ++row)
                    if (g[col] >> row & 1) rect(s, x + col * size, y + row * size, size, size, c);
        }
        x += 6 * size;
    }
    return x;
}

static char* u_to_a(char* out, u32 value) {
    char tmp[12];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    while (n) *out++ = tmp[--n];
    *out = 0;
    return out;
}

static int number(int s, int x, int y, int size, u16 c, u32 value) {
    char t[12];
    u_to_a(t, value);
    return text(s, x, y, size, c, t);
}

#define WHITE RGB1(31, 31, 31)
#define GREY RGB1(16, 17, 19)
#define DIM RGB1(8, 9, 11)
#define INK RGB1(2, 2, 4)
#define GOLD RGB1(31, 25, 6)
#define RED RGB1(31, 8, 6)
#define GREEN RGB1(10, 28, 12)

static void ui_mode(void) {
    gfx_reset();
    VRAMCNT(BANK_A) = 0x81;
    VRAMCNT(BANK_C) = 0x84;
    DISPCNT_A = 5u | 1u << 16 | 1u << 11;
    DISPCNT_B = 5u | 1u << 16 | 1u << 11;
    BG_CNT(0, 3) = (u16)(1u << 14 | 1u << 7 | 1u << 2);
    BG_CNT(1, 3) = (u16)(1u << 14 | 1u << 7 | 1u << 2);
    rect(0, 0, 0, 256, 192, INK);
    rect(1, 0, 0, 256, 192, INK);
}

/* --- One test ---------------------------------------------------------------------------------------------------- */

static void port_info(u32* info) {
    port_cmd(CMD_INFO, 0, 0, 0);
    port_seek(0);
    for (int i = 0; i < 9; ++i) info[i] = port_get();
}

static int smoke; /* every test for a second, with a screenshot: proves the flow, scores nothing worth keeping */

/* Waits for the settings; the restart this asks for ends this process, so normally this does not return from the
 * request, and the next boot finds them in place. False when they never arrived. */
static int settings_for(const Test* t) {
    u32 info[9];
    port_info(info);
    const int ok = info[2] == t->scale && info[3] == t->filter && info[4] == t->shader;
    if (ok) return 1;
    ui_mode();
    text(1, 16, 80, 2, WHITE, "APPLYING");
    char line[32];
    char* p = u_to_a(line, t->scale);
    *p++ = 'X';
    *p = 0;
    text(1, 16, 104, 2, GREY, line);
    saved_write();
    if (!port_cmd(CMD_REQUEST, t->scale, t->filter, t->shader)) return 0;
    for (int i = 0; i < 60 * 30; ++i) { /* a save-quit and restart takes seconds; a game menu that never came takes this */
        wait_vblank();
        port_info(info);
        if (info[2] == t->scale && info[3] == t->filter && info[4] == t->shader) return 1;
    }
    return 0;
}

static u32 summary[F_COUNT];

/* Frames, up to a limit of real time: at 5 fps a fixed count would take minutes. The score reads rates, so a short
 * window costs a slow test nothing but samples. */
#define MIN_FRAMES 20u
static void run_frames(const Scene* scene, u32 frames, u32 limit_ms, u32* n) {
    const u32 start = port_cmd(CMD_NOW, 0, 0, 0);
    for (u32 i = 0; i < frames; ++i) {
        wait_vblank();
        scene->frame((*n)++);
        if (i >= MIN_FRAMES && port_cmd(CMD_NOW, 0, 0, 0) - start > limit_ms) break;
    }
}

static void window(const Scene* scene, int mode, u32 frames, u32 limit_ms, u32* n) {
    port_cmd(CMD_BEGIN, (u32)mode, 0, 0);
    run_frames(scene, frames, limit_ms, n);
    port_cmd(CMD_END, 0, 0, 0);
    port_seek(0);
    for (int i = 0; i < F_COUNT; ++i) summary[i] = port_get();
}

static char log_text[1024];
static char* log_at;
static void log_str(const char* s) {
    while (*s) *log_at++ = *s++;
}
static void log_field(const char* key, u32 value) {
    *log_at++ = ',';
    *log_at++ = '"';
    log_str(key);
    *log_at++ = '"';
    *log_at++ = ':';
    log_at = u_to_a(log_at, value);
}
static void log_send(void) {
    *log_at++ = '}';
    const u32 bytes = (u32)(log_at - log_text);
    port_seek(0);
    while ((u32)(log_at - log_text) & 3) *log_at++ = ' ';
    const u32* w = (const u32*)log_text;
    for (u32 i = 0; i < (u32)(log_at - log_text) / 4; ++i) port_put(w[i]);
    port_cmd(CMD_LOG, bytes, 0, 0);
}

static void run_test(int index) {
    const Test* t = &TESTS[index];
    Result* r = &saved.results[index];
    u32 n = 0;
    memset(r, 0, sizeof *r);
    if (!settings_for(t)) {
        /* Never applied: it scores 0, and the run moves on. */
        r->wrong = 0xFFFF;
        return;
    }
    gfx_reset();
    g_wrong = 0;
    const Scene* scene = &SCENES[t->scene];
    scene->init();
    run_frames(scene, smoke ? 30 : 90, 3000, &n); /* warm-up: first-use costs of the settings' own start are not this test's */
    if (smoke) port_cmd(CMD_SHOT, 0, 0, 0);
    log_at = log_text;
    log_str("{\"test\":\"");
    log_str(t->name);
    *log_at++ = '"';
    log_field("version", BUILD_VERSION);
    log_field("scale", t->scale);
    log_field("filter2d", t->filter);
    log_field("shader", t->shader);
    if (t->kind == KIND_PACED) {
        /* a quarter over the time it should take, so a scene at full speed always gets all its frames */
        window(scene, RUN_PACED, smoke ? 60 : (u32)t->seconds * 60, (u32)t->seconds * 1250, &n);
        PacedRun p = {0};
        p.frames = summary[F_FRAMES];
        p.wall_us = summary[F_WALL_US];
        const u32 missed = summary[F_MISSED_0] > summary[F_MISSED_1] ? summary[F_MISSED_0] : summary[F_MISSED_1];
        p.missed = missed + summary[F_DROPPED] + summary[F_SKIPPED];
        p.gap_us = summary[F_GAP_US];
        p.period_us = summary[F_PERIOD_US];
        p.hires = summary[F_HIRES];
        p.audio_dry = summary[F_AUDIO_DRY];
        p.latency_p99_us = summary[F_LAT_P99];
        r->missed = (u16)p.missed;
        r->work_p99_ms10 = (u16)(summary[F_WORK_P99] / 100);
        r->gap_ms = (u16)(p.gap_us / 1000);
        log_field("frames", p.frames);
        log_field("wall_us", p.wall_us);
        log_field("missed", p.missed);
        log_field("flips0", summary[F_FLIPS_0]);
        log_field("flips1", summary[F_FLIPS_1]);
        log_field("dropped", summary[F_DROPPED]);
        log_field("late", summary[F_LATE]);
        log_field("gap_us", p.gap_us);
        log_field("period_us", p.period_us);
        log_field("work_p50_us", summary[F_WORK_P50]);
        log_field("work_p99_us", summary[F_WORK_P99]);
        log_field("work_p999_us", summary[F_WORK_P999]);
        log_field("work_max_us", summary[F_WORK_MAX]);
        log_field("lat_p50_us", summary[F_LAT_P50]);
        log_field("lat_p99_us", summary[F_LAT_P99]);
        log_field("hires", summary[F_HIRES]);
        log_field("locked", summary[F_LOCKED]);
        log_field("audio_dry", summary[F_AUDIO_DRY]);
        log_field("temp_start", summary[F_TEMP_START]);
        log_field("temp_end", summary[F_TEMP_END]);
        /* the same scene with the brakes off: every frame drawn, as fast as it goes */
        window(scene, RUN_UNCAPPED, smoke ? 30 : 240, 4000, &n);
        p.uncapped_frames = summary[F_FRAMES];
        p.uncapped_wall_us = summary[F_WALL_US];
        const u32 fps = uncapped_fps_q16(p.uncapped_frames, p.uncapped_wall_us);
        r->fps10 = (u16)(mul_div(fps, 10, Q16));
        r->score = (u16)score_paced(&p, t->scale > 1);
        log_field("uncapped_fps10", r->fps10);
        log_field("uncapped_work_p99_us", summary[F_WORK_P99]);
    } else {
        window(scene, RUN_FF, smoke ? 60 : (u32)t->seconds * 60, (u32)t->seconds * 1000, &n);
        r->fps10 = (u16)mul_div(summary[F_FRAMES] - 1, 16715u * 100u, summary[F_WALL_US]);
        r->score = (u16)score_ff(summary[F_FRAMES], summary[F_WALL_US]);
        r->work_p99_ms10 = (u16)(summary[F_WORK_P99] / 100);
        log_field("frames", summary[F_FRAMES]);
        log_field("wall_us", summary[F_WALL_US]);
        log_field("presents", summary[F_PRESENTS]);
        log_field("speed_x100", r->fps10);
    }
    r->wrong = g_wrong > 0xFFFE ? 0xFFFE : (u16)g_wrong;
    if (g_wrong) r->score = 0;
    log_field("wrong", g_wrong);
    log_field("score", r->score);
    log_send();
}

/* --- Results ----------------------------------------------------------------------------------------------------- */

static u32 total_of(int count, int category) {
    static u16 scores[TEST_COUNT];
    static u8 weights[TEST_COUNT];
    int n = 0;
    for (int i = 0; i < count; ++i)
        if (category < 0 || TESTS[i].category == category) {
            scores[n] = saved.results[i].score;
            weights[n++] = TESTS[i].weight;
        }
    return score_total(scores, weights, n);
}

static int tests_in_run(void) { return saved.quick ? QUICK_TESTS : TEST_COUNT; }

/* The score's own voice. Nothing short of 10000 is a pass. */
static const char* verdict(u32 total) {
    if (total >= 10000) return "IMPECCABLE";
    if (total >= 9000) return "SO CLOSE. NOT YET.";
    if (total >= 7000) return "GOOD IS NOT THE GOAL";
    if (total >= 4000) return "NOT YET";
    if (total >= 1500) return "NOT EVEN CLOSE";
    return "A LONG WAY TO GO";
}

/* score out of 1000 */
static u16 bar_colour(u32 score) { return score >= 1000 ? GOLD : score >= 500 ? GREEN : score >= 200 ? RGB1(28, 20, 6) : RED; }

static void page_summary(void) {
    const u32 total = saved.last;
    rect(0, 0, 0, 256, 192, INK);
    rect(1, 0, 0, 256, 192, INK);
    text(0, 8, 8, 1, GREY, saved.quick ? "RG DS IMPECCABLE  /  QUICK RUN" : "RG DS IMPECCABLE  /  DS");
    char big[12];
    u_to_a(big, total);
    int digits = 0;
    while (big[digits]) ++digits;
    const u16 colour = total >= 10000 ? GOLD : WHITE;
    text(0, (256 - digits * 36) / 2, 44, 6, colour, big);
    text(0, 64, 96, 1, DIM, "OF 10000");
    const char* say = verdict(total);
    int len = 0;
    while (say[len]) ++len;
    text(0, (256 - len * 12) / 2, 116, 2, total >= 10000 ? GOLD : RED, say);
    text(0, 56, 146, 1, GREY, total >= 10000 ? "NOW RAISE THE BAR." : "KEEP PUSHING.");
    int x = text(0, 8, 172, 1, DIM, "BEST ");
    x = number(0, x, 172, 1, GREY, saved.best);
    x = text(0, x + 12, 172, 1, DIM, "PREVIOUS ");
    number(0, x, 172, 1, GREY, saved.prior);

    int y = 8;
    const int count = tests_in_run();
    for (int c = 0; c < CAT_COUNT; ++c) {
        if (c != CAT_CORE && c != CAT_SPEED && saved.quick) continue;
        const u32 s = total_of(count, c);
        text(1, 8, y, 1, WHITE, CATEGORY[c]);
        number(1, 216, y, 1, GREY, s);
        rect(1, 8, y + 10, 240, 3, DIM);
        rect(1, 8, y + 10, (int)(s * 240 / 10000), 3, bar_colour(s / 10));
        y += 22;
    }
    /* The weakest link: where the next push pays most. */
    int weak = 0;
    for (int i = 1; i < count; ++i)
        if (saved.results[i].score < saved.results[weak].score) weak = i;
    y += 2;
    text(1, 8, y, 1, DIM, "WEAKEST LINK");
    y += 11;
    text(1, 8, y, 1, RED, TESTS[weak].name);
    number(1, 216, y, 1, RED, saved.results[weak].score / 10);
    text(1, 8, 180, 1, DIM, "L/R EACH TEST   A BACK TO MENU");
}

#define TESTS_PER_PAGE 23
static void page_tests(int page) {
    const int count = tests_in_run(), per = TESTS_PER_PAGE;
    const int top = page * per;
    rect(1, 0, 0, 256, 192, INK);
    rect(0, 0, 0, 256, 192, INK);
    text(0, 8, 8, 1, GREY, "EACH TEST, 0-1000");
    text(0, 8, 28, 1, GREY, "SCORE  THE TEST");
    text(0, 8, 40, 1, GREY, "UNC    FPS WITH THE BRAKES OFF");
    text(0, 8, 52, 1, GREY, "WORK   99TH PCT FRAME, MS");
    text(0, 8, 64, 1, GREY, "MISS   REFRESHES NOT NEW");
    text(0, 8, 84, 1, DIM, "FF: THE SPEED IS IN UNC, X100");
    text(0, 8, 104, 1, DIM, "FLAWLESS AT 60 WITH NOTHING");
    text(0, 8, 114, 1, DIM, "TO SPARE IS 500. 1000 NEEDS");
    text(0, 8, 124, 1, DIM, "120 FPS OF HEADROOM AND NOT");
    text(0, 8, 134, 1, DIM, "ONE MISSED REFRESH.");
    int y = 2;
    for (int i = top; i < count && i < top + per; ++i) {
        const Result* r = &saved.results[i];
        text(1, 2, y, 1, WHITE, TESTS[i].name);
        number(1, 100, y, 1, r->wrong ? RED : bar_colour(r->score / 10), r->wrong == 0xFFFF ? 0 : r->score / 10);
        number(1, 130, y, 1, GREY, TESTS[i].kind == KIND_FF ? r->fps10 : r->fps10 / 10);
        number(1, 166, y, 1, DIM, r->work_p99_ms10 / 10);
        number(1, 196, y, 1, r->missed ? RED : DIM, r->missed);
        y += 8;
    }
}

/* --- Top level --------------------------------------------------------------------------------------------------- */

static void finish_run(void) {
    const int count = tests_in_run();
    const u32 total = total_of(count, -1);
    saved.prior = saved.last;
    saved.last = total;
    if (!saved.quick && total > saved.best) saved.best = total;
    ++saved.runs;
    saved.running = 0;
    saved_write();
    log_at = log_text;
    log_str("{\"total\":");
    log_at = u_to_a(log_at, total);
    log_field("version", BUILD_VERSION);
    log_field("quick", saved.quick);
    log_field("run", saved.runs);
    log_send();
}

static void title_screen(void) {
    ui_mode();
    text(0, 24, 40, 3, WHITE, "IMPECCABLE");
    text(0, 56, 76, 1, GREY, "THE RG DS BENCHMARK");
    text(0, 26, 120, 1, DIM, "10000 IS THE ONLY PASS.");
    text(0, 26, 132, 1, DIM, "IT IS MEANT TO BE OUT OF REACH.");
    text(1, 24, 40, 1, WHITE, "A       FULL RUN, ABOUT 15 MINUTES");
    text(1, 24, 60, 1, WHITE, "START   1X ONLY, ABOUT 6 MINUTES");
    if (saved.runs) text(1, 24, 80, 1, WHITE, "B       LAST RESULTS");
    text(1, 24, 120, 1, DIM, "PLUG IN. LEAVE IT ALONE. THE GAME");
    text(1, 24, 130, 1, DIM, "RESTARTS AS SETTINGS CHANGE.");
}

static void no_port_screen(void) {
    ui_mode();
    text(0, 24, 40, 3, WHITE, "IMPECCABLE");
    text(1, 16, 70, 1, WHITE, "THIS ROM MEASURES RG DS OS.");
    text(1, 16, 84, 1, GREY, "A PROGRAM INSIDE AN EMULATOR");
    text(1, 16, 94, 1, GREY, "CANNOT SEE REAL TIME, SO IT");
    text(1, 16, 104, 1, GREY, "NEEDS DSCORE'S BENCH PORT.");
    for (;;) wait_vblank();
}

static u32 wait_key(void) {
    while (keys_down()) wait_vblank();
    for (;;) {
        wait_vblank();
        const u32 k = keys_down();
        if (k) return k;
    }
}

static void run_all(void) {
    while (saved.running && (int)saved.next < tests_in_run()) {
        run_test((int)saved.next);
        ++saved.next;
        saved_write();
    }
    if (saved.running) finish_run();
}

static void results_loop(void) {
    ui_mode();
    int page = 0;
    const int pages = 1 + (tests_in_run() + TESTS_PER_PAGE - 1) / TESTS_PER_PAGE;
    page_summary();
    for (;;) {
        const u32 k = wait_key();
        if (k & (KEY_R | KEY_RIGHT)) page = (page + 1) % pages;
        else if (k & (KEY_L | KEY_LEFT)) page = (page + pages - 1) % pages;
        else if (k & (KEY_A | KEY_START)) return;
        else continue;
        if (page == 0) page_summary();
        else page_tests(page - 1);
    }
}

int main(void) {
    gfx_reset();
    scenes_boot();
    if (!port_present()) no_port_screen();
    saved_load();
    u32 info[9];
    port_info(info);
    smoke = info[8] == 3 || (info[8] >= 100 && info[8] < 200);
    if (info[8] >= 100) { /* one test alone, by index, for working on a scene: 100+ a smoke of it, 200+ at full length */
        run_test((int)info[8] % 100);
        for (;;) wait_vblank();
    }
    if (info[8] && !saved.running) {
        saved.quick = info[8] == 2;
        saved.running = 1;
        saved.next = 0;
        memset(saved.results, 0, sizeof saved.results);
        saved_write();
    }
    run_all(); /* a restart mid-run lands here */
    if (info[8]) { /* hands-free: a screenshot of every page, for the tool to look at, then nothing more to do */
        ui_mode();
        const int pages = 1 + (tests_in_run() + TESTS_PER_PAGE - 1) / TESTS_PER_PAGE;
        for (int page = 0; page < pages; ++page) {
            if (page == 0) page_summary();
            else page_tests(page - 1);
            for (int i = 0; i < 90; ++i) wait_vblank();
            port_cmd(CMD_SHOT, 0, 0, 0);
            for (int i = 0; i < 90; ++i) wait_vblank();
        }
        title_screen();
        for (int i = 0; i < 90; ++i) wait_vblank();
        port_cmd(CMD_SHOT, 0, 0, 0);
        for (;;) wait_vblank();
    }
    if (saved.runs) results_loop();
    for (;;) {
        title_screen();
        const u32 k = wait_key();
        if (k & KEY_B && saved.runs) {
            results_loop();
            continue;
        }
        if (!(k & (KEY_A | KEY_START))) continue;
        saved.quick = (k & KEY_START) != 0;
        saved.running = 1;
        saved.next = 0;
        memset(saved.results, 0, sizeof saved.results);
        saved_write();
        run_all();
        results_loop();
    }
}
