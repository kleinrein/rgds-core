# Installing RG DS Core on the working Anbernic card

The current candidate is a drop-on-card installer for the working Anbernic Linux
card. No formatting, flashing or SSH is required. It is a private testing build;
public licensing and payload provenance are still being resolved.

1. Extract the `RG-DS-Core-Installer-*.zip` at the games-card root, merging `Roms/`
   and copying `RGDS-Install/` alongside it.
2. Open stock **APPS → Install RG DS Core**. The installer verifies the package,
   installs it and opens the launcher.
3. Optionally enable **Settings → System → Startup → Boot into → RG DS**.

For an update, exit the game, return to stock and repeat with the newer installer.
ROMs, saves, settings and artwork are retained. **Settings → System → Updates &
recovery → Use previous build** rolls back the application. Keep enough space
for both versions; the installer checks staging space before writing.
The payload ZIP is consumed on success. Copy the full bundle again to reinstall.
Errors are recorded in `RGDS-Install/install.log`. Stock APPS also retains
**RGDS Recovery** for disabling direct startup without deleting saves.

Developers can generate the candidate with `make installer`, or wrap an existing
package using `python3 tools/package_installer.py`. SSH deployment remains
available in [Development](DEVELOPMENT.md). See the [release and OTA plan](RELEASE_PLAN.md)
for supported behavior, validation limits and public-release prerequisites.

The Armbian image/flash/recovery experiment is retired. Its source and historical
documentation are archived under `build/retired-armbian-20261011/`; hardware
failure logs and stock backups are retained. There is no maintained independent
RG DS Core OS image to flash. The OTA foundation is installed, but its public channel is not configured. See [OTA setup](OTA.md).

First installation now enables RG DS Core direct startup on the supported stock supervisor. Upgrades preserve an explicit Anbernic startup preference. Change it in Settings → System → Startup.
