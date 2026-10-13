# Next stock RG DS session

The owner authorized SSH deployment and emulator testing on 2026-10-11. The [device review](archive/DEVICE_REVIEW_2026-10-11.md) records completed smoke tests; the subsequent [gameplay review](archive/GAMEPLAY_REVIEW_2026-10-11.md) records matched World 1-1 scenes, measured fast-forward and remaining limits. Anbernic Linux remains the boot baseline; no replacement image or SD flash is planned. `dist/latest.txt` selects the application package.

The candidate includes the previous browser/artwork/touch polish, paused game
menu, lid watcher, 21 shader choices, DSperate default and the new source-built
melonDS SDL engine. The latter has synthetic Linux ARM64 checks; retail gameplay,
physical dual displays, controls, touch, audio and microphone remain unaccepted.
DraStic still requires a game restart for Threaded 3D; melonDS applies it live.

## Remaining device work

- Read free space. Eleven inactive internal releases were archived with full file-hash verification to the games card, recovering about 317 MiB. Retain
  active/previous releases, ROMs, saves, user preferences and stock boot backup.
- Repeat power cycles after the October 12 RK817 saved-state repair. Ordinary
  stock restart returned normally; the owner confirmed stock and RGDS Power off
  both extinguished the LED, and normal Power press started the device. Repeat
  on battery/charger; do not use forced
  power cuts as a fallback or revive the retired alternate-OS card.
  See [power/Wi-Fi investigation](archive/POWER_WIFI_2026-10-12.md).
- Read NNDDSS configuration/logs and measure baseline FPS, clocks and temperature.
  The user reports every game is slow. One old log near 60 FPS does not refute that.
- Check NNDDSS's private library links resolve to the active verified release;
  do not prune that release during play. Test release switch/rollback and a
  copy-only card filesystem separately. See [launch/menu changes](archive/LAUNCH_MENU_POLISH_2026-10-11.md).
- Set the requested global default to DSperate, preserving per-game choices.
  New installations already default to DSperate; the installed backup had NNDDSS.
- Check the current emulator title and **Next emulator** picker using buttons
  and touch, including **Use default** and Cancel. A cross-engine restart saves
  the old engine's checkpoint and uses the selected engine's separate files;
  it must not imply save conversion. Confirm unsupported settings remain inert,
  and check all descriptions fit rounded and pixel fonts. See the
  [settings and add-on review](archive/DRASTIC_ADDONS_2026-10-11.md).
- Enable Settings → Power → Sleep on lid close. Existing explicit `lid=0` is
  preserved by upgrades, while new installations default to On. The October 12
  physical trace identified stock close/open key pulses (110/111); the new shared
  event monitor replaces the incorrect hallkey sysfs position check. Repeat
  physical sleep/wake acceptance on this candidate.

## Acceptance with the application candidate

The [ROCKNIXDS review](archive/ROCKNIXDS_REVIEW_2026-10-11.md) adds a recognized-binary
DraStic thread-start barrier and quieter idle input/logging. Repeat DraStic
launch/quit and checkpoint restores with threaded 3D enabled, retain any
`helper initialization timed out` diagnostic, and compare idle launcher CPU
with the gamepad present. Native VM checks do not close this hardware item.

1. Confirm both screens, navigation, touch, header alignment in both fonts,
   Covers/List/cartridge views, scrollbar and A–Z; background artwork must not
   block input. Public Automatic/GameTDB/Libretro need no developer credentials.
2. In all four engines, check controls, touch, sound, native battery save, RGDS
   checkpoint, actual restored progress after relaunch, and clean return home.
   Use a copied known save for initial melonDS compatibility testing.
3. Open the game menu: gameplay/audio must pause, menu input stays responsive,
   and resume restores correct screens/input/audio. Test mapped fast-forward
   press/release, release while paused, and live shaders on both screens.
4. In melonDS, toggle Threaded 3D off/on without restarting and verify unchanged
   progress and FPS-counter preference. Test real microphone capture with a game that requires it. NDS
   direct boot only: DSi system mode, camera, multiplayer, Slot-2 and add-ons are
   not implemented in this frontend.
5. Benchmark the same known titles with shader Off, native 3D and the same power
   profile. Capture game and display FPS separately, CPU clocks/temperature,
   process usage and menu latency. Compare against equivalent SimpleOS/ROCKNIX DS
   sessions; no competitive performance claim exists yet.
6. Repeat lid close/open at home, in gameplay and in the paused menu. Check
   physical screens, input, audio, Wi-Fi and game progress after wake. Ten cycles
   on each path are hardware checks, separate from simulated tests.
7. Verify Wi-Fi, charging/battery indicators and shutdown on battery/charger.
   Stock and RGDS Power off passed after the saved-state repair. The replacement
   boot logo, direct RGDS startup and B staying at Home were confirmed on October
   12; repeated power cycles and changed-network Wi-Fi acceptance remain. Preserve
   `logs/system-exit.ini`, `logs/system-exit.log`, lid/session/engine logs on failure.

Record the exact release and boot ID with every result. Stock boot and device
failure evidence remains under `build/stock-polish-20261011` and `build/os`.
The full former checklist is archived in
`build/retired-armbian-20261011/historical-documents.tar.gz`.

Cross-engine save sharing is not implemented. Preserve each engine's native
saves and checkpoints separately; follow [the save guide](SAVES.md) before any
manual import experiment.

## Optional SuperDrastic — physical acceptance still pending

Native startup, checkpoint exit and checkpoint reload in both directions with Standard passed SSH smoke tests. The original-ROM DS counter measured 59.93 emulated FPS normally, 785.27 during native-protocol fast-forward, and 59.97 after returning to normal. This synthetic result proves acceleration, not retail speed or a physical button press. See [the measurement and method](SUPERDRASTIC.md#actual-emulation-speed-check--2026-10-11). Continue with one game's override; keep DSperate and Standard defaults intact.

1. Record standard DraStic/DSperate launch and menu latency and same-scene FPS.
2. Select SuperDrastic for that game. Verify Weston releases DRM, both panels
   appear, touch maps correctly, input/audio work, and native menu pauses the game.
3. Check Anbernic menu, Start + Anbernic exit, remapped controls and fast-forward
   hold/toggle; releasing, menu entry and wake must restore normal speed.
4. Check native menu volume/brightness, physical volume keys, battery/charging
   and lid close/open with audio recovery. No power/boot acceptance is implied.
5. Make an in-game save and checkpoint, exit/relaunch, then switch to Standard
   DraStic and verify actual saved progress. Try auto-load on with auto-save off.
6. Exercise a failed display acquisition and normal exit; launcher panels and
   navigation must return. Inspect native ready/exit receipts and logs.
7. Compare identical resolution, shader-off and power policy. Keep SuperDrastic
   experimental until these pass; do not change defaults from upstream claims.
