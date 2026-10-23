<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/brand/lockup-dark.svg">
    <img alt="RG DS Core" src="assets/brand/lockup-light.svg" width="380">
  </picture>
</p>

# RG DS Core

A DSi-style home for your Nintendo DS games on the **Anbernic RG DS**, on the stock Linux firmware. Boots straight into your library, plays games in **DSCore**, our emulator with sharp 2×–5× 3D (may cause visual glitches and will struggle on high 3D games), and sleeps when you close the lid.

![Home screen, the library and the in-game menu, captured on an RG DS](docs/images/screens.png)

> **Community preview.** Tested on the standard RG DS. May work on RG DS Plus but this is not tested since I don't own one. Plus owners, [please help test](docs/RG_DS_PLUS.md).

## Highlights

- **Library:** cartridges, covers or a list. Search, A–Z jump, favourites, recents, play time. Covers download by themselves.
- **DSCore:** 3D at native to 5×, switchable mid-game. 20 shaders and LCD filters. Fast-forward. Reduced input latency.
- **Original in game menu:** pause, checkpoints, cheats, game switcher and game settings.
- **Other engines availables:** DraStic, DSperate or melonDS.
- **Tailored:** Light or dark, accent colours, high contrast, rounded or pixel fonts, menu sounds and music, 22 languages.
- **PictoChat:** Our homegrown chat client. Wi-Fi rooms between RG DS Core handhelds with pens, stamps, rainbow ink. No Nintendo files. [More](docs/PICTOCHAT.md).
- **Safe updates:** Signed over-the-air updates that roll back if they fail.

![Cartridge, cover and list views](docs/images/library.png)

![Light theme home, favourites, and the Theme & display settings](docs/images/settings.png)

![PictoChat lobby and a chat room](docs/images/pictochat.png)

## DSCore performance

DSCore is a fork of [DSperate](dscore/FORK.md). 3D goes through Vulkan on the Mali-G52, both screens are composed off the emulation thread.

Input delay: finished frames are handed to the panels directly instead of through the compositor's GL path, which cut the wait from a finished frame to its page flip from about 17–22 ms to 10–12 ms on a standard RG DS (October 2026, measured with the display's own flip timestamps). Touch on that direct path reaches the game as well. [How we measure](docs/PERFORMANCE.md).

![Mario Kart DS and New Super Mario Bros in DSCore, and the Graphics page](docs/images/dscore.png)

## Install

You need the RG DS's **stock linux version V1.0** and a computer.

> This is not tested on V1.1 yet.

1. Download `RG-DS-Core-Installer-<version>.zip` from [releases](https://github.com/kleinrein/rgds-core/releases).
2. Extract to the root of the card: merge `Roms/`, copy `RGDS-Install/` next to it.
3. On the handheld: **APPS → Install RG DS Core**. It installs, opens and starts on every boot.
4. Put `.nds` files in `Roms/NDS`.

Updates arrive over Wi-Fi: when one is out, Home shows a banner and **Y** installs it (or **Settings → System → Updates & recovery**). Saves and settings are kept. **Settings → System → Startup** brings the stock menu back, and **APPS → Uninstall RG DS Core** removes the app while keeping your saves. Details: [installing](docs/SD_FLASH.md), [card setup](docs/DEVICE_SETUP.md), [saves](docs/SAVES.md).

**Controls:** A plays, B goes back, Y is View, X is Options, L2/R2 jump by letter. In a game, Home opens the menu and R2 fast-forwards (change it under Controls).

## Good to know

- Only tested on the standard RG DS with anbernic linux V1.0.
- No games, BIOS files or cover art included. DSCore needs no BIOS.
- Each engine keeps its own saves.
- DSCore engine is new and not optimized properly yet.

## Developing

```sh
make preview        # launcher on your computer
make test           # tests
make package        # application package (needs the engine payloads)
make installer      # drop-on-card installer
```

See [development](docs/DEVELOPMENT.md), the [documentation index](docs/README.md) and [releases with automatic updates](docs/OTA.md). The emulator lives in [`dscore/`](dscore/README.md).

AI tools helped write the code, documentation and artwork, so keep backups of your saves. Tested on a RG DS, not a plus. Third-party components keep their own licenses; see [THIRD_PARTY.md](THIRD_PARTY.md).
