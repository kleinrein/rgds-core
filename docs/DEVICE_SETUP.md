# First RG DS device test

Use a separate stock Linux development card. Keep your existing card and back up saves before changing the firmware card. The first package launches from stock APPS and has no auto-start or boot-partition modifications.

## Prepare the card

1. Download official RG DS Linux from [Anbernic](https://win.anbernic.com/download/640.html), flash the image, and let its first boot finish. Record the firmware version. Linux and Android downloads appear on the same page; use the Linux image.
2. Check both screens, buttons, touch, sound, Wi-Fi, and lid sleep in stock. This gives us a baseline for later regressions.
3. Run `make package` on the Mac, or use the already generated ZIP shown in `dist/latest.txt`.
4. Shut the device down and extract the ZIP contents into the user partition root, beside `Roms/`. The result should include `rgds/` and the new entries under `Roms/APPS/`. Avoid an extra enclosing folder.

```text
Roms/APPS/
  RGDS Inventory.sh
  RGDS Probe.sh
  Try RGDS.sh
  RGDS Recovery.sh
rgds/
  main.sh
  inventory.sh
  state/active
  releases/<build>/
```

Do not copy this overlay onto a ROCKNIX card. It targets stock Linux and has not been checked against an arbitrary stock revision yet. A package-extraction update is suitable for this first test card; subsequent development updates should use the staged SSH deployment tools rather than overwrite a running build.

## Capture the baseline

Boot stock and run `APPS → RGDS Inventory`. The report goes to `rgds/userdata/inventory.txt`. It records system and library versions, mounts, displays, input devices, power interfaces, network interface addresses, and available helpers. It excludes Wi-Fi profiles and private keys.

If stock offers SSH, enable it and note the IP address. The development tools accept `DEVICE=root@<IP>`. Stock SSH setup and credentials must be verified on the actual firmware; this package does not enable or expose an SSH service itself. Device sessions require Python 3.8 or newer; Inventory can run without Python and will report what is present.

## Check both displays and input

Run `APPS → RGDS Probe`. It shows labelled top/bottom screens, colour blocks, a touch grid and the latest input. It automatically exits after 90 seconds. B or Escape exits sooner with the initial mapping. The wrapper pauses only identified stock frontend processes and restores the same processes after completion; a separate guardian also handles a terminated wrapper.

Check orientation, the top/bottom labels, raw button numbers, D-pad/sticks, and whether the crosshair appears exactly under your finger on the bottom screen. The current defaults are top `DSI-1`, bottom `DSI-2`, A button 0, B button 1. These are hypotheses from the SimpleOS package, not verified mappings for your card. Overrides exist as `RGDS_BUTTON_A`, `RGDS_BUTTON_B`, `RGDS_SDL_LIBRARY`, and launcher `--top-display/--bottom-display` arguments.

If the probe fails, stock should return and `rgds/userdata/session.log` will contain the SDL/library error. A normal reboot always boots stock because this package does not install auto-start. Capture the report and log before changing device init or power scripts.

## Try each emulator

Put games in `Roms/NDS/` or `rgds/games/`. Existing `simpleos/games/` is also scanned if present. Run `APPS → Try RGDS`, pick the engine using START or the touch buttons, then choose a game and press A.

Start with a small 2D title. Check both screens, touch, audio, normal in-game saving, the emulator's own menu, and return to the launcher. Repeat separately for DraStic, DSperate and NNDDSS. Core availability does not prove that its display and audio dependencies match stock.

DraStic uses existing stock saves/settings. DSperate uses its separate development profile; NNDDSS creates a separate writable root. This build does not import saves from ROCKNIX or convert state files. NNDDSS's touch/key defaults are `/dev/input/event1` and `/dev/input/event3`; adjust `NNDDSS_TOUCH_DEV`/`NNDDSS_KEYS_DEV` only after Inventory confirms the devices. Required BIOS are copied from the stock installation, never supplied in the ZIP.

Exit each emulator normally through its own menu so save handling completes. Games retain stock sleep handling; the new RGDS sleep controls apply in the menu.

## Check power and brightness settings

Open Settings → Theme & display → Screen brightness and verify both panels at 5%, 50% and 100%. In Power, try each profile, confirm battery/charging information, then return to Stock. The profiles use frequencies advertised by the CPU/GPU/DMC drivers; unavailable controls are labelled. Exit RGDS and confirm stock's original brightness and clocks return, then relaunch to check preference persistence.

Test Sleep now from the menu, wake with Power, and verify both panels, input and sound. Then test menu idle sleep and Sleep on lid close, including several close/open cycles and charging while asleep. These automatic controls default to disabled. Lid sleep requires the Anbernic hall sensor and writable wake control; verify the reported hall polarity against your card. Capture `session.log` and a fresh Inventory if any wake behavior differs from stock. These new hardware paths have fixture and desktop coverage but still require device validation; in-game suspend and power-off integration remain future work.

## Development updates and recovery

After SSH access is confirmed:

```sh
make probe DEVICE=root@<IP>
make deploy DEVICE=root@<IP>
make run DEVICE=root@<IP>
make logs DEVICE=root@<IP>
```

If the card mounts somewhere other than `/mnt/mmc`, add `SD_ROOT=/mnt/SDCARD` using the inventory's actual path. Deployment stages and verifies the build, refuses to interrupt gameplay, and preserves the previous version. `make rollback DEVICE=root@<IP>` selects the previous build, then `make run` starts it.

`APPS → RGDS Recovery` creates `rgds/userdata/boot_stock`, which disables further temporary RGDS launches. Remove that file from the computer when ready to test again. Stock remains the normal boot target. To remove this experiment, delete `rgds/` and these four APPS entries after backing up any development saves; no stock files need restoration.

If a launcher is stuck, use its normal B/Escape exit or terminate the identified RGDS session over SSH. Do not run broad `killall` commands against stock services. The session log and inventory are the first evidence to inspect.
