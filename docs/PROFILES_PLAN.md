# Profiles on RG DS Core: the plan

Status: 2026-11-05. Phases 0 to 5 are built and uncommitted. They ran on the owner's handheld on a copy of userdata, from
a RAM overlay, on real engines (section 10 has what was and was not covered); no image holds them yet. Translations are
mine and have not been reviewed by native speakers. How to use it is in [PROFILES.md](PROFILES.md). Code facts were read from `develop` and tag `v0.8.0`, and the rating numbers
from the live catalogs the handheld already uses (GameTDB, libretro-database). The audit ran against the owner's handheld
on 0.8.0 over SSH, reading folder listings and `info.ini` only. The owner confirmed the decisions in section 12.

The goal: share one handheld with a child. Each person gets their own saves, their own library and their own look. A
child's profile shows age-appropriate games, can be locked with a PIN, and cannot change the device.

## 1. What it does

- **Profiles, like Netflix.** Up to six, each with a name, a picture and a DS favourite colour. A "Who's playing?"
  screen at boot, and a profile chip on Home to switch.
- **Two kinds of profile.** Adult has its settings open. Kid is locked down and filtered. Both are optional beyond the
  owner's own, so a second adult can have a separate profile too.
- **Saves never mix.** In-game saves, auto-save resume points, slots, playtime, favourites and recents all belong to one
  profile (the full list is in section 3).
- **A DS name per profile.** DS games and PictoChat see that profile's name, not the owner's (section 3).
- **Age filter.** Set an age on a profile and games above it disappear from it, everywhere, by themselves.
- **Hide or allow by hand.** Any game can be forced hidden or shown per profile, which beats the age filter.
- **PIN lock.** A PIN keeps a child out of the owner's profile and out of the device's settings.
- **Nothing changes until you opt in.** A handheld with one profile looks and behaves exactly as it does today, with no
  new screen and no file moved.

Separate and independent: **Games opens all games** (section 8), a flat carousel with a System filter in place of the
Systems step. It can ship first.

## 2. What the code forces

| Finding | Where | Consequence |
| --- | --- | --- |
| The data partition is **exFAT**, and every file has mode 0777 | `rcS:44`, `firstboot:19` | No symlinks and no permissions. Separation has to be in the paths we choose. A PIN can only be a gate in the UI |
| Use as USB drive hands that partition to a computer | `usr/libexec/rgds/usb` | Anyone with the card and a computer can read everything. This is the threat model's limit, and also the way to recover a PIN |
| An update never touches the data partition, and **a failed update boots the previous build on the same data** | `docs/OTA.md` | Both builds must read one tree, so any migration is a risk. The design below has none |
| `library/<key>/` mixes artwork with saves | `library_cache.hpp`, `save_library.py` | One folder holds `box.bmp` and `info.ini` (about the game) next to `auto-*.dss`, `slots/`, `stats.ini`, `game.ini` (about the player). It has to be split by file, not by folder |
| Every helper takes one `--data` | `game_launch.hpp:172`, `worker_service.hpp:94`, `game_menu.py:500`, `emulator_bridge.cpp:326` | There is no "whose data" concept to extend, so it has to be introduced |
| The engine guardian cleans the environment | comment at `emulators.py:791` | An `RGDS_*` variable will not reach everything, so the active profile must be a file |
| Page numbers and section ids are stored in the state file | `state_persistence.hpp`, `settings.hpp:591` | New pages are appended to the enum. A restored page must be checked against the lock |
| `all_games` is already the single source for lists | `systems_play.hpp:59`, and every use of it | Search, facets, Recents, the switcher and Last played all derive from it, so one filter there covers them. `load_library` sets `model.games` before it runs: a test must pin that |

## 3. The core design: two roots

Rejected, and why:

- **Moving saves into `profiles/<owner>/`.** It breaks the fallback build and risks the one thing that must never break.
- **Symlink or folder swapping.** exFAT has no symlinks, and a power cut during a rename swap could put a child's game in
  the owner's folder.
- **Bind mounts per launch.** They work on exFAT, but cannot split `library/<key>/`, cannot be tested on a Mac, and fail
  silently into the wrong folder.
- **One launcher `--data` per profile.** Device state (`session/`, Wi-Fi, updates) would fork, and the scraper would
  download artwork once per profile.

**Chosen: a device root and a player root.**

- `device` is `userdata/`, as today.
- `player` is `userdata/` itself for the owner, and `userdata/profiles/<id>/` for anyone else.

So the owner's profile **is** today's tree, with every path identical. This gives the property the whole plan rests on:
**a mistake in sorting a path into device or player can only affect a new profile, never an existing user's saves.**

### What belongs to whom

| Device root: one copy | Player root: one per profile |
| --- | --- |
| `session/ device/ update/ scraper/ logs/ cache/ support/` | `preferences.ini ui.ini` (look, language, layout, auto-save policy) |
| `system.ini volume.ini boot.ini experience.ini performance.ini` | `ds.ini pictochat.ini` (the DS name, the chat name) |
| `controls.ini` (first version; per-profile can come later) | `system-defaults/`, `library/<key>/controls.ini` (a game's own), `cache/disks/` (a zipped C64 game's high scores) |
| `setup.ini frames/ imports/ exports/` | `resume/ save-history/` |
| `library/<key>/`: `box.bmp icon.bmp info.ini banner.ini synopsis.* snap.bmp title.bmp artwork.ini artwork-status.ini info-status.ini` | `library/<key>/`: `game.ini stats.ini sessions.tsv play.ini ds-input.ini slots/ auto-*.dss* manual-state-backups/` |
| `library/<key>/`: `port.ini port-files.ini touch.ini` (ports are shared) | |
| `emulators/rgds-experimental/panvk.json`, `cheats/usrcheat.dat` (13 MB, one copy is enough), Play's `cheats/` | `emulators/rgds-experimental/{saves,states,home,config.ini}` and the rest of `emulators/rgds-play/` |

Checked against every path built from the data folder in `runtime/`, `src/launcher`, `src/platform` and `play/src`, and
against the folders on the owner's handheld.

### Places in the code that assume one folder

These are what phase 1 has to change beyond a plain path swap. Each is harmless for the owner, where both roots are the
same folder, and wrong for a child if missed.

| Where | The assumption | The change |
| --- | --- | --- |
| `main.cpp:2753-2760` | `system.ini` and `o.data` are derived from the folder holding `preferences.ini` | Pin both to the device root before pointing `preferences` at the player root |
| `emulator_bridge.cpp` (`RGDS_VOLUME_DATA`) | One folder holds `volume.ini` (device) and `ui.ini` (player) | Pass the two separately |
| `port_menu.hpp:141,158` and `play/.../main.cpp:741` | The launcher reads `second-screen.ini` at a fixed path; Play writes it under its `--data` | Both use the player root |
| `save_library.py` | `library/` and `exports/`, `imports/` hang off one `data` | Takes both roots: slots are the player's, zips go to the shared folders |
| `quick_resume.py`, `device_services.py:686` | `experience.ini` (device) and `resume/` (player) hang off one `data` | Given the player root, it finds the shared file itself. The ticket is in that player's folder, so it can only resume that player |
| `save_guard.import_old_ds_save` and Play's `prepare_save` | A game with no save takes one from beside the ROM | **Would have handed the owner's save to a child's first start.** Owner only now (`is_owner`, `--no-save-import`) |
| `play_options.unpacked_disks` | A zipped C64 game's disks, where it writes its high scores, are in the shared cache | In the player's `cache/disks/` |
| `emulators.py:115` | `usrcheat.dat` is copied into the engine's folder | Read it from the device root, or every profile gets a 13 MB copy |
| `item_folder`, `folder_for_game`, `LibraryCache::folder` | One folder per game | They keep meaning the shared folder, which is most uses (art). A new `player_folder` serves `game.ini`, `play.ini`, `slots`, `ds-input.ini` and a game's `controls.ini` |
| `sleep_sounds.py`, `hud_theme.py`, `emulator_profiles.py`, `emulators.py:1071` | Read `preferences.ini` and `ui.ini` from `data` | Use the player root |

exFAT on this card has 128 KiB clusters, so each small file and folder takes 128 KiB (measured with `du`). A game played
on a child's profile costs under 1 MB in small files, which is fine with 40 GB free, but it is why `visibility.ini` stays
one file and why folders are created only when something is written to them.

### The DS name

How it works today: `ds.ini`'s `nickname` is the handheld-wide name, and a game's own `ds-input.ini` wins over it
(`emulators.py:514`). The launch writes it into `[user] nickname` of DSCore's config, and DSCore's generated firmware
carries it to the game. The limit is ten UTF-16 units. `save_owner_name` writes the same name to `pictochat.ini`.

Both files are in the player root, so once `ds_owner_name`, `save_owner_name` and `emulators.py:514` use that root, each
profile has its own name with no change to DSCore.

- **Profile editor:** a **DS name** row, filled in from the profile's name and cut to ten units (the editor's existing
  field 7 enforces that), and asked for when a profile is created. The owner's stays what `ds.ini` says today.
- **Kid profiles:** the row is in the PIN-locked editor. A game's own **DS nickname** row under DS controls stays open.
- **Also carry the favourite colour.** The launch passes only language and nickname today. Passing the profile's colour as
  `[user] colour` makes a DS game's own colour match. The launcher and DSCore name colours 9-15 differently, so check them
  on screen. The birthday can wait.
- **What a rename does not change.** A resume point holds the firmware it was saved with, and games may copy the name into
  their own save, so a rename shows after a fresh start. Which games keep the old name is not tested. New profiles don't
  run into this, since their saves start empty.
- **Test:** a child's launch plan writes the child's name, and the owner's `config.ini` and `games/*.ini` stay unchanged.

### On disk

```
userdata/                      the owner: exactly as on 0.8.0, nothing moved
  profiles/                    absent until a second profile is created
    index.ini                  order, ask-at-boot, the owner's name, picture, colour and PIN
    active                     the id of the current profile
    pin-state.ini              wrong tries and the wait, so a restart does not reset them
    p1/
      profile.ini              name, picture, colour, restricted, age, unrated, permissions, PIN
      visibility.ini           game key = hide | show
      preferences.ini ui.ini ds.ini ...
      library/<key>/...        only the player's files
      emulators/...            saves, states, engine homes
      save-history/ resume/
```

Ids are short and never change (`p1`), so renaming a profile moves nothing and no name reaches the filesystem.

### How the code reaches it

- **`runtime/profiles.py`**: `player_root(data)`, read from `profiles/active`, with the same rule in `launcher/profiles.hpp`.
  The launcher, its helpers, the menu helper and the runtime all read this one file, and only the launcher writes it, never
  while a game runs (`core_running`).
- **`LibraryCache`** gets two folders: `metadata` reads the device root; `stats`, `options` and `saved_options` read the
  player root. `CollectionFilter`, `LibrarySummary`, `FacetIndex` and `search_preview` construct it, so they follow.
- **`emulators.py launch_plan`** resolves `player` once. `session/*` and device files use `data`; `library/`, `emulators/`
  and the bridge's `RGDS_AUTOSTATE_FILE`/`RGDS_STATE_DIR` use `player`.
- **`save_guard`, `save_library`, `quick_resume`, `record_playtime`** already take `data` and work relative to it, so they
  are handed `player` and need almost no change. `save-history` then lands inside the profile by itself.
- **Scraper and `game_info`** always use the device root: artwork is shared, fetched once.

### What each engine needs

| Engine | Player data today | Change | What the audit found |
| --- | --- | --- | --- |
| DSCore | `emulators/rgds-experimental/{saves,states}`, config in `home` | `root` from `player` | `home/.config/dsperate/games/` holds per-game config with the nickname, so it belongs to the player. `panvk.json` and `usrcheat.dat` go to the device root |
| RG DS Play (GBA, GB, libretro) | `--data emulators/rgds-play`: `saves/<system>`, `states/`, `home/`, `*.mcd`, `second-screen.ini`, `TEMP/`, `cdata/` | One argument | Everything found fits under that one root. Whether any core writes beside the ROM is not settled by a listing: the per-engine run in section 10 covers it |
| 3DS (Tanuki) | `HOME=emulators/rgds-play/3ds` | `home` from `player`, **plus a seed** | **`shaders/*.list` and `jit/*.list` live in `HOME`** (6 MB). They are what makes Mario Kart 7 run at speed, so a new profile must start with them. **`3ds/sdmc/3ds/dspfirm.cdc` lives there too** and is firmware, not a save |
| PSP (PPSSPP) | `emulators/rgds-play/PSP/{SAVEDATA,PPSSPP_STATE}` | Follows Play's `--data` | `PSP/SYSTEM` is 384 KiB of settings and cache. Seemingly nothing needs seeding; a first launch on a copy confirms it |
| **Ports** | `portmaster/home` and each port's own folder | **Cannot be separated** | Ports share saves across profiles, are off for Kid profiles, and the Ports page says so |

**Seeding the 3DS.** Before a launch on a non-owner profile, copy the owner's lists and `dspfirm.cdc` into the profile,
for files it does not have yet, using the existing `copy_missing`. No symlinks exist on exFAT, and the engine appends to
these lists while it plays, so a shared folder would need a change in the engine. Installing a new engine must be avoided
for this work anyway, since that has invalidated resume points before.

**Shader caches.** The session already sets `MESA_SHADER_CACHE_DIR=/data/rgds/cache/mesa`, which is shared, and it had 112
files written in the last three days. The 35 MB and 21 MB `home/.cache` folders in the two engines' homes were last written
on 2 November and 27 October, so they look like leftovers. No change is made; a real launch under a child's profile confirms
it.

**A disk a game writes to.** A loose C64 or ZX disk image in the games folder is the game's own file, so anything a game
saves onto it is shared between profiles. Most games keep their saves in the player's folder, but this is a second
exception beside Ports.

### Every kind of save, per profile

Owner paths are shown; another profile has the same paths under `profiles/<id>/`.

| Save | Where | Per profile |
| --- | --- | --- |
| In-game save, DS | `emulators/rgds-experimental/saves/<ROM>.sav` | Yes |
| In-game save, GBA, GB and libretro systems | `emulators/rgds-play/saves/<system>/` | Yes |
| In-game save, PS1 memory cards | `emulators/rgds-play/*.mcd` | Yes |
| In-game save, PSP | `emulators/rgds-play/PSP/SAVEDATA/` | Yes |
| In-game save, 3DS | `emulators/rgds-play/3ds/.local/share/Tanuki3DS/3ds/{savedata,extdata,sdmc}` | Yes |
| Auto-save resume point | `library/<key>/auto-<engine>.dss` | Yes |
| The engines' own states | `emulators/rgds-experimental/states/`, `emulators/rgds-play/states/`, `PSP/PPSSPP_STATE/` | Yes |
| Save gallery slots | `library/<key>/slots/` | Yes |
| Copies taken before each launch | `save-history/` | Yes |
| Resume after power off ticket | `resume/` | Yes |
| **Ports** | `portmaster/home` and the port's own folder | **No:** the one exception, see above |

Phase 1's audit test covers each row: a game started in a child's profile must write its save and its resume point inside
that profile, and the owner's copy of the same game must stay byte-identical.

### Guards

1. **At launch, refuse rather than mix.** For any non-owner profile, `launch_plan` checks that each save, state and
   `HOME` path in the plan is under the player root. If not, the game does not start, with a clear error.
2. **An audit test** builds a plan for every system under a child profile in a temp tree and fails on any path into the
   owner's player files. A new engine or path that forgets the rule fails here.
3. **Fail closed.** If `profiles/index.ini` exists but `active` is missing or names an unknown profile, open the picker.
   Never fall back to the owner unlocked.

## 4. Coming from 0.8.0 by OTA

The owner's tree is untouched, so there is no migration to go wrong.

| Situation | What happens |
| --- | --- |
| Update to this build, no profiles created | No `profiles/` folder and no new screen. Every path is the same as before |
| New `ui.ini` keys | Absent means today's behaviour. 0.8.0's loader ignores unknown keys, though it drops them if it saves settings after a fallback, which only resets the new option |
| First profile created | The owner's name comes from `ds.ini`. Only `profiles/` is added |
| Update fails and falls back to the old slot | The old build ignores `profiles/` and opens the owner, with no lock, while the update has failed. Saves on both sides are intact |
| Owner goes back with Use previous build | Same. Children's saves wait in `profiles/` for the next upgrade |
| `Back up all saves` | **Must gain `profiles/*`** or a child's saves silently miss the backup. Part of phase 1, not a polish item |

**Release gate:** `tests/test_profile_upgrade.py` with a fixture tree in 0.8.0's layout (saves, states, slots, resume
ticket, 3DS and PSP folders). It hashes every file, runs the launcher and a launch plan as the owner, creates and then
removes a profile, and requires every hash to match, apart from a short list of files that are allowed to change, such as
`stats.ini`. `own_os_release.py` runs it, as it does `resume_check.py`.

**Removing a profile never deletes saves at once.** It writes a backup zip to `exports/`, then moves the folder to
`profiles/.removed/<id>-<time>/`. Erasing it for good is a separate action on the owner's page, with its own warning.

## 5. The age filter

### What the catalogs really hold

Measured 2026-11-05, counting each dump or code as one. This is the catalogs as a whole; the owner's real library comes
after it.

| System | Source | Usable rating | Note |
| --- | --- | --- | --- |
| DS | GameTDB | 4171 of 8460 (49%); **66%** borrowing another region's | 4289 entries name a board with an **empty value** |
| 3DS | GameTDB | 1803 of 4785 (38%); **70%** borrowing | Most of the list is eShop titles |
| GBA | libretro ESRB | 1840 of 3503 (53%) | |
| Game Gear | libretro ESRB | 381 of 607 (63%) | |
| N64 | libretro ESRB | **1 today**; 1834 in the list we do not ask for | `CATALOGS` names `Nintendo 64`; the data is in `Nintendo 64 (BigEndian)` |
| GBC | libretro ESRB | 895 of 2433 (37%) | |
| SNES | libretro ESRB | 1156 of 3730 (31%) | |
| Mega Drive | libretro ESRB | 760 of 2656 (29%) | |
| GB | libretro ESRB | 259 of 2115 (12%) | 109 more say `NOT RATED` |
| Master System | libretro ESRB | 106 of 1057 (10%) | |
| NES | libretro ESRB | 571 of 13286 (4%) | |
| **PS1** | libretro ESRB | **6 of 13407** | Effectively none |
| **PSP, ZX, C64, G&W, ScummVM, Ports** | none | **0** | ScreenScraper, which has ratings, was removed from the runtime |

Also: ratings only exist after **Fetch missing artwork** has run online, and `auto_scrape` is off by default.

**The conclusion that drives the design:** filtering only games that have metadata would let through about half the DS
library, nearly every PS1 game and every PSP game. Mature PS1 and PSP titles would reach a child's profile precisely
because nothing is known about them. Unrated has to be an explicit choice per profile, and the default for a kid profile
is hidden (decision 1).

### On the owner's real library

`tools/rating_audit.py --device`, reading what the handheld has saved today:

| System | Games | Rated | Unrated | 3+ | 7+ | 10+ | 12+ | 13+ | 18+ |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| DS | 56 | 38 | 18 | 29 | 2 | 1 | 5 | | 1 |
| GBA | 26 | 21 | 5 | 18 | | | | 3 | |
| GBC | 7 | 6 | 1 | 6 | | | | | |
| GB | 7 | 1 | 6 | 1 | | | | | |
| 3DS | 4 | 3 | 1 | 3 | | | | | |
| NES | 7 | 4 | 3 | 4 | | | | | |
| SNES | 3 | 1 | 2 | 1 | | | | | |
| Mega Drive | 3 | 1 | 2 | 1 | | | | | |
| Game & Watch | 59 | 0 | 59 | | | | | | |
| Ports | 6 | 0 | 6 | | | | | | |
| ScummVM | 3 | 0 | 3 | | | | | | |
| N64, PS1, PSP, ZX, C64 | 5 | 0 | 5 | | | | | | |
| **All** | **186** | **75** | **111** | **63** | **2** | **1** | **5** | **3** | **1** |

So a Kid profile set to age 7 would start with **65 games visible, 10 above the age, and 111 waiting for a decision.**

- **The unrated games are not the ones you would expect.** Among them are Peppa Pig, Hello Kitty, Littlest Pet Shop,
  Crayola Treasure Adventures, Nintendogs, Sam Power, Pokémon Blue and Yellow, Super Mario World and Super Mario Kart.
  Many of the games a child would actually play are the ones the catalogs know least about. The parent's approval step is
  therefore the normal way to set up a Kid profile, not an edge case (section 7).
- **The fixes help, but not much.** Against the games on the Mac, with the fixes below, region borrowing rated 2 more of 56
  DS games and the N64 list fix rated the one N64 game (so a `.z64` CRC does match that list). Borrowing by title across
  ESRB lists rated nothing more, so it is not in the plan. The catalog-wide gains above do not carry over to this library.
- **Six DS games, three GBA, two NES and the single 3DS, C64, PSP, PS1 and ZX game were never looked up.** Fetching costs
  nothing to try, and the Game & Watch set has no catalog at all.

### Fixes to `game_info.py`, all additive

1. A board with no value (`"ESRB"`) is no rating. Today it is saved and shows up as a Rating filter value: 12 of the owner's
   games have one.
2. **Borrow from another region:** with no rating of its own, a DS or 3DS game takes the strictest rating among codes with
   the same first three letters. Stored with `rating_source=region`, so the page can say so.
3. Fetch N64 from the `(BigEndian)` list.
4. Refetch only games with no usable rating, so the bump does not redo a whole library.

### From a rating to an age

One table in `launcher/age_rating.hpp`, tested against every value measured above:

| Minimum age | ESRB | PEGI | CERO | GRB / CSRR |
| --- | --- | --- | --- | --- |
| 3 | EC, E, KA | 3 | A | ALL |
| 6 / 7 | | 6 / 7 | | |
| 10 | E10+ | | | |
| 12 / 13 | T (13) | 12 | B (12) | 12 |
| 15 / 16 | | 16 | C (15) | 15 |
| 17 / 18 | M (17), AO (18) | 18 | D (17), Z (18) | 18 |
| Unrated | `NOT RATED`, `RP`, empty | empty | empty | empty |

The profile stores a plain age, not a birth date: the clock cannot be fully trusted, and the owner just raises the age.

### The rule, in order

1. Forced `hide` → hidden. 2. Forced `show` → visible. 3. No age limit → visible. 4. Rated → `minimum age <= profile age`.
5. Unrated → the profile's `unrated` setting. Ports, PortMaster and Homebrew go by permission (section 6), never by rating.

The age picker also offers **Only games I pick**, an allow-list for a small child: everything is hidden unless forced
`show`.

### Where it runs

`refresh_other_systems` keeps the full list as `catalog_games` (for the curation page) and fills `all_games` with what
passes. Empty systems drop off `listed_systems`.

- **No overrides and no age limit (every existing user):** the filter does nothing, at no cost.
- **Overrides only:** a `game_key` lookup per game, no file reads.
- **An age limit:** reading thousands of `info.ini` files is slow on a card, so it runs on a worker (`CatalogReader`).
  **Until it finishes the profile shows no games,** never the unfiltered list. There is no `visible.cache`: a stale cache
  could let a hidden game through, and whether the read is slow enough to need one has not been measured on the handheld.
  New ratings from a download apply when the player is next back at a list.

As a last check, `App::launch` asks whether the game is allowed, covering any route the list filter missed.

## 6. PIN and lock-down

### The threat model, stated plainly

This stops a child using the handheld. It does not stop someone with the card and a computer, because the partition is
exFAT and readable by anything. Ratings and PINs are a parental gate, not encryption, and the docs will say so.

### The PIN

- Four digits on a keypad on the bottom screen, with touch or D-pad and A. Digits are hidden.
- Any profile may have one. The owner's is what makes a child's profile actually locked: creating the first restricted
  profile asks for it. It can be skipped, with a line saying the profile is then not locked.
- Stored as a salted, iterated SHA-256, so it cannot be read in a text editor. A four-digit PIN can be brute-forced
  offline, so this is no more than that.
- Five wrong tries wait 30 s, doubling to 5 minutes, kept in `pin-state.ini` so a restart does not clear it.
- **Forgot it:** create an empty file `rgds/reset-pin` on the card from a computer. At boot the launcher removes the
  owner's PIN, deletes the file, and says so. That is deliberately a job for a grown-up with a card reader.

### Who is what

Creating a profile starts with one choice, **Adult** or **Kid**. On disk that is `restricted=0` or `1`, and "restricted"
in this plan means Kid.

| | Owner | Adult | Kid |
| --- | --- | --- | --- |
| Who | Today's `userdata/`, cannot be removed | A second adult or an older child | A child |
| Own saves, look, favourites | Yes | Yes | Yes |
| Settings | Open, plus managing profiles | Open, but managing profiles needs the owner's PIN | Locked, see below |
| Age limit | None | None by default, can be set | Set when created |
| Games with no rating | Shown | Shown | Hidden by default |
| Games opens all games | Off, as today | Off | On |
| PIN to enter | Optional | Optional | Optional |
| Enter other profiles | Any, with no PIN | Asks for that profile's PIN | Asks for that profile's PIN |

On a Kid profile, PIN-locked rows offer the owner's PIN in place, unlocking until you leave Settings, so a parent can fix
the Wi-Fi without switching profile.

### One checkpoint, not many doors

`enforce_lock` runs before each frame is drawn. It looks at the page a Kid profile is about to see, against a list of what
is open: anything not on the list asks for the owner's PIN, then continues to that page. So a shortcut, a restored state or
a page added later is closed by default. It cannot see actions that are not pages, so those check for themselves: the
question at boot, PictoChat, local wireless and update prompts. In a game's menu there is no PIN pad over the game, so what
is off stays off. Without an owner's PIN a Kid profile is filtered but not locked, and its page says so.

A child cannot enter a grown-up's profile just because it has no PIN of its own: the owner's stands guard. Whoever played
last still needs their PIN at boot, and turning the question off does not skip that.

### What a Kid profile can reach

| Area | Kid |
| --- | --- |
| Theme, Display, Sound, Statistics (its own), Power off | Open |
| Power, Library, Connectivity, System, Updates, Controls | Locked with the PIN |
| Settings → Profiles | Switch only; managing needs the PIN |
| Game options: favourite, shader | Open |
| Game options: artwork, engine, Save gallery export and import | Locked |
| In-game menu: save, load, sound, controls view, game switcher | Open; the switcher lists only its games |
| In-game menu: cheats | Off, unless permitted |
| PictoChat, DS local wireless | Off, unless permitted (a per-profile switch) |
| Ports and PortMaster, Homebrew shelves | Off, unless permitted; Homebrew downloads are always locked |

### Every way out, each needing a test

| Route | How it is closed |
| --- | --- |
| Settings tile, Y on Home, the update banner | Banner and hotkey hidden; the update itself still downloads in the background per its setting |
| Home key search, Anbernic button, Last played tile, Recents | Derive from the filtered `all_games` |
| In-game switcher | `session/switcher-library.ini` is written from the filtered list; `save_switcher_library` also adds the selected game, which is already filtered |
| Restoring the saved page at boot | `restore_state` opens the page only if the lock allows it. The handheld's session passes no `--state`, so today this is only a development path |
| Resume after power off | The ticket lives in the player root, so it can only resume that player, and must not skip a PIN. Nothing on RG DS OS calls `quick_resume.consume` today, so this needs a test once that is settled |
| Network file sharing left on | Turned off on a Kid profile, in the checkpoint. **USB networking is left on, changed from the first plan:** the setting is kept across restarts, so turning it off would leave it off for the owner too; it needs a computer and the owner's SSH key, and Use as USB drive starts only from the locked menu |
| Port files QR upload, Run setup again, `--settings` entries | Locked, or gone with Ports |
| A keyboard's `Tab` or `c` (development shortcuts straight to Library settings, in `input_handling.hpp`) | Closed by the checkpoint, which is what a Bluetooth keyboard would run into |
| A game on a hidden list launched by path | The check in `App::launch` |
| A game file renamed or moved | Its key changes, and it has no info there. Unrated hidden keeps it hidden. With `unrated=show` it would appear: another reason for decision 1 |

## 7. Screens

- **Who's playing?** (`Page::ProfilePicker`, appended). Tiles on the bottom screen; the top screen shows the selected
  profile's big picture, name, age limit and last game, drawn without animation (upper-screen animation has broken the
  handheld before). A padlock marks a profile with a PIN.
- **At boot:** shown only with more than one profile, after the boot scene, with the last profile selected, so A goes
  straight on.
- **Home:** a small chip with the picture opens the picker. Never while a game runs, and sleep and wake stay in the same
  profile.
- **Switching** reuses the return-from-game path (`finish_launch(true)`: clear the cache, re-request the summary, reload
  preferences) rather than restarting the launcher, plus a reload of theme and language. To be timed on the handheld.
- **Pictures** (2026-11-06): twelve plain silhouettes plus twelve characters (fox to bunny), drawn smooth by
  `tools/build_profile_avatars.py` into `assets/profiles/*.bmp` (192 px, alpha, white sticker edge); the 16 x 16 colour
  pixel versions in `profile_pictures.hpp` stand in if a file is missing. A Kid profile wears a multicolour **Kids** mark.
- **PIN pad** (`Page::PinPad`), shared by the picker and the locked rows.
- **Settings → Profiles** (`Page::Profiles`, `Page::ProfileEdit`): Adult or Kid, name, DS name, picture from the pixel
  icons, colour from the 16 DS colours, Age, Unrated games, PIN, permissions, Remove.
- **A profile's games** (`Page::ProfileGames`): every game with a tick box, its icon and its rating badge. Redone
  2026-11-06 after the owner found the first version "horrible": A cycled Automatic → Always show → Always hide, and on the
  Shown tab a game vanished the moment it changed. Now ticked means shown; `visibility.ini` keeps only what differs from
  the age rule, so unticking a picked game returns it to automatic. Rows are only re-sorted when a filter changes. The
  header has three buttons, Search (X, edit field 11), System (Y, a picker) and Showing (L/R or a picker), and the top
  screen shows the cover (it was never requested before, so it never loaded).
- **Creating a Kid profile ends here,** on the **No rating** tab, saying how many games wait for a decision. It can be
  left and come back to, and the games stay hidden until then.
- **Approve a whole shelf.** Going through 111 games one at a time is too much, and 59 of the owner's are Game & Watch.
  The list groups by system, and a shelf's own tick box ticks every game listed under it that is not rated above the age
  (asking first), or unticks all of them. It works on what is listed, so a search or the No rating view narrows it.
- **Game options, on the owner's profile:** a row **Visible to…** for the same choice from a game's own page.
- **Hiding from yourself:** the same `visibility.ini` works on any profile. With one person, Game options has **Hide this
  game**; with more, **Visible to…**. The filter sheet's **Hidden games** chip lists them, to bring them back. It is not
  offered on a Kid profile, and a hidden game does not start from it.
- **After creating a restricted profile:** if games lack information and there is Wi-Fi, offer to fetch it (the existing
  scrape, `info` only). Otherwise show "312 games have no age information yet and stay hidden".

A new profile starts with a copy of the owner's look and with **Games opens all games** on, since a small curated library
does not need a Systems step.

## 8. Games opens all games (independent)

Built 2026-11-06: `all_games` in `ui.ini`, the row **Games opens** shown under **Systems** only while other systems are
on (so the rows below keep their place for DS-only players), new profiles get `all_games=1`. Each profile's page has
the same choice as row 17 under Games (it also turns other systems on, since that is what it chooses between). `view_source(0)` sorts by
title and honours `browse_system`, set by the filter sheet's **System** chips (`facet` -2); a **Systems page** chip
(`facet` -3) reaches Ports and Homebrew. Not yet tried with a big library on the handheld. The notes below are the plan.

A bool in `ui.ini`, default off, under Settings → Library, per profile once profiles exist.

- With it on, the Games tile goes to `library_view(0)` with every system, skipping `open_systems`.
- `view_source` already returns `all_games` for a search, but that list is concatenated by system: it needs a merge sorted
  by title, kept apart from the per-system lists.
- The filter sheet gets a **System** chip row. `facet_rows` grows by one, and `facets_match` needs the game's system,
  which is on `Game`, not in `info.ini`. Chip focus is by index (see the library view sheet), so the row must exist in both
  modes.
- **PortMaster and Homebrew are not games and only have a door on the Systems page.** They need one here: a tile in the
  filter sheet's top section, with the Systems step still reachable from it.
- Check a big library in the carousel on the handheld: a tester already reported crashes with big libraries, with the
  cause unproven.

## 9. Phases

Each ends with something testable. Nothing is committed by me; each is left for review.

| Phase | Work | Done means | Size |
| --- | --- | --- | --- |
| **0. Audit: done 2026-11-05** | Every path sorted, `tools/rating_audit.py` run on the real library, Tanuki and PPSSPP homes read on the handheld | Section 3 is checked, including what must be seeded. Left for the first launches: the `home/.cache` folders, and cores that write beside the ROM | S |
| **A. All games** | Section 8 | UI tests; carousel on the handheld | S-M |
| **1. Two roots: built 2026-11-05, not yet run with a real engine** | `profiles.py`, `profiles.hpp`, `LibraryCache` split, `launch_plan`, the changes listed in section 3, the 3DS seed, the DS name, backup, launch guard, audit test, upgrade test. No UI; a profile is made by hand: `mkdir -p userdata/profiles/p1`, then `echo p1 > userdata/profiles/active`, and `echo owner` to go back | Owner's save paths identical to 0.8.0's (compared against the tag's own code); child plan clean for 15 systems here and for 6 on the handheld's real environment; backup includes profiles. **Left: a real engine under a child's profile on a copy of userdata,** which takes the panels and needs a new build on the handheld. The colour for DS games waits for phase 2, when a profile has one | L |
| **2. Profiles: built 2026-11-05, host preview only** | `index.ini`, picker, Home chip, switch, management pages, per-profile look, DS name row. `profile_store.hpp`, `profiles_screen.hpp`, `profile_pictures.hpp` | Done in the preview: create, rename, change picture and colour, switch with the look following, remove with a backup; one person sees no new screen and nothing is written. **Left: two profiles keeping separate saves through a real launch on a copy of userdata.** Home's carousel layout has no bottom bar, so there the way to switch is Settings > Profiles. Erasing a removed profile for good waits for phase 5 | M-L |
| **3. Visibility: built 2026-11-05, host preview only** | `game_info` fixes, `age_rating` in both languages, filter (`profile_visibility.hpp`, `apply_visibility`), `visibility.ini`, the profile's games page with shelf approval, age and permission rows, Hide this game and Visible to…, the Hidden games filter | Every route tested: Home counts, systems, library, search, favourites, recents, switcher list, ports and the last check at launch. **Left: reading ratings on the handheld's real library, timed,** and a fetch with the new `game_info`, which no real network run has exercised. PictoChat, cheats and local wireless permissions wait for phase 4 | M |
| **4. PIN and lock: built 2026-11-05, host preview only** | `profile_pin.hpp`, the PIN pad, backoff, reset file, permissions, and one checkpoint (`enforce_lock`) before each frame | One test per route. **Left: real launches and buttons on the handheld** | M |
| **5. Finish: built 2026-11-05, except the hand run** | 96 strings in all 22 languages, `docs/PROFILES.md`, `docs/SAVES.md`, README, release gate, erasing removed profiles (the one place saves are deleted, owner only, asks first) | Suite run; German and Japanese checked on screen. **Left: the hand run on the handheld** | S |

Phases 1-4 ship in one release: without the lock, a "kids profile" would promise something it does not do. Phase 1 can
land earlier because it changes nothing for anyone. Phase A can ship any time.

## 10. Testing

- **Host:** unit tests for `player_root`, the age table (every measured value), the visibility rule and the PIN; launcher
  UI tests in `--preview` for each new page and each route in section 6.
- **Host tests only cover `--preview`,** which has hidden real-handheld failures before, so phases 1, 2 and 4 each get a
  real launch.
- **On the handheld: never the owner's saves.** Launches run on a copy of userdata (`emulators.py --data <copy>`) and never
  take the panels while the owner is at the device. A real run on `/data/rgds/userdata` needs the owner's go-ahead.
- **Each engine, once, under a child profile:** DS, GBA, GB, SNES, N64, PS1, PSP and 3DS. Save in game, quit, resume, then
  confirm the owner's files for that game are byte-identical.
- **Interrupted writes:** power cut during `active`, `index.ini` and `visibility.ini` writes, using the existing atomic
  rename and `fsync`. Each must come back at the picker with nothing lost.

**Handheld run, 2026-11-05** (real pad through `/dev/input/event7`, launcher and engines on a copy of userdata):

- **Ran as a child's profile:** DS (Animal Crossing, Mario Kart DS), GBA (Kirby, Golden Sun), GB (Wario Land II), SNES (Super
  Mario World), PSP (OutRun 2006), 3DS (Mario Kart 7, 60 fps on a new profile with the seeded shader lists). Resume points
  stay apart: as the child OutRun started at its title screen, as the owner in the owner's race; DS loaded the state in the
  child's folder on the second launch. The child's first Mario Kart DS `.sav` differs from the owner's, and the owner's
  folders on the copy did not change.
- **The owner's real `userdata/`, all 2097 files by SHA-256, and `/data/roms` by size and time: unchanged.** Outside the test
  folder only the shared Mesa shader cache was written, so `home/.cache` and writes beside the ROM are settled for these
  engines.
- **Through the real UI:** creating Kid and Adult profiles, name, age, PIN, Allow all, the picker at boot, wrong and right
  PIN, the locks, the game menu, Return home, removing, erasing, `reset-pin`.
- **Timing:** reading every rating with a cold page cache takes about 0.9 s on the worker, so no cache is needed. Removing a
  profile takes 60 to 75 s, nearly all of it the backup.
- **Found only there:** name fields showed stars (they fell through to the password rule), nothing on screen during the
  removal backup, the Cheats tile looked available on a profile without cheats.
- **Not covered:** N64 and PS1, a save made inside a game by playing to one (engines wrote their first `.sav`, states and
  checkpoints), touch, the power cut above, a real OTA from 0.8.0, a real image, any save on the real `userdata`.
- **Not a profile bug, fixed:** a PSP quit could time out. RG DS Play writes the state on the thread that also runs the
  bridge, so a 50 MB OutRun state held every bridge tick; when the write ended past 7 s the bridge answered `save-timeout`
  although the state was whole, and the game kept running. Seen in two of six runs above. The bridge now takes a slot that
  RG DS Play renamed into place at once (no `.writing` file beside it). On the handheld, with 600 MB written beside the quit
  to slow the card: the old bridge timed out with the state written, the new one saved and quit (46 s), and a calm quit took
  7.2 s, just past the old limit. Test: `test_rgds_play_keeps_a_renamed_state_after_a_save_that_held_the_bridge`.

## 11. Risks

| Risk | Answer |
| --- | --- |
| A child's game writes into the owner's saves | The launch guard and the audit test. Owner paths are unchanged, so the reverse cannot happen |
| 3DS state compatibility and engine homes | A 3DS resume point is tied to its engine build; a child's are no different. Do not reinstall the 3DS engine for this work: installing has invalidated resume points before |
| A Kid profile looks nearly empty at first, because most games have no rating | Creating one ends on the approval list, with shelf approval, and says why the games are hidden |
| A 3DS game runs slowly on a new profile | The seed in section 3. Compare Mario Kart 7's numbers on both profiles |
| A slow first filter on a big library | Worker, cache, "Preparing…". Timed on the handheld |
| A forgotten path shows a hidden game | Filter at `all_games`, check again at launch, a test per route |
| Disk space from extra states | `save_guard` already refuses a copy below 256 MiB free. Profile pages show each profile's size |
| Parents think the lock is stronger than it is | Stated in the UI and docs |

## 12. Decisions

Confirmed by the owner, 2026-11-05:

1. **Unrated games on a Kid profile: hidden** (the setting can change it). Filtering only games with metadata would let
   through about half the DS library and nearly all of PS1 and PSP (section 5). A parent approves those games by hand,
   through the **No rating** tab.
2. **"Everyone" counts as age 3,** so a 5-year-old with US dumps still sees Mario Kart DS. Anything a parent disagrees with
   can be hidden by hand.
3. **At boot, ask Who's playing? once there is more than one profile,** last one selected.
4. **Adult and Kid profiles both exist,** so a second adult can have a separate profile with settings open.
5. **Look and language are per profile; controls and volume are per device.**
6. **In-game saves and auto-save resume points are both per profile** (section 3). Ports are the one exception.

## 13. Not in the first version

- Time limits and bedtime, a natural next step from the playtime tracker.
- A volume cap for a child's profile.
- Lock on wake (going back to the picker after sleep).
- Moving a battery save from one profile to another. Slot export and import already works across profiles for
  checkpoints, since it checks the ROM and the engine, not the owner.
- A bundled offline ratings table, so a card with no Wi-Fi has ratings from the start.
- A profile step in the setup wizard.
- An age the owner sets once for a game, shared by all Kid profiles, for a household with more than one child. In the
  first version, approval is per profile.
