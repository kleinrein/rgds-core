"""Deferred missing-adapter recovery, preserving every existing wireless interface."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

VENDOR_HELPER = Path("/usr/bin/wifibt-init.sh")
# Owner's unmodified vendor script, inspected from the device on 2026-10-07.
# Its start_wifi branch skips Bluetooth; full start/restart do not.
REVIEWED_HELPERS = frozenset({"3d3f209218f5ce7022826aac155a6ba97333649f8dea26a43df0b93fdbcdfc73"})


def capture_helper(system_root="/"):
    path = Path(system_root) / str(VENDOR_HELPER).lstrip("/")
    try:
        digest = hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() and os.access(path,os.X_OK) else None
    except OSError:digest = None
    return dict(schema=1, helper=str(VENDOR_HELPER), sha256=digest, reviewed=digest in REVIEWED_HELPERS)


def wireless_state(sysfs=Path("/sys")):
    """Return real non-p2p interfaces and whether an existing PHY must be left alone."""
    sysfs = Path(sysfs);interfaces = [];phy = False
    for entry in (sysfs / "class/net").iterdir():
        wireless = (entry / "wireless").is_dir() or (entry / "phy80211").exists()
        if not wireless:
            try:wireless = "DEVTYPE=wlan" in (entry / "uevent").read_text().splitlines()
            except FileNotFoundError:pass
        if wireless:
            phy = True
            if not entry.name.startswith("p2p"):interfaces.append(entry.name)
    radio = sysfs / "class/ieee80211"
    if radio.exists():phy = phy or any(radio.iterdir())
    return sorted(interfaces),phy


def _stop_parent(process):
    # Signal only the unreaped process we launched. The vendor's background
    # initialization follows its own bounded wait; never kill a network group.
    if process.poll() is not None:return
    process.terminate()
    try:process.wait(timeout=.5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=.5)


def _wait_for_existing_phy(sysfs,clock,sleep,timeout):
    deadline=clock()+timeout
    while clock()<deadline:
        sleep(min(.1,max(0,deadline-clock())))
        interfaces,_phy=wireless_state(sysfs)
        if interfaces:return dict(state="existing",interfaces=interfaces)
    raise ValueError("A Wi-Fi PHY has no usable interface; keeping it untouched.")


def ensure_boot_network(configuration, sysfs=Path("/sys"), helper=VENDOR_HELPER,
                        launch=None, clock=None, sleep=None, timeout=10, phy_timeout=2):
    """Use only the approved Wi-Fi-only entry; call outside the launcher's startup path."""
    clock = clock or time.monotonic;sleep = sleep or time.sleep
    interfaces,phy = wireless_state(sysfs)
    if interfaces:return dict(state="existing",interfaces=interfaces)
    if phy:return _wait_for_existing_phy(sysfs,clock,sleep,min(phy_timeout,timeout))

    guard = configuration.get("network_guard",{})
    if not isinstance(guard,dict) or guard.get("schema") != 1 or guard.get("helper") != str(VENDOR_HELPER):
        raise ValueError("No verified Wi-Fi-only boot helper is configured.")
    expected = guard.get("sha256")
    if not isinstance(expected,str) or expected not in REVIEWED_HELPERS:
        raise ValueError("The configured Wi-Fi helper has not been reviewed.")
    helper = Path(helper)
    if not helper.is_file() or not os.access(helper,os.X_OK) or hashlib.sha256(helper.read_bytes()).hexdigest()!=expected:
        raise ValueError("The vendor Wi-Fi helper changed since configuration.")
    # Stock initialization can finish while the helper is being verified.
    interfaces,phy = wireless_state(sysfs)
    if interfaces:return dict(state="existing",interfaces=interfaces)
    if phy:return _wait_for_existing_phy(sysfs,clock,sleep,min(phy_timeout,timeout))
    environment = os.environ.copy()
    for key in tuple(environment):
        if key.startswith(("SIMPLEOS_","RGDS_")) or key in ("LD_PRELOAD","LD_LIBRARY_PATH"):
            environment.pop(key,None)
    environment["PATH"]="/usr/sbin:/usr/bin:/sbin:/bin"
    deadline = clock()+timeout
    process = (launch or subprocess.Popen)([str(helper),"start_wifi"],env=environment,
              stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,start_new_session=True)
    try:
        result = process.wait(timeout=min(2,max(.1,deadline-clock())))
    except BaseException:
        _stop_parent(process)
        raise
    if result:
        interfaces,_phy = wireless_state(sysfs)
        if interfaces:return dict(state="existing",interfaces=interfaces)
        raise ValueError("The vendor Wi-Fi-only initializer failed.")
    # The verified script backgrounds its worker. Parent exit0 is not readiness.
    while True:
        interfaces,_phy = wireless_state(sysfs)
        if interfaces:return dict(state="initialized",interfaces=interfaces)
        if clock()>=deadline:break
        sleep(min(.1,max(0,deadline-clock())))
    raise ValueError("Wi-Fi initialization did not create an interface in time.")


def quiet_wireless_logs(proc=Path("/proc/net")):
    """Keep the vendor Realtek driver to errors only for this kernel boot.

    The stock driver logs at INFO and writes several kernel lines per second
    while associated, waking klogd/syslogd and filling the tmpfs log. The
    setting is runtime-only; the next boot starts from the vendor default.
    """
    quieted = []
    try:entries = sorted(Path(proc).glob("rtl*/log_level"))
    except OSError:return quieted
    for entry in entries:
        try:
            current = entry.read_text().splitlines()
            level = next((line.split(":", 1)[1].strip() for line in current if line.startswith("drv_log_level:")), "")
            if level.isdigit() and int(level) > 2:
                entry.write_text("2\n");quieted.append(entry.parent.name)
        except OSError:continue
    return quieted


def run_deferred(configuration, result, recover=None):
    """Run in a worker after the first frame; network failure cannot stop RGDS."""
    began = time.monotonic()
    try:
        config = json.loads(Path(configuration).read_text())
        if not isinstance(config, dict):
            raise ValueError("Invalid boot network configuration.")
        record = dict(schema=1, **(recover or ensure_boot_network)(config))
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        record = dict(schema=1, state="unavailable", interfaces=[], error=str(error))
    quiet = quiet_wireless_logs()
    if quiet:record["quiet_logs"] = quiet
    record["elapsed_seconds"] = round(time.monotonic() - began, 3)
    # Readers see either the previous complete result or the new complete result.
    # This is ephemeral boot diagnostics, so it never adds a disk sync to startup.
    result = Path(result)
    pending = None
    try:
        result.parent.mkdir(parents=True, exist_ok=True)
        descriptor, pending = tempfile.mkstemp(prefix="." + result.name + "-", dir=result.parent)
        with os.fdopen(descriptor, "w") as stream:
            json.dump(record, stream)
            stream.write("\n")
        os.replace(pending, result)
    except OSError:
        pass  # Diagnostic storage is optional; recovery has already completed.
    finally:
        if pending is not None:
            try:Path(pending).unlink(missing_ok=True)
            except OSError:pass
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--deferred", required=True, type=Path, metavar="CONFIG",
                        help="Recover networking after the launcher has shown its first frame")
    parser.add_argument("--result", required=True, type=Path)
    args = parser.parse_args()
    return int(run_deferred(args.deferred, args.result)["state"] == "unavailable")


if __name__ == "__main__":
    sys.exit(main())
