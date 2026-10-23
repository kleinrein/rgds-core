# RG DS Play

RG DS Play (`play/`) is the player for every system that isn't the DS. It starts with the Game Boy Advance, Game Boy and Game Boy Color, and with the thing that makes it belong on a two-screen handheld: the **companion screen**.

The game runs on the top panel. The bottom panel shows the game's own map, kept up to date while you play, the way the DS Castlevanias and Metroid Prime Hunters use their second screen. Or it takes the game's HUD off the game screen, the way Mario Kart DS does: in Mario Kart: Super Circuit the laps, time, the top four and a large course map move to the bottom panel. Games without a companion profile show a title card on the bottom panel and play as usual on the top one.

> **Status, 23 October 2026: in RG DS Core, behind an option, not released.** GBA, GB and GBC games launch from the launcher through RG DS Play with the shared in-game menu (Settings → Library → Show all systems, off by default). It runs on the owner's RG DS as a side-by-side test release (`tools/play_device.py install`); no public package or update carries it yet. Mario Kart: Super Circuit (Europe) has been hand-played on the handheld.

## In RG DS Core

Other systems behave like DS games: the same library, the same in-game menu, the same saves and statistics. Everything that differs by system lives in one place in each layer, so a new system plugs into the same path.

- **Show all systems** (Settings → Library, first row; `show_systems` in `ui.ini`, off by default). Off: Home and the DS library are exactly as before, and other systems' folders are not even scanned. On: Home's **Games** opens **Systems**, one DSi-style card per system with games (the DS first, then Game Boy Advance, Game Boy Color, Game Boy) and its game count. A card opens that system's games in the usual Cartridges / Covers / List views; Back returns to Systems, then Home. **Favourites**, **Recents** and **Last played** span every system.
- **Where games are found:** the stock card's `Roms/GBA`, `Roms/GBC`, `Roms/GB` and `rgds/games/gba|gbc|gb` (both cards), passed by `runtime/session.py` (`system_roots`) as `--system-dir`. A `gba` folder inside a DS folder is left out of the DS list.
- **Which engine:** chosen from the game's system wherever it is started (library, Last played, game switcher, a restart): `runtime/play_options.py` `engine_for()`. DS games keep their DS engines; `rgds-play` is appended to the engine list (stored indexes keep their meaning) and is never offered as a DS engine.
- **The in-game menu** is the shared one: Resume, Save and Load (the checkpoint gallery with pictures), **Graphics**, Emulator (FPS, fast-forward, power profile, Save & restart, Reset state; the DS-only rows are unavailable), Sound, Brightness, Controls, Game switcher, Return home. The bridge drives the player like any SDL engine: F5 saves the machine to the state slot, F7 loads it, Tab toggles fast-forward, quitting saves first when auto-save is on. Auto-load, quick resume, lid sleep, volume keys and playtime come from the runtime as for every engine.
- **Graphics** for RG DS Play: **Game screen** (Sharp fill · Whole pixels · Smooth fill), **Second screen** (Companion · Off · Swap screens) and **Frame** (Off · Indigo · Glacier · Black · Platinum · Black bar · SP Onyx · SP Cobalt · SP Platinum, then the owner's picture frames), applied live through the bridge (`play-graphics:<fit>:<second>:<frame>`, the player's exported `rgds_play_set_graphics`) and kept per game in `library/<key>/play.ini` once the player confirms; and **Shader**, the same carousel and presets as the DS engines.
- **Frames** (`play/src/ui/frame.cpp`) are drawn by the player from shapes and the launcher's font, with no pictures of real consoles:
  - the four **shells**: a plastic body in a classic shell colour, the dark screen lens with its power light, three dots in red, green and blue under the screen and a speaker grille, the game at the largest whole scale that leaves the shell room (2× on the 640×480 panels);
  - the **black bar**: the game across the full width at the top (640×427 for the GBA, like Sharp fill) and the system's name on a black bar below, ADVANCE in a violet-to-pink sweep, COLOR a colour a letter; the layout spruceOS uses with the "Perfect" overlays;
  - three **SP** clamshells (Onyx, Cobalt, Platinum): a slim lens with the game as large as fits (2.5× for the GBA) and GAME BOY ADVANCE SP under it.
- **Picture frames** are the owner's own: any PNG in `userdata/frames/<system>/` (`gba`, `gbc`, `gb`) is offered by its file name after the built-in frames. They are laid over the game like RetroArch overlays (640×480 or any size, scaled to the panel), and the game fills their see-through part (alpha below half), which is left fully clear (an LCD grid an overlay draws there is dropped, so the Shader setting alone decides the screen's look), so the 640×480 overlays made for the Miyoo Mini and RG35XX fit as they are. None are shipped: the ones spruceOS uses, the "Perfect" overlays (`ourigen/perfect_overlays`, after 1playerinsertcoin, with bezels by mugwomp93 and drkhrse), carry no licence and Nintendo's logo, and libretro's MIT `overlay-borders` has only 16:9 photographs of the console. Frames go through `play-graphics` by name; the bridge takes the rest of the command line, so names may hold spaces.
- **Shaders** run in the bridge's GL pass as for the DS: it also takes 240×160 and 160×144 textures from `rgds-play`, and compiles the presets that draw an LCD grid with the game's own grid instead of the DS's 256×192, so each cell is one GBA or Game Boy pixel. While a shader is chosen the player draws its game texture plainly (no render target, the bridge's `rgds_shader_live`), so the pass can take that draw and do the scaling. The second screen is shaded too, like a DS game's bottom screen: the bridge takes RG DS Play's 640×480 streaming companion texture (not its static frame textures) and gives it a grid of the same cell size as the game screen (8/3 panel pixels for a GBA game at sharp fill, so 240×180 cells), measured from the game's own draw.
- **GBA screen** (preset 30) is what a GBA game shows while its own shader is Default: the GBA LCD's colour response (after Pokefan531's public-domain GBA model: a darker gamma, colours bleeding into each other, slightly lower brightness; darkened half as much as the unlit AGB-001 screen, since the RG DS panels are lit) and the faint gaps between its pixels, one cell per GBA pixel. **GBA Color** (29) is the colour alone. Both sit in the carousel's GBA screens group. A screen group is offered only for its own system's games: a GBA game's carousel has no DS screens (DS Classic, DS Lite, the NDS Color mixes), a DS game's no GBA screens, a Game Boy or Game Boy Color game's neither; `runtime/play_options.py` (`shader_for`) keeps the same rule at launch, so the DS default from Settings never reaches a GBA game, and a Game Boy game given a DS screen as its default gets Off.
- **Windows:** the player gives its two windows the launcher's Wayland roles (`org.rgds.top`, `org.rgds.bottom`), so Weston puts the game and the companion on their own panels.
- **Two GL contexts, one thread:** each panel has its own window, renderer and GL context. On the RG DS (Mali, Wayland) SDL's idea of the current context drifted from EGL's, so the companion's texture uploads landed in the game window's context and the bottom panel kept its first picture (the title card, or the first HUD) for a whole race. The player now binds a panel's context for real (released, then made current) before touching it, and draws and presents each window in one go (`use_panel` in `play/src/frontend/main.cpp`). Verified on the handheld from the display controller itself, through RG DS Core's own launch, a race, the game menu and back.
- **Seeing what the panels show:** renderer readbacks and the bridge's capture read an app's own buffers, which hid this bug. `tools/scanout.c` reads the planes the display controller scans out (after Weston), so it shows exactly what is on each panel; build and run it as its header says.
- **Saves:** battery saves in `userdata/emulators/rgds-play/saves/<system>/`, checkpoints in `userdata/emulators/rgds-play/states/` and `library/<key>/auto-rgds-play.dss`, all in **Back up all saves**. The rules under Saves below apply.

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

**Cost on the RG DS, Super Circuit racing (600 frames):** emulation 4.9 ms a frame alone, 5.3 ms with the split; drawing both panels in software 2.5 ms (the windowed player draws the top one on the GPU). About 8 ms of a 16.7 ms frame.

## Profiles

A profile is a text file in `play/profiles/<system>/`, picked by the game code in the ROM header. Profiles are data: adding a game, or fixing one, needs no rebuild.

```ini
[game]
title = Metroid Fusion
codes = AMTE                    # one or more 4-letter game codes

[companion]
live = mem8[03000BDE] == 01     # in gameplay: captures make sense
menu = mem8[02000000] == 0      # title/file screens: forget the map
room = mem16[02000002]          # values that change on entering a room
interval = 90                   # frames between routine captures
dim_when_unavailable = no

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
backdrop = game                 # behind the HUD: the game's scenery, or #RRGGBB
backdrop_layers = E             # drawn by a silent copy with only these layers
backdrop_rows = 76              # only the rows above the horizon
moves_when = mem16[06003808] == C044   # the layout below applies (racing, not results)
to_game = 104 0 33 29 > 4 4     # a HUD box put back on the game screen at X Y
panel = 140 64 100 96 > 136 84 488 380 trim   # HUD box > slot on a 640x480 panel
```

Conditions are `memN[address] [& mask] == a | b`, `!= a`, or `in lo..hi`, joined with `and`; addresses and values are hex. A parse error names the file and line, and that profile is skipped; the game still plays.

Shipped today:

| Code | Game | State |
| --- | --- | --- |
| `ZRDT` | RG DS Play test cartridge | Tested end to end, on the Mac and the RG DS |
| `AMTE` | Metroid Fusion (USA) | Facts from GBA DS; not yet run through RG DS Play |
| `A2CE` | Castlevania: Aria of Sorrow (USA) | Facts from GBA DS; not yet run through RG DS Play |
| `AMKE` `AMKP` `AMKJ` | Mario Kart: Super Circuit (USA, Europe, Japan) | HUD on the second screen while racing; the pause menu, lap times, results and TRY AGAIN / NEXT RACE stay on the game screen, with the race's last HUD kept below. Whole races driven headless with `tools/play_drive.py` (with and without items) and replayed through the real player on the RG DS |

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
| `src/companion/` | Profiles and conditions, the shadow capture engine, the second-screen logic |
| `src/app/` | `Session` (one game: core, save, companion) and save storage |
| `src/ui/` | CPU canvas, the launcher's rounded font (RGF1 atlases), panel layout, BMP output |
| `src/frontend/` | `rgds-play` (SDL2) |
| `src/tools/` | `play-shot` |
| `profiles/` | Companion profiles per system |
| `tests/` | Unit tests and the test cartridge |

mGBA (MPL-2.0) is built from a pinned commit (`tools/build_play.py`) and linked statically. The handheld build uses mGBA's bundled zlib and the firmware's own SDL2.

## What comes next

In rough order. Each step keeps the rules above.

1. **Real games.** Run Metroid Fusion and Aria of Sorrow through `play-shot` with the owner's own dumps, tune their profiles, and measure their capture cost on the A55. Covers for other systems come from the Libretro thumbnails.
2. **Every other system through libretro.** A libretro host in the same player, using the cores already on the stock card (`/oem/retro/cores`). The companion code talks only to `Core`, so the shadow-copy map works for any core that can save state (a separate process, since one libretro core can't be loaded twice in one process).
3. **More of the second screen.** More HUD games on the GBA (Super Circuit's results and podium screens; Super Mario Advance's status bar). On the SNES, snes9x's layer switches let a second copy draw only the HUD, which is how Super Mario Kart's map and race status can get the bottom screen too. Touch: pan and zoom the map, tap to drop a pin. For every game: box art, playtime, save states with pictures.
