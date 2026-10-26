# Artwork sources

Settings → Library → Artwork sources now defaults to **Automatic**.
Select a source, then **Fetch missing artwork**. No developer credentials or
player account are required for GameTDB or Libretro. The job runs in the
background, can be stopped, and keeps completed downloads for offline browsing.

- **GameTDB** matches the four-character game code in the DS header, including
  inside a ZIP containing one DS game. It prefers the game's regional cover.
- **Libretro** tries the ROM filename, then a normalized title against the public
  DS thumbnail index, then resolves the exact header game code through Libretro's
  public No-Intro catalog. This also handles renamed or localized dumps. It keeps
  sequel numbers, prefers the ROM's region and refuses ambiguous title matches.
  Unknown/homebrew titles may still have no published cover.
- **Automatic** tries GameTDB and falls back to Libretro.


Only game codes or titles are requested from the public providers; ROM contents
are never uploaded. Requests are serial with at least half a second between
public requests, verified HTTPS, host-restricted redirects and bounded downloads.
Transient public-service failures are retried once; an unavailable host cools
down for a minute so an offline library does not make the same slow request for
every game. A failed game does not abort the remaining collection. Existing art
is skipped. Public covers preserve existing titles/year/developer
metadata and record attribution in the item's `info.ini`. Cover attribution is
shown on the top display. Credentials are never sent to public providers.

Sources: [GameTDB DS](https://www.gametdb.com/DS/Downloads) and
[Libretro thumbnails](https://docs.libretro.com/guides/roms-playlists-thumbnails/).
On October 11 both providers downloaded and decoded three real device covers
without credentials. A later **local-only** audit downloaded and decoded covers
for all 62 previously observed game codes on the Mac; four deliberately renamed
Norwegian-title fixtures also resolved through Libretro's exact serial catalog.
This is provider coverage evidence, not a later device deployment or UI acceptance.
ROM banner icons remain available without networking.

## Unmatched games and corrections

The job shows how many covers are available, how many have no match and how many
need retrying. **Review unmatched** opens games in the current collection that lack
artwork or had an error. Select a game to inspect its last result. You can also use
**Game options → Artwork** directly. **Matching title** supplies an exact or normalized
Libretro title; **Fetch artwork** retries just that game. **Use ROM identity** clears
the correction and returns to automatic game-code/filename matching. These actions
do not rename ROMs, change library keys or move saves. A failed replacement keeps
the existing cover.

Public title/cover catalogs are cached for seven days and can fall back to the
previous cache during an outage. Covers retain aspect ratio and are capped at
384 pixels on the longest edge before BMP storage. This reduces SD reads and GPU
texture memory while covering the launcher's displayed sizes. Completed results
survive cancellation in each game's `artwork-status.ini`; `scraper/report.json`
contains the last job's audit. Fetch missing artwork resumes using existing covers.

## Game information and extra pictures

Besides the cover, **Fetch missing artwork** can also keep text and pictures
about each game. Both are optional, never replace a cover or a title, and a
library without them looks exactly as before. They work for DS, GBA, GBC and GB;
`runtime/game_info.py` does the work and `--extras info,snap,title` selects the
parts (Settings → Library → Artwork sources chooses them in the launcher).

- **Game info** adds developer, publisher, genre, release date, rating, player
  count, online play, series and a description. DS games come from the
  [GameTDB](https://www.gametdb.com/DS/Downloads) database (one 4.5 MB download,
  cached for seven days, matched by the header game code, with descriptions in
  every language it has). GBA, GBC and GB games come from Libretro's metadata
  lists, matched by the ROM's CRC32 (read from the ZIP directory for zipped
  games, so nothing is unpacked).
- **Screenshot** and **Title screen** come from Libretro's `Named_Snaps` and
  `Named_Titles` and are stored next to the cover as `snap.bmp` and `title.bmp`
  (at most 384 px). The name is resolved like covers: your matching title, the
  exact No-Intro name of the ROM's CRC, the filename, then the header serial.
  Ambiguous titles are skipped rather than guessed.

In the game's library folder: `info.ini` gains `info_title`, `publisher`,
`genre`, `release`, `rating`, `players`, `online`, `features`, `franchise`,
`languages`, `region`, `info_provider` and `synopsis_languages`; `developer`
and `year` are only filled when empty, and `title` is never touched. Each
description is `synopsis.<LANG>.txt`. `info-status.ini` records what was tried,
so a game with no entry is not asked about again until **Fetch artwork** on that
game forces it. Network failures are retried on the next run.

In the launcher, Settings → Library → Artwork sources has **Game info** (on by
default) and **Extra pictures** (off by default: Screenshots, Title screens or
both, because each is one more request per game).

**Game options → Game info** (just above Save gallery) opens the page. The top
screen shows the picture you have (cover, title screen, screenshot) with a strip
The title screen is fetched by default and is also the icon of a GBA, GBC or GB game (then the screenshot, the cover, a drawn cartridge).
of thumbnails, the title, developer, publisher and release date, and chips for
genre, rating, players, online play and series. The bottom screen shows the
description in your interface language (English if it has none), or the details
as a grid for games without one. L/R, Left/Right, A or the pill at the top step
through the pictures; Up/Down or a tap on the upper/lower half scrolls. A game
without information says how to get it, and nothing else in the library changes.

### Filtering by game information

**Y** in the library opens the Library view, a scrollable sheet over the lower
screen. Its top holds tiles for the collection, the browser layout, Jump to
letter (L2), Search games (R2) and Clear filters; below them chips cover played state, genre, rating, online play / has game info,
publisher and developer; each shows how many games have it and toggles at once.
**Players** and **Released** are range sliders over a histogram of your library
(one bar per player count or year), each with two handles: Left/Right moves the
chosen handle, A switches between the low and high end, and a finger can drag
either handle. A range at its full extent is not a filter. The list behind the
sheet follows every change and the header counts the matching games. Long lists
(genre, publisher, developer) show the commonest values and a **More…** chip that
opens the full list. X resets the filters; L/R jump between sections; **Show games** closes
the sheet on the results. Filters combine (all must match), search also finds a
game by its developer or publisher, and **Clear filters** resets
everything, search included. A game without a known player count or year never matches a narrowed
range, and games without information only match the played-state and "Without
info" chips.

## Play history and storage

Home shows game count, total sessions (Times played), time played and the last
played title for Games, Favourites and Recently played. Game details show that
game's session count. These use existing local records; historical statistics
from other frontends are not imported. Playtime includes paused/menu time and
excludes kernel suspend time through the monotonic clock.

Artwork, metadata and play history remain in `userdata/library/<game-key>/`.
The key follows the canonical ROM path, so moving a ROM creates a different key.
Release updates retain the existing library data.

## ScreenScraper availability

ScreenScraper has been removed from the source picker and runtime CLI. Previous
selections migrate to Automatic. The old adapter remains in the source tree for
possible future use, but login/logout and provider selection are unavailable.
Existing downloaded artwork is preserved. No account setup is needed.
