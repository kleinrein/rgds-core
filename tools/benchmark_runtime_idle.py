#!/usr/bin/env python3
"""Host-only measurements of log wakeups and emulator preparation memory.

Uses generated code-file fixtures and a quiet pipe; never contacts a device.
These are runtime overhead measurements, not game FPS or battery estimates.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import select
import statistics
import sys
import tempfile
import threading
import time
import tracemalloc
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT/'runtime'))


def module(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec); spec.loader.exec_module(result)
    return result


def measure(runtime, nnddss_app=None):
    adapters = module(runtime/'emulators.py', 'bench_adapters')
    logger = module(runtime/'engine_log.py', 'bench_log')
    with tempfile.TemporaryDirectory() as folder:
        root = Path(folder); original = root/'original.so'; installed = root/'installed.so'
        # Deliberately larger than the comparison buffer; allocate before tracing.
        with original.open('wb') as stream:
            for _ in range(64): stream.write(b'RGDS' * 65536)
        adapters.copy_component(original, installed)
        tracemalloc.start(); began = time.perf_counter()
        adapters.copy_component(original, installed)
        elapsed = time.perf_counter() - began
        _, peak = tracemalloc.get_traced_memory(); tracemalloc.stop()
        reader, writer = os.pipe(); stream = os.fdopen(reader, 'rb', buffering=0)
        capture = logger.EngineLog(root, 'dsperate', ['fixture'], root)
        original_select = select.select; entered = threading.Event(); counts = dict(waits=0, timer_wakeups=0)
        def observed(*args):
            counts['waits'] += 1; entered.set()
            result = original_select(*args)
            if not result[0]: counts['timer_wakeups'] += 1
            return result
        try:
            with patch.object(logger.select, 'select', side_effect=observed):
                capture.start(stream)
                if not entered.wait(2): raise RuntimeError('Log worker did not start')
                time.sleep(.5)
                quiet = dict(counts)
                began = time.perf_counter(); capture.finish(0)
                shutdown = time.perf_counter() - began
        finally:
            os.close(writer)
            if capture.worker and capture.worker.is_alive(): capture.finish(-1)
        result = dict(scope='host overhead only; no handheld or gameplay measurements',
                    component_bytes=original.stat().st_size,comparison_peak_python_bytes=peak,
                    comparison_seconds=elapsed,quiet_seconds=.5,quiet_log=quiet,
                    log_shutdown_seconds=shutdown)
        if nnddss_app:
            app = nnddss_app.resolve(); binary = app/'emulators/nnddss/nnddss'
            data = root/'nnddss-data'
            adapters.prepare_nnddss(app, data, binary)
            original_open = Path.open
            counts = dict(library_read_bytes=0, writes=0)
            libraries = {p.name for p in (binary.parent/'lib').iterdir() if p.is_file()}
            class CountedRead:
                def __init__(self, stream): self.stream = stream
                def __enter__(self): self.stream.__enter__(); return self
                def __exit__(self, *args): return self.stream.__exit__(*args)
                def __getattr__(self, name): return getattr(self.stream, name)
                def read(self, *args):
                    content = self.stream.read(*args)
                    counts['library_read_bytes'] += len(content)
                    return content
            def observed_open(path, mode='r', *args, **kwargs):
                stream = original_open(path, mode, *args, **kwargs)
                if any(flag in mode for flag in ('w', 'a', '+')): counts['writes'] += 1
                return CountedRead(stream) if mode=='rb' and path.name in libraries else stream
            samples=[]
            with patch.object(Path, 'open', observed_open):
                for _ in range(12):
                    began=time.perf_counter(); adapters.prepare_nnddss(app, data, binary)
                    samples.append((time.perf_counter()-began)*1000)
            result['nnddss_preparation'] = dict(app=str(app), iterations=len(samples),
                warm_ms=samples, median_ms=statistics.median(samples), **counts,
                excludes='release integrity verification and emulator process startup; unchanged by this optimization')
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', type=Path, default=ROOT/'runtime')
    parser.add_argument('--nnddss-app', type=Path, help='Prepared release to benchmark; never executes its emulator')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    sys.path.insert(0, str(args.runtime.resolve()))
    report = measure(args.runtime.resolve(), args.nnddss_app)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2)+'\n'); print(json.dumps(report, indent=2))


if __name__ == '__main__': main()
