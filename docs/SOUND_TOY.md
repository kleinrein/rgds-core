# Sound toy

A voice changer on Home, after the DSi's sound app: record a word with the microphone, play it back through 24 voices (a chipmunk, a robot, a monster, backwards, a choir, a dragon...), and draw on the lower screen to bend it. Any of the six recordings can be the sound the handheld plays when it starts.

## Using it

- **Home → Sound toy.** The tile comes after PictoChat and Music. In the column layout it sits beside Recents when it is the only app, and the last row holds three narrower tiles when PictoChat, Music and the toy are all there. **Settings → Sound → Sound toy** hides it (on by default).
- **Record (X):** up to 10 seconds; X, A or B stops it early. The silence before and after the word is trimmed, the level is evened out, and the sound plays straight away.
- **Voices (Left/Right, or tap a face):** 24 of them, six to a page; the arrows and dots under them turn the page, and Left/Right walks straight across pages. Changing the voice plays the sound again.

| Page | Voices |
|---|---|
| 1 | Normal · Chipmunk (1.8× tape speed) · Robot (ring modulation through a 110 Hz metal comb) · Monster (0.62× speed, growled and darkened) · Backwards · Echo (quarter-second repeats) |
| 2 | Alien (ring modulation, fast vibrato, a short flutter echo) · Giant (9 semitones down at the same speed, a room) · Ghost (slow wobble, high-passed, a big hall) · Underwater (muffled, wobbling, bubbles of level) · Choir (a major chord: the voice plus a third and a fifth above) · 8-bit (5 kHz sample-and-hold, 12 levels) |
| 3 | Telephone (450-3000 Hz, a little crackle) · Megaphone (narrow and driven hard, a slap-back) · Cave (a big room and an echo) · Jelly (a wide vibrato) · Scratch (cut into pieces, some played twice or three times, some fast or slow) · Whisper (a four-band vocoder on noise: the words, no voice) |
| 4 | Sloth (half speed, pitch kept) · Speedy (double speed, pitch kept) · Astronaut (radio band, an echo, and the "over" beep) · Dragon (an octave down, growled, a room) · Baby (6 semitones up, a little vibrato) · Opera (3 up, a singer's vibrato, a hall) |
- **Bend (draw on the pad):** left to right is the recording, up is up to an octave higher, down an octave lower. Undrawn parts stay as they were. Drawing while it plays bends what you hear at once; touching the pad when nothing plays starts it. **Clear (Y)** removes the curve; with nothing drawn the button reads **Erase**, and pressing it twice within three seconds deletes the sound (and the startup sound, if it was this one).
- **Six sounds (L/R or Up/Down):** each keeps its own recording, voice and curve.
- **Startup sound (Start, or the button in the header):** this sound, with its voice and curve, plays once when Home first appears after the handheld starts (at most 4 seconds, faded). Press again to go back to a quiet start. Mute keeps it quiet; hiding the tile does not switch it off.

## How it works

| Part | Where |
|---|---|
| Voices, curve, trimming, WAV files | `src/launcher/voice_toy.hpp` (pure C++, no SDL) |
| Faces | `src/launcher/voice_faces.hpp` (pure C++: anti-aliased signed-distance painting) |
| Screens, keys, touch, recording, startup | `src/launcher/voice_toy_screen.hpp`, state in `voice_toy_ui.hpp` |
| Playback | `MenuAudio::play_toy` in `src/launcher/menu_audio.hpp` |
| Microphone | `runtime/microphone_test.py --record` |

- **Each voice is a recipe** (`VoiceSpec`): tape speed and vibrato, up to two delay-line pitch shifters (40 ms windows; they change pitch without changing speed, so Sloth and Speedy keep your pitch and Choir makes a chord), ring modulation, state-variable high- and low-pass, drive, a bit crusher, the robot's comb, an echo, a small Schroeder room, the whisper's vocoder and the scratch cutter. A soft limit (tanh) ends every chain; gains are set so the voices land within about 1.5 dB of Normal.
- **Faces** are painted per voice from circles, ellipses, boxes, triangles and arcs measured as signed distances: anti-aliased edges, a darker rim, a top-to-bottom gradient, a soft shadow and a glassy highlight. Each is baked once per size and mouth opening (five steps) into a texture (`vt.faces`, dropped with the renderers); talking frames are evicted first when the cache passes 96.
- **Playback is live.** `voice_toy::Player` runs inside the menu sink's audio callback (no allocation, locks or file access there), so a curve drawn mid-sound is heard from the next block. It plays whether or not menu effects are switched on. A sound asked for while the sink is shut waits until it is open, as cues do.
- **Recording** forks the microphone helper in `--record` mode: it raises the RK817 capture level as the microphone test does, prints `rec` once samples arrive (or `busy`/`unavailable`), then streams raw mono PCM16 at 16 kHz on stdout for at most 11 s. It writes nothing to disk; the launcher keeps what it is given. Menu cues are silent while recording and menu music is off while the toy is open, so neither ends up in the recording.
- **Files** are under `userdata/sound-toy/`: `slot-N.wav` (the trimmed recording, mono PCM16 16 kHz), `slot-N.ini` (`voice=` and 64 `bend=` values), and `startup.wav` + `startup.ini` (the rendered startup sound and which slot it came from). Changing the startup slot's voice or curve renders it again.
- **Home tiles** are numbered 0 Settings, 1 Games, 2 Favourites, 3 Recents, then the apps densely (PictoChat, Music, Sound toy), and 7 the last played game. The state file keeps the numbers from before the toy: it writes the last played game as `home 6` and tile 6 as `home 7`.
- **The preview** has no microphone: Record makes up a two-note "hello", or reads the WAV named by `RGDS_SOUND_TOY_WAV` (mono PCM16, any rate up to 48 kHz) so real voices can be tried on a Mac.

## Listening on a Mac

The preview's Record uses a made-up "hello" (or `RGDS_SOUND_TOY_WAV`). To hear every voice on a real voice, `say` can make one:

```sh
say -o /tmp/hello.aiff "Hello! Do you want to play with me?"
afconvert -f WAVE -d LEI16@16000 -c 1 /tmp/hello.aiff /tmp/hello.wav
RGDS_SOUND_TOY_WAV=/tmp/hello.wav make preview
```

## Tests

```sh
python3 -m unittest tests.test_voice_toy -v
```

`tests/voice_toy_test.cpp` (built under ASan/UBSan) checks trimming, that every voice is heard, stays finite and inside full scale and ends, the pitch and length of the speed and shifter voices, backwards, the scratch, the curve, the player's blocks and the files. The UI tests drive the preview: the tile and its setting, a recording through to Robot and the startup sound, Clear then Erase, walking across the voice pages, and the column's three-tile row. `RGDS_TEST_LAUNCHER` points them at a launcher other than `build/host/rgds-launcher`.

## Not verified on the handheld

Host-verified only (preview screenshots, the tests above). Not yet heard or seen on the device: real capture through `rgds_mic` at 16 kHz in record mode, how loud the voices are through the speakers, the startup sound at boot (timing against the boot volume restore and the menu music), and frame cost on the A55 while the face talks and the playhead moves (about 30 redraws a second while sound plays).
