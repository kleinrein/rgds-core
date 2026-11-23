#!/usr/bin/env python3
"""Run the Python test suite (tests/test_*.py) in parallel worker processes.

Each test class runs in its own `python -m unittest` process with its own TMPDIR, and the classes are spread over
-j workers, slowest first (timings from the last run are kept in build/test-times.json). Every process gets
SDL_AUDIODRIVER=dummy: under load the first menu sound otherwise blocks CoreAudio long enough for scripted keys to
be dropped, which shows up as false UI failures.

  python3 tools/test.py                 # everything, one worker per core (at most 8)
  python3 tools/test.py -j 4 music ota  # modules whose name contains "music" or "ota"
  python3 tools/test.py --ci            # what .github/workflows/ci.yml runs (the exclusion lists below)
  python3 tools/test.py -j 1            # one class at a time, still one process per class

Full output of every failing class goes to build/test-logs/; the summary at the end lists each failing test.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TESTS = ROOT / "tests"
TIMES = ROOT / "build/test-times.json"
LOGS = ROOT / "build/test-logs"

# The CI exclusion lists (.github/workflows/ci.yml calls this script with --ci, so they live here only).
# Need build/host/rgds-launcher or the fetched engines
NEEDS_BUILD = [
    "bluetooth_ui", "game_info_ui", "gap_closure_ui", "ota_ui", "release_polish_ui", "shader_pixels",
    "shader_preview_validation"
]
# Over a minute: assembles the whole compat layer
SLOW = ["portmaster"]
# Already failing when CI was added (core_refinement on develop only); take a module out once it is fixed
KNOWN_RED = ["companion_lint", "core_refinement", "game_switcher", "shader_preview"]

# A class that hangs is killed after this long and reported as an error.
UNIT_TIMEOUT = 900

SUMMARY_LINE = re.compile(r"^(?:FAIL|ERROR): (\S+) \(([^)]+)\)", re.M)
RAN_LINE = re.compile(r"^Ran (\d+) tests? in", re.M)
SKIPPED = re.compile(r"skipped=(\d+)")


@dataclass
class Unit:
    """One worker process: a test class (or a whole module when it cannot be loaded here)."""
    name: str
    module: str
    estimate: float = 0.0


@dataclass
class Outcome:
    unit: Unit
    seconds: float
    returncode: int
    ran: int = 0
    skipped: int = 0
    failed: list[str] = field(default_factory=list)
    timed_out: bool = False
    log: Path | None = None

    @property
    def ok(self) -> bool:
        return self.returncode == 0 and not self.timed_out


def modules(filters: list[str], exclude: list[str]) -> list[str]:
    names = sorted(path.stem for path in TESTS.glob("test_*.py"))
    if filters:
        names = [name for name in names if any(f.removeprefix("test_") in name for f in filters)]
    if exclude:
        drop = re.compile(r"^test_(%s)$" % "|".join(map(re.escape, exclude)))
        names = [name for name in names if not drop.match(name)]
    return names


def classes(module: str) -> list[str]:
    """The test classes in a module, in file order; [] when the module does not import (run it whole instead)."""
    try:
        suite = unittest.defaultTestLoader.loadTestsFromName(module)
    except Exception:
        return []
    found: list[str] = []

    def walk(item):
        if isinstance(item, unittest.TestSuite):
            for child in item:
                walk(child)
        elif isinstance(item, unittest.TestCase):
            cls = type(item)
            # loader failures (_FailedTest) belong to the module run, not a class
            if cls.__module__ != module:
                found.clear()
                raise StopIteration
            name = f"{module}.{cls.__qualname__}"
            if name not in found:
                found.append(name)

    try:
        walk(suite)
    except StopIteration:
        return []
    return found


def plan(names: list[str], times: dict[str, float]) -> list[Unit]:
    sys.path[:0] = [str(TESTS), str(ROOT / "runtime")]
    units: list[Unit] = []
    for module in names:
        found = classes(module)
        for name in found or [module]:
            units.append(Unit(name, module, times.get(name, 5.0)))
    # longest first so the slow classes do not start last and stretch the wall time
    units.sort(key=lambda unit: -unit.estimate)
    return units


def environment(tmp: Path) -> dict[str, str]:
    env = dict(os.environ)
    env["PYTHONPATH"] = os.pathsep.join(filter(None, [str(TESTS), str(ROOT / "runtime"), env.get("PYTHONPATH")]))
    env["SDL_AUDIODRIVER"] = "dummy"
    env["TMPDIR"] = str(tmp)
    env["TEMP"] = env["TMP"] = str(tmp)
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    env["PYTHONUNBUFFERED"] = "1"
    return env


def run(unit: Unit, scratch: Path, verbose: bool) -> Outcome:
    tmp = Path(tempfile.mkdtemp(prefix=unit.name.replace(".", "-") + "-", dir=scratch))
    command = [sys.executable, "-m", "unittest"] + (["-v"] if verbose else []) + [unit.name]
    start = time.monotonic()
    timed_out = False
    try:
        proc = subprocess.run(
            command,
            cwd=ROOT,
            env=environment(tmp),
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors="replace",
            timeout=UNIT_TIMEOUT
        )
        output, returncode = proc.stdout, proc.returncode
    except subprocess.TimeoutExpired as error:
        output = (error.stdout or b"").decode("utf-8", "replace") if isinstance(error.stdout,
                                                                                bytes) else (error.stdout or "")
        output += f"\n*** killed after {UNIT_TIMEOUT} s\n"
        returncode, timed_out = -9, True
    seconds = time.monotonic() - start
    shutil.rmtree(tmp, ignore_errors=True)
    outcome = Outcome(unit, seconds, returncode, timed_out=timed_out)
    ran = RAN_LINE.findall(output)
    outcome.ran = int(ran[-1]) if ran else 0
    tail = output[output.rfind("\nRan "):] if "\nRan " in output else ""
    skipped = SKIPPED.search(tail)
    outcome.skipped = int(skipped.group(1)) if skipped else 0
    outcome.failed = [
        f"{where}" if where.endswith(test) else f"{where}.{test}" for test, where in SUMMARY_LINE.findall(output)
    ]
    if not outcome.ok:
        if not outcome.failed:
            outcome.failed = [unit.name + (" (timed out)" if timed_out else " (exit %d)" % returncode)]
        LOGS.mkdir(parents=True, exist_ok=True)
        outcome.log = LOGS / f"{unit.name}.log"
        outcome.log.write_text(" ".join(command) + "\n\n" + output)
    return outcome


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("filters", nargs="*", help="run only modules whose name contains one of these")
    parser.add_argument("-j", "--jobs", type=int, default=min(8, os.cpu_count() or 1), help="worker processes")
    parser.add_argument("--ci", action="store_true", help="leave out the needs-build, slow and known-red modules")
    parser.add_argument("--no-build", action="store_true", help="leave out the modules that need build/host")
    parser.add_argument("--no-slow", action="store_true", help="leave out the slow modules")
    parser.add_argument("--no-known-red", action="store_true", help="leave out the known-red modules")
    parser.add_argument("--exclude", action="append", default=[], help="module name (without test_) to leave out")
    parser.add_argument("-v", "--verbose", action="store_true", help="unittest -v in each worker (shows in logs)")
    parser.add_argument("--list", action="store_true", help="print the planned units and exit")
    args = parser.parse_args()

    exclude = list(args.exclude)
    if args.ci or args.no_build:
        exclude += NEEDS_BUILD
    if args.ci or args.no_slow:
        exclude += SLOW
    if args.ci or args.no_known_red:
        exclude += KNOWN_RED

    try:
        times = json.loads(TIMES.read_text())
    except (OSError, ValueError):
        times = {}
    units = plan(modules(args.filters, exclude), times)
    if args.list:
        for unit in units:
            print(f"{unit.estimate:7.1f}  {unit.name}")
        return 0
    if not units:
        print("no test modules matched", file=sys.stderr)
        return 2

    jobs = max(1, args.jobs)
    print(f"{len(units)} test classes from {len({u.module for u in units})} modules on {jobs} workers", flush=True)
    if LOGS.exists():
        shutil.rmtree(LOGS, ignore_errors=True)
    scratch = Path(tempfile.mkdtemp(prefix="rgds-tests-"))
    start = time.monotonic()
    outcomes: list[Outcome] = []
    lock = threading.Lock()
    try:
        with ThreadPoolExecutor(max_workers=jobs) as pool:
            futures = [pool.submit(run, unit, scratch, args.verbose) for unit in units]
            for future in as_completed(futures):
                outcome = future.result()
                with lock:
                    outcomes.append(outcome)
                    mark = "ok  " if outcome.ok else "FAIL"
                    print(
                        f"[{len(outcomes):4}/{len(units)}] {mark} {outcome.seconds:6.1f}s  {outcome.unit.name}",
                        flush=True
                    )
    except KeyboardInterrupt:
        print("\ninterrupted", file=sys.stderr)
        return 130
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    wall = time.monotonic() - start

    # keep the timings for the next run's ordering (merged, so a filtered run does not forget the rest)
    times.update({o.unit.name: round(o.seconds, 2) for o in outcomes})
    TIMES.parent.mkdir(parents=True, exist_ok=True)
    TIMES.write_text(json.dumps(dict(sorted(times.items())), indent=1) + "\n")

    ran = sum(o.ran for o in outcomes)
    skipped = sum(o.skipped for o in outcomes)
    bad = [o for o in outcomes if not o.ok]
    cpu = sum(o.seconds for o in outcomes)
    print()
    print("slowest classes:")
    for o in sorted(outcomes, key=lambda o: -o.seconds)[:8]:
        print(f"  {o.seconds:6.1f}s  {o.unit.name}")
    if bad:
        print()
        print(f"failing ({sum(len(o.failed) for o in bad)} tests in {len(bad)} classes):")
        for o in sorted(bad, key=lambda o: o.unit.name):
            for test in o.failed:
                print(f"  {test}")
            print(f"    log: {o.log.relative_to(ROOT)}")
    print()
    status = "FAILED" if bad else "OK"
    print(f"{status}: {ran} tests ({skipped} skipped) in {wall:.0f} s wall, {cpu:.0f} s summed, {jobs} workers")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
