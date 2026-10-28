#!/usr/bin/env python3
"""Save-to-refresh development in the desktop preview (the handheld side is tools/own_os_push.py)."""
import argparse
import hashlib
from pathlib import Path
import signal
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]


def snapshot(paths):
    """Content hashes also catch atomic editor saves and preserved timestamps."""
    result = []
    for path in sorted(set(paths)):
        try:
            result.append((str(path), hashlib.sha256(path.read_bytes()).hexdigest()))
        except FileNotFoundError:
            result.append((str(path), None))
    return tuple(result)


def source_paths():
    paths = [ROOT / "tools/build.py"]
    paths += [p for p in (ROOT / "assets/fonts").glob("*.rgf")]
    paths += [p for p in (ROOT / "src").rglob("*") if p.suffix in {".cpp", ".hpp", ".h"}]
    return paths


class Changes:
    def __init__(self, initial, now, debounce=0.35):
        self.seen = self.emitted = initial
        self.changed_at = now
        self.debounce = debounce

    def poll(self, current, now):
        if current != self.seen:
            self.seen, self.changed_at = current, now
        if current != self.emitted and now - self.changed_at >= self.debounce:
            self.emitted = current
            return True
        return False


def build():
    began = time.monotonic()
    try:
        subprocess.run([sys.executable, str(ROOT / "tools/build.py"), "host"], cwd=ROOT, check=True)
    except subprocess.CalledProcessError:
        print("Build failed; keeping the current menu. Fix the error and save again.", flush=True)
        return False
    print(f"Build ready in {time.monotonic() - began:.1f}s.", flush=True)
    return True


def stop_preview(child):
    if child and child.poll() is None:
        child.terminate()
        try:
            child.wait(timeout=3)
        except subprocess.TimeoutExpired:
            child.kill()
            child.wait()


def preview(theme, roms):
    command = [
        str(ROOT / "build/host/rgds-launcher"), "--preview", "--watch-assets", "--theme",
        str(theme), "--state",
        str(ROOT / "build/dev/menu-state.txt"), "--data",
        str(ROOT / "build/dev")
    ]
    for root in roms:
        command += ["--rom-dir", str(root)]
    return subprocess.Popen(command, cwd=ROOT)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--theme", type=Path, default=ROOT / "themes/default/light.ini")
    parser.add_argument("--rom-dir", type=Path, action="append", default=[])
    args = parser.parse_args()
    args.theme = args.theme.resolve()
    if not args.theme.is_file():
        raise ValueError("Theme file is missing: " + str(args.theme))
    sources = Changes(snapshot(source_paths()), time.monotonic())
    child = None
    try:
        if build():
            child = preview(args.theme, args.rom_dir)
        print("Watching. Themes reload in place; source saves rebuild. Ctrl+C stops the watcher.", flush=True)
        while True:
            if sources.poll(snapshot(source_paths()), time.monotonic()) and build():
                stop_preview(child)
                child = preview(args.theme, args.rom_dir)
            if child and child.poll() is not None:
                print("Preview closed. Save source to reopen it, or stop with Ctrl+C.", flush=True)
                child = None
            time.sleep(0.2)
    finally:
        stop_preview(child)


if __name__ == "__main__":

    def interrupt(_signum, _frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, interrupt)
    try:
        main()
    except KeyboardInterrupt:
        print("Watcher stopped.")
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print("RGDS dev: " + str(error), file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError) and error.stderr:
            print(error.stderr.strip(), file=sys.stderr)
        sys.exit(1)
