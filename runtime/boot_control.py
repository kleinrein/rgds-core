"""Owned, reversible startup routing through Anbernic's existing autostart hook."""
import hashlib
import json
import os
import platform
from pathlib import Path
import shlex
import stat
import sys
import tempfile

CONTROL = Path('/userdata/rgds-boot')
HOOK = Path('/mnt/mod/ctrl/autostart')
SUPERVISOR = Path('/mnt/vendor/ctrl/loadapp.sh')
TAG = '# RGDS_AUTOSTART_V1'


def location(system_root, absolute):
    return Path(system_root) / str(absolute).lstrip('/')


def atomic(path, content, mode=0o600):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, pending = tempfile.mkstemp(prefix='.rgds-', dir=path.parent)
    try:
        with os.fdopen(fd, 'wb') as stream:
            os.fchmod(stream.fileno(), mode)
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(pending, path)
        descriptor = os.open(path.parent, os.O_RDONLY)
        try:
            os.fsync(descriptor)
        finally:
            os.close(descriptor)
    finally:
        Path(pending).unlink(missing_ok=True)


def read_json(path):
    try:
        value = json.loads(Path(path).read_text())
        return value if isinstance(value, dict) else {}
    except (OSError, ValueError):
        return {}


def hook_text(root, control=CONTROL, run=Path('/run'), chain=False):
    # Stock mode exits in the shell before starting Python or reading releases.
    body = (
        '#!/bin/sh\n' + TAG + '\n' + '[ "${RGDS_SKIP_BOOT_ONCE:-0}" != 1 ] || exit 0\n' + 'RGDS_BOOT_CONTROL=' +
        shlex.quote(str(control)) + '\n' + '[ -f "$RGDS_BOOT_CONTROL/enabled" ] || exit 0\n' +
        '[ ! -f "$RGDS_BOOT_CONTROL/installing" ] || exit 0\n' + 'RGDS_BOOT_ROOT=' + shlex.quote(str(root)) + '\n' +
        '[ ! -f "$RGDS_BOOT_ROOT/userdata/boot_stock" ] || exit 0\n' +
        'RGDS_BOOT_ID=$(cat "$RGDS_BOOT_ROOT/state/active" 2>/dev/null) || exit 0\n' +
        'case "$RGDS_BOOT_ID" in ""|*[!a-z0-9.-]*|.*) exit 0;; esac\n' +
        'RGDS_BOOT_ENTRY="$RGDS_BOOT_ROOT/releases/$RGDS_BOOT_ID/runtime/boot.py"\n' +
        '[ -f "$RGDS_BOOT_ENTRY" ] || [ -f "$RGDS_BOOT_ROOT/update_guard.py" ] || exit 0\n' + 'RGDS_BOOT_ATTEMPT=' +
        shlex.quote(str(Path(run) / 'rgds-boot-hook-attempted')) + '\n' +
        '(umask 077; mkdir "$RGDS_BOOT_ATTEMPT") 2>/dev/null || exit 0\n' +
        'if [ -f "$RGDS_BOOT_ROOT/update_guard.py" ]; then\n' +
        '  /usr/bin/python3 -B "$RGDS_BOOT_ROOT/update_guard.py" --root "$RGDS_BOOT_ROOT" --boot-entry --control "$RGDS_BOOT_CONTROL" || exit 0\n'
        + 'else\n' + '  /usr/bin/python3 -B "$RGDS_BOOT_ENTRY" --control "$RGDS_BOOT_CONTROL" || exit 0\n' + 'fi\n' +
        'exit 0\n'
    )
    if not chain: return body
    # A hook that was there before us keeps running once our session has returned (or declined).
    first, rest = body.split('\n', 2)[:2], body.split('\n', 2)[2]
    original = shlex.quote(str(Path(control) / 'original-autostart'))
    return '\n'.join(first) + '\n(\n' + rest + ')\n[ ! -s ' + original + ' ] || /bin/sh ' + original + '\nexit 0\n'


def ours(content):
    return TAG.encode() in content


def inspect(root=None, system_root='/'):
    control = location(system_root, CONTROL)
    hook = location(system_root, HOOK)
    saved = read_json(control / 'installation.json')
    config = read_json(control / 'config.json')
    try:
        content = hook.read_bytes()
    except OSError:
        content = b''
    owned = bool(
        not hook.is_symlink() and saved.get('schema') == 1 and content
        and hashlib.sha256(content).hexdigest() == saved.get('hook_sha256')
    )
    matches = root is None or config.get('root') == str(Path(root).resolve())
    valid = config.get('schema') == 1 and isinstance(config.get('warning', True), bool)
    enabled = owned and matches and valid and not (control / 'installing').exists() and (control / 'enabled').is_file(
    ) and config.get('mode') == 'rgds'
    return dict(
        installed=int(owned and matches),
        mode='rgds' if enabled else 'stock',
        warning=int(config.get('warning', True) is not False),
        root=config.get('root', '')
    )


def plan(root, mode, system_root='/', warning=True):
    if mode not in ('stock', 'rgds') or not isinstance(warning, bool):
        raise ValueError('Choose stock or rgds startup and a boolean warning setting.')
    root = Path(root).resolve()
    supervisor = location(system_root, SUPERVISOR)
    try:
        supervisor_source = supervisor.read_text()
    except OSError:
        raise ValueError('The stock loadapp supervisor is missing; startup integration has not been changed.')
    if str(HOOK) not in supervisor_source:
        raise ValueError('The stock supervisor does not expose the expected autostart hook.')
    control = location(system_root, CONTROL)
    hook = location(system_root, HOOK)
    saved = read_json(control / 'installation.json')
    if hook.is_symlink(): raise ValueError('Refusing to replace a symlinked autostart hook.')
    try:
        current = hook.read_bytes()
    except FileNotFoundError:
        current = None
    # Full verification happens when selecting/installing a boot target.
    from activate import read_pointer, verify_release
    build = read_pointer(root / 'state/active')
    if not build: raise ValueError('This RGDS installation has no active release.')
    app = root / 'releases' / build
    verify_release(app)
    if not (app / 'runtime/boot.py').is_file(): raise ValueError('This release has no RGDS boot entry.')
    from session import ensure_writable_data
    ensure_writable_data(root / 'userdata')
    from boot_network import capture_helper
    return dict(
        schema=1,
        root=str(root),
        mode=mode,
        warning=warning,
        hook=str(HOOK),
        adopts=bool(current and not ours(current) and hashlib.sha256(current).hexdigest() != saved.get('hook_sha256')),
        network_guard=capture_helper(system_root),
        preserves=['kernel splash', 'vendor init', 'Wi-Fi initialization', 'Weston', 'stock supervisor'],
        writes=['config.json', 'owned autostart hook', 'enabled marker']
    )


def configure(root, mode, system_root='/', warning=True):
    if mode not in ('stock', 'rgds') or not isinstance(warning, bool):
        raise ValueError('Choose stock or rgds startup and a boolean warning setting.')
    if str(
        Path(system_root).resolve()
    ) == '/' and (sys.platform != 'linux' or os.geteuid() != 0 or platform.machine() not in ('aarch64', 'arm64')):
        raise ValueError('Startup installation runs as root on the RG DS; use an explicit fixture root for tests.')
    if mode == 'stock':
        # Recovery must remain available with damaged/missing engine payloads or
        # a read-only game card. Never verify/install a target to turn it off.
        disable(system_root)
        control = location(system_root, CONTROL)
        control.mkdir(parents=True, exist_ok=True, mode=0o700)
        config = dict(schema=1, root=str(Path(root).resolve()), mode='stock', warning=warning)
        atomic(control / 'config.json', (json.dumps(config, sort_keys=True) + '\n').encode())
        return inspect(root, system_root)
    proposal = plan(root, mode, system_root, warning)
    control = location(system_root, CONTROL)
    hook = location(system_root, HOOK)
    control.mkdir(parents=True, exist_ok=True, mode=0o700)
    atomic(control / 'installing', b'1\n')
    # Disabling first makes an interrupted update fall through to stock.
    (control / 'enabled').unlink(missing_ok=True)
    saved = read_json(control / 'installation.json')
    existing = hook.read_bytes() if hook.is_file() else b''
    stale = ours(existing)  # our own hook from an install whose control folder is gone
    foreign = bool(existing) and not ours(existing) and hashlib.sha256(existing).hexdigest() != saved.get('hook_sha256')
    if not saved or foreign:
        # Someone else's hook (or a first install) is kept as the original: it runs after ours, and uninstall puts it back.
        existing = b'' if stale else existing
        previous_mode = stat.S_IMODE(hook.stat().st_mode) if existing else 0o755
        atomic(control / 'original-autostart', existing, previous_mode)
        saved = dict(schema=1, had_hook=bool(existing), previous_mode=previous_mode)
    chain = bool(saved.get('had_hook')) and (control / 'original-autostart').is_file()
    text = hook_text(Path(proposal['root']), CONTROL, chain=chain).encode()
    atomic(hook, text, 0o755)
    saved['hook_sha256'] = hashlib.sha256(text).hexdigest()
    atomic(control / 'installation.json', (json.dumps(saved, sort_keys=True) + '\n').encode())
    config = {key: proposal[key] for key in ('schema', 'root', 'mode', 'warning', 'network_guard')}
    atomic(control / 'config.json', (json.dumps(config, sort_keys=True) + '\n').encode())
    (control / 'installing').unlink()
    if mode == 'rgds': atomic(control / 'enabled', b'1\n')
    return inspect(root, system_root)


def disable(system_root='/'):
    # This rescue operation removes our routing marker and returns any stock
    # service RGDS deferred (system_tuning) to its journalled original mode.
    control = location(system_root, CONTROL)
    (control / 'enabled').unlink(missing_ok=True)
    try:
        from system_tuning import remove, remove_poweroff
        remove(system_root)
        remove_poweroff(system_root)
    except (ImportError, OSError, ValueError):
        pass
    return dict(mode='stock')


def uninstall(system_root='/'):
    control = location(system_root, CONTROL)
    hook = location(system_root, HOOK)
    saved = read_json(control / 'installation.json')
    if not saved: return dict(mode='stock')
    if hook.is_symlink(): raise ValueError('The installed hook was changed externally; it has been left untouched.')
    try:
        current = hook.read_bytes()
    except FileNotFoundError:
        current = b''
    if hashlib.sha256(current).hexdigest() != saved.get('hook_sha256'):
        raise ValueError('The installed hook was changed externally; it has been left untouched.')
    disable(system_root)
    if saved.get('had_hook'):
        atomic(hook, (control / 'original-autostart').read_bytes(), saved['previous_mode'])
    else:
        hook.unlink(missing_ok=True)
    (control / 'installation.json').unlink(missing_ok=True)
    return dict(mode='stock')
