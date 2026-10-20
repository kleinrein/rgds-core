# First RG DS device test

For the RG DS Plus, use its own stock Linux card and follow the [Plus preview checklist](RG_DS_PLUS.md). Compatibility is implemented but has not been tested on a physical Plus.

Use a separate stock Linux development card. Keep your existing card and back up saves before changing the firmware card. APPS remains the default; System → Startup optionally enables direct RG DS through the original stock supervisor hook. Application updates preserve the kernel and vendor Wi-Fi/display initialization. The separate verified branding installer can patch only boot-logo pixels and checksums, with original backups. See [startup and recovery](BOOT.md).

## Prepare the card

1. Download official RG DS Linux from [Anbernic](https://win.anbernic.com/download/640.html), flash the image, and let its first boot finish. Record the firmware version. Linux and Android downloads appear on the same page; use the Linux image.
2. Check both screens, buttons, touch, sound, Wi-Fi, and lid sleep in stock. This gives us a baseline for later regressions.
3. Run `make installer`, or use the verified SD installer ZIP in `dist/`.
4. Shut down the device and extract the installer at the games-card root, beside `Roms/`. Copy both `RGDS-Install/` and `Roms/`, without an extra enclosing folder.
5. Boot stock and open **APPS → Install RG DS Core**. The installer verifies and stages the release, then opens the launcher. Subsequent card updates use the same route and preserve user data.

See [the installation guide](SD_FLASH.md) for the complete user-facing path. This is an application installer for stock Linux, not a firmware image for Android or ROCKNIX. The raw application ZIP is an internal packaging artifact; use the SD installer for card installation or the staged SSH tools for development.

## Capture the baseline

Over SSH, `make inventory DEVICE=root@<IP>` runs `runtime/inventory.sh` on the device and saves the report under `docs/`. It records system and library versions, mounts, displays, input devices, power interfaces, network interface addresses, and available helpers. It excludes Wi-Fi profiles and private keys.

If stock offers SSH, enable it and note the IP address. The development tools accept `DEVICE=root@<IP>`. Stock SSH setup and credentials must be verified on the actual firmware; this package does not enable or expose an SSH service itself. Device sessions require Python 3.8 or newer; the inventory script runs without Python and reports what is present.

## Check both displays and input

Over SSH, `sh /mnt/mmc/rgds/main.sh --probe` starts the display and input probe. It shows labelled top/bottom screens, colour blocks, a touch grid and the latest input. It automatically exits after 90 seconds. B or Escape exits sooner with the initial mapping. The wrapper stops identified stock frontends to release audio/input handles under its own exclusive stop marker. A separate guardian restores hardware and releases that marker before returning ownership to the original stock supervisor. It does not manually respawn the captured frontend or restart Wi-Fi/display services.

Check orientation, the top/bottom labels, raw button numbers, D-pad/sticks, and whether the crosshair appears exactly under your finger on the bottom screen. The current defaults are top `DSI-1`, bottom `DSI-2`, A button 0, B button 1. The tested Goodix panel reports Type A multitouch with X 0–640 and Y 0–480; the raw decoder handles Type A, Type B and single-touch devices with SDL finger fallback only when raw input is unavailable. While the raw device is open, it owns touch even between gestures so display-mapped SDL coordinates cannot override panel coordinates. Check Inventory on your own firmware before relying on that mapping. Overrides exist as `RGDS_BUTTON_A`, `RGDS_BUTTON_B`, `RGDS_SDL_LIBRARY`, and launcher `--top-display/--bottom-display` arguments.

If the probe fails, stock should return. `rgds/userdata/launch.log` captures startup errors, including missing release files, Python import failures and display/session-lock discovery; `rgds/userdata/session.log` captures the launcher after startup. The RG DS Core entry follows the configured RGDS installation, including when Startup is set to stock. Without a usable configuration, they prefer the package beside their own `Roms/APPS/` folder, then check the standard stock card mounts. A reboot uses stock unless System → Startup was explicitly set to RG DS. Capture the report and both logs when diagnosing a failure.

## Try each emulator

Put games in `Roms/NDS/` or `rgds/games/`. Both stock card mounts remain scanned when RGDS runs from writable internal storage. Run `APPS → RG DS Core`. The home carousel contains Settings, Games, Favourites and Recents. Open Games, choose a cartridge and press A. Choose the default engine in Settings → Library, or use the cartridge's Options → Emulator override. Back from a game list returns to its home category; Back at Home stays in RGDS. Settings → System → Return to stock is a confirmed recovery action.

Start with a small 2D title. Check both screens, touch, audio, normal in-game saving, the emulator's own menu, and return to the launcher. Repeat separately for DraStic, DSperate and NNDDSS. Core availability does not prove that its display and audio dependencies match stock.

DraStic copies existing stock saves/settings into a separate development home. DSperate uses its separate profile; NNDDSS creates a separate writable root. This build does not import saves from ROCKNIX or convert state files. On the tested unit, touch was `/dev/input/event1` and ANBERNIC keys were `/dev/input/event4`, which are the NNDDSS defaults. Adjust `NNDDSS_TOUCH_DEV`/`NNDDSS_KEYS_DEV` only after Inventory confirms the devices on your firmware. Required BIOS are copied from the stock installation, never supplied in the ZIP.

Open Settings → Sound and mute before the first game test. Confirm muted startup, then raise the level slightly with the physical buttons. The same saved volume is used by all engines. The first run with no saved volume starts at no more than 25%; it does not trust a stock mixer left at full volume. Validate buttons, held-key repeats, mute, normal return and resume separately for each engine.

Menu/Anbernic opens the same RGDS pause menu for every engine. B or Resume goes back to play. Return home saves first by default; wait for it to complete. If it fails, resume/retry or deliberately choose Return without saving. Load restoration, native controls, menu focus and save persistence still need hardware validation. The local lid-sleep update extends RGDS lid monitoring to gameplay and the game menu for all three engines; hardware suspend/resume acceptance is still pending.

The pause menu's Shaders list switches the active renderer immediately and saves the cartridge's preset. DraStic/NNDDSS use the owned GL pass; DSperate exposes native Off/bilinear. Missing rendering capabilities report an error while retaining the original pixels. Save & restart remains an explicit checkpoint-first action. These paths still need a device check for each engine.

## Wi-Fi and network file sharing

Settings → Connectivity shows the connected Wi-Fi network and its IPv4 address.
Choose **Network file sharing** to turn SMB sharing on or off, then open
**Sharing details** for the address, username and temporary password. On macOS,
use Finder → Go → Connect to Server with the displayed `smb://` address. On
Windows, enter the displayed `\\` address in File Explorer and use the login
shown on the device.

Sharing exposes existing DS games folders as `Games` (and `Games2`, etc. when
both cards have games), plus `Imports` and `Exports` for portable save archives.
Put `.rgdssave.zip` archives in Imports or collect backups from Exports; use the
save gallery to import/export them. Scan for games after transferring ROMs.
The RGDS save store, Wi-Fi credentials and RGDS configuration are outside these shares.

Sharing starts off, requires a Wi-Fi connection, and stops when Wi-Fi is turned
off, another network is selected, the connection changes, or the RGDS session
ends. Its password changes on each enable. The runtime uses a private Samba
configuration and does not change or stop firmware-owned sharing services.
RGDS bundles its ARM64 SMB server, login tool, RPC service and runtime libraries
under `services/samba/`. Nothing needs to be installed in the firmware. The
toggle shows On only after an authenticated connection succeeds. Both the SMB
server and RPC workers stop together when sharing is turned off. RGDS must run
as root to bind port 445, as it does on the stock card. Private passwords,
databases and RPC sockets live in `/run/rgds-samba/`, so FAT games cards work and
login details disappear on reboot. Shared files remain on the games card.

On October 13, 2026, the bundled service was verified on the stock FAT games
card: authenticated SMB3 share listing and upload/download over Wi-Fi passed,
and switching Off closed port 445. The installed service update is
`0.0.1-dev-smb-b7a4e1aed19b`; evidence is retained in ignored
`build/smb-acceptance/`. RPC workers are passed by their explicit package paths,
so service startup does not depend on directories on the build machine.

## Check power and brightness settings

Open Settings → Theme & display → Screen brightness and verify both panels at 5%, 50% and 100%. In Power, try each profile, confirm battery/charging information, then return to Stock. The profiles use frequencies advertised by the CPU/GPU/DMC drivers; unavailable controls are labelled. Exit RGDS and confirm stock's original brightness and clocks return, then relaunch to check preference persistence.

Test Sleep now from the menu, wake with Power, and verify both panels, input and sound. Then test menu idle sleep and Sleep on lid close, including several close/open cycles and charging while asleep. A lid sleep during a game freezes the engine and the game carries on where it was when the lid opens; the emulated DS lid is not closed (DSCore's own hinge pulse on resume is off: a game that had not reached its sleep by the open edge stayed asleep with white panels and refused to save). New settings default to lid sleep On and menu idle sleep Off. Existing saved Off choices are preserved: enable **Settings → Power → Sleep on lid close** after updating an older installation. Lid sleep requires the session event monitor, writable wake control and stock `pm-suspend`. On the tested firmware, `rk805 pwrkey` emits code 110 on close and 111 on open. The guardian latches press events and shares their state in RAM; the `hallkey` sysfs value stays at 3 and is not a position reading. Launcher sleep/wake was confirmed by the owner on October 12; see [current evidence and remaining checks](archive/POWER_WIFI_2026-10-12.md).

The launcher watches the lid during navigation as well as at rest. The game runner owns this check during DraStic, DSperate, NNDDSS and melonDS, including their overlay menu, so there is only one sleep requester. It waits for pending game commands, stops only its owned engine/menu processes, invokes stock sleep hooks, then restores their previous paused/running state. Closing the lid never requests a checkpoint, quit or restart. Unknown or stale sensor readings do not count as opening the lid; one stable closure triggers one attempt, and a stable reopen rearms it. On failure, reopen and close to retry. No raw kernel-suspend fallback or manual panel blanking is used.

Capture `session.log`, the current engine log, `logs/lid-sleep.ini`, `/var/log/pm-suspend.log` when present, and a fresh inventory (`make inventory`) if wake behavior differs from stock. `helper-returned` records a successful helper exit, not proof of physical suspend. Confirm both panels, controls, touch, sound, Wi-Fi and game progress after reopening; repeat with a held button and stylus, a game menu open, a save in progress, and USB charging. These paths have offline coverage but still require device validation. Home-menu power-off and restart requests use stock init and also need hardware regression.

## Development updates and recovery

After SSH access is confirmed:

```sh
make probe DEVICE=root@<IP>
make deploy DEVICE=root@<IP>
make run DEVICE=root@<IP>
make logs DEVICE=root@<IP>
```

If the card mounts somewhere other than `/mnt/mmc`, add `SD_ROOT=/mnt/SDCARD` using the inventory's actual path. Deployment stages and verifies the build, refuses to interrupt gameplay, and preserves the previous version. `make rollback DEVICE=root@<IP>` selects the previous build, then `make run` starts it.

Settings → System → Startup switches back to Anbernic startup. `APPS → Uninstall RG DS Core` removes the application: it puts the stock autostart hook, trim service, shutdown step and boot artwork back from their backups, deletes `rgds/releases`, `rgds/state` and the other application files, and removes the RGDS entries from APPS on both cards. `rgds/userdata` (saves, settings, artwork cache, logs) and `rgds/games` stay, so a later install finds them again; delete `rgds/userdata` by hand to remove them too. It refuses while a game is running. Vendor init files are left intact.

Use Settings → System → Return to stock for a deliberate recovery exit. If the launcher is stuck, terminate the identified RGDS session over SSH. Do not run broad `killall` commands against stock services. The launch log, session log and inventory are the first evidence to inspect.

First installation now enables RG DS Core direct startup on the supported stock supervisor. Upgrades preserve an explicit Anbernic startup preference. Change it in Settings → System → Startup.
