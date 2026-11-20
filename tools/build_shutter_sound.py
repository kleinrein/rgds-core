#!/usr/bin/env python3
"""Synthesise assets/sounds/shutter.wav, the screenshot's camera click (32 kHz mono, like the rest of the bank).

Two filtered noise bursts a few tens of milliseconds apart (the curtain opening and closing) over a short
low thump. Deterministic: the noise is seeded, so a rebuild writes the same bytes.
Usage: python3 tools/build_shutter_sound.py [--output assets/sounds/shutter.wav]
"""
import argparse
import array
import math
from pathlib import Path
import random
import wave

ROOT = Path(__file__).resolve().parents[1]
RATE = 32000


def build(output):
    length = round(.16 * RATE)
    noise = random.Random(7)
    samples = [0.] * length
    for start, level, decay in ((0, 1.0, .006), (.052, .75, .009)):
        low = 0.
        begin = round(start * RATE)
        for i in range(begin, length):
            t = (i - begin) / RATE
            white = noise.uniform(-1, 1)
            low += .45 * (white - low)  # a soft one-pole low-pass: a click, not a hiss
            samples[i] += level * (white - .6 * low) * math.exp(-t / decay)
    for i in range(length):
        t = i / RATE
        samples[i] += .35 * math.sin(math.tau * 140 * t) * math.exp(-t / .018)
    release = round(.02 * RATE)
    for i in range(length):
        samples[i] *= min(1, i / 32) * min(1, (length - 1 - i) / release)
    peak = max(map(abs, samples))
    pcm = array.array('h', (round(x * .66 / peak * 32767) for x in samples))
    with wave.open(str(output), 'wb') as clip:
        clip.setparams((1, 2, RATE, 0, 'NONE', 'not compressed'))
        clip.writeframes(pcm.tobytes())
    print(f'{output}: {length / RATE:.3f}s')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'assets/sounds/shutter.wav')
    build(parser.parse_args().output)
