# Installing and updating RG DS Core

RG DS Core installs as an app on the RG DS's own stock Linux card. Nothing is
formatted or flashed, and the stock menu stays available.

## Install

1. Shut the handheld down normally and put its games card in your computer.
2. Extract `RG-DS-Core-Installer-<version>.zip` to the card root. Merge its
   `Roms/` folder with yours and copy `RGDS-Install/` next to it:

   ```text
   Roms/APPS/Install RG DS Core.sh
   RGDS-Install/
     install_package.py
     package.zip
     README.txt
   ```

3. Put the card back and open **APPS → Install RG DS Core**. The installer checks
   the package, installs it into `rgds/`, opens RG DS Core and makes it the
   startup app.
4. Copy your `.nds` files (or ZIPs with one game each) to `Roms/NDS`, then choose
   **Settings → Library → Add your games → Scan for games**, or just reopen the library.

If something goes wrong, `RGDS-Install/install.log` says why. The installer
removes its package after a successful install; copy the bundle again to
reinstall.

Starting from Android or a blank card? Prepare the stock Linux card first: see
[card setup](DEVICE_SETUP.md). RG DS Plus owners use the Plus's own stock card;
see [RG DS Plus](RG_DS_PLUS.md).

## Update

- **Over Wi-Fi:** **Settings → System → Updates & recovery → Check for updates**.
  Updates are signed, download in the background and install on restart. If the
  new version fails to start, the previous one comes back by itself.
  [How updates work](OTA.md).
- **By card:** repeat the install with a newer installer. Games, saves, settings
  and artwork are kept.

## Recovery

- **Settings → System → Updates & recovery → Use previous build** returns to the
  version you had before.
- **Settings → System → Startup → Boot into** chooses RG DS Core or the stock menu.
- **APPS → RGDS Recovery** (in the stock menu) turns off direct startup without
  touching your saves.
- A memory card that has become read-only after a crash is checked and repaired
  on the next start; RG DS Core shows a short "Checking the memory card" screen.

Logs live on the card: `rgds/userdata/launch.log`, `rgds/userdata/session.log`
and `rgds/userdata/update/status.ini`.

## For developers

`make installer` builds the installer; `python3 tools/package_installer.py` wraps
an existing package. SSH deployment for development is in
[Development](DEVELOPMENT.md).
