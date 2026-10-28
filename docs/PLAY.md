# RG DS Play

RG DS Play (`play/`) is the player for every system that isn't the DS. It starts with the Game Boy Advance, Game Boy and Game Boy Color, and with the thing that makes it belong on a two-screen handheld: the **companion screen**.

The game runs on the top panel. The bottom panel shows the game's own map, kept up to date while you play, the way the DS Castlevanias and Metroid Prime Hunters use their second screen. Or it takes the game's HUD off the game screen, the way Mario Kart DS does: in Mario Kart: Super Circuit the laps, time, the top four and a large course map move to the bottom panel. Games without a companion profile show a title card on the bottom panel and play as usual on the top one.

> **Status, 23 October 2026: in RG DS Core, behind an option, not released.** GBA, GB and GBC games launch from the launcher through RG DS Play with the shared in-game menu (Settings → Library → Systems → All systems, off by default). It runs on the owner's RG DS as a side-by-side test release (`tools/play_device.py install`); no public package or update carries it yet. Mario Kart: Super Circuit (Europe) has been hand-played on the handheld.

## In RG DS Core

Other systems behave like DS games: the same library, the same in-game menu, the same saves and statistics. Everything that differs by system lives in one place in each layer, so a new system plugs into the same path.

- **Systems** (Settings → Library, first row, DS only or All systems; `show_systems` in `ui.ini`, off by default). Off: Home and the DS library are exactly as before, and other systems' folders are not even scanned. On: Home's **Games** opens **Systems**, one DSi-style card per system with games (the DS first, then Game Boy Advance, Game Boy Color, Game Boy) and its game count. A card opens that system's games in the usual Cartridges / Covers / List views; Back returns to Systems, then Home. **Favourites**, **Recents** and **Last played** span every system.
- **System icons:** every system has its own drawing instead of a shared cartridge, in the icon style the theme uses: the generated artwork (Rich) or the hand-drawn 16×16 mask (Simple pixel). The set covers every system of the stock Anbernic Linux card, so a system that RG DS Play opens later already has its icon. See [the icon set](../assets/systems/README.md).
- **The two-screens mark:** a game that RG DS Play gives a second screen of its own (a map, the team, the HUD) carries a small chip with two stacked screens in the bottom-left corner of its cover on the games screen, mirroring the favourite heart in the top-right. It has no text, so it needs no translations, and it is drawn on the selected game's cover only, not on the small tiles of the carousel. The launcher finds the games from the companion profiles themselves (`src/launcher/dual_screen.hpp`): a Game Boy or Game Boy Color cart is found by its maker code or, for the older carts without one, by its header title; a Game Boy Advance cart by its code; a zipped game cannot be opened to see its header and shows no mark.
- **Where games are found:** the stock card's `Roms/GBA`, `Roms/GBC`, `Roms/GB` and `rgds/games/gba|gbc|gb` (both cards), passed by `runtime/session.py` (`system_roots`) as `--system-dir`. A `gba` folder inside a DS folder is left out of the DS list.
- **Which engine:** chosen from the game's system wherever it is started (library, Last played, game switcher, a restart): `runtime/play_options.py` `engine_for()`. DS games keep their DS engines; `rgds-play` is appended to the engine list (stored indexes keep their meaning) and is never offered as a DS engine.
- **The in-game menu** is the shared one: Resume, Save and Load (the checkpoint gallery with pictures), **Graphics**, Emulator (FPS, fast-forward, power profile, Save & restart, Reset state; the DS-only rows are unavailable), Sound, Brightness, Controls, Game switcher, Return home. The bridge drives the player like any SDL engine: F5 saves the machine to the state slot, F7 loads it, Tab toggles fast-forward, quitting saves first when auto-save is on. Auto-load, quick resume, lid sleep, volume keys and playtime come from the runtime as for every engine.
- **Graphics** for RG DS Play: **Game screen** (Sharp fill · Whole pixels · Smooth fill), **Second screen** (Companion · Info screen · Off · Swap screens: the Info screen is the second screen's own quiet controls, see [COMPANION.md](COMPANION.md)) and **Frame** (Off, the game's own handheld in its shell colours, Black bar, then the owner's picture frames; see Frames below), applied live through the bridge (`play-graphics:<fit>:<second>:<frame>`, the player's exported `rgds_play_set_graphics`) and kept per game in `library/<key>/play.ini` once the player confirms; and **Shader**, the same carousel and presets as the DS engines.
- **Screen colours** (Graphics, last row; `play/src/ui/colour.cpp`, the list in `play/src/ui/gb_palettes.hpp`) is the handheld a Game Boy game is shown on. It is a colour step on the picture just before it is drawn, not a shader: it looks at one pixel at a time, so it also works on the direct KMS present, where the bridge's filters do not run. An original **Game Boy** game gets **Game Boy** (the default: the first handheld's green) · **Game Boy Pocket** · **Game Boy Light** (shades after SameBoy's measured palettes) · **Game Boy Color** · **Black and white**: its four greys take that screen's four colours. **Game Boy Color** is the colour handheld's own colours for the game: its boot ROM picks palettes for the background and both sprite sets by the cartridge header (the table mGBA carries, and the boot ROM's white, green, blue default for a game it does not list); the core draws them (`play/src/core/mgba_gb.c`, live, no restart), then the Game Boy Color screen correction below. Every Game Boy game plays on a handheld: a Super Game Boy cartridge (Pokémon Red and Blue, Wario Land II) runs as a plain Game Boy, so these screens apply to it (an older save state made in Super Game Boy mode is turned back into a Game Boy when it loads). A game that runs on the **Game Boy Color** (a `.gbc`, or a `.gb` made for both, flag 0x80 in its header, such as Pokémon Yellow) gets **Game Boy Color** (the default) · **Game Boy Color, darker** · **Full colour**: the screen's channel mixing (the weights Gambatte made common) that tames raw RGB555 on a modern panel, applied through a 32,768-entry lookup table; Full colour is the raw values. Game Boy Advance games keep the GBA screen shader's colour response. The emulator, the companion's probes and the HUD split see the game's own greys (except Game Boy Color colours, which the core draws), because the step runs on a copy. Applied live through the bridge (`play-color:<palette>:<correction>`, the player's exported `rgds_play_set_color`), kept per game in `library/<key>/play.ini` (`palette=`, `colour=`) and passed to the player as `--palette` / `--colour`. The companion screen's own drawing (a map, the party) is not coloured.
- **Frames** (`play/src/ui/frame.cpp`) are drawn by the player from shapes and the launcher's font, with no pictures of real consoles and no logos. Each system is offered only its own handhelds: one list, `play/src/ui/frame_styles.hpp`, read by the player and the launcher's Frame picker, with `runtime/play_options.py` `FRAMES` repeating its names and systems (a test compares). A frame kept for another system's game (an old GBA shell on a Game Boy game) falls back to Off, unless the owner has a picture of that name in the game's own frames folder.
  - **Game Boy**: the original (warm grey body, slate bezel with its big low-right curve, the game in its own dark window, the red and blue lines with DOT MATRIX WITH STEREO SOUND over it, the red power light with BATTERY under it, GAME BOY in the logo's slanted lettering under the bezel), also in the Play It Loud colours (Red, Yellow, Green, Blue, Black); and the **Pocket** in Silver, Black, Yellow, Red, Green, Blue and Pink (a slim dark bezel with even corners, GAME BOY pocket under it). The game as large as the bezel leaves room for (about 2.4×).
  - **Screen only** for the Game Boy, the Pocket and the Color: just the bezel, edge to edge, so the game fills the panel at exactly 3× (480×432, whole pixels) with the window, the power light and (Game Boy, Color) the lines or the name around it.
  - **Game Boy Color** in Berry, Grape, Kiwi, Dandelion, Teal and Atomic Purple (clear, its insides showing): a black bezel with the power light and GAME BOY COLOR under the screen, COLOR a colour a letter.
  - **Game Boy Advance** in Indigo, Glacier, Black and Platinum: the AGB's own wide, squat body (the dark lens in its upper half, the green power light beside the screen, GAME BOY over ADVANCE low on the left, the speaker's round holes low on the right, and the shell tapering in at its belly), the game at 2×; and the **SP** in Cobalt, Onyx and Platinum: the clamshell's lid with its slim lens, the hinge along the bottom edge and GAME BOY ADVANCE SP under the screen.
  - the **black bar**, for every system: the game across the full width at the top (640×427 for the GBA, like Sharp fill) and the system's name on a black bar below, ADVANCE in a violet-to-pink sweep, COLOR a colour a letter; the layout spruceOS uses with the "Perfect" overlays.
  - **Every other stock system** already has its frames, ready for when RG DS Play runs it (until then nothing offers them): the **Neo Geo Pocket Color** (Silver, Black) and the **WonderSwan Color** (White, Blue) as the tall portrait handhelds they are, the picture in the upper half and the D-pad, the A and B buttons and the speaker below it; the **PSP** (Black, White) with its wide lens, the D-pad, the four coloured face buttons and the speaker under the screen; each **home console** in its own colours with **its own mark** on the strip under the screen — the NES's red stripe with the blue line under it and its front badge, the Super Nintendo's purple mark on its dark panel, the Mega Drive's SEGA over the red 16-BIT band, the Master System's 16-BIT band, the PC Engine's orange band, the Neo Geo's gold band, the PlayStation's blue PS line, the Nintendo 64's wordmark with the N in colour, the Saturn's wordmark with its ringed planet, the Dreamcast's orange spiral, the Atari 2600's gold fuji, the ColecoVision's stacked mark, the Intellivision's tan badge, the MSX's mark and the PICO-8's own coloured cart; the sets their games were played on: a **grey TV** (controls under the tube) for the 8-, 16- and 32-bit consoles, a **wood-grain TV** (dials on a silver panel beside the tube) for the older ones and the NES and Master System, a beige **PC monitor** for DOS and MSX, an **arcade cabinet** (lit marquee, side art, stick and buttons) for Arcade and the Neo Geo, and the **Vectrex**'s own upright screen with its joystick. The DS and 3DS have their own engines and get none. Systems are the launcher's stock ids (`src/launcher/system_icons.hpp`); one row may serve several (`"nes,snes,..."`).
- **Picture frames** are the owner's own: any PNG in `userdata/frames/<system>/` (`gba`, `gbc`, `gb`) is offered by its file name after the built-in frames. They are laid over the game like RetroArch overlays (640×480 or any size, scaled to the panel), and the game fills their see-through part (alpha below half), which is left fully clear (an LCD grid an overlay draws there is dropped, so the Shader setting alone decides the screen's look), so the 640×480 overlays made for the Miyoo Mini and RG35XX fit as they are. None are shipped: the ones spruceOS uses, the "Perfect" overlays (`ourigen/perfect_overlays`, after 1playerinsertcoin, with bezels by mugwomp93 and drkhrse), carry no licence and Nintendo's logo, and libretro's MIT `overlay-borders` has only 16:9 photographs of the console. Frames go through `play-graphics` by name; the bridge takes the rest of the command line, so names may hold spaces.
- **Shaders** run in the bridge's GL pass as for the DS: it also takes 240×160 and 160×144 textures from `rgds-play`, and compiles the presets that draw an LCD grid with the game's own grid instead of the DS's 256×192, so each cell is one GBA or Game Boy pixel. While a shader is chosen the player draws its game texture plainly (no render target, the bridge's `rgds_shader_live`), so the pass can take that draw and do the scaling. The second screen is never shaded: the companion and the Info screen are the player's own interface in the launcher's look, and like the launcher they keep their pixels (only the game picture is filtered).
- **GBA screen** (preset 30) is what a GBA game shows while its own shader is Default: the GBA LCD's colour response (after Pokefan531's public-domain GBA model: a darker gamma, colours bleeding into each other, slightly lower brightness; darkened half as much as the unlit AGB-001 screen, since the RG DS panels are lit) and the faint gaps between its pixels, one cell per GBA pixel. **GBA Color** (29) is the colour alone. **GBA true colour** (33) is the full-strength correction for players who want the colours the artists drew for: GBA games were painted pale and washed out so they would read on the dark, unlit AGB-001 screen, and look faded on a modern panel; this preset follows the GBA colour model Talarubi and Near measured for higan (the LCD's 4.0 gamma against the panel's 2.2, the channels' bleed, slightly cool whites), not halved. **GBA true screen** (34) is that colour with the same faint pixel gaps as GBA screen. All four sit in the carousel's GBA screens group. **Game Boy screen** (31) is a Game Boy game's Default: the original dot-matrix LCD, whose gaps show the lighter backing (most of all on dark areas; on light areas gaps and pixels are the same colour) and whose dark pixels cast a faint shadow down and to the right. **Game Boy Color screen** (32) is a Game Boy Color game's Default: its faint pixel gaps. Neither changes colour: Screen colours does. A game of another system is offered only the Original presets (Off, Sharp bilinear) and its own system's screens; the other looks (DS screens, Natural, Vivid, Pixel art, Moods) were drawn for the DS picture and stay with DS games, and a DS game is offered no other system's screens. `runtime/play_options.py` (`shader_for`) keeps the same rule at launch: a GBA or Game Boy game whose choice is Default, or a look made for the DS, gets its own screen.
- **Windows:** the player gives its two windows the launcher's Wayland roles (`org.rgds.top`, `org.rgds.bottom`), so Weston puts the game and the companion on their own panels.
- **Two GL contexts, one thread:** each panel has its own window, renderer and GL context. On the RG DS (Mali, Wayland) SDL's idea of the current context drifted from EGL's, so the companion's texture uploads landed in the game window's context and the bottom panel kept its first picture (the title card, or the first HUD) for a whole race. The player now binds a panel's context for real (released, then made current) before touching it, and draws and presents each window in one go (`use_panel` in `play/src/frontend/main.cpp`). Verified on the handheld from the display controller itself, through RG DS Core's own launch, a race, the game menu and back.
- **Seeing what the panels show:** renderer readbacks and the bridge's capture read an app's own buffers, which hid this bug. `tools/scanout.c` reads the planes the display controller scans out (after Weston), so it shows exactly what is on each panel; build and run it as its header says.
- **Saves:** battery saves in `userdata/emulators/rgds-play/saves/<system>/`, checkpoints in `userdata/emulators/rgds-play/states/` and `library/<key>/auto-rgds-play.dss`, all in **Back up all saves**. The rules under Saves below apply. A checkpoint is mGBA's own state file: the machine, then an extra-data block with the cartridge RAM (no clock metadata, so the same moment always gives the same bytes). mGBA's bare machine state leaves that RAM out, and a game that keeps its working variables there (Super Mario Land 3) hung when the auto-load put the machine in before the first frame, beside the battery's RAM from another moment. Loading puts the RAM into memory only: the battery on the card follows when the game next writes it, so an older checkpoint never rolls an in-game save back by itself. Checkpoints made before 26 October 2026 are the machine alone and still load.

### Trying it on the handheld

```sh
python3 tools/build.py device && python3 tools/build_play.py device
python3 tools/play_device.py install     # a test release beside the installed one, then RG DS Core restarts
python3 tools/play_device.py rollback    # back to the release that was active before
```

`install` copies the active release to `releases/<build>-play<hash>`, replaces what `tools/package.py` would replace from this tree (launcher, bridge, runtime, assets, themes, `emulators/rgds-play`), writes the copy's manifest, verifies it and activates it with the release's own `activate.py`. The installed release is never modified, and `userdata` keeps every save and setting. A staged over-the-air update is not touched: it can only be installed while its own base release is active.

## How the companion finds the map

The idea comes from kyannon's [GBA DS](https://github.com/kyannon/gba-ds), which showed it works. RG DS Play is our own implementation of it.

1. **A shadow copy.** Next to the live game, the player keeps a second, silent copy of the same game: its own memory, no sound, no battery file.
2. **A snapshot, never a pause.** When it's time to look, the live game's state is copied (about 400 KB, well under a millisecond), and the shadow loads the copy. The game you're playing is never paused, slowed or written to. `tests/test_play.py` checks this frame by frame: an 800-frame run gives the same picture hash for every frame with the companion off, on in lockstep, and on racing the game as it does on the handheld.
3. **Open the map in the shadow.** The shadow lets go of every button for two frames (so a button the player is holding still reads as a fresh press), then plays the profile's `open` buttons.
4. **Trust, but check.** A capture is accepted only when the profile's memory condition says the map is up, its pixel probes match, and the picture has held still for `stable` frames. Then a few more frames are recorded so the player marker keeps blinking. A blank map is rejected. A failure keeps the last good map on screen.
5. **When to look:** every `interval` frames (1.5 s by default); 10 frames after a `room` value changes; 12 frames after a fade to black ends (a room transition); and only while `live` matches (in gameplay, not in a menu). Back on the title screen (`menu`), the old map is dropped.

On the bottom panel the map is drawn at the largest whole scale that fits, on the map's own background colour from edge to edge, so it reads as the game's second screen rather than a picture in a box. A change bigger than the marker moving cross-fades over 12 frames. The panel is redrawn only when what it shows changes.

**Cost on the RG DS:** with the test cartridge, a capture took about 80 ms of shadow emulation on a worker thread at `nice 10`. At one capture per 1.5 s that is about 5% of one A55 core; the live game's thread is untouched. Real games open their map more slowly, so measure each new profile.

## How the HUD moves

The live core gets a second mGBA software renderer (`play/src/core/mgba_split.c`). It is told about every register, palette, VRAM and sprite write the game makes, so it always has what the game's own renderer has, but it draws only the HUD layers (the profile's BG layers, and the sprites of one priority, through filtered copies of sprite memory), on a key colour where there is no HUD. While the profile's `on` condition holds, the game's renderer leaves those layers out. Only drawing changes: over 5,401 frames of Super Circuit (title, menus, a race, the split switching on), the emulator's whole state hashed the same every frame with the split and without it. `tests/test_play.py` checks this on the test cartridge always, and on Super Circuit when `RGDS_PLAY_ROMS` points at a folder with your dump.

The second screen is laid out by the profile: each `panel` box of the HUD gets a slot and is drawn at the largest whole-pixel scale that fits it. `trim` cuts a box down to what the HUD has drawn in it this race, so each course's map fills its slot whatever its shape. Behind the HUD is the game's own scenery, drawn by a silent copy with only the scenery layers (no karts, no Lakitu, no HUD) every ten seconds on its own thread. `to_game` boxes go back onto the game screen where the player looks (Super Circuit: the item box and your place).

On the Game Boy the HUD is usually a band of whole rows: the window layer at the bottom (Link's Awakening, Metroid II) or a status bar kept at the top (the Oracle games). A `[hud]` with `strip = 0 y w h` needs no second renderer: while `on` holds, those rows are cut from the game picture (the top panel shows a shorter picture, at a larger scale) and drawn at a whole scale along the bottom of the second screen, under the map, team or view. With a screen filter on, the top panel keeps the whole picture, since the bridge shades only full-size pictures.

**Cost on the RG DS, Super Circuit racing (600 frames):** emulation 4.9 ms a frame alone, 5.3 ms with the split; drawing both panels in software 2.5 ms (the windowed player draws the top one on the GPU). About 8 ms of a 16.7 ms frame.

## Profiles

A profile is a text file in `play/profiles/<system>/`, picked by the game code in the ROM header. Profiles are data: adding a game, or fixing one, needs no rebuild. Game Boy and Game Boy Color profiles live in `profiles/gb/`; carts older than the Game Boy Color carry no game code, so their profiles name the header title instead, upper case with `_` between words (`POKEMON_BLUE`, `METROID2`).

```ini
[game]
title = Metroid Fusion
codes = AMTE                    # one or more 4-letter game codes

[companion]
live = mem8[03000BDE] == 01     # in gameplay: captures make sense
menu = mem8[02000000] == 0      # title/file screens: forget the map
room = mem16[02000002]          # values that change on entering a new room
interval = 90                   # frames between routine captures
dim_when_unavailable = no
note = The time and your gold appear once you're farming   # what the second screen will show

[map]
release = 2                     # frames with nothing held before `open`
open = START:4                  # buttons:frames, "-" for nothing, B+UP for both
timeout = 180                   # frames to wait for the map
stable = 3                      # frames in a row that barely change
max_diff = 640                  # pixels that may change between them (blinking)
ready = mem8[03000BDE] == 03    # memory says the map is up
pixels = 0,0=#9C9C9C 239,0=#9C9C9C
pixels_min = 0                  # how many probes must match (0: all)
tolerance = 8
background = #9C9C9C            # the map's empty colour (default: the most common edge colour)
min_foreground = 12             # pixels that must differ from it
crop = 0 0 240 160              # x y w h kept
anim_frames = 8                 # frames recorded for blinking markers
anim_step = 4
layers = 1F                     # BG0-3 and sprites drawn in the capture

[hud]
on = mem16[04000000] == 1F41    # the HUD moves to the second screen
keep = mem16[04000000] == 1F41  # the last HUD stays up (paused)
bg = 1                          # BG layers that are HUD (bit 0 = BG0)
sprites = 0                     # sprites of this priority are HUD (none: no sprites)
backdrop = game                 # behind the HUD: the game's scenery, theme (the second screen's own ground), or #RRGGBB
backdrop_layers = E             # drawn by a silent copy with only these layers
backdrop_rows = 76              # only the rows above the horizon
moves_when = mem16[06003808] == C044   # the layout below applies (racing, not results)
to_game = 104 0 33 29 > 4 4     # a HUD box put back on the game screen at X Y
panel = 140 64 100 96 > 136 84 488 380 trim   # HUD box > slot on a 640x480 panel
sprite_tiles = 256-295 0        # sprites by first tile (and priority): HUD too
window_effects_off = 1          # no shade inside WIN0 (1) / WIN1 (2) on the game screen
bg_priority_below = 2           # HUD = the BG layers below this priority (games that move menus between layers)
map_cnt = 4C03                  # on only while some BGxCNT holds this (the map's layer)

[party]
format = pokemon3               # pokemon1/2/3 (Red/Blue, Gold/Silver/Crystal, Ruby to LeafGreen), goldensun, saga1
count = 03004350                # the member count (pokemon3) or party flags (goldensun)
slots = 03004360                # the first member
icons = 083BBD3C                # ROM icon table, palette indices and palette table (pokemon3, optional;
icon_palette_index = 083BC41C   # checked against the palette table's tags, so another revision
icon_palettes = 083BC5D4        # shows no icons rather than noise)
names = 1:DE41                  # pokemon1/2: nicknames; dex_order and icon_kinds: their ROM tables

[view]
format = metroid2               # a second screen the player draws itself (companion/view.hpp):
on = mem8[FF9B] == 04 | 08      # metroid2, board or pokemon_battle; shown while `on` holds
level_bank = D058               # the format's own keys, mostly addresses

[map.dungeon]                   # another capture, used instead of [map] while `when` holds
when = mem8[DBA5] == 01 and mem8[FFF7] in 00..08
open = START:4                  # the same keys as [map]

[hud]                           # Game Boy: a band of whole rows instead of layers
on = mem8[DB95] == 0B and mem8[FF4A] == 80
strip = 0 128 160 16            # x y w h: full width, at the top or the bottom
```

Game Boy addresses can name a bank: `1:DCD7` is work RAM bank 1 (Game Boy Color games switch banks mid-frame, so the bus alone reads the wrong one half the time), `1C:590D` is ROM bank 1C. They work in conditions (`mem8[1:DCD7] == 01`) and in every address key.

The silent copy that captures maps starts with the live game's battery memory, and gets it again whenever the game saves: the European Oracle games keep their language there, and draw garbage without it.

With a `[party]` section and no map or HUD on show, the second screen draws the team live from memory: one card per member with its icon, name, level, an HP bar (and PP for Golden Sun) and status.

Two `[view]` formats serve any Game Boy game:

- **`board`**: a status board of numbers the game keeps in memory, drawn as launcher cards: `tile1 = Coins | bcd A97A 2 art coin`, in number order. Kinds are `bcd ADDRESS BYTES` (two digits a byte, the first byte the highest; `low` for a little-endian word of BCD), `num ADDRESS SIZE` (little-endian; `big`, `mask HEX`) and `bits ADDRESS BYTES` (how many are set, up to 32 bytes for a Pokédex). `count ADDRESS BYTES is HEX` counts the bytes that hold one value (one kind of item in an inventory of slots). `div N` divides (health in quarters), `plus N` adds (lives kept one less than shown), `of N` adds a meter (`max ADDRESS` reads its most from a byte: health whose most grows) (for bits up to 16 with an icon: a row of them), and `wide` gives the tile a row of its own. Every tile has a badge: `icon NAME` (`play/src/ui/icons.hpp`) or `art NAME`, the game's own tiles from its ROM (`art.coin = gb 11:73F0 1x1`), drawn in the palette the top screen uses. `page NAME` puts tiles on pages, shown as tabs in the header and chosen by touch. `tiers` turns the value into named steps from `tiersN = 300 Birdhouse; 10072 Tree trunk` with a meter to the next, and `names` does the same for plain names (a sword's level, day or night). `heading` and `heading_art`/`heading_icon` title it, `proper = A; B` lists the game's own names (which stay untranslated), and `covers_hud = yes` shows it in place of a `[hud]` strip's band rather than above it (the band still leaves the game picture).
- **Boards over a Game Boy Advance HUD split:** `Label | hud X Y W H` is a tile of the HUD itself, what the split draws in that box of the game screen (a copy ability's name, a row of hearts, a coin counter), trimmed to what is drawn and as large as the tile takes it, and `art.NAME = hud X Y W H` makes a badge of it. With `covers_hud = yes` such a board takes the HUD picture's place on the second screen. It claims only its boxes: whatever else the HUD layer draws (dialogue, a boss's name and health, a message, the buttons) goes back onto the game screen, so nothing the game says is lost. The board waits until the HUD draws something in its boxes, so a cutscene that hides the HUD leaves the second screen quiet.
- **Several views:** `[view.<name>]` sections beside `[view]`, and the first whose `on` holds is shown (Wario Land: Super Mario Land 3 has a course board and a world-map board). A view with `alternative = yes` waits behind the **switch** (top right): a tap flips between the game's own screen (the map, the team) and that board, and with nothing else to show it shows by itself. Link's Awakening, the Oracles and Wario Land 3 have status boards there, and Pokémon Red, Blue, Yellow and Crystal have trainer cards.
- **The look and the rules** are in [COMPANION.md](COMPANION.md): the launcher's Theme (passed as `--preferences`), its language, touch, the dock, and the lint that checks every profile.
- **`pokemon_battle`**: the Game Boy Pokémon games' battle: the opponent (icon, name, level, types, HP) and yours with its four moves (type, power, PP left of its most, PP Ups counted), names from the cartridge's own tables. Keys: `generation` (1 or 2), the battle structs `player` and `enemy` and their nicknames `player_name` and `enemy_name`, the ROM tables `move_names`, `moves` and `type_names`, and the `[party]` icon keys. A view that shows comes before the team, so the battle replaces the team panel until it ends.

- **`mosaic`**: a map of everywhere the player has been, made from the game's own picture: every few frames the playfield (`crop`, in the `native` frame; a core that crops overscan shows its middle) is pasted, `scale` world pixels to a map pixel, where the expressions `x` and `y` say the frame's top left is in the world, one map per `area` value, the player (`player_x`, `player_y`) blinking on it. `when` holds off pastes mid-scroll. The whole area fits the panel at the largest whole zoom, or a big one follows the player. Kept beside the save (`RGMS`). For games with no map (Metroid) or one in a menu (Zelda's levels, Super Metroid). Checked on Metroid II through mGBA.
- **`grid`**: an automap for a first-person dungeon kept in memory as squares (Phantasy Star's 16x16 floors): `cells`, `position` (y high nibble, x low), `facing` (0 up, 1 right, 2 down, 3 left), `level`, `type_mask` and the hex kinds `open`, `wall`, `up`, `down`, `door`. A square is drawn once the player has seen it: the corridor ahead (`sight` squares, stopped by anything not open) and the squares beside each step, as the game draws them. Kept per level (`RGGR`).
- **`trace`**: a course map from where the player has driven (F-Zero keeps no map, only the machine's place): the first lap draws the road, later laps run on it. `x`, `y`, `course`, `when`. Kept per course (`RGTR`).
- **Expressions** (`play/src/companion/expr.hpp`): the numbers these views read are hex numbers and `mem8/mem16/mem32[ADDRESS]` with `+ - * / << >> & |` and parentheses, C's precedence: `x = (mem8[EB] & F) * 100`.
- **`strip_fill = yes`** in `[hud]`: the band is a second picture of its own (Super Mario Kart's course map under the race) and fills the second screen's width instead of sitting in a card: grown to the next whole size, then smoothed down to the fit, as the game screen's sharp fit does. **`game_rows = N`** keeps only N rows on the game screen while the band is off, so a gap between the two pictures (Super Mario Kart's black rows 100-107) is on neither screen. The board's `clock` word shows a two-byte num (minutes, then hours) as 18:05.
- **Light guns:** `lightgun = yes` in `[game]` makes the second port the core's light gun (the NES Zapper in FCEUmm): the game starts on the touch screen (swapped) and a finger aims and fires there, the trigger held four frames so the game sees the shot.

A `[map.<name>]` section is a second screen captured instead of `[map]` while its `when` holds; the first that holds wins. Link's Awakening captures the island map with SELECT outdoors and the dungeon's floor map from the inventory (START) inside one.

Conditions are `memN[address] [& mask] == a | b`, `!= a`, or `in lo..hi`, joined with `and`; addresses and values are hex. A parse error names the file and line, and that profile is skipped; the game still plays.

Shipped today:

| Code | Game | State |
| --- | --- | --- |
| `ZRDT` | RG DS Play test cartridge | Tested end to end, on the Mac and the RG DS |
| `AMTE` `AMTP` | Metroid Fusion | Map (START) on the second screen. Run on the European dump |
| `A2CE` `A2CP` | Castlevania: Aria of Sorrow | Castle map (SELECT). Run on the European dump |
| `AMKE` `AMKP` `AMKJ` | Mario Kart: Super Circuit (USA, Europe, Japan) | HUD on the second screen while racing; the pause menu, lap times, results and TRY AGAIN / NEXT RACE stay on the game screen, with the race's last HUD kept below. Whole races driven headless with `tools/play_drive.py` (with and without items) and replayed through the real player on the RG DS |
| `A2CJ`, `AMTJ` | Aria of Sorrow and Metroid Fusion (Japan) | GBA DS's facts, Japanese addresses shifted as GBA DS does; not yet run |
| `ACHE` `ACHP` `ACHJ` | Castlevania: Harmony of Dissonance | Castle map (SELECT). Run on the European dump (the Japanese release not yet run) |
| `AAME` `AAMP` `AAMJ` | Castlevania: Circle of the Moon | Map (SELECT). Run on the USA dump |
| `BMXE` `BMXP` `BMXJ` | Metroid: Zero Mission | Map (START); facts from GBA DS, not yet run |
| `AZLE` `AZLP` `AZLJ` | The Legend of Zelda: A Link to the Past | World map (L) outdoors. Run on the European dump (the dungeon maps not yet) |
| `AA2E` `AA2P` | Super Mario Advance 2 | In levels a board of the status bar's pieces (lives, bonus stars, time, coins, score), each the game's own drawing in a card; the item box and message boxes stay. The SELECT level list on the world map. Run on the European dump |
| `AX4E` `AX4P` | Super Mario Advance 4 | In levels a board of the status line's pieces with the items carried on a second page; the P-meter stays. On the world map the world, lives, coins and the inventory counted by kind (Data Crystal's USA RAM map, which holds for the European release). Run on the European dump; the inventory checked by poking items |
| `AXVE` | Pokémon Ruby | Live team panel with the game's own icons. Run with a team on Ruby (Rev 2) |
| `AXPE` | Pokémon Sapphire | Live team panel, no icons (Ruby's layout; no Sapphire dump to find its icon table) |
| `BPEE` | Pokémon Emerald | Live team panel with icons; team addresses from pokeemerald, icon tables found in the dump; not yet run with a team |
| `BPRE` `BPGE` | Pokémon FireRed and LeafGreen | Live team panel (icons on FireRed Rev 1); team addresses from pokefirered; not yet run with a team |
| `B24E` `B24P` | Pokémon Mystery Dungeon: Red Rescue Team | The floor map and the status line on the second screen in dungeons. Run on the European dump |
| `AGSE` `AGSP` | Golden Sun | Live party panel: name, level, HP and PP. Run on the USA/Europe dump |
| `AWRE` `AWRP` | Advance Wars | The commander and terrain/unit panels on the second screen on the battle map. Run on the European dump |
| `AW2E` `AW2P` | Advance Wars 2: Black Hole Rising | The same panels as Advance Wars. Run on the European dump (the first map) |
| `AGFE` `AGFP` | Golden Sun: The Lost Age | Live party panel as Golden Sun. Run on the USA/Europe dump (Felix and Jenna) |
| `AWAE` `AWAP` | Wario Land 4 | In levels a board of the game's own hearts, heart gauge and coin counter, drawn large; the pause screen stays on the game screen. The jewel pieces, CD and Keyzer are not found yet. Run on the USA/Europe dump |
| `AE7E` `AE7X` | Fire Emblem | Map windows, menus with the portrait and battle stats on the second screen. Run on the European dump (the prologue) |
| `AFXE` `AFXP` | Final Fantasy Tactics Advance | Unit panel with portrait, dialogue and menus on the second screen in battles (GBA DS's tile ranges, no ROM patch), on the second screen's own ground. Run on the European dump |
| `B8KE` `B8KP` | Kirby & the Amazing Mirror | A board in place of the HUD: the copy ability as the game writes it, health against its most, lives, the phone's battery, the Mirror shards (not checked on a save with any), and a Friends page with the three helper Kirbys' abilities and health. Boss names and bars stay on the game screen. Run on the European dump in the first area |
| `BZME` `BZMP` | The Legend of Zelda: The Minish Cap | A board in place of the HUD: the hearts and rupees as drawn, heart pieces, bombs, arrows; a Collection page with Mysterious Shells, figurines, Kinstone fusions and enemies defeated (zeldaret/tmc's save layout; only health checked in play). The buttons and dialogue stay on the game screen; the menus and the story no longer count as play. Run on the European dump in Link's house |

Game Boy and Game Boy Color (`profiles/gb/`):

| Code | Game | State |
| --- | --- | --- |
| `POKEMON_BLUE` `POKEMON_RED` | Pokémon Red and Blue | Live team panel with the party icons in each species' Super Game Boy colours; in a battle, the battle panel (the opponent, your Pokémon's moves and PP). Team run on Blue (USA, Europe); the battle panel run on Yellow's layout |
| `BYTE` | Pokémon Crystal | Live team panel with each species' own icon in its normal or shiny colours, read from work RAM bank 1; in a battle, the battle panel. Run with a team on Rev 1; a battle set up in memory with `poke` (play-shot --drive) |
| `AAUE` `AAXE` | Pokémon Gold and Silver | As Crystal (team and battle panels), addresses from pokegold's symbols; not yet run (no dumps here) |
| `AZLE` `AZLP` `AZLJ` | Link's Awakening DX | Status bar under the island map (SELECT) or, in a dungeon, under its floor map from the inventory (START): rooms seen, chests and boss with the map and compass, keys. Run on Rev 2 (USA, Europe); Tail Cave reached by setting the house door's warp (wWarp0, D401) to category 1, map 00, room 17. The name entry is confirmed with START, not A; then the PLAYER SELECT screen picks the file |
| `AZ7E` `AZ7P`, `AZ8E` `AZ8P` | Oracle of Seasons, Oracle of Ages | Status bar under the map SELECT opens, recaptured as Link changes rooms. Run on Seasons and Ages (Europe) with saves made in the probe (Ages' flag is bit 2 of C6D1, its room CC30). The state byte C2EE is the game state (2 or 3 in play, 0 in the story and the menus); the status bar sits in the top 16 rows. START and SELECT only buzz until the game sets GLOBALFLAG_INTRO_DONE (bit 2 of C6CB in Seasons); the map fades from white, so a capture waits 24 unchanged frames |
| `METROID2` | Metroid II: Return of Samus | An explored-area map drawn from the cartridge's level data as Samus goes (kept beside the save), with the status bar under it. Run on the World release, on the Mac and the RG DS |
| `AW8A` | Wario Land 3 | The level's keys, treasure chests, musical coins and coins from the pause screen, recaptured when the coins (CA04), keys (CA5B) or musical coins (CA5C) change: the progress WRAM starts at CA00 after wROMBank (C5FF) and the room block tiles. Run on the World release |
| `SAGA` | The Final Fantasy Legend | Live party panel: name, race and HP. Run on the World release (one member) |
| `HARVEST_MOON_GB` | Harvest Moon GB | The time and gold box (SELECT) on the second screen all day, recaptured when the gold (CCCA) changes and every ten seconds for the hour. Run on the European release |
| `POKEMON_YELLOW` | Pokémon Yellow | As Red and Blue (team and battle panels), with the party a byte lower and Pikachu's own icon (Yellow's icon table has 30 copies, not 28). Run with a team and a battle on Yellow (USA, Europe) |
| `SUPERMARIOLAND3` | Wario Land: Super Mario Land 3 | In a course, a board in place of the status bar: coins, time, lives, hearts, the treasures found (of 15) and the coin bank, with the game's own status-bar tiles as badges, and a Worlds page with the courses cleared per world and the ending the bank reaches so far. On the world map, the journey: lives, hearts, bank, treasures, the ending, and the worlds. The game keeps its work RAM in the cartridge's (A000-BFFF): A8C3 is 01 on the map and 03 in a course; the status bar's routine (0CC5), the save block (A804-A817) and Data Crystal's RAM map give the rest. Run on the World release; the course bits were read from Data Crystal and drawn with poked values, not cleared in play |
| `GOLF` | Golf | The hole's overview (B) on the second screen while you line up a shot, recaptured after each stroke (C487); the swing meter stays on the game screen. FFB5 is the screen: 02 looking, 03 the overview, 06 aiming and in flight. The scorecard (START) stays a button away: one picture fits, and the hole is the one you need while aiming. Run on the World release |
| `WARIOLAND2` | Wario Land II | A board in a level: the coins carried (C50E-C50F) and the coin total (C50B-C50D, added at 1D18 with a 99999 cap), which the game shows only between levels. C002 is the mode, 06 in a level. No disassembly: found by playing and from the ROM. Run on the USA/Europe release |
| `AWQE` | Dragon Warrior Monsters | Live party panel: each monster's name, species (from the ROM's name list), level, HP and MP. The monster storage starts at CAC1 and the game puts the party at its front (551D0), with a byte per place at C8C4 (FF empty). Drawn from monsters written into memory; not yet run with one caught in play |

### NES, SNES and Master System

The libretro systems find their profiles in `play/profiles/nes`, `snes` and `sms` (the runtime passes all five folders). A game is known by its cartridge's CRC-32 without a dumper's header (`CRC_3FE272FB`), by a SNES cartridge's own name (`SUPER_METROID`), or by its file's name made a code (`Legend of Zelda, The (USA)` → `LEGEND_OF_ZELDA`); the launcher's two-screens mark finds them the same way (`src/launcher/dual_screen.hpp`). `peek` reads the system's own addresses: the core's memory map where it gives one, else an NES's 0000-07FF and cartridge RAM at 6000, a SNES's work RAM at 7E0000-7FFFFF, a Master System's at C000-DFFF. FCEUmm runs with eight rows of overscan off the top and bottom (256x224) so profiles can name rows.

| Profile | Game | What the second screen shows |
|---|---|---|
| `nes/ZELDA` | The Legend of Zelda | The status bar (top 56 rows) and a `mosaic` of Hyrule and each level, screen by screen (00EB); the switch has a board (rupees, keys, bombs, Triforce, sword) |
| `nes/METROID` | Metroid | A `mosaic` per area from the map position (0050, 004F) and scroll (00FD, 00FC); a board of energy tanks and missiles |
| `nes/CASTLEVANIA2` | Castlevania II: Simon's Quest | A board: the time of day (0085-0086 as a clock), days, day or night, hearts, level, experience. A map waits for the byte that names the room |
| `nes/DUCKHUNT` | Duck Hunt | Light gun by touch; a board of score, round and shots |
| `snes/SUPER_MARIOKART` | Super Mario Kart | The lower picture (the course map or rear view, rows 108-223) fills the second screen's width in a race (`strip_fill`); the top screen keeps the race alone (rows 0-99, `game_rows`), without the black gap between the two. Split measured on the handheld |
| `snes/F_ZERO` | F-Zero | A `trace` of the course per track; a board of power and time |
| `snes/SUPER_METROID` | Super Metroid | The status bar (top 32 rows) and a `mosaic` per area from the room's square (7E07A1/3) and the camera (7E0911/5); a board of energy and ammunition |
| `sms/PHANTASY_STAR` | Phantasy Star | A `grid` automap of each dungeon floor (CB00, C30C, C30B, C30D, C202 = 0B); a board of the party's HP and meseta |

**Not run on a cartridge yet.** The addresses come from Data Crystal's RAM maps and the Phantasy Star disassembly (SMS Power!, maxim-zhao/psrp); none of the eight has been played through these profiles, and FCEUmm, Snes9x and PicoDrive have not been run on the Mac. Super Mario Kart's race condition (lap byte 7E10C1 in 7F..84) and split row (112) are the least certain.

### Playing a Game Boy game headless

`tools/play_gobot.py` drives a Game Boy or Game Boy Color game from the console through RG DS Play and looks at the screen while it plays, so a profile's facts can be found without a human at the handheld. It tells the menus from the title screen from the cutscenes from the world, presses the right button for each, reads the game's memory and writes the picture after every step.

    .cache/venv/bin/python tools/play_gobot.py --rom GAME.gbc --profile gb --script steps.txt --out shots/

`poke ADDRESS BYTE...` (play-shot --drive, and so the scripts) writes bytes into the game's memory, to set a scene up that would take hours to play to: a battle, a dungeon's map and compass. The player itself never writes.

The Game Boy games need their opening played out: the Oracle games open with an attract demo, so the way in is the title (START), the language, the file, NEW GAME and the name (a letter, then down to the bottom row and right to OK), then the message-speed box and the story. Link's Awakening DX takes its name with START and then the PLAYER SELECT screen.

### Not done yet

- **Run on real dumps:** Pokémon Gold and Silver (no dumps here); Dragon Warrior Monsters and Super Mario Land 3's treasures with real ones caught and found (both read from the ROM's own code).

## Saves

Rules, enforced in `play/src/app/storage.cpp` and tested in `play/tests/play_tests.cpp` and `tests/test_play.py`:

- **Its own folder.** Everything lives under `userdata/emulators/rgds-play/` (`userdata/play/` for the standalone tools). The DS library, its saves, playtime, statistics and settings are never read or written; a test fills a fake `userdata/` with DS files and checks every byte and timestamp afterwards.
- **Atomic writes.** A battery save is written to `<name>.sav.writing`, fsynced, then renamed over the old file. A power cut or a lid sleep mid-write leaves the previous save. Writes happen on their own thread, at the moment mGBA reports that the game's write has settled.
- **Backups.** Each start keeps the previous save as `backups/<name>.sav.1`, `.2` and `.3`, skipping a rotation when nothing changed.
- **An unreadable save stops the start.** The game is not started over it, which would replace it.
- **Import, never move.** With no save of its own yet, the player looks next to the ROM (`<name>.sav`, `<name>.srm`) and in any `--save-source` folder (stock RetroArch's), copies the first one it finds, and leaves the original alone.
- **Signals save too.** SIGTERM, SIGINT and SIGHUP leave the loop through the same save path as quitting.

Layout: `<data>/saves/<system>/<rom name>.sav` and `<data>/saves/<system>/backups/`, where `<data>` is `userdata/emulators/rgds-play` under RG DS Core.

## Building and testing

```sh
python3 tools/build_play.py host --fetch   # libmgba (pinned) + rgds-play, play-shot, play-tests
python3 tools/build_play.py device         # aarch64 / glibc 2.17; needs `make build-engines` once
build/play/host/play-tests                 # unit tests
python3 -m unittest tests.test_play        # end to end on the test cartridge (needs Zig)
```

The test cartridge (`play/tests/testrom/`) is a 6 KB homebrew GBA game built with Zig: a title screen, twelve rooms, fades between them, a map screen on SELECT with a blinking marker, and an SRAM save. It carries no Nintendo logo or BIOS data.

`play-shot` runs a game headless from a script and writes both screens as one picture:

```sh
build/play/host/play-shot --rom GAME.gba --data /tmp/play --profiles play/profiles/gba \
  --fonts assets/fonts --script script.txt --out shots/
```

Script lines are `<frame> <buttons>` (held until the next line), `<frame> shot <name>`, `<frame> hudshot <name>` (the raw HUD layer), `<frame> capture`, `<frame> status` and `<frame> quit`. Captures finish before the next frame, so a run is the same every time; `--async` lets them race the game as on the handheld. `--hashes FILE` writes a hash of every frame's picture, `--state-hashes FILE` of the whole emulator state, and `--bench` times emulation and drawing per frame.

`rgds-play` is the windowed player: one window per panel when there are two displays, otherwise both panels stacked in one window. On the RG DS it reads the buttons straight from the pad's evdev device and grabs it, so nothing else receives or queues them while a game runs; elsewhere the keyboard and SDL game controllers work. Home, or Select+Start held for a second, saves and quits (`quit:` in its log says which). The game screen defaults to `--fit sharp`: the full panel width (640×427 on the RG DS), scaled up by a whole number on the GPU without smoothing and down to the panel with it, so pixels stay crisp and even; `--fit integer` keeps whole multiples (480×320), `--fit fill` is plain nearest-neighbour. These become Graphics options in the player's menu.

To hand-play on the handheld before the player is in a release:

```sh
python3 tools/build_play.py device
python3 tools/play_device.py deploy                      # to /mnt/mmc/rgds-play
python3 tools/play_device.py run "/mnt/mmc/Roms/GBA/Game.gba"
python3 tools/play_device.py log                         # status, stop
```

Everything lives in `/mnt/mmc/rgds-play` (the player, profiles, fonts, and saves in `data/`). The installed release is only read: the runner borrows its engine environment and its mixer-safe pause (`stop_outside_mixer`), pauses the launcher or the stock menu while the game runs, and resumes it when the player exits, however it exits. It refuses to start while another engine or benchmark is running. First hand-played on 23 October 2026: Mario Kart: Super Circuit on both panels, from the stock menu.

### Testing a profile over whole races

`tools/play_drive.py` races Super Circuit from a checkpoint with no one at the controls: `play-shot --drive` takes commands on stdin, and the script steers the player's kart along the trails the computer karts drive, printing every change of the HUD's registers and the split with a shot of both screens. Its `inputs.txt` replays the same race through the real player (`RGDS_PLAY_INPUT`, with `RGDS_PLAY_SHOTS`, `RGDS_PLAY_SHOT_EVERY` and `RGDS_PLAY_QUIT_AT`), on the Mac or on the handheld. The player logs each change of the HUD with the profile's probes (`rgds-play: frame N, HUD … (mem16[…]=…)`), so a device log says why the HUD moved.

## Layout of `play/`

| Path | What |
| --- | --- |
| `src/core/` | The system-neutral `Core` interface, the libmgba core (GBA, GB, GBC) and its HUD split |
| `src/companion/` | Profiles and conditions, the shadow capture engine, the second-screen logic, the team panels (`party*.cpp`) and the player's own views (`view.cpp`; `automap.cpp` for Metroid II, `board.cpp` for status boards, `battle_gb.cpp` for the Pokémon battles) |
| `src/app/` | `Session` (one game: core, save, companion) and save storage |
| `src/ui/` | CPU canvas, the launcher's rounded font (RGF1 atlases), panel layout, BMP output |
| `src/frontend/` | `rgds-play` (SDL2) |
| `src/tools/` | `play-shot` |
| `profiles/` | Companion profiles per system |
| `tests/` | Unit tests and the test cartridge |

mGBA (MPL-2.0) is built from a pinned commit (`tools/build_play.py`) and linked statically. The handheld build uses mGBA's bundled zlib and the firmware's own SDL2.

## What comes next

In rough order. Each step keeps the rules above.

1. **Real games.** Measure the capture cost of the map games (Metroid Fusion, the Castlevanias, A Link to the Past) on the A55. Covers for other systems come from the Libretro thumbnails.
2. **Every other system through libretro.** A libretro host in the same player, using the cores already on the stock card (`/oem/retro/cores`). The companion code talks only to `Core`, so the shadow-copy map works for any core that can save state (a separate process, since one libretro core can't be loaded twice in one process).
3. **More of the second screen.** More HUD games on the GBA (Super Circuit's results and podium screens; Super Mario Advance's status bar). On the SNES, snes9x's layer switches let a second copy draw only the HUD, which is how Super Mario Kart's map and race status can get the bottom screen too. Touch: pan and zoom the map, tap to drop a pin. For every game: box art, playtime, save states with pictures.
