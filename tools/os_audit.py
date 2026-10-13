#!/usr/bin/env python3
"""Capture OS migration evidence on the host, optionally with read-only SSH.

No deployment, game launch, restart, suspend, mount or disk-writing action is
available. Device facts and built ELF dependencies are separate evidence.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import sys

from device import target
from elf_abi import inspect_elf

ROOT = Path(__file__).resolve().parents[1]


def local_components():
    candidates = [ROOT / "build/device/rgds-launcher", ROOT / "build/device/librgdsbridge.so",
                  ROOT / "build/engines/dsperate/dsperate", ROOT / "vendor/engines/nnddss/nnddss"]
    candidates += sorted((ROOT / "vendor/engines/nnddss/lib").glob("*.so"))
    results = {}
    for path in candidates:
        label = path.relative_to(ROOT).as_posix()
        if not path.is_file():
            results[label] = {"error": "not built/provisioned"}
            continue
        try:
            results[label] = inspect_elf(path, source=label) or {"error": "not ELF"}
        except (OSError, ValueError) as error:
            results[label] = {"error": str(error)}
    return results


def probe(device, roots):
    for root in roots:
        if not root.is_absolute() or ".." in root.parts:
            raise ValueError("Installation roots must be absolute paths without '..'.")
    source = (ROOT / "tools/os_probe.py").read_text()
    command = "python3 -B -" + "".join(" --install-root " + shlex.quote(str(root)) for root in roots)
    result = subprocess.run(["ssh", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=yes",
        "-o", "IPQoS=none", "-o", "ConnectTimeout=10", "-o", "ServerAliveInterval=5",
        "-o", "ServerAliveCountMax=2", target(device), command], input=source,
        text=True, capture_output=True, check=True, timeout=60)
    evidence = json.loads(result.stdout)
    if (not isinstance(evidence, dict) or evidence.get("schema") != 1
            or evidence.get("read_only") is not True or not isinstance(evidence.get("system"), dict)
            or evidence["system"].get("machine") not in ("aarch64", "arm64")):
        raise ValueError("Unexpected OS probe schema or architecture; keep evidence unaccepted.")
    return evidence


def capture(output, device=None, roots=()):
    output = Path(output)
    # Reserve a fresh directory before SSH; never replace an earlier baseline.
    output.mkdir(parents=True, exist_ok=False)
    report = {"schema": 1, "project": "rgds", "purpose": "os-migration-baseline",
              "captured_at_utc": datetime.now(timezone.utc).isoformat(),
              "local_components": local_components(), "device": None,
              "limits": ["ELF metadata covers direct linkage, not dlopen or recursive dependency closure.",
                         "SSH inspection uses the running OS; it does not validate a replacement kernel.",
                         "No hardware acceptance tests were dispatched."]}
    failed = False
    if device:
        try:
            report["device"] = probe(device, roots)
        except (OSError, ValueError, subprocess.SubprocessError) as error:
            report["device_error"] = type(error).__name__ + ": " + str(error)
            if isinstance(error, subprocess.CalledProcessError):
                report["device_error"] += "\n" + (error.stderr or "")[:4096]
            failed = True
    report["device_capture_status"] = "failed" if failed else "captured" if device else "not requested"
    report["probe_source_sha256"] = hashlib.sha256((ROOT / "tools/os_probe.py").read_bytes()).hexdigest()
    encoded = json.dumps(report, indent=2, sort_keys=True) + "\n"
    (output / "audit.json").write_text(encoded)
    (output / "audit.sha256").write_text(hashlib.sha256(encoded.encode()).hexdigest() + "  audit.json\n")
    print("OS evidence saved to " + str(output / "audit.json"))
    return 1 if failed else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", help="Explicit SSH target; omit for local ELF audit only")
    parser.add_argument("--install-root", type=Path, action="append", default=[])
    parser.add_argument("--output", type=Path, default=ROOT / "build/os-audit" /
                        datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ"))
    args = parser.parse_args()
    if args.install_root and not args.device:
        parser.error("--install-root requires --device")
    try:
        return capture(args.output, args.device, args.install_root)
    except (OSError, ValueError) as error:
        print(error, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
