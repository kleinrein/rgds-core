# RG DS Core refinement — 2026-10-12

## Interface

The status bar and boot sequence use generated RG DS Core wordmarks with a
split-screen D detail, theme-specific lettering and tight alpha bounds. Twelve
image-generated icons cover Home and main Settings categories. The Games icon
uses a DS cartridge front with a dual-screen label. All runtime icons are bounded
128px BMPs and preloaded outside the render path.

The full-width bottom rail again contains a broad grey viewport window and a
separate narrow position marker. The window represents roughly three visible
cartridges; the marker can move within it, especially in the four-item Home menu.

Home reports game count, play sessions, total duration and last played title for
the selected collection. Per-game details include session count. A cancellable
worker aggregates existing history with a one-entry detail cache; it never scans
history in drawing callbacks. New requests replace outdated pending results.

## Artwork and languages

Artwork choices are Automatic, GameTDB and Libretro. Removed ScreenScraper UI and
CLI entry points; old selections migrate to Automatic and cached covers remain.
The dormant adapter is retained in source for possible future work.

German, French, Spanish and Brazilian Portuguese join US English and Norwegian
Bokmål. Each catalog has the same 525 messages, including in-game menus. All
characters are covered by the shipped rounded font; pixel accents were extended.
Shortened several action labels after inspecting actual 640px screen renders.

## Validation

Host and Linux AArch64 builds succeeded. Focused suite: 154 tests passed; the only
failure was an obsolete ScreenScraper login expectation. Replaced that test with
provider migration behavior, then ran it and all four refinement checks: 5 passed.
Coverage includes 10,000-game collection cancellation, bounded media/detail caches,
scroll gestures, settings, boot branding, artwork, in-game IPC and font coverage.
Inspected light/dark Home and all four new language Settings/game-menu renders.

A final missing artwork-status translation was added to all six catalogs; the four
refinement checks passed again.

Build: `0.0.1-dev-fc0132346672`. Deployment and device capture evidence lives under
`build/core-refinement/` (not versioned).

The previously reported first Power press doing nothing remains unresolved.
This visual update does not claim to repair that hardware/early-boot behavior.

## Device verification

Deployed to `192.168.68.118`, with the previous release retained. Status reports
launcher/session running, no active game, no recovery and no record errors. The
live screenshot confirms the generated wordmark, cartridge/navigation icons,
restored rail and history totals: 62 games, 36 sessions, 25 minutes, last played
New Super Mario Bros. Boot artwork was installed and its readback verified;
no physical power cycle was performed during this refinement.
