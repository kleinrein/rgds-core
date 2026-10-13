# Release polish — October 12, 2026

## Interface

- The horizontal blue thumb now fills the 48px inner track height, retaining its
  118px width, grey viewport window and grey position marker.
- Language is the first System row. English (US) is first/default; Deutsch,
  Español, Français, Italiano, Norsk bokmål and Português (Brasil) follow by
  native name. Existing language preferences survive updates.
- Connectivity and the network list show the current SSID on the top screen.
  Disconnected and radio-off states use existing localized labels.
- Sound/mute and Wi-Fi use new high-contrast 28px antialiased status assets, with
  light/dark variants and radio-off/signal-level states.
- Game details align times played and time played in equal columns. The artwork
  provider label is removed from the top screen; attribution remains in metadata.
- Eight additional imagegen assets complete the ten main in-game actions in Rich
  mode, including resume, save/load, shaders, brightness, restart and home. Pixel
  mode remains available; the saved style is preserved.
- Returning from System subpages selects their new row, including restoration of
  the final Startup row after a launcher restart.

## Installer and validation

`packaging/install_package.py` is shared by SD and SSH deployment.
`tools/package_installer.py` produces a 31 MB drop-on-card candidate with an APPS
entry, complete-package checksum and README. See [installation](../SD_FLASH.md)
and [release/OTA plan](../RELEASE_PLAN.md). This is an app installer, not a flash image.

Host validation exercised real SDL layout, both fonts/themes, language persistence
into the game menu, all rich menu icons, scrollbar geometry, navigation, library
caching and startup integrity. Two old tests were corrected: font sampling now
isolates text instead of counting colors in rich artwork, and the game-menu test
uses its own navigation state as the runtime does. Installer tests cover clean
install, upgrade, rollback, preserved saves, corruption, unsafe archives, low
space, active gameplay, interrupted activation and retry.

The development RG DS at the user's supplied address successfully ran the SD
APPS shell entry via SSH, installed the package and reopened the launcher. Actual
dual-screen capture confirmed the new scrollbar and status assets, with the
existing dark theme and pixel icon preference retained. This is an upgrade test;
a clean spare-card installation selected through the physical stock APPS menu
still needs acceptance. No game was running during installation.

Evidence: `build/release-polish/` contains screenshots, build/test logs, installer
metadata and device status. The first on-device candidate was
`0.0.1-dev-454af24cd33c`; the final candidate adds the System focus restoration fix
as `0.0.1-dev-4d874c1d22cb`.

OTA remains unimplemented. Public release is blocked by unresolved project and
NNDDSS redistribution licensing, signing and release infrastructure. App rollback
is manual; shared bootstrap and OS/boot partitions are not transactional A/B.
No boot/power hardware behavior is changed or claimed fixed in this pass.
