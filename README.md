<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/brand/lockup-dark.svg">
    <img alt="RG DS Core" src="assets/brand/lockup-light.svg" width="380">
  </picture>
</p>

# RG DS Core

A DSi-style home for your games on the **Anbernic RG DS**, on the stock Linux firmware. Boots straight into your library, plays Nintendo DS games in **DSCore**, our emulator with sharp 2×–5× 3D (may cause visual glitches and will struggle on high 3D games), plays Game Boy Advance, Game Boy and Game Boy Color games in **RG DS Play** with the game's own map or HUD on the second screen, and sleeps when you close the lid.

![Home screen, the library and the in-game menu, captured on an RG DS](docs/images/screens.png)

> **Community preview.** Tested on the standard RG DS. May work on RG DS Plus but this is not tested since I don't own one. Plus owners, [please help test](docs/RG_DS_PLUS.md).

## Highlights

- **Library:** cartridges, covers or a list. Search, A–Z jump, favourites, recents, play time. Covers, descriptions, genres and release dates download by themselves.
- **DSCore:** 3D at native to 5×, switchable mid-game. 32 shaders and LCD filters, plus HD 2D scaling for the flat screens. Fast-forward. Reduced input latency.
- **GBA, GB and GBC:** play them on the same shelf (Settings → Library → Show all systems). The second screen shows what the game hides: the map in Metroid Fusion and Castlevania, the HUD in Mario Kart: Super Circuit, your team in Pokémon. About 40 games have profiles. The original screens of the Game Boy, Pocket, Light, Color and GBA, shell frames. [More](docs/PLAY.md).
- **Original in game menu:** pause, checkpoints, cheats, game switcher and game settings, the same for every system.
- **Cheats:** Action Replay codes in DSCore, switched on and off while you play, remembered per game. [More](docs/CHEATS.md).
- **Ports:** PortMaster ports on the same shelf under Show all systems, with the same in-game menu. The libraries the stock firmware lacks download by themselves, so 32-bit ports, desktop-OpenGL ports and Westonpack ports run too. Games that need your own files show a QR code: open it on your phone or computer and drop the files in the browser. Mouse-driven ports can be played by tapping the bottom screen.
- **Other engines available:** DraStic, DSperate or melonDS.
- **Tailored:** Light or dark, accent colours, high contrast, rounded or pixel fonts, menu sounds and music, 22 languages including Japanese, Chinese and Korean, all time zones, a drawn icon for every system.
- **PictoChat:** Our homegrown chat client. Wi-Fi rooms between RG DS Core handhelds with pens, stamps, rainbow ink. No Nintendo files. [More](docs/PICTOCHAT.md).
- **Safe updates:** Signed over-the-air updates that roll back if they fail. Home shows a banner when one is out.

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
- No games, BIOS files or cover art included. DSCore and RG DS Play need no BIOS.
- Other systems are off until you turn on **Settings → Library → Show all systems**. Companion screens exist only for the games with a profile; the rest get a title card and play as usual.
- Each engine keeps its own saves.

## Developing

```sh
make preview        # launcher on your computer
make test           # tests
make package        # application package (needs the engine payloads)
make installer      # drop-on-card installer
```

See [development](docs/DEVELOPMENT.md), the [documentation index](docs/README.md) and [releases with automatic updates](docs/OTA.md). The emulator lives in [`dscore/`](dscore/README.md).

AI tools helped write the code, documentation and artwork, so keep backups of your saves. Tested on a RG DS, not a plus. Third-party components keep their own licenses; see [THIRD_PARTY.md](THIRD_PARTY.md).
