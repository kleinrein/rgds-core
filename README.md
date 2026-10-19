# RG DS Core

A fast, friendly home for your Nintendo DS games on the **Anbernic RG DS**, built on the stock Linux firmware. It boots straight into a DSi-style library, plays games in **DSCore**, our tuned DS emulator with sharp 2×–5× 3D, and sleeps when you close the lid.

![The home screen, the library in dark mode and the list view](docs/images/screens.png)

> **Community preview.** Tested on the standard RG DS. RG DS Plus support is built in, but nobody has tried it on real hardware yet. Plus owners, [please help test](docs/RG_DS_PLUS.md).

## Highlights

- **Two-screen library:** cartridge, cover and list views; search, A–Z jump, favourites, recents and play time. Cover art downloads by itself.
- **DSCore emulator:** GPU-rendered 3D at native, 2×, 3×, 4× or 5×; switch resolution mid-game; 20 shaders and LCD filters; fast-forward.
- **One menu for every game:** pause, nine checkpoints per game with previews, undo, a game switcher and save backups.
- **Choose your emulator per game:** DSCore (default), DraStic, DSperate, melonDS and NNDDSS.
- **Make it yours:** light or dark, accent colours, high contrast, rounded or pixel fonts, rich or pixel icons, page transitions, menu sounds and 14 languages.
- **PictoChat:** an original chat pad on the Home screen. Four Wi-Fi rooms with other RG DS Core handhelds, pens, rainbow ink, stamps and an on-screen keyboard. No BIOS or Nintendo files, and it cannot talk to a real DS. See [PictoChat](docs/PICTOCHAT.md).
- **Safe updates:** signed over-the-air updates. A broken update rolls itself back.

## DSCore performance

DSCore started as a fork of the [DSperate](dscore/FORK.md) emulator. On the RG DS's Mali-G52 it renders 3D through Vulkan, composes both screens on separate threads and presents off the emulation thread.

| Game, scene                                       | Native | 2×      | 3×  |
| ------------------------------------------------- | ------ | ------- | --- |
| Mario Kart DS, race                               | 60     | 60      | 60  |
| Mario Kart DS, Cheep Cheep Beach (lap 3, ink)     | 60     | 60 (56) | —   |
| Metroid Prime Hunters, intro                      | 60     | 60      | —   |
| Kingdom Hearts 358/2 Days and Re:coded, intro     | 60     | 60      | —   |
| Golden Sun: Dark Dawn, intro                      | 60     | 56      | —   |
| Zelda: Phantom Hourglass, intro                   | 60     | 54      | —   |
| Zelda: Spirit Tracks, intro                       | 60     | 47      | —   |

Emulated frames per second on a standard RG DS, audio muted (October 2026). **Adaptive 2×** keeps the frame rate: when the GPU falls behind in a heavy moment, DSCore draws that stretch at native resolution and returns to 2× as soon as it can (the number in brackets is without it). Switching between 2× and 5× mid-game takes about 0.1 s; the engine starts in under 2 s even at high resolution. [How we measure](docs/PERFORMANCE.md).

## Install

You need the RG DS's **stock Linux card** and a computer to copy files.

1. Download `RG-DS-Core-Installer-<version>.zip` from the releases page.
2. Extract it to the root of the games card. Merge its `Roms/` folder and copy `RGDS-Install/` next to it.
3. On the handheld, open **APPS → Install RG DS Core**. It installs, opens and starts on future boots.
4. Put your `.nds` files in `Roms/NDS`.

Updates arrive over Wi-Fi under **Settings → System → Updates & recovery**. Saves and settings are always kept. **APPS → RGDS Recovery** brings back the stock menu. More detail: [installing](docs/SD_FLASH.md), [first-time card setup](docs/DEVICE_SETUP.md), [saves](docs/SAVES.md).

**Controls:** A opens or plays, B goes back, Y opens View, X opens game Options, and L2/R2 jump by letter. In a game, the Home button opens the RG DS Core menu.

## Good to know

- Games, BIOS files and cover art are not included. DSCore runs without BIOS files.
- Each emulator keeps its own saves; switching engines does not move your progress.
- Hotel Dusk: Room 215 does not boot in DSCore yet, so it starts in DraStic by default.

## For developers

```sh
make preview        # launcher on your computer
make test           # tests
make package        # application package (needs the engine payloads)
make installer      # drop-on-card installer
```

See [development](docs/DEVELOPMENT.md), the [documentation index](docs/README.md) and [releasing with automatic updates](docs/OTA.md). The emulator lives in [`dscore/`](dscore/README.md).

AI tools helped write the code, documentation and artwork, so keep backups of your saves. Third-party components keep their own licenses; see [THIRD_PARTY.md](THIRD_PARTY.md).
