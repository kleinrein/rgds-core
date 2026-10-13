# Menu sounds

Settings → Sound → Menu sounds contains the independent enable switch, effects volume, and a 17-sound preview picker, including Close lid and Open lid. The switch starts off. It is independent of interface animations and view transitions. System mute still silences everything; switching sounds off closes the audio device immediately. Microphone testing temporarily closes menu audio and restores it on return.

The library and in-game menu share the same action cues. Left/right browsing always plays on a successful step, with a clearly lower/higher pitch. Navigation varies subtly with speed and touch position; rapid input is rate-limited instead of queued. Forward travel swells inward, while back has a lower descending cue. Favourites, switches, keyboard entry, saves, loads, resume and launch have distinct effects. Save/load completion is played only after a matching successful supervisor acknowledgement (or a simulated preview completion).

The bank is 17 mono PCM16 WAV files at 32 kHz, totaling 413,440 bytes of samples. Taps last 70–130 ms, other actions 150–640 ms, and launch 1.35 seconds. No synthesis, decoding, file access or allocations happen in the audio callback. Four voices are bounded, navigation owns one voice, interrupted cues fade, and the SDL device pauses when playback finishes. The default-off path does not read the WAVs or open audio. An unavailable audio device leaves navigation functional.

Launch waits asynchronously for its flourish, capped at 1.5 seconds, and remains cancellable. Resume has a 180 ms asynchronous handoff to allow its short cue to finish. The emulator receives the audio device after the menu closes.

Lid close blanks the panels and pauses gameplay, with a 240 ms descending cue. A five-second grace period allows a brief reopen to restore the screens without deep sleep; the lid is checked every 20 ms during this interval. Longer closures use the complete stock suspend sequence. Before deep sleep, the normal panel brightness is returned to the driver so its own resume can light the displays; RGDS does not leave a zero-brightness override across kernel suspend. The stock device currently takes about 1.7 seconds to resume fully from deep sleep, including its Wi-Fi driver.

A successful quick reopen or deep wake plays the 500 ms opening cue. Native menus use cached audio without blocking navigation for the cue duration; gameplay uses the shared finite player. Saved volume is restored before playback and after firmware resume. Every new native menu sink also restores saved volume while paused, including startup before hardware status is ready; a failed restoration keeps that sink closed. Sounds remain independent of animations, and mute/off does not open audio.

The guardian records a close generation alongside position, preserving an open/close pair that occurred while the owner was busy. It wakes directly on hall events; native and game-owner debounce uses two samples at 50 ms intervals. Unknown lid position cancels a pending suspend safely. Panel levels are restored on quick reopening, cancellation, errors and guardian recovery. The in-game menu releases its audio device before being paused and reopens on wake. Failed suspend and requests cancelled before the screen-off pause do not play an opening cue. Desktop Sleep now simulates both cues without calling hardware.

The lid player expands the mono assets to stereo because the RK817 hardware rejects mono output. On the known stock ALSA profile, RGDS uses a private playback mixer shared by its launcher, engines and lid player: a paused engine can keep its stream open while a cue plays. The mixer uses 48 kHz stereo PCM16, 512-frame periods and a 2048-frame buffer, with no daemon. Stock capture remains intact. This configuration is inherited only by RGDS processes and removed for stock helpers/restore; `/etc/asound.conf` is never edited. Custom ALSA configurations are respected. Audio failures include the SDL driver's reason in the session log.

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
