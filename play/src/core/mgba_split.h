/* SPDX-License-Identifier: GPL-3.0-or-later */
/* HUD split for the live GBA core: a second mGBA software renderer that is
 * told about every register, palette, VRAM and sprite write the game makes,
 * and draws only the HUD layers, on a key colour where there is no HUD. While
 * the split is on, the game's own renderer leaves those layers out. The game
 * itself (its memory, timing, state) is not touched: only what is drawn. */
#ifndef PLAY_MGBA_SPLIT_H
#define PLAY_MGBA_SPLIT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mCore;
typedef struct PlaySplit PlaySplit;

/* The HUD picture's "nothing here" colour, as mGBA writes pixels (bytes
 * R, G, B, X): compare with (pixel & 0xFFFFFF). */
uint32_t play_split_key(void);

/* `hud` is GBA_W x GBA_H pixels, owned by the caller while attached. Only a
 * GBA core; NULL otherwise. One split per core. */
PlaySplit* play_split_attach(struct mCore* core, uint32_t* hud);
/* BG layers in bg_mask (bit 0 = BG0) and the sprites of priority
 * sprite_priority (-1: none) move to the HUD. Takes effect from the next
 * frame; call between frames. */
void play_split_set(PlaySplit* split, bool on, unsigned bg_mask, int sprite_priority);
bool play_split_on(const PlaySplit* split);
void play_split_detach(PlaySplit* split);

#ifdef __cplusplus
}
#endif

#endif
