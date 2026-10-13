# Stock Linux recovery and polish — 2026-10-11

The working Anbernic card remains the hardware foundation. No kernel, bootloader,
partition table, firmware, Wi-Fi startup script or compositor was replaced.

## Correct installation

SSH confirmed that direct startup ran `0.0.1-dev-f385dc83edc8` from
`/userdata/rgds-device-test/rgds`, while APPS selected `0.0.1-dev-a1eab2fe6582`
from `/mnt/mmc/rgds`. The boot installation still contained the old DraStic
finger-input crash. Updating the card installation alone could not fix startup.

The deployment tools now follow the enabled startup root by default. An explicit
`--sd-root`/`SD_ROOT` still chooses another mount. The watcher retains its active-game
checks, staged verification and previous-release rollback. Try RGDS and Probe
also follow the enabled startup installation; card-local fallback remains available.
Userdata and the prior startup configuration were backed up before deployment in
`build/stock-polish-20261011/device-before.tar.gz`.

## Interface and artwork

- Status text uses visible glyph bounds to align with the icons in Rounded and Pixel modes.
- Library layout offers Cartridges, Covers and List, persisted independently of
  the All/Favourites/Recents filter. The lower toolbar and Library settings expose it.
- A–Z has a touch grid and shoulder-button shortcut. Only letters present in the
  current collection are offered. Selecting a letter changes selection without launching.
- GameTDB, Libretro and Automatic (free) work without accounts or application
  credentials. ScreenScraper remains optional. Existing downloaded art is retained.
- Soft scanlines, Warm paper and Cool crisp extend the catalog to 18 choices,
  including Off. They use one texture sample and the existing shared GLES pass.
  All 54 reference previews were rendered through GLES and verified against their
  actual source files; the packed previews occupy about 3.4 MiB.

## Hardware evidence

`build/stock-polish-20261011/` holds logs, backups, screenshots and measurements.
GameTDB and Libretro each downloaded and decoded covers for three existing ROMs
on the device, with no credentials, in 3.73 and 13.97 seconds respectively.

DraStic, DSperate and NNDDSS launched New Super Mario Bros with the new Cool crisp
shader, isolated save folders, native resolution and automatic saving disabled.
Each survived three injected Goodix gestures, three acknowledged menu/resume
cycles, and normal quit with exit zero. DSperate's short sample averaged 59.76
presentation FPS. NNDDSS's game-frame counter reported 59.05 FPS. DraStic's last
sample was 60 presentation FPS; its short average including startup was 58.33.
These tests establish short-session behavior, not sustained game compatibility
or physical stylus acceptance. Existing player checkpoints were not loaded or
replaced.

## Boot branding and remaining limits

The stock boot partition was copied read-only to `stock-boot.img` on the Mac.
The parser found a signed FIT image and rejected an early-logo patch. No boot
partition bytes were written. Seven known userspace splash files were replaced
with RGDS artwork, with durable original backups and readback under
`/userdata/rgds-boot/userspace-branding`. `runtime/boot_userspace.py restore`
restores those originals. The earliest Anbernic logo remains in the signed image.

The old startup build recorded 19.854 seconds from Linux startup to RGDS's first
frame, including 2.591 seconds from the owned startup hook. This excludes the
bootloader and the power-button interval. A new cold-boot measurement, physical
touch check, sleep/wake and sustained retail gameplay remain hardware acceptance
checks. No 100x speed-up or complete physical acceptance is claimed.


## Final application candidate

`0.0.1-dev-31ae97d6bb7d` was installed over SSH; all 174 manifest file hashes were
verified. Archive SHA-256:
`92be1564c2b2949dca8bbe1bb22ce95fef76d1dea29c8ea83813723951c4d257`.
The emulator bridge and runtime files are byte-identical to the device-tested
candidate; later changes concerned UI previews, controller shortcuts and capture
acknowledgment. The final touchscreen checks traversed Settings → Theme & display
→ Home three times with repeatable page pixels. Additional device captures verified
Artwork, Covers, List and the letter grid. Three tested GameTDB covers were added
to the user's actual cache without replacing existing art.

An initial capture check was invalid because the launcher acknowledged requests
before writing screenshots. Acknowledgment now follows both saved panel images;
the final test uses that behavior. Physical fingers/stylus still need user testing.

Validation logs include 71 passing focused tests, 11 passing preview tests, the
development/rollback checks, and reversible userspace-branding failure tests. A
stale test expecting the former last shader was updated to the new final ID and
passed; the production preview cache remains bounded. Counts from overlapping
runs must not be added as if they were unique tests.


## Normal restart before the local-only work

The final installed `31ae97d6bb7d` build survived a normal stock reboot; SSH
reconnected and the active launcher path was checked. Its first frame was
recorded at 17.823 seconds of Linux uptime, 2.532 seconds after the hook and
1.832 seconds after session start. This one restart excludes bootloader time and
is not a cold-power measurement. See `reboot-verification.log` in the evidence folder.

The owner then requested no more device deployments tonight. Subsequent browser,
artwork and branding-inspection changes are local only; see
[the local follow-up](LIBRARY_POLISH_2026-10-11.md). The owner also reports still
seeing the vendor boot logo; userspace file replacement is not visual acceptance.
