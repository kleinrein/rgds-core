# Menu sounds

Settings → Sound → Sounds & music contains the independent **Menu effects** switch, **Effects volume**, and a 17-sound preview picker, including Close lid and Open lid. The switch starts off. It is independent of interface animations and view transitions. System mute still silences everything; switching sounds off closes the audio device immediately. Microphone testing temporarily closes menu audio and restores it on return.

The library and in-game menu share the same action cues. Left/right browsing always plays on a successful step, with a clearly lower/higher pitch. Navigation varies subtly with speed and touch position; rapid input is rate-limited instead of queued. Forward travel swells inward, while back has a lower descending cue. Favourites, switches, keyboard entry, saves, loads, resume and launch have distinct effects. Save/load completion is played only after a matching successful supervisor acknowledgement (or a simulated preview completion).

The bank is 17 mono PCM16 WAV files at 32 kHz, totaling 413,440 bytes of samples. Taps last 70–130 ms, other actions 150–640 ms, and launch 1.35 seconds. No synthesis, decoding, file access or allocations happen in the audio callback. Four voices are bounded, navigation owns one voice, interrupted cues fade, and the SDL device pauses when playback finishes. Menu sounds are on by default; with them off nothing reads the WAVs or opens audio. An unavailable audio device leaves navigation functional.

Launch waits asynchronously for its flourish, capped at 1.5 seconds, and remains cancellable. Resume has a 180 ms asynchronous handoff to allow its short cue to finish. The emulator receives the audio device after the menu closes.

Lid close blanks the panels and pauses gameplay, with a 240 ms descending cue. A five-second grace period allows a brief reopen to restore the screens without deep sleep; the lid is checked every 20 ms during this interval. Longer closures use the complete stock suspend sequence. Before deep sleep, the normal panel brightness is returned to the driver so its own resume can light the displays; RGDS does not leave a zero-brightness override across kernel suspend. The stock device currently takes about 1.7 seconds to resume fully from deep sleep, including its Wi-Fi driver.

A successful quick reopen or deep wake plays the 500 ms opening cue. Native menus use cached audio without blocking navigation for the cue duration; gameplay uses the shared finite player. Saved volume is restored before playback and after firmware resume. Every new native menu sink also restores saved volume while paused, including startup before hardware status is ready; a failed restoration keeps that sink closed. Sounds remain independent of animations, and mute/off does not open audio.

The guardian records a close generation alongside position, preserving an open/close pair that occurred while the owner was busy. It wakes directly on hall events; native and game-owner debounce uses two samples at 50 ms intervals. Unknown lid position cancels a pending suspend safely. Panel levels are restored on quick reopening, cancellation, errors and guardian recovery. The in-game menu releases its audio device before being paused and reopens on wake. Failed suspend and requests cancelled before the screen-off pause do not play an opening cue. Desktop Sleep now simulates both cues without calling hardware.

The lid player expands the mono assets to stereo because the RK817 hardware rejects mono output. On the known stock ALSA profile, RGDS uses a private playback mixer shared by its launcher, engines and lid player: a paused engine can keep its stream open while a cue plays. The mixer uses 48 kHz stereo PCM16, 512-frame periods and a 2048-frame buffer, with no daemon. Stock capture remains intact. This configuration is inherited only by RGDS processes and removed for stock helpers/restore; `/etc/asound.conf` is never edited. Custom ALSA configurations are respected. Audio failures include the SDL driver's reason in the session log.

## Menu music

Settings → Sound → Sounds & music also holds **Menu music** and **Music volume** (default on, 40 %). One track loops under the cues on the home screen, in the library and in settings. It fades in over 0.6 s, fades out when a game starts and comes back when the game returns. It does not play in the in-game menu, during the microphone test or while asleep, and system mute silences it without turning it off. Music volume has a Mute choice like Effects volume.

The volume buttons (and the Sound page) also scale everything the menu plays, in software, on top of the DAC: a 12 dB range from full at 100 % to silent at 0 (`MenuAudio::set_master`, ramped over 50 ms). The DAC curve alone left the cues and the track sounding the same at every level above mute. Host-verified only; not yet listened to on the handheld.

The track is `assets/music/menu.wav`: mono IMA ADPCM, 4 bits per sample, 32 kHz, about 16 KB per second. The launcher decodes it with its own small decoder (`src/launcher/menu_music.hpp`) on a helper thread the first time music is switched on, so the SDL version on the device does not matter and nothing is read while music is off. The decoded PCM stays in memory (3.3 MB for 52 seconds); the audio callback reads it with linear interpolation and keeps no other state.

The shipped track is a 62-second loop cut from "RGDS – Heavenly Menu", generated in the user's Suno account on 2026-10-20 with v6-mini from a short instrumental prompt (heavenly, dreamy ambient handheld menu music: soft celesta and music-box bells, airy pads with a choir-like shimmer, slow, major key, no drums). The downloaded M4A take is kept under `build/menu-music/sources/`; the loop is seconds 32–96 of it with a 2 s crossfade at the seam. As with the cues, this records provenance, not a claim of commercial redistribution rights; it is not a Nintendo recording. `tools/build_menu_music.py --synth` still renders an original fallback composition (F major, 74 bpm, pad, bass, bells, sparkle, two echoes, seamless) when no source is at hand. To rebuild from the source:

```sh
python3 tools/build_menu_music.py --source build/menu-music/sources/heavenly-menu-take1.m4a --start 32 --duration 64 --crossfade 2
```

That trims, normalises to the same level, crossfades the end into the start for the loop and encodes the ADPCM file in place.

## Sources and rebuilding

Four original source families were generated in the user's Suno account on 2026-10-12 with v6-mini. The account's current creation screen exposed Songs and Speech; short instrumental prompts supplied the source material. These are original handheld-inspired cues, with no Nintendo recordings or existing melodies requested. The download dialog identified the account plan as Free; generation/download provenance is recorded here, not a claim of commercial redistribution rights.

- [RGDS – Stardust Launch](https://suno.com/song/f35dab5d-67c0-450b-b6b1-7e4309aa1674): rounded bubble, ascending glass bells, celesta, warm major-ninth sparkle.
- [RGDS – Bubble Step](https://suno.com/song/e278d740-9123-407f-817c-6da9b746f979): glassy plip, soft wooden click and rubbery upward bend.
- [RGDS – Pocket Portal](https://suno.com/song/1a165315-96a4-4938-b323-6ef37f9f009e): descending celesta and a soft liquid whoop.
- [RGDS – Crystal Keepsake](https://suno.com/song/40ca4bc4-7c89-4ef1-95da-38732d39524c): rising crystal chimes, bubble pop and a stardust tail.

The downloaded M4A originals are retained locally under `build/menu-sounds/sources/`. The packaged WAVs layer an original authored glass-note vocabulary over trimmed, time-compressed Suno textures. The launch adds a rising six-note motif and a warm major-ninth bloom. Everything is filtered, normalized and faded by `tools/build_menu_sounds.py` (requires ffmpeg only when rebuilding):

```sh
python3 tools/build_menu_sounds.py --sources build/menu-sounds/sources
python3 -m unittest discover -s tests -p test_menu_sounds.py -v
python3 -m unittest discover -s tests -p test_sleep_sounds.py -v
```

Host tests use SDL's dummy audio driver. Device checks additionally exercise the RK817 PCM with another playback owner stopped, which the dummy driver cannot reproduce. Speaker balance and character still benefit from a handheld listening pass.
