/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "core/mgba_gb.h"

#include <mgba-util/crc32.h>
#include <mgba/core/core.h>
#include <mgba/gb/interface.h>
#include <mgba/internal/gb/gb.h>
#include <mgba/internal/gb/io.h>
#include <mgba/internal/gb/video.h>

static struct GB* gb_of(struct mCore* core) {
    return core && core->platform(core) == mPLATFORM_GB ? (struct GB*) core->board : NULL;
}

/* The three DMG palette registers again, so the renderer takes new colours now
 * instead of at the game's next palette write. */
static void rewrite_palettes(struct GB* gb) {
    GBVideoWritePalette(&gb->video, GB_REG_BGP, gb->memory.io[GB_REG_BGP]);
    GBVideoWritePalette(&gb->video, GB_REG_OBP0, gb->memory.io[GB_REG_OBP0]);
    GBVideoWritePalette(&gb->video, GB_REG_OBP1, gb->memory.io[GB_REG_OBP1]);
}

void play_gb_handheld_model(struct mCore* core) {
    struct GB* gb = gb_of(core);
    if (!gb || gb->model >= GB_MODEL_CGB || !(gb->model & GB_MODEL_SGB)) return;
    gb->model &= ~GB_MODEL_SGB;
    gb->video.renderer->deinit(gb->video.renderer);
    gb->video.renderer->init(gb->video.renderer, gb->model, gb->video.sgbBorders);
    /* A fresh renderer knows no registers or colours: tell it again, as mGBA's
     * own state loader does (GBVideoDeserialize, GBIODeserialize). */
    for (int i = 0; i < 64; ++i) gb->video.renderer->writePalette(gb->video.renderer, i, gb->video.palette[i]);
    static const uint16_t registers[] = {GB_REG_LCDC, GB_REG_SCY, GB_REG_SCX, GB_REG_WY, GB_REG_WX};
    for (size_t i = 0; i < sizeof(registers) / sizeof(registers[0]); ++i)
        gb->video.renderer->writeVideoRegister(gb->video.renderer, registers[i], gb->memory.io[registers[i]]);
    rewrite_palettes(gb);
}

bool play_gb_colour_model(struct mCore* core) {
    struct GB* gb = gb_of(core);
    return gb && gb->model >= GB_MODEL_CGB;
}

void play_gb_set_gbc_colours(struct mCore* core, bool on) {
    struct GB* gb = gb_of(core);
    if (!gb || gb->model >= GB_MODEL_CGB || !gb->memory.rom) return;
    /* mGBA's greys (video.c), and the Game Boy Color boot ROM's own choice for
     * a game it has no entry for: white, green, blue, black behind; white,
     * pink, dark red, black for both sprite sets. */
    static const uint32_t greys[4] = {0xF8F8F8, 0xA8A8A8, 0x505050, 0x000000};
    static const uint32_t fallback[12] = {0xFFFFFF, 0x7BFF31, 0x0063C5, 0x000000, 0xFFFFFF, 0xFF8484,
                                          0x943A3A, 0x000000, 0xFFFFFF, 0xFF8484, 0x943A3A, 0x000000};
    uint32_t colours[12];
    struct GBCartridgeOverride found = {0};
    found.headerCrc32 = (int) doCrc32(&gb->memory.rom[0x100], sizeof(struct GBCartridge));
    const bool listed = on && GBOverrideColorFind(&found, GB_COLORS_CGB);
    for (int i = 0; i < 12; ++i) {
        /* A table entry sets bit 24 for the colours it gives; the rest repeat
         * the background's or the first sprite set's (as GBOverrideApply). */
        uint32_t c = !on ? greys[i & 3] : !listed ? fallback[i] : found.gbColors[i];
        if (listed && !(c & 0xFF000000)) c = i >= 4 ? colours[i - 4] : greys[i & 3];
        colours[i] = c & 0xFFFFFF;
        GBVideoSetPalette(&gb->video, (unsigned) i, colours[i]);
    }
    rewrite_palettes(gb);
}
