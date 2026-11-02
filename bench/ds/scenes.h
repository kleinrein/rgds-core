#pragma once
#include "hw.h"
enum {
    SC_PACE, SC_ARM9, SC_JIT, SC_ARM7, SC_STORM, SC_GEOMETRY, SC_FILL, SC_CHURN, SC_SPIKE, SC_DUAL, SC_FEEDBACK, SC_GAUNTLET, SC_SHOWCASE,
    SCENE_COUNT
};
typedef struct {
    void (*init)(void);
    void (*frame)(u32 n);
} Scene;
extern const Scene SCENES[SCENE_COUNT];
extern volatile u32 g_wrong;
void scenes_boot(void);
void gfx_reset(void);
