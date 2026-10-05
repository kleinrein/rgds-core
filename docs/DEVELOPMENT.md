# Save-to-refresh development

Start the local loop from this repository:

```sh
make dev
```

Keep it running while you edit. The native SDL preview uses the same renderer as the device, with the top and bottom panels stacked in one window.

| Change | What happens |
| --- | --- |
| Save `themes/default/light.ini` or `dark.ini` | The matching active palette reloads in the existing preview after roughly 300 ms of stable file contents. Cartridge selection and emulator choice stay in place. |
| Save a `.cpp` / `.hpp` file under `src/` | The launcher rebuilds, then the preview restarts and restores your selection and core. Build duration is printed. |
| Introduce a compiler error | The current preview keeps running. Fix it and save again to retry. |
| Enter an invalid theme colour | The current theme stays visible; the error appears in the terminal. The next valid save reloads. |
| Press Escape/B in the preview | Settings goes back one level. From the library, the preview closes; a source save opens it again. Ctrl+C ends the watcher. |

The current theme format controls nine palette colours plus a `mode=light|dark` hint. Geometry, controls, text and interaction changes live in `src/launcher/main.cpp`; those use the rebuild loop. Artwork and richer theme layouts are later UI work. This is automatic refresh for a native application: theme changes stay in the same process, while compiled code needs a process restart.

Pick the dark palette or scan real game filenames:

```sh
make dev THEME=themes/default/dark.ini
python3 tools/dev.py --rom-dir /path/to/your/NDS --theme themes/default/light.ini
```

Preview never starts an emulator. Its remembered selection/core is stored under the ignored `build/dev/` directory. To edit the live launcher sources in Codex, start with `src/launcher/main.cpp` and the selected `.ini` theme file.

Open Settings with S or the gear, or launch it directly with `make settings`. Theme & display offers Light/Dark and DS rounded/Pixel classic. Touch the choice directly, or use up/down to focus a row and left/right to choose. A toggles the focused choice; B goes back. Choices apply immediately and are saved in `build/dev/preferences.ini`, or `rgds/userdata/preferences.ini` on the device. A saved colour-mode choice takes precedence over the initial `THEME` file selection; choose the matching mode in Settings to see the palette you are editing.

Power and screen brightness now have working controls with desktop simulation and device capability checks. Library offers the default emulator and ScreenScraper artwork/account controls. Sound, Connectivity, System, favourites/recent games and home layout/wallpaper remain labelled Later.

DS rounded uses the bundled, open-licensed Nunito font at weight 600. Native rendering reads prebuilt alpha atlases, so the card requires no additional font library. Latin/Latin-1 text is covered. To regenerate the atlases after a deliberate font change, run `tools/bake_font.py` with a Python environment containing Pillow. The font source, exact upstream commit, license and generated atlases live under `assets/fonts/`. Ordinary builds do not need Pillow or font downloads.

## The device loop

First prepare official Linux, run Inventory and Probe as described in [DEVICE_SETUP.md](DEVICE_SETUP.md), and confirm SSH works. The host address below is an example. Use the address shown by your RG DS.

The unattended watcher requires SSH key authentication and an already verified host key. Connect manually first, then check that this exits without prompting:

```sh
ssh root@192.168.1.42
ssh -o BatchMode=yes root@192.168.1.42 true
```

Exit the first SSH session before running the second command. If the second needs a password, install your existing public SSH key on the stock development card before starting the watcher. Do not disable host-key verification. If keys are not ready, `make deploy` and `make run` still support an interactive SSH connection.

Start save-to-device refresh:

```sh
make dev-device DEVICE=root@192.168.1.42
```

The first successful build uploads the private package and starts a temporary development session. Later source saves build and upload a new release; runtime and packaging edits also trigger a device build. Theme-only saves transfer the small selected `.ini` file into `rgds/userdata/dev/theme.ini`, which the running menu reloads. Hashed release contents stay intact. Colours can therefore be tuned without packaging all the cores again.

While a game is running, changes stay queued. Exit through the emulator's own menu; the watcher applies the newest queued build after the game returns. The installer takes the activity lock and checks again before stopping our menu, covering a game launch that races the watcher. Saves, ROMs and emulator configuration are retained. The watcher only recognizes games launched through our session; use the stock development card and our launcher for this workflow.

Connection failures retain the candidate and retry after five seconds while the watcher continues watching local edits. A failed device launch stops the watcher and prints logs, keeping recovery bounded. A `userdata/boot_stock` recovery marker prevents the watcher from starting another session. Ctrl+C stops watching; it leaves the current device session available. B returns from our menu to stock.

Python cache writes are disabled during staging, verification and runtime startup. Release directories must remain identical to their manifests; generated `__pycache__` / `.pyc` files are not exempted from verification. An older watcher that reports `Unlisted files in release` during first deployment must be stopped and restarted after updating the tools. The repaired installer recreates its staging directory before checking it.

Activity records now use flushed temporary files and atomic replacement. Readers retry short-lived incomplete writes; a persistently unreadable record keeps updates queued instead of treating the device as idle. If a previous interrupted session leaves empty/stale records, exit to stock and run `make doctor DEVICE=<your SSH address>`. Doctor refuses to repair while a session/recovery lock, launcher, emulator or recorded live process exists. Eligible records are renamed to diagnostic backups, not deleted.

Useful commands in a second terminal:

```sh
make logs DEVICE=root@192.168.1.42
make status DEVICE=root@192.168.1.42
make rollback DEVICE=root@192.168.1.42
make run-dev DEVICE=root@192.168.1.42
```

Stop `make dev-device` before manual rollback, otherwise a pending development build may be applied again. Rollback switches the release; `make run-dev` starts it with the existing live theme. `make run` uses the theme inside the release instead. Delete `rgds/userdata/dev/theme.ini` while our session is stopped to reset the live override. Use `SD_ROOT=/actual/mount` if the inventory shows a different mount than `/mnt/mmc`.

Restart the watcher after modifying `tools/dev.py` or `tools/device.py`; those Python modules are loaded by the watcher itself. The watcher watches renderer/runtime/theme files and build inputs, rather than arbitrary files in the repository. New emulator payloads in `simpleos-source/` require a manual package build and provenance review.

Code deployments transfer the complete private package, including all three emulator adapters and the bundled font. Theme refresh is small and fast. A live override's `mode` hint identifies the palette it edits; switch to that colour mode in Settings to see it. Measure code-to-screen latency on your Wi-Fi connection before setting performance targets. Hardware input, display orientation, sleep/wake, emulator return, and actual rendering smoothness still need testing on the stock card.

## Refining the interface

Use the preview for palette, spacing, hierarchy and navigation ideas, then check the same change on the actual panels. The current icon strip animates with a 190 ms eased slide, a small selection lift, and 150 ms settings transitions. It redraws during transitions and then returns to event/clock-driven idle rendering. Dragging tracks the pointer until release, and swipes never launch a game. The full-width map shows the visible library range and supports seeking. Emulator choice is in Library settings; the top screen reads cached artwork and playtime. Favourites and per-game overrides remain later work.

For an explicit one-shot hardware capture, create `rgds/userdata/session/capture-request` while our launcher runs. The next frame writes `ui-snapshot-top.bmp` and `ui-snapshot-bottom.bmp` in the same directory and removes the request. This is a development-only filesystem control; the launcher does not continuously capture screens. Password entry remains masked. Profile animation frame time on the device before adding further effects.
