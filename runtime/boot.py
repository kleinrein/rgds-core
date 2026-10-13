#!/usr/bin/env python3
"""Foreground RGDS boot entry; stock initialization owns what happens next."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time

from boot_control import CONTROL, read_json


def same_runtime(app, receipt):
    """A changed active release gets a fresh interpreter instead of mixed imports."""
    directory = (Path(app) / 'runtime').resolve()
    if Path(__file__).resolve().parent != directory:return False
    for name in receipt.get('files', {}):
        if not name.startswith('runtime/') or not name.endswith('.py'):continue
        module_name = name[len('runtime/'):-len('.py')]
        if '/' in module_name:continue
        module = sys.modules.get(module_name)
        if module is None:continue
        source = getattr(module, '__file__', None)
        if source is None or Path(source).resolve().parent != directory:return False
    return True


def run_verified_session(app, argv, receipt, environment):
    """Load the verified entry from its exact path, reusing the boot interpreter."""
    import importlib.util
    import signal
    previous_environment = os.environ.copy()
    previous_path = list(sys.path)
    previous_bytecode = sys.dont_write_bytecode
    previous_handlers = {signum:signal.getsignal(signum) for signum in (signal.SIGINT, signal.SIGTERM)}
    alias = '_rgds_verified_boot_session'
    previous_module = sys.modules.get(alias)
    try:
        os.environ.clear();os.environ.update(environment)
        sys.dont_write_bytecode = True
        sys.path.insert(0, str(Path(app) / 'runtime'))
        began = time.monotonic()
        spec = importlib.util.spec_from_file_location(alias, Path(app) / 'runtime/session.py')
        if spec is None or spec.loader is None:raise ValueError('Verified session entry cannot be loaded.')
        module = importlib.util.module_from_spec(spec)
        sys.modules[alias] = module
        spec.loader.exec_module(module)
        receipt['session_import_seconds'] = round(time.monotonic()-began, 3)
        return module.main(argv=argv, startup_receipt=receipt)
    finally:
        if previous_module is None:sys.modules.pop(alias, None)
        else:sys.modules[alias] = previous_module
        try:
            for signum, handler in previous_handlers.items():signal.signal(signum, handler)
        finally:
            sys.path[:] = previous_path
            sys.dont_write_bytecode = previous_bytecode
            os.environ.clear();os.environ.update(previous_environment)


def run(control=CONTROL, runtime=Path('/run'), launch=None):
    if os.environ.get('RGDS_SKIP_BOOT_ONCE') == '1': return 0
    control = Path(control);runtime = Path(runtime)
    config = read_json(control / 'config.json')
    if not (control / 'enabled').is_file() or (control / 'installing').exists() or config.get('schema') != 1 or config.get('mode') != 'rgds':
        return 0
    raw_root = config.get('root', '')
    if not isinstance(raw_root, str) or not isinstance(config.get('warning', True), bool): return 0
    root = Path(raw_root)
    if not raw_root or not root.is_absolute() or '..' in root.parts or (root / 'userdata/boot_stock').exists():
        return 0
    began = time.monotonic()
    dispatch_seconds = None
    dispatch_mode = None
    receipt = None
    try:
        runtime.mkdir(parents=True, exist_ok=True)
        attempted = runtime / 'rgds-boot-attempted'
        try:
            descriptor = os.open(attempted, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            os.close(descriptor)
        except FileExistsError:
            return 0  # One attempt per kernel boot; a crash/return never loops.
        from activate import read_pointer, verify_startup
        build = read_pointer(root / 'state/active')
        if not build: raise ValueError('No active RGDS release')
        app = root / 'releases' / build
        verify_began = time.monotonic()
        receipt = verify_startup(app, receipt=True)
        receipt['began'] = began
        receipt['verify_seconds'] = round(time.monotonic()-verify_began, 3)
        arguments = ['--root', str(root), '--boot', '--boot-network-config', str(control / 'config.json')]
        if config.get('warning', True):arguments.append('--boot-warning')
        environment = os.environ.copy()
        for key in tuple(environment):
            if key.startswith(('SIMPLEOS_', 'RGDS_')) or key in ('LD_PRELOAD', 'LD_LIBRARY_PATH', 'SSH_CONNECTION', 'SSH_CLIENT'):
                environment.pop(key, None)
        environment['PYTHONDONTWRITEBYTECODE'] = '1'
        environment['PATH'] = '/usr/sbin:/usr/bin:/sbin:/bin'
        if launch is None and same_runtime(app, receipt):
            dispatch_mode = receipt['dispatch_mode'] = 'in_process'
            dispatch_seconds = round(time.monotonic()-began, 3)
            status = run_verified_session(app, arguments, receipt, environment)
        else:
            # Preserve isolation when the active pointer changed after this boot
            # entry was loaded, or an explicit caller supplies a subprocess runner.
            import tempfile
            dispatch_mode = receipt['dispatch_mode'] = 'subprocess'
            command = [sys.executable, '-B', str(app / 'runtime/session.py'), *arguments]
            with tempfile.TemporaryFile(mode='w+b') as verified:
                verified.write(json.dumps(receipt).encode())
                verified.seek(0)
                command += ['--startup-fd', str(verified.fileno())]
                dispatch_seconds = round(time.monotonic()-began, 3)
                result = (launch or subprocess.run)(command, env=environment, check=False, pass_fds=(verified.fileno(),))
            status = result.returncode
    except Exception as error:
        print('RGDS boot fallback: ' + str(error), file=sys.stderr)
        status = 1
    # Only our internal status is recorded. Always return to the stock caller.
    record = dict(schema=1, exit=status, dispatch_seconds=dispatch_seconds, dispatch_mode=dispatch_mode,
                  session_seconds=round(time.monotonic()-began, 3))
    if receipt is not None and 'session_import_seconds' in receipt:record['session_import_seconds'] = receipt['session_import_seconds']
    try:
        (runtime / 'rgds-boot-result.json').write_text(json.dumps(record) + '\n')
    except OSError: pass
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--control', type=Path, default=CONTROL)
    parser.add_argument('--run-dir', type=Path, default=Path('/run'))
    args = parser.parse_args()
    return run(args.control, args.run_dir)


if __name__ == '__main__': sys.exit(main())
