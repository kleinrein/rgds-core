#!/usr/bin/env python3
"""Stable application boot supervisor. OTA never replaces this file or the trust root."""
import argparse
from contextlib import contextmanager
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import signal
import subprocess
import sys
import tempfile
import time

PROTOCOL = 1
BUILD = re.compile(r'[a-z0-9][a-z0-9.-]{0,100}\Z')
BOOT_TIMEOUT = 90
HEALTH_GRACE = 15

def atomic(path, value):
    path = Path(path); path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix='.update-', dir=path.parent)
    try:
        with os.fdopen(fd, 'w') as out:
            out.write(value); out.flush(); os.fsync(out.fileno())
        os.replace(tmp, path)
        try:
            fd = os.open(str(path.parent), os.O_RDONLY)
            try: os.fsync(fd)
            finally: os.close(fd)
        except OSError: pass
    finally:
        Path(tmp).unlink(missing_ok=True)

def save(path, value): atomic(path, json.dumps(value, sort_keys=True)+'\n')
def read(path):
    try:
        value=json.loads(Path(path).read_text())
        if not isinstance(value,dict): raise ValueError('Invalid update record')
        return value
    except FileNotFoundError: return {}

def pointer(root, name):
    try: value=(root/'state'/name).read_text().strip()
    except FileNotFoundError: return None
    if not BUILD.fullmatch(value): raise ValueError('Invalid release pointer')
    return value

@contextmanager
def update_lock(root):
    path=root/'state/update.lock'; path.parent.mkdir(parents=True,exist_ok=True)
    with path.open('a') as lock:
        try: fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
        except BlockingIOError: raise ValueError('Another update is already running.') from None
        try: yield
        finally: fcntl.flock(lock,fcntl.LOCK_UN)

def verify(root, build):
    """Verify a candidate without importing any code from it."""
    if not isinstance(build,str) or not BUILD.fullmatch(build): raise ValueError('Invalid build')
    app=root/'releases'/build
    if app.is_symlink() or (app/'manifest.json').is_symlink(): raise ValueError('Unsafe release')
    manifest=read(app/'manifest.json'); files=manifest.get('files')
    if manifest.get('schema')!=1 or manifest.get('build')!=build or manifest.get('project')!='rgds' or manifest.get('target')!='aarch64-linux-gnu' or not isinstance(files,dict) or 'bin/rgds-launcher' not in files: raise ValueError('Invalid release manifest')
    actual=set()
    for path in app.rglob('*'):
        if path.is_symlink(): raise ValueError('Unsafe release file')
        if path.is_file(): actual.add(path.relative_to(app).as_posix())
        elif not path.is_dir(): raise ValueError('Unsafe release file')
    if actual != set(files)|{'manifest.json'}: raise ValueError('Release inventory mismatch')
    for name,digest in files.items():
        p=Path(name)
        if p.is_absolute() or '..' in p.parts or p.as_posix()!=name or '\\' in name or not isinstance(digest,str) or not re.fullmatch('[0-9a-f]{64}',digest): raise ValueError('Unsafe release manifest')
        h=hashlib.sha256()
        with (app/p).open('rb') as stream:
            for block in iter(lambda:stream.read(1024*1024),b''): h.update(block)
        if h.hexdigest()!=digest: raise ValueError('Release checksum failed')
    return app

def status(root, state, message, **values):
    fields=dict(state=state,message=message,**values)
    atomic(root/'userdata/update/status.ini',''.join(k+'='+str(v).replace('\n',' ').replace('\r',' ')+'\n' for k,v in fields.items()))

def rollback_trial(root, pending, reason):
    previous=pending['previous']; verify(root,previous)
    # Pointer is repaired before removing the trial journal. Repeating is safe.
    atomic(root/'state/active',previous+'\n')
    save(root/'state/update-result.json',dict(state='rolled-back',build=pending['build'],previous=previous,reason=reason))
    (root/'state/update-pending.json').unlink(missing_ok=True)
    (root/'state/update-apply.json').unlink(missing_ok=True)
    status(root,'rolled-back','The update could not start. Your previous version was restored.',current=previous)

def begin_trial(root):
    with update_lock(root):
        pending=read(root/'state/update-pending.json'); request=read(root/'state/update-apply.json')
        if not pending or pending.get('phase')!='staged' or request.get('build')!=pending.get('build'): return False
        current=pointer(root,'active')
        if current!=pending['previous']: raise ValueError('The installed version changed. Check for updates again.')
        verify(root,current)
        try:verify(root,pending['build'])
        except (OSError,ValueError):
            rollback_trial(root,pending,'Candidate verification failed before activation')
            return False
        pending.update(phase='trial',attempted=False)
        save(root/'state/update-pending.json',pending)
        atomic(root/'state/previous',current+'\n')
        atomic(root/'state/active',pending['build']+'\n')
        (root/'state/update-apply.json').unlink(missing_ok=True)
        return True

def recover_interrupted(root):
    with update_lock(root):
        pending=read(root/'state/update-pending.json')
        if pending.get('phase')=='trial':
            if pending.get('attempted') or pointer(root,'active')!=pending.get('build'):
                rollback_trial(root,pending,'Startup was interrupted')
                return True
        return False

def stop_child(child):
    if child.poll() is None:
        child.terminate()
        try: child.wait(timeout=15)
        except subprocess.TimeoutExpired:
            child.kill(); child.wait(timeout=5)
    # Session recovery has its own guardian. Never overlap it with a new session.
    deadline=time.monotonic()+15
    while Path('/tmp/rgds-session.lock').exists() and time.monotonic()<deadline: time.sleep(.1)
    if Path('/tmp/rgds-session.lock').exists(): raise ValueError('Session recovery is still running. Return to stock and retry.')

def supervise(root, session_args=(), launch=subprocess.Popen, timeout=BOOT_TIMEOUT, grace=HEALTH_GRACE, interval=.25, heartbeat_timeout=5):
    recover_interrupted(root)
    begin_trial(root)
    while True:
        build=pointer(root,'active'); pending=read(root/'state/update-pending.json')
        trial=pending.get('phase')=='trial' and pending.get('build')==build
        token=secrets.token_hex(24) if trial else ''
        if trial:
            try: verify(root,build)
            except (OSError,ValueError):
                with update_lock(root): rollback_trial(root,pending,'Candidate verification failed')
                continue
            with update_lock(root):
                pending.update(attempted=True,token=token)
                save(root/'state/update-pending.json',pending)
            status(root,'testing','Checking the new version…',version=pending.get('version',''),progress=100)
        env=os.environ.copy(); env['PYTHONDONTWRITEBYTECODE']='1'
        env.pop('RGDS_UPDATE_TOKEN',None)
        if token: env['RGDS_UPDATE_TOKEN']=token
        args=[a for a in session_args if not (trial and a=='--boot-warning')]
        command=[sys.executable,'-B',str(root/'releases'/build/'runtime/session.py'),'--root',str(root),*args]
        # Recovery releases the stock frontend. A subsequent session must use
        # normal APPS handoff, even when this supervisor began at cold boot.
        session_args=list(session_args)
        if '--boot-network-config' in session_args:
            index=session_args.index('--boot-network-config');del session_args[index:index+2]
        session_args=tuple(a for a in session_args if a!='--boot')
        try:child=launch(command,env=env)
        except OSError:
            if not trial:raise
            with update_lock(root):rollback_trial(root,pending,'Could not start candidate')
            continue
        began=time.monotonic(); ready_at=None;last_pulse=None;last_seen=None
        try:
            while child.poll() is None:
                if trial:
                    try:health=read(root/'userdata/session/update-health.json')
                    except (ValueError,OSError):health={}
                    if health.get('token')==token and health.get('build')==build:
                        if ready_at is None:ready_at=time.monotonic()
                        if last_seen is None or health.get('pulse')!=last_pulse:last_seen=time.monotonic();last_pulse=health.get('pulse')
                    if last_seen is not None and time.monotonic()-last_seen>heartbeat_timeout:
                        stop_child(child);break
                    if ready_at is not None and time.monotonic()-ready_at>=grace:
                        with update_lock(root):
                            save(root/'state/update-result.json',dict(state='installed',build=build,version=pending.get('version',''),sequence=pending['sequence']))
                            (root/'state/update-pending.json').unlink(missing_ok=True)
                            status(root,'installed','Update installed.',version=pending.get('version',''),current=build,progress=100)
                        trial=False
                    elif time.monotonic()-began>=timeout:
                        stop_child(child); break
                time.sleep(interval)
        except BaseException:
            stop_child(child)
            # A power/shutdown interruption leaves the journal for next startup.
            raise
        if trial:
            stop_child(child)
            with update_lock(root): rollback_trial(root,pending,'No healthy menu before deadline' if ready_at is None else 'Exited during startup check')
            continue
        if not begin_trial(root): return child.returncode

def main():
    p=argparse.ArgumentParser();p.add_argument('--root',required=True,type=Path);p.add_argument('--boot-entry',action='store_true');p.add_argument('--control',type=Path,default=Path('/userdata/rgds-boot'))
    args,extra=p.parse_known_args();root=args.root.resolve()
    if (root/'userdata/boot_stock').exists(): return 0
    if args.boot_entry:
        config=read(args.control/'config.json')
        if not (args.control/'enabled').is_file() or (args.control/'installing').exists() or config.get('mode')!='rgds' or config.get('root')!=str(root): return 0
        extra=['--boot','--boot-network-config',str(args.control/'config.json')]
        if config.get('warning',True):extra.append('--boot-warning')
    guard=root/'state/update-supervisor.lock';guard.parent.mkdir(parents=True,exist_ok=True)
    with guard.open('a') as lock:
        try:fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
        except BlockingIOError: return 1
        def stop(signum,frame): raise KeyboardInterrupt
        signal.signal(signal.SIGTERM,stop);signal.signal(signal.SIGINT,stop)
        try: return supervise(root,extra)
        except KeyboardInterrupt:return 143

if __name__=='__main__':
    try:sys.exit(main())
    except (OSError,ValueError) as error:
        print('RG DS Core recovery: '+str(error),file=sys.stderr);sys.exit(1)
