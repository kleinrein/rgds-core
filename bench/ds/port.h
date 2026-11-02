/* The RG DS bench port (dscore/src/core/io/bench_port.h, dscore/src/frontend/sdl/bench_recorder.h). */
#pragma once
#include "hw.h"
#define PORT(off) REG32(0x04FFFB00 + (off))
#define PORT_MAGIC 0x50424752u
enum { CMD_INFO = 1, CMD_BEGIN = 2, CMD_END = 3, CMD_REQUEST = 5, CMD_LOAD = 6, CMD_SAVE = 7, CMD_LOG = 8, CMD_SHOT = 9, CMD_NOW = 10 };
enum { RUN_PACED, RUN_FF, RUN_UNCAPPED };
enum {
    F_FRAMES, F_PRESENTS, F_SKIPPED, F_HIRES, F_WALL_US, F_FLIPS_0, F_FLIPS_1, F_MISSED_0, F_MISSED_1, F_DROPPED, F_LATE,
    F_GAP_US, F_WORK_P50, F_WORK_P99, F_WORK_P999, F_WORK_MAX, F_LAT_P50, F_LAT_P99, F_PERIOD_US, F_AUDIO_DRY,
    F_TEMP_START, F_TEMP_END, F_LOCKED, F_COUNT
};
static inline int port_present(void) { return PORT(0) == PORT_MAGIC; }
static inline u32 port_cmd(u32 cmd, u32 a, u32 b, u32 c) {
    PORT(4) = a;
    PORT(8) = b;
    PORT(12) = c;
    PORT(0) = cmd;
    return PORT(0x14);
}
static inline void port_seek(u32 i) { PORT(0x18) = i; }
static inline u32 port_get(void) { return PORT(0x1C); }
static inline void port_put(u32 v) { PORT(0x1C) = v; }
