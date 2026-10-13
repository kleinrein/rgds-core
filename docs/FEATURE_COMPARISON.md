# Feature comparison and priorities

Checked October 12, 2026 against the projects' own documentation. Their claims are
not independent performance measurements. RG DS Core's column reflects the local
implementation and its documented validation limits.

## Where RG DS Core stands

It covers the central DS loop: find a game, choose an engine, launch, pause,
save/load a checkpoint, adjust controls/shaders, exit and return to the library.
It has favourites, recents, artwork, play statistics, translations and device
settings. The OTA foundation adds authenticated staging and startup rollback.
This is a substantial DS-focused alpha, not a feature-complete firmware release.

| Area | RG DS Core today | Useful next step |
| --- | --- | --- |
| Saves | Nine-slot gallery, available screenshots, timestamps, overwrite/load undo, validated slot import/export and idle backup UI | Cross-engine native-save conversion, backup restore UI and cloud sync |
| Starting again | Default launcher boot; opt-in power-off resume with verified one-shot tickets | Repeated physical power-cut/cold-boot acceptance |
| Library | Three layouts, search, filters, favourites/recents, A–Z, first-run ROM setup and statistics | Larger real-card stress matrix |
| DS-specific inputs | DSperate/melonDS real microphone, simulated blow, lid and DS language controls | Expand verified controls to closed-engine adapters and test game-specific microphone behavior |
| Network features | Wi-Fi, public artwork; authenticated OTA with automatic check/download options | Save sync and simple file transfer, with conflict-safe backups |
| Achievements/cheats | No unified RG DS Core interface | Engine-aware RetroAchievements and cheat support; avoid unsupported cross-engine promises |
| Performance | Four engines, power profiles, shaders and selected hardware measurements | Broader game matrix, frame pacing/input/audio latency measurements and long-session validation |
| Recovery | Session guardian, previous-release recovery and OTA trial supervisor | Spare-card power-cut matrix, game-freeze watchdog and release acceptance |
| Distribution | Stock Linux SD installer candidate | Public license/provenance, signed release channel, spare-card installation guide; independent image later |

## GammaOS Nano

GammaOS Nano is broader Android firmware. Its official documentation describes
multi-system emulation, applications/media, network shares, several theme families
and Syncthing. Those are mostly absent from our DS-only scope.
[Official overview](https://thegammasqueeze.github.io/GammaOSNano-WIKI/index.html),
[Syncthing](https://thegammasqueeze.github.io/GammaOSNano-WIKI/syncthing.html).

Its DS menu includes multiple save slots, save/load undo, achievements, cheats,
DS language, microphone controls, save import and advanced renderer options.
Our shared interface now covers slots, undo and validated slot import, plus DS controls for DSperate/melonDS. Achievements and cheats remain missing; our optional
SuperDrastic backend should not be mistaken for a complete unified implementation.
[Official DraStic Nano reference](https://thegammasqueeze.github.io/GammaOSNano-WIKI/drastic-nano.html).

Quick Resume can save on power-off and reopen the same game after boot, with
corrupt-state and repeated-resume safeguards. Our one-shot shutdown-resume flow is narrower: it arms after returning home with a successful save and using RG DS Core Power off.
[Official Quick Resume guide](https://thegammasqueeze.github.io/GammaOSNano-WIKI/quick-resume.html).

## ROCKNIXDS

ROCKNIXDS documents fresh-card images, separate RG DS/Plus releases, menu updates
with stable/beta channels, achievements, a save-slot menu with previews/undo,
a game-freeze watchdog, menu sounds and automatic artwork. It also describes
its Gengis renderer and performance work, plus optional ports and Pokémon tools.
Our significant gaps are distribution maturity, achievements,
freeze recovery and a broader hardware/performance validation matrix. We already
have DS library/artwork/statistics and an optional SuperDrastic integration,
but have not established performance parity or adopted every upstream feature.
[Official ROCKNIXDS README and release history](https://github.com/JorreFog/rgds-rocknix).

## Recommended order

1. Finish release reliability: clean-card install, power/sleep cycles, destructive
   update fault tests on disposable media, signing custody and licensing.
2. Validate the new save gallery/undo/import/export against more real gameplay;
   extend native-save import and add whole-backup restoration.
3. Complete DS essentials: microphone/blow, emulated lid, DS language and a
   repeatable test matrix for games that depend on them.
4. Validate search at collection scale and physical power-off resume; then achievements and sync.
5. Expand to other systems/media only if the product scope changes.

The two-press cold-start report is still unresolved. Neither this comparison nor
an app updater proves the PMIC/boot hardware issue is fixed.
