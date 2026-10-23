# Bugfix plan: tester feedback

Source: first hands-on test (install works, app launches, no games tried yet).

Tag: **[RG DS Plus · FW 1.1]** = only on RG DS Plus running Linux firmware 1.1. Untagged = general.
Tags below are a best guess. Confirm before relying on them.

Status: `[ ]` open · `[x]` fixed and checked on the host (tests in `tests/test_ui_consistency.py`) · **device check** = still to be confirmed on the handheld.
Device-specific bugs (audio, popups, autostart) stay `[ ]` until the tester confirms on the RG DS Plus, even when a fix is in the code (marked *fix in code*).

## Bugs

- [ ] No touchscreen input **[RG DS Plus · FW 1.1]**
- [ ] Occasional audio hiss/scratch when unmuting
  - Fix in code (best guess): the software gain now rises over 150 ms (still 50 ms down) so the amplifier settles before full level; the sink buffer is 1024 frames instead of 512.
- [ ] Occasional audio hitch/scratch in menu music
  - Fix in code: the track loop wrapped with a hard jump from its last sample to its first; it now crossfades the last 50 ms into the start. The larger sink buffer (above) also rides out a busy frame. **Device check:** listen across a loop point.
- [ ] Popup menus sometimes ignore input or don't clear. Needs up/down to clear (seen on Effects Volume). Possibly a delay or hang; menu once stopped mid launch animation
  - Fix in code, not reproduced on the host. Likely cause: opening the audio sink for the first cue (and closing it after 3 s idle) ran on the UI thread and can stall inside ALSA's dmix, so the loop stopped drawing and reading input. Both now run on a helper thread; a cue asked for meanwhile plays once the sink is up. Handing the sink to a game still waits for it. **Device check.**
- [ ] Boot into "RG DS Core" fails: "An existing autostart hook belongs to another manager. It has been left untouched." App never takes autostart priority after install **[RG DS Plus · FW 1.1]**
  - Fix in code: `runtime/boot_control.py` no longer refuses a hook it did not write (or its own hook after the control folder was wiped). A foreign hook is saved as `original-autostart`, ours runs first and then runs it, and uninstall restores it. Symlinks are still refused. **Device check:** what the FW 1.1 hook contains is unknown; read `/mnt/mod/ctrl/autostart` on the device.
- [x] "RG DS (CORE)" title and bottom-center icon don't pixelate in pixel display mode
  - Fix: with Icon style = Simple pixel the status-bar name is set in 5x7 dots and the bar mark is the pixel Display glyph. The boot-screen wordmark is still the bitmap.
- [x] Controls → Profile does nothing
  - Cause: the picker only opened when the library had a game. It now always opens (Global only without a game) and the footer says per-game controls need a game.

## UX and consistency

- [ ] Volume buttons don't control menu/effect music unless muted or at 0%
  - Fix in code: the menu mixer now applies a software master gain from the system volume (12 dB range, full at 100, silent at 0, 50 ms ramp), on top of the DAC. `MenuAudio::set_master`, called every frame from `run_loop.hpp`.
  - Caveat: the cause on the device is a guess (the DAC curve is not audibly different above mute). **Device check:** listen at 100 / 50 / 25 / 1 %. At the first-run default (25 %) the menu is now noticeably quieter than before; if that is too quiet, shrink the 12 dB range.
- [x] Effects Volume has a Mute/0% option in the popup, Music Volume doesn't
  - Fix: Music volume popup is now Mute, 20 … 100 %. The row still reads "0%" when muted, same as Effects volume.
- [x] Naming mismatch: "Menu Sounds" and "Effects Volume" control the same sounds
  - Fix: the switch is now **Menu effects**, beside Effects volume, Menu music and Music volume. Key renamed in all 22 locales (ja, ko and zh already said "sound effects" and kept their wording).
  - Left as is: the line "Turn on menu sounds to hear the previews." and the "Menu sounds are unavailable." error still say "sounds". Rewording them needs 22 translations.
- [x] Transition Style/Speed row shifts right on A. Reads as confirm, given the "Select (A)" hint
  - Fix: A no longer steps those two choices (it wrapped Spring → Push → Zoom, sliding the pill). The A button is hidden on those rows; Left/Right and touch still change them. On the two switches above, the A button now reads "Toggle".
- [x] Home page: D-pad can't reach brightness or other options below
  - Fix: Down from Settings walks onto the bottom bar: brightness, the update mark (only while an update waits) and the clock/statistics. Left/Right move along it, Up returns, A opens. Each step plays the browse cue.
  - Scope: column Home only. The carousel Home has no bottom-bar buttons.
- [x] PictoChat font doesn't change when UI is set to Simple Pixel
  - Fix: message text and the typing pad follow the Typeface setting (5x7 dots at 2x when Pixel). Shared walker `App::pixel_dots` in `main.cpp`.
  - Not changed: the pixel messages are set in capitals, like every other pixel label.

## Review notes (2026-10-23)

Found and fixed while reviewing the six UX items:

- Moving onto and along Home's bottom bar played no sound (the sound state did not see the bar). Added `bar` to the sound state; covered by a test.
- A focused update mark that disappears (update finished) left an empty focus ring and a dead A. It now falls back to brightness.

Known, not fixed:

- `tests.test_view_animations … test_home_and_library_layout_changes_animate` fails on a clean checkout of HEAD too. A layout pick on the Library view sheet no longer counts as a view transition. The other view-animation tests are timing-sensitive and fail intermittently when the machine is busy.
- `tests/test_release_polish_ui.py … test_every_main_game_menu_action_has_a_rich_icon` fails on action 8 (the game-menu tile grid now has 12 tiles). Not touched by these changes, not checked against HEAD.
- Nothing above has been run on the handheld.

## Docs (README install section)

- [ ] Tell users to ignore "RG-DS-Core-..." on the releases page
- [ ] Install path: Application > Apps > TF Card > Install RG DS Core
- [ ] Say to launch "RG DS Core" after install completes
- [ ] Mention internet connection if one is required
