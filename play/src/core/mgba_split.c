/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "core/mgba_split.h"

#include <mgba/core/core.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/io.h>
#include <mgba/internal/gba/renderers/video-software.h>

#include <stdlib.h>
#include <string.h>

/* Bright magenta: a colour GBA HUDs don't use (BGR555 0x7C1F). */
#define KEY_555 0x7C1F

struct PlaySplit {
    struct GBA* gba;
    struct GBAVideoRenderer* main;
    struct GBAVideoSoftwareRenderer hud;
    /* the main renderer's own entries, called first */
    uint16_t (*writeVideoRegister)(struct GBAVideoRenderer*, uint32_t, uint16_t);
    void (*writeVRAM)(struct GBAVideoRenderer*, uint32_t);
    void (*writePalette)(struct GBAVideoRenderer*, uint32_t, uint16_t);
    void (*writeOAM)(struct GBAVideoRenderer*, uint32_t);
    void (*drawScanline)(struct GBAVideoRenderer*, int);
    void (*finishFrame)(struct GBAVideoRenderer*);
    void (*reset)(struct GBAVideoRenderer*);
    /* sprite memory as each renderer sees it: the HUD's sprites taken out of
     * the game's copy, everything else out of the HUD's */
    union GBAOAM game_oam, hud_oam;
    bool on;
    unsigned bg_mask;
    int sprite_priority;
    PlayObjRule rules[8];
    int rule_count;
    unsigned effects_off;
};

/* The renderer an mGBA callback was called with belongs to one split; a
 * handful of live games at once is more than the player ever runs. */
#define SLOTS 4
static PlaySplit* slots[SLOTS];

static PlaySplit* split_of(struct GBAVideoRenderer* r) {
    for (int i = 0; i < SLOTS; ++i)
        if (slots[i] && slots[i]->main == r) return slots[i];
    return NULL;
}

uint32_t play_split_key(void) { return (uint32_t)mColorFrom555(KEY_555) & 0xFFFFFF; }

/* An object hidden: not affine, disabled (attr0 bits 8-9 = 10). */
static uint16_t hidden(uint16_t attr0) { return (uint16_t)((attr0 & ~0x0300) | 0x0200); }

static bool to_hud(const PlaySplit* s, const struct GBAObj* obj) {
    const int priority = (obj->c >> 10) & 3, tile = obj->c & 0x3FF;
    for (int i = 0; i < s->rule_count; ++i) {
        const PlayObjRule* r = &s->rules[i];
        if (tile >= r->lo && tile <= r->hi && (r->priority < 0 || r->priority == priority)) return true;
    }
    return s->sprite_priority >= 0 && priority == s->sprite_priority;
}

static void filter_object(PlaySplit* s, int index) {
    const struct GBAObj* obj = &s->gba->video.oam.obj[index];
    bool hud = s->on && to_hud(s, obj);
    s->game_oam.obj[index] = *obj;
    s->hud_oam.obj[index] = *obj;
    if (hud) s->game_oam.obj[index].a = hidden(obj->a);
    else s->hud_oam.obj[index].a = hidden(obj->a);
}

static void filter_all(PlaySplit* s) {
    for (int i = 0; i < 128; ++i) filter_object(s, i);
}

/* WININ as the game picture sees it: without the effect bits of the windows
 * whose shade belonged to a panel now on the second screen. */
static uint16_t game_winin(const PlaySplit* s, uint16_t value) {
    if (!s->on) return value;
    if (s->effects_off & 1) value &= (uint16_t)~0x0020;
    if (s->effects_off & 2) value &= (uint16_t)~0x2000;
    return value;
}

static void apply_layers(PlaySplit* s) {
    for (int i = 0; i < 4; ++i) {
        bool hud = s->on && ((s->bg_mask >> i) & 1);
        s->main->disableBG[i] = hud;
        s->hud.d.disableBG[i] = !hud;
    }
    s->hud.d.disableOBJ = !(s->on && (s->sprite_priority >= 0 || s->rule_count > 0));
    s->main->oam = s->on ? &s->game_oam : &s->gba->video.oam;
    s->writeVideoRegister(s->main, GBA_REG_WININ, game_winin(s, s->gba->memory.io[GBA_REG(WININ)]));
    filter_all(s);
    s->writeOAM(s->main, 0);
    s->hud.d.writeOAM(&s->hud.d, 0);
}

/* The HUD renderer never blends: its backdrop must stay the key colour, and
 * the HUD reads best at full strength on the second screen. */
static uint16_t hud_register(PlaySplit* s, uint32_t address, uint16_t value) {
    if (address == GBA_REG_BLDCNT) value = 0;
    return s->hud.d.writeVideoRegister(&s->hud.d, address, value);
}

static void hud_palette(PlaySplit* s, uint32_t address, uint16_t value) {
    s->hud.d.writePalette(&s->hud.d, address, address == 0 ? KEY_555 : value);
}

static uint16_t w_register(struct GBAVideoRenderer* r, uint32_t address, uint16_t value) {
    PlaySplit* s = split_of(r);
    uint16_t stored = s->writeVideoRegister(r, address, address == GBA_REG_WININ ? game_winin(s, value) : value);
    /* what the game reads back is what it wrote: only the picture drops the effect */
    if (address == GBA_REG_WININ) stored = value & 0x3F3F;
    hud_register(s, address, value);
    return stored;
}
static void w_vram(struct GBAVideoRenderer* r, uint32_t address) {
    PlaySplit* s = split_of(r);
    s->writeVRAM(r, address);
    s->hud.d.writeVRAM(&s->hud.d, address);
}
static void w_palette(struct GBAVideoRenderer* r, uint32_t address, uint16_t value) {
    PlaySplit* s = split_of(r);
    s->writePalette(r, address, value);
    hud_palette(s, address, value);
}
static void w_oam(struct GBAVideoRenderer* r, uint32_t oam) {
    PlaySplit* s = split_of(r);
    /* all four halfwords, the fourth being a rotation/scaling parameter */
    filter_object(s, (int)(oam >> 2) & 127);
    s->writeOAM(r, oam);
    s->hud.d.writeOAM(&s->hud.d, oam);
}
static void w_scanline(struct GBAVideoRenderer* r, int y) {
    PlaySplit* s = split_of(r);
    s->drawScanline(r, y);
    if (s->on) s->hud.d.drawScanline(&s->hud.d, y);
}
static void w_finish(struct GBAVideoRenderer* r) {
    PlaySplit* s = split_of(r);
    s->finishFrame(r);
    s->hud.d.finishFrame(&s->hud.d);
}
static void w_reset(struct GBAVideoRenderer* r) {
    PlaySplit* s = split_of(r);
    s->reset(r);
    s->hud.d.reset(&s->hud.d);
    apply_layers(s);
}

PlaySplit* play_split_attach(struct mCore* core, uint32_t* hud) {
    if (!core || core->platform(core) != mPLATFORM_GBA) return NULL;
    int slot = -1;
    for (int i = 0; i < SLOTS; ++i)
        if (!slots[i]) slot = i;
    if (slot < 0) return NULL;
    struct GBA* gba = core->board;
    PlaySplit* s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->gba = gba;
    s->main = gba->video.renderer;
    s->sprite_priority = -1;

    GBAVideoSoftwareRendererCreate(&s->hud);
    s->hud.outputBuffer = (mColor*)hud;
    s->hud.outputBufferStride = GBA_VIDEO_HORIZONTAL_PIXELS;
    s->hud.d.palette = gba->video.palette;
    s->hud.d.vram = gba->video.vram;
    s->hud.d.oam = &s->hud_oam;
    s->hud.d.cache = NULL;
    s->hud.d.init(&s->hud.d);
    s->hud.d.reset(&s->hud.d);
    /* bring it up to the game's current registers, palette and sprites */
    hud_register(s, GBA_REG_DISPCNT, gba->memory.io[GBA_REG(DISPCNT)]);
    for (uint32_t address = GBA_REG_BG0CNT; address < 0x56; address += 2)
        if (address != 0x4E) hud_register(s, address, gba->memory.io[address >> 1]);
    for (uint32_t i = 0; i < 512; ++i) hud_palette(s, i * 2, gba->video.palette[i]);
    s->hud.d.writeVRAM(&s->hud.d, 0);

    s->writeVideoRegister = s->main->writeVideoRegister;
    s->writeVRAM = s->main->writeVRAM;
    s->writePalette = s->main->writePalette;
    s->writeOAM = s->main->writeOAM;
    s->drawScanline = s->main->drawScanline;
    s->finishFrame = s->main->finishFrame;
    s->reset = s->main->reset;
    slots[slot] = s;
    s->main->writeVideoRegister = w_register;
    s->main->writeVRAM = w_vram;
    s->main->writePalette = w_palette;
    s->main->writeOAM = w_oam;
    s->main->drawScanline = w_scanline;
    s->main->finishFrame = w_finish;
    s->main->reset = w_reset;
    apply_layers(s);
    return s;
}

void play_split_set(PlaySplit* s, bool on, unsigned bg_mask, int sprite_priority) {
    if (!s) return;
    if (s->on == on && s->bg_mask == bg_mask && s->sprite_priority == sprite_priority) return;
    s->on = on;
    s->bg_mask = bg_mask & 0xF;
    s->sprite_priority = sprite_priority;
    apply_layers(s);
}

void play_split_set_objects(PlaySplit* s, const PlayObjRule* rules, int count) {
    if (!s) return;
    if (count < 0) count = 0;
    if (count > 8) count = 8;
    if (count == s->rule_count && !memcmp(s->rules, rules, sizeof(*rules) * (size_t)count)) return;
    memcpy(s->rules, rules, sizeof(*rules) * (size_t)count);
    s->rule_count = count;
    apply_layers(s);
}

void play_split_set_window_effects_off(PlaySplit* s, unsigned windows) {
    if (!s || s->effects_off == (windows & 3)) return;
    s->effects_off = windows & 3;
    apply_layers(s);
}

bool play_split_on(const PlaySplit* s) { return s && s->on; }

void play_split_detach(PlaySplit* s) {
    if (!s) return;
    s->on = false;
    apply_layers(s);
    s->main->writeVideoRegister = s->writeVideoRegister;
    s->main->writeVRAM = s->writeVRAM;
    s->main->writePalette = s->writePalette;
    s->main->writeOAM = s->writeOAM;
    s->main->drawScanline = s->drawScanline;
    s->main->finishFrame = s->finishFrame;
    s->main->reset = s->reset;
    s->main->oam = &s->gba->video.oam;
    for (int i = 0; i < SLOTS; ++i)
        if (slots[i] == s) slots[i] = NULL;
    s->hud.d.deinit(&s->hud.d);
    free(s);
}
