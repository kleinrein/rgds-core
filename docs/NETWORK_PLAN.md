# Network life plan

Status (2026-11-22): phases 1-3 built and host-tested (sections 11-13). Decisions 8.2 and 8.3 taken (OpenSSH, openssl);
the rest of section 8 is open. Device probes are phase 0.

The ask: RomM / home-server sync, SMB and SFTP fetch, a file manager, LocalSend, Tailscale. These are not five
separate apps. They are one idea: **the handheld can reach files that live somewhere else, and move them to the
right place without a computer.** So the plan builds one shared layer (remote sources + a transfer queue), puts a
file manager on top of it, and adds the five sources one at a time.

## 1. What already exists

| Piece | Where | Reuse |
|---|---|---|
| Samba 4.25 bundle with `smbclient` already built | `tools/build_samba.py` (TARGETS includes `client/smbclient`) | SMB client needs no new binary |
| SMB *server* sharing, USB-C NCM network (10.55.0.1), Bonjour via avahi | `runtime/file_sharing.py`, `usr/libexec/rgds/usb` | Unchanged; the file manager is the on-device half |
| Python 3 with `ssl`, `sqlite`, `zlib`, `xz`, `bz2` | defconfig | HTTPS clients (RomM, Tailscale auth URL) in plain Python |
| Resumable HTTP downloads with `Range` and verified sizes | `runtime/ota.py` (`fetch`, ~l.298-361) | Transfer queue copies this logic |
| QR encoder + phone web page with code pairing and resumable PUTs | `runtime/qr.py`, `runtime/port_files.py` | Typing server addresses/passwords on the phone instead of the pad |
| "Send to phone" HTTP server for screenshots | `runtime/album_share.py` | LocalSend becomes a second target beside it |
| Secret storage pattern | ScreenScraper password in `private/` | Same for SMB/SFTP/RomM credentials |
| `save_guard`, never-touch-owner-saves rule | `runtime/save_guard.py` | Every save download goes through it |
| Kernel: `CONFIG_TUN=m`, `CONFIG_WIREGUARD=m`, `CONFIG_IP_NF_IPTABLES=y`, `CONFIG_FUSE_FS=y`; `CONFIG_CIFS` off | `os/buildroot/board/rgds/linux-*.config` | Tailscale can use the kernel TUN; SMB stays user space (no mounts) |
| Buildroot ships `tailscale` 1.78.1 and `openssh` with a client-only option | `.cache/own-os/buildroot-ref/package` | No hand-made packages needed |
| Dropbear (SSH server, no SFTP) | defconfig | SFTP *server* needs OpenSSH's `sftp-server`; client needs `sftp` or Python |

Constraints that shape the design:

- `/data` is exFAT: no Unix permissions, no symlinks, case-insensitive. "chmod 600" on a secrets file means nothing;
  secrets are protected only by the card being in the owner's hand. Say so in the UI, offer per-source "don't
  remember password".
- Rootfs is a read-only squashfs; anything that wants to write `/etc` (Tailscale MagicDNS → `resolv.conf`) must be
  pointed at `/run`.
- The menu frame is expensive (~37 ms, `project-menu-speed`). All network work runs in a helper process; the
  launcher only polls a status file, like OTA does.
- OTA slot size: the image is already near its cap after the 3DS engine. Tailscale (~25-30 MB unpacked) is the only
  big item here (decision 8.1).

## 2. Shared layer: sources and the transfer queue

**`runtime/net_sources.py`** — one interface, several backends:

```
list(path) -> [Entry(name, is_dir, size, mtime)]
stat(path)
read(path, offset) -> stream        # resumable download
write(path, stream)                 # uploads (save sync, send)
```

Backends: `local`, `smb` (drives the bundled `smbclient` in `-c` batch mode, parses `ls` output; no kernel mount),
`sftp` (OpenSSH `sftp -b` batch, or `dbclient` + remote `sftp-server`; decision 8.2), `romm` (HTTPS JSON),
`localsend` (receive-only inbox). Sources are rows in `/data/rgds/network/sources.ini`; passwords live in
`/data/rgds/private/network.ini`.

**Adding a source** has two ways in, both already proven elsewhere:

1. On the handheld: host, share/path, user, password on the on-screen keyboard. SMB and RomM hosts are found
   automatically by mDNS (`_smb._tcp`, `_sftp-ssh._tcp`, `_ssh._tcp`, `_http._tcp` named "RomM") through the avahi
   daemon that is already running, so most people pick from a list and only type the password.
2. "Set up on your phone": QR on the top screen, the phone page (port_files pattern) has real keyboards and
   paste. Strongly preferred for passwords and Tailscale auth keys.

**`runtime/transfers.py`** — one queue for every copy, local or remote:

- Jobs in `/data/rgds/network/transfers.tsv`, a status file in `/run/rgds/transfers` the launcher polls.
- Resumable: partial files are `name.part` beside the target, resumed by offset (SMB `reget`, SFTP `reget`, HTTP
  `Range`). Survives sleep, Wi-Fi loss and a reboot.
- Verifies size (and hash when the source has one: RomM gives sha1/crc).
- **Lands files in the right place**: a ROM goes to the system folder its extension or the RomM platform says,
  `.zip`/`.7z` are peeked to decide, ambiguous ones (e.g. `.bin`) ask once. Then a library rescan of that system
  only. This is the feature people actually want from "SMB fetch".
- Status-bar glyph + Home banner while something is moving, same style as the update glitter bar.
- Keeps Wi-Fi awake and stops the idle sleep timer while a job runs; lid close pauses it (true sleep keeps 21-25 mA
  only if Wi-Fi is down).

## 3. File manager

A launcher screen (Apps → Files; decision 8.4), dual-screen like the rest:

- **Lower screen**: the list. Touch scroll with the kinetics helpers, D-pad, L/R jump between roots, Y marks,
  key_hints badges everywhere. Roots: Games (`/data/roms`), Saves, Screenshots, Music, Videos/Books (when
  MEDIA_PLAN lands), Downloads/Inbox, the whole card, and every network source.
- **Upper screen**: what the selected thing *is*. A ROM shows its library card (box art, title, system, playtime);
  an image shows itself; a zip lists its contents; text shows its first lines; a folder shows count and size;
  a save shows which game owns it and when it was last written.
- **Actions (A / X menu)**: Open (launch the game in its system, show an image in the Album viewer, play music),
  Copy, Move, Rename, New folder, Delete, Extract archive, Send (LocalSend / phone), Add to library.
  Copy/Move between any two roots, so local ↔ SMB/SFTP is the same gesture as local ↔ local, all through the queue.
- **Safety**: delete goes to `/data/.rgds-trash` (emptied after 30 days or on low space, with a row to empty it now);
  saves and `userdata` need a second confirm and pass through `save_guard`; the OS and boot partitions are not
  listed at all. Kid profiles get no file manager, or a read-only one (PROFILES_PLAN types).
- exFAT rules shown up front: names with `: * ? " < > |` are rejected with a reason, not a failed copy.

## 4. SMB and SFTP fetch

Mostly falls out of sections 2 and 3: a network source is a root in the file manager.

- **SMB**: `smbclient` from the bundle, SMB2/3 only (server is built `--without-smb1-server`; client minimum
  protocol SMB2_02). Guest and user/password. Anonymous share listing for the "pick a share" step.
- **SFTP**: password or key. The handheld generates an ed25519 key on first use and shows the public key as a QR /
  on the phone page so it can be pasted into `authorized_keys`. Host keys pinned on first connect (TOFU) with a
  clear "this server changed" stop.
- Also serve SFTP *from* the handheld (OpenSSH `sftp-server` behind Dropbear) so Cyberduck/WinSCP/Android file apps
  work over Wi-Fi, USB and Tailscale. Small; same package as the client.
- Speed target: Wi-Fi up measured 2.2 MB/s with a 3DS run loading the CPUs (`project-usb-gadget`); idle numbers
  are phase 0. If SMB is slow, `smbclient` `-O` socket options and `max xmit` before anything else.

## 5. LocalSend

LocalSend is the cross-platform AirDrop people already have on phones and PCs. Its protocol (v2) is open: UDP
multicast announce on 224.0.0.167:53317, then a small REST API on port 53317 (`/api/localsend/v2/register`,
`/prepare-upload`, `/upload`, `/cancel`). Plain Python, no new binary.

- **Receive**: while Files or the Album is open, or for 10 minutes after "Receive" on Home, the handheld appears as
  "RG DS (name)" in LocalSend. An incoming offer shows on the top screen (who, what, size) with A accept / B decline;
  files land in Inbox, and ROMs offer "Add to library" which goes through the same placement rules.
- **Send**: file manager and Album "Send" list nearby LocalSend devices.
- Security: always ask before accepting; optional PIN (the protocol has one). HTTPS needs a self-signed cert;
  Python's `ssl` can serve one but cannot *make* one, so either ship `openssl` (decision 8.3) or start with the
  protocol's HTTP mode. Check whether current iOS/Android LocalSend still talk to an HTTP-only peer (phase 0).
- Naming: we speak the LocalSend protocol; the UI says "LocalSend" only as "works with the LocalSend app", the
  feature is "Nearby send".
- Risk: rtw88 and multicast in power-save mode. Phase 0 checks that a phone sees our announce at all.

## 6. RomM and home-server sync

RomM is a self-hosted ROM manager with a REST API (FastAPI; the server serves its own OpenAPI at `/openapi.json`,
and the client must be checked against it at build time, not written from memory).

1. **Browse + fetch**: RomM appears as a source whose tree is Platform → Game, with RomM's cover art and metadata on
   the upper screen. A on a game downloads it into the right system folder (RomM platform slug → our system id
   table; unknown platforms are listed but greyed). Multi-file games (`.cue`+`.bin`, `.m3u` sets) come as RomM's
   zip and are unpacked. Shows "on the handheld" vs "on the server" per game.
2. **Save sync**: on game exit, upload the save (and optionally the last named state) to RomM against that ROM;
   on launch, if the server has a newer one, ask "Use the save from <device> (yesterday 21:40)?" — never replace
   silently. The replaced local save is kept beside it via `save_guard`. DS saves live in
   `userdata/emulators/rgds-experimental/saves/` (not next to the ROM); the mapping table handles every system.
3. **Plain home-server sync** for people without RomM: the same save-sync rules against a folder on an SMB or SFTP
   source (`<share>/rgds-saves/<system>/<game>.sav` + a small index file with mtime and device name). This is the
   "home-server" half that needs no server software at all.
4. Optional later: push playtime and RetroAchievements-style "last played" to RomM if its API has a place for it.

Conflict rule (decision 8.5): newest write wins *only after asking*; two devices playing the same game offline
produce two saves, both kept, the owner picks.

## 7. Tailscale

Makes everything above work from outside the house: RomM, SMB and SFTP at home reach the handheld on a train, and
the owner can `ssh` to it for development without the USB cable.

- Buildroot's `tailscale` package (1.78.1 in our reference tree; bump to current). Kernel TUN module is already
  built; WireGuard in-kernel is not used by tailscaled. Shrink with the `ts_omit_*` build tags and the combined
  `tailscaled`+CLI binary.
- **Sign-in**: `tailscale up` prints an auth URL; we show it as a QR on the top screen (`runtime/qr.py`) and the
  phone finishes the login. Auth keys can be pasted on the phone page. A **custom login server** row supports
  Headscale, which the self-hosting crowd that runs RomM often uses.
- State in `/data/rgds/tailscale/`, DNS written to `/run` (rootfs is read-only), `--accept-dns` off by default so
  the handheld's own DNS keeps working; MagicDNS names resolved in our own clients through `tailscale ip`/the local
  API as a fallback.
- Off by default; Settings → Connectivity → Tailscale (on/off, account, this device's name and 100.x address).
  Stopped on suspend and restarted on wake, so true sleep current is unchanged (measure in phase 0).
- Exposes only what is already on: SMB sharing, SSH, SFTP, LocalSend never go over the tailnet unless their own row
  says "also over Tailscale".

## 8. Decisions for the owner

1. **Tailscale in the image or downloaded on demand** to `/data/rgds/tailscale/bin` (exFAT is mounted
   `fmask=0000`, so it can execute). In the image is simpler and OTA-updated; on demand keeps the slot small.
2. **Decided 2026-11-21: OpenSSH.** SFTP via OpenSSH (`BR2_PACKAGE_OPENSSH_CLIENT` + `sftp-server`, ~1 MB) or a pure-Python client (no new
   package, but SSH crypto in Python means `cryptography`, which is far bigger). Recommend OpenSSH.
3. **Decided 2026-11-21: yes.** `openssl` CLI in the image for LocalSend HTTPS and SFTP-key QR fingerprints (~1 MB), or HTTP-only LocalSend.
4. **Where Files lives**: Home tile, Apps entry, or both. And whether Kid profiles see it. For now (phase 2) it is the
   last row of Settings > Library, after Album; Kid profiles are not yet kept out.
5. **Save-sync conflict policy**: always ask (recommended) vs newest-wins with backup.
6. **RomM version floor**: target the current RomM release only, or also the previous major.

## 9. Phases

| # | What | Done when |
|---|---|---|
| 0 | Device probes: SMB/SFTP throughput idle and under load over Wi-Fi and USB; LocalSend phone sees a test announce; HTTP-only LocalSend from iOS and Android; tailscaled size, idle wakeups and sleep current; `resolv.conf` path on the image; mDNS browse through avahi | Numbers written into this file |
| 1 | `net_sources.py` (local + smb), `transfers.py` queue with resume, placement rules, status file; tests with a fake smbclient | Built 2026-11-21, host tests green, no UI (section 11) |
| 2 | File manager, local roots only, trash, save confirms, upper-screen previews | Built 2026-11-22, host preview + UI tests (section 12); not on the handheld |
| 3 | SMB + SFTP roots in Files, mDNS discovery, phone setup page, "Add to library" | Built 2026-11-22 except the phone page (section 13); host only |
| 4 | LocalSend receive, then send (Files + Album) | Phone → handheld and back with the real app |
| 5 | RomM browse/fetch, then save sync; plain SMB/SFTP save sync | Real RomM server in Docker on the Mac, round trip of a save between two profiles |
| 6 | Tailscale (+ Headscale login server), "also over Tailscale" rows | RomM fetch and `ssh` from outside the LAN |

Phases 4 and 6 do not depend on 2-3 and can run in parallel sessions; 5 needs 1.

## 10. Testing

- Host: real `smbd` from the bundle on a temp port, OpenSSH `sshd` on a temp port with a temp key, a RomM
  container (or a recorded fixture from its OpenAPI), a LocalSend fake peer in Python on loopback multicast.
- Device: copies of `userdata`, never the owner's saves (`feedback-never-touch-owner-saves`). Panels only when the
  owner is not playing.
- Fault cases: Wi-Fi drop mid-file, lid close mid-file, card full, server password changed, host key changed,
  name illegal on exFAT, two devices uploading the same save.

## 11. Phase 1 as built (2026-11-21)

- `runtime/net_sources.py`: `LocalSource`, `SmbSource` (bundled `smbclient`, `-s` a throwaway smb.conf pinned to
  SMB2_02..SMB3, `-A` a 0600 login file so the password is never in argv, `-D` for the folder, `get`/`reget`).
  `sources.ini` / `private/network.ini` on the device root (not per profile). Names with `"` `;` or a newline are
  refused (smbclient `-c` cannot quote them). NT_STATUS codes map to owner messages and a `transient` flag.
- `runtime/transfers.py`: queue in `network/transfers.json` under a flock; `add` stats the file now (typos and wrong
  passwords show at once); jobs are a file or a whole folder (disc sets, DOS/ScummVM games). Staging in
  `network/incoming/<job>/<source parent folder>/` so `system_of` still sees "Genesis" for a plain `.bin`. Retries
  transient errors 3 times with `reget`; a job left `running` by a power cut resumes. Space check keeps 64 MiB spare.
  Placement: `/data/roms/<system>` (gg/sg on `sms`, the session's `--system-dir` folders), unknowns to
  `/data/inbox`, same-size duplicates skipped, others get " (2)". Status fields in `/run/rgds/transfers` with a
  `serial` that grows on every placed job, for the launcher to call `rescan_games`.
- CLI: `transfers.py --data D add SOURCE PATH [--to DIR] | list | cancel ID | retry ID | clear | run`.
- Tests: `tests/test_transfers.py` (20) against `tests/fixtures/fake_smbclient.py`.

Not verified: the real `smbclient` 4.25 output and exit codes (the fake copies its documented `ls` and NT_STATUS
lines; the first device run must check `ls` parsing, `reget`, `-D` with nested folders and `-L -g`), throughput,
anything on the handheld. Nothing starts the runner yet: the launcher (phase 3) will spawn `transfers.py run` like
other workers. `.7z` with no games folder in its path goes to the inbox (Python cannot look inside).

## 12. Phase 2 as built (2026-11-22)

- **Where**: Settings > Library > Files, the last row (library row id 15; Album and RetroAchievements moved one up for
  Back). Deep link `--settings --section files`.
- **Roots** (L/R, shoulder tabs name the neighbours): Games `/data/roms`, Inbox `/data/inbox`, Screenshots (the active
  player's album), Music (first `--music-dir`), Card `/data`, Trash (`<data>/trash`). `RGDS_FILES_ROOTS="Name=path;..."`
  replaces all but Trash (host previews, tests).
- **Lower screen**: folders first, then files, by name ignoring case; dot files and `.part` hidden; 5000 rows at most.
  A opens a folder or the menu on a file, B goes up (clears marks first; at the root back to Library, and rescans the
  library if anything changed), X marks, Y menu: Paste/Move here, Copy, Move, Rename, Delete (Delete for good in the
  Trash), Extract here (zip), New folder, Clear marks, Cancel paste, Empty trash. Rename/New folder use the Keyboard
  page (edit fields 14 and 15).
- **Upper screen**: place (root / path), the thing's art (the library's game art when the file is a known game, the
  picture for .png/.bmp, an icon otherwise), name, size and date or item count, marks, and a zip listing or a text's
  first lines from `files.py peek`. During a command: what it is doing, the current file and a progress bar.
- **runtime/files.py** owns the card changes and the confirm rules: anything in the system folder (`<data>`, where
  saves and settings live) and any save file or folder holding saves stops with `state=confirm`; the page asks and
  re-runs with `--confirmed`. Delete is a rename into `trash/<time>/` with `.from.tsv`; batches older than 30 days go
  on the next delete. Copies go through `.part`; moves on one card are renames; zips unpack into a new folder beside
  them with zip-slip refused; names exFAT cannot store are refused with the reason.
- Tests: `tests/test_files.py` (15), `tests/test_files_ui.py` (5, keys against the real helper).

Not done or not verified: the handheld; touch beyond the generic list taps; opening a game from Files (A shows the
menu); restore from the trash; 7z; Kid profiles; image preview of PNGs the album's decoder cannot read (it reads 8-bit
RGB/RGBA, non-interlaced) and JPEG (none).

## 13. Phase 3 as built (2026-11-22)

- **SFTP** (`SftpSource`): OpenSSH `sftp -b` batch with `-F /dev/null`, `ls -la` parsed, `get -a` resumes. Signs in
  with the handheld's own ed25519 key (`rgds/private/ssh/id_ed25519`, made by `ssh-keygen` on first use) or a password
  through `SSH_ASKPASS` + `SSH_ASKPASS_REQUIRE=force` (a 0600 file, never argv). The key is copied to a 0600 file in
  the work folder per command, because exFAT shows it as 0777 and ssh refuses it. Work folder: `/run/rgds/net` on the
  handheld (RAM), the temp dir elsewhere; the transfer queue uses it too now. Server keys: trust on first use in
  `rgds/network/known_hosts`; a changed key stops with "not the server it was before".
- **Discovery** (`runtime/mdns.py`): one legacy-unicast PTR query for `_smb._tcp`, `_sftp-ssh._tcp`, `_ssh._tcp`, asked
  twice in 1.5 s; PTR -> SRV -> A resolved with name compression; one entry per SSH host. Checked on the owner's LAN
  against a real responder (the Mac's AirPlay entry); no SMB/SSH server was advertising there that day.
  `RGDS_MDNS_TARGET=host:port` points it at one responder (tests).
- **net_sources.py CLI** for the launcher (`--out` file, first line `ok` or `error<TAB>message`): `sources`, `ls`,
  `test`, `add` (password on stdin), `remove`, `discover`, `key`, `shares`. `transfers.py add` takes several paths,
  `--run` works the queue after adding (waits up to 3 s for a runner that is just leaving) and `--answer` reports.
- **Files**: each place in `network/sources.ini` is a root after the card's own (a "Network" root explains how to add
  one when there is none). Folders are listed by `net_sources.py ls` off the frame loop. Y on a place: Add to library,
  Download to Inbox, Copy (then Paste here in any folder on the card), Refresh, Add network place, Remove this place.
  Copies run in their own session (`setsid`) through the queue, with a strip on the upper screen (name, how far, how
  many more, or "N downloads stopped"); a placed game marks the library for a rescan.
- **Add network place**: SMB or SFTP -> servers found (name and address) or "Type an address" (`host:port` works)
  -> user (empty = guest on SMB) -> SMB password and a share picked from the server's list, or for SFTP a password or
  "This handheld's key" (the upper screen shows the key to put in `authorized_keys`; A continues) and a folder ->
  added and tested; a failed test offers "Keep it anyway" / "Remove it". Keyboard fields 16-19 (18 is secret).
- Tests: `tests/test_sftp_source.py` (7, real sshd on loopback), `tests/test_mdns.py` (9),
  `tests/test_files_network_ui.py` (4, keys; one adds an SFTP place against a real sshd), transfers +1.

Not done: the phone setup page (QR + web form) - typing on the handheld works for now; the image config is in `rgds_os_defconfig` since 2026-11-22
(`BR2_PACKAGE_OPENSSH`, `BR2_PACKAGE_LIBOPENSSL_BIN`) but no image has been built with it yet (0.9.1-f7f88d06601c
has the Files page without them); uploads to a place; password
SFTP is untested (macOS sshd without root has no password login) and SMB is still only tested against the fake
smbclient; nothing on the handheld.
