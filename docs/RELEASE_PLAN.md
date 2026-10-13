# RG DS Core installation and releases

## What works today

The first distribution format is a drop-on-card application installer for the
working Anbernic RG DS Linux card. It does not flash firmware. Build with
`make installer`, or wrap an existing verified package with
`python3 tools/package_installer.py`. The output in `dist/` contains a README,
`Roms/APPS/Install RG DS Core.sh` and `RGDS-Install/`. Extract both directories
at the card root, then launch the installer from stock APPS. No SSH is required
for this route. Existing ROMs, saves, artwork and preferences stay in place.
English is the default for new installations; updates preserve the chosen language.

The SD and SSH routes now share `packaging/install_package.py`. Before activation
it validates archive paths, duplicate entries, file hashes, target and release
contents, and checks staging space. The SD entry also pins the complete package
SHA-256, covering bootstrap files. An active game blocks installation. Releases
are installed side by side, with previous/active pointers and manual rollback for SD installs. Signed OTA
updates additionally run a supervised startup trial with automatic rollback.
Recovery remains available from the stock APPS menu.

This is a private installer candidate. The repository has no project-wide
license yet, and the supplied NNDDSS payload's public redistribution provenance
is unresolved (see [THIRD_PARTY](../THIRD_PARTY.md)). These are public release
blockers. Packages exclude user ROMs, saves, Wi-Fi profiles and Nintendo BIOS
dumps. The checksum detects damage; it does not establish publisher identity.

## Release sequence

1. Validate the installer on a spare, clean stock card through the physical APPS
   menu, then test upgrade and rollback. The development device upgrade and host
   simulations are useful evidence, but do not replace first-install acceptance.
2. Resolve project and bundled-component licensing; remove or replace payloads
   that cannot be redistributed. Keep matching source, licenses and build records
   with every distributed emulator. Publish build/setup instructions with the repo.
3. Tag immutable versions (for example `0.1.0-alpha.1`), with testing and stable
   channels. Each release should include the installer, checksum, changelog,
   supported hardware/base-firmware list, source tag and provenance records.
   Current content-derived `0.0.1-dev-*` IDs remain development builds.
4. Provision authenticated OTA and complete release acceptance before advertising automatic updates.
   Promote the same tested artifact to stable instead of rebuilding it.

## OTA foundation

The signed update service, progress UI and automatic startup rollback are now
implemented. No public channel is provisioned yet. See [the protocol and release
setup](OTA.md) for trust pinning, signing, sequence/expiry checks, staged activation,
boot health and the remaining hardware acceptance work.

Before public rollout: configure the real repo/feed and public key in the initial
installer, protect the signing key, validate a clean stock card and a real HTTPS
release, and exercise power cuts on disposable media. Future save migrations must
remain backward-compatible while rollback is offered. Shared bootstrap upgrades
still require the SD installer; OTA does not flash the kernel or boot partitions.

## A future flash image

The retired Armbian experiment is not a supported base. A public flash image
needs a redistributable kernel/userspace and hardware drivers, reproducible clean
image/partition construction, first-boot resize/setup and an independent recovery
route. Do not publish a clone of a personal card. Kernel/boot updates require a
separate recovery or A/B design; app rollback cannot repair those components.
The stock-card installer is the practical initial release path.
