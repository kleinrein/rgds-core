# Manuals and maps beside the game: the plan

Status: 2026-11-20. Phase 1 and the PDF part of phase 3 are built and uncommitted (section 12), Mac-verified
only: nothing has run on the handheld yet. Pinch (phase 2), maps, Game info, upload, DS and ScreenScraper downloads
are not built. Code facts were read from `develop` that day.

The goal: a scanned manual or a map (PDF, CBZ or plain images next to the ROM) shows on the lower screen while you
play. Swipe to turn pages, pinch to zoom, drag to pan. It works the same on every system (see the consistency rule),
with one honest exception for DS, whose lower screen is the game.

## 1. What it does

- **Manual and Maps on the second screen.** For every Play system (GBA, GB/GBC, SNES, NES, MD, …) they are two more
  companion views on the existing view switch, beside a profile's boards, the automap and the keyboard.
- **DS: a reading mode.** A hotkey and a game-menu row put both DS screens on the upper panel, the manual on the lower
  one. The same key goes back. Section 5.
- **Pinch to zoom, drag to pan, swipe to turn.** Double tap toggles fit-width and fit-page. With the game running the
  buttons stay the game's; the touch screen is the reader's.
- **Remembers its place.** The last page, zoom and position per game and per document; bookmarks.
- **Formats.** Folders of PNG/JPEG, CBZ (zip of images), and PDF. A scanned PDF is converted once into page images
  (section 4); a PDF with vector text and no page images gets a clear message instead of a blank page in v1.
- **Getting files there.** Drop them next to the ROM, upload them over the existing QR/web upload, or (phase 4) let the
  artwork fetch download the manual.
- **Game info gets a Manual button** that opens the same reader outside a game.

Not in scope: OCR or text search inside manuals, manuals for ports (PortMaster), editing PDFs.

## 2. What the code forces

| Finding | Where | Consequence |
| --- | --- | --- |
| The second screen of Play systems is a list of `View`s with `update`/`draw`/`touch`/`save`/`load` | `play/src/companion/view.hpp` | A reader is one more `View`. `save()`/`load()` already give it per-game state next to the save |
| `Touch` carries one finger: kind, x, y, no id | `view.hpp` | Pinch needs a second contact through the whole chain (next row) |
| The touch decoder tracks 16 slots but reports **only the primary contact**, and ignores the rest until all fingers lift | `src/platform/touch.hpp:118-160` (`primary`, `ignore_remaining`) | The deliberate one-finger behaviour games rely on must stay. A second output (all contacts) is added beside it, used only by the reader |
| The panel controller looks like a Goodix GT911 (5 points as a chip) | kernel config in `os/` | Likely fine, unproven: section 9 |
| Play decodes PNG and BMP, with zlib linked; no JPEG | `play/src/ui/png.hpp` | Scans are JPEG almost always: a JPEG decoder is the one new dependency for images. zlib covers CBZ's deflate |
| The launcher reads only BMP (`LoadBMP_RW`, `bmp_budget.hpp`) | `src/launcher/media.hpp` | The Game info reader either reuses Play's decoders or reads the converted pages (section 4 makes them a format both can read) |
| DSCore already knows stacked, side-by-side, PiP and dominant layouts, and has `layout_next` | `dscore/src/frontend/sdl/main.cpp:145`, `input.cpp:33` | The DS reading mode is a layout that puts both screens on the upper panel; the drawing exists, the two-panel present path has to learn it |
| ScreenScraper media are fetched by type through `mediaJeu.php` | `runtime/screenscraper.py:225` | A `manuel` media type (PDF) is one more kind, if the account and the game have it |
| The data partition is exFAT, files 0777 | `docs/PROFILES_PLAN.md` §2 | Cache folders are plain folders; no symlinks |
| Consistency rule: every system gets the same feature | owner feedback | DS gets a reading mode rather than nothing |

## 3. Where files are found

For a ROM `Games/gba/Golden Sun.gba`, in this order:

1. `Golden Sun.pdf`, `Golden Sun.cbz`, `Golden Sun.manual/` (a folder of images, sorted by name): the manual.
2. `Golden Sun.map.png` / `.jpg`, `Golden Sun.maps/` (a folder, one map per file, named by file): the maps.
3. `library/<key>/manual.*` and `library/<key>/maps/`: uploaded or downloaded ones, so the ROM folder stays clean.

Several manuals (`Golden Sun.pdf` and `Golden Sun (Strategy Guide).pdf`) are listed by name in the view's own picker.
A ROM in a zip uses the zip's stem. Secondary game folders are searched the same way as the main one.

The scan happens with the library scan (cheap: a few `stat`s per game) and is stored as a flag in the library cache, so
Game info and the view switch know there is a manual without touching the card at play time.

## 4. Pages: one format inside

Every source becomes the same thing, a **page set** in `library/<key>/pages/<document id>/`:

- one JPEG (or PNG for line art) per page at the source's resolution, capped (e.g. 2400 px long side),
- a small `pages.ini`: count, sizes, source path, source size and mtime (a changed file is converted again),
- a fit-to-screen copy per page (640 px wide), so turning a page never waits on a full decode.

Sources:

- **Images and folders**: used as they are; only the fit copies are made.
- **CBZ**: read with a small zip reader (stored and deflate entries, zlib). Images only; other entries skipped.
- **PDF**: v1 does not render PDF. It walks the file for page objects whose content is a single full-page image
  (`/DCTDecode` JPEG, or `/FlateDecode` raw pixels) and writes those images out. That covers almost every scanned
  manual (archive.org, Replacement Docs, ScreenScraper), and needs no PDF library. A page that is not one image is
  counted; if any are, the reader says "This PDF has pages that are not scans" and suggests converting it on a
  computer. Encrypted or broken files say so.

Conversion runs on the existing background worker the first time a document is opened (with a progress bar), or during
the library scan when the owner turns on "Prepare manuals" (default off: it is card wear and time). A 40-page manual
should take seconds; to be measured (section 9).

**Rejected for v1**: MuPDF (AGPL; fine with GPL-3.0 legally but it pulls the whole tree under AGPL terms, and it is
large), poppler (heavy, many dependencies in the image), pdfium (BSD but huge and a Chromium-style build). If text
PDFs turn out common, pdfium or MuPDF can come later as an optional converter; the page-set format does not change.

## 5. Showing it

### Play systems: the Manual and Maps views

`companion/manual.cpp` implements `View`:

- `shown()` is true when the game has a page set; the views go after a profile's own views and before the keyboard,
  so a game without a profile shows the manual on its own only if the owner picks it (the default view stays as today).
- `draw()` blits the visible part of the current page into the second screen's canvas: fit copy at fit zoom, tiles of
  the full page above it (256x256, decoded on a worker, an LRU of about 24 MB). While a tile is not ready the fit copy is
  drawn scaled, so zooming never shows blank.
- `key()` changes with page, zoom, pan and tiles arriving, so the companion redraws only then.
- `touch()` gets the gesture events of section 6.
- `save()`/`load()` keep page, zoom, pan and bookmarks; they live next to the game's other companion state.
- A thin page bar at the bottom (page n of m, bookmark star) fades after two seconds, using the launcher look via the
  shared headers like the other companion UI.

Maps are the same view with a different source and two extras: the map picker, and the start zoom at "fill" not
"fit" (a map is meant to be panned).

### DS: reading mode

- New DSCore layout `reading`: both DS screens stacked on the upper panel (256x384 at 1.25 = 320x480, centred, or
  side-by-side at 2.5x for the owner who prefers it), the lower panel given to the reader.
- Touches on the lower panel go to the reader, not the game, while the mode is on. The game keeps running by default;
  "Pause while reading" is a setting (section 10).
- Entered from the game menu (Manual row, shown only when the game has one) and a hotkey (`manual_toggle`,
  unbound by default), left the same way or with B on the reader's bar.
- The reader itself is shared code (section 7), drawn by the bridge or a helper on the lower panel. Which of the two
  owns the panel is the main open design point for DS: the bridge already owns the lower panel's touch reader
  (`touchId -2`), so drawing there too is likely the smaller change.

### Game info

A Manual button (and Maps) on the Game info screen opens the reader full screen on the lower panel with the page on
the upper one at fit size, outside any game. Same reader code, same remembered page.

## 6. Touch and gestures

Two changes, and the existing one-finger path must not change at all for games:

1. **`TouchDecoder` gets a second output**, `contacts()`: every active contact with a stable id, in panel pixels. The
   current primary-only output stays exactly as it is; a unit test replays captured bytes through both and checks the
   old one is identical.
2. **`Touch` gets an id** (default 0), and a reader-only `MultiTouch` event carries up to two points.

A small `gestures.hpp` beside `kinetics.hpp` turns contacts into: tap, double tap, drag (with the existing flick and
glide), pinch (scale and centre), and two-finger drag. Pinch zooms around the midpoint between the fingers, clamped
from fit-page to 4x the page's pixels.

Fallbacks, so the reader works even if section 9 finds only one finger: double tap cycles fit-width, fit-page, 2x; a
zoom slider on the page bar; one-finger drag pans. No buttons: while a game runs they stay the game's.

## 7. Code shape

- `play/src/reader/` (new, plain C++ with no SDL in its core):
  - `pages.hpp/.cpp`: page-set read/write, `pages.ini`.
  - `convert.cpp`: images, CBZ (`zip.cpp`), PDF image extraction (`pdf_images.cpp`).
  - `tiles.hpp/.cpp`: tile cache and worker.
  - `reader.hpp/.cpp`: state (page, zoom, pan, bookmarks), gestures in, draw into a `Canvas` out.
- `play/src/companion/manual.cpp`: the `View` wrapper.
- JPEG: `stb_image.h` (public domain, decode only, vendored) for decoding; JPEG writing for fit copies with
  `stb_image_write.h` or plain PNG for them. libjpeg-turbo is faster on ARM and already a Buildroot package; worth
  switching to if decode time shows up in section 9.
- Launcher: library scan flag, Game info buttons, upload target in the port files QR/web upload, settings rows.
- DSCore: the `reading` layout, the hotkey, routing lower-panel touches.
- `runtime/screenscraper.py`: the `manuel` media kind (phase 4).
- Strings in all locales (`assets/locales/*.tsv`): Manual, Maps, page n of m, the PDF message, settings rows.

## 8. Phases

| Phase | What | Done when |
| --- | --- | --- |
| 0 | Hardware check (section 9): second finger, decode speed of a 2400 px JPEG, tile budget | Numbers written here |
| 1 | Reader core + images/CBZ + Manual view on Play systems, one-finger gestures | A GBA game shows a CBZ manual on the second screen, page and zoom remembered, Mac and handheld |
| 2 | Multi-contact decoder output + pinch | Pinch on the handheld; old touch path byte-identical in the replay test |
| 3 | PDF image extraction + Game info reader + Maps view + upload | A scanned archive.org PDF opens; a map pans and pinches |
| 4 | DS reading mode | A DS game reads its manual while running; touch returns to the game on leaving |
| 5 | ScreenScraper manuals, bookmarks UI, "Prepare manuals" | A signed-in fetch downloads a manual for a game that has one |

Phase 1 alone is already a useful release. Phases 2 and 3 can swap.

## 9. Open questions to measure first

1. ~~**Does the lower panel report two contacts?**~~ **Yes, both panels do** (measured 2026-11-20). Both are
   `Goodix Capacitive TouchScreen` at 640x480 (`event1` on i2c-3, `event2` on i2c-5), type-B multitouch with 5 slots
   (`ABS_MT_SLOT 0..4`) and tracking ids. A 60 s read-only recording while the owner pinched on each screen (lower
   first, so `event2` is most likely the lower panel): 2 fingers at once on both, 638 frames with two contacts on
   `event2` and 173 on `event1`, finger distance moving 74-458 px and 120-494 px, so pinch distances come through.
   Nothing grabs the devices (`EVIOCGRAB` free), the engines and `emulators.py` read them shared. The upper screen being
   touch too means the reader could also live there for DS later. Section 6's fallbacks stay for comfort, not need.
2. **Decode time**, measured 2026-11-20 on the handheld (`pages_probe`, nice 10, the Advance Wars PDF from
   ScreenScraper beside its ROM): opening the 79-page file 28 ms, every page decoded, slowest 34 ms (1270x684 JPEG).
   A larger scan still to try. Originally: of a 2400x3300 JPEG with stb_image on the handheld's CPU, and of a fit copy.
   Target: page turn under 100 ms from the fit copy, tiles under 300 ms.
3. **Memory**: tile cache size that does not hurt the heaviest games running beside it (N64, PSP, Dreamcast).
4. **Second screen redraw cost** while a game runs: the companion already redraws on `key()` change; a pinch must
   not cost game frames. Measure frame times with the reader panning in a GBA game and in Mario Kart DS (reading mode).
5. ~~**ScreenScraper coverage** of `manuel` for the owner's library.~~ Measured 2026-11-20, section 9a.

### 9a. ScreenScraper coverage, measured 2026-11-20

One `jeuInfos.php` per game in the owner's `/data/roms`, from the Mac with the owner's account and the app's developer
pair, matched by file name and size only (no CRC, so slightly worse matching than the app's own fetch). Dreamcast
discs were not queried.

| System | Games | Matched | With a manual |
| --- | ---: | ---: | ---: |
| DS | 56 | 55 | 34 |
| Game & Watch | 59 | 59 | 27 |
| GBA | 25 | 23 | 23 |
| GB / GBC | 14 | 14 | 14 |
| 3DS | 8 | 6 | 4 |
| NES | 7 | 5 | 5 |
| SNES, PCE, MD, PSP, ScummVM, N64, Amiga, C64, ZX, Atari, Coleco, Pico | 21 | 18 | 18 |
| **All** | **190** | **180** | **125 (66%)** |

- Every matched game outside DS, 3DS and Game & Watch had a manual. The DS gaps are mostly licensed kids' and
  Nordic-only releases; the unmatched files are homebrew, test ROMs, demos and renamed files (`mk7.cci`).
- Media type `manuel`; all 268 manual entries are **PDF**. Several per game is common (one per region: us 94, jp 40,
  eu 36, wor 27, de 23, fr 21, …), so the fetch picks a region the same way covers do, the owner's language first.
- Sizes: median 3.1 MB, largest 16.5 MB. `scraper.Client.request` caps a response at 8 MiB, so manuals need their own
  limit (and should stream to a file rather than into memory).
- No paid tier was needed for the listing or the downloads.

### 9b. What the PDFs are, three samples, 2026-11-20

Downloaded with `mediaJeu.php`, `media=manuel(<region>)` (the same call as covers, no `maxwidth`/`outputformat`):
Polarium (DS, us, 2.5 MB, 14 pages), Advance Wars (GBA, eu, 7.4 MB, 79 pages), Super Mario World (SNES, eu, 5.0 MB,
16 pages). Inspected with poppler's `pdfimages`/`pdfinfo`/`pdftotext`.

- **All three are pure scans:** every page draws one JPEG (`DCTDecode`) scaled to the page with a single `cm`, no text
  layer at all (0 characters), not encrypted. Section 4's extraction route holds: the JPEG stream bytes *are* the page
  file, copied out with no decode or re-encode.
- **One exception per file can happen:** Polarium's cover has a second small image (a 141x35 logo) with a soft mask
  over the scan. So the extractor draws every image on a page at its `cm` (JPEG plus an optional Flate `SMask`), not
  "the one image". Still nothing like a PDF renderer; pages with anything else (text, vector paths) stay the "not a
  scan" case.
- **Scans are modest:** 1229-1379 px wide at 120-200 dpi, about 90-160 KB a page. The 2400 px cap of section 4 never
  bites, a whole page decodes small enough that the fit copies may be unnecessary (measure in phase 0), and "4x the
  page's pixels" is far more zoom than the scans hold: cap zoom at about 2x the scan instead.
- **Pages are often two-page spreads:** DS pages are 2:1 landscape spreads, Advance Wars is 78 spreads after a single
  cover, Super Mario World is 4:3 pages. The reader needs a **Split spreads** mode (a page wider than about 1.6:1 shows
  as its left half then its right half, each fit to width with vertical pan), on by default for spreads and remembered
  per document.
- Producers are Ghostscript, Acrobat's image conversion and NAPS2: ScreenScraper keeps contributors' files as they
  are, so other games may differ. Keep the "not a scan" message for those.

## 10. Decisions for the owner

- **DS: game keeps running while reading, or pauses?** Proposal: keeps running, pause as a setting.
- **DS layout in reading mode:** stacked small, or side-by-side? Proposal: stacked.
- **Manual as a default second screen** for games with no profile and no other view? Proposal: no; the owner picks it
  once and the choice is remembered per game.
- **"Prepare manuals" in the scan**, or convert on first open only? Proposal: on first open; the setting for those who
  want instant pages.
- **Text PDFs**: message only in v1, or ship a renderer? Proposal: message, decide after phase 3 with real files.

## 11. Testing

- Unit: zip reader (stored, deflate, data descriptors, zip64 refusal), PDF image extraction on a small corpus (scans
  from archive.org, a text PDF, an encrypted one, a broken one), page-set invalidation by size/mtime, gesture maths,
  decoder replay with one and two fingers (old output unchanged).
- Mac: the Play frontend's lower window with mouse drag; two-finger pinch from a trackpad via SDL finger events.
- Handheld, on a copy of userdata as always: one GBA, one SNES, one DS game, a 100-page manual, a large map; frame
  times with and without the reader.

## 12. Built, 2026-11-20

- `play/src/reader/pages.{hpp,cpp}`: one page list from a PDF of scans, a CBZ/zip or a folder of pictures. The PDF
  side is our own small object reader (objects in file order with stream data skipped, object streams, indirect
  lengths, the page tree with inherited resources and `/Rotate`, the largest image a page draws, one level into form
  XObjects). JPEG pages are read straight from their place in the PDF; 8-bit Flate pages (grey, RGB, CMYK, ICC) are
  inflated. Anything else on a page is "not a scan". Encrypted files say so. Pages wider than 2048 px are halved
  until they fit.
- `play/src/third_party/stb_image.{h,cpp}`: stb_image v2.30 (public domain/MIT, THIRD_PARTY.md) for JPEG, PNG, BMP,
  GIF.
- `play/src/reader/manual.{hpp,cpp}`: `find_manuals` (`<rom stem>.pdf/.cbz/.manual` beside the ROM, a zipped ROM by
  its zip's name, then `manual.pdf/.cbz/manual/` in the library folder; one spelling per kind, since exFAT and macOS
  ignore case) and the reader view: a worker thread opens the file and keeps the shown page and its neighbours
  decoded (with halvings for filtering), so the game thread never decodes. Spreads wider than 1.6:1 show a half at a
  time. One finger: tap the outer fifth or swipe to turn, drag to pan when zoomed, double tap for fit page, fit width
  and the scan's own pixels. A page chip (`n / count`) shows for two seconds after a turn or tap. The page is kept per
  manual in `<library folder>/manual-place.txt`.
- `companion.{hpp,cpp}`: the manual is one more screen behind the top-right switch, never shown by itself. The switch
  steps game screen (or title card) -> alternative (board or keyboard) -> manual, shows a book icon when the manual is
  next, and remembers `2` in `second-screen.txt` (older builds read that as the game screen). While the manual shows,
  a HUD split stays in the game picture (`hud_allowed()`). Not on `[screens]` games (Game & Watch), whose lower screen
  is the game.
- `session.cpp` attaches the first manual found. Strings: "This manual could not be opened", "This page is not a
  scan", "This page could not be read" in all 23 catalogs (my translations, not reviewed by native speakers);
  "Loading…" already existed.
- Tests: `play/tests/reader_tests.cpp` (`play-reader-tests`, 43 checks: folder, CBZ stored/deflated, a hand-built PDF
  with an object stream, an indirect length and a quarter turn, the size cap, finding, page turns and spreads, the
  kept page, the switch cycle, a lift off the switch not turning a page), run by `tests/test_play.py ManualReader`.
  The existing `play-tests` still has one failure at `play_tests.cpp:1197` (mosaic save size), the same at the last
  commit.
- Real manuals on the Mac: the three ScreenScraper samples open (14, 79, 16 pages), every page decodes in 5-15 ms
  there. `play-shot --drive` with the Advance Wars manual beside the test cartridge: switch, cover, spread halves,
  fit-width zoom, swipe, back.
- Not done: the handheld (decode time, memory beside heavy cores, redraw cost while panning), pinch, maps, a picker
  for several manuals, CCITT/JBIG2/JPX pages (black-and-white document scans), Game info, upload, DS reading mode,
  ScreenScraper download, bookmarks.
- Handheld, 2026-11-20: built for the device (`tools/build_play.py device --only rgds-play`, same libraries as the
  image's binary). A bind mount over the release's `emulators/rgds-play/rgds-play` is not enough on RG DS OS:
  `emulators.py` runs `activate.verify_engine` before each Play launch and hashes that file against the release
  `manifest.json`, so every Play game would refuse to start. Replacing the manifest was refused by my permission
  check, so the mount was taken off again (original back, check passes). Trying it needs the owner's go for that, or an
  OS image push.
- Pinch, 2026-11-20 (after the owner tried the reader on the handheld: works, no pinch): `TouchDecoder` keeps
  `fingers` (every contact after each frame) beside its unchanged one-finger stream, `TouchDevice::fingers()` exposes
  it. RG DS Play opens its own read-only reader on `RGDS_TOUCH_DEV` only for a game with a manual (no grab; the
  bridge keeps feeding the one-finger stream) and passes the set to `Companion::fingers` -> the reader's `Pinch`
  interface. Pinch scales continuously from fit page to 2x (at least twice fit), the page point between the fingers
  stays between them so two fingers also pan, the first finger's own moves and lift are ignored for that gesture, and
  letting go within 4% of fit snaps back to the whole page; double tap after a pinch returns to fit. 16 more checks in
  `play-reader-tests` (59). Installed on the handheld the same way (bind mounts of the binary and a manifest copy,
  owner's go); not yet tried by hand.
- Page overview, 2026-11-20 (owner asked for faster browsing): a grid of the manual's pages, five a row under the
  switch's row, the current one ringed in the accent colour, numbers under each. Opened by pinching the whole page in
  (fingers closer than 70% of where they started) or by tapping the `n / count` chip; a tap on a page opens it, two
  fingers spread go back. Thumbnails are made on the worker after the shown page and its neighbours, the rows on screen
  first, and kept within 60 pages of them. Drag scrolls, with a glide after a flick that stops at the ends. 7 more
  checks (66). Installed on the handheld by bind mount on top of the Sound toy session's manifest copy.
- Launcher badge, 2026-11-20: an open book on the cover's bottom-right corner (the dual-screen mark is bottom-left)
  for games with a manual: `src/launcher/manual_mark.hpp` mirrors `find_manuals` (keep the two in step), `game_manual`
  / `manual_badge` in `library_screen.hpp`, `stage_manual` in `panels.hpp`. No mark on DS, 3DS or Game & Watch.
  Checked on the Mac in the GBA list (an -O1 build: Apple clang crashes at -O3 on this tree for everyone). The device
  launcher is built from a scratchpad snapshot because other sessions kept changing launcher sources mid-build; not yet
  on the handheld.
