# RG DS OS 0.9.2 promo video

A plan and shot-by-shot script for a short video about 0.9.2. The features come from the release notes
(`dist/release-os/notes-0.9.2.md`). Nothing here is built yet.

## Goal

- Show that 0.9.2 is a big update: full **RG DS Plus** support, **a dozen new systems**, and a lot more to do
  between games.
- Every headline in the release notes gets at least one shot. The small items share one fast montage.
- It should feel like the OS feels: calm, quick and tactile. Real UI, no mock-ups, no voice-over. Short captions and
  the menu music do the talking.
- End on one clear next step: update over Wi-Fi, or flash a card.

## Deliverables

| Cut          | Size           | Length | For                                 |
| ------------ | -------------- | ------ | ----------------------------------- |
| Main         | 1920x1080, 60p | ~75 s  | YouTube, the website, release page  |
| Vertical     | 1080x1920, 60p | ~30 s  | Shorts, Reels, TikTok, Reddit       |
| Loop clips   | 1080x1080, 60p | 4-6 s  | GIF/WebM per feature for the README |

The clamshell is tall, so the vertical cut is the most natural fit: both screens fill the frame. The main cut puts
the handheld left of centre with the caption to its right.

## Look

- **The handheld:** the drawn shells from `tools/device_shot.py`. The black **Plus** is the hero for most shots. The
  red **RG DS** appears in the "one card image" beat and the end card.
- **Background:** a soft gradient in the theme's accent colour that shifts per section (blue for the Plus, warm for
  systems, green for the apps, and so on). It is calm and never busy behind the screens.
- **Camera:** slow push-ins (2-4%) and gentle moves between the two screens. Zoom right into a screen only when the
  detail matters (the VMU, an achievement pop-up, the 24-hour battery chart).
- **Type:** the launcher's own rounded font, in two sizes only: a headline (≤ 4 words) and an optional sub-line
  (≤ 8 words). Captions fade and slide up in about 200 ms, in step with the UI's own motion.
- **Cuts:** on the music's beat. When the UI moves, the cut waits until the motion settles.
- **Sound:** the menu music (`assets/music/menu.wav`) as the bed, plus the real menu sounds (confirm, move,
  shutter) mixed low under the matching actions. The music ducks for the Sound toy beat.

## Footage sources

Each shot below is tagged with where its frames come from.

- **UI**: the host launcher (`build/host/rgds-launcher --preview`), driven by an `--input-script`. It needs a new
  record mode (see Production). It is smooth, repeatable and never touches the handheld.
- **GAME**: game frames rendered on the Mac (host RG DS Play / `play-shot`, the DSCore Mac build in
  `build/twin-mac`). Each system must be checked to see whether it runs on the host at all.
- **DEV**: captured on the handheld. This is only for what the Mac cannot show (the Plus's 1024x768 panels if host
  scaling doesn't match, Dreamcast, 3DS). It uses a copy of userdata, and only while the owner is not playing.

## Script (main cut, ~75 s)

Times are approximate. A ✱ marks a shot that needs an open question answered first.

### 1. Hook (0:00-0:06)

| #   | Time      | Picture                                                                                                                                        | Caption                     | Source |
| --- | --------- | ---------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------- | ------ |
| 1.1 | 0:00-0:02 | Black. The Plus shell fades in, closed screens dark. The boot splash lights both panels.                                                        | —                           | UI     |
| 1.2 | 0:02-0:04 | Home appears with the time-of-day colour wash. The camera pushes in.                                                                            | **RG DS OS 0.9.2**          | UI     |
| 1.3 | 0:04-0:06 | A fast phone-style Home swipe across three system cards, with the card following the finger.                                                    | The biggest update yet      | UI     |

### 2. RG DS Plus (0:06-0:16)

| #   | Time      | Picture                                                                                                                                              | Caption                            | Source  |
| --- | --------- | ---------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------- | ------- |
| 2.1 | 0:06-0:09 | Zoom into the Plus's upper screen: crisp text and rounded corners at 1024x768. A split wipe compares it with a 640x480 upscale.                       | **Made for the RG DS Plus**        | UI ✱    |
| 2.2 | 0:09-0:12 | The library. A card slides into the second slot (animated over the shell) and a new shelf of games appears.                                            | Second card slot, live             | UI      |
| 2.3 | 0:12-0:16 | The camera pulls back. The red RG DS slides in beside the Plus, both on Home, and one microSD card icon sits between them.                            | One card. Both handhelds.          | UI      |

### 3. New systems (0:16-0:34)

The fastest section. About 1.5 s per system, cut on the beat, with the system name as a small label in the corner
rather than a headline.

| #    | Time      | Picture                                                                                             | Caption                         | Source  |
| ---- | --------- | --------------------------------------------------------------------------------------------------- | ------------------------------- | ------- |
| 3.0  | 0:16-0:18 | The Home carousel flicks through the new shelves.                                                    | **A dozen new systems**         | UI      |
| 3.1  | 0:18-0:20 | Dreamcast: the game on top, the VMU on the bottom screen. Zoom onto the VMU.                         | Dreamcast · VMU below           | DEV ✱   |
| 3.2  | 0:20-0:22 | Arcade: a vertical shooter standing tall across both screens.                                        | Arcade · tall games, two screens | GAME ✱  |
| 3.3  | 0:22-0:23 | Punch-Out!! on two monitors.                                                                         | Two-monitor cabinets            | GAME ✱  |
| 3.4  | 0:23-0:25 | Virtual Boy in red 3D across both screens.                                                           | Virtual Boy in 3D               | GAME ✱  |
| 3.5  | 0:25-0:27 | DOS: a finger types on the PC keyboard on the bottom screen, then it flips to the touch-pad mouse.   | DOS · keyboard and mouse        | GAME ✱  |
| 3.6  | 0:27-0:28 | Atari 7800 switch panel, then the 5200 keypad on the touch screen.                                   | Atari 5200 · 7800 · Lynx        | GAME ✱  |
| 3.7  | 0:28-0:29 | Amstrad CPC booting to BASIC, its keyboard below.                                                    | Amstrad CPC                     | GAME    |
| 3.8  | 0:29-0:31 | WonderSwan Color and Neo Geo Pocket Color, side by side in a split frame.                            | WonderSwan · Neo Geo Pocket     | GAME ✱  |
| 3.9  | 0:31-0:32 | A Flash game, touched on both screens.                                                               | Flash games                     | GAME ✱  |
| 3.10 | 0:32-0:34 | DSiWare from the DS shelf.                                                                           | DSiWare (experimental)          | DEV ✱   |

### 4. While you play (0:34-0:50)

| #   | Time      | Picture                                                                                                                     | Caption                              | Source     |
| --- | --------- | --------------------------------------------------------------------------------------------------------------------------- | ------------------------------------ | ---------- |
| 4.1 | 0:34-0:39 | A game moment goes wrong (a fall or a hit). Menu → Time travel: the Cover Flow of earlier moments slides, one is picked and play resumes from before the mistake. | **Rewind anything** · even DS games | GAME + UI ✱ |
| 4.2 | 0:39-0:42 | The Play together page: two players with ping, then the online toggle for Wiimmfi / WiiLink.                                  | Play together · local and online     | UI (`RGDS_PREVIEW_NET=1`) |
| 4.3 | 0:42-0:45 | An achievement pop-up slides onto the bottom screen mid-game. Zoom in.                                                       | RetroAchievements                    | GAME + UI ✱ |
| 4.4 | 0:45-0:47 | Menu + R: the shutter flashes on both screens, the shot lands in Album, gets "Set as cover", and the cover updates in the library. | Screenshots → covers               | UI         |
| 4.5 | 0:47-0:50 | A game on top, its manual on the bottom screen. A finger turns a page.                                                       | The manual, beside the game          | GAME + UI ✱ |

### 5. Between games (0:50-1:04)

| #   | Time      | Picture                                                                                                    | Caption                      | Source |
| --- | --------- | ---------------------------------------------------------------------------------------------------------- | ---------------------------- | ------ |
| 5.1 | 0:50-0:53 | The library at the cover view. Two fingers (one per screen) pinch out to the wall of games.                 | **Pinch to see everything**  | UI     |
| 5.2 | 0:53-0:56 | Media: Music with the visualiser → Videos with subtitles → a comic page, through the section pills.        | Music · videos · books       | UI ✱   |
| 5.3 | 0:56-0:58 | Files: the desktop layout copies a folder from an SMB share, and the toast slides in when it finishes.      | Files · network folders      | UI     |
| 5.4 | 0:58-1:00 | The Play journal monthly recap, then a "Where you left off" card on waking.                                 | Your play journal            | UI     |
| 5.5 | 1:00-1:02 | Little kid profile: Home turns into big game pictures. A tap starts a game.                                 | A Home for little ones       | UI     |
| 5.6 | 1:02-1:04 | Battery health: the health ring, cycles and the 24-hour chart drawing in.                                   | Battery health               | UI     |

### 6. Little things (1:04-1:10)

A fast montage at about 0.8 s each. One caption covers the whole stretch: **And a lot more.**

- Sound toy: the voice changer's waveform wobbles and becomes the startup sound, with the music ducked under the
  voice. (UI, needs an audio clip ✱)
- Bring over: the setup wizard's "Bring your things over" step moving saves from a stock card.
  (UI with `RGDS_IMPORT_PREVIEW`)
- Bluetooth headphones: the codec row showing LDAC. (UI with `RGDS_PREVIEW_BLUETOOTH`)
- A toast sliding in and out of the way.
- Home's app row with Album.
- The hidden game: 0.4 s of a glitchy pixel snake on the upper screen, cut before it can be read. It is a tease, not
  a tutorial.

### 7. End card (1:10-1:16)

| #   | Time      | Picture                                                                                                       | Caption                                           |
| --- | --------- | ------------------------------------------------------------------------------------------------------------- | ------------------------------------------------- |
| 7.1 | 1:10-1:13 | Both handhelds centred. Home shows the update banner, Y is pressed and the glitter bar fills.                  | **Update over Wi-Fi** · press Y on Home           |
| 7.2 | 1:13-1:16 | The logo, the version and the URL. The music resolves.                                                          | RG DS OS 0.9.2 · github.com/kleinrein/rgds-core   |

## Vertical cut (~30 s)

The same footage with tighter picks and one feature per beat. The handheld fills the frame, with captions above
the upper screen.

1. 0:00-0:02 Boot → Home, **RG DS OS 0.9.2** (1.1-1.2)
2. 0:02-0:05 Plus sharpness zoom + second card slot (2.1-2.2)
3. 0:05-0:13 Five systems: Dreamcast VMU, tall arcade, Virtual Boy, DOS keyboard, Flash (3.1, 3.2, 3.4, 3.5, 3.9)
4. 0:13-0:17 Rewind Cover Flow (4.1)
5. 0:17-0:19 Achievement pop-up (4.3)
6. 0:19-0:22 Pinch to the wall of games (5.1)
7. 0:22-0:25 Media + Little kid + battery, one second each
8. 0:25-0:30 Update banner → logo + URL (7.1-7.2)

## Production

### 1. A record mode for the preview (launcher, preview only)

- `--record DIR` writes both panels every frame as `DIR/top-%05d.bmp` and `DIR/bottom-%05d.bmp`. Raw frames, so
  ffmpeg can encode them however each cut needs.
- With `--record`, the preview runs on a **fixed 60 Hz clock** instead of wall time, so every animation is smooth
  and every run matches, however slow writing the frames is. The `--input-script` times follow the same clock.
- It is refused outside `--preview`, the same as `--input-script`.
- A plain `tap`/`drag` in the input script already works. A two-screen pinch needs the second finger to land on
  the upper window. Check `finger-*` with a finger id and a window per screen before scripting 5.1.

### 2. Scene scripts

- One folder per shot: `tools/promo/shots/<id>/` with `input.txt` (the input script), `env` (preview variables
  such as `RGDS_PREVIEW_NET=1`), and the preview data (a demo library, profiles, journal and battery history).
- The demo data lives in `tools/promo/data/` and is never anyone's real userdata. Game titles and covers in it
  follow the content rule below.
- `tools/promo/record.py <id>` runs the launcher with `--record` and the shot's script, then trims the frames.

### 3. Compositing

- `tools/promo/compose.py` reuses `device_shot.py`'s shell drawing (numpy) to put each frame pair into the Plus or
  RG DS shell over the section's gradient. It also handles the camera moves (scale/position keyframes per shot)
  and the captions (text baked with the launcher's font).
- ffmpeg joins the shots with cuts, adds the music and the UI sounds, and encodes:
  H.264 high, CRF 16, yuv420p, 60 fps for the web. A ProRes master is optional.
- One edit list (`tools/promo/edit.toml`) holds each cut's shot order, timings, captions and camera keyframes. A
  timing change then means editing a line and re-running, never re-recording.

### 4. Review loop

1. A rough cut from screenshots only (no recording yet), so the pacing and captions can be checked against the
   music.
2. The UI shots recorded and dropped in.
3. The game footage dropped in.
4. Colour, sound mix and caption polish, then the vertical cut and the loop clips.

## Content rule for games

Commercial game footage in a promo is a legal grey area, and the project is careful not to ship anything from
Nintendo. Two options:

- **A. Homebrew and free games only** (the homebrew shelves, freeware Flash, Flashpoint, DOS shareware, public
  domain demos). This is the safest choice. Some beats lose their best-known example (Punch-Out!!, a famous
  shooter).
- **B. Commercial games in short clips** with no title cards or box art. This is riskier and depends on the
  platforms the video is posted to.

The script works with either option. Under option A, beat 3.3 (two monitors) needs a homebrew example or is cut.

## Open questions (✱)

1. **Game content:** option A or B above?
2. **Plus panels (2.1):** does the host preview render true 1024x768 at the Plus scale, or does that shot need the
   handheld?
3. **Host game coverage:** which of Dreamcast, FBNeo, Beetle VB, DOSBox Pure, Atari, WonderSwan/NGP, Ruffle and
   DSiWare run on the Mac? Anything that doesn't moves to DEV or is cut.
4. **Rewind, achievements and manuals in a game (4.1, 4.3, 4.5):** are they drawn by the bridge/companion on the
   host, or only on the device?
5. **Music:** the menu music came from Suno. Check that the account's plan allows its use in a published video.
   Otherwise use the synth fallback or another licensed track.
6. **Media (5.2):** a video and a comic page that are free to show.
7. **Sound toy:** whose voice? A synthetic one, or the owner's.
8. **Hidden game:** tease it (as written) or leave it out?
