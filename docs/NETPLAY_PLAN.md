# Two-console netplay for DS local wireless

Status, 2026-11-26: phase 1 done (TwinMp, `dscore/src/net/twin_mp.cpp`, headless `--twin PATH --twin-role host|guest`).
Phase 2 done (`dscore/src/net/input_link.cpp`, headless `--input-send HOST:PORT`, `--input-recv PORT`,
`--input-delay D`). Phases 3-4 not started.

## Why

DS local wireless today is lockstep over the LAN (melonDS-style, `dscore/src/net/lan_mp.cpp`). Every multiplayer
exchange the game makes waits for a round trip between the handhelds. Mario Kart DS makes about 2.4 a frame in a
race, so the round trip sits inside every frame.

Measured in a real two-handheld race on 2026-11-26, after the Wi-Fi timer fix (below):

| | RG DS (host) | RG DS Plus (guest) |
|---|---|---|
| Race | 47-52 fps | 42-52 fps |
| Waiting on the network | 7-8.6 ms a frame | 7.5-9.3 ms a frame |
| Menus before the race | 60 fps, 0 ms | 59 fps, 0.1 ms |

The ping between the two averages 5.0 ms through the home router (2.6 min, 18 max); the spikes are the stutters.
A better router or the same Wi-Fi band shaves some of this, but the lag is still set by the network, and it only
works on one shared LAN.

## The idea

Each handheld runs **both** consoles: its own, shown on its screens, and the other player's, hidden. The two
consoles on one handheld talk local wireless to each other through shared memory, with no network in between. Only
the buttons and touches cross the network, a few bytes a frame, sent a few frames ahead (input delay), so a 5 ms
or even 50 ms round trip no longer stalls a frame. Both handhelds compute the same thing from the same inputs, so
the hidden console on one is the visible console on the other.

This also opens the door to playing over the internet (a relay server for the inputs), which lockstep can't do.

## Measured feasibility (RG DS Plus, MKDS, headless, 2026-11-26)

- **Cost of the hidden console.** With drawing skipped (`--frameskip`, which still draws frames the game reads
  back), a hidden MKDS console takes **5.6 ms a frame** mean, 9.3 ms p95, 11.7 ms p99, one core. A drawn one (CPU
  3D, one core) takes 13.1 ms. Both at once on separate cores: 13.5 and 5.8 ms, about 3% slower each. Four cores
  leave room for the GPU and present threads.
- **Skipping drawing does not change the game.** A run drawing every 10th frame matched a fully drawn run on all
  300 of the frames it drew (3000 frames).
- **Run to run.** Two runs: identical hashes on all 3000 frames.
- **Across handhelds.** RG DS and Plus: byte-identical frame hashes for 3000 frames (same engine binary).

The Wi-Fi timer fix that made this cheap: the 8 us Wi-Fi tick was a scheduler event each (125,000 a second, each
ending the JIT's run) and cost ~30% of a MKDS frame. `Wifi::us_timer(ticks)` now runs 4 ticks per event while the
radio is quiet (`radio_quiet()`), 8.2 vs 7.9 ms with the radio off.

## Design

### 1. The twin link (the hard part)

The two consoles on one handheld are two engine processes (the JIT runtime `g_rt` is global, so not two consoles in
one process). Their wireless exchange must come out identical on both handhelds whatever the scheduling, so it is
synchronised by **game time**, never wall time:

- `TwinMp` (`dscore/src/net/twin_mp.cpp`), an `MpTransport` over a file-backed shared block: a message ring each
  way and each side's published clock. Both consoles boot together, so their scheduler clocks are one timeline.
- A message sent at game time s reaches the other console at s + 512 us (the lead).
- A console at time c answers "nothing yet" only once the other's clock has passed c - lead: nothing older can
  still come. Otherwise it waits (a spin). Two consoles can never both wait, since each waits only for the other to
  be within the lead of it.
- The host, as its CMD goes out, promises to send nothing until the reply window ends, and collects replies until
  they are in or the other's clock passes the window: what the hardware does, decided in game time.
- Otherwise the queue rules are LanMp's, so the DS side behaves as over the LAN. No core change, nothing reads a
  clock. The frontend publishes the clock after every frame, so a console with its radio off holds the other up
  by a frame at most.

### 2. The input session (between handhelds)

- One handheld hosts, the other joins (the existing Play together page and discovery).
- **Start from the same place.** Before the start, the host sends the guest everything that shapes a console: both
  battery saves, both firmware user blocks (names, birthday), the config that changes emulation, and one clock
  start time for both RTCs. Each handheld then boots both consoles from that.
- **Inputs.** Every frame each handheld sends its pad and touch for frame n + D (D frames of input delay, about 3 to
  start: 50 ms, enough for a 5-30 ms round trip). A console runs frame n only when it has both players' inputs for
  n; if the other side's inputs are late, both consoles wait (a stall, not a desync).
- **Desync check.** Every 60 frames each side sends a hash of both consoles' RAM; a mismatch ends the session with
  a clear message (and a log of the frame).
- Transport: UDP with redundancy (each packet repeats the last few frames' inputs), so a lost packet costs nothing.

### 3. What the player sees

- Same Play together page: Local wireless, then Play now. The session tiles stay.
- Save states and rewind stay off in a session, as now.
- Leaving: the hidden console is closed; your own console's save is written as usual. The other player's console
  save on your handheld is thrown away (it belongs to the other handheld).

## Phase 1 result (2026-11-26)

Two headless consoles on the Mac (interpreter build), linked by TwinMp, driven by scripted taps (`tools/mkdsin.py`):
host Multiplayer -> Create group -> Normal -> Cut off -> VS, guest Multiplayer -> the group, then A through
character, kart and course; both hold A from frame 5000 and steer. They race on Figure-8 Circuit with items
(8000 frames). Frame hashes of both consoles were identical between a plain run and one with the guest started 3 s
late and six CPU hogs running (host waits 18 s vs 61 s).

## Phase 2 result (2026-11-26)

The RG DS and the RG DS Plus, each running both consoles as two headless processes (own core each, drawing
skipped), only inputs crossing the home Wi-Fi (UDP, 3 frames of delay, last 8 frames repeated in each packet):

- **Same game on both handhelds.** Main RAM, shared and ARM7 WRAM, DTCM and both CPUs' registers hashed every frame
  (`DS_FRAME_HASH`): identical on both handhelds, for both consoles, all 8000 frames (menus, lobby, a race).
- **Speed.** Unpaced, a race frame of the pair costs 7.6 ms mean (p99 9-10 ms) on either handheld. Paced at 60 fps
  (`--pace`), 3000 race frames took 50.1 s on all four consoles: a steady 60 fps (50.14 s is 60 fps).
- **The lead.** Publishing the clock only on transport calls (every 512 us on the host) made the twins take turns.
  Now `MpTransport::tick()` publishes it every Wi-Fi timer event, and the lead is 512 us: 128 us cost 10.6 ms a race
  frame, 512 us 7.4 ms, 2 ms 7.6 ms (one handheld, pair only).

Not yet measured: the visible console's drawing in the real engine (GPU) on top of the pair's 7.6 ms. Phase 3 will.

## Phases

1. **TwinMp in headless:** two headless processes on one machine, linked by TwinMp, run MKDS (or Download Play)
   into a multiplayer session from recorded inputs. Check: the session forms, and two runs give identical hashes.
2. **Input session in headless:** two pairs on two handhelds over UDP, inputs only; desync checks pass for a full
   race.
3. **The SDL engine:** the visible console in `rgds-experimental`, the hidden one as a `--twin` headless child
   pinned to its own core; Play together wiring; save handling.
4. **More than two players:** each handheld runs every player's console (cost: 5.6 ms and a core per extra
   console, so three players is the limit on four cores).

## Open questions

- Battery saves: games that write the save during a session (MKDS records, Pokemon trades) write it on both
  handhelds' copies; only your own console's copy is kept.
- Heat: two consoles keep two more cores busy; the 80 C Performance mode limit needs a longer race test.
- Games that use the real-time clock or the microphone in multiplayer need those fed from the session too.
