#!/usr/bin/env python3
"""Compare host release-validation overhead; this does not measure device boot time.

The baseline reproduces the previous selected startup verifier, including its
per-file path resolution and second rglob inventory, run once by boot.py and
again by session.py. The current route hashes once and checks a fresh receipt.
Neither measurement includes Python process startup, display presentation,
network initialization, the bootloader, or a cold device filesystem cache.
"""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import re
import statistics
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "runtime"))
import activate


def startup_file(name):
    return (
        name == "bin/rgds-launcher" or name.startswith("runtime/") or name.startswith("themes/")
        or name == "assets/shaders/catalog.tsv" or (name.startswith("assets/fonts/rounded-") and name.endswith(".rgf"))
    )


def previous_verify_startup(app):
    """Exact previous verify_release(app, startup_file) validation algorithm."""
    app = Path(app)
    if app.is_symlink() or (app / "manifest.json").is_symlink():
        raise ValueError("Unsafe release directory or manifest.")
    manifest = json.loads((app / "manifest.json").read_text())
    if (
        not isinstance(manifest, dict) or manifest.get("project") != "rgds" or manifest.get("schema") != 1
        or manifest.get("target") != "aarch64-linux-gnu"
    ):
        raise ValueError("Wrong project, schema, or architecture.")
    if manifest.get("build") != app.name or not activate.BUILD_ID.fullmatch(app.name):
        raise ValueError("Release directory does not match manifest.")
    files = manifest.get("files", {})
    if not isinstance(files, dict) or "bin/rgds-launcher" not in files:
        raise ValueError("Manifest has no launcher.")
    for name, digest in files.items():
        if not isinstance(name, str) or not name or not isinstance(digest, str):
            raise ValueError("Invalid manifest file entry.")
        path = Path(name)
        if (
            path.is_absolute() or ".." in path.parts or "\\" in name or str(path) != name
            or not re.fullmatch(r"[0-9a-f]{64}", digest)
        ):
            raise ValueError("Invalid manifest file entry.")
        source = app / path
        if (source.is_symlink() or not source.is_file() or app.resolve() not in source.resolve().parents):
            raise ValueError("Missing or unsafe release file: " + name)
        if startup_file(name) and hashlib.sha256(source.read_bytes()).hexdigest() != digest:
            raise ValueError("Release file failed SHA-256: " + name)
    actual = {str(path.relative_to(app)) for path in app.rglob("*") if path.is_file() and path != app / "manifest.json"}
    if actual != set(files):
        raise ValueError("Unlisted files in release.")
    with (app / "bin/rgds-launcher").open("rb") as stream:
        header = stream.read(20)
    if len(header) != 20 or header[:6] != b"\x7fELF\x02\x01" or header[18:20] != b"\xb7\x00":
        raise ValueError("Launcher is not a Linux AArch64 ELF.")
    return manifest


def previous_boot_validation(app):
    previous_verify_startup(app)
    previous_verify_startup(app)


def current_boot_validation(app):
    receipt = activate.verify_startup(app, receipt=True)
    activate.verify_startup_receipt(app, receipt)


def describe_samples(samples):
    return dict(
        median_ms=round(statistics.median(samples), 3),
        minimum_ms=round(min(samples), 3),
        maximum_ms=round(max(samples), 3),
        samples_ms=[round(value, 3) for value in samples]
    )


def benchmark(app, iterations, warmup):
    manifest = previous_verify_startup(app)
    activate.verify_startup(app)
    jobs = {"previous_duplicate_startup": previous_boot_validation, "current_hash_and_receipt": current_boot_validation}
    for _ in range(warmup):
        for work in jobs.values():
            work(app)
    samples = {name: [] for name in jobs}
    for iteration in range(iterations):
        order = list(jobs)
        if iteration % 2: order.reverse()
        for name in order:
            began = time.perf_counter()
            jobs[name](app)
            samples[name].append((time.perf_counter() - began) * 1000)
    results = {name: describe_samples(values) for name, values in samples.items()}
    previous = results["previous_duplicate_startup"]["median_ms"]
    current = results["current_hash_and_receipt"]["median_ms"]
    files = manifest["files"]
    try:
        source = str(app.resolve().relative_to(ROOT))
    except ValueError:
        source = str(app)
    return dict(
        schema=1,
        measurement="host_release_validation_warm_cache",
        hardware_claim=False,
        device_boot_timing_verified=False,
        app=source,
        build=app.name,
        manifest_sha256=hashlib.sha256((app / "manifest.json").read_bytes()).hexdigest(),
        verifier_source_sha256=hashlib.sha256((ROOT / "runtime/activate.py").read_bytes()).hexdigest(),
        benchmark_source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        python=platform.python_version(),
        host_os=platform.system(),
        host_arch=platform.machine(),
        iterations=iterations,
        warmup=warmup,
        manifest_files=len(files),
        payload_bytes=sum((app / name).stat().st_size for name in files),
        startup_hashed_bytes_per_pass=sum((app / name).stat().st_size for name in files if startup_file(name)),
        previous_hash_passes=2,
        current_hash_passes=1,
        median_reduction_percent=round((1 - current / previous) * 100, 2),
        results=results,
        excluded=[
            "Python process startup", "first display presentation", "network initialization", "bootloader",
            "cold device filesystem cache", "power-on-to-menu timing"
        ]
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", required=True, type=Path, help="Complete existing packaged release directory")
    parser.add_argument("--iterations", type=int, default=30)
    parser.add_argument("--warmup", type=int, default=3)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.iterations < 1 or args.warmup < 0: parser.error("Iterations must be positive and warmup nonnegative.")
    try:
        result = benchmark(args.app, args.iterations, args.warmup)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    result["reproduce"] = [
        "python3", "tools/benchmark_boot.py", "--app", result["app"], "--iterations",
        str(args.iterations), "--warmup",
        str(args.warmup)
    ]
    serialized = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(serialized)
    print(serialized, end="")


if __name__ == "__main__": main()
