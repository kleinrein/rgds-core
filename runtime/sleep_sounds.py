"""Finite lid cues for the shared menu/game suspend owner; no worker survives sleep."""
import array
import ctypes as C
import os
from pathlib import Path
import subprocess
import sys
import time
import wave

from audio import Volume
from fields import read_fields


def effects_volume(data):
    fields = read_fields(Path(data) / 'preferences.ini')
    fields.update(read_fields(Path(data) / 'ui.ini'))
    if fields.get('menu_sounds') != '1':
        return 0
    try:
        raw = fields.get('sound_volume', '55')
        value = int(raw)
        return value if raw.strip().lstrip('+').isdigit() and 0 <= value <= 100 else 55
    except ValueError:
        return 55


class AudioSpec(C.Structure):
    _fields_ = [
        ('freq', C.c_int), ('format', C.c_uint16), ('channels', C.c_uint8), ('silence', C.c_uint8),
        ('samples', C.c_uint16), ('padding', C.c_uint16), ('size', C.c_uint32), ('callback', C.c_void_p),
        ('userdata', C.c_void_p)
    ]


def load_sdl():
    candidates = [
        os.environ.get('RGDS_SDL_LIBRARY'), 'libSDL2-2.0.so.0', 'libSDL2.so', '/opt/homebrew/lib/libSDL2.dylib',
        '/usr/local/lib/libSDL2.dylib'
    ]
    for path in candidates:
        if not path:
            continue
        try:
            sdl = C.CDLL(path)
            break
        except OSError:
            pass
    else:
        raise OSError('SDL audio is unavailable')
    signatures = {
        'SDL_InitSubSystem': (C.c_int, [C.c_uint32]),
        'SDL_QuitSubSystem': (None, [C.c_uint32]),
        'SDL_OpenAudioDevice': (C.c_uint32, [C.c_char_p, C.c_int,
                                             C.POINTER(AudioSpec),
                                             C.POINTER(AudioSpec), C.c_int]),
        'SDL_CloseAudioDevice': (None, [C.c_uint32]),
        'SDL_PauseAudioDevice': (None, [C.c_uint32, C.c_int]),
        'SDL_QueueAudio': (C.c_int, [C.c_uint32, C.c_void_p, C.c_uint32]),
        'SDL_GetQueuedAudioSize': (C.c_uint32, [C.c_uint32]),
        'SDL_GetError': (C.c_char_p, []),
    }
    for name, (result, args) in signatures.items():
        function = getattr(sdl, name)
        function.restype, function.argtypes = result, args
    return sdl


def play(data, cue, assets=None):
    """Best effort, bounded to one short cue; mute/off never opens audio."""
    device = 0
    sdl = None
    initialized = False
    try:
        if cue not in ('lid-close', 'lid-open'):
            raise ValueError('Unknown sleep cue')
        level = effects_volume(data)
        volume = Volume(data)
        if not level or volume.saved() == 0:
            return False
        folder = Path(assets) if assets is not None else Path(__file__).resolve().parents[1] / 'assets/sounds'
        with wave.open(str(folder / (cue + '.wav')), 'rb') as clip:
            if (clip.getnchannels(), clip.getsampwidth(),
                clip.getframerate()) != (1, 2, 32000) or not 1 <= clip.getnframes() <= 19200:
                raise ValueError('Invalid sleep sound')
            samples = array.array('h', clip.readframes(clip.getnframes()))
        if sys.byteorder != 'little':
            samples.byteswap()
        # The RK817's hardware PCM accepts stereo only. Duplicate the mono
        # bank into both channels instead of relying on driver conversion.
        pcm = array.array('h', (round(value * level / 100 * .65) for value in samples for _channel in range(2)))
        if sys.byteorder != 'little':
            pcm.byteswap()
        raw = pcm.tobytes()
        sdl = load_sdl()
        if sdl.SDL_InitSubSystem(0x10) != 0:  # SDL_INIT_AUDIO, no video or input.
            raise OSError('Cannot initialize sleep audio: ' + sdl.SDL_GetError().decode(errors='replace'))
        initialized = True
        wanted = AudioSpec(freq=32000, format=0x8010, channels=2, samples=512)  # AUDIO_S16LSB
        obtained = AudioSpec()
        device = sdl.SDL_OpenAudioDevice(None, 0, C.byref(wanted), C.byref(obtained), 0)
        if not device:
            raise OSError('Cannot open sleep audio: ' + sdl.SDL_GetError().decode(errors='replace'))
        # A fresh sink can reset the RK817 DAC. Restore the saved level before
        # unpausing, including the first sound following firmware resume hooks.
        if volume.apply() == 0:
            return False
        buffer = C.create_string_buffer(raw)
        if sdl.SDL_QueueAudio(device, buffer, len(raw)) != 0:
            raise OSError('Cannot queue sleep audio: ' + sdl.SDL_GetError().decode(errors='replace'))
        sdl.SDL_PauseAudioDevice(device, 0)
        deadline = time.monotonic() + len(samples) / 32000 + .3
        while sdl.SDL_GetQueuedAudioSize(device):
            if time.monotonic() >= deadline:
                raise OSError('Sleep audio timed out')
            time.sleep(.005)
        time.sleep(.04)  # Let the final submitted hardware buffer leave the DAC.
        return True
    except (OSError, ValueError, AttributeError, wave.Error, subprocess.SubprocessError) as error:
        print('Sleep sound: ' + str(error), file=sys.stderr, flush=True)
        return False
    finally:
        if device:
            sdl.SDL_CloseAudioDevice(device)
        if initialized:
            sdl.SDL_QuitSubSystem(0x10)
