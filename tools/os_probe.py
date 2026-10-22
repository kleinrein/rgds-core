#!/usr/bin/env python3
"""Read-only Linux facts for OS bring-up; prints JSON, never starts hardware tests.

Run through SSH on stdin with python3 -B. No external programs, library loading,
device writes, service changes, game discovery or network-profile reads occur.
Missing facts remain null; presence is not a hardware acceptance result.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import stat
import sys

LIMIT = 256 * 1024
BUILD_ID = re.compile(r"[a-z0-9][a-z0-9.-]{0,100}\Z")
PROCESS_NAMES = {
    "rgds-launcher", "dsperate", "nnddss", "drastic", "weston", "dmenu.bin", "muos2.bin", "muos3.bin", "MainUI",
    "NetworkManager", "wpa_supplicant", "power_daemon"
}


def read(path, limit=LIMIT):
    try:
        with Path(path).open("rb") as stream:
            data = stream.read(limit + 1)
        if len(data) > limit:
            return None
        return data.decode("utf-8", errors="replace").strip()
    except OSError:
        return None


def pairs(value):
    result = {}
    for line in (value or "").splitlines():
        key, separator, item = line.partition("=")
        if separator and key in {"NAME", "VERSION", "ID", "VERSION_ID", "PRETTY_NAME"}:
            result[key] = item.strip('"')
    return result


def file_record(path):
    """Hash regular configuration/libraries only, never devices or huge files."""
    try:
        info = path.stat()
        if not stat.S_ISREG(info.st_mode) or info.st_size > 64 * 1024 * 1024:
            return {"path": str(path), "error": "not a bounded regular file"}
        digest = hashlib.sha256()
        total = 0
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                total += len(block)
                if total > 64 * 1024 * 1024:
                    return {"path": str(path), "error": "file grew beyond limit"}
                digest.update(block)
        return {"path": str(path), "resolved": str(path.resolve()), "bytes": total, "sha256": digest.hexdigest()}
    except OSError as error:
        return {"path": str(path), "error": type(error).__name__}


def mounts(value):
    result = []
    for line in (value or "").splitlines():
        fields = line.split()
        if len(fields) >= 4:
            # Omit options other than read/write: mount options can contain secrets.
            result.append(
                {
                    "source": fields[0],
                    "mount": fields[1],
                    "type": fields[2],
                    "writable": "rw" in fields[3].split(",")
                }
            )
    return result


def installation(root):
    active = read(root / "state/active", 128)
    result = {
        "root": str(root),
        "active": active,
        "previous": read(root / "state/previous", 128),
        "stock_marker": (root / "userdata/boot_stock").exists()
    }
    if active and BUILD_ID.fullmatch(active):
        app = root / "releases" / active
        result["session_present"] = (app / "runtime/session.py").is_file()
        manifest = app / "manifest.json"
        if manifest.is_file():
            result["manifest"] = file_record(manifest)
    elif active:
        result["error"] = "invalid active build ID; release not inspected"
    return result


def collect(roots=(), base=Path("/")):
    """base exists for fixture tests; normal collection always uses Linux /."""
    base = Path(base)

    def at(name):
        return base / name.lstrip("/")

    def values(patterns):
        result = {}
        for pattern in patterns:
            for path in sorted(base.glob(pattern.lstrip("/")))[:128]:
                result["/" + path.relative_to(base).as_posix()] = read(path, 8192)
        return result

    try:
        libc = os.confstr("CS_GNU_LIBC_VERSION")
    except (OSError, ValueError, AttributeError):
        libc = None
    mem = read(at("/proc/meminfo"), 8192)
    match = re.search(r"^MemTotal:\s+(\d+) kB$", mem or "", re.MULTILINE)
    processes = []
    proc = at("/proc")
    for entry in sorted(proc.glob("[0-9]*"))[:4096]:
        name = read(entry / "comm", 256)
        if name not in PROCESS_NAMES:
            continue
        mapped = set()
        for line in (read(entry / "maps") or "").splitlines():
            fields = line.split(maxsplit=5)
            if len(fields) == 6 and fields[5].startswith("/"):
                path = fields[5]
                if ".so" in Path(path).name:
                    mapped.add(path)
        processes.append({"pid": int(entry.name), "name": name, "mapped_libraries": sorted(mapped)})
    library_patterns = [
        "usr/lib/libSDL2*.so*",
        "usr/lib/aarch64-linux-gnu/libSDL2*.so*",
        "usr/lib/libEGL*.so*",
        "usr/lib/libGLES*.so*",
        "usr/lib/libmali*.so*",
        "usr/lib/libdrastouch*.so*",
        "usr/lib/aarch64-linux-gnu/libEGL*.so*",
        "usr/lib/aarch64-linux-gnu/libGLES*.so*",
        "mnt/vendor/deep/drastic_aarch64/libs/libSDL2*.so*",
        "mnt/vendor/deep/drastic64/libs/libSDL2*.so*",
    ]
    libraries = sorted({path for pattern in library_patterns for path in base.glob(pattern)})[:128]
    configs = [
        "/etc/init.d/S50launch", "/etc/init.d/S49weston", "/etc/init.d/S36wifibt-init.sh",
        "/etc/init.d/S45network-manager", "/etc/xdg/weston/weston.ini", "/etc/weston.ini", "/etc/pm/sleep.d/03wifibt",
        "/boot/extlinux/extlinux.conf", "/boot/armbianEnv.txt", "/boot/config.txt"
    ]
    model = read(at("/proc/device-tree/model"), 1024)
    compatible = read(at("/proc/device-tree/compatible"), 4096)
    return {
        "schema": 1,
        "read_only": True,
        "captured_at_utc": datetime.now(timezone.utc).isoformat(),
        "system": {
            "machine": platform.machine(),
            "kernel": platform.release(),
            "python": platform.python_version(),
            "libc": libc,
            "os_release": pairs(read(at("/etc/os-release"), 8192)),
            "boot_id": read(at("/proc/sys/kernel/random/boot_id"), 128),
            "uptime": read(at("/proc/uptime"), 128),
            "pid1": read(at("/proc/1/comm"), 256)
        },
        "hardware": {
            "model": model.rstrip("\0") if model else None,
            "compatible": compatible.rstrip("\0").split("\0") if compatible else [],
            "memory_kib": int(match[1]) if match else None,
            "model_profile": "unclassified; confirm physical model"
        },
        "mounts": mounts(read(at("/proc/mounts"))),
        "displays": values(["sys/class/drm/card*-*/status", "sys/class/drm/card*-*/modes"]),
        "input_devices": read(at("/proc/bus/input/devices")),
        "backlights": values(["sys/class/backlight/*/brightness", "sys/class/backlight/*/max_brightness"]),
        "power": values(
            [
                "sys/power/state", "sys/power/mem_sleep", "sys/class/power_supply/*/type",
                "sys/class/power_supply/*/status", "sys/class/power_supply/*/capacity",
                "sys/class/anbernic_misc/hallkey", "sys/devices/platform/anbernic_misc/power/wakeup"
            ]
        ),
        "clocks": values(
            [
                "sys/devices/system/cpu/cpufreq/policy*/scaling_available_frequencies",
                "sys/devices/system/cpu/cpufreq/policy*/scaling_available_governors",
                "sys/class/devfreq/*/available_frequencies", "sys/class/devfreq/*/available_governors"
            ]
        ),
        "block_devices": values(
            [
                "sys/class/block/mmcblk*/size", "sys/class/block/mmcblk*/ro", "sys/class/block/mmcblk*/removable",
                "sys/class/block/mmcblk*/partition"
            ]
        ),
        "processes": processes,
        "libraries": [file_record(path) for path in libraries],
        "configuration_hashes": [file_record(at(name)) for name in configs if at(name).is_file()],
        "init_entries": sorted(path.name for path in at("/etc/init.d").glob("*")),
        "suspend_hooks": sorted(
            str(path) for folder in ("/etc/pm/sleep.d", "/usr/lib/pm-utils/sleep.d") for path in at(folder).glob("*")
        ),
        "installations": [installation(Path(root)) for root in roots],
        "acceptance": {
            "replacement_kernel": "not tested",
            "dual_screen_render": "not tested",
            "gameplay": "not tested",
            "save_restore": "not tested",
            "suspend_wake": "not tested",
            "reboot_shutdown": "not tested"
        },
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--install-root", type=Path, action="append", default=[])
    args = parser.parse_args()
    if sys.platform != "linux":
        parser.error("Collect on the Linux device; use tools/os_audit.py on the host.")
    if any(not root.is_absolute() or ".." in root.parts for root in args.install_root):
        parser.error("Installation roots must be absolute paths without '..'.")
    print(json.dumps(collect(args.install_root), indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
