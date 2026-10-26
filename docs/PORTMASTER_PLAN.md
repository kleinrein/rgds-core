# PortMaster on RG DS Core: the plan

Status: plan, 2026-10-25. Nothing here is built yet. Facts about PortMaster were
read from PortMaster-GUI `2026.10.24-1611` (the latest release when this was
written) and the live `ports.json`. Facts about the handheld come from the
read-only probe (`tools/portmaster_probe.sh`) run on the owner's RG DS on
2026-10-25 (stock Linux 6.1.141, Buildroot 2024.02, release
`0.7.0-006555981d79-playacc5b9b7` at home), and from the Phase 0 spike the same
day, which ran two ports from SSH (results in section 14).

## 1. What "full support" means here

PortMaster is the de facto port manager for Linux handhelds: one GUI and
installer (pugwash + harbourmaster), one shared shell layer every port sources
(`control.txt`, `device_info.txt`, `funcs.txt`, `mod_<CFW>.txt`), shared
runtimes (Godot/FRT, GameMaker, Mono, .NET, Java, Love, Ren'Py, Weston...) and
about 1,470 ports. It ships a new release almost daily and updates itself.

Full support on RG DS Core means:

1. **The real, latest PortMaster**, downloaded from upstream and left to update
   itself. We do not fork it and we do not freeze a copy in our package.
2. **Ports are games.** They sit in our library with covers, descriptions,
   Favourites, Recents, Last played, playtime and the game switcher, like DS
   and GBA games: every system behaves the same.
3. **The shared in-game menu works in ports**: Home opens it, the port freezes,
   Resume/Sound/Brightness/Controls/Return home all work, lid sleep works.
4. **The second screen is used**: controls guide, touchpad, keyboard, volume.
5. **Getting ports feels native**: browse, install, update and remove from our
   UI, with a clear path for ports that need the owner's own game files.
6. **It is optional**: off by default, one switch in Settings → Library, next
   to "Show all systems".
7. **It never disturbs anything else**: DS saves, userdata, the stock card and
   other releases stay untouched, and every port's mounts and processes are
   gone when it exits.

Official support (PortMaster listing RG DS Core as a platform) is a separate,
later step ([section 13](#13-the-road-to-official-support)). Everything before
it works with unmodified upstream releases.

## 2. PortMaster today, in numbers

| | |
| --- | --- |
| Latest GUI release | `2026.10.24-1611` (GitHub PortsMaster/PortMaster-GUI, MIT) |
| Release rhythm | several releases a week, sometimes several a day |
| Ports in `ports.json` | 1,468 (709 ready to run, the rest need the owner's game files) |
| aarch64, no runtime | 919 (495 ready to run) |
| aarch64 + squashfs runtime | 265 (101 ready to run): FRT/Godot 3 (147), GameMaker `gmtoolkit` (51), .NET 8 (46), Godot 4 (44), Mono (30), Pyxel, Java, Ren'Py, rlvm, Solarus |
| Needs the `weston` runtime | 80 (X11/Wine/Godot 4 GL ports that bring their own Weston) |
| armhf or x86 only | 204 (out of scope at first, see [section 11](#11-compatibility-tiers)) |
| Requirement flags in use | `power` 57, `!lowres` 55, `analog_2` 38, `2gb`, `ultra`, `opengl` 2 |

How a port runs, from a real script (Brotato):

```bash
controlfolder=...            # /opt/system/Tools/PortMaster, /opt/tools/PortMaster,
                             # $XDG_DATA_HOME/PortMaster, else /roms/ports/PortMaster
source $controlfolder/control.txt                 # ESUDO, directory, DEVICE_*, GPTOKEYB...
[ -f "$controlfolder/mod_${CFW_NAME}.txt" ] && source "$controlfolder/mod_${CFW_NAME}.txt"
get_controls                                      # SDL controller mapping -> $sdl_controllerconfig
GAMEDIR=/$directory/ports/brotato/                # "roms" unless control.txt says otherwise
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
$ESUDO mount "$controlfolder/libs/frt_3.7.squashfs" "$HOME/godot"
$GPTOKEYB "frt_3.7" -c "./brotato.gptk" &         # pad -> uinput keyboard/mouse
pm_platform_helper "frt_3.7"
"frt_3.7" $GODOT_OPTS --main-pack gamedata/brotato.pck
$ESUDO umount "$HOME/godot"
pm_finish
```

Three things follow from that script and shape the whole design:

- **`control.txt` is the contract.** Every CFW that is not ArkOS-like ships its
  own `control.txt` (spruce, TrimUI, Miyoo, Batocera, knulli) that sets
  `directory`, `controlfolder`, `ESUDO` and `get_controls`, and its platform
  hook copies it into place after each PortMaster update.
- **`$XDG_DATA_HOME/PortMaster` is the clean way in.** We set `XDG_DATA_HOME`
  for the launch and the script finds our PortMaster without touching `/opt`
  or creating `/roms`.
- **Ports assume root, bash, loop-mounted squashfs, uinput and GLES.**
  PortMaster's maintainers publish the full list (section 3).

HarbourMaster (the Python backend) already has what we need to drive it:
`HM_TOOLS_DIR`, `HM_PORTS_DIR`, `HM_SCRIPTS_DIR` overrides, a CLI (`ports.json`,
`install`, `uninstall`, `runtime_check`, `auto_update`, `device_info`), a
`Callback` class with `progress`/`message` hooks, and a default platform
(`PlatformBase`) for a CFW it does not know.

## 3. Where the RG DS stands

PortMaster's own CFW requirements (bmdhacks' guide, linked from their PR
reviews) against the stock RG DS card, as probed on 2026-10-25.

| Requirement | Stock RG DS (probed) | Our answer |
| --- | --- | --- |
| GNU bash 4+ at `/bin/bash` | Yes: bash 5.2.15, and `/bin/sh` is bash (`/bin` → `usr/bin`) | Nothing to do |
| GNU coreutils | `cp mv ln chmod tee md5sum sha256sum dd timeout setsid` are real; `pkill pidof ps mount umount` are real (procps, util-linux) | Nothing to do |
| GNU sed `-i`, grep `-P`, awk, find, xargs, tar, unzip, gzip, killall | **BusyBox** applets; `grep -P` is refused, `sed` is not GNU, no `gawk` | Compat layer: GNU sed, grep (PCRE2), gawk, findutils, tar, unzip, gzip, psmisc, first in `PATH` for ports only |
| xz, 7z, zstd, xdelta3, innoextract | `xz`/`unxz` real; no 7z, zstd, innoextract | PortMaster ships 7zzs, xdelta3 and innoextract; zstd in the compat layer |
| rsync, dialog, curl | Missing (`wget` is BusyBox) | rsync and dialog in the compat layer; harbourmaster downloads with Python `requests`, so no curl |
| glibc ≥ 2.34 | 2.41 | Nothing to do: newer than every other CFW |
| SDL2 family | SDL2 **2.32.10** (Wayland + KMSDRM), SDL2_image, SDL2_ttf, SDL2_mixer, SDL2_net, SDL2_gfx | Nothing to do |
| Other common libraries | libpng16, libjpeg, freetype, fontconfig, OpenAL, vorbis, ogg, FLAC, mpg123, evdev, udev, xkbcommon, curl, ssl 3.2, lzma, bz2, ICU, ffmpeg; **no** libtheoradec, libX11, libGL, Vulkan loader | PortMaster's `libs.aarch64` carries libtheoradec; desktop-GL and X11 ports need gl4es or the `weston` runtime |
| GLES 2+ | Mali-G52, `libmali-bifrost-g52-g24p0-wayland-gbm`, GLES/EGL/GBM | Nothing to do; no `opengl` capability, which only 2 ports require |
| squashfs + loop | `CONFIG_SQUASHFS` with zlib, lzo, xz (no zstd, no lz4); 8 loop devices plus `/dev/loop-control` | Nothing to do: all 48 aarch64 runtimes in `ports.json` are gzip squashfs; more loop devices are created on demand |
| `/dev/uinput` | Yes, `crw-------` root | Ports run as root, so nothing to do |
| tmpfs `/tmp`, `/dev/shm` | Both tmpfs, 1.5 GB (`/dev/shm` is `noexec`) | Nothing to do |
| zram / swap | `CONFIG_ZRAM` (lzo-rle) and `/dev/zram0` present; no swap active; `mkswap`/`swapon` real, no `zramctl` | Optional zram while a heavy port runs, set up through sysfs |
| cgroups, namespaces | `cgroup2` and `CONFIG_CGROUP_FREEZER` in the kernel but **nothing mounted** on `/sys/fs/cgroup`; `CONFIG_NAMESPACES`, `PID_NS`, `USER_NS`; no `unshare` binary | The runner mounts cgroup2 (`/sys/fs/cgroup` if empty) and enters namespaces through ctypes |
| FUSE | `CONFIG_FUSE_FS`, `/dev/fuse`; no `fusermount` | Not needed |
| ALSA | One card (rk817), stock `asound.conf` asym + our dmix profile | Ports inherit `ALSA_CONFIG_PATH`, so they mix with menu sounds |
| PulseAudio | `pulseaudio` and `pactl` installed, daemon **not running**; no PipeWire | `SDL_AUDIODRIVER=alsa`; OpenAL falls back to ALSA; starting PulseAudio is not worth it |
| sudo/doas, systemctl | Neither; we run as root | `ESUDO=""` (PortMaster's own no-sudo path); `pm_finish` calls `systemctl … &` with errors hidden, so no shim is needed |
| Python 3 | 3.11.8 with lzma, bz2, zlib, ssl (OpenSSL 3.2.1), ctypes, sqlite3; no `os.unshare` | Nothing to do |
| Card filesystem | `/mnt/mmc` is **FAT32 (vfat)**, `fmask=0000` (everything executable), case-insensitive, 40 GB free | No symlinks and a 4 GB file limit on the card (see section 4) |
| Other partitions | `/` ext4, 3.9 GB free; `/mnt/ports` ext4, 1.9 GB (stock's PortMaster); `/userdata`, `/oem`, `/mnt/vendor` ext4 | Ours stays on the card; stock partitions are left alone |
| armhf userland | Kernel has `CONFIG_COMPAT`, but no `ld-linux-armhf.so.3` and no 32-bit libraries | Out of scope at first |
| CPU, RAM | RK3568, 4× A55; 3.0 GB (PortMaster reports `DEVICE_RAM=3`) | Capabilities `power` and `2gb`, **not** `ultra` (needs 4 GB): Brotato and other `ultra` ports are hidden unless "Show all" |
| Display | `card0-DSI-1` and `card0-DSI-2` at 640×480, **plus** `card0-Writeback-1` at 1920×1080 | Export `DISPLAY_WIDTH/HEIGHT` from `device_profile` (Plus: 1024×768), so the writeback connector can never be taken for a screen |
| Compositor | Weston 14.0.2, DRM backend, **desktop-shell** (not kiosk), `weston-keyboard` input panel, no `app-ids` rules | Click-to-focus shell: the companion must never take input (section 8) |
| Pad in `gamecontrollerdb.txt` | Not in PortMaster's db. Stock's own PortMaster has a line for `1900b655…` with `back:b8,guide:b6`, which swaps Select and Home against our mapping (`back:b6,guide:b8`) | Our overlay writes our launcher's mapping; upstream it later |
| Identity | `/etc/os-release` NAME=Buildroot; device-tree model "Rockchip RK3568 DEEP LP3 V10 Board"; no `/opt/muos`, `/opt/system/Tools`, `/storage`; `/roms` and `/userdata` exist | PortMaster sees `CFW_NAME=Buildroot` and the default platform |

## 4. Architecture

```
 rgds-launcher (Library: Ports system, Get ports, Game info, per-port options)
        │ launch(core="portmaster", rom=<port .sh>)
        ▼
 runtime/emulators.py ──► runtime/portmaster.py
        │                  ├─ overlay: RGDS control.txt, mod_RGDS.txt, pad db line  (re-applied after updates)
        │                  ├─ compat: GNU sed/grep/gawk/findutils/tar/unzip/gzip/psmisc/zstd, PATH for ports only
        │                  └─ port_plan(): env, private mount ns, cgroup, top-panel app id
        ▼
 PortRunner (GameMenu for a process tree)          rgds-launcher --port-companion (bottom panel)
   cgroup freeze/thaw, cgroup.kill, lid sleep,      the in-game menu, controls guide, touchpad,
   volume/brightness keys, playtime, menu requests  keyboard, volume; reads touch with EVIOCGRAB
        ▼
 bash <port>.sh  ──► control.txt (ours) ──► upstream device_info/funcs ──► game + gptokeyb
        ▲
 PortMaster (upstream, untouched, self-updating):  <tools>/PortMaster, harbourmaster, pugwash
```

The rule that keeps "latest" possible: **upstream files are never edited in
place.** Our changes are separate files PortMaster already knows how to use
(`control.txt` variants, `mod_<CFW>.txt`), written by us after every PortMaster
install or update, exactly as the spruce and Miyoo platform hooks do upstream
(`PlatformSpruce.portmaster_install`).

### Folders

What the stock card already has, and why we keep clear of it:

- **Stock PortMaster 6.55** (the 2022 dialog-based one, before harbourmaster)
  on its own ext4 partition `/mnt/ports` (1.9 GB, 1.6 GB free), linked from
  `/roms/ports/PortMaster`. Its `control.txt` starts Anbernic's
  `/mnt/vendor/bin/portsCtrl.dge` and writes `/sys/class/anbernic_misc/runapp`
  and `nds_esckey`.
- **A stock Ports menu** (`dmenu.bin`/`muos2.bin`) that lists scripts in
  `/mnt/mmc/Ports` (empty on the owner's card).
- A port script that does not find our control folder falls back to
  `/roms/ports/PortMaster`, which is stock's old one. Our launch always sets
  `XDG_DATA_HOME`, which upstream scripts check first, so that never happens.

The card is FAT32 and case-insensitive, so `/mnt/mmc/ports` **is** the stock
`/mnt/mmc/Ports` folder. Scripts there would show in the stock menu and start
without our environment, against stock's old PortMaster. Our folders therefore
use a name the stock menu does not scan:

| Path | What | Who writes it |
| --- | --- | --- |
| `/mnt/mmc/portmaster/` | `XDG_DATA_HOME` for launches; holds `PortMaster/` (the control folder), `home/` (ports' `$HOME`) and `rgds/` (compat layer) | PortMaster and `runtime/portmaster.py` |
| `/mnt/mmc/portmaster/ports/` | Port scripts and game folders (`HM_PORTS_DIR` = `HM_SCRIPTS_DIR`), `directory=mnt/mmc/portmaster`, so `GAMEDIR=/mnt/mmc/portmaster/ports/<port>` | harbourmaster, the owner (game files) |
| `/mnt/mmc/portmaster/autoinstall/` | Drop zone: port zips or `PortMaster.zip` copied over SMB install on next open | the owner |
| `rgds/userdata/library/<key>/` | Our per-port options, stats, save backups, as for any game | the launcher |
| `rgds/userdata/portmaster.ini` | Enabled, installed version, last overlay hash, store preferences | `runtime/portmaster.py` |

All of it lives outside `rgds/`, so OTA, rollback and "Uninstall RG DS Core"
never touch it; uninstall asks whether to keep ports. We do not write to
`/mnt/ports`, `/roms` or the stock PortMaster.

FAT32 has two consequences. **No symlinks:** a port zip with symlinks fails to
unpack there, and ports that link config folders need PortMaster's
`bind_directories`/`bind_files`, which work because we are root. **4 GB per
file:** squashfs runtimes are at most 249 MB (Mono), but a port with a bigger
data file cannot live on this card. Both go into the compatibility notes the
store shows. If they bite often, a later option is a loop-mounted ext4 image
on the card (`portmaster.img`) mounted inside the port's namespace.

## 5. Install and update ("latest, of course")

1. **Settings → Library → PortMaster → Install** (or the first open of Ports).
   `runtime/portmaster.py install` asks the GitHub API for the latest release,
   downloads `PortMaster.zip` (about 23 MB), checks the release's MD5 file and
   GitHub's SHA-256 asset digest, and installs it with PortMaster's own
   installer (`harbourmaster --no-check install ./PortMaster.zip`), so the
   layout is exactly what upstream expects. Progress uses our glitter bar.
2. **Offline install**: `PortMaster.zip` dropped into `portmaster/autoinstall/`
   over SMB (Settings → Connectivity already shares the card) is picked up the
   same way.
3. **Updates belong to PortMaster.** pugwash checks hourly and replaces itself;
   `harbourmaster auto_update` does the same headless. We trigger a headless
   check when the Ports system opens with Wi-Fi up, at most once a day, and
   show "PortMaster updated to 2026.x" in the status pill.
4. **Overlay after every change.** Before any PortMaster or port launch,
   `portmaster.py ensure` compares `PortMaster/version` and the overlay hash
   with `portmaster.ini`. On a change it rewrites our files (below), then
   validates them (section 12). It never edits an upstream file.
5. **Drift guard.** A scheduled check (CI or `tools/portmaster_drift.py`)
   downloads each new PortMaster release, runs our overlay tests against it,
   and fails loudly when an upstream change (a new variable in
   `control.txt`, a moved file, a runtime packed with zstd, which the stock
   kernel cannot mount) needs our attention. This is how "always the
   latest" stays safe with a project that releases daily.

### The overlay files

All generated from templates in `runtime/portmaster/` in our release:

- `PortMaster/control.txt`: the RGDS variant, modelled on spruce's.
  `controlfolder`, `directory="mnt/mmc/portmaster"`, `ESUDO=""`,
  `CUR_TTY=/dev/null`, `get_controls()` that selects our pad's line (the pad
  is `platform-gpio-keys-polled-event-joystick`, event4), then sources upstream
  `device_info.txt` and `funcs.txt` unchanged and exports `GPTOKEYB`/`GPTOKEYB2`
  through our wrapper (controls overrides, section 7). The upstream generic
  `control.txt` is kept as `control.upstream.txt` for the drift test.
- `PortMaster/mod_Buildroot.txt`: `CFW_NAME` comes from the stock
  `/etc/os-release` (`NAME=Buildroot`) until upstream knows us. It sets
  `GODOT_OPTS`/`GODOT2_OPTS` for the top panel, the stock RetroArch location
  (`/oem/retro/retroarch` + cores) for RetroArch-based ports, and a
  `pm_platform_helper` that tells the runner which process is the game (for
  the menu and playtime), like muOS writes `foreground_process`.
- One `gamecontrollerdb.txt` line for `ANBERNIC-rk3568-keys`, appended if it
  is missing, following our launcher's face-button labels. PortMaster's own
  "swap A/B" (`gcd_mode`) stays available in pugwash.
- `PortMaster.sh` is left alone; we launch pugwash through
  `utils/pm_launch.txt` with our hook file (`pm_hook_*` overrides: no tty,
  frontend refresh = a marker our launcher reads).

## 6. Launching a port

New engine id `portmaster`, appended to `runtime/engines.py` `CORES` (index 6;
never offered as a DS engine, bounded like `rgds-play`). `play_options.engine_for`
learns `system == "ports"`. `emulators.launch_plan` gets a `portmaster` branch
that calls `portmaster.port_plan()`:

- **Command**: `bash <script>` with `cwd` at the script's folder, run under a
  small launcher that enters a **private mount namespace** (`unshare(CLONE_NEWNS)`
  through ctypes; Python 3.11 has no `os.unshare`) and a **cgroup v2 leaf**
  (`/sys/fs/cgroup/rgds/port`; the stock card mounts no cgroup filesystem, so
  the runner mounts cgroup2 on the empty `/sys/fs/cgroup` once per boot, which
  is what every systemd distribution does). Every squashfs
  mount a port makes disappears with the namespace even if the port is killed
  before its `umount` line; `cgroup.kill` ends every process it started,
  including the ones that `setsid` or daemonise.
- **Environment**: the session environment (Wayland socket, vendor SDL,
  ALSA profile, TZ) plus `XDG_DATA_HOME=/mnt/mmc/portmaster`,
  `HOME=/mnt/mmc/portmaster/home`, `SDL_VIDEODRIVER=wayland`,
  `SDL_AUDIODRIVER=alsa`, compat `bin/` first in `PATH` and compat `lib/` last
  in `LD_LIBRARY_PATH` (ports put their own libraries first, as the guide
  requires; no global `LD_PRELOAD`).
- **Top panel**: with the session environment, both spike ports (a native
  SDL2 game and Godot FRT 3.2.3) opened fullscreen on the top panel (CRTC
  130) by themselves; SDL display 0 is the top. We still set
  `SDL_VIDEO_WAYLAND_WMCLASS=org.rgds.top` (the stock SDL 2.32.10 knows it) so
  the window carries our role. Not yet seen: a port that bundles its own SDL.
- **Focus**: desktop-shell gives focus to the newest window and does not give
  it back. The port gets focus because it maps last; nothing of ours maps a
  window after it (sections 7 and 8). `SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1`
  (present in the stock SDL) keeps a pad-reading port working whatever
  happens to focus.
- **Display size**: `DISPLAY_WIDTH/HEIGHT` from `device_profile` (640×480,
  Plus 1024×768) instead of `device_info.txt`'s DRM scan, which sees DSI-1,
  DSI-2 and a 1920×1080 writeback connector.
- **Performance**: the game clock floors (`game_clocks`) and an optional
  per-port Performance mode (governor, GPU floor, and zram on `/dev/zram0`
  for heavy runtimes such as Mono, .NET, Java and Godot 4 on the 3 GB RG DS).
- **Records**: `session/core.json` names the runner, so the guardian's
  crash recovery (`stop_owned_group`) also ends a port.

**Not the bridge.** Our `librgdsbridge.so` is preloaded into emulators to hook
SDL. Ports are arbitrary programs with their own `LD_PRELOAD`s (gptokeyb2's
`libinterpose`, hacksdl, gl4es), so preloading the bridge there would break
the override chain PortMaster depends on. Everything below works from outside
the port.

### Display modes

- **Wayland (default)**: the port is a Weston client. The menu, the companion,
  volume and lid sleep all work.
- **Direct KMS (per-port option, later)**: for ports whose bundled SDL has no
  Wayland and for `weston`-runtime ports that start their own compositor. Weston
  is stopped as for DSCore's direct present (`direct_display.py`). A frozen
  process keeps DRM master, so no menu window can open: the menu is replaced by
  "Return home" (Home held) and lid sleep. The option says so.

## 7. The in-game menu in a port

The runner is `GameMenu` generalised to a process tree (`PortRunner`), so the
status files, playtime, lid sleep and switcher code are shared.

**The menu must not open new windows over a port.** The Phase 0 spike showed
that on the stock Weston (desktop-shell), mapping fullscreen windows while a
port runs takes its keyboard focus, and closing them does not give it back:
gptokeyb's keys stop reaching the game. A synthetic mouse click on the port
did not restore it either. Freezing and thawing alone keep focus. So for
ports, the menu is drawn **inside the companion's bottom-panel window**, which
is mapped before the port starts and stays for the whole session; the top
panel keeps showing the frozen game itself. In the spike that kept focus: the
companion redrew as the "menu", the port thawed, and Enter reached the game.
This means `rgds-launcher` gets a persistent bottom-only menu mode (the
QuickMenu page and its subpages on one panel), driven by the runner's status
files, reading the pad through evdev (the window never has focus).

| Action | DS / GBA today | Port |
| --- | --- | --- |
| Open | Home/Menu key via `MenuInput` (evdev, no bridge needed) | Same key, same reader |
| Freeze | `stop_outside_mixer(child)` SIGSTOP | `cgroup.freeze=1` for the whole tree (verified: `cgroup.events` reports frozen, the top panel holds still), with the same "not inside the dmix semaphore" retry for every member (`mixer_holder`), process-group SIGSTOP as fallback |
| Menu windows | A helper opens windows on both panels | The companion's existing bottom window switches to the menu; no new windows |
| Top panel during the menu | Captured backdrop | The frozen port itself. Scanout reads the top plane (linear XR24) when a picture is needed, for save backups |
| Resume | SIGCONT + `.resume` | `cgroup.freeze=0` |
| Return home | Bridge save-quit | Confirm ("Save in the game first?"), then SIGTERM to the tree, 3 s, `cgroup.kill`; gptokeyb gone, mounts gone with the namespace |
| Lid sleep | Freeze + resume | Same freeze |
| Volume keys | Bridge (`RGDS_VOLUME_KEYS`) | Runner reads the keys and calls `audio.py`; the HUD is drawn on the bottom panel by the companion |

The menu page keeps the same 12-tile geometry and style. For `--game-core
portmaster` the tiles are:

| Resume (wide) | | |
| --- | --- | --- |
| Back up saves | Restore saves | Performance |
| Port info | Sound | Brightness |
| Restart port | Return home | Controls |
| Game switcher (wide) | | Second screen (wide) |

- **Back up / Restore saves**: ports have no save states. Most keep their
  saves in `$GAMEDIR/conf`, `savedata`, `saves` or `$XDG_DATA_HOME` (Brotato
  sets `XDG_DATA_HOME=$GAMEDIR/conf`). We find those folders per port, copy
  them into the save gallery's storage (`save_library.py`) with a screenshot,
  and restore them only while the port is closed. A port with none found shows
  the tile muted with "This port keeps no save folder we can find."
- **Controls**: shows the port's `.gptk` mapping as a picture of the pad.
  Remapping writes `library/<key>/controls.gptk`; our `GPTOKEYB` wrapper swaps
  it in for the port's `-c` file at launch, so a port update never loses it.
- **Port info**: description, instructions, porter credits, runtime, licence
  notes from `port.json` / `gameinfo.xml` / `README.md`.
- **Game switcher**: lists ports with other games. Built 2026-10-26: the
  Anbernic button opens it beside a port as for every game, and picking a game
  closes the port (it saves itself, as on Return home) without asking first;
  arriving in a DS game works as today.
- **Hidden for ports**: Save/Load checkpoint, Shaders/Graphics, Emulator,
  Cheats, fast-forward. Shaders are a GL pass inside the bridge and cannot
  reach a port.

## 8. The second screen

Built 2026-10-26: while a port plays, the helper's bottom window is RG DS
Play's Info screen (`port_info` in `src/launcher/port_menu.hpp`; game switcher,
held Return home, brightness, screen off). PortMaster itself keeps its install
progress view. The modes below are the original plan.

`rgds-launcher --port-companion` opens one window on SDL display 1 (the bottom
panel; display 0 is the top, verified in the spike) before the port starts,
and keeps it until the port exits. It is also where the in-game menu is drawn
(section 7). Modes, chosen per port (Second screen tile) with a sensible
default:

1. **Guide** (default): the port's cover, its controls as a pad picture,
   battery, clock, and the volume/brightness HUD.
2. **Touchpad**: the touch panel drives a uinput mouse on the top panel, for
   point-and-click ports (ScummVM-style, AGS, many GameMaker ports). Tap =
   left click, two-finger tap = right click.
3. **Keyboard**: our on-screen keyboard (`keyboard_screen.hpp`) typing into
   the port through uinput, for name entry and text adventures.
4. **Off**: panel dark, for battery.

Input rule that keeps the port focused: Weston runs desktop-shell, which moves
keyboard focus to whatever is touched or newly mapped, and does not hand it
back when a window closes. So the companion is mapped before the port (the
port, mapped last, gets focus, as the spike showed), never maps another
window, reads the touch panel (`gt9xx-0`, event1) with `EVIOCGRAB` while a
port runs (Weston never sees the touch), and has an empty input region.
Touchpad and Keyboard modes write to uinput devices, as gptokeyb does.

### Touch screen mode (built 2026-10-26, host-verified)

A **Touch screen** tile beside Screen off on the info screen turns the bottom
panel into an absolute mouse over the top one (`src/platform/uinput_pointer.hpp`,
`port_touch_*` in `port_menu.hpp`). The panels are the same size and stacked,
so a touch at (x, y) below clicks at (x, y) above. Tap = left click, drag =
left-button drag, holding still for 0.6 s = right click. The pill in the
corner, held for 1 s, leaves the mode. The choice is kept per port
(`library/<key>/touch.ini`).

Under the pad is the top panel's own picture, dimmed and refreshed about 12
times a second. The companion reads it from the display controller
(`src/platform/scanout_mirror.hpp`, the same way as `tools/scanout.c`).

Probed on the handheld (2026-10-26) with a stand-alone build of the same code:

| Question | Result |
| --- | --- |
| How Weston sees the virtual mouse | udev tags it `Mouse`, libinput calls it "a pointer", and Weston attaches it to DSI-1, the top panel ("associating input device event6 with output DSI-1"). Weston logged `failed to set cursor: Invalid argument` (hardware cursor plane) |
| Cost of the mirror | The top panel's plane is linear XR24, 640×480. A copy takes 0.17 ms on average, 0.24 ms at worst (30 frames) |
| Real taps in a port (Cats on Mars, owner's finger) | The pointer follows the finger 1:1 on the top panel. libinput logged 11 `BTN_LEFT` press/release pairs for the taps, but Cats on Mars ignores the mouse (it is played with the pad), so nothing reacted in the game |
| Clicks reaching a window | `weston-eventdemo` on the top panel received button 272 (left) and 273 (right) at the clicked spot |
| Click length | Press and release first came in the same millisecond, which a game reading the button once a frame can miss; a click is now held for 50 ms |
| Not verified | A mouse port reacting | The Sphinx of Time (AGS) started its game when the virtual mouse clicked START on its title screen |
| Not verified | A finger tap in a mouse port (the owner's test); a port whose buffer Weston scans out directly with an AFBC modifier (then there is no mirror, only the grid), and the Plus's 1024×768 top panel |

### Tall mode (spike, not built)

Tall mode means using both panels as one 640×960 portrait screen. It does not
fit the stock compositor. Weston places DSI-1 at 0,0 and DSI-2 at 640,0 (the
touch panel's `LIBINPUT_CALIBRATION_MATRIX=0.5 0 0.5 0 1 0` assumes that), and a
fullscreen surface covers exactly one output. Changing the layout means
editing the stock `weston.ini` and the touch calibration, and restarting
Weston. The one route that leaves the system alone is a nested Weston from
PortMaster's Westonpack runtime: its Wayland backend with two 640×480 outputs
stacked vertically, one window on each panel, and the port in a 640×960
window inside it. That costs an extra composition pass for every frame on
both panels, and only ports that can render a portrait 640×960 picture would
use it (Godot through `GODOT_OPTS`, some LÖVE games). For now, Touch screen
mode with the mirror gives the DS feel at almost no cost. Tall mode waits for
real demand and a device spike with Westonpack.

## 9. In the library

### Decided: Ports is part of "Show all systems" (built 2026-10-25)

The owner's call: one switch decides between DS focus mode and multi-system
mode, so there is no separate PortMaster switch.

- **Settings → Library → "Show all systems"** off: DS only, nothing of
  PortMaster anywhere. On: the Systems carousel lists Nintendo DS, the RG DS
  Play systems that have games, and **Ports**.
- **Ports is always listed in multi-system mode**, because its first entry is
  **PortMaster** itself (`runtime/portmaster_files/entry/PortMaster.sh`, passed
  to the launcher as a `ports=` system folder by `runtime/session.py`).
  Starting it installs the latest PortMaster on first use (progress on the
  bottom screen) and opens PortMaster's own GUI to get and update ports.
- Installed ports (`/mnt/mmc/portmaster/ports/*.sh`, top level only) are the
  other entries, with the system's stats, Favourites, Recents and Last played
  like any game. Their covers come from each port's own picture
  (`portmaster.artwork`, written after every port or PortMaster session).
- **Not on Home**: ports are games and sit with the games.
- **PortMaster is a tool, not a game** (the owner's second call, same day): its
  entry is pinned first in Ports with PortMaster's own app icon (cropped from
  the installed `trimui/icon.png`), the top screen says "Get, update and remove
  ports" and its tile says OPEN. It is left out of the game count, Favourites
  and Recents, records no playtime and has no game options. While it runs the
  bottom screen names it with one hint, Home closes; Home opens a two-choice
  sheet (Back to PortMaster, Close PortMaster, focus on Close) instead of the
  12-tile game menu.

### How ports look

- `scan_ports()` beside `scan_system()`: each installed port from
  harbourmaster's records (`<port>/port.json` items name the `.sh`), with
  title, genres, "Ready to run", porter, runtime and `date_updated`.
- Covers from `cover.png`, `screenshot.jpg` as the top-screen picture,
  `gameinfo.xml` text for Game info; no scraper calls for ports.
- System badge "PORT" and a drawn pixel icon (`tools/system_pixel_art.py`).
- Per-port options (X): Favourite, Performance, Second screen, Display mode,
  Controls, Save backups, Port info, Update, Uninstall.
- **Update available** badge when the source's `date_updated` is newer than
  the installed one; "Update all" in Settings → Library → PortMaster.
- Library keys are `game_key(<script path>)`, so stats, favourites and backups
  follow the port as long as its script stays put.

## 10. Getting ports

**Stage A, PortMaster's own GUI.** "Get ports" opens pugwash on the top panel
(through `pm_launch.txt` with our hooks), with the companion in Guide mode
showing pugwash's buttons. It is the full upstream experience on day one:
sources, Multiverse, runtimes, themes, its own updates. When it exits, the
launcher rescans Ports.

**Stage B, our own store.** A native page in the launcher, backed by
`runtime/portmaster.py serve`, which imports harbourmaster as a library (it
targets Python 3.7, we run 3.11) and reports progress as JSON lines through a
`Callback` subclass. It is the same code pugwash uses, so installs, runtimes
and requirement checks stay upstream's.

- Browse by genre, Ready to run, recently added/updated, most downloaded
  (harbourmaster's sort functions), and search with our keyboard.
- Top screen: the port's screenshot; bottom: title, porter, size, runtime,
  "Ready to run" or "Needs your files", Install.
- Only ports that pass `match_requirements` for this device appear, like
  pugwash; "Show all" mirrors its setting.
- Install, update and remove with the glitter bar and resumable downloads
  (the OTA downloader's resume logic), runtime downloads included.
- **Needs your files** flow: after installing, a card shows the port's
  instructions and exactly where the files go:
  `\\<ip>\<share>\portmaster\ports\brotato\gamedata\`, with the share switched on from
  the card (`file_sharing.py`). When the files arrive the badge turns to
  "Ready".
- "Open PortMaster" stays as an advanced entry for everything Stage B does
  not cover.

### Add game files (built 2026-10-26, host-verified)

759 of the 1,468 ports need files the owner bought. Pressing A on such a port
(port.json `rtr: false`, no files added yet) opens **Add game files** instead
of starting it (`src/launcher/port_files_screen.hpp`):

- The top screen shows a QR code for a phone, plus `http://anbernic.local` and
  the IP address with a four-digit code for a computer's browser. Below them
  is the target folder, then live progress, then "Files arrived".
- The bottom list has Play (it also plays without files, and remembers that),
  How to get the files (the port's own instructions) and Back.
- `runtime/port_files.py` serves the page only while the launcher page is
  open. It stops when the page closes, when the launcher goes away, or after
  30 idle minutes. On the page you pair with the code (rate limited; the cookie
  is kept for the session) and get every installed port: instructions, store
  links, a hint for Steam/GOG/itch folders matched to the computer's OS, and a
  drop zone for whole folders or files. A dropped folder's contents go into
  the target. Uploads go in resumable 32 MiB pieces, so a dropped Wi-Fi
  connection loses at most one piece. Zips can be unpacked on the handheld,
  without their wrapper folder. Clutter (`.DS_Store`, `__MACOSX`, `._*`) is
  skipped, names FAT refuses are cleaned, files over 4 GB on FAT and anything
  that would leave the port's folder are refused, and free space is checked
  before the first byte.
- `runtime/qr.py` makes the QR code (byte mode, level M, versions 1–10, no
  library). It is checked against macOS's own decoder and the ISO
  error-correction example.
- `portmaster.notes()` writes `library/<key>/port.ini` (`needs_files`,
  `target`, `instructions`) at session start and after each PortMaster run.
  Files added are counted in `port-files.ini`.
- With network file sharing on and Show all systems on, sharing also offers a
  **PORTS** share, for people who prefer Explorer or Finder.
- Not verified on the handheld: the page on the real network, the bottom-screen
  render on the device, uploads to the FAT card.

### The compat layer (built 2026-10-26, device-verified)

Ports are built for Ubuntu-style handheld systems. On the stock card they miss
shared libraries, find BusyBox where they expect GNU tools, and armhf-only ports
find no 32-bit userland at all. `runtime/portmaster_compat.py` fills those gaps
without shipping anything in our package and without touching the system:

- It downloads Ubuntu 20.04 (focal) packages from `ports.ubuntu.com` (falling
  back to `old-releases`), each pinned by version and SHA-256 in
  `runtime/portmaster_files/compat.json` (64-bit) and `compat-armhf.json`
  (32-bit). Focal is what most ports are built against, and its packages are xz,
  which the card's Python unpacks. It copies only the listed files into
  `/mnt/mmc/portmaster/rgds/` (FAT has no symlinks, so a soname is a real file),
  building in `rgds.new/` and swapping, so a failed download leaves the previous
  layer working. The PortMaster entry and every port launch run it when the
  manifests change; the first install took about a minute.
- **64-bit libraries** (`rgds/lib`, last in `LD_LIBRARY_PATH`): only sonames
  the card lacks, so nothing the system provides is ever shadowed. glibc,
  libgcc and libstdc++ always come from the card (2.41 and GCC 13, newer than
  any port needs). Libraries focal does not have come from Ubuntu 18.04
  (bionic: `libreadline.so.7` for Wizznic and Yatka) or Debian 10 (buster, on
  archive.debian.org: ICU 63 for VCMI); those packages name their own mirrors.
- **Desktop GL** (`rgds/gl4es/`, on the path of the ports that need it only):
  PortMaster's own gl4es build as `libGL.so.1` (taken from a pinned port zip),
  focal's GLU, and two stubs we build (`libOpenGL.so.0`, `libGLX.so.0`, each
  naming `libGL.so.1`) for glvnd-linked ports, and a preloaded SDL shim
  (`librgds-gl-attrs.so`) that makes SDL's GL requests ones Mali's Wayland EGL
  can meet: a compatibility or core profile becomes ES (otherwise EGL refuses
  it, or libmali hands back a desktop context nothing draws into), the depth
  buffer is capped at 24 bits, and `SDL_GL_GetProcAddress` answers from gl4es
  (otherwise functions come straight from the driver, around gl4es: Bugdom drew
  only its clear colour). At launch a port gets this folder and the shim, with
  `LIBGL_ES=2` and `LIBGL_GL=21` unless it sets them, when its
  binaries name `libGL.so.1`, `libOpenGL.so.0` or `libGLX.so.0` and it brings
  no gl4es (Bugdom, Nanosaur, MiceAmaze, Half-Life), or when it runs on the
  Ren'Py runtime. Never on every port's path, and never for ports on
  PortMaster's Weston runtime: that runtime probes for a system `libGL`, and
  gl4es's start-up messages broke its parsing.
- **GNU tools** (`rgds/bin`, first in `PATH` for port scripts only): tar, sed,
  grep (with `-P`), gawk/awk, find, xargs, gzip/gunzip, unzip, killall, zstd,
  rsync, bc, xxd, cpio, zramctl. The card's own `zip` and `file` are real.
- **armhf** (`rgds/armhf/`): focal's 32-bit glibc, libstdc++, SDL2 family,
  Wayland, xkbcommon, ALSA and friends (126 libraries), Rockchip's 32-bit
  `libmali-bifrost-g52-g24p0-wayland-gbm.so` from the JeffyCN/mirrors libmali
  branch (pinned SHA-256; the same driver generation as the card's 64-bit one),
  and tiny stubs we build (`tools/build_compat_stubs.py`: `libEGL.so.1`,
  `libGLESv2.so.2`, `libgbm.so.1`... each naming only `libmali.so.1`, as the
  card's own 64-bit stubs do). The card has no binfmt_misc and no overlayfs,
  and `/lib/ld-linux-armhf.so.3` is not ours to add, so at launch an armhf-only
  port's programs get their `PT_INTERP` pointed at `/tmp/rgds-ld-armhf.so.3`, a
  copy of the loader on tmpfs (same length or shorter, port files only). Our
  `control.txt` then reports `DEVICE_HAS_ARMHF=Y` and the `armhf` capability,
  so PortMaster lists armhf ports.
- **PortMaster's Weston runtime** (Westonpack): `westonwrap.sh` looks for
  PortMaster in `$HOME/.local/share/PortMaster` and otherwise falls back to
  `/roms/ports/PortMaster`, which on the stock card is the 2022 PortMaster whose
  `control.txt` starts Anbernic's `portsCtrl.dge` (seen in a port log). Each
  port's private mount namespace now binds our PortMaster onto both paths
  (`port_runner.prepare_binds`). Westonpack has a mode for libmali with a host
  compositor (crusty draws into the running Weston); it turns that on for
  ROCKNIX only, so our `control.txt` tells the wrapper, and only the wrapper,
  that it runs on ROCKNIX, and points its libxkbcommon at the card's keymaps.
  crusty's window is maximized, not fullscreen, so the port helper no longer
  shows its own (never visible) top window, which sat above it.

Measured with `tools/portmaster_audit.py` (range requests into each port zip,
DT_NEEDED of every ELF, Android libraries that gmloader serves left out): all
1,264 aarch64 and architecture-independent ports. 146 needed something the card
lacks, led by libjpeg.so.8 (42), libzstd.so.1 (37), libtheoradec.so.1 (32),
libmodplug (28), X11 client libraries (20) and libFLAC (13), then a tail of 60
more (SDL 1.2, ffmpeg's codec libraries, speex, OpenSSL 1.1, readline 7...).
Some hits are ports whose runtime brings the library (Qt 6 in Dust Racing 2D's
runtime, GL inside Westonpack); those are left out of the manifest. The twelve most used runtimes, mounted on the handheld: gmtoolkit, Mono,
rlvm, FRT and Godot 4 need nothing; Westonpack needs libjpeg.so.8; .NET 8 only
an optional tracing library; Java 17 only X11 for AWT; Ren'Py needs libGL.

Verified on the handheld, each started from the launcher:

| Port | Before | After |
| --- | --- | --- |
| Grafx2 (native) | `libzstd.so.1`, `libjpeg.so.8` missing | Runs |
| NetSurf, MiceAmaze (ldd) | 1 library missing each | All resolve |
| Mr. Platformer (Godot 4 + Westonpack) | Ran the stock PortMaster's control.txt, exited | Runs on the top panel |
| Sokobrawn (armhf GameMaker, gmloader) | No 32-bit userland | Runs, with the GPU |
| Chromium B.S.U. (gl4es) | Already ran | gl4es works on our Wayland in every `LIBGL_FB` mode, so no `libgl_Buildroot.txt` is needed |
| Bugdom, Nanosaur (glvnd, no gl4es) | No libOpenGL/libGLX | Run, 3D rendered (main menu, credits) |
| MiceAmaze (libGL, no gl4es) | No libGL | Runs |
| Just Kiss Him Already (Ren'Py runtime) | librenpython needs libGL | Runs (title screen) |
| Wizznic (libreadline.so.7) | Missing | Runs, readline from bionic |

Nanosaur writes its settings to ROCKNIX's `/storage/roms/ports/nanosaur`,
which does not exist here, so its settings are not kept. Not done: OpenMW (a
debug OpenSceneGraph build no distribution ships), armhf
ports inside squashfs runtimes (read-only, so no PT_INTERP change), and pad
input inside Westonpack ports (not checked). Apotris crops its picture on 4:3 panels by its own design (Integer
Scale on scales 160 lines to 480 and cuts the sides); its Options turn it off.

## 11. Compatibility tiers

| Tier | Ports | Plan |
| --- | --- | --- |
| 1. aarch64, SDL2, no runtime | ~920 | Phase 1 target |
| 2. aarch64 + squashfs runtime | ~265 | Phase 1: every aarch64 runtime is gzip squashfs, which the stock kernel mounts |
| 3. Bundled SDL without Wayland | unknown share | Direct KMS mode (no menu window) |
| 4. `weston` runtime | 80 | Built 2026-10-26: Westonpack's own libmali/host-compositor ("ROCKNIX") mode, our PortMaster bound where westonwrap looks; Mr. Platformer verified |
| 5. armhf only | 204 (68 ready to run) | Built 2026-10-26: the compat layer's armhf part (focal 32-bit userland, Rockchip's 32-bit libmali, PT_INTERP pointed at a loader copy in /tmp); Sokobrawn verified |
| 6. x86_64 only | ~60 | Never |

The store shows a port's tier as a small note ("Runs without the menu") rather
than hiding tiers 3 and 4.

## 12. Testing

Host (Mac and Linux CI), in `tests/test_portmaster*.py`:

- Overlay generation against the pinned and the latest PortMaster release:
  `bash -n` on every generated file, and a fake control folder where a stub
  port script sources our `control.txt` and must see `GAMEDIR=/mnt/mmc/portmaster/ports/x`,
  `ESUDO=""`, our pad line in `$sdl_controllerconfig`, and `GPTOKEYB` pointing
  at our wrapper. PortMaster's own `tests/test_launchers.py` approach.
- `port_plan()` environment, namespace and cgroup commands (dry run).
- `PortRunner` freeze/thaw/kill on a fake process tree that forks, daemonises
  and holds a fake mixer semaphore.
- `scan_ports()` and the store adapter on PortMaster's `tests/data/ports.json`
  fixture; requirement filtering for 640×480 and 1024×768.
- Launcher: Ports in the Systems carousel under "Show all systems", port
  menu tiles, translations in every catalog.

Device:

- **Phase 0 probe** (read-only, `tools/portmaster_probe.sh`; rerun it on an RG
  DS Plus and after stock firmware updates).
- **PortMaster Test Suite** (`pmtest.zip` from PortMaster's `do_release.sh
  testsuite`): its pytest run, GUI tests on the real screen, an online
  install/uninstall and button checks. Its report is what upstream asks for.
- **Smoke matrix** (PortMaster's own tier-4 list): DevilutionX or VVVVVV
  (native), Spelunky Classic HD and 6 Feet Under (GameMaker), Mr. Rescue or
  BYTEPATH (Love2D), Cats on Mars (Godot FRT), Slime 3K Demake (Godot 4 +
  Weston), Cave Story lr or 2048 (RetroArch), Mindustry or Shattered Pixel
  Dungeon (Java 17), Megaball (Pyxel), Cute Fame (Ren'Py), Shards of God (AGS);
  with the owner's files: Celeste (Mono), Papers Please (box64). Each row
  records: starts, top panel, sound, controls, menu open/resume/quit, lid
  sleep, clean exit (no mounts, no processes, Weston focus back).
- Panel truth comes from `tools/scanout.c`, not renderer readbacks (they lie
  with two GL windows on Mali + Wayland).
- Install safety: DS userdata bytes and mtimes unchanged across a PortMaster
  install, a port install, a port run and an uninstall, as `test_play.py`
  checks for RG DS Play.

## 13. The road to official support

PortMaster's maintainers accept new firmwares (spruce in PR #237, the Retroid
devices in #326/#333) but turned down MiniLoong (#324) for lack of a testing
commitment. So:

1. Ship sections 4–10 with unmodified upstream PortMaster first, and collect
   Test Suite reports and the smoke matrix on the RG DS and RG DS Plus.
2. Talk to the team on their Discord with those results.
3. PR to PortMaster-GUI, the same shape as spruce's: detection in
   `device_info.txt` (a marker RG DS Core provides; `/etc/os-release` is the
   stock card's, not ours), `rgds/control.txt` + `rgds/PortMaster.txt` +
   `rgds/launch_hooks.txt`, `platform/rgds.py` (`PlatformRGDS` copying them in
   `portmaster_install`/`first_run`, telling our launcher to rescan), a
   `config.detect_paths` branch, our pad in `gamecontrollerdb.txt`, and the RG
   DS / RG DS Plus in `hardware.py`.
4. Commit to running the Test Suite on every PortMaster stable. Once merged,
   our overlay shrinks to nothing and `CFW_NAME` becomes ours.

## 14. Phases

Each phase ends usable and reversible.

**Phase 0 — Probe and spike. Done 2026-10-25.**
The read-only probe (`ssh root@<address> 'nice -n 10 sh -s' <
tools/portmaster_probe.sh`) filled section 3. The spike installed PortMaster
`2026.10.24-1611` (MD5 and GitHub SHA-256 checked) to `/mnt/mmc/portmaster`
by unpacking `PortMaster.zip`, bootstrapped harbourmaster with
`HM_TOOLS_DIR`/`HM_PORTS_DIR`/`HM_SCRIPTS_DIR`, replaced `control.txt` with a
spruce-style one (upstream kept as `control.upstream.txt`), added
`mod_Buildroot.txt`, and installed Apotris (native SDL2) and Cats on Mars
(Godot FRT 3.2.3, runtime downloaded and validated by harbourmaster). Each
port ran from SSH with the launcher paused, in a private mount namespace and
its own cgroup2 leaf, with panels read by scanout. What it showed:

| Question | Result |
| --- | --- |
| Install and bootstrap | Works as is: pylibs unpack, `device_info.txt` reports Buildroot, rk3568, 640×480, 3 GB, caps `1gb 2gb 4:3 640x480 aarch64 analog_0-2 power restore rumble touch vulkan` |
| Which panel | Both ports fullscreen on the top panel (CRTC 130); the bottom untouched |
| Keys from uinput (as gptokeyb sends them) | Reach the focused port: Enter took Cats on Mars from the title to its instructions |
| Runtime mount | The FRT squashfs was mounted only inside the port's namespace (`/mnt/mmc/portmaster/home/godot`), never visible outside, gone after the kill |
| cgroup2 | Mounting cgroup2 works (controllers `cpuset cpu`); `cgroup.freeze` froze bash, the game, gptokeyb and tee, and the top panel held still; `cgroup.kill` left nothing; the leaf and mount were removed |
| Focus after a menu | **Lost** when fullscreen windows were opened over the port and closed (no reaction to Enter after), kept with freeze/thaw alone, not restored by a uinput mouse click on either half of the layout. **Kept** when a bottom window was mapped before the port and redrawn as the menu |
| Godot framing | Without `GODOT_OPTS` the Godot window was cropped and off-centre; `mod_Buildroot.txt` with `--resolution 640x480 -f` fixed it |
| BusyBox tar | `funcs.txt` unpacks `NotoSans.tar.xz` on every port start and BusyBox tar fails ("invalid tar magic"): GNU tar in the compat layer is needed |
| Not covered | The physical pad through gptokeyb (no one pressed buttons), sound, a port with its own SDL, pugwash on the screen, `weston` runtime ports |

The spike's runner and overlay files are kept in `tools/portmaster_spike/`
(`run.py <script> <key> <key2>`, with `SPIKE_NOMENU`, `SPIKE_REFOCUS`,
`SPIKE_COMPANION` switches) and `play.py` for hand play from SSH (`ssh -t
root@<address> python3 -B /mnt/mmc/portmaster/.spike/play.py apotris|cats`;
Home, Select+Start, Ctrl+C or a dropped SSH session end the port and resume
the launcher; volume keys work through the release's `VolumeWatch`, with no
on-screen indicator). On the card they sit in `/mnt/mmc/portmaster/.spike/`
(`run.py`, a `scanout` build, the verified `PortMaster.zip`); the two ports and
the FRT runtime are installed. Nothing in `rgds/`, `/mnt/ports`, `/roms` or
`/mnt/mmc/Ports` was touched.

**Phase 1 — Ports run, from the library. Built 2026-10-25, except the compat layer.**
What exists: `runtime/portmaster.py` (folders, install of the latest release
with MD5 + SHA-256 checks and bottom-screen progress, overlay `ensure`,
launch plans, port covers), `runtime/port_runner.py` (`PortMenu`: cgroup2
freeze/thaw/kill, private mount namespace, input drain before thaw, key-device
grab while the menu is open, the menu helper started before the port),
`runtime/portmaster_files/` (control.txt, mod_Buildroot.txt, the PortMaster
entry), engine `portmaster` (CORES index 6), the Ports system in the launcher
(`model.hpp`, `game_launch.hpp`), `rgds-launcher --port-menu`
(`src/launcher/port_menu.hpp`: companion view, menu on Home, tiles Resume,
Sound, Brightness, Restart port, Return home), touch grab, 11 new strings in
all catalogs, `tests/test_portmaster.py`. Verified on the handheld through
`emulators.py launch --core portmaster`: port on the top panel, companion on
the bottom, menu freezes the tree, resume thaws it and keys still reach the
game, quit exits 0 with the cgroup and mounts gone; PortMaster's GUI opens
from its entry. Not verified: the physical pad, sound, lid sleep with a port,
the launcher screens on the device (host renders only).
Still to do from the original Phase 1 list:
`runtime/portmaster.py` (install, ensure, overlay, port_plan, scan), the
compat layer as a verified payload (`tools/build_portmaster_compat.py`: GNU
sed, grep with PCRE2, gawk, findutils, tar, unzip, gzip, psmisc, zstd, rsync,
dialog; static aarch64 builds from pinned sources, THIRD_PARTY rows; bash,
coreutils and the SDL libraries are already on the card), engine
`portmaster`, `show_ports` and the Ports
system, pugwash as "Get ports", basic `PortRunner` (freeze, resume, quit,
lid sleep, volume keys), playtime and Recents.
Done when: the owner installs PortMaster from Settings, installs VVVVVV and
Cats on Mars in pugwash, starts them from Ports, opens the menu, resumes,
returns home, and DS userdata is byte-identical.

**Phase 2 — The menu and the second screen (about a week).**
The bottom-only menu mode in the companion (port tile set), scanout pictures
for save backups, Controls page with `.gptk` overrides, save
folder backups, Port info, Performance mode, companion Guide/Touchpad/Keyboard,
volume HUD on the bottom panel, game switcher with ports.
Done when: every smoke-matrix row passes its menu checks.

**Phase 3 — Our own store (1–2 weeks).**
`portmaster.py serve`, the store page, Needs-your-files flow with SMB, update
badges and Update all, autoinstall drop zone.
Done when: install, update and remove of a ready-to-run port work without
opening pugwash, offline and online.

**Phase 4 — The hard tiers and official support (open-ended).**
Direct KMS mode, nested Weston for `weston` ports, the drift CI job, the Test
Suite on every stable, the upstream PR.

## 15. Risks

| Risk | Effect | Mitigation |
| --- | --- | --- |
| A future runtime or port image uses zstd or lz4 squashfs | It cannot mount (the kernel has zlib, lzo, xz only) | The drift check reads every runtime's compression; a `mount` shim that unpacks once with a bundled `unsquashfs` and bind-mounts the folder |
| Weston's desktop-shell gives focus to our windows (seen in the spike) | Port stops getting gptokeyb's keys | No window of ours maps after the port: the menu lives in the pre-mapped companion window; the companion grabs the touch panel; `SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1` |
| The bottom-only menu feels different from the DS menu | Consistency | Same tiles, style and controls on the bottom panel; the top shows the frozen game instead of a captured backdrop |
| A port with its own SDL opens on the bottom panel | Game on the wrong screen | Native SDL ports landed on top in the spike; test bundled-SDL ports in the smoke matrix; Direct KMS as fallback |
| Upstream changes `control.txt` contract | Ports break after an update | Drift guard per release; overlay keeps upstream's variables; the official PR removes the overlay |
| FAT32 card: no symlinks, 4 GB per file | Some ports fail to unpack or set up | PortMaster's `bind_directories`/`bind_files`; a compatibility note in the store; later an ext4 image on the card |
| A port started from the stock menu | It would run against stock's PortMaster 6.55 | Our scripts live in `/mnt/mmc/portmaster/ports`, which the stock menu does not scan |
| Scanout plane is AFBC | No picture for a save backup | Seen linear XR24 on both planes in the spike; back up without a picture otherwise |
| Heavy ports (Godot 4, Java, Mono) in 3 GB | Out of memory | Respect `2gb`/`ultra` reqs (the RG DS is not `ultra`); zram while they run |
| Owner's game files over SMB on a busy card | Slow copies, half-copied files | Show progress per folder; mark "Ready" only when the port's checks pass |
| Mixer semaphore held by a frozen port | Menu sounds freeze | Same retry as `stop_outside_mixer`, per member |

## 16. Decisions for the owner

1. ~~Placement~~: decided, part of "Show all systems" (section 9).
2. **Folders**: everything under `/mnt/mmc/portmaster` (recommended), or share
   the stock `/mnt/mmc/Ports` folder so ports also show in the stock menu
   (where they would run against stock's old PortMaster)?
3. **Store**: start with PortMaster's own GUI (Stage A) and build ours after
   (recommended), or wait and ship ours first?
4. **Upstream**: approach the PortMaster team once Phase 2 passes the matrix
   (recommended), or stay unofficial?

## Sources

- [PortMaster-GUI releases](https://github.com/PortsMaster/PortMaster-GUI/releases) and source (`control.txt`, `device_info.txt`, `funcs.txt`, `utils/pm_launch.txt`, `pylibs/harbourmaster/config.py`, `platform/miyoo.py`, `CLAUDE.md`)
- [PortMaster ports catalogue](https://github.com/PortsMaster/PortMaster-New/releases/latest/download/ports.json) and [Brotato's port](https://github.com/PortsMaster/PortMaster-New/tree/main/ports/brotato)
- [CFW requirements guide by bmdhacks](https://gist.github.com/bmdhacks/f26dbb60dc0ff0b9bc8e043fac8b813b), linked from [PR #324](https://github.com/PortsMaster/PortMaster-GUI/pull/324)
- [spruce support, PR #237](https://github.com/PortsMaster/PortMaster-GUI/pull/237)
- [portmaster.app](https://portmaster.app/)
