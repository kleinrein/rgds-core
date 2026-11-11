# Music

A music player for the handheld. It is off until you ask for it: **Settings → Sound → Music player** adds a Music
tile to Home (beside PictoChat in the column layout, after it in the carousel). Switching it off removes the tile and
stops the song.

## Putting music on the card

Music lives in the `music` folder at the top of the data partition, next to `roms` (`/data/music`). Copy files and
folders there with Settings → Connectivity → Use as USB drive, or over the network: file sharing has a Music share
(the same `/data/music`, made if it is missing). Any folder layout works. The
player looks through the folder when it starts and again whenever it is opened; only new and changed files are read,
so a large library is slow once.

What plays: MP3, FLAC, AAC and Apple Lossless (`.m4a`), Ogg Vorbis, Opus, WAV, AIFF, WMA, Monkey's Audio, WavPack,
Musepack, TTA, AC-3 and the other formats FFmpeg decodes by itself, and tracker modules (MOD, XM, S3M, IT and
relatives) through libopenmpt. Titles, artists, albums, track and disc numbers and years come from the files' tags; a
file without a title goes by its name, and files without an album tag are grouped by their folder. Covers come from
the tag, or from a `cover`, `folder`, `front`, `album` or `artwork` picture (JPEG, PNG, WebP, BMP) in the same folder.

## The screens

The lower screen is the player; the upper screen shows what it is playing.

- **Library**: five tabs, Songs, Albums, Artists, Folders and Lists (Favourites and Recently added). A song plays its
  whole page from that song on: the album, the folder, every song. While something plays, a bar along the bottom
  shows it, with play/pause and next; tapping the bar opens Now playing.
- **Now playing**: the cover, a seek bar to drag, shuffle, previous, play/pause, next and repeat, and three buttons in
  the header for the visualiser, the equaliser and the favourite heart. Swipe the cover left or right for the next or
  previous song; tap it to pause.
- **Queue**: what plays next, in order. Tap a song to jump to it.
- **Equaliser**: ten sliders from 31 Hz to 16 kHz, 12 dB either way, and twelve presets. The curve is kept
  separately for the speakers, for headphones and for a Bluetooth headset, and the header says which one is being
  shaped. Boosts never clip: the player takes the curve's own peak off in front, and a limiter sits behind.

| Control | Library and queue | Now playing | Equaliser |
| --- | --- | --- | --- |
| D-pad | Up/Down a row, Left/Right a page | Left/Right 10 seconds, Up larger display, Down queue | Left/Right a band, Up/Down 1 dB |
| A | Play or open | Play or pause | On or off |
| B | Back | Library | Back |
| X | Options: Play next, Add to queue, favourite | Equaliser | Flat |
| Y | Now playing | Next visualiser | |
| L / R | Tabs (in the queue: previous, next) | Previous, next | Preset |
| L2 / R2 | Shuffle, repeat | Shuffle, repeat | Shuffle, repeat |
| Start | Play or pause | Play or pause | Play or pause |
| Touch | Tap to play, flick to scroll, hold a row for its options | Drag the seek bar, swipe the cover | Drag a slider, tap either half of the preset |

Home and Back from the screen's edges work as everywhere, when touch gestures are on.

## The visualiser

The upper screen's dark window is the visualiser, drawn in big pixels and tinted with the theme's accent colour:
Spectrum (an LED ladder whose caps drop like weights), Oscilloscope (a phosphor trace), Fire, Plasma, Starfield, VU
meters (two needles with real ballistics), Vectorscope (left against right, as a studio's goniometer), Radial
spectrum (the spectrum bent into a ring that breathes with the kick), Waterfall (past spectra as ridges marching
away) and Tunnel (rings shaped by the music flying out of the dark). The pictures that draw the wave itself follow
its level, so a quiet recording still fills the window. Y steps through them; the header button lists them and has Larger display, which
gives the window the whole upper screen. On the equaliser page the window is always the spectrum, with the curve
being shaped drawn across it. Over Bluetooth the picture is held back by a fifth of a second, so it moves with what
is heard rather than with what was sent.

## Playing on

- Music keeps playing when you leave the player for Home, the library or Settings; four small bars beside the clock
  in the status bar dance to it. The menu's own sounds and music stand down while a song plays.
- Loudness: the volume keys set the DAC, and songs play at full scale under it (only the lowest few steps taper a
  little further). On the internal speakers songs get 6 dB of make-up gain, held under full scale by the limiter;
  headphones and Bluetooth play the file as it is.
- Starting a game pauses the song and frees the sound device for the game.
- **Settings → Sound → Play with the lid closed** (on by default) keeps a song playing under a closed lid: the
  screens go dark and buttons and touches are ignored until it opens. When the song is paused or the queue runs out
  with the lid still shut, the handheld waits half a minute and then sleeps as usual. Off, the lid always sleeps.
- A Bluetooth headset connected in Settings → Connectivity takes the sound when it connects. If it goes away
  (switched off, out of range) the song pauses instead of carrying on out of the speakers; pulling out wired
  headphones pauses too. A headset's own play/pause, next and previous keys work, also under a closed lid.
- The player remembers the queue, the place in the song, shuffle, repeat, the visualiser and the equaliser across
  restarts.

## How it is built

The sound is not the launcher's. `rgds-music` (`src/music/engine.cpp`) is a small process of its own that decodes
with FFmpeg, runs the equaliser and limiter, and plays through SDL audio, which is ALSA on the handheld and so
follows `/etc/asound.conf` to wherever `bt-audio` has pointed playback. It reads the next track ahead, so albums
play without gaps. Being a separate process is what lets music carry on while the menu is busy or restarting, and
means a damaged file can only take the engine down.

- The launcher starts the engine when Music is opened and talks to it with one-line commands on a Unix datagram
  socket (`play`, `seek 90000`, `eq 1 3 4 3 2 …`; the list is at the top of `engine.cpp`).
- The engine publishes what it is doing in a mapped block (`src/music/shared.hpp`): the track, the position, the
  equaliser, the scan's progress, and sixty times a second the spectrum, waveform and levels the visualisers draw.
- The library is `library.tsv` in `rgds/music/`, with covers beside it at exactly the three sizes the screens draw
  (`art/<key>-44.bmp`, `-88`, `-200`). The queue (`queue.tsv`), favourites, the session and the equaliser are files
  in the same folder.
- On RG DS OS the engine is a Buildroot package (`os/buildroot/package/rgds-music`) so that it is compiled against
  the image's own FFmpeg; on a development machine `python3 tools/build_music.py` builds it beside the host
  launcher from the system's FFmpeg and SDL2 (`brew install ffmpeg sdl2`).
- From a shell on the handheld: `rgds-music status --run /run/rgds/music` prints what the engine publishes,
  `rgds-music send --run /run/rgds/music next` sends a command, `rgds-music probe FILE` says what FFmpeg makes of a
  file. The engine's own messages are in `/run/rgds/music/engine.log`.

Launcher side: `music_client.hpp` (the library as lists, the queue, the engine as a process), `music_screen.hpp`
(the screens, as members of `App`), `music_viz.hpp` (the visualisers, plain arithmetic on a pixel buffer),
`music_glyphs.hpp` (the symbols, one shape each for both icon styles). The Home icon is drawn by
`tools/build_music_icon.py`.

## Tests

```sh
python3 tools/build_music.py && python3 tools/build.py host
python3 -m unittest tests.test_music_player tests.test_music_ui
```

`test_music_player` runs the equaliser, analyser, library grouping, queue and pictures under sanitizers, then has the
engine scan and play real MP3, FLAC, AAC and WAV files (made with `ffmpeg`) through SDL's dummy audio driver: seeking,
the gapless change, repeat, a file that will not open, a restart, a headset that goes. `test_music_ui` drives the
launcher: the switch in Settings, the Home tile, the tabs, a tapped song that plays on after the menu has left.
