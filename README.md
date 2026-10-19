# RG DS Core

A DSi-style home for your Nintendo DS games on the **Anbernic RG DS**, on the stock Linux firmware. Boots straight into your library, plays games in **DSCore**, our emulator with sharp 2×–5× 3D, and sleeps when you close the lid.

![Home screen, the library and the in-game menu, captured on an RG DS](docs/images/screens.png)

> **Community preview.** Tested on the standard RG DS. RG DS Plus support is built in but untested on hardware. Plus owners, [please help test](docs/RG_DS_PLUS.md).

## Highlights

- **Library:** cartridges, covers or a list. Search, A–Z jump, favourites, recents, play time. Covers download by themselves.
- **DSCore:** 3D at native to 5×, switchable mid-game. 20 shaders and LCD filters. Fast-forward.
- **One menu in every game:** pause, nine checkpoints with previews, game switcher, save backups.
- **Other engines per game:** DraStic, DSperate, melonDS, NNDDSS.
- **Your look:** light or dark, accent colours, high contrast, rounded or pixel fonts, menu sounds, 14 languages.
- **PictoChat:** our own chat pad. Wi-Fi rooms between RG DS Core handhelds, pens, stamps, rainbow ink. No Nintendo files. [More](docs/PICTOCHAT.md).
- **Safe updates:** signed over-the-air updates that roll back if they fail.

![Cartridge, cover and list views](docs/images/library.png)

![Light theme home, favourites, and the Theme & display settings](docs/images/settings.png)

![PictoChat lobby and a chat room](docs/images/pictochat.png)

## DSCore performance

DSCore is a fork of [DSperate](dscore/FORK.md). 3D goes through Vulkan on the Mali-G52, both screens are composed off the emulation thread.

![Mario Kart DS and New Super Mario Bros in DSCore, and the Graphics page](docs/images/dscore.png)

| Game, scene                                   | Native | 2×      | 3×  |
| --------------------------------------------- | ------ | ------- | --- |
| Mario Kart DS, race                           | 60     | 60      | 60  |
| Mario Kart DS, Cheep Cheep Beach, lap 3, ink  | 60     | 60 (56) | —   |
| Metroid Prime Hunters, intro                  | 60     | 60      | —   |
| Kingdom Hearts 358/2 Days and Re:coded, intro | 60     | 60      | —   |
| Golden Sun: Dark Dawn, intro                  | 60     | 56      | —   |
| Zelda: Phantom Hourglass, intro               | 60     | 54      | —   |
| Zelda: Spirit Tracks, intro                   | 60     | 47      | —   |

Emulated fps, standard RG DS, October 2026. **Adaptive 2×** (default) drops to native for heavy moments and comes back; the bracket is without it. Changing resolution mid-game takes 0.1 s. [How we measure](docs/PERFORMANCE.md).

## Install

You need the RG DS's **stock Linux card** and a computer.

1. Download `RG-DS-Core-Installer-<version>.zip` from [releases](https://github.com/kleinrein/rgds-core/releases).
2. Extract to the root of the games card: merge `Roms/`, copy `RGDS-Install/` next to it.
3. On the handheld: **APPS → Install RG DS Core**. It installs, opens and starts on every boot.
4. Put `.nds` files in `Roms/NDS`.

Updates arrive over Wi-Fi: **Settings → System → Updates & recovery**. Saves and settings are kept. **APPS → RGDS Recovery** brings the stock menu back. Details: [installing](docs/SD_FLASH.md), [card setup](docs/DEVICE_SETUP.md), [saves](docs/SAVES.md).

**Controls:** A plays, B goes back, Y is View, X is Options, L2/R2 jump by letter. In a game, Home opens the menu.

## Good to know

- No games, BIOS files or cover art included. DSCore needs no BIOS.
- Each engine keeps its own saves.
- Hotel Dusk: Room 215 does not boot in DSCore yet; it starts in DraStic.

## For developers

```sh
make preview        # launcher on your computer
make test           # tests
make package        # application package (needs the engine payloads)
make installer      # drop-on-card installer
```

See [development](docs/DEVELOPMENT.md), the [documentation index](docs/README.md) and [releases with automatic updates](docs/OTA.md). The emulator lives in [`dscore/`](dscore/README.md).

AI tools helped write the code, documentation and artwork, so keep backups of your saves. Third-party components keep their own licenses; see [THIRD_PARTY.md](THIRD_PARTY.md).
