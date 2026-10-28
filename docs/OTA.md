# Releases and over-the-air updates

RG DS OS updates itself over Wi-Fi from a signed feed. An update is the whole system image, written into the card's
other root slot while the running one stays untouched; the handheld then restarts into it on trial and falls back to
the slot it came from if the new one does not come up. Games, saves and settings live on the data partition, which
an update never touches.

## Publishing on GitHub

GitHub Releases host everything. Each release carries the card image for new installs, the update package and the
signed feed (`stable-os.json`); `https://github.com/OWNER/REPO/releases/latest/download/stable-os.json` always points
at the newest feed.

Once, after creating the repository:

```sh
python3 tools/release.py init --repo OWNER/REPO
```

`init` creates an Ed25519 signing key at `~/.config/rgds/release-key.pem` (keep it private and backed up; it never
goes in the repository) and pins the feed and the public key in `packaging/update-trust.json`, which every image
carries in `app/os/`. The pinned feed names `stable.json`; RG DS OS reads `stable-os.json` beside it.

For every release:

```sh
echo 0.8.0 > VERSION
make image                                  # tools/own_os_build.py, after the package and engine builds
make release NOTES=notes.md                 # tools/own_os_release.py --version 0.8.0 --notes notes.md
```

`own_os_release.py` writes `dist/release-os/<version>/`:

| File | What it is |
| --- | --- |
| `RG-DS-OS-<version>.img.gz` and `.sha256` | The card image for a new install, written to a spare microSD card with a flasher |
| `RG-DS-OS-<build>.zip` | The update: `<build>/rootfs.squashfs`, `Image.lz4`, the device tree, `extlinux.conf` and `manifest.json` with their hashes (the squashfs is stored, not re-compressed) |
| `stable-os.json` | The feed, signed with the key `init` made |
| `notes.md` | The release notes, also inside the signed feed |

It refuses a version other than the last image's, a package larger than handhelds accept, and a key that is not
pinned in `packaging/update-trust.json`. It prints the `gh release create --draft` command; `--upload` runs it.
Either way the release is a **draft**: nothing is public, and no handheld sees it, until it is published on GitHub
and marked as the latest release. Each publish gets a higher sequence number (the publish time), so devices always
move forward. Versions are three-part (`0.8.0`, not `0.8`).

Handhelds on v0.7.0, the old add-on for the stock firmware, read `stable.json` from the latest release. A release
without that file answers them with "not found", which they show as nothing new; they are never offered an OS image.

### A local test before publishing

The full flow can run against this computer instead of GitHub, with the real signing key and the real image, and
nothing uploaded (done for 0.8.0 on 2026-10-28):

1. A throwaway CA and a certificate for the Mac's LAN address (use Homebrew's `openssl`: macOS's LibreSSL signs with
   SHA-1, which the handheld's OpenSSL 3 refuses, and its Python cannot talk TLS to it either).
2. A small HTTPS file server with `Range` support in a folder holding the update zip and a `stable-os.json` signed by
   `publish_update.sign()` whose package URL points at that server (two days' expiry).
3. On the handheld: a copy of `/etc/ssl/certs` in `/run` with the CA added and its hash link, bind-mounted over
   `/etc/ssl/certs`, and `/data/rgds/state/update-trust.json` naming `https://MAC:8443/stable.json` (the state copy
   wins over the image's).
4. Home shows the banner within the next check (or run `device_services.py update-check`), and Y, Restart & install
   work as for a real release. Afterwards remove `state/update-trust.json`; the bind mount ends with the restart.

## User flow

When the background check (every 6 hours, 30 minutes after a failed one) finds a release, Home shows a "System
update" banner on the upper screen and a badge on the RG DS mark. **Y** on Home (or a tap on the mark) opens Updates &
recovery and starts the download; once it is staged, Y asks to restart and install. B returns to Home. Settings →
System → Updates & recovery offers the same steps by hand: Check for updates, Release notes, Download update,
Restart & install, with Y in the footer as the shortcut.

While it runs, the upper screen shows the release, four steps (Download, Verify, Prepare, Install), the current
step's percent, megabytes, speed and time left on a glass progress bar. The download resumes on its own after a
dropped connection: up to six attempts in a row without new bytes (2, 4, 8, 16 and 30 s apart), 120 s below 64 KiB
counts as dropped, and no download runs longer than six hours. A paused or interrupted download, including one cut
off by a restart, resumes from its part file. A missing package (HTTP 4xx) is not retried. Errors remain visible
and allow retry; a staged update can be discarded. A failed background check never replaces what the owner last
saw. The running system continues until the owner confirms the restart.

`status.ini` fields the launcher reads: `state`, `message`, `offer` (the version on offer, empty once current,
applied or discarded), `version`, `current`, `notes`, `size`, `received`, `progress` (of the current step), `rate`
(bytes/s), `eta` (seconds, -1 unknown) and `prepared`.

## What an update does

`runtime/os_update.py` (`OsUpdater`, chosen by `device_services.py` on RG DS OS) reuses `runtime/ota.py` for the
feed, the signature, the download with resume and the launcher's pages; what differs is what an update is.

**Download update** writes the root filesystem straight to the other slot's partition and that slot's kernel and
device tree to its folder on the boot partition, reading every byte back against the manifest; its `LABEL` block in
`extlinux.conf` is replaced, `DEFAULT` is not. Nothing the running slot uses is touched.

**Restart & install** puts the new slot first with the running one as `FALLBACK`, arms U-Boot's boot counter
(`/bootcount` on the boot partition, 4 bytes: magic, version, count, upgrade-in-progress) through
`/usr/libexec/rgds/trial arm`, and restarts. From then on each boot of the new slot counts: once RG DS Core is up
(the launcher's first update check, or the session one minute after the launcher started) `trial commit` clears the
counter and drops the `FALLBACK` line. Three boots without a commit and U-Boot runs `altbootcmd`
(`uboot-patches/003`), which takes the `FALLBACK` label: the old slot boots, `rcS` (`trial boot`) notices it is not
the trial slot, records `rolled-back` for that build and puts `DEFAULT` back, and the launcher shows "The update
could not start. Your previous version was restored." That build is not offered again. A kernel that panics
restarts after 10 s (`panic=10`); a slot that boots but never reaches the launcher is restarted by `rcS` after four
minutes so the counter advances. `tools/own_os_push.py` arms the same trial for a developer push.

Not covered: the boot partition's own `extlinux.conf` and U-Boot are never replaced by an update (a U-Boot change is
a card reflash, or `own_os_push.py --uboot` by hand), and the data partition is never touched, so a release must keep
reading the previous release's userdata.

## Trust and signing

The image's `app/os/update-trust.json` (from `packaging/update-trust.json`) pins the channel, the feed and one or more
raw Ed25519 public keys; `app/os/update_guard.py` holds the update lock, the journal and the protocol number. Both
are on the read-only system image, so an update can only change them by shipping a new image signed with a key the
running one already trusts. A `state/update-trust.json` on the data partition wins (a testing channel, or the local
test above); the OS feed is derived from it the same way. Multiple pinned keys allow a planned overlap during
rotation; a remotely supplied key is never trusted.

```json
{
  "schema": 1,
  "channel": "stable",
  "feed": "https://github.com/OWNER/REPO/releases/latest/download/stable.json",
  "keys": {"release-2026": "64_HEX_CHARACTERS_OF_ED25519_PUBLIC_KEY"}
}
```

The signed payload holds schema, target (`rgds-os-rk3566`), base (`rgds-os`), minimum updater protocol, channel,
version, build ID, a monotonically increasing sequence, issued/expiry timestamps, the HTTPS package URL, compressed
and unpacked sizes, SHA-256 and release notes. `tools/publish_update.py` signs the exact UTF-8 bytes with OpenSSL
Ed25519; the JSON envelope holds `key_id`, base64 `signed` and base64 `signature`. The handheld verifies with
OpenSSL's [EVP Ed25519 API](https://docs.openssl.org/3.0/man3/EVP_DigestVerifyInit/) through `libcrypto`, not custom
cryptographic code. It rejects expired metadata, older sequences, same-sequence changes, untrusted signatures, bad
hashes, oversized or unsafe archives, a low battery without the charger and a running game. TLS uses the system
certificate store; HTTPS redirects cannot downgrade to HTTP. Upload the package before the feed, keep each artifact
immutable, and publish a new sequence rather than replacing a package behind a signed URL.

## Automatic checks and downloads

`userdata/experience.ini` stores `auto_update`: `0` off, `1` idle checks (default), `2` idle checks plus download.
Checks are persisted and limited to once per six hours, including after launcher restarts. An empty release feed
produces no network request. Active gameplay and pending update trials suppress background work. A new release
appears in the menu; restart/install remains explicit and uses the same signature, power and rollback gates as
manual updating.

## Validation

Host tests (`tests/test_ota.py`, `tests/test_os_update.py`, `tests/test_ota_ui.py`) exercise real signatures,
tampering, expiry/replay, target mismatches, resumed transfers, package corruption, slot staging and read-back,
`extlinux.conf` label replacement, the launcher's entry point and the trust file choice.

On hardware (2026-10-28, standard RG DS): 0.7.1 on slot B updated to 0.8.0 through the launcher with the local test
feed: banner on Home, Y, 362 MB at 7.6 MB/s over Wi-Fi, slot A written and verified, Restart & install, slot A up in
about 20 s, trial committed after a minute, "You are up to date". The fallback path was exercised for real on
2026-10-27 (a trial slot that never committed: three counted boots, the fourth fell back and recorded
`rolled-back`). That test also found the two bugs that had kept every OS update action from working:
`OsUpdater.run` was shadowed by a path attribute, and a trust file in `state/` skipped the `-os` feed.
