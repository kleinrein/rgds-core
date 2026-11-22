# RG DS Flasher

Puts RG DS OS on a microSD card in five steps: **Download → Pick card → Flash → Verify → Done**. No Rufus or balenaEtcher needed, and nothing to install.

1. It reads the latest release on GitHub (`kleinrein/rgds-core`) and downloads `RG-DS-OS-<version>.img.gz` (or the `RG-DS-OS-Plus-` one; RG DS is the default). It checks the file against its `.sha256` and keeps it, so the next card needs no download. A stopped download picks up where it left off. Offline, it offers the saved copy.
2. It lists removable cards only. Internal disks, the system disk, disk images and USB hard drives are never listed. Anything over 256 GB is shown but can't be picked, and neither can a card smaller than the image.
3. Before writing it asks "Erase this card?" and names the card. It warns when the card has a `roms` folder.
4. It unpacks the image on the fly and writes every byte raw to the whole disk, zero blocks included. The first 4 MiB (partition tables, U-Boot at 32 KiB) is zeroed first and written last, so while the flasher works the card looks blank and no OS mounts anything on it. Each other chunk is read back and compared while writing goes on, two chunks behind so the reader's cache isn't what answers.
5. A final pass reads everything back again. Any chunk that no longer matches is rewritten from the image and checked again. Then the partition table goes on and is checked, and the card is ejected.

Everything it does is logged to `flasher.log` next to the saved image (`~/Library/Caches/RG DS Flasher/` on macOS, `%LOCALAPPDATA%\RG DS Flasher\` on Windows, `~/.cache/RG DS Flasher/` on Linux): steps, error numbers, and the disk before and after. After a failure, the UI links to it (**Show log**).

## Coming from BananaOS

BananaOS's card has the same shape as ours (boot, two root slots, exFAT data), so flashing erases it. Our root slots land on the start of its data partition. When the picked card has a mounted BananaOS data partition (`banana/` beside `saves/` or `roms/`), the card list says what it holds. The Erase dialog then offers **Bring my saves, play time and favourites along** (on by default) and **Also copy my games to this computer** (off). See `carry.go`.

1. Before anything is written, while the card is still mounted and readable without admin rights, the flasher copies `saves/`, `states/`, `drastic/backup/`, `bios/`, `banana/playstats.tsv`, `favorites.txt` and `systems.conf` to `~/RG DS backup/BananaOS <date>/`. It adds `manifest.json`, which lists the games by name and size. With the second box ticked, it also copies `roms/`, under RG DS OS's folder names (`megadrive` becomes `md`; PortMaster's `ports` stays behind).
2. The same files, without the games, go into a gzip'd tar (`carry.tar.gz` in the cache folder). If states push it past 512 MB, it is packed again without them; they stay in the backup.
3. The tar goes onto the card past the image, ending on a MiB boundary just before the card's last MiB. A one-line header, `RGDS-CARRY 1 <offset> <length> <sha256>`, goes at the start of that last MiB. The bundle is written before the image and read back again after it, so a fake card that wraps around is caught either way.
4. On the first boot, `firstboot` makes the data partition over that space. It writes only its own metadata at the partition's start; this was checked with the real `sfdisk` and `mkfs.exfat` on a loop device. Then rcS's `handoff` copies the bundle in once and checks its sha256. Setup's **Bring over** step (`runtime/migrate.py`) puts the saves and play time in place with one A press. The games go back by copying the backup's `roms` folder onto the card.

## Coming from the stock system (RG DS and RG DS Plus)

A card whose mounted partition has Anbernic's `Roms/` folder, with `saves_RA/`, `states_RA/`, `save/` or `Roms/NDS` beside it, is the stock system. It gets the same dialog. RetroArch's saves and states and the standalone emulators' `save/` go into the backup and the bundle; with games ticked, `Roms/` goes into the backup under RG DS OS's folder names (`SFC` becomes `snes`, `PS` becomes `psx`, `PICO` becomes `pico8`).

DraStic keeps the DS saves on the system card's ext4 vendor partition (`/mnt/vendor/deep/drastic*/backup/*.dsv`), which macOS and Windows can't read. The flash reads them itself (`ext4.go`, a small read-only ext4 reader) once it has the raw card open and before anything is written. They go into the bundle and the backup folder, and the done screen counts them. Any card is looked at this way, even one nobody chose to keep anything from: on a two-card Plus the slot 1 card holds the system and these saves, while its games, which the card list recognises, are on slot 2. Unticking **Bring my saves along** stops that too. `testdata/ext4-*.img.gz` are real `mkfs.ext4` images (one modern, one without extents) for its tests.

A ROCKNIX card only gets a warning. Its data is on ext4, which macOS and Windows can't read. RG DS OS can bring it over from the old card in a Plus's second slot or a USB card reader instead.

## When things go wrong

- **The card reader drops the card.** Some USB readers reset now and then, mostly while reading ("device not configured"). The flasher waits for the card to come back (the one disk with the same name and size), opens it again and carries on. One authorization covers every reopen (`security authorize` holds the `sys.openfile.readwrite./dev/rdiskN` rights, and `authopen -extauth` uses them), so this needs no new password. Reads go in 256 KiB pieces on macOS: buggy readers often reset on large reads (Linux caps USB storage requests at 120 KiB for the same reason). After a reset, macOS reads the partition table again and mounts what it finds, writing to it (`.fseventsd`, the FAT dirty flag). The card has no partition table until the end, so there is nothing to mount. If the reader drops the card after the table is on, RGDS-BOOT is checked again and repaired. macOS also gets no cache-flush command: writes to `/dev/rdisk` aren't cached, and the eject flushes.
- **A locked SD adapter** is shown as "Write-protected" and can't be picked.
- **A dropped download** retries by itself five times, resuming where it stopped. A damaged one is deleted and fetched again.
- **GitHub's API limit** (60 calls an hour per address, which shared or school networks run out of): it finds the release through the `releases/latest` page instead. Offline, it uses the saved image.
- **Sleep**: the computer stays awake while it downloads or writes (`caffeinate` on macOS, `SetThreadExecutionState` on Windows, a `systemd-inhibit` lock on Linux).
- **A fake or worn-out card** fails the read-back check with a message saying so.

| | Windows | macOS | Linux |
|---|---|---|---|
| File | `RG-DS-Flasher-Windows.exe` | `RG-DS-Flasher-macOS.zip` (universal app) | `RG-DS-Flasher-Linux-x64` |
| Admin rights | A UAC prompt when you press Flash. Only the writer runs as administrator | One password prompt when you press Flash. Apple's `authopen` opens the card (root alone isn't enough: macOS guards raw access to removable disks). The rights are held for the whole flash, so reopening the card asks nothing | `pkexec` asks for it, or start it with `sudo` |
| Before writing | Every volume on the card is locked and dismounted (`FSCTL_LOCK_VOLUME`, `FSCTL_DISMOUNT_VOLUME`) and stays locked until the card is ejected | `diskutil unmountDisk` | `umount`, then the disk is opened `O_EXCL` |
| Writes to | `\\.\PhysicalDriveN`, unbuffered and write-through | `/dev/rdiskN`, through `authopen` | `/dev/sdX`, `/dev/mmcblkN` with `O_DIRECT` |

The window is Edge or Chrome in app mode (a window without tabs) when one is installed, otherwise the default browser. The UI is served on `127.0.0.1` behind a random token, and the program quits once the window is closed and nothing is being written. The window runs in a browser profile of its own. On Close, the page closes its window and the browser gets a normal quit (SIGTERM on macOS and Linux; on Windows the browser ends with its last window). A kill is the last resort only, because the browser takes it for a crash. A browser still running on that profile from an earlier run is ended first. Otherwise it would take the new window, and old screens would pile up behind it. A page whose program has gone away says so and closes itself. If no window has shown the page after 15 seconds (a snap Chromium can't use the profile folder), the default browser opens it.

**Windows and "format this disk?":** Windows can't read the Linux partitions, so when a flashed card goes back in, Explorer may ask to format it. The flasher keeps the card's volumes locked from before the write until it is ejected, so that prompt can't appear while it works. Afterwards the UI tells you to choose **Cancel**. Formatting is the one thing that breaks a flashed card. Seeing only `RGDS-BOOT` in Explorer is normal.

**Windows first launch:** the .exe isn't code-signed, so SmartScreen says "Windows protected your PC" (More info → Run anyway), and the UAC prompt names an unknown publisher. Some antivirus programs distrust unsigned programs that write raw disks. Signing it with a code-signing certificate fixes both.

**Linux:** cards mounted under `/media`, `/run/media` or `/mnt` are offered. A disk with anything mounted elsewhere (`/`, swap, a live USB's medium) never is. The writer runs as root through `pkexec`, which needs a desktop polkit agent; without one, start the flasher with `sudo`.

**macOS first launch:** the app is not notarized (that needs a Developer ID), so macOS blocks the first open. Right-click → Open, or System Settings → Privacy & Security → Open Anyway.

## Building

```sh
flasher/build.sh          # all targets into flasher/dist/, plus SHA256SUMS
flasher/build.sh --test   # runs the Go tests first
```

It needs Go 1.21 or newer, which fetches the toolchain `go.mod` pins (Go 1.25: older linkers make macOS binaries that macOS 26 won't start). There's no cgo and no outside module, so it cross-compiles from any OS. The macOS app comes out universal when `lipo` is there. With `CODESIGN_IDENTITY="Developer ID Application: …"` it is signed for notarization; without it, it is signed ad hoc.

`tools/own_os_release.py` builds the flasher and attaches the three files to every OS release (`--no-flasher` leaves them out). To add them to a release that is already out:

```sh
flasher/build.sh && gh release upload v0.8.0 flasher/dist/RG-DS-Flasher-* --repo kleinrein/rgds-core
```

The README links `releases/latest/download/RG-DS-Flasher-…`, so those links follow each new release.

## Options and testing

```sh
"RG DS Flasher" --image path/to/RG-DS-OS-0.8.0.img.gz   # flash a local image (checked against a .sha256 next to it)
"RG DS Flasher" --list                                  # print the cards it would offer
"RG DS Flasher" --no-window                             # print the address instead of opening a window
```

`RGDS_FLASHER_DISK_IMAGES=1` (macOS) also offers attached disk images, so the whole flow can run without a card:

```sh
hdiutil create -size 2g -type SPARSE -layout NONE test.sparseimage
hdiutil attach -nomount test.sparseimage        # /dev/diskN, writable without a password
RGDS_FLASHER_DISK_IMAGES=1 "RG DS Flasher" --no-window
```

This also lists other attached images (Xcode simulator runtimes, for example). Pick yours by its `/dev/diskN`. Those others are root-owned, so writing to one would ask for your password first.

`go test ./...` covers the write and verify path against a file: zero blocks, a padded last sector, a card that reads back wrong, a damaged gzip, and a reader that drops the card before the check. `RGDS_NET=1 go test -run Network` checks that the API and the release-page fallback find the same assets.

The UI's colours, font (Nunito) and logo come from `themes/default/`, `assets/fonts/source/` and `assets/brand/`. `build.sh` copies the font and logo into `ui/`. `packaging/` holds the app icon (rendered from `assets/brand/mark.svg`), the Windows manifest and the resource script that `rsrc_windows_amd64.syso` is compiled from. To regenerate that file, run `llvm-windres --target=x86_64-w64-mingw32 -O coff flasher.rc ../rsrc_windows_amd64.syso` in `packaging/`.
