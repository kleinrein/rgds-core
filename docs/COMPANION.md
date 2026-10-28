# The companion screen: rules

RG DS Play's second screen is part of RG DS, not a debug overlay. It looks like the launcher, follows the owner's Theme settings, reads in every language the launcher speaks, takes touches, and is useful at a glance. These rules hold for every game, and two tools check them on every profile (`tools/lint_companions.py`, which runs `play-lint`).

What the screen shows for a game is set out in its profile (`play/profiles/<system>/<NAME>.ini`, the format is in [PLAY.md](PLAY.md)). How it is drawn is the kit in `play/src/ui/kit.hpp`. Every screen goes through the kit, so a rule changed there changes everywhere.

## Look

1. **The launcher's look, from the launcher's settings.** Colour mode, accent colour, high contrast, the grid background, Typeface (DS rounded or Pixel classic), Icon style (Rich or Simple pixel) and Animations come from `userdata/preferences.ini` and `ui.ini`, read by the launcher's own code (`src/launcher/settings.hpp`, `accent.hpp`). Theme colours never appear as literals in a view. Game colours (a type chip, an HP bar) are fine. Their ink must still pass the contrast rule.
2. **The DS touch screen is the ground.** Each screen starts with `backdrop()`: the 20 px tiles with Grid, the faint ruling with Plain.
3. **Cards, not boxes.** Content sits on launcher cards: `card()` (the settings card) or `raised()` (a glossy pill with a rim, a gradient and its shadow). Empty places are `sunken()`, meters are `meter()`. Radius 14 for cards, 12 for icon boxes and tiles, h/2 for pills.
4. **Layout in launcher units.** 24 px side margins, 12 px gaps, 44 px icon badges, on the 640×480 panel.
5. **No screen filter.** The bridge's shaders filter the game picture only. The companion and the Info screen are interface, like the launcher, and are never shaded.
6. **Text at the baked sizes only:** 16 (small), 20 (body), 24 (title), 28 (large) and 36 (numbers). The rounded font is drawn one atlas texel to a pixel, never scaled, and that is what keeps it crisp. Pixel classic draws the launcher's 5×7 dots at whole scales for the same sizes (`launcher/pixel_text.hpp`, shared). *(lint: size)*
7. **Readable.** Text has 4.5:1 contrast against what is behind it (3:1 from 24 px), and 7:1 and 4.5:1 with High contrast on. *(lint: contrast)*
8. **Inside the glass.** Text stays 8 px clear of the panel's edges and never overlaps other text. *(lint: edge, overlap)*
9. **A badge for every number.** A board tile has an icon (`icon NAME`, the launcher's pixel glyphs plus the companion's own in `tools/build_play_icons.py`) or, better, the game's own art (`art NAME`: the game's tiles from its ROM, drawn in the palette the top screen uses). *(static lint)*
10. **Short labels.** At most 18 characters, sentence case, the game's own words ("Coin bank", "Heart pieces"). At most 8 tiles to a page and 3 pages. *(static lint)*
11. **Words the launcher can translate.** Headings, labels, page names and step names go through the launcher's catalogs (`assets/locales/*.tsv`). A word missing from `en-US.tsv` is reported. Game names, place names and item names that are proper nouns stay as the game writes them: a view lists them in `proper = A; B` and the lint leaves them be.

## Touch

12. **The lower panel is a touch screen.** Under RG DS Core the bridge reads it and sends it on (as for the DS engines). On its own, the player reads it itself (`src/platform/touch.hpp`, shared with the launcher). On a desktop the mouse stands in for a finger.
13. **Targets at least 48×48.** *(lint: target)*
14. **The corners belong to the system.** Bottom right is the dock's button, and top right is the switch between the game's own screen and its status board. Content never puts text there. Boards give up the end of a tile instead. *(lint: corner)*
15. **Nothing important on one careless tap.** Return home from the second screen is held (1 s, a ring fills). A tap only says how. The game switcher opens on a tap: it only pauses the game. Screen off is a tap, and any touch brings the screen back, doing nothing else. A dimmed screen's first touch only wakes it.

## Motion

16. **Animations follow the launcher's setting.** With Animations off everything is instant: the dock opens without its circle and fades without steps. With them on, motion is short (about 260 ms) and eases out.
17. **Low-key when left alone.** The dock's button fades to faint after 4 s. The Info screen dims to about a third after 15 s. Controls left open over the companion close after 15 s.

## Usefulness

18. **Show what the game hides.** A good companion shows what the top screen doesn't: the coin bank and treasures in a Wario Land course, the ending the money reaches so far, seashells and instruments in Link's Awakening, the Pokédex and badges in Pokémon. Don't repeat the status bar unless it takes the status bar's place (`covers_hud`). Never a dimmed copy of the game behind a HUD: a Game Boy Advance HUD becomes a board, its pieces (`hud X Y W H` tiles) in cards beside what the game keeps in memory, and only while the game is really in play, not on its menus or in its story.
19. **The right screen for the moment.** Use `on` conditions: a course board in a course, a journey board on the world map, the battle in a battle, the team otherwise. An `alternative = yes` board waits behind the switch.
20. **Facts with sources.** Every profile starts with a comment saying what the screen shows and where each address comes from (a disassembly, Data Crystal, a RAM search), and what was checked on a real save. *(static lint)*
21. **Verified, or said not to be.** Shots from `play-shot`/`tools/play_gobot.py` with a real save, and the lint's shots (`--shots DIR`) in all four looks, before a profile ships.

## The second screen's modes

The in-game menu's Graphics page sets **Second screen**:

- **Companion**: the profile's screen, with the dock's button.
- **Info**: no companion, just the dock as a quiet screen. It has the game's name, the session's time, the clock and battery, the game switcher, a held Return home, brightness, and screen off. The game keeps its whole picture, HUD and all.
- **Off**: nothing on the second screen, and the game keeps its whole picture. A HUD band or HUD split is only taken off the game while a companion shows it.
- **Swapped**: the companion on top, the game below (no touch, since the top panel is not a touch screen).

Brightness is the second screen's own, 10 to 100 %, kept in `userdata/emulators/rgds-play/second-screen.ini`. The game switcher and Return home are the runtime's own triggers beside the control file (`emulator.control.menu` and `.home`, the same the Anbernic button and the home shortcut write), so Return home saves first exactly as the menu's does. Save and Load stay in the menu, with its gallery.

A port's second screen (`rgds-launcher --port-menu`, `src/launcher/port_menu.hpp`) is this same Info screen in the launcher's drawing: same places, same holds and fades, the same brightness file. PortMaster itself keeps its install progress there instead.

## Running the lint

```
python3 tools/lint_companions.py            # static checks, then play-lint (builds it first if needed)
build/play/host/play-lint --profiles play/profiles/gb --profiles play/profiles/gba --fonts assets/fonts --shots /tmp/lint
```

`play-lint` draws every view and team, the title card and the dock against a fake core, which answers every read with zeros and then with 0x99 (the widest BCD). Each view's `on` is made true, every board page is drawn through its tabs, and all of it happens in four looks: light, dark with an accent, high contrast, and Pixel classic with simple icons and no grid. `tests/test_companion_lint.py` runs both and fails on any problem.
