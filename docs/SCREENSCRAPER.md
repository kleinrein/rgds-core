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
