# Screenshots and the Album

## Taking a picture

- **In a game:** press **Menu + R** (either order), or choose **Screenshot** in the Game menu. The Screenshot tile sits beside Resume.
- **In the launcher:** press **Menu + R**, or Print Screen on a keyboard.
- A Menu press that is still held now waits for R, for up to 0.6 s. A lone Menu opens the switcher when you let go. A quick tap behaves as before. The same rule is in `controls.Shortcut` and in the bridge's `ControlShortcut`, so neither path opens the switcher early. The game runner also drops the bridge's late Menu press when R joined it (`MenuInput.chord_holds_menu`).

## How a picture is taken

| Where | How |
|---|---|
| Any game under the bridge | `game_menu.py` writes a `shot-…` token to `emulator.control.capture`. The overlay saves each panel as it is presented: SDL renderer, DSCore's direct canvas, or the GL swap for GPU engines (`capture_gl`). Captures are taken before the HUD, so menus, toasts and the flash never appear in them. |
| Game menu tile | Saves the frame the menu captured when it opened, which is the game behind the menu. |
| Launcher | The same path as `session/capture-request`, drawn into `session/launcher-shot-{top,bottom}.bmp`. |

A panel that never presents within 0.5 s is left out, for example a one-window engine. In that case only the panel that did present is saved.

## Feedback

- **Flash:** a white flash fades over 240 ms on both panels. In a game, the overlay draws it (`flash_level`) on the first panel saved. In the launcher, `shutter_overlay` draws it.
- **Buzz:** the `Save` haptic pattern, at the player's menu haptics strength.
- **Click:** `assets/sounds/shutter.wav`, made by `tools/build_shutter_sound.py`. In a game it is played by `sleep_sounds.play(data, 'shutter')`, which respects the effects volume and mute. The Game menu tile and the launcher play the menu's own Save cue instead.

## The album on disk (`runtime/screenshots.py`)

```
<player root>/screenshots/
  settings.ini              screens=both|top|bottom (Album > Y > Screenshot)
  album.tsv                 path, game key, title, system, YYYYMMDD; newest first; the launcher reads this
  <game key>/album.ini      title, system, rom
  <game key>/<YYYYMMDD-HHMMSS>[-n].png         both screens stacked (upper on top), or one screen
  <game key>/<YYYYMMDD-HHMMSS>[-n].thumb.bmp   160 px wide, for the list
```

- The game key is `library.game_key(rom)`. Pictures taken in the menus use the key `launcher`.
- Saving needs only the standard library: a BMP reader, PNG output through zlib, and nearest-neighbour scaling.
- The launcher runs the script for every change: `save`, `index`, `delete`, `view`, `cover` and `screens`. One copy of the code owns the format.

## Album page (Home > Album; Settings > Library until 2026-11-24)

- Pictures are grouped by game, newest game first. A gap separates the days within a game. The upper screen previews the selected picture.
- **A** opens the picture full size, with its upper and lower screens on their own panels. L/R or the D-pad moves between pictures, and B goes back.
- **X** deletes the picture, after you confirm.
- **Y** opens a menu: Photo mode, View, Use as the game's cover, Send in PictoChat, Send to phone, Delete, and Screenshot (which screens to save).
- **Use as the game's cover** writes the picture's upper screen to `<data>/library/<key>/box.bmp`. The first cover it replaces is kept as `box.before-screenshot.bmp`.

## Photo mode (Album: A on a picture you are viewing, or Y > Photo mode)

Photo mode is inspired by the camera apps of the DS era. Everything in it is drawn by our own code: `src/launcher/photo_mode.hpp` holds the pictures and `photo_screen.hpp` the page.

**The two screens**
- The **lower screen** is the desk. The picture sits at 0.7 scale, with a tool rail on the right and the strip for the current tool below it.
- The **upper screen** shows the finished photo exactly as it will be saved.

**Stitched photos (both screens)**
- A screenshot of both screens is edited as two pages, and saved back as one stitched picture (`save --top --bottom --screens both`).
- The desk shows one screen at a time. Switch with **Select**, the **Upper/Lower** switch in the footer, or Up/Down while a frame, lens or the date is in hand.
- The upper screen shows the whole stitched photo at half size. The screen on the desk is lit and the other is dimmed.
- Lenses and frames dress both screens. The date goes in the lower screen's corner, at the end of the picture. Pen strokes and stamps stay on the screen you draw them on.
- The footer of a stitched photo holds Back, Undo, the switch and Save. The key hints move to the upper screen.

**Motion** (all of it is off with Settings > Motion > Animations off)
- The desk springs up as the page opens, the tools slide in one after another, and the strip and footer rise.
- The highlight glides along the rail, and a tool's options slide in from the side it came from.
- A lens, frame, the date, Undo or Clear all cross-fades from the old picture.
- A stamp lands big and settles, with a burst of sparkles in its own colour. The rainbow pen sheds glitter and the eraser sheds dust. While the D-pad pen is down, the cursor breathes.
- The desk slides between the two screens.
- **Save** prints the photo: a white-bordered print shrinks, tilts and drops off the desk under the shutter flash, and then the Album opens on it.

**What you can add**

| Tool | What it does |
|---|---|
| Pen | 8 inks, a rainbow ink (its colour turns along the stroke) and an eraser, in 3 sizes. **Clear** at the end of the strip wipes every stroke and stamp on that screen, and Undo brings them back. Draw by touch, or move the cursor with the D-pad: A puts the pen down and lifts it again. |
| Stamps | 12 original 12x12 pixel stamps in 3 sizes. Tap to place, or press A at the cursor. |
| Frames | Our own: Instant photo, Film strip, Postage stamp, Rounded card, Hearts and Stars. Then every console frame the player draws, via `play::draw_frame`. The PSP's is left out, because it frames a wide picture and a 4:3 photo gets only a sliver of colour. |
| Lenses | Normal, Sepia, Black & white, Pop, Mosaic, Mirror, Kaleidoscope, Fisheye and Old photo. |
| Date | An orange seven-segment date in the corner, like a film camera's. It uses the shot's own day, in the order of System > Date format. |

**How it is drawn**
- Everything is composed on the CPU in order: lens, frame, ink layer, date. The lens and frame are composed again only when they change, so a stroke redraws just the ink and the date.
- Undo keeps the last 8 ink states.

**Keys** (every tool can be used without touching the screen)
- **L/R:** change tool.
- **On the strip** (where the page opens): Left/Right move a highlight along the inks, the sizes and Clear, or the stamps and sizes. The highlight picks what it lands on, except Clear, which waits for A. On frames, lenses and the date, Left/Right step them. Up or **A** goes onto the picture.
- **On the picture:** an accent edge goes round the desk. The D-pad moves the cursor, and holding it keeps it moving. **A** puts the pen down and lifts it again, drawing as the cursor moves, or places a stamp. **B** goes back to the strip.
- **Y:** next ink, stamp, frame or lens. **X:** undo. **Select:** the other screen of a stitched photo (Down on the strip does the same).
- **Start**, or the Save button: save.
- **B on the strip:** leave. If you made changes, it asks first: Keep editing, Leave without saving, or Save.
- The hints in the footer (or on the upper screen for a stitched photo) say what A and B do at that moment. A touch hides the highlight until the next button press.

**Saving**
- A save is a new picture in the same game's album, written with `screenshots.py save`: one screen for a one-screen picture, both screens stitched for a stitched one. The screenshot it came from is kept.
- The shutter flash and the Save cue play.

**Building it**
- The launcher compiles the player's `ui/canvas.cpp` and `ui/frame.cpp` in through `photo_play_sources.hpp`.
- `tools/build.py` adds `-Iplay/src` and puts `play/src/ui` in the launcher's source snapshot.
- Picture frames (the owner's PNGs) are not offered, because the launcher has no PNG decoder.

**Tests:** `tests/test_photo_mode.py`.

## Send to phone (Album > Y > Send to phone)

`runtime/album_share.py` serves a page while the sheet is open. The page is `runtime/album_files/album.html`.

**On the handheld**
- The upper screen shows the QR code, the address and a 4-digit code, the same way as the port files page.
- The QR link carries the code, so a phone that scans it is paired straight away.
- A computer opens the address and types the code once.

**On the phone or computer**
- The pictures are listed by game, newest first.
- Each picture can be viewed full size or saved. A saved file is named after the game and the time it was taken.
- "Download all" sends one zip, with a folder per game. The zip is streamed as it is built.

**What the page can do**
- It only reads, and only album PNGs. Any other path gets a 404.
- Pairing, the status file and the idle stop come from `port_files.py`. The launcher reads `session/album-share.ini` and `session/album-share.qr`.

**When it stops:** on Done or B, when you leave the Album, when the launcher goes away, or after 30 minutes idle.

**Tests:** `tests/test_album_share.py`.

## Send in PictoChat (Album > Y > Send in PictoChat)

**What you see**
- The picture's upper screen becomes a pad picture with `pictochat::make_picture`.
- It is shrunk to fit 128x96 (area average), then error-diffused onto the 8 inks plus the paper.
- It waits as `Chat::pending_picture` and goes on the pad when you join a room. Text starts to its right, and you can write or draw beside it before sending.

**The message format**
- Pictures are `Kind::Picture` (4): the position, the width and height, then two pixels a byte.
- A message may hold at most one picture, about 6 KiB of the 8 KiB budget.
- A message with a picture is format version 2. Ordinary messages stay version 1, which older handhelds still read.
- The decoder accepts a picture only in version 2 and only once. It refuses ink indices above 8 and a spare nibble that isn't zero.

**Tests:** case 6 in `tests/pictochat_core_test.cpp`.

## Not built yet

- Device checks. None of this has run on the handheld yet. Engines that do not present through SDL_GL_SwapWindow, such as Tanuki's Vulkan present or PPSSPP's shared texture, may need their own read-back.
