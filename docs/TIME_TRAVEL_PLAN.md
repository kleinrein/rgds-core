# Time travel plan

Three features built on the same pieces: whole-machine states, deterministic
input replay and the companion's RAM reads.

1. **Rewind**: a filmstrip of the last minutes on the lower screen. Drag to
   scrub, let go to jump back. What you left behind stays as a branch, so the
   moments form a tree you can browse, not ten slots.
2. **Ghosts**: a recorded run (a time trial, a boss attempt) as an input
   replay. A friend's handheld plays it beside their own game, live on the
   second screen.
3. **Speedrun mode**: splits that trigger from RAM (boss dead → split),
   practice states per segment, and your best run as a ghost.

Status, 2026-11-22: rewind built for every system, the DS included, host-tested
only (the DSCore build is cross-compiled, not run). Nothing on the handheld yet.

## What exists already

| Piece | Where | Used for |
| --- | --- | --- |
| Whole-machine states, any system | `Core::save_state/load_state` (play/src/core/core.hpp); `.dss` in DSCore | rewind moments, ghost start points, practice states |
| Cart RAM inside Play states | `mCoreSaveStateNamed(SAVESTATE_SAVEDATA)` | a jump back never loads a stale battery into a running game |
| Deterministic DS input log | `DSIN` in dscore/src/core/input/input_log.h | DS ghosts, DS replays |
| Scripted Play input | `RGDS_PLAY_INPUT` ("FRAME BUTTONS" lines) | the Play input log format grows from this |
| Shadow cores | `Core::shadow()`, the companion's backdrop | a ghost runs in a shadow beside the live game |
| RAM conditions | `parse_condition` (`mem8[03000BDE] == 03`), profiles | split triggers |
| Save gallery with object store | runtime/game_menu.py, docs/SAVES.md | the tree, persisted (phase 2) |

## Phase 1: rewind, every system (built)

One screen, two ways in, every engine behind the same hooks.

**The screen** (`play/src/companion/rewind.{hpp,cpp}`) is a launcher page like
the others: the header with the back arrow, a row of settings cards (the middle
one larger and focused, the time from now under each: `-0:42`, `+0:10` on a
future you left, `Now`), a row for the other future where a moment has one, and
the footer's Back and A (**Play from here**). In the game menu the top screen is
the Save gallery's card: the moment's picture, how long ago, what to do. The row
moves on the launcher's spring: a flick coasts and settles, the ends give a
little. Dragging, flicking and tapping only look; A or **Play from here** plays.
In the game menu (a software canvas) there is no glide: a flick lands as far as
it would have coasted, at once. Pad:
Left/Right, L/R by ten, Up/Down the future, A play, B back.

The first version (a Cover Flow stage with perspective, reflections and stars)
was out of place next to the rest of RG DS and slow on the handheld: per-pixel
drawing of the whole screen every frame. It was replaced on 2026-11-22. The game
menu now draws each picture as a texture made once, so scrolling only moves
textures.

**The moments.**

- RG DS Play (every non-DS system): `play/src/app/timeline.{hpp,cpp}`. Once a
  second of play the state is taken on the game thread and deflated (zlib level
  1) on a worker, with a picture up to 208x156. Memory cap 48 MiB, states and
  pictures together; past it the oldest go and their children hang from the
  parent. States over 24 MiB (PSP, Dreamcast) get no rewind; between 512 KiB and
  24 MiB the step widens with the size (N64 ~ one per 30 s).
- DSCore: `dscore/src/frontend/sdl/rewind_ring.h`, the same tree. A moment
  every 2 s (`DS_REWIND_SECONDS`), 128 MiB (`DS_REWIND_MB`), `DS_REWIND=0` off;
  none during a replay, a recording or a network session. Kept raw, not
  deflated: the deflate worker cost ~510 dropped frames in 45 s of Mario Kart 2x
  on the handheld (23 Nov 2026); a raw moment is one copy, ~20 moments ~ 40 s. The picture is both screens at their own size (256x384), the
  top over the bottom, taken from what was last presented (every present tier).
  A DS state deflates to about 2.4 MB, so 96 MiB holds roughly a minute; delta
  against the previous moment is the way to more.
- **No look runs anything in DSCore.** The first version loaded a moment and ran
  one unscaled frame for each look while the game was held. On the Vulkan engine
  that frame's GPU work was never collected, and the first real frame after
  Play from here hung waiting for it (Mario Kart DS on the handheld,
  2026-11-22). The menu now shows the moment's stored picture; only Play from
  here and Back load anything, as a checkpoint load does.

**Two ways in.**

- RG DS Play's dock: a fourth button, **Rewind** (a clock), between Menu and
  Switch. The player draws the screen itself on the lower panel; the game holds
  and goes quiet; the top panel shows the chosen moment live (loaded, one silent
  frame run, no achievement checks).
- The game menu, any engine: a **Rewind** tile in the top row (Resume, Rewind,
  Screenshot). The launcher compiles the player's screen in
  (`src/launcher/rewind_play_sources.hpp`, `rewind_menu.hpp`) and draws it on
  the lower panel; the top panel shows the chosen moment's own screen at full
  size. The path: game menu → `runtime/game_menu.py` (actions `rewind-open`,
  `rewind-show:<id>`, `rewind-play:<id>`, `rewind-back:<id>`) → the bridge
  (`src/platform/emulator_bridge.cpp`) → the engine's exported hooks
  `rgds_rewind_open/count/moment/rate/show/jump` (DSCore's
  `rgds_frontend_ui.hpp`, RG DS Play's `frontend/main.cpp`). The bridge writes
  the list `<control>.rewind` and the pictures (`.thumbs`, `.shown`) in the
  format in `moments.hpp`; the pictures go to `/run/rgds` (RAM) on RG DS OS, up
  to ~38 MB for a Play game that the card never needs to see, and the menu
  deletes them once read. The strip scrolls locally from pictures read once; only the
  picture it settles on is asked for. Leaving the page any way but Play sends
  `rewind-back`.

**Saves stay safe.** While the menu shows an old moment, the machine holds that
moment. Both engines put back "now" before anything writes a save (DSCore's
`flush_save` and autosave, Play's `flush` and `save_state_file`) and before the
next frame runs, so a menu closed any other way than Back or Play from here
(the menu button) never leaves the game on a moment only looked at. Playing from an
earlier moment keeps the battery save first, on every system: the first of the
session as `<save>.before-rewind` (never replaced, so a second rewind cannot put
a rewound save there), the latest as `<save>.before-rewind.last`. When the copy
cannot be written (a full card), Play from here is refused. Then the moment's
cartridge goes on: DSCore writes it at once; libretro cores write it at their
next battery check; mGBA when the game next saves. Back, and Play from the
moment the menu opened at, load nothing: the machine is still there.

Failures never take the game down: the engines' `rgds_rewind_*` hooks and the
per-frame take catch exceptions (out of memory is the likely one), the bridge
answers "unavailable" when the list cannot be written, and a core that refuses a
state is asked again a quarter step later (eight refusals before the first
moment turn rewind off for that game). DSCore reuses an evicted moment's buffer
for the next take and counts capacity, not size, against `DS_REWIND_MB`. The
game menu frees its pictures and textures however the page is left, and the
runtime deletes any picture file left in `/run/rgds` when the menu closes.

Not in phase 1: the tree lives in memory and is gone when the game closes;
translations for the new strings (English shows meanwhile).

Tests: `play-timeline-tests` (tests/test_timeline.py): the tree, eviction, big
states, the screen's touch and pad, the bridge format read back into the same
tree, and a real session on the test cartridge (Back to now restores the exact
state; Play from here branches).

### Checks to run on the handheld

- [ ] DSCore: the cost of a moment on the emulation thread (save_state of ~4 MB)
      at 2x/3x in Mario Kart DS, against the phase lock; raise the step if it
      shows
- [ ] capture cost per Play system (GBA, SNES, MD, N64) and the worker's deflate
- [ ] memory: 64 MiB (DS) / 48 MiB (Play) plus the worker's buffer, with a big
      library
- [ ] the game menu round trip for a look (menu → runtime → bridge → engine)
- [ ] the preview frame on GPU cores (N64) and on DSCore's scanout tier
- [ ] touch on the real panel: flick feel, the branch pill, the scrubber
- [ ] the springs' frame rate in the launcher's software canvas

## Phase 2: the tree on the card, merged with the Save gallery

- "Keep" on a moment writes it into the gallery's object store as a
  checkpoint with `parent` (the object it branched from) and `position`.
- The gallery shows the tree: each slot is a branch tip, Y → History opens
  that branch's moments as a strip like the rewind screen.
- On quit, the moments of the session's current line thin out (one per
  minute) and go into a per-game history file with a size cap. The next
  launch's rewind starts with them. Old ones go first; kept ones never go.

## Phase 3: input recording and ghosts

- **Play input log** (`.rgin`): header (system, core id + build, ROM SHA-1,
  frame rate), the start state (deflated), then per frame: buttons u32, the
  sticks when the system has them, pointer/keys when used (run-length coded,
  so a held button costs nothing). It is recorded from the moment a ghost run
  starts (a "Record" chip on the rewind screen, or a split timer start).
- **Replay check**: the log ends with a hash of the state at its last frame.
  A replay that does not match says so. That is the determinism test per
  core, run in CI on the test ROM and by hand per system on the handheld.
  Cores known to drift (threaded renderers) do not offer ghosts.
- **Ghost on the second screen**: a shadow core loads the start state and is
  fed the log one frame per live frame, drawn on the lower panel with its
  timer. The live game starts its run from the same state, so both begin
  together. Cost: twice the emulation, fine for GB/GBA/NES/SNES/MD on the
  A55. The N64 and up do not offer it.
- **Sending**: a ghost is a small file (start state plus a few KB of input).
  It goes through the network plan's transfer queue (LocalSend, the web
  upload page) and PictoChat's room as an attachment. Received ghosts land in
  `userdata/play/ghosts/<game>/` and show in the rewind screen's Ghost tab.
  The game's ROM hash and core build must match, or the ghost is refused
  with a reason.
- DS ghosts (Mario Kart DS): DSIN already replays, but the DS uses both
  screens, so the ghost would need a third place. Options: the ghost's top
  screen as a small window over the lower panel, or an on-track marker drawn
  from the ghost's RAM position (needs the profile's addresses). Decide after
  phase 3 works in Play.

## Phase 4: speedrun mode

- Profiles get a `[splits]` section: `start`, `reset` and `split.N = Name |
  condition` lines, the conditions in the existing `parse_condition` grammar.
  Each split fires once, on the frame its condition first holds.
- The timer counts emulated frames (game time, not wall time), so a menu or a
  rewind never adds time. Rewinding during a run marks it as practice.
- Splits on the top screen: a small panel over the game's border (the frame
  area), or the lower screen when the frame has no room. Gold and red per
  split against the best.
- At each split a state is kept: "Practice from split N" loads it and times
  only that segment. Best segments are kept per split.
- The best full run is kept as a ghost (phase 3) and can play beside the
  next attempt.
- Stored in `userdata/play/speedrun/<game>.ini` (splits, best times, gold
  segments) with the ghost and practice states beside it.

## Phase 5: the DS beyond rewind

DS rewind is in phase 1. Left: ghosts from DSIN logs and speedrun splits from
the DS profiles' RAM reads, with the same files and screens as the other
systems.

## Open questions

- A hotkey for rewind (a held combo) without opening the game menu, if owners
  ask.
- Whether to deflate against the previous moment (XOR delta) to keep more
  minutes in the same memory. It makes eviction harder. Measure on the device
  first.
