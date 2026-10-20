#!/usr/bin/env python3
"""Build assets/music/menu.wav, the optional menu music loop.

The launcher plays one mono IMA ADPCM WAV (4 bits per sample, 32 kHz) through
its own decoder, looped end to start. Two ways to make it:

  --source FILE   a downloaded track (Suno M4A/MP3/WAV): trimmed, loudness
                  matched, loop-crossfaded and encoded (needs ffmpeg)
  --synth         the bundled original composition: a calm handheld-menu tune
                  rendered here from sine partials, no samples, no ffmpeg
                  needed for the rendering (ffmpeg only encodes the ADPCM)

The output is small (16 KB per second) and decodes to 16-bit PCM once, when
the user turns music on. Keep tracks under about 90 seconds.
"""
import argparse
import array
import math
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RATE = 32000


def write_pcm_wav(path, samples):
    data = array.array('h', (max(-32767, min(32767, int(round(s * 32767)))) for s in samples)).tobytes()
    header = b'RIFF' + struct.pack('<I', 36 + len(data)) + b'WAVE' + b'fmt ' + struct.pack('<IHHIIHH', 16, 1, 1, RATE, RATE * 2, 2, 16) + b'data' + struct.pack('<I', len(data))
    path.write_bytes(header + data)


def read_pcm(path, start, duration):
    """Decode any audio file to mono float samples at RATE with ffmpeg."""
    command = ['ffmpeg', '-v', 'error', '-i', str(path)]
    if start: command += ['-ss', str(start)]
    if duration: command += ['-t', str(duration)]
    command += ['-ac', '1', '-ar', str(RATE), '-f', 's16le', '-']
    raw = subprocess.run(command, check=True, capture_output=True).stdout
    return [s / 32768 for s in array.array('h', raw)]


def normalise(samples, peak=0.6):
    top = max(1e-6, max(abs(s) for s in samples))
    gain = peak / top
    return [s * gain for s in samples]


def loop_crossfade(samples, seconds):
    """Blend the tail into the head so the loop point is inaudible."""
    n = min(int(seconds * RATE), len(samples) // 4)
    if n <= 0: return samples
    body = samples[:len(samples) - n]
    tail = samples[len(samples) - n:]
    for i in range(n):
        t = i / n
        body[i] = body[i] * t + tail[i] * (1 - t)
    return body


# ---------------------------------------------------------------- synthesis

def note(name):
    names = {'C': 0, 'D': 2, 'E': 4, 'F': 5, 'G': 7, 'A': 9, 'B': 11}
    pitch = names[name[0]] + (1 if '#' in name else 0) - (1 if 'b' in name else 0)
    octave = int(name[-1])
    return 440.0 * 2 ** ((pitch + 12 * (octave + 1) - 69) / 12)


def render_bell(buffer, at, freq, seconds, gain, rng):
    """A glass bell: fundamental plus soft upper partials, exponential decay."""
    n = int(seconds * RATE)
    detune = 1 + rng.uniform(-0.0008, 0.0008)
    partials = ((1.0, 1.0, 1.0), (2.0, 0.22, 1.6), (3.0, 0.11, 2.2), (4.2, 0.05, 3.0))
    for i in range(n):
        t = i / RATE
        attack = min(1.0, t / 0.006)
        v = 0.0
        for ratio, amp, speed in partials:
            v += amp * math.exp(-t * 2.4 * speed) * math.sin(2 * math.pi * freq * detune * ratio * t)
        j = at + i
        if j < len(buffer): buffer[j] += v * gain * attack


def render_pad(buffer, at, freqs, seconds, gain):
    """A warm pad: three detuned sines per note with slow attack and release."""
    n = int(seconds * RATE)
    voices = [(f * d, phase) for f in freqs for d, phase in ((1.0, 0.0), (1.004, 1.1), (0.996, 2.3))]
    for i in range(n):
        t = i / RATE
        env = min(1.0, t / 0.9) * min(1.0, (seconds - t) / 1.2)
        v = 0.0
        for f, phase in voices:
            v += math.sin(2 * math.pi * f * t + phase) + 0.18 * math.sin(2 * math.pi * f * 2 * t + phase)
        j = at + i
        if j < len(buffer): buffer[j] += v * gain * env / len(voices)


def render_bass(buffer, at, freq, seconds, gain):
    n = int(seconds * RATE)
    for i in range(n):
        t = i / RATE
        env = min(1.0, t / 0.03) * math.exp(-t * 1.3) * min(1.0, (seconds - t) / 0.15)
        v = math.sin(2 * math.pi * freq * t) + 0.25 * math.sin(2 * math.pi * freq * 2 * t)
        j = at + i
        if j < len(buffer): buffer[j] += v * gain * env


def compose(seed=7):
    """An original, calm tune for a handheld menu: F major, 74 bpm, 16 bars."""
    rng = random.Random(seed)
    bpm = 74
    beat = 60 / bpm
    bars = 16
    length = int(bars * 4 * beat * RATE)
    buffer = array.array('d', [0.0]) * length
    chords = [('F3', 'A3', 'C4', 'E4'), ('D3', 'F3', 'A3', 'C4'), ('Bb2', 'D3', 'F3', 'A3'), ('C3', 'E3', 'G3', 'Bb3')]
    roots = ['F2', 'D2', 'Bb1', 'C2']
    # Pads and bass: one chord per two bars, four chords, twice.
    for bar in range(0, bars, 2):
        chord = chords[(bar // 2) % 4]
        at = int(bar * 4 * beat * RATE)
        render_pad(buffer, at, [note(n) for n in chord], 8 * beat, 0.16)
        for half in range(4):
            render_bass(buffer, at + int(half * 2 * beat * RATE), note(roots[(bar // 2) % 4]), 2 * beat, 0.18)
    # Melody: a pentatonic phrase that answers itself, bells over the pad.
    scale = ['F4', 'G4', 'A4', 'C5', 'D5', 'F5', 'G5', 'A5']
    phrase_a = [(0, 2, 1.0), (1, 3, 0.75), (2, 4, 1.0), (3.5, 3, 0.5), (4, 5, 1.5), (6, 4, 0.5), (6.5, 3, 0.5), (7, 2, 1.0)]
    phrase_b = [(0, 4, 1.0), (1, 5, 0.75), (2, 6, 1.0), (3.5, 5, 0.5), (4, 4, 1.5), (6, 3, 0.5), (6.5, 2, 0.5), (7, 1, 1.0)]
    phrase_c = [(0, 3, 0.5), (0.5, 4, 0.5), (1, 5, 1.0), (2.5, 4, 0.5), (3, 3, 1.0), (4, 2, 0.5), (4.5, 1, 0.5), (5, 2, 1.5), (7, 0, 1.0)]
    plan = [phrase_a, phrase_b, phrase_a, phrase_c, phrase_b, phrase_a, phrase_c, None]
    for index, phrase in enumerate(plan):
        if phrase is None: continue   # the last two bars rest, so the loop breathes
        start = int(index * 2 * 4 * beat * RATE)
        for offset, degree, hold in phrase:
            gain = 0.30 + rng.uniform(-0.03, 0.03)
            render_bell(buffer, start + int(offset * beat * RATE), note(scale[degree]), hold * beat + 1.4, gain, rng)
    # A quiet sparkle: eighth-note arpeggio an octave up, every other bar.
    for bar in range(1, bars, 2):
        chord = chords[(bar // 2) % 4]
        for step in range(8):
            at = int((bar * 4 + step * 0.5) * beat * RATE)
            render_bell(buffer, at, note(chord[step % 4]) * 2, 0.9, 0.055, rng)
    # Space: a dotted-eighth echo and a longer wash, both fed back softly.
    out = list(buffer)
    for delay, feedback, mix in ((int(0.75 * beat * RATE), 0.32, 0.22), (int(1.5 * beat * RATE), 0.22, 0.14)):
        echo = [0.0] * len(out)
        for i in range(len(out)):
            source = out[i - delay] + feedback * echo[i - delay] if i >= delay else 0.0
            echo[i] = source
        out = [o + mix * e for o, e in zip(out, echo)]
    # Wrap the echo tail into the start so the loop is seamless.
    tail = int(2.0 * RATE)
    wrapped = out[:]
    for i in range(tail):
        t = i / tail
        wrapped[i] += out[len(out) - tail + i] * (1 - t)
    # A short crossfade removes the last step at the seam.
    return normalise(loop_crossfade(wrapped, 0.05), 0.62)


# ------------------------------------------------------------------ encode

def encode_adpcm(pcm_path, out_path):
    if not shutil.which('ffmpeg'): raise SystemExit('ffmpeg is needed to encode the ADPCM file')
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', str(pcm_path), '-ac', '1', '-ar', str(RATE), '-c:a', 'adpcm_ima_wav', '-block_size', '1024', str(out_path)], check=True)


def verify(path):
    """Parse the result the way the launcher does and report its shape."""
    data = path.read_bytes()
    assert data[:4] == b'RIFF' and data[8:12] == b'WAVE', 'not a WAVE file'
    at = 12; fmt = None; size = 0
    while at + 8 <= len(data):
        chunk, length = data[at:at + 4], struct.unpack('<I', data[at + 4:at + 8])[0]
        body = data[at + 8:at + 8 + length]
        if chunk == b'fmt ': fmt = struct.unpack('<HHIIHH', body[:16]) + (struct.unpack('<H', body[18:20])[0] if length >= 20 else 0,)
        if chunk == b'data': size = length
        at += 8 + length + (length & 1)
    assert fmt and fmt[0] == 0x11 and fmt[1] == 1 and fmt[2] == RATE and fmt[5] == 4, fmt
    block, per_block = fmt[4], fmt[6] or (fmt[4] - 4) * 2 + 1
    seconds = size // block * per_block / RATE
    return seconds, size


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--source', type=Path, help='a downloaded track to convert (M4A, MP3, WAV)')
    parser.add_argument('--synth', action='store_true', help='render the bundled original composition')
    parser.add_argument('--start', type=float, default=0.0, help='seconds to skip at the start of --source')
    parser.add_argument('--duration', type=float, default=0.0, help='seconds to keep from --source (0: all)')
    parser.add_argument('--crossfade', type=float, default=1.5, help='loop crossfade in seconds for --source')
    parser.add_argument('--output', type=Path, default=ROOT / 'assets/music/menu.wav')
    parser.add_argument('--keep-pcm', type=Path, help='also write the 16-bit PCM version here')
    args = parser.parse_args()
    if bool(args.source) == args.synth: raise SystemExit('Choose --source FILE or --synth')
    if args.source:
        samples = normalise(loop_crossfade(read_pcm(args.source, args.start, args.duration), args.crossfade), 0.62)
    else:
        samples = compose()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as temp:
        pcm = Path(temp) / 'menu-pcm.wav'
        write_pcm_wav(pcm, samples)
        if args.keep_pcm: shutil.copy(pcm, args.keep_pcm)
        encode_adpcm(pcm, args.output)
    seconds, size = verify(args.output)
    print(f'{args.output}: {seconds:.1f} s, {size // 1024} KB of ADPCM')


if __name__ == '__main__':
    main()
