# OTA application updates

RG DS Core updates itself over Wi-Fi from a signed feed. The channel ships
**unconfigured**: until you pin a feed and a public key, the updater makes no
network requests. Updates replace the RG DS Core application only, on the tested
standard RG DS / Anbernic Buildroot 2024.02 base.

## Publishing on GitHub

GitHub Releases can host everything: each release carries the update archive,
the signed feed (`stable.json`) and the installer, and
`https://github.com/OWNER/REPO/releases/latest/download/stable.json` always
points at the newest feed.

Once, after creating the repository:

```sh
python3 tools/release.py init --repo OWNER/REPO
make installer        # new installs now follow your channel
```

`init` creates an Ed25519 signing key at `~/.config/rgds/release-key.pem` (keep
it private and backed up; it never goes in the repository) and pins the feed
and the public key in `packaging/update-trust.json`.

For every release:

```sh
echo 0.1.0 > VERSION
make installer
python3 tools/release.py publish --version 0.1.0 --notes notes.txt
```

`publish` signs an update for the latest package, writes `dist/release/0.1.0/`
and prints the `gh release create` command that uploads the archive, the feed
and the installer (`--upload` runs it for you). It refuses a package whose
version differs from `--version`, and one built before `init`, since installs
from it would never see the channel. Each publish gets a higher sequence number,
so devices always move forward. Use `--channel testing` (and a separate `init`)
for a beta channel.

Devices installed before the channel was configured need the new installer
once; after that, updates arrive over Wi-Fi.

## User flow

When the background check (every 6 hours, 30 minutes after a failed one) finds
a release, Home shows an accent banner on the upper screen and a badge on the
RG DS mark. **Y** on Home (or a tap on the mark) opens Updates & recovery and
starts the download; once it is staged, Y asks to restart and install. B returns
to Home. Settings → System → Updates & recovery offers the same steps by hand:
Check for updates, Release notes, Download update, Restart & install, with Y in
the footer as the shortcut.

While it runs, the upper screen shows the release, four steps (Download, Verify,
Prepare, Install), the current step's percent, megabytes, speed and time left on
a glass progress bar. The download resumes on its own after a dropped
connection: up to six attempts in a row without new bytes (2, 4, 8, 16 and 30 s
apart), 120 s below 64 KiB counts as dropped, and no download runs longer than
six hours. A paused or interrupted download, including one cut off by a restart,
resumes from its part file. A missing package (HTTP 4xx) is not retried. Errors
remain visible and allow retry; a staged update can be discarded. A failed
background check never replaces what the owner last saw. The installed
application continues to run until the user confirms restart.

`status.ini` fields the launcher reads: `state`, `message`, `offer` (the
version on offer, empty once current, applied or discarded), `version`,
`notes`, `size`, `received`, `progress` (of the current step), `rate` (bytes/s),
`eta` (seconds, -1 unknown) and `prepared`.

A trusted update must match the target, base, channel and updater protocol. The
updater rejects expired metadata, older sequences, same-sequence changes,
untrusted signatures, bad hashes, oversized/unsafe ZIPs, insufficient space,
low/unknown battery without external power and active gameplay. TLS uses the
system certificate store; HTTPS redirects cannot downgrade to HTTP.

## Trust and artifacts

The manually installed `rgds/state/update-trust.json` pins the channel/feed and
one or more raw Ed25519 public keys. OTA cannot replace it or `update_guard.py`.
The SD installer preserves an existing trust file. Provision the public trust
file explicitly on existing devices when enabling a channel or rotating keys;
reinstalling does not overwrite pinned trust. New installations take the bundled
trust file. A new manually trusted installer upgrades the stable supervisor protocol.
Multiple pinned keys allow a planned overlap during rotation; a remotely supplied
key is never automatically trusted.

```json
{
  "schema": 1,
  "channel": "stable",
  "feed": "https://YOUR-RELEASE-HOST/stable.json",
  "keys": {"release-2026": "64_HEX_CHARACTERS_OF_ED25519_PUBLIC_KEY"}
}
```

This is a documentation template, not a usable configuration. Before enabling a
public channel, put the real public configuration in `packaging/update-trust.json`
and rebuild the initial installer. Keep the private key outside this repository,
application packages and devices, ideally in a protected signing environment.

`tools/publish_update.py` extracts only the immutable release from a tested
application package into an `.ota.zip`; shared bootstraps and userdata are excluded.
It signs exact UTF-8 metadata bytes using OpenSSL Ed25519. The JSON envelope holds
`key_id`, base64 `signed`, and base64 `signature`. Verification uses stock OpenSSL's
[EVP Ed25519 API](https://docs.openssl.org/3.0/man3/EVP_DigestVerifyInit/), not custom
cryptographic code. The device does not need the OpenSSL CLI or pip dependencies.

The signed payload contains schema, target, base, minimum updater protocol,
channel, version, content-derived build ID, monotonically increasing sequence,
issued/expiry timestamps, HTTPS package URL, compressed/unpacked sizes, SHA-256
and release notes. Every feed replacement, including renewed expiry, needs a
higher sequence. Stable and testing should have separate feeds and trust choices.
A fresh publisher release needs a new immutable build after a failed candidate;
failed builds are not offered again automatically.

`tools/release.py` wraps `tools/publish_update.py`, which can also be used
directly for other hosts: it builds the archive and signed feed and uploads
nothing. Upload the archive before the feed, keep each artifact immutable and
publish a new sequence rather than replacing a package behind a signed URL.

## Activation and automatic rollback

1. A shared install lock serializes OTA and SD installs. The active release is
   untouched during download/staging. Packages are extracted into an inbox,
   fully verified and moved into a versioned release directory.
2. The staged journal records the candidate and exact previous release. On the
   user's restart confirmation, the UI writes an apply request and exits normally.
3. The stable supervisor waits for session/hardware recovery, verifies both
   releases without importing candidate code, writes the trial journal durably,
   then changes previous/active pointers.
4. Each trial gets a fresh random token. The launcher acknowledges only after a
   usable menu frame, library readiness and session initialization. It sends
   startup heartbeats; a stale acknowledgement cannot satisfy a later trial.
5. A 90-second startup deadline and 15-second healthy period protect activation.
   A crash, absent first frame or stopped heartbeat causes termination/recovery,
   restoration of the verified previous pointer and an automatic relaunch.
   The optional Touch to continue screen is skipped during a trial so unattended
   startup validation cannot wait on the user's touch.
6. A trial interrupted by power loss/shutdown is rolled back on the next launch
   before candidate code executes. Successful trials clear the journal and keep
   the previous release for manual rollback.

Both stock APPS and the owned direct-startup hook use the stable supervisor.
Legacy hooks are upgraded by the SD installer while retaining startup preferences.
A second supervisor cannot run concurrently. The original stock launcher remains
the fallback if even the previous release or hardware recovery cannot start.

## Validation and limits

Host tests exercise actual signatures, tampering, expiry/replay, target/protocol
mismatches, resumed transfers, package corruption, staging, low space, active
games, install locking, crashes/hangs, healthy acknowledgement and interrupted
trials. The device passed real-launcher startup/heartbeat and deliberately failed
candidate rollback checks; see [acceptance evidence](archive/OTA_FOUNDATION_2026-10-12.md).
Starting a game during the trial retains the startup heartbeat after SDL closes.
SDL tests exercise update navigation and progress/status layout. Production
publishing is still blocked until a real feed/key is provisioned and accepted.

Rollback covers **application startup**, not future gameplay crashes, arbitrary
userdata migrations, card corruption, kernel or boot-partition updates. Candidate
releases must keep userdata backward-compatible with the previous version. A
future incompatible migration needs its own transactional design. Directory/file
flushes reduce interruption risk, but FAT/card power-cut behavior still needs
hardware fault testing on disposable media. Do not advertise OS-level A/B safety.

## Automatic checks and downloads

`userdata/experience.ini` stores `auto_update`: `0` off, `1` idle checks (default),
`2` idle checks plus download. Checks are persisted and limited to once per six
hours, including after launcher restarts. An empty release feed produces no network
request. Active gameplay and pending update trials suppress background work.
A new release appears in the menu; restart/install remains explicit and uses the
same signature, storage, power and rollback gates as manual updating.
