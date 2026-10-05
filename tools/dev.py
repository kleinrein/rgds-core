#!/usr/bin/env python3
"""Save-to-refresh development for the desktop preview or an explicit stock RG DS."""
import argparse
import hashlib
from pathlib import Path
import signal
import subprocess
import sys
import time

import device as transport

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


def source_paths(on_device, theme):
    paths = [ROOT / "tools/build.py"]
    paths += [p for p in (ROOT / "assets/fonts").glob("*.rgf")]
    for folder, suffixes in (("src", {".cpp", ".hpp", ".h"}),):
        paths += [p for p in (ROOT / folder).rglob("*") if p.suffix in suffixes]
    if on_device:
        for folder, suffixes in (("runtime", {".py", ".sh"}), ("packaging", {".sh"}), ("themes", {".ini"})):
            paths += [p for p in (ROOT / folder).rglob("*") if p.suffix in suffixes and p.resolve() != theme]
        paths += [ROOT / "tools/package.py", ROOT / "VERSION", ROOT / "THIRD_PARTY.md"]
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


def build(on_device):
    began = time.monotonic()
    try:
        subprocess.run([sys.executable, str(ROOT / "tools/build.py"), "device" if on_device else "host"], cwd=ROOT, check=True)
        if on_device:
            subprocess.run([sys.executable, str(ROOT / "tools/package.py")], cwd=ROOT, check=True)
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
    command = [str(ROOT / "build/host/rgds-launcher"), "--preview", "--watch-assets", "--theme", str(theme),
               "--state", str(ROOT / "build/dev/menu-state.txt"), "--data", str(ROOT / "build/dev")]
    for root in roms:
        command += ["--rom-dir", str(root)]
    return subprocess.Popen(command, cwd=ROOT)


class DeviceQueue:
    """Coalesce saves into the newest build; never restart an active game."""
    def __init__(self, device, root, theme):
        self.device, self.root, self.theme = device, root, theme
        self.archive = None
        self.theme_pending = False
        self.message = None

    def announce(self, message):
        if self.message != message:
            print(message, flush=True)
            self.message = message

    def flush(self):
        if not self.archive and not self.theme_pending:
            return
        status = transport.status(self.device, self.root, batch=True)
        if status.get("record_errors"):
            self.announce("Activity records are unreadable; updates are queued safely. Run make doctor DEVICE=" + self.device + " after returning to stock.")
            return
        if status["recovery"]:
            raise RuntimeError("userdata/boot_stock is set. Remove it when you want to resume device development.")
        if status["game"]:
            self.announce("Update queued. Exit the game normally; the newest build will load at the menu.")
            return
        if status["lock"] and (not status["session"] or not status["launcher"]):
            self.announce("Waiting for the previous RGDS session to recover.")
            return
        if self.archive:
            self.announce("Deploying " + self.archive.name + "…")
            # The installer checks the game lock again, covering a launch racing this status call.
            transport.deploy(self.device, self.root, self.archive, batch=True)
        theme_uploaded = False
        if self.theme_pending:
            try:
                transport.sync_theme(self.device, self.root, self.theme, batch=True)
                theme_uploaded = True
            except (ValueError, OSError) as error:
                self.announce("Theme edit rejected; keeping the device's current theme. " + str(error))
            self.theme_pending = False
        if self.archive:
            transport.start_dev_session(self.device, self.root)
            self.archive = None
            self.announce("Device development menu is running. Save a theme or source file to refresh.")
        elif theme_uploaded:
            self.announce("Live theme uploaded. The running development menu will reload it.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", help="Enable SSH watch mode, e.g. root@192.168.1.42")
    parser.add_argument("--sd-root", type=Path, default=Path("/mnt/mmc"))
    parser.add_argument("--theme", type=Path, default=ROOT / "themes/default/light.ini")
    parser.add_argument("--rom-dir", type=Path, action="append", default=[])
    args = parser.parse_args()
    args.theme = args.theme.resolve()
    if not args.theme.is_file():
        raise ValueError("Theme file is missing: " + str(args.theme))
    if not args.sd_root.is_absolute() or ".." in args.sd_root.parts:
        raise ValueError("--sd-root must be an absolute mount path without ..")
    on_device = args.device is not None
    queue = None
    if on_device:
        address = transport.target(args.device)
        # Do not enter an unattended password or unknown-host prompt.
        transport.status(address, args.sd_root / "rgds", batch=True)
        queue = DeviceQueue(address, args.sd_root / "rgds", args.theme)
    now = time.monotonic()
    sources = Changes(snapshot(source_paths(on_device, args.theme)), now)
    themes = Changes(snapshot([args.theme]), now)
    child = None
    try:
        if build(on_device):
            if queue:
                queue.archive = transport.latest_package()
                queue.theme_pending = True
            else:
                child = preview(args.theme, args.rom_dir)
        print("Watching. Themes reload in place; source saves rebuild. Ctrl+C stops the watcher.", flush=True)
        next_remote = 0
        while True:
            now = time.monotonic()
            if sources.poll(snapshot(source_paths(on_device, args.theme)), now) and build(on_device):
                if queue:
                    queue.archive = transport.latest_package()
                    # Re-send the selected live theme after every full deployment.
                    queue.theme_pending = True
                else:
                    stop_preview(child)
                    child = preview(args.theme, args.rom_dir)
            if queue and themes.poll(snapshot([args.theme]), now):
                queue.theme_pending = True
                queue.message = None
            if queue and now >= next_remote:
                try:
                    queue.flush()
                    next_remote = time.monotonic() + 2
                except subprocess.CalledProcessError as error:
                    # Includes the installer's second game check. Keep the candidate queued.
                    queue.announce("Device update waiting; keeping the current session. "
                                   "Retrying in 5s. " + (error.stderr or "Check SSH/device logs.").strip())
                    next_remote = time.monotonic() + 5
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
        print("Watcher stopped. The device session, if running, stays available.")
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print("RGDS dev: " + str(error), file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError) and error.stderr:
            print(error.stderr.strip(), file=sys.stderr)
        sys.exit(1)
