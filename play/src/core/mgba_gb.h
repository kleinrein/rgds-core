/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Game Boy screens in the live core: which handheld a Game Boy game is shown
 * on. mGBA picks the model from the cartridge; RG DS Play plays every game on
 * a handheld, so a Super Game Boy cartridge runs as a plain Game Boy (its four
 * shades take the chosen screen's colours, ui/colour.cpp) and the Super Game
 * Boy's TV colours are not used. A Game Boy Color can show an original Game
 * Boy game in colour: its boot ROM picks four-colour palettes for the
 * background and both sprite sets by the cartridge's header, the table mGBA
 * carries. Only what is drawn changes: memory, timing and saves are untouched. */
#ifndef PLAY_MGBA_GB_H
#define PLAY_MGBA_GB_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mCore;

/* Call after reset and after every state load: a state carries the model it
 * was made with (older states of a Super Game Boy cartridge say SGB). */
void play_gb_handheld_model(struct mCore* core);
/* True when the game runs on the Game Boy Color hardware (its own colours). */
bool play_gb_colour_model(struct mCore* core);
/* An original Game Boy game: the Game Boy Color's colours for it (true) or
 * the four plain greys (false). Takes effect at once. Nothing on a colour
 * game or a GBA core. */
void play_gb_set_gbc_colours(struct mCore* core, bool on);

#ifdef __cplusplus
}
#endif

#endif
