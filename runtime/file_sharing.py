"""Session-scoped SMB shares using the private Samba bundle shipped with RGDS."""
import argparse
from contextlib import contextmanager
import fcntl
import hashlib
import ipaddress
import json
import os
from pathlib import Path
import secrets
import signal
import socket
import subprocess
import sys
import time

from fields import read_fields, write_fields
from records import process_identity, read_record, write_record


def process_state(pid):
    try:
        return (Path('/proc') / str(pid) / 'stat').read_text().rsplit(')', 1)[1].split()[0]
    except (OSError, IndexError):
        return None


def boot_id():
    return Path('/proc/sys/kernel/random/boot_id').read_text().strip()


class Sharing:
    def __init__(self, data, app=None, runtime_root=None):
        self.data = Path(data)
        if runtime_root is not None:
            self.folder = Path(runtime_root)
        elif sys.platform == 'linux':
            identity = hashlib.sha256(str(self.data.resolve()).encode()).hexdigest()[:16]
            self.folder = Path('/run/rgds-samba') / identity
        else:
            self.folder = self.data / 'device/sharing'
        self.record = self.folder / 'process.json'
        self.app = Path(app) if app is not None else Path(__file__).resolve().parents[1]
        self.bundle = self.app / 'services/samba'
        # Service-only updates preserve the installed launcher. Its original
        # details screen reads this transient file instead of sharing_login.
        try:
            legacy = json.loads((self.app / 'manifest.json').read_text()).get('service_update', {}).get('legacy_sharing_details', False)
        except (OSError, ValueError):
            legacy = False
        self.compat_login = self.data / 'device/sharing/login.ini' if legacy else None
        self._process = None

    @contextmanager
    def locked(self):
        self.folder.mkdir(parents=True, exist_ok=True, mode=0o700)
        self.folder.chmod(0o700)
        with (self.folder / 'lock').open('a') as stream:
            fcntl.flock(stream, fcntl.LOCK_EX)
            yield

    def supported(self):
        return os.geteuid() == 0 and all((self.bundle / 'bin' / name).is_file()
            for name in ('smbd', 'smbpasswd', 'smbclient', 'samba-dcerpcd', 'rpcd_classic', 'rpcd_winreg', 'rpcd_lsad'))

    def environment(self):
        return dict(PATH='/usr/sbin:/usr/bin:/sbin:/bin', LC_ALL='C',
                    LD_LIBRARY_PATH=str(self.bundle / 'lib'),
                    RGDS_SAMBA_SAMBA_LIBEXECDIR=str(self.bundle / 'bin'),
                    RGDS_SAMBA_LOGFILEBASE=str(self.folder))

    def alive(self, record):
        if not record or record.get('boot') != boot_id() or process_identity(record['pid']) != record['start'] or process_state(record['pid']) in (None, 'Z'):
            return False
        try:
            # A PID/start match alone must never authorize signalling another group.
            return os.getpgid(record['pid']) == record['pid']
        except ProcessLookupError:
            return False

    @staticmethod
    def reachable(ip):
        try:
            with socket.create_connection((ip, 445), timeout=.2):
                return True
        except OSError:
            return False

    def status(self, ip):
        with self.locked():
            record = read_record(self.record)
            if self.alive(record) and record.get('ip') != ip:
                self._stop(record)
                record = None
            on = self.alive(record) and self.reachable(ip)
            if not self.alive(record):
                (self.folder / 'login.ini').unlink(missing_ok=True)
                if self.compat_login is not None:
                    self.compat_login.unlink(missing_ok=True)
            elif on and self.compat_login is not None:
                write_fields(self.compat_login, read_fields(self.folder / 'login.ini'))
                self.compat_login.chmod(0o600)
            return dict(sharing_supported=int(self.supported()), sharing=int(on),
                        sharing_address='smb://' + ip if on else '',
                        sharing_login=str(self.folder / 'login.ini') if on else '')

    def _stop(self, record):
        if self.alive(record):
            os.killpg(record['pid'], signal.SIGTERM)
            deadline = time.monotonic() + 5
            while self.alive(record):
                if time.monotonic() >= deadline:
                    raise ValueError('File sharing did not stop. Try again.')
                time.sleep(.05)
        self.record.unlink(missing_ok=True)
        (self.folder / 'login.ini').unlink(missing_ok=True)
        if self.compat_login is not None:
            self.compat_login.unlink(missing_ok=True)
        if self._process is not None:
            self._process.wait(timeout=5)
            self._process = None

    def stop(self):
        with self.locked():
            self._stop(read_record(self.record))

    @staticmethod
    def config_path(path):
        value = str(Path(path).resolve())
        if any(char in value for char in '\r\n\0%') or value != value.strip():
            raise ValueError('The sharing folder path is unsupported.')
        return value

    def configure(self, ip, roots, username):
        lines = ['[global]', 'server role = standalone server', 'security = user',
                 'map to guest = Never', 'server min protocol = SMB2', 'smb ports = 445',
                 'interfaces = ' + ip, 'bind interfaces only = yes', 'disable netbios = yes',
                 'load printers = no', 'printing = bsd', 'printcap name = /dev/null',
                 'unix extensions = no', 'wide links = no', 'follow symlinks = no',
                 'rpc start on demand helpers = no', 'nt acl support = no', 'ea support = no']
        for key in ('private dir', 'state directory', 'cache directory', 'lock directory', 'pid directory', 'ncalrpc dir'):
            folder = self.folder / key.replace(' ', '-')
            folder.mkdir(exist_ok=True, mode=0o700)
            lines.append(key + ' = ' + self.config_path(folder))
        lines.append('passdb backend = tdbsam:' + self.config_path(self.folder / 'private-dir/passdb.tdb'))
        shares = []
        seen = set()
        for root in roots:
            root = Path(root)
            if not root.is_dir() or root.is_symlink():
                continue
            path = self.config_path(root)
            if path not in seen:
                name = 'Games' if not shares else 'Games' + str(len(shares) + 1)
                shares.append((name, path)); seen.add(path)
        for name in ('Imports', 'Exports'):
            root = self.data / name.lower()
            if root.is_symlink():
                raise ValueError('The save transfer folder is unsupported.')
            root.mkdir(exist_ok=True)
            shares.append((name, self.config_path(root)))
        for name, path in shares:
            lines += ['', '[' + name + ']', 'path = ' + path, 'read only = no',
                      'valid users = ' + username, 'force user = ' + username,
                      'guest ok = no', 'follow symlinks = no', 'wide links = no']
        config = self.folder / 'smb.conf'
        config.write_text('\n'.join(lines) + '\n')
        return config, ', '.join(name for name, _path in shares)

    def start(self, ip, roots):
        ip = str(ipaddress.IPv4Address(ip))
        with self.locked():
            if not self.supported():
                raise ValueError('The RGDS file sharing service is missing or damaged. Reinstall the RGDS update.')
            record = read_record(self.record)
            if self.alive(record) and record.get('ip') == ip and self.reachable(ip):
                return
            self._stop(record)
            # Never stop or reconfigure a firmware-owned SMB service.
            try:
                with socket.socket() as check:
                    check.bind((ip, 445))
            except OSError:
                raise ValueError('SMB port is already in use or unavailable. Check the firmware sharing service.') from None
            username = 'root'
            password = secrets.token_hex(6)
            config, shares = self.configure(ip, roots, username)
            environment = self.environment()
            result = subprocess.run([str(self.bundle / 'bin/smbpasswd'), '-c', str(config), '-s', '-a', username],
                                    input=password + '\n' + password + '\n', text=True,
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                    env=environment, timeout=10)
            if result.returncode:
                raise ValueError('Could not prepare the file sharing login. See the sharing server log.')
            process = None
            try:
                with (self.folder / 'server.log').open('w') as log:
                    process = subprocess.Popen([sys.executable, '-B', str(Path(__file__).resolve()), 'serve',
                                                '--bundle', str(self.bundle), '--config', str(config)],
                                               stdin=subprocess.DEVNULL, stdout=log, stderr=log,
                                               env=environment, start_new_session=True)
                identity = process_identity(process.pid)
                if not identity:
                    raise ValueError('Could not start SMB sharing.')
                record = dict(pid=process.pid, start=identity, boot=boot_id(), ip=ip)
                write_record(self.record, record)
                write_fields(self.folder / 'login.ini', dict(username=username, password=password, shares=shares))
                (self.folder / 'login.ini').chmod(0o600)
                deadline = time.monotonic() + 5
                while process.poll() is None:
                    if self.reachable(ip):
                        # A listening port alone does not prove authentication works.
                        check = subprocess.run([str(self.bundle / 'bin/smbclient'), '//' + ip + '/Imports',
                            '-s', str(config), '-A', str(self.folder / 'login.ini'), '-c', 'quit'], env=environment,
                            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=3)
                        if check.returncode == 0 and process.poll() is None:
                            self._process = process
                            return
                    if time.monotonic() >= deadline:
                        break
                    time.sleep(.1)
                raise ValueError('SMB sharing could not start. See the sharing server log.')
            except BaseException:
                if process is not None:
                    # Also covers cancellation before the ownership record is written.
                    if process.poll() is None:
                        os.killpg(process.pid, signal.SIGTERM)
                    process.wait(timeout=5)
                self._stop(read_record(self.record))
                raise


def preview(data, enabled):
    folder = Path(data) / 'device/sharing'
    folder.mkdir(parents=True, exist_ok=True, mode=0o700)
    if enabled:
        write_fields(folder / 'login.ini', dict(username='rgds', password='preview-only', shares='Games, Imports, Exports'))
        (folder / 'login.ini').chmod(0o600)
    else:
        (folder / 'login.ini').unlink(missing_ok=True)


def serve(bundle, config):
    """Keep SMB and its RPC workers in one owned group until both have stopped."""
    children = []
    def interrupted(_signum, _frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    options = ['--foreground', '--no-process-group', '--configfile=' + str(config),
               '--log-basename=' + str(config.parent)]
    try:
        # Explicit workers avoid Samba's build-time libexec directory. This
        # also prevents accidental success through a builder-only fallback.
        workers = [str(bundle / 'bin' / name) for name in ('rpcd_classic', 'rpcd_winreg', 'rpcd_lsad')]
        children.append(subprocess.Popen([str(bundle / 'bin/samba-dcerpcd'), *options, '--np-only', *workers], stdin=subprocess.DEVNULL))
        children.append(subprocess.Popen([str(bundle / 'bin/smbd'), *options], stdin=subprocess.DEVNULL))
        while all(child.poll() is None for child in children):
            time.sleep(.1)
        return 1
    except KeyboardInterrupt:
        return 0
    finally:
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        signal.signal(signal.SIGINT, signal.SIG_IGN)
        for child in children:
            if child.poll() is None:
                child.terminate()
        for child in children:
            child.wait(timeout=5)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['serve'])
    parser.add_argument('--bundle', type=Path, required=True)
    parser.add_argument('--config', type=Path, required=True)
    args = parser.parse_args()
    sys.exit(serve(args.bundle, args.config))
