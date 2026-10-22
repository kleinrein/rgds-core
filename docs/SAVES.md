# Saves and changing emulators

RGDS preserves each engine's saves under userdata across application updates and
when switching away and back. **Changing engines does not transfer progress.**
There is no automatic shared-save or conversion feature yet.

## Two different kinds of save

- **In-game/battery save:** progress saved using the game's own Save command.
  Compatible raw DS cartridge saves can potentially move between engines, but
  wrappers, save-chip sizes and naming differ. A file extension alone is not
  proof of compatibility. RGDS has not validated a cross-engine import matrix.
- **Checkpoint/save state:** a snapshot of one engine's CPU, memory and devices.
  RGDS checkpoints belong to the engine that made them. A DSperate `.dss` must
  not be loaded into DraStic or melonDS just because the suffix matches.
  Even engine upgrades can change snapshot compatibility.

Auto-save/Auto-load in Library control checkpoint behavior; continue using the
game's normal Save command as well. Menu save acknowledgement verifies that a
state file was produced and copied, not that every game's battery save works.
Hardware restoration checks remain pending for the current candidate.

## Save gallery, import/export and backup

Open **Game menu → Save checkpoint** or **Load checkpoint** while playing, or
**Game options → Save gallery** from the library. There are nine slots per game
and emulator. A captured top-screen preview and timestamp identify each slot.
Engines without capture support show a save symbol. The optional SuperDrastic
backend retains its own native save menu.

Every slot answers the same three buttons, and the footer names them:

- **A** does what the page was opened for: *Save here* on the Save checkpoint
  page, *Load* on the Load checkpoint page, *Export* in the library gallery.
  Replacing or loading an occupied slot asks first, with Cancel selected. Slots
  the page cannot use, such as empty slots when loading, are dimmed.
- **X** deletes the focused slot after asking. Deleting removes the slot entry
  only; the checkpoint object is kept, so Undo restores it.
- **Y** opens the rare actions for that slot. In game: *Export slot*, *Undo last
  slot change* and *Undo last load*. In the library: *Import into this slot*,
  *Undo last slot change* and *Import help*.

**Undo last slot change** swaps back the most recent save, replace or delete.
**Undo last load** restores the checkpoint captured immediately before loading
another slot. The engine must acknowledge a fresh save before loading can
proceed; a failed capture changes no slot. Only the most recent slot change and
load are retained as undo points.
Immutable checkpoint objects are verified by SHA-256 before use, and catalog
publication is atomic. Old objects are retained; no automatic pruning deletes
save history in this preview.

**Export** writes a `.rgdssave.zip` to `userdata/exports/`. Copy it to another
installation's `userdata/imports/`, open that game's gallery in the library and
choose **Y → Import into this slot**. The importer verifies the complete ROM hash, engine identity and state
checksum; unsafe archive paths and oversized archives are rejected. It refuses
cross-engine imports. Engine versions can still have incompatible state formats.

**Settings → Library → Back up all saves** creates a ZIP in `userdata/exports/`
while no game is running. It contains slot history, canonical checkpoints and
known native save/state directories, plus a SHA-256 inventory. It excludes games,
BIOS and network credentials. Copy the archive off the card for a separate backup.
Whole-backup restore is currently a manual offline operation; portable slot
restore is available through the Import into this slot action. Neither path converts battery
saves between engines.

## Resume after power off

Enable **Settings → System → Startup → Resume after power off**. Return home with
a successful save, then use RG DS Core's Power off action. A verified ticket names
the last cleanly saved game, its engine, ROM hash and checkpoint hash. On the next
direct boot it is consumed before one launch attempt. Failed attempts cannot repeat
on every boot, and an OTA trial opens the launcher first to complete its health check.
Starting another game invalidates the old candidate. Stock shutdown, forced power
loss and an unsuccessful save do not manufacture a new resume checkpoint.

## Locations

All paths below are relative to the installation's writable `userdata/` folder,
which may be on internal storage or the game card. They are outside versioned
application releases.

| Engine | Native in-game saves | Native states |
| --- | --- | --- |
| DraStic | `emulators/drastic/backup/` | `emulators/drastic/savestates/` |
| DSperate | `emulators/dsperate/saves/<ROM basename>.sav` | `emulators/dsperate/states/`, using the DS game code |
| melonDS | `emulators/melonds/saves/<RGDS game key>.sav` | `emulators/melonds/states/<RGDS game key>.dss` |

RGDS stores its independent checkpoint as
`library/<game key>/auto-<engine>.dss`. When automatic loading replaces an
existing native manual slot, the previous different contents are retained under
`library/<game key>/manual-state-backups/<engine>/`.

DSperate also has its own native automatic state, `<GAMECODE>.auto.dss`.
When Auto-load is enabled, RGDS selects the newer matching canonical or native
automatic checkpoint and loads that exact file before gameplay; canonical
wins equal timestamps. A newer native `.sav` prevents either checkpoint from
rolling back in-game progress. Auto-load disabled bypasses both automatic
paths. Loading does not replace a native automatic state or manual slot.
Before selection, a bounded v4 DSST header check compares the stored game code
and the core's ROM identity (header plus image size). A newer native state for
another ROM revision cannot override the matching per-game checkpoint. Files
with an unsupported/truncated header or mismatched identity remain preserved;
when neither candidate matches, RGDS starts the game normally. This is an
identity check, not a replacement for the emulator's full state validation.

On first preparation, the stock DraStic adapter imports missing files from its stock home into RGDS's
private DraStic home; it does not overwrite an existing private save. That is a
same-engine bootstrap, not cross-engine synchronization.

The RGDS game key currently depends on the original game path (the ZIP path for
an archived game). Renaming or moving games can change that identity. Some
native engines use only a basename/game code, so equal filenames or different
revisions are another reason not to merge save folders blindly.

## DraStic backend switching

[SuperDrastic](SUPERDRASTIC.md) shares the Standard backend's verified DraStic
executable, save home and automatic checkpoint. It does not copy progress into
another emulator. Automatic loads use a staging copy; confirmed exit saves
retain the previous checkpoint under `checkpoint-backups/`. A newer native
`.dsv` prevents an older automatic checkpoint from loading in either backend.
Hardware restoration between these two paths remains pending.

## Before attempting a manual import

1. Make a normal in-game save, exit cleanly and back up the whole source and
   destination save folders. Never edit files while either game is running.
2. Use the same game/region/revision and verify the destination's expected raw
   save format and size. Import into a copy; preserve both original histories.
   Do not simply rename an unknown `.dsv` or a checkpoint to `.sav`.
3. Disable Auto-load for the first destination boot. An old checkpoint can
   restore older cartridge memory and undo an otherwise valid battery import.
4. Check progress in the game's own Load/Continue screen, save again, exit and
   relaunch before treating the transfer as successful.

A future importer should identify ROM content, validate each engine's format,
back up both sides, reject conflicts and publish atomically. It must be tested
with actual saved progress before automatic sharing becomes an option.

### DSperate save acknowledgement

The verified packaged DSperate source snapshot closes `SLOT.tmp` and renames it
onto `SLOT.dss`. RGDS can therefore acknowledge the new slot at the next bridge
poll (normally within 50 ms) instead of applying the generic 600 ms minimum and
350 ms stable-file delay. This removes a wait after writing; it does not remove
checkpoint serialization, durable copying, or the emulator's final native auto-save.

This capability is granted only after release verification, for the packaged
binary whose verified build record matches the reviewed source commit and patched
source fingerprint. A binary override, unverified development tree, changed source,
and every other emulator retain the existing save protocol. The bridge requires a
new inode for a replaced slot, refuses a remaining `.tmp` file, and checks the
complete DSST v4 chunk boundaries through the final `VIEW`/optional `CHVO` chunk.
An unchanged old state, in-place rewrite, truncated replacement, or failed durable
copy cannot produce a successful acknowledgement on this path. Canonical file and
parent-directory `fsync` still finish before `saved`/`quitting` is reported.

The contract comes from pinned DSperate
[`save_state_file`](https://github.com/beebono/DSperate/blob/1b76c355109c9f7576363ccc023927b3137d3c6f/src/frontend/sdl/main.cpp#L492)
and its [v4 chunk format](https://github.com/beebono/DSperate/blob/1b76c355109c9f7576363ccc023927b3137d3c6f/src/core/state/state.h).
The reviewed patched source SHA-256 is
`c8f4a7243832dd609cca4b0b33fed3120fadbf0a3106c0f8b497981c91ec9129`.
The structural check also rejects a truncated file if the native writer's unchecked
`fclose` encountered a flush error. It does not replace the emulator's semantic
state-load validation or promise power-loss recovery beyond the filesystem's
actual guarantees. Native exit auto-save remains enabled for normal and unexpected
emulator exits.
