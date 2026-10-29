#!/usr/bin/env python3
"""Prepare the small PCM sound bank from the four downloaded Suno sources.

Usage: python3 tools/build_menu_sounds.py --sources /path/to/downloads
Requires ffmpeg; playback on the handheld does not require it.
"""
import argparse
import array
import math
from pathlib import Path
import shutil
import subprocess
import wave

ROOT = Path(__file__).resolve().parents[1]
# source family, start, source length, tempo, pitch ratio, final seconds, reverse
RECIPES = {
    'navigate': ('Bubble Step', .10, .35, 1.4, 1.12, .13, False),
    'type': ('Bubble Step', .12, .24, 1.7, 1.4, .07, False),
    'adjust': ('Bubble Step', .13, .35, 1.4, .85, .12, False),
    'confirm': ('Crystal Keepsake', .10, 1.1, 2.1, 1, .30, False),
    'open': ('Pocket Portal', .10, 2.7, 4, 1.12, .40, True),
    'back': ('Pocket Portal', .10, 2.7, 4, .86, .43, False),
    'enable': ('Crystal Keepsake', .10, .8, 2, 1.2, .22, False),
    'disable': ('Pocket Portal', .35, .9, 2.2, .83, .24, False),
    'favourite': ('Crystal Keepsake', .10, 3.2, 4, 1.15, .64, False),
    'unfavourite': ('Crystal Keepsake', .12, 1.5, 3, .84, .33, True),
    'launch': ('Stardust Launch', .10, 5.8, 4, 1, .70, False),
    'resume': ('Stardust Launch', .30, .65, 3, 1.14, .15, False),
    'save': ('Crystal Keepsake', .10, 2.8, 4, 1, .55, False),
    'load': ('Pocket Portal', .12, 2.6, 4, 1.08, .50, True),
    'error': ('Bubble Step', .12, 1.2, 2, .62, .29, False),
    'lid-close': ('Pocket Portal', .20, 1.0, 3, .78, .24, False),
    'lid-open': ('Crystal Keepsake', .10, 2.3, 4, 1.08, .50, True),
}

# A coherent original musical vocabulary sits over the generated textures.
# Notes are authored here, not sampled from an existing console melody.
NOTES = {
    'navigate': [(0, 1046.5)],
    'type': [(0, 1318.5)],
    'adjust': [(0, 784)],
    'confirm': [(0, 1046.5), (.075, 1318.5)],
    'open': [(0, 523.25), (.09, 784), (.18, 1046.5)],
    'back': [(0, 1046.5), (.09, 784), (.18, 523.25)],
    'enable': [(0, 659.25), (.065, 1046.5)],
    'disable': [(0, 784), (.075, 523.25)],
    'favourite': [(0, 1046.5), (.085, 1318.5), (.17, 1568), (.255, 2093)],
    'unfavourite': [(0, 1046.5), (.11, 659.25)],
    'launch': [(0, 523.25), (.10, 659.25), (.20, 784), (.33, 987.77), (.47, 1174.66), (.62, 1568)],
    'resume': [(0, 784), (.045, 1046.5)],
    'save': [(0, 659.25), (.09, 784), (.18, 1046.5)],
    'load': [(0, 1046.5), (.105, 784), (.21, 1318.5)],
    'error': [(0, 392), (.10, 369.99)],
    'lid-close': [(0, 987.77), (.065, 659.25), (.13, 493.88)],
    'lid-open': [(0, 659.25), (.075, 987.77), (.15, 1318.5), (.225, 1975.53)],
}


def musical_layer(name, samples, duration):
    length = round(duration * 32000)
    samples.extend([0.] * max(0, length - len(samples)))
    del samples[length:]
    peak = max(map(abs, samples)) or 1
    # The texture stays underneath a clear, immediate note, even on tiny speakers.
    for i in range(length):
        samples[i] = samples[i] / peak * .16
    for start, frequency in NOTES[name]:
        decay = .24 if name == 'launch' else .12 if name in ('favourite', 'save', 'load', 'lid-open') else .055
        for i in range(round(start * 32000), length):
            t = i / 32000 - start
            attack = 1 - math.exp(-t / 0.0025)
            bell = (
                math.sin(math.tau * frequency * t) * math.exp(-t / decay) +
                .22 * math.sin(math.tau * frequency * 2.005 * t) * math.exp(-t / (decay * .55)) +
                .07 * math.sin(math.tau * frequency * 3.98 * t) * math.exp(-t / (decay * .3))
            )
            samples[i] += .52 * attack * bell
    if name == 'launch':
        for i in range(length):
            t = i / 32000
            # Quiet, warm major-ninth bloom beneath the ascending glass notes.
            pad = sum(math.sin(math.tau * f * t) for f in (130.8128, 196, 246.9417, 293.6648)) / 4
            swell = (1 - math.exp(-t / .15)) * math.exp(-t / .6)
            sparkle = math.sin(math.tau * 3136 * t) * (.5 + .5 * math.sin(math.tau * 7 * t)) * math.exp(-t / .35)
            samples[i] += .16 * pad * swell + .025 * sparkle
    return samples


def build(source, out):
    out.mkdir(parents=True, exist_ok=True)
    ffmpeg = shutil.which('ffmpeg')
    if not ffmpeg:
        raise SystemExit('ffmpeg is required to rebuild the sound bank')
    for name, (family, start, length, tempo, pitch, duration, reverse) in RECIPES.items():
        path = source / f'RGDS - {family}.m4a'
        filters = f'aresample=32000,atrim=start={start}:duration={length},asetpts=PTS-STARTPTS,atempo={tempo},asetrate={32000*pitch},aresample=32000'
        if reverse:
            filters += ',areverse'
        filters += f',atrim=duration={duration},highpass=f=100,lowpass=f=10000'
        raw = subprocess.run(
            [ffmpeg, '-v', 'error', '-i',
             str(path), '-ac', '1', '-ar', '32000', '-af', filters, '-f', 'f32le', '-'],
            check=True,
            capture_output=True
        ).stdout
        samples = array.array('f', raw)
        if not samples:
            raise ValueError(f'{path} contains no audio')
        samples = musical_layer(name, samples, duration)
        dc = sum(samples) / len(samples)
        # Smooth 2 ms attack and a finite release; no hard cuts or long tails.
        attack = 64
        release = min(len(samples) // 2, int(32000 * min(.18, duration * .48)))
        for i in range(len(samples)):
            envelope = min(1, i / attack) * min(1, (len(samples) - 1 - i) / release)**1.5
            samples[i] = (samples[i] - dc) * envelope
        peak = max(map(abs, samples))
        if peak < .0001:
            raise ValueError(f'{name} is silent')
        pcm = array.array('h', (round(x * .66 / peak * 32767) for x in samples))
        with wave.open(str(out / f'{name}.wav'), 'wb') as wav:
            wav.setparams((1, 2, 32000, 0, 'NONE', 'not compressed'))
            wav.writeframes(pcm.tobytes())
        print(f'{name}: {len(samples)/32000:.3f}s, {len(pcm)*2} bytes')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sources', type=Path, required=True)
    parser.add_argument('--output', type=Path, default=ROOT / 'assets/sounds')
    args = parser.parse_args()
    build(args.sources, args.output)
