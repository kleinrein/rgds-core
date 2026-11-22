# Apps

An app is a program anyone can package for RG DS Core without a change on our side: a game port, a tool, a
homebrew engine. It is one folder with an `app.ini` beside the program. The launcher lists apps on the **Ports**
shelf next to PortMaster's ports. It starts them the same way (`runtime/port_runner.py`), so every app gets the same
handling with no work from its author:

- the whole process tree in one cgroup: Home freezes all of it, and Quit ends every process it started;
- on RG DS OS, the top panel as its only display (a DRM lease, so plain SDL `kmsdrm` works);
- the bottom screen's info page, the Home menu, the game switcher, playtime and Recents;
- the pad as an SDL game controller (`SDL_GAMECONTROLLERCONFIG` is set for this handheld).

The code is in `runtime/apps.py` and the tests in `tests/test_apps.py`.

## The folder

```
/data/apps/hello/            the folder name is the app's id; a zip's needs letters, digits, . _ - (at most 64)
    app.ini                  required
    hello.sh                 the program: a .sh runs with bash, anything else must be executable
    icon.png                 optional cover: an RGBA PNG is cropped to what it draws
    lib/                     optional: put on LD_LIBRARY_PATH first
    bin/                     optional: put on PATH first
```

```ini
schema=1
name=Hello
exec=hello.sh
```

| Key | Needed | Meaning |
|---|---|---|
| `schema` | no | `1`, the only format so far. Another number is refused. |
| `name` | no | The title in the library. Defaults to the folder name. |
| `exec` | yes | The program, relative to the folder. Paths that climb out (`..`, `/`) are refused. |
| `args` | no | Extra arguments, split like a shell does (`--fullscreen "two words"`). |
| `icon`, `cover` | no | The cover picture. Without them: `cover.png`, `cover.jpg`, `icon.png`, `screenshot.png`, `screenshot.jpg`. |
| `author`, `version`, `description`, `url` | no | Shown to people, not used to run the app. |
| `arch` | no | `aarch64` (default) or `armhf`. armhf uses PortMaster's 32-bit layer (below). |
| `gl` | no | `gles` (default) or `gl4es` for desktop GL through PortMaster's gl4es (below). |
| `min_core` | no | The oldest RG DS Core version it works with, such as `0.9.1`. |

`check` reports any other key so a typo does not go unnoticed, but the app still starts: a key added by a later
RG DS Core never stops an app from running on an older one. A UTF-8 BOM and spaces around `=` are ignored.

## What the app gets

| Variable | Value |
|---|---|
| `RGDS_APP_DIR` | the app's folder (also the working directory) |
| `RGDS_APP_DATA`, `HOME` | `/data/rgds/userdata/apps/<id>`, kept when the app is updated or reinstalled |
| `XDG_CONFIG_HOME`, `XDG_DATA_HOME`, `XDG_CACHE_HOME` | `config`, `share` and `cache` inside it |
| `RGDS_APP_ID` | the folder name |
| `RGDS_PANEL_WIDTH`, `RGDS_PANEL_HEIGHT` | the top panel: 640x480 on the RG DS, 1024x768 on the Plus |
| `RGDS_CORE_VERSION` | RG DS Core's version |
| `SDL_VIDEODRIVER` | `kmsdrm` on RG DS OS |
| `SDL_AUDIODRIVER` | `alsa` |

Write everything into `$RGDS_APP_DATA`, never into the app's own folder: an update replaces that folder.
`/data` is exFAT, so Unix permissions and symlinks are not available.

`arch=armhf` and `gl=gl4es` use the libraries PortMaster's compat layer installs (`runtime/portmaster_compat.py`).
If they are not there yet, the app does not start. It asks the owner to open PortMaster in Ports once.

## Installing

- Copy the folder into `/data/apps` over file sharing (the **APPS** share), USB or the card. Only folders with an
  `app.ini` are listed; loose files there (a stray `.sh` too) are not.
- Or copy a zip into `/data/apps`. At the next start the zip is checked and unpacked, then moved into
  `/data/apps/.installed/`. A zip that fails stays where it is, with `NAME.zip.txt` beside it saying why. The zip
  holds either one folder with `app.ini` in it, or `app.ini` at its top (then the zip's name becomes the folder name).
  Absolute paths, `..` and symbolic links are refused. An older copy is replaced whole and its data folder is
  kept. If the power goes in the middle of that, the next start puts the older copy back.

## For app authors

```bash
python3 runtime/apps.py check path/to/hello
```

`check` prints everything worth fixing and exits 1 when anything is listed. Only some of it stops the app from
starting: no `exec`, a program outside the folder or not executable, an unclosed quote in `args`, an unknown `arch`,
`gl` or `schema`, or a newer `min_core`. The rest (unknown keys, a missing picture, a folder name a zip could not
carry) is advice. `install ZIP`, `list` and
`artwork --data USERDATA` (unpack waiting zips, make missing covers) do what the boot does.

## Not done yet

- Not tried on the handheld. The Mac tests cover the manifest, installs, covers, the launch plan, and the Ports
  shelf listing an app. A real app has not been started on the device yet.
- A newly unpacked zip shows up on the shelf only after the next library scan, because installs run beside the boot.
- There is no Remove entry for apps. Remove the folder over file sharing.
