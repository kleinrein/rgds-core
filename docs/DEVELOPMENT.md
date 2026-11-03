# Save-to-refresh development

## Prerequisites and private payloads

The host preview needs Python 3.9+, a C++17 compiler and SDL2 development files (`sdl2-config` on the host). Device launcher/bridge builds use Zig 0.13; source-built engines also need CMake and Ninja. Ordinary builds use checked-in fonts/previews. Regenerating assets needs Pillow and, for shader previews, host EGL/GLES2. Update signing needs OpenSSL 3.

Packaging requires an independently provisioned, ignored `vendor/engines/` bundle. The one-time importer takes an owner-supplied system payload directory containing `bin/dsperate` and community `shaders/`, retains notices and verifies an exact inventory:

```sh
python3 tools/provision_engines.py --source /path/to/owner-supplied/system
make package               # the release that the RG DS OS image is built around
make image
```

Do not fetch unspecified proprietary payloads. Provisioning does not resolve redistribution rights; see [third-party provenance](../THIRD_PARTY.md). The supplied DSperate binary is excluded from new packages in favor of the source build. RG DS OS ships no DraStic. Source builders record pinned commits, patched corresponding source, toolchain inputs and hashes; packaging rejects stale or changed build outputs.

Community shader data lives in `vendor/engines/shaders/`; original RGDS effects and the catalog live in `assets/shaders/`. `tools/build_shader_previews.py --shader-dir /path/to/presets` can use another explicit community pack.

See [performance measurement](PERFORMANCE.md) for repeatable host/device checks and [the documentation index](README.md) for current guides.

## Host preview loop

Start the local loop from this repository:

```sh
make dev
```

Keep it running while you edit. The native SDL preview uses the same renderer as the device, with the top and bottom panels stacked in one window.

| Change | What happens |
| --- | --- |
| Save `themes/default/light.ini` or `dark.ini` | The matching active palette reloads in the existing preview after roughly 300 ms of stable file contents. Cartridge selection and emulator choice stay in place. |
| Save a `.cpp` / `.hpp` file under `src/` | The launcher rebuilds, then the preview restarts and restores your selection and core. Build duration is printed. |
| Introduce a compiler error | The current preview keeps running. Fix it and save again to retry. |
| Enter an invalid theme colour | The current theme stays visible; the error appears in the terminal. The next valid save reloads. |
| Press Escape/B in the preview | Settings goes back one level. From the library, the preview closes; a source save opens it again. Ctrl+C ends the watcher. |

The current theme format controls nine palette colours plus a `mode=light|dark` hint. Geometry, controls, text and interaction changes live in `src/launcher/main.cpp`; those use the rebuild loop. This is automatic refresh for a native application: theme changes stay in the same process, while compiled code needs a process restart.

Pick the dark palette or scan real game filenames:

```sh
make dev THEME=themes/default/dark.ini
python3 tools/dev.py --rom-dir /path/to/your/NDS --theme themes/default/light.ini
```

Preview never starts an emulator. Its remembered selection/core is stored under the ignored `build/dev/` directory. To edit the live launcher sources in Codex, start with `src/launcher/main.cpp` and the selected `.ini` theme file.

Open Settings with S from the desktop preview, or launch it directly with `make settings`. On device, use Select or Menu/Anbernic at home. Theme offers Light/Dark and DS rounded/Pixel classic. Touch the choice directly, or use up/down to focus a row and left/right to choose. A toggles the focused choice; B goes back. Choices apply immediately and are saved in `build/dev/preferences.ini`, or `rgds/userdata/preferences.ini` on the device. A saved colour-mode choice takes precedence over the initial `THEME` file selection; choose the matching mode in Settings to see the palette you are editing.

Power and brightness have desktop simulation and device capability checks. Sound has a shared slider/mute; System has timezone, device information and local recovery. Connectivity scans and joins Wi-Fi, shows its IP address, and toggles session-scoped SMB sharing using the bundled service. Sharing details contains the address and temporary login; the desktop preview simulates the toggle without opening a server. Library offers emulator/filter defaults, automatic checkpoint preferences and ScreenScraper. X/long press opens cartridge options; Y cycles All/Favourites/Recent. Display settings can disable animations and the touch indicator independently. Display → Animations also offers optional Spring and Push transition styles. Spring keeps the playful menu motion; Push slides level pages in from the right and reveals the parent from the left on Back, with a short parallax movement. The style is saved as `view_transition_style=spring|push` in `ui.ini`; existing preferences keep Spring and the current transition toggle. Push uses three reusable 640×480 render targets, including the last displayed composition so rapid navigation can interrupt a transition without queuing input. It settles in 300 ms and returns to idle; renderers without target support use immediate navigation.

`make game-menu` opens the shared in-game screen on the desktop. It uses the same renderer and settings as home, with simulated save/load actions. `--game-core rgds-experimental|rgds-play` changes its engine label; all share the RGDS controls. Preview opens no emulator and no audio device.

DS rounded uses the bundled, open-licensed Nunito font at weight 600. Native rendering reads prebuilt alpha atlases, so the card requires no additional font library. Latin/Latin-1 text is covered. To regenerate the atlases after a deliberate font change, run `tools/bake_font.py` with a Python environment containing Pillow. The font source, exact upstream commit, license and generated atlases live under `assets/fonts/`. Ordinary builds do not need Pillow or font downloads.

## The device loop

The handheld runs RG DS OS from its own card (`docs/OWN_OS_PLAN.md`; `tools/own_os_card.py` flashes a spare one).
Boot it, join Wi-Fi from Settings, and SSH in as `root@rgds.local` (key authentication; the card's boot partition
takes `rgds-setup/authorized_keys` on the first boot). The system is a read-only image, so a change reaches the
handheld as a new image in its other slot:

```sh
make image                 # tools/own_os_build.py: Buildroot in the Lima VM, after the package and engine builds
make push                  # tools/own_os_push.py rgds.local --reboot: the inactive slot, on trial, then a restart
make shot                  # both panels as one PNG in build/shots/
```

A USB-C cable to the computer is faster than Wi-Fi and needs no network: the port is a USB network card
(`usr/libexec/rgds/usb`, CDC NCM, no driver needed on macOS, Linux or Windows 10 and later). The handheld is always
`10.55.0.1`, and the computer gets an address from it but no route, so its internet is untouched. `make push`,
`make shot` and `make ssh` use the cable when it answers (`tools/device_addr.py`) and fall back to `rgds.local`;
`DEVICE=` overrides that. `rsync` is on the image. File sharing listens on the cable too and works without Wi-Fi:
`smb://10.55.0.1`, or the address and login in Sharing details. Settings > Connectivity > USB data transfer switches it
off across boots, and the port still charges (`/data/rgds/usb-off`).

Settings > Connectivity > Use as USB drive makes the data partition a real drive on the computer (`usb drive`, run by
the session with the menu quit and `/data` unmounted, since a second writer would corrupt the card). Ejecting it, pulling
the cable or pressing A on the handheld brings `/data` and the menu back, with a partition check and macOS's clutter
files removed. SSH over the cable stays up throughout.

`own_os_build.py` stages the launcher from `build/device` and refuses one older than the tree (run
`tools/build.py device` first), and takes runtime, themes and assets from the checkout. `own_os_push.py` arms the
same boot trial an over-the-air update uses (docs/OTA.md), so a push that does not come up falls back by itself;
games and saves stay on the data partition. Logs are in `/data/rgds/logs` and `/run/rgds/messages`; boot marks in
`/run/rgds/boot-times`.

Init respawns the launcher whenever it exits (`killall rgds-launcher` restarts the menu); to keep it down for a
hand-run, bind-mount a copy of `/usr/libexec/rgds/session` that waits instead (the copy must be executable, or init
gives up on it). The launcher opens its input devices when it starts, so a virtual uinput keyboard for remote keys
must exist before it does. BusyBox `pgrep -f` matches its own SSH command; anchor patterns or read
`/proc/*/cmdline`.

For a quiet hardware test, open Settings → Sound and select Mute before launching. The saved level follows all engines, and physical volume keys work during play. Increase it deliberately after confirming muted startup and return. The 2026-10-07 volume fix has offline coverage but has not yet been checked on hardware.

The game runner owns the menu process and both hardware menu keys. By default it pauses the owned engine process and draws the menu in its own GPU windows. A first-frame timeout restores the game if menu rendering fails. The optional live background uses shared canvases and is opt-in. The bridge filters Guide/Home before legacy handlers see them and discards queued input from the menu when play resumes. GPIO button ordering on the recorded ANBERNIC device places raw joystick button 8 at Linux code 312; code 316 belongs to another stick button. Generic controller Guide remains code 316. This mapping and the process/display handoff need device confirmation.

Checkpoint saves acknowledge a changed, stable expected engine slot and flush the independent checkpoint before replacement. A failed save keeps the prior checkpoint and presents retry/resume/return-without-saving. DSCore's load reply acknowledges dispatch only. Actual restored game progress still needs hardware checks. Keep these distinctions when reporting test results.

Additive animation/touch-indicator/filter/view choices use `userdata/ui.ini` so older development builds can still read the base appearance file. Per-cartridge overrides and favourite flags live in `userdata/library/<key>/game.ini`. The versioned IDs in `assets/shaders/catalog.tsv` preserve Off/Sharp/LCD at 0/1/2 and add the full supplied pack. Both launcher and Python adapters read that catalog. DraStic maps to the supplied shader IDs, and DSperate/melonDS use the shared SDL/GLES shader path. All four paths support the current 28 effects plus Off. The picker is a Home-style carousel in six groups (Original, DS screens, Natural, Vivid, Pixel art, Moods): Left/Right browse, Up/Down, L/R or the group chips jump between groups, each tile shows its preset at 1:1 from the packaged previews, and per-game Default inheritance is kept. The eight original presets added on 18 October 2026 (DS Classic, DS Lite, Natural glow, Vivid LCD, Natural/Vivid smooth, Pixel beads, Glossy tiles) draw their pixel structure on the DS's own 256×192 grid, anti-aliased from `u_target_size`, so they hold up at the RG DS's 2.5× and the Plus's 4× and fade out below 2×. Library choices take effect on launch; in-game choices update the running renderer and keep the picker open. Actual appearance and frame pacing must still be checked on the Mali driver. The picker remembers its page during the development rebuild loop.

## Refining the interface

### The 2026-11-01 motion pass

Touch and transitions got physics (`src/launcher/kinetics.hpp`, driven each frame by
`kinetics_update` in `touch_kinetics.hpp`; `tests/test_kinetics.py` pins the numbers):

- **Flicks coast.** The settings list, the Filter sheet, pickers and the library's list layout
  read the finger's speed over its last ~100 ms (`FingerSpeed`) and coast on after the lift
  with exponential friction (a firm flick runs about a second), ticking a haptic detent per
  row. Pulled past an end they give like a rubber band (`ScrollMotion::band`, at most 72 px,
  less and less) and spring back; a coast into an end bounces a little. The launcher still
  reads each list's clamped integer scroll; `list_shift()` and friends are what the drawing
  adds. A D-pad focus change glides the rows there with a small overshoot instead of jumping.
- **A hard flick goes far.** Above `FingerSpeed::hard_flick` (2.2 px/ms) a list's coast speed is
  boosted steeply, a flick during a coast adds to the one running (up to 32 px/ms, about 140
  rows), and the rows stop ticking the motor above 3 px/ms. An end only ever takes 3 px/ms, so the
  bounce stays small however fast the coast came in.
- **Carousels only coast on a hard flick** (`Carousel::flick_target`): an ordinary swipe settles
  on the nearest item, a hard one travels 1.5 to about 14 items, and a long tween takes up to
  510 ms.
- **Edge gestures move the page.** Under a Home pull the live page lifts and shrinks with the
  finger (drawn through `gesture_canvas`), under a Back pull it slides aside; when the gesture
  fires it flies away over the page it revealed (`PageDrag`), when the finger lifts short of it
  the page springs back. Needs the master `animations` switch, not view transitions.
- **The upper screen travels with the lower one** again, cheaply this time: with view
  transitions on, every upper frame is drawn to `top_canvas` and copied to the panel, and the
  canvas is swapped into `top_previous` after each idle frame, so the frame a transition leaves
  from is at hand without ever rendering the upper screen twice (the 2026-10-28 attempt
  re-rendered it on every key press and was reverted). `top_signature` is void while a
  transition runs. Whatever the lower screen's style, the upper screen only crossfades and is
  never moved or scaled: the first device trial moved and scaled the slab per style and the
  text shimmered, a 12-24 px drift still glitched. Part of that glitch was a clock bug: the upper
  panel is drawn first, `view_drawn_at` was still older than the transition's start, and the
  unsigned difference finished the fade on the first frame (new page, back to the old one, fade
  in). `ViewMotion::fade` keeps its own clock; `top_fade_reversals` in the report stays 0.
- **The Filter sheet rises** over the dimmed library and slides back down (same 280/240 ms as
  the pickers, scaled by the view Speed); the library underneath is drawn with
  `sheet_underlay` set so the sheet branch of `bottom` does not recurse.
- **Back returns to the tile you came from.** `home_origin` remembers the Home tile the
  library was entered from (and `systems_origin` whether through Systems), so switching the
  collection on the way (Recents → All games through the sheet) no longer lands Back on the
  new collection's tile. Report fields: `home_origin`, `kinetic_frames`, `gesture_frames`,
  `sheet_frames`, `top_animation_frames`, `kinetics_active`.

### The 2026-10-22 follow-up

- **Held scrolling covers a whole card.** `hold_navigate` steps one game per repeat
  (`HoldRepeat` still paces 130/75/45 ms); from 2.4 s held, `hold_stride` doubles the
  stride every 600 ms (capped at games/40, at most 128) and the run stops at either end
  instead of wrapping. `HoldRepeat::held_for` reports the press length. Home's rail and
  cards under 24 games keep single steps.
- **The view sheet closes cleanly.** A choice on the sheet changes the page under it; the
  sheet's own 140 ms slide is the whole transition. `sync_view_motion` starts no Library
  crossfade while the sheet is open or leaving (`sheet_leaving`), so the old frame, sheet
  and all, no longer hangs over the closing sheet until a crossfade runs out.
- **One line of tips on the Games top screen.** `cover_line` draws the top screen's lower
  edge after the DSi: L (Previous) and R (Next) in the corners like its shoulder tabs, Y
  (View) beside L, X (Options) beside R, and the play count and time (or a notice) in a pill
  centred on the screen between the two halves. No pills on the tips, muted 14 px text. When
  a long translation leaves no room for the centred pill, L and R drop their words and keep
  their badges (Russian does with any stats). Tried and dropped on the way: a staggered
  X-over-Y pair (too big), and Y/X in a row above R (it ran into tall box art on the handheld).
- **L and R look like shoulders.** `key_badge` draws L, R, L2 and R2 with
  `shoulder_fill`: a flat tab with small corners whose outer top corner sweeps down in a
  deep quarter ellipse, as the DSi draws its shoulder buttons. Face buttons stay round.
  Every key tip goes through `key_badge`, so this covers the Games top screen, the
  timezone tabs, the cheats and shader "L R" tips and the L2/R2 hotkeys on the view
  sheet, in the launcher and the in-game menu alike.
- **Accent on the edge only.** The top-screen note (`dsi_note`) is a panel card with a
  2 px accent edge; the sheet's selected tile is a panel tile with its accent outline;
  the column Home's tiles are plain panels with the brackets as the selection mark.
- **Last played starts over Home.** `open_last_played` keeps Home up and calls
  `launch(true)`, with the rail reset to Home's tile; `finish_launch` returns to Home.
  Preview launches leave after 500 ms and the handoff waits at most 1200 ms for the
  launch cue (1.35 s with a quiet tail).

### The 2026-10-21 DSi pass

- **Buttons are DSi buttons, not glass.** `glossy_pill` now draws a fine darker rim, one
  smooth vertical shade (lighter top, slightly deeper bottom) and a single bright line
  inside the top edge. There is no half-height gloss band any more; a press turns the
  shade over. Pale bodies (white panels) get no lift, dark ones only a little. Accent
  swatches and the Home column tiles use it too.
- **START frame.** `start_frame` draws the selected cartridge (and the shader picker's
  selected tile) as the DSi does: one accent body in one continuous shade, the tile set
  into it behind a darker rim, and PLAY / OPEN centred in the space below. No second
  band or outer ring: it reads as one piece in both palettes.
- **Edge-to-edge rail.** `draw_rail` follows the DSi scroll bar: arrow blocks flush with
  the screen corners, the recessed track filling the strip between them, and a white
  grabber with a neutral rim only. Blocks and grabber are square, like the DSi's; the
  grabber spans the strip's full height and its travel ends flush against the blocks. `RailGeometry` (`carousel.hpp`) is `left 64, width
  512, top 424, height 56`; the arrow hit areas follow it.
- **Lower-screen tiles.** `pattern(..., lower=true)` draws the original DS touch-screen
  grid (20 px square tiles with a pale edge) under the bottom screen; the top keeps the
  DSi graph paper.
- **Segmented controls slide.** `segment_position` eases the lit pill between choices
  (140 ms), keyed by page and row.
- **Quieter Home and settings.** No status line under the Home clock/calendar (only
  "Preparing" and the update banner remain) and no last-played box on the carousel Home;
  the calendar's weeks fill the card to its bottom edge. The accent preview is one card
  without the name/"A Apply" footer. Value pills keep 14 px margins with text, swatch and
  chevron on one centre line. The library header says "All games"; the view sheet is a
  3 x 3 grid (Collection, Browser layout, Search: Jump to letter, Search games, Clear
  filters) with Up/Down keeping the column.

### The 2026-10-15 polish pass

- **Depth everywhere.** `glossy_pill`, `raised_pill`, `sunken_track`, `footer_button` and
  `secondary_button` in `main.cpp` draw every button, toggle, segmented choice, slider,
  footer and sheet control the same way (shadow, glass band, crisp top line, 2 px sink
  while pressed). New controls go through them; a flat `round_rect` button is a bug.
- **The vertical rail** (`draw_scrollbar`) matches the horizontal one: a recessed groove
  with a pip per page over a shelf, a glossy thumb with an engraved grip that widens
  while held. The list layout, pickers and detail sheets all use it.
- **Games screen.** The collection dropdown and search field are gone: the header's first
  row shows the collection (or the active search), the count and a search glyph, and a
  tap on it opens search; a tap on the title row opens the view sheet. Y / X hints sit in
  the lower corners of the top screen; L2 jumps to a letter and R2 searches (Q / E on a
  keyboard), and a second press closes what the first opened. Play count and time under
  the cover share one centre line (`centred_label_y`).
- **Background loading.** `Media` runs one loader thread that reads and decodes covers and
  cartridge banners (`request`, `adopt`); the main thread only creates textures, and a
  user event wakes an idle menu when a load lands. The selected cartridge is still
  prepared in place; ten neighbours (fourteen in the list) and five covers each way are
  requested nearest first, so a first fast scroll no longer waits on the card. Covers on
  their way show a blank card, not "No cover yet".
- **Top panel skips.** On the library pages the upper screen repaints only when its
  `top_signature` changes (and at least every two seconds); transitions and browsing
  animate the lower screen alone. `top_skips` is in the cache report.
- **Home layout** (Display): the carousel, or the **Column** (the default, after the DSi) with the last
  played game on top, the categories as wide tiles, a brightness / mark / statistics bar
  below, and the DSi's clock and calendar on the upper screen. **Background** adds the
  DSi's ruled grid behind every screen. **Touch gestures** (off by default): swipe up from
  the bottom edge for Home, in from the left edge for Back. Saved in `ui.ini` as
  `home_layout`, `background` and `gestures`.
- **Settings.** System has a Time format (24-hour / 12-hour, `time_format` in
  `system.ini`) and a Daylight saving row under Timezone (Automatic follows the zone's tzdata rules; On/Off pin the
  standard offset, +1 h for On, as a fixed POSIX TZ — `dst` in `system.ini`, `timezone_value` in the launcher and
  `runtime/emulators.py`); Sound is laid out as cards like every other page; Connectivity's upper
  screen is a live status card (green while connected with IP and signal, a spinner while
  connecting or scanning, the service's message when it failed); Statistics has a ring of
  the played share with the hours inside, tiles, the most played game, and medals for the
  top three. Transition speeds are now 0.55 / 1 / 1.7 of the base duration.
- **Status bar.** The Wi-Fi-off glyph has quiet arcs and dot with a solid slash, in both
  the bitmap and pixel styles.
- **Second pass (same day).** The PictoChat room follows the DS arrangement (tools down
  the left, names under the pad, the keyboard narrowed to 90 % with Send, the arrows and
  Leave beside it); favourites wear a heart badge on the cover stage and on every tile;
  switching gestures on opens an intro sheet, a handle at the bottom edge rises with the
  finger, a Back disc slides in from the left and a ring marks a fired gesture; Date and
  Time format sit under Timezone; the vertical rail lost its hover growth; the Library
  view sheet outlines and tints the current choice in the accent; the DSi column's clock
  and calendar share one height and the grid is graph paper (4, 16 and 64 px).
- **19 October 2026.** Holding Up or Down repeats in every list and picker
  (320 ms, then 130, 75 and 45 ms steps, the same `HoldRepeat` as Left/Right;
  the poll tells vertical from horizontal by ±2). The grid background now runs behind
  Settings on both screens and behind PictoChat's lower screen. Edge gestures work in
  PictoChat's room picker and name screen, not inside a room, where the edges belong to
  the chat; an edge touch that does not travel is still a tap. In preview input scripts a
  `key` the script never releases is a tap, so only `key` … `key-up` pairs hold.
- **Shutdown.** Power off and Restart fade both screens to the background, show the mark
  in a turning ring with "Powering off…" / "Restarting…", and ignore input until the
  system takes over (`begin_shutdown`, `shutdown_overlay`); a failed request lifts the
  veil. In the DSi column, Down from the full-width tiles returns to the column you were
  in. The top-panel signature carries the battery's charge phase, so charging no longer
  keeps the loop dirty while the upper screen is skipped.


Use the preview for palette, spacing, hierarchy and navigation ideas, then check the same change on the actual panels. The current icon strip animates with a 190 ms eased slide, a small selection lift, and 150 ms settings transitions. It redraws during transitions and returns to event/clock-driven idle rendering. Dragging tracks the pointer until release, and swipes never launch a game. The DSi-style title balloon, raised cartridges and integrated START frame retain these gestures. Range arrows step/wrap and the centre seeks. Footer view buttons remain available when empty. Queued releases/back actions are handled before held-key repeat, including under slow capture frames.

The status bar groups `%m/%d  %H:%M`, renders segmented battery state/charging and keeps the optional percentage. Dark mode has its own charcoal/cyan palette, with matching settings and game-menu surfaces. Gradients use a small fixed set of vector bands; no bitmap UI or extra rendering dependency is added. Confirm readability, contrast, animation frame time and physical battery/charging behavior on the actual panels.

Starting a cartridge fades the library under a small eight-block DS-inspired loader. There is no card or loading text. The overlay has a 110 ms eased entrance and an 80 ms exit. ROM preparation, helper shutdown and pending controls advance through the normal event loop; B and window-close remain responsive. A ready engine interrupts the entrance, fades the UI into the theme's neutral background, then receives the display handoff after SDL closes. There is no minimum loading hold on device. Preview shows preparation for 650 ms, then fades back to the library; cancellation and errors also return to the library. Disabling animations skips both transitions and keeps the loader static. Eight native rectangles draw the loader, with no textures or per-frame allocations. Repeated START presses and navigation are ignored during preparation. Direct runtime launches do not require the display handshake.

Touch contact gets a small ring (toggle Touch indicator under Display), selected cartridges depress while held, and the existing 600 ms hold gesture shows progress followed by “Release for options”. A swipe cancels that cue. Release fades the contact away; focus loss and cancelled gestures clear it. With animations disabled, contact and loading feedback stay static. Feedback geometry is cached, animation redraws are capped at roughly 60 Hz, and a stationary hold returns to idle once the cue completes. Host SDL tests cover the display handshake, duplicate launch input, touch cleanup, reduced motion and return; panel handoff and frame pacing still need confirmation on device.

The horizontal rail has glossy primary-colour caret buttons with 84×71 touch targets. Its thumb expands and spreads its grip while captured, even if the finger moves outside the rail or the touch indicator is disabled. Press feedback eases in over 100 ms and releases over 180 ms; disabling animations keeps immediate static pressed states. Settled controls return to idle rendering. Grabbing the thumb preserves the contact offset instead of jumping to a different selection; tapping the track seeks and the carets retain step/wrap navigation. Empty and single-item rails use a muted appearance. SDL regression checks cover both palettes, release/focus-loss cleanup and dragging; physical touch feel and frame pacing still need handheld verification.

For an explicit one-shot hardware capture, create `rgds/userdata/session/capture-request` while our launcher runs. The next frame writes `ui-snapshot-top.bmp` and `ui-snapshot-bottom.bmp` in the same directory and removes the request. This is a development-only filesystem control; the launcher does not continuously capture screens.

`make shot` (or `tools/device_shot.py`) drives this over SSH: it asks, wakes an idle launcher with a harmless key, pulls both panels in one connection and stitches them with numpy. `DEVICE` defaults to `rgds.local`; while a game runs it uses the emulator bridge's `emulator.control.capture` instead. Output goes to `build/shots/`. Options: `--style float|device|plain` (`device` draws the open RG DS to scale, 160 x 172 mm around 88 x 66 mm panels that fill the screen wells like the photo,: red lid rim, grey lid face with seven-dot speakers, black hinge barrel, D-pad, ABXY, both sticks, four small keys and the green lights; it only stacks, and `--shell red|black|white|turquoise` picks the lid colour, the base stays dark), `--layout stack|side`, `--bg auto|transparent|#RRGGBB`, `--scale 1-4` (2 by default, 1 for `device`), `--raw` (each panel as its own PNG), `--copy` (clipboard, macOS), `--open`, and `--from TOP.bmp BOTTOM.bmp` to restyle saved panels without the device, for example `make shot SHOT_ARGS="--style device --layout side --open"`. Password entry remains masked. Profile animation frame time on the device before adding further effects.


## DS engine and local verification

DSCore (`rgds-experimental`) is the only DS engine. DraStic, DSperate, NNDDSS and melonDS are retired: engine IDs 0–3
keep their slots because settings store the index, but they are never offered and a saved choice of any falls back to
DSCore. DSCore's source is `dscore/`, a fork of DSperate at a pinned commit (`dscore/FORK.md`); nothing builds or
ships DSperate itself. `tools/engine_toolchain.py` holds the shared cross-build pieces (zig toolchain, CMake, the SDL2
link SDK it builds on first use) for DSCore, its Vulkan renderer and RG DS Play.

For local launch/menu work, build the host launcher and run the focused checks:

```sh
python3 -B tools/build.py host
PYTHONDONTWRITEBYTECODE=1 PYTHONPATH=tests:runtime python3 -m unittest \
  test_launch_preparation test_game_menu \
  test_game_menu_ui test_shader_pixels
```

`test_shader_pixels` uses a real host EGL/GLES implementation (Mesa or the
installed Android emulator's SwiftShader libraries); it does not run games.
If the shader pipeline changes, regenerate previews with
`tools/build_shader_previews.py` using a Python environment with Pillow, then
validate/package normally. Preview manifests bind to the exact pipeline source.

`tools/benchmark_runtime_idle.py --output build/runtime-overhead.json` measures
the runtime's log-worker wakeups and component-copy overhead on the host.
Keep core/user saves out of benchmark fixtures. No deploy watcher or hardware
command is needed for these checks. [Save locations](SAVES.md) document the current boundaries.

Run
`PYTHONDONTWRITEBYTECODE=1 PYTHONPATH=tests:runtime python3 -m unittest test_emulator_settings_ui test_settings_interactions`
after building the host launcher to exercise disabled options, picker cancel/apply,
default inheritance, restart selection and failed preference writes. The tests
drive real SDL input with temporary data; they do not launch handheld emulators.

## Crash reports

A launcher crash leaves a record in `userdata/logs/crashes.log`: signal or exception, frames, memory map, screen and
game count (`src/platform/crash_record.hpp`). `rgds/logs/launcher.log` ends in the exit status, with `dmesg` after a
failure, and the last five runs are kept. A kernel panic restarts after 5 s and rcS moves its record from RAM to
`rgds/logs/pstore/`. A hard freeze or a power-off records nothing.

Settings > Maintenance > Save device report bundles all of this into `userdata/support/report-*.tar.gz`, with network
names and addresses removed, for a tester to send. ROM file names stay in it. To read a record:

```bash
tools/symbolize_crash.py crashes.log
```

`tools/build.py device` keeps symbols in `dist/symbols/` by build id (needs `llvm-objcopy`: `brew install llvm`). `dist/`
is not in git, so keep that folder for every build handed to testers: a record from a lost build stays bare addresses.

## Formatting

Python is laid out by yapf (`.style.yapf`), C and C++ by clang-format
(`.clang-format`; `dscore/.clang-format` keeps DSCore's two-space indent). Both
use a 120-column limit. `tools/format.py` installs the versions pinned in
`requirements-format.txt` into `.cache/format-tools` on first use, so every
machine formats alike, and only touches tracked or new files. Vendored and
generated sources are listed in `.clang-format-ignore` and `.yapfignore`.

```sh
make format-check                      # list files that need formatting; changes nothing
make format                            # rewrite them in place
python3 tools/format.py --diff src/    # preview one area
python3 tools/format.py --changed      # only files changed against HEAD
```

Formatting a file under `dscore/` changes the fork's source hash, so rebuild
the engines before packaging.

## Setup on a new card

`src/launcher/welcome_screen.hpp` is the wizard a new card opens with (`Page::Welcome`), and Settings > System > Run setup again opens the same steps. The steps are language, colour and light/dark, name, Home (layout, background, animations, typeface), DS only or all systems, time, Wi-Fi, how to add games, and a last page of tips.

- **When it runs.** The session passes `--first-run`. The launcher runs setup unless `userdata/setup.ini` says `done=1`. With no `setup.ini`, any of `preferences.ini`, `ui.ini`, `system.ini`, `pictochat.ini`, `ds.ini` or `device/status.ini` means the card has been used: it writes `done=1` and setup never appears, so an update never interrupts anyone. `library/` is not a sign of use, because the PortMaster artwork job can create it beside the first boot.
- **Saving.** Every choice goes to the normal settings files as it is made. `setup.ini` only keeps `step=N`, so a power cut or Use as USB drive (which restarts the launcher) comes back to the same step.
- **Input.** Items are rectangles from `welcome_items()`; the D-pad and touch both work from that list. Timezone, Wi-Fi networks and the name keyboard are the normal pages, with `welcome_detour` bringing them back into setup.
- **Defaults.** The language preselects a timezone and clock and date formats. A new card starts on all systems; the Systems step or Library settings can narrow it to DS.
- **The name.** `userdata/ds.ini` `nickname` is the handheld's name for DS games (`runtime/emulators.py`), under a game's own DS nickname. Setup also writes it to PictoChat's profile.
- **Testing.** `tests/test_welcome_ui.py`. Put `step=N` in `setup.ini` to open a step directly with `--preview --boot-startup --first-run --data <folder>`.

## Saves

A game's own save must survive every update, engine rebuild and test run.

- **Nothing an update writes touches them.** An OS update writes a root slot and its boot files; the data partition is only formatted while it is labelled `rgds-new`, on the first boot.
- **Save history.** `runtime/save_guard.py` copies a game's saves before each launch to `userdata/save-history/<game key>/<time>` (10 copies; 3DS and PSP share `3ds` and `psp` folders) and its resume point to `<game key>/resume` (2 copies). The player's systems also keep three of their own in `saves/<system>/backups`. `python3 save_guard.py <userdata>` lists the copies, `python3 save_guard.py <userdata> restore <copy>` puts one back, keeping what it replaces.
- **Earlier emulators.** A DS game with no DSCore save takes the newest one found beside the ROM or in a retired engine's folder. The old file stays where it is.
- **Writes.** DSCore and the player fsync the file and the folder around the rename.
- **Resume points across updates.** A resume point is lost when a new engine refuses it and quitting replaces it. So:
  - DS: the state format is field by field and reads across builds. Never raise `OLDEST_READABLE_VERSION` in an update; add fields with `more()` so old states still load. `DS_STATE_FORMATS` in `runtime/emulators.py` must match (a test checks). A state the engine refuses starts the game without it.
  - Player systems: keep a libretro core's pin unless its states are known to load across the bump. mGBA reads old states.
  - 3DS: `src/savestate.c` in the fork stores everything field by field, kernel objects and callbacks by name, so a state survives engine rebuilds; its top comment has the rules. Format 1 states, from before this, only load in the binary that wrote them: the engine keeps them as `<state>.refused-1`. Before giving players a new engine, run `tools/tanuki3ds/state-check.sh` with the previous one and `state-diff.py`.
  - `tools/resume_check.py` loads a resume point from every earlier release (`tests/resume-fixtures/ds`, from our own ROM) in the new DS engine, and `tools/own_os_release.py` stops if one fails. It also adds this release's to the set.
  - Save history keeps the resume point each update found (`resume/before-<build>`, last 4 updates).
- **Skipped resume points.** The launcher skips a DS resume point older than the `.sav`, and quitting then writes a new one, so anything that rewrites a `.sav` costs the player their place.
- **Test runs never use the owner's `userdata`.** Pass `--data` a copy, as `os/banana-probe/real_trial.sh` does.

