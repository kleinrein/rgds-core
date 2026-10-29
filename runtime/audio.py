"""One saved volume for the menu and all engines on the RG DS RK817 codec (or the RG DS Plus's AW88166 speakers)."""
import argparse
from contextlib import contextmanager
import fcntl
import math
import os
from pathlib import Path
import re
import select
import struct
import subprocess
import sys
import threading
import time

from fields import read_fields, write_fields


def clean_environment():
    env = os.environ.copy()
    for key in ("LD_PRELOAD", "LD_LIBRARY_PATH"):
        env.pop(key, None)
    return env


BT_SINK = Path("/run/rgds/bt-sink")
BT_AUDIO = Path("/usr/libexec/rgds/bt-audio")
# RG DS OS: "<card> rk817|aw88166", the card that plays now (/usr/libexec/rgds/audio); absent: the RK817 as card 0
AUDIO_MIXER = Path("/run/rgds/audio-mixer")


CAPTURE_CONF = Path("/run/rgds/audio-capture.conf")


def capture_device(conf=CAPTURE_CONF):
    """The microphone's ALSA device: RG DS OS names it (the RK817 is not card 0 on every RG DS Plus), stock has the
    codec as card 0."""
    return "rgds_mic" if conf.is_file() else "plughw:0,0"


def dac_percent(level):
    # RK817's lowest ~30% lies below the speaker floor. Zero remains mute.
    return 0 if level == 0 else 30 + round(70 * (level - 1) / 99)


def mixer_target(path=AUDIO_MIXER):
    try:
        card, kind = path.read_text().split()[:2]
        return int(card), kind
    except (OSError, ValueError):
        return 0, "rk817"


class Mixer:
    # The stock kernel names the RK817's playback volume DAC; mainline's codec driver (RG DS OS, BananaLinux) calls
    # the same register Master.
    CONTROLS = ("DAC", "Master")

    def __init__(self, target=mixer_target):
        self.control = None
        self.target = target
        self.card = None

    def query(self):
        card, kind = self.target()
        if card != self.card:
            self.card, self.control = card, None
        if kind == "aw88166":
            return dac_percent(Amplifier(card).level())
        for control in ((self.control,) if self.control else self.CONTROLS):
            result = subprocess.run(
                ["amixer", "-c", str(card), "sget", control],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                env=clean_environment(),
                timeout=2
            )
            levels = [int(v) for v in re.findall(r"\[(\d+)%\]", result.stdout)]
            if result.returncode == 0 and levels:
                self.control = control
                return max(levels)
        raise ValueError("DAC playback mixer unavailable")

    def set(self, level):
        card, kind = self.target()
        if kind == "aw88166":
            Amplifier(card).set(level)
            return
        if not self.control or card != self.card:
            self.query()
        subprocess.run(
            ["amixer", "-q", "-c", str(card), "sset", self.control, f"{dac_percent(level)}%"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            env=clean_environment(),
            check=True,
            timeout=2
        )


class Amplifier:
    """The RG DS Plus's two AW88166 speaker amplifiers. Their control is an attenuation, not a level: 0 the loudest,
    1023 mute, 64 a 6 dB step (BananaLinux's measurements). Level 1-100 spans 50 dB, 0 is mute. The kernel calls the
    controls "Left PCM" and "Right PCM" (or one "PCM")."""
    CONTROLS = (("Left PCM", "Right PCM"), ("PCM",))

    def __init__(self, card, run=None):
        self.card = card
        self.run = run or subprocess.run

    @staticmethod
    def raw(level):
        return 1023 if level <= 0 else round((100 - min(100, level)) * 16 / 3)

    @staticmethod
    def from_raw(value):
        return 0 if value >= 1023 else max(1, min(100, round(100 - value * 3 / 16)))

    def get(self, control):
        result = self.run(["amixer", "-c", str(self.card), "sget", control],
                          text=True,
                          stdout=subprocess.PIPE,
                          stderr=subprocess.DEVNULL,
                          env=clean_environment(),
                          timeout=2)
        match = re.search(r"Playback (\d+) \[", result.stdout or "")
        return int(match[1]) if result.returncode == 0 and match else None

    def controls(self):
        for names in self.CONTROLS:
            if self.get(names[0]) is not None:
                return names
        raise ValueError("AW88166 playback control unavailable")

    def level(self):
        return self.from_raw(self.get(self.controls()[0]))

    def set(self, level):
        for control in self.controls():
            self.run(["amixer", "-q", "-c", str(self.card), "sset", control,
                      str(self.raw(level))],
                     stdout=subprocess.DEVNULL,
                     stderr=subprocess.DEVNULL,
                     env=clean_environment(),
                     check=True,
                     timeout=2)


def headset_volume(level):
    """A Bluetooth headset (RG DS OS, /usr/libexec/rgds/bt-audio) follows the saved level on its own control."""
    if BT_SINK.is_file() and BT_AUDIO.is_file():
        subprocess.run(
            [str(BT_AUDIO), "volume", str(level)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            env=clean_environment(),
            timeout=5,
            check=False
        )


class Volume:
    def __init__(self, data, mixer=None):
        self.data = Path(data)
        self.path = self.data / "volume.ini"
        self.mixer = mixer if mixer is not None else Mixer()

    def saved(self):
        try:
            fields = read_fields(self.path)
            value = int(fields["volume"])
            return value if fields.get("schema") == "1" and 0 <= value <= 100 else None
        except (OSError, ValueError, KeyError):
            return None

    def initial(self):
        percent = self.mixer.query()
        if percent < 30:
            return 0
        level = max(1, min(100, 1 + round((percent - 30) * 99 / 70)))
        from linux_platform import independent
        if independent():
            return min(25, level)
        # Stock's frontend sometimes attenuates in software while leaving DAC at
        # 100%. Use its remembered dB value when available, and adopt quietly.
        for path in (
            Path("/.config/retroarch/retroarch_volume.cfg"), Path("/root/.config/retroarch/retroarch_volume.cfg")
        ):
            try:
                match = re.search(r'audio_volume\s*=\s*"?(-?\d+(?:\.\d+)?)', path.read_text())
                if match:
                    db = float(match[1])
                    if math.isfinite(db):
                        level = min(level, 0 if db <= -60 else round(100 * 10**(min(0, db) / 20)))
                    break
            except OSError:
                pass
        return min(25, level)

    def status(self):
        # Query even when saved so an absent codec is reported as unavailable.
        percent = self.mixer.query()
        saved = self.saved()
        if saved is not None:
            return saved
        return self.initial() if percent else 0

    @contextmanager
    def locked(self):
        folder = self.data / "session"
        folder.mkdir(parents=True, exist_ok=True)
        with (folder / "volume.lock").open("a") as stream:
            fcntl.flock(stream, fcntl.LOCK_EX)
            yield

    def apply(self, value=None, delta=0):
        if value is not None and not 0 <= value <= 100:
            raise ValueError("Volume must be 0–100 percent")
        with self.locked():
            saved = self.saved()
            previous = saved if saved is not None else self.initial()
            target = max(0, min(100, previous + delta)) if value is None else value
            # Check the actual mixer: opening a new sink may reset it to 100%.
            expected = dac_percent(target)
            if abs(self.mixer.query() - expected) > 1:
                self.mixer.set(target)
            if saved != target:
                try:
                    write_fields(self.path, {"schema": "1", "volume": str(target)})
                except OSError:
                    self.mixer.set(previous)
                    raise
                headset_volume(target)
            return target


EVENT = struct.Struct("llHHi")


def volume_inputs(directory=Path("/dev/input")):
    """Open only devices advertising KEY_VOLUMEUP / KEY_VOLUMEDOWN; never grab."""
    descriptors = []
    for path in sorted(directory.glob("event*")):
        descriptor = -1
        try:
            descriptor = os.open(path, os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC)
            bits = bytearray(96)
            fcntl.ioctl(descriptor, 0x80604521, bits)  # EVIOCGBIT(EV_KEY, 96)
            if any(bits[code // 8] & (1 << (code % 8)) for code in (114, 115)):
                descriptors.append(descriptor)
            else:
                os.close(descriptor)
        except OSError:
            if descriptor >= 0:
                os.close(descriptor)
    return descriptors


class VolumeWatch:
    """Bound to the emulator runner, with no independently surviving worker."""
    def __init__(self, data, volume=None):
        self.volume = volume or Volume(data)
        self.stop = threading.Event()
        self.thread = None
        self.descriptors = []
        self.held = {}  # code -> [pressed at, next repeat]; the keys send no autorepeat.

    def start(self):
        (self.volume.data / "session/volume-hud.ini").unlink(missing_ok=True)
        self.volume.apply()
        self.descriptors = volume_inputs()
        self.thread = threading.Thread(target=self.watch, name="rgds-volume", daemon=True)
        self.thread.start()

    def step(self, code):
        level = self.volume.apply(delta=5 if code == 115 else -5)
        # The native bridge renders this on the top screen while playing.
        # A token also refreshes the HUD when a key hits 0% or 100% again.
        write_fields(
            self.volume.data / "session/volume-hud.ini",
            dict(schema=1, volume=level, token=f"{os.getpid()}-{time.monotonic_ns()}"),
            durable=False
        )

    def handle(self, data, now):
        for offset in range(0, len(data) - EVENT.size + 1, EVENT.size):
            _sec, _usec, kind, code, value = EVENT.unpack_from(data, offset)
            if kind != 1 or code not in (114, 115):
                continue
            if value == 0:
                self.held.pop(code, None)
            elif value == 1:
                self.held[code] = [now, now + .35]
                self.step(code)

    def repeat(self, now):
        """Keep stepping while a button stays down: slow at first, then faster."""
        for code, state in list(self.held.items()):
            if now - state[0] > 15:  # A release lost to a sleep must not run away.
                del self.held[code]
            elif now >= state[1]:
                held = now - state[0]
                state[1] = now + (.04 if held > 1.5 else .065 if held > .8 else .1)
                self.step(code)

    def watch(self):
        started = time.monotonic()
        retry = 0
        while not self.stop.is_set():
            try:
                readable, _, _ = select.select(self.descriptors, [], [], .02 if self.held else .1)
                for descriptor in readable:
                    self.handle(os.read(descriptor, EVENT.size * 32), time.monotonic())
                self.repeat(time.monotonic())
                # Also repair resets during startup/resume. Skip identical writes.
                if not self.held and time.monotonic() >= retry:
                    self.volume.apply()
                    retry = time.monotonic() + (.4 if time.monotonic() - started < 8 else 2)
            except (OSError, ValueError, subprocess.SubprocessError) as error:
                print(f"Volume control: {error}", file=sys.stderr, flush=True)
                self.stop.wait(.5)

    def close(self):
        self.stop.set()
        if self.thread:
            self.thread.join(timeout=5)
        for descriptor in self.descriptors:
            os.close(descriptor)
        self.descriptors = []


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("sync", "set"))
    parser.add_argument("value", nargs="?")
    parser.add_argument("--data", required=True, type=Path)
    args = parser.parse_args()
    volume = Volume(args.data)
    if args.command == "set":
        volume.apply(int(args.value))
    elif args.command == "sync":
        volume.apply()


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, TypeError, subprocess.SubprocessError) as error:
        print(f"Volume control: {error}", file=sys.stderr)
        sys.exit(1)
