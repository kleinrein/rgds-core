# Library follow-up — local only, 2026-10-11

The owner requested no more RG DS deployments for the night. This work stays on
the Mac. The running device remains the previously installed
`0.0.1-dev-31ae97d6bb7d`; no SSH session, device restart, card write or deployment
was performed during this follow-up.

## Browser fixes

Covers now uses a 184-pixel focused cover with smaller neighboring covers drawn
behind it. Visible neighbors are loaded before drawing, fixing the old behavior
where only previously selected games had artwork. Cover hit testing follows the
same overlap geometry as rendering. Dragging, snapping, selection and launch are
separate; dragging never launches a game.

List has a continuous viewport, row-by-row controller scrolling, clipped partial
rows during a swipe and a vertical scrollbar with a usable touch target. Drawing,
row hit testing and scrollbar position share one geometry helper. Changing views,
restoring selection and jumping to a letter reveal the selected row. A visible
count and horizontal position scrollbar are shared by Cartridges, Covers and List.
The layout/A–Z toolbar and All/Favourites/Recents controls stay in the same places.

Visible titles are prepared before drawing. Recognized release/region tags are
removed from filename fallback titles, avoiding publisher text from multi-line DS
banners. Rich metadata titles still take precedence. The existing texture cache
remains bounded at 64 entries / 16 MiB. Local preview captures show zero file reads
or file-stat calls within the draw pass; this is not a device frame-rate measurement.

## Artwork fixes and coverage

- GameTDB still matches exact header codes and now considers Nordic languages.
- Libretro can recover renamed files through the public No-Intro serial/name
  catalog, retaining sequel numbers and refusing ambiguous different titles.
- Catalogs are cached; transient requests retry once and unavailable hosts cool
  down. One failed game no longer abandons the rest of the collection.
- Per-game results distinguish cached/downloaded, unmatched and failed items.
  Completed results survive cancellation; fetching missing artwork resumes the work.
- Artwork → Review unmatched opens a game's result. Game options → Artwork adds
  an editable matching title and a single-game fetch. A correction changes neither
  the ROM path nor saves. Failed replacements keep existing artwork.
- BMPs retain aspect ratio and are capped at 384 pixels to bound cache storage and
  texture uploads. The SDL resize/save path is tested with real SDL operations.

The previously observed **62 distinct game codes** all resolved in Libretro's
catalog. The local Automatic-provider audit downloaded covers for all **62/62**
through GameTDB and decoded them with Pillow in 105.2 seconds on the Mac. Four
intentionally renamed Norwegian-title fixtures separately downloaded valid
Libretro images using their exact game codes. These fixtures contain only a
synthetic header with the observed code; no ROM content was copied/uploaded.

The Mac lacks SDL2_image. The full network audit therefore used the host Pillow
decoder; the runtime's actual SDL resize/BMP path was tested separately with SDL's
BMP decoder. Previous three-game device download/decoder results remain in the
stock report. This is strong provider coverage evidence, not proof that a full
scrape has run on the device or that every possible DS game has published art.

## Boot logo

The owner still sees the Anbernic logo. The saved stock boot image again failed
the strict parser's signed-FIT check. The early logo is **not fixed**. No signature
bypass, bootloader edit or partition write was attempted. The userspace installer
now explicitly reports its limited scope and offers a read-only `inspect` action
that distinguishes installed/original/changed/missing files. Installing those files
does not establish which ones the firmware actually displays.

## Checks and next device session

Evidence lives in `build/library-polish-20261011/`:

- The broader UI regression run completed 92 tests with one obsolete expectation
  (shader 14 was the former last preset). The application correctly selected 17;
  the expectation was corrected and retested.
- The focused follow-up completed **56 passing tests** covering artwork, cache
  reuse, failures, cancellation, serial matching, redirects, image sizing,
  viewport geometry, browser touch controls and the corrected
  shader endpoint. Do not sum overlapping runs as unique checks.
- Both host and AArch64 builds succeeded. The launcher self-test passed.
- Six local previews cover all three layouts in rounded/light and pixel/dark.
- Package identity, engine equivalence and the final selected package are recorded
  below after packaging. No claim of new hardware acceptance is made.

Tomorrow, first check device free space and the actual active startup release.
The internal filesystem was tight; reserve enough space for staging/rollback and
artwork before installing. Preserve active/previous releases and saves, archiving
any older unselected release with verified copies before reclaiming space. Then
use one ordinary application deployment with rollback, test both screens and
physical touch, scroll all three layouts to both ends, fetch the remaining artwork,
try a title correction, and launch/return from each of the three engines. Keep
boot-logo investigation separate until the visible stage is identified.


## Packaged local candidate

Build: `0.0.1-dev-220c7bad87ae` (not deployed).
Archive: `dist/RGDS-0.0.1-dev-220c7bad87ae.zip`.
SHA-256: `3684e18d3654b1716112503a46c9f6967a68748342903b0bf42937145792ccb4`.

All 174 manifest entries verify. The 56 emulator payload/bridge and
session integration files compared against installed `31ae97d6bb7d` are byte
identical. The package retains DraStic, DSperate, NNDDSS and all 18 shader choices.
Downloaded covers, public catalogs and local fixtures are absent from the release.

Final follow-ups passed 34 smoke/development/idle tests, 17 provider tests and
three targeted cover/list/render-cache checks. These overlap previous runs.
The final file list and archive receipt are in `package-validation.json`.
