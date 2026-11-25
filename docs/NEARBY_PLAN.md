# Nearby plan: RG DSes that notice each other

Status (2026-11-25): research, plan and the phase 0 radio probes (section 11). Nothing is built. Section 9 has the
decisions that are still open.

The ask: make the OS feel *alive* together with other RG DSes, and make it easier for them to talk to each other.

Right now, two RG DSes in the same room have no idea the other one is there. Three features each talk on their own,
and each needs the same Wi-Fi router. The plan:

1. One service that always knows **who is nearby** and **what they are doing**.
2. One place where small **things arrive from other people**, even while you weren't paying attention.
3. A way to meet **without a router**.

Everything else is built on top of those three.

## 1. The feel we are after

The research (section 10) comes down to six rules. Every feature in section 6 has to follow at least one of them,
and must not break any.

1. **Things happen without arranging anything.** DQ IX tag mode spread a rare map across Japan in about two weeks,
   just because people carried a closed DS around. Meeting someone should never need both people to open a menu at
   the same moment.
2. **You notice later, calmly.** StreetPass worked with one green light that stayed on until you opened the lid. Our
   version: one LED colour, one soft buzz when the lid opens, and one digest card. Not a toast for every event.
   (A notification study found three batches a day was best; hourly was no better than none.)
3. **What arrives is worth having.** Puzzle pieces, a visitor, a ghost to race, a drawing. A bare "someone was
   here" ping is not enough.
4. **Seeing someone always comes with something to do.** Switch shows "playing X" and offers nothing to do with it.
   Every presence row here gets a one-tap action: wave, visit, join, send.
5. **It still works with two devices, or with one.** StreetPass died of the empty room outside Japan. The owner has
   two RG DSes, plus family and friends, not millions of strangers. Every feature needs a two-device version, and
   collections need a solo way forward (spend playtime instead of encounters).
6. **Peer-to-peer outlives servers.** WiiConnect24, Miiverse and SpotPass are gone; StreetPass still works. Nothing
   here needs our server. An internet relay is optional and self-hostable.

## 2. What already exists

| Piece | Where | Notes |
|---|---|---|
| PictoChat, own app, rooms A-D | `runtime/pictochat_net.py` (UDP broadcast 49320, unprivileged helper, rate limits, strict decoder), `src/launcher/pictochat_app.hpp` | Runs only while the app is open. Its helper model (launcher as root never reads packets) is the template |
| DS local wireless | `dscore/src/net/lan_mp.{h,cpp}`: discovery broadcast 7063, ENet 7064 | Same wire format as melonDS byte for byte, so it should play against melonDS on a PC (untested). Host/Guest/Auto picked by hand in DS controls |
| DS online (WFC) | `dscore/src/net/slirp_driver.*`, applied live in game | |
| mDNS | avahi in the image, `runtime/mdns.py` | |
| Send to phone, planned LocalSend ("Nearby send") | `runtime/album_share.py`, `docs/NETWORK_PLAN.md` §5 | The name "Nearby send" is taken. This plan's service is the layer under it |
| Identity bits | `ds.ini` (DS name, colour, birthday), profiles (Adult/Kid/Little kid, `limits.wireless`) | |
| Things to show | Playtime/Journal, Album, RetroAchievements, Time travel rewind ring, Sound toy clips, haptics (`haptics.hpp`), LEDs | |

## 3. Hardware facts (probed on the RG DS Plus, 2026-11-25)

- **Wi-Fi:** RTL8821CS on mainline `rtw88_8821cs` (kernel 7.2.7).
  - nl80211 reports these interface types: station, AP, ad-hoc, monitor (plus AP_VLAN).
  - There are **no interface combinations**, so the radio can't be on home Wi-Fi and run a hotspot at once. The
    driver registers combinations only for the 8822C.
  - **No P2P** (no Wi-Fi Direct), **no mesh** (removed from rtw88 upstream), **no NAN**.
  - AP mode on SDIO was turned off in 2024 for broken beacons and turned back on in 2025. That makes it new and
    lightly tested.
  - Injected management frames go out at 1 Mbps.
- **Image gaps:** no `iw`, no `hostapd`. wpa_supplicant 2.11 is built without the AP/P2P/mesh options. Kernel has
  `CONFIG_BT_LE=y` but no `CONFIG_BT_BNEP` (no Bluetooth PAN).
- **Bluetooth:** the same chip, Bluetooth 4.2 over UART (HCI version 8, manufacturer 93 = Realtek). That means
  legacy LE advertising only (31 + 31 bytes), 5 advert instances, and LE privacy and static addresses supported. It
  is off until something starts it (`/usr/libexec/rgds/bt`). Measured in section 11.
- **LEDs:** `green:power` (on, no trigger), `red:status` (0/1, free), and `amber:charging` (charger trigger, owned
  by `usr/libexec/rgds/charge-led`). Green and red are free for a "someone came by" signal.
- **Lid:** `gpio-keys-hall`. **No motion sensor** (the only IIO device is the SAR ADC), so there are no
  step-counter coins. Playtime is the solo currency instead.
- **USB-C:** DWC3 with `dr_mode = "peripheral"`. A cable between two RG DSes would need OTG/host on one side plus
  `CDC_NCM` host drivers. It's possible but it's DT work, so it is last.
- **Power, for scale:** idle at Home about 515 mA, deep sleep about 24-37 mA. Wi-Fi connected and idle costs about
  100 mA of that (section 11).

## 4. Architecture

```
                 ┌─────────────── launcher (root) ───────────────┐
                 │ Home presence row · digest card · Visitors    │
                 │ PictoChat · Join · Gifts inbox · Settings     │
                 └───────────────▲───────────────┬───────────────┘
                     events (lines)│               │commands (lines)
                 ┌───────────────┴───────────────▼───────────────┐
                 │ rgds-nearby  (unprivileged, like pictochat_net)│
                 │ identity · presence table · card store · inbox │
                 └──┬──────────────┬──────────────┬──────────────┘
                 LAN UDP+mDNS    BLE adverts     Room (soft AP)
                 (same Wi-Fi)    (no network)    (one RG DS hosts)
```

- **`runtime/nearby.py`** is a long-running helper started by session. It drops root and speaks the same line
  protocol style as `pictochat_net.py`.
  - PictoChat's presence moves into it, so you can see who is in Room B from Home.
  - The launcher still only gets checked, structured payloads.
- **Identity:** one key pair per *profile* (`/data/rgds/nearby/<profile>/id`).
  - The public card holds: DS name, colour, birthday month/day (if shared), avatar, the profile type flag, and a
    greeting.
  - Python has no ed25519 in its standard library. Choice in §9.1.
- **Presence beacon:** a small signed packet every 2-4 s on the LAN and in a BLE advert. It carries:
  - a rotating ID (daily secret + time slot; friends resolve it, strangers can't track it)
  - a status (Home / playing `<system, game hash>` / in PictoChat room N / hosting a Room)
  - "has new card" and "has something for <friend>" bits.

  BLE carries only the 31-byte summary. Cards travel over GATT or IP.
- **Card store:** append-only, de-duplicated by hash, bounded in size. When two devices meet they swap hash lists,
  then the missing items. This is how passes, gifts, drawings and votes travel.
  - Received items go to an inbox. Nothing applies itself.
  - Saves never land on a save: they go through `save_guard` and need a confirm.
- **Transports:** whichever is available, in order: same LAN (UDP broadcast + mDNS `_rgds-nearby._udp`), Room
  (soft AP), BLE. Later, an optional relay or the Tailscale plan.
- **Everything is opt-in per profile.** Kid profiles follow `limits.wireless` (§7).

## 5. Rooms without a router

A **Room** means one RG DS hosts a small Wi-Fi network and the others join it. The host leaves its home Wi-Fi while
it hosts (no STA+AP on this chip).

- **Starting one:** "Host a Room" on Home or in PictoChat, or automatically when Join is pressed and the two devices
  share no LAN.
- **Under the hood:**
  - hostapd, or wpa_supplicant built with AP support, on channel 1, 6 or 11.
  - udhcpd from BusyBox.
  - The SSID `RG DS Room · <name>` and a per-Room passphrase sent in the BLE advert or shown as a QR code. Phones can
    join for LocalSend too.
- Guests join through wpa_supplicant with a temporary network block. Their saved home networks are restored when
  they leave.
- PictoChat, DS local wireless (7063/7064) and Nearby all run on top unchanged, because they are plain UDP and ENet.
- **Both modes work (section 11), and ad-hoc measured better.** An ad-hoc (IBSS) Room between RG DSes had about
  half the tail latency and 1.5x the throughput of a soft AP, and nobody has to host. A soft AP is still needed
  when a phone should join. So: a Room is ad-hoc when only RG DSes are in it, and a soft AP when "Let a phone join"
  is ticked. Ad-hoc is open (no WPA here), so Nearby's own signed and encrypted layer carries anything private.

## 6. Features, in phases

Each phase ends usable on its own. "2" means it works with two devices.

### Phase 0: probes (numbers go into this file)

| Probe | How |
|---|---|
| Interface modes and frame types | add `BR2_PACKAGE_IW`; `iw list` (the nl80211 dump in §3 already shows the modes) |
| Soft AP stability on SDIO | hostapd on channel 6, two handhelds plus a phone, 30 min `ping` + `iperf3`, then 30 min of DS local wireless |
| IBSS | `iw dev wlan0 set type ibss; iw dev wlan0 ibss join rgds 2437` on both |
| BLE | start bt; `btmgmt info`, `btmgmt advinfo`, `btmgmt add-adv`; scan from the other unit; HCI LE Read Local Supported Features |
| BLE while playing | bt-audio A2DP + `bluetoothctl scan le` at a 30 ms/1.5 s duty cycle: listen for dropouts |
| Cost | `current_now` at Home: Wi-Fi only, + BLE adv, + BLE scan, + hosting a Room |
| Sleep window | RTC dark wake every 5 min, 3 s BLE scan, back to sleep: average mA (the freeze seen with opt-in RTC dark wakes on 2026-10-28 must be understood first) |
| DS vs melonDS | DS local wireless between the handheld and melonDS on the Mac |

### Phase 1: seeing each other (2)

- **Nearby row on Home's upper screen.** "Ola · playing Mario Kart DS", "Kari · in PictoChat Room B", "Mamma ·
  on Home". It fades in when someone arrives and adds a short haptic tick.
  - Rows show only for friends (§7). Strangers are counted only: "2 others nearby".
- **One-tap actions** on a row: **Wave** (their device buzzes twice and shows your colour), **Join**, **Send**,
  **Visit** (phase 3).
- **Befriend by bumping.** Both press Befriend within a few seconds of each other on the same LAN, or with the BLE
  RSSI above a threshold (no NFC on this hardware). You compare a 4-emoji code, then swap keys.
  - This is the only way to become friends, and it is how the Kid rules (§7) stay simple.
- **PictoChat lobby shows live counts from Home.** "Someone is in Room A" appears in the digest when you weren't in
  PictoChat.

### Phase 2: playing together (2)

- **Join sets everything up.** Pressing Join on "Ola · playing MKDS":
  1. checks that you have the same game (header hash; else it says which version is needed)
  2. launches it with local wireless already set, with the role picked automatically (presence host = DS host)
  3. applies it live in game, like the online-play fix of 2026-11-24.

  The Host/Guest setting stays as an advanced override.
- **Room hosting** (§5), when there's no shared LAN.
- **Game Boy link over the network.**
  - An mGBA GB SIO driver in Play sends the SB/SC bytes over ENet. Gen 1/2 Pokémon trades and battles work because
    the externally clocked side waits.
  - The link-cable option lives in the in-game menu for GB/GBC, and the same Join flow starts it.
  - GBA multiplayer serial is timing-tight. That comes later: lockstep over a Room, or both machines run on one
    device with remote input.
- **Netplay for the other Play systems.** Rollback using the serialization Play already has for Time travel
  (GekkoNet, BSD-2, or our own). It needs deterministic cores, and is gated per core like the rewind support. The
  same Join flow, so the systems stay consistent with DS.
- **Watch.** A friend's game streamed to your upper screen at a low frame rate ("watching Ola"). It reuses the
  screenshot path. Optional.

### Phase 3: things arrive (2, better with more)

- **Passes.** Two devices in range swap cards in the background (LAN or BLE). The card holds the greeting, avatar
  pose, favourite game, playtime ring, last achievement and a small drawing.
  - Received cards build a **Visitors book** (a Journal tab). Each card has the date and a rough place (only "home
    Wi-Fi name" or "Room", never GPS).
- **The green moment.** A pass that arrives while the lid is closed or the screen is off sets the red/green LED
  pattern ("someone came by"). Opening the lid gives one soft buzz and the digest card: "3 visitors · 1 gift · Ola
  beat your time in Rainbow Road".
- **Visitors walk across Home.** People you passed today stroll along the upper-screen rail as small avatars in
  their DS colour. Tap one (lower screen mirror) to see their card.
- **Shared pixel puzzles.** Panels of pixel art, owner-drawn or drawn in PictoChat, split into pieces. Each device
  carries a few random pieces and passes swap missing ones. A missing piece can also be bought with playtime minutes,
  the solo way forward.
- **Gifts.** Wrap a wallpaper, a Sound toy clip, a PictoChat drawing, a screenshot, a cheat list, or a *copy* of a
  save for a friend. It is delivered the next time you are near, and opened from the inbox with a ribbon animation.
- **Drifting notes.** A PictoChat drawing left "on the table" in a room is delivered to friends who come by later.
- **Room peek.** A read-only snapshot of a friend's shelf (covers, playtime, Journal highlights), browsable after one
  pass even when they're gone, like visiting a dream island.

### Phase 4: alive at a distance and asleep (2+)

- **Tag while asleep.** Probe-gated (phase 0 "Sleep window").
  - A dark wake every few minutes runs a short BLE scan and advert, swaps cards with friends whose bits say "has
    something", then goes back to sleep.
  - It is off by default, with a battery estimate shown beside the switch.
- **Home relay.** A docked or charging RG DS acts as the household's relay: it stores passes and gifts and forwards
  them to the others.
- **Optional internet relay**, small, self-hostable, NetPass-style, for friends in other towns. Or the Tailscale
  phase of NETWORK_PLAN.
- **Weekly drop.** One device marked as the "community host" seeds a small item each week (a puzzle panel, a theme,
  a vote question, a homebrew game) that spreads by passing. Votes are answered by passing; results show as a chart
  once five have come back.
- **Ghosts.** Passes carry best-time records for games both devices own: our own race records from input replays
  over Time travel states where the game allows it. This is research: the in-game ghosts of MKDS live in the game's
  own save, which we never touch.

## 7. Safety and privacy

Swapnote's SpotPass was shut down in 2013 because strangers sent kids images. The rules:

- **Strangers** can only swap structured items: avatar, stamp, puzzle piece, a greeting picked from preset phrases,
  a game *title*. No free text, no drawings, no files.
- **Friends** (befriended by bump, §6 phase 1) may also send drawings, free text and gifts. Kid and Little kid
  profiles:
  - befriend only with an owner-profile OK
  - with `limits.wireless` off: no Nearby at all
  - Little kid: friends only, stamps and Wave only.
- **Block and forget** from any card. Blocked keys are dropped by the helper before the launcher sees anything.
- **Logs stay on the card.** There is no place data beyond "where you were connected". Rotating IDs keep strangers
  from tracking a device. Visibility is per profile: Off / Friends / Everyone.
- **Abuse limits:** rate limits and the strict decoder as in `pictochat_net.py`, bounded stores, signed cards, and a
  size cap per item kind.

## 8. Not doing, and why

- **Real DS/DSi PictoChat or multi-card play over the air.** DS multiplay acknowledgements are timed in microseconds
  by the DS radio. Injecting over SDIO on Linux is millisecond-jittery. GBATEK and the unfinished PCPA project
  confirm what is needed.
  - Possible later as a toy: a monitor-mode **sniffer** that shows real DSes nearby ("a DS is in PictoChat on
    channel 7"). Download Play to FlashMe units only.
  - Our PictoChat stays our own app (as in the PictoChat note).
- **Wi-Fi Direct / mesh:** the driver has neither.
- **USB-C cable link:** needs DT role switching and host-side drivers. It's last, if ever.
- **Passing game files:** only homebrew that the owner marks as shareable.

## 9. Decisions for the owner

1. **Signing:** vendor Monocypher into a tiny C helper, a pure-Python ed25519 (slow but fine at a few cards a
   minute), or the `openssl` CLI that is already in the image.
2. **AP stack:** hostapd 2.11 (1.2 MB stripped, proven on the handhelds in section 11) or wpa_supplicant with
   `CONFIG_AP` (one less daemon, untested).
3. **Nearby on at boot:** on with Visibility=Friends, or off until the setup wizard asks?
4. **Names in the UI:** "Nearby", "Visitors", "Passes", "Rooms", "Wave". Avoid Nintendo's trademarks
   (StreetPass/SpotPass).
5. **LED for "someone came by":** green blink or red. The amber one belongs to the charger.
6. **Phase order:** phases 1 and 2 first (seeing and joining), or the passes of phase 3 first (the most "alive"
   but needs more density)?

## 10. Research notes and sources

- DQ IX tag mode, Akihabara crowds, the map that spread in two weeks:
  https://www.siliconera.com/the-social-impact-of-dragon-quest-ixs-tag-mode-in-japan/
- StreetPass/SpotPass, green and blue LED, Relay Points (ended 2018-03-28), SpotPass end 2024-04-08:
  https://en.wikipedia.org/wiki/SpotPass_and_StreetPass
- Kids cheering at the green light:
  https://www.nintendolife.com/news/2013/12/parent_trap_streetpass_makes_social_networks_social_again
- Empty room: zero hits in New York in 2021:
  https://www.nintendolife.com/news/2021/07/random_an_eager_3ds_streetpass_fan_spent_a_day_in_new_york_got_zero_hits
- NetPass (internet StreetPass, 2024):
  https://www.timeextension.com/news/2024/06/streetpass-fans-take-note-netpass-resurrects-one-of-the-3ds-best-features
- Play Coins (100 steps = 1, 10 a day):
  https://www.nintendo.com/en-gb/Hardware/Nintendo-3DS-Family/Instant-Software/Activity-Log/Activity-Log-115580.html
- Mii Parade: https://www.nintendo.co.uk/Support/Wii/Wii-Channels/Mii-Channel/Mii-Channel-Mii-Parade/Mii-Channel-Mii-Parade-242327.html
- Tamagotchi Connection visits: https://web-japan.org/Kidsweb/archives/cool/05-06/cool050601.html
- Dream islands: https://en-americas-support.nintendo.com/app/answers/detail/a_id/50094
- Dark Souls messages and summons: https://gamesradar.com/elden-ring-and-dark-souls-summoning-system-was-inspired-by-miyazakis-car-breaking-down
- Playdate seasons: https://techcrunch.com/2019/05/22/panics-playmate-is-a-pint-sized-gaming-machine-with-a-season-of-12-intriguing-titles
- Spotify Jam: https://techcrunch.com/2023/09/26/spotify-launches-jam-a-real-time-collaborative-playlist-controlled-by-up-to-32-people
- Swapnote SpotPass pulled: https://www.gamespot.com/articles/nintendo-pulls-swapnote-spotpass-feature-after-being-actively-misused/1100-6415907/
- WiiConnect24 shutdown, "very low participation rate": https://en-americas-support.nintendo.com/app/answers/detail/a_id/3512
- Notification batching study: https://communities.springernature.com/posts/batching-notifications-can-improve-attention-well-being-and-productivity
- Calm technology: https://calmtech.com/papers/designing-calm-technology
- Whereabouts Clock: https://www.microsoft.com/en-us/research/?p=154324
- rtw88 AP/ad-hoc on SDIO again (2025): https://git.zx2c4.com/linux/log/drivers/net/wireless/realtek/rtw88/main.c
- DS multiplay timing: https://problemkaputt.de/gbatek-ds-wifi-multiplay-master.htm,
  https://problemkaputt.de/gbatek-ds-wifi-ieee802-11-frames.htm, https://github.com/thesola10/pictochat
- Exposure Notification rotating IDs: https://covid19.apple.com/contacttracing
- Gambatte network link (Pokémon trades): https://forums.libretro.com/t/gambatte-game-boy-multiplayer/19553
- mGBA has no network link: https://github.com/mgba-emu/mgba/issues/2379
- GekkoNet rollback: https://github.com/HeatXD/GekkoNet

## 11. Phase 0 results (2026-11-25, both handhelds)

Run on the RG DS and the RG DS Plus side by side on a desk, both on 0.9.2, kernel 7.2.7. The tools ran from
`/run/rgds/probe` and were removed afterwards:
- hostapd 2.11 cross-built in the build VM (tarball sha256 matches Buildroot's hash)
- `btmgmt` from our bluez5_utils build, which compiles it but doesn't install it
- a Python link test.

The raw log is `/data/rgds/logs/nearby-probe.txt` on each handheld. Each Wi-Fi run restored the normal Wi-Fi by
itself (`net wifi`) and got its home address back within about 7 s.

**Wi-Fi link** (500 UDP round trips at 20 ms intervals, 8 s TCP, 5 broadcasts per second):

| Mode | Join | RTT p50 | p95 | p99 | max | Lost | TCP | Broadcasts seen |
|---|---|---|---|---|---|---|---|---|
| Soft AP, run 1 (Plus hosts, WPA2, ch 6, 802.11n) | associated 3 s, DHCP address at 6 s | 3.1 ms | 13.4 | 18.2 | 116 | 0/500 | 17 Mbit/s | not measured (test bug) |
| Soft AP, run 2 | 3 s, address at 4 s | 2.7 ms | 9.8 | 17.3 | 77 | 0/500 | 31 Mbit/s | 76/78 |
| Ad-hoc / IBSS (open, 2437 MHz, plain wpa_supplicant `mode=1`) | about 8-11 s to merge | 2.0 ms | 5.3 | 7.3 | 20 | 0/500 | 46 Mbit/s | 68/74 |

- hostapd starts in about 3 s on rtw88 SDIO and held its one station for the whole run, with no deauths. The
  station shows `[SHORT_PREAMBLE][WMM][HT]`.
- IBSS works with the system wpa_supplicant as it is (`ap_scan=2`, `mode=1`). It prints a warning that nl80211
  is "not designed" for `ap_scan=2`, but both units merged into one BSSID and stayed there.
- Either mode is easily good enough for DS local wireless and Game Boy link play. Broadcast delivery was around
  92-97%, so presence beacons should repeat, as PictoChat's already do every 2 s.

**Bluetooth LE:**
- Both units advertise and scan with `btmgmt`. A 23-byte advert (flags + 16-bit UUID + service data) from one
  unit was found by the other within a 12 s LE discovery window, among 15-18 other adverts in the house.
- BlueZ sent it from a random address, so addresses rotate for free.
- RSSI side by side was -22 to -24 dBm. Phones and other gear in the same room were at -44 to -70 dBm, so
  "hold the two together" (bump to befriend) can be told apart by RSSI.
- `btmgmt find` prints only the advert length, not its contents. The real helper has to read service data
  through BlueZ's D-Bus API, or open a raw HCI socket (the image's Python has no `AF_BLUETOOTH`).
- Gotcha: `btmgmt` hangs after its command when stdin is not a terminal. Run it under `timeout`, or don't use it.

**Battery cost** (RG DS on battery, Mario Kart DS open in the background, backlights 333/333, mean `current_avg`
over 30 s):

| State | mA |
|---|---|
| Wi-Fi connected, BT off | 452 (first run), 411 (second run) |
| + BT on, idle | 449 / 419 |
| + BT advertising | 417 |
| + BT LE scanning continuously | 434 |
| Wi-Fi and BT both off | 312 |

The baseline drifted about 40 mA between the two runs, so treat anything under about 10 mA as noise.

- **BT on and advertising costs next to nothing.** Continuous scanning costs about 15-25 mA, and a duty-cycled
  scan would cost less. BLE presence while awake is affordable.
- **Wi-Fi connected and idle costs about 100 mA**, about a quarter of the menu's draw. The kernel has
  `CONFIG_CFG80211_DEFAULT_PS` off, so Wi-Fi power save is likely off by default (the nl80211 query returned
  EINVAL, so this is unconfirmed).
  - This is a battery win for everyone, separate from Nearby: try `iw dev wlan0 set power_save on` and measure
    again.
  - Watch broadcast delivery under power save, because broadcasts wait for the DTIM.
- Hosting a soft AP and IBSS idle could not be measured on battery, because both units were on cables during
  those runs.

**Not run:** DS local wireless against melonDS on the Mac (melonDS is not installed, and it needs a person at both
ends).
