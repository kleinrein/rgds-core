#!/usr/bin/env python3
"""Run the experimental probe on Linux ARM64; write separate VM evidence.

No RG DS access, SDL display or game files are required. This is a core API
check, not device performance or physical touchscreen/audio validation.
"""
import argparse
import json
from pathlib import Path
import platform
import resource
import subprocess
import time

try:
    from .build_melonds import COMMIT, INPUTS_SHA256, ROOT, validate_build
except ImportError:
    from build_melonds import COMMIT, INPUTS_SHA256, ROOT, validate_build


def probe(directory, report):
    if platform.system() != "Linux" or platform.machine() not in ("aarch64", "arm64"):
        raise ValueError("Run the probe inside the Linux ARM64 builder")
    if report.exists() or report.is_symlink():
        raise ValueError("Use a fresh report path; existing evidence is preserved")
    record = json.loads((directory / "build.json").read_text())
    abi = validate_build(directory)
    if (
        record.get("kind") != "experimental-melonds-core-probe" or record.get("binary") != abi
        or record.get("source", {}).get("commit") != COMMIT
        or record.get("source", {}).get("inputs_sha256") != INPUTS_SHA256
        or record.get("selectable_engine") is not False
    ):
        raise ValueError("Experimental probe provenance does not match its executable")
    start = time.monotonic()
    result = subprocess.run([str(directory / "rgds-melonds-probe")], capture_output=True, text=True, timeout=30)
    elapsed = time.monotonic() - start
    rows = [json.loads(line) for line in result.stdout.splitlines()]
    if (
        result.returncode or len(rows) != 2 or {row.get("mode")
                                                for row in rows} != {"interpreter", "arm64-jit"}
        or not all(row.get("passed") is True and row.get("frames") == 13 for row in rows)
    ):
        raise ValueError("melonDS probe failed: " + result.stderr[-4000:])
    evidence = {
        "schema": 1,
        "kind": "experimental-melonds-linux-core-check",
        "source_commit": COMMIT,
        "binary_sha256": abi["sha256"],
        "runtime_tested": True,
        "device_tested": False,
        "selectable_engine": False,
        "system": platform.platform(),
        "machine": platform.machine(),
        "elapsed_seconds": elapsed,
        "peak_child_resident_kib": resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss,
        "checks": rows,
        "stderr": result.stderr,
        "limitations": [
            "synthetic homebrew only", "memory checkpoint only", "no hardware or battery-save validation",
            "no performance comparison to RG DS"
        ],
    }
    report.parent.mkdir(parents=True, exist_ok=True)
    with report.open("x") as stream:
        json.dump(evidence, stream, indent=2, sort_keys=True)
        stream.write("\n")
    print(json.dumps({"report": str(report), "checks": rows}, indent=2))
    return evidence


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=ROOT / "build/melonds-experiment")
    parser.add_argument("--report", type=Path, default=ROOT / "build/melonds-experiment/probe.json")
    args = parser.parse_args()
    probe(args.build.resolve(), args.report.resolve())


if __name__ == "__main__":
    main()
