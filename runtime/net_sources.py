"""Places the handheld can read files from: local folders and SMB shares (docs/NETWORK_PLAN.md section 2).

Every backend answers the same four questions: what is in a folder (list), what is one file (stat), copy a file
into a local partial file, resuming from whatever is already there (fetch), and which shares a server offers
(SMB only). Sources are rows in network/sources.ini on the device root; their passwords live apart in
private/network.ini so the sources file can be shown, copied or shared without them.
"""
from collections import namedtuple
import configparser
import os
from pathlib import Path, PurePosixPath
import re
import secrets
import shutil
import subprocess
import tempfile
import time

from profiles import device_root

Entry = namedtuple('Entry', 'name is_dir size mtime')
TYPES = ('local', 'smb', 'sftp')
CHUNK = 1 << 20


class SourceError(Exception):
    """A message the owner can act on ("Wrong password for nas."), never a traceback. transient: worth trying
    again by itself (the Wi-Fi dropped), as opposed to a wrong password or a missing file."""
    def __init__(self, message, transient=False):
        super().__init__(message)
        self.transient = transient


class Cancelled(Exception):
    pass


def clean_path(path):
    """A path inside a source, as 'a/b/c' with no '..', no leading slash and forward slashes only."""
    parts = []
    for part in PurePosixPath(str(path).replace('\\', '/')).parts:
        if part in ('/', '.', ''):
            continue
        if part == '..':
            raise SourceError('That folder is outside the source.')
        parts.append(part)
    return '/'.join(parts)


class LocalSource:
    kind = 'local'

    def __init__(self, ident, root, name=''):
        self.id, self.root, self.name = ident, Path(root), name or Path(root).name

    def _path(self, path):
        return self.root / clean_path(path) if clean_path(path) else self.root

    def list(self, path=''):
        folder = self._path(path)
        try:
            items = list(os.scandir(folder))
        except FileNotFoundError:
            raise SourceError(f'{path or self.name} is not there any more.') from None
        except NotADirectoryError:
            raise SourceError(f'{path} is a file, not a folder.') from None
        except OSError as error:
            raise SourceError(f'Could not open {path or self.name}: {error.strerror}.') from None
        entries = []
        for item in items:
            try:
                info = item.stat()
            except OSError:
                continue
            entries.append(Entry(item.name, item.is_dir(), 0 if item.is_dir() else info.st_size, int(info.st_mtime)))
        return sorted(entries, key=lambda e: (not e.is_dir, e.name.lower()))

    def stat(self, path):
        target = self._path(path)
        try:
            info = target.stat()
        except OSError:
            raise SourceError(f'{path} is not there any more.') from None
        return Entry(target.name, target.is_dir(), 0 if target.is_dir() else info.st_size, int(info.st_mtime))

    def fetch(self, path, part, stop=lambda: False):
        source = self._path(path)
        part = Path(part)
        offset = part.stat().st_size if part.exists() else 0
        try:
            with open(source, 'rb') as reader, open(part, 'ab') as writer:
                reader.seek(offset)
                while True:
                    if stop(): raise Cancelled()
                    block = reader.read(CHUNK)
                    if not block: break
                    writer.write(block)
                writer.flush()
                os.fsync(writer.fileno())
        except OSError as error:
            raise SourceError(f'Could not read {path}: {error.strerror}.') from None


# smbclient's `ls` line: two spaces, the name, its attribute letters, the size and a ctime()-style date.
SMB_LS = re.compile(
    r'^  (?P<name>.+?)\s+(?P<attrs>[A-Z]*)\s+(?P<size>\d+)\s+'
    r'(?P<date>\w{3} \w{3} [ \d]\d \d\d:\d\d:\d\d \d{4})$'
)
SMB_STATUS = re.compile(r'NT_STATUS_[A-Z_]+')
# What the owner sees for the failures that have an obvious fix.
SMB_MESSAGES = {
    'NT_STATUS_LOGON_FAILURE': 'Wrong user name or password for {name}.',
    'NT_STATUS_ACCESS_DENIED': '{name} did not allow that. Check the share\'s permissions.',
    'NT_STATUS_BAD_NETWORK_NAME': '{name} has no share called "{share}".',
    'NT_STATUS_OBJECT_NAME_NOT_FOUND': 'That file or folder is not on {name} any more.',
    'NT_STATUS_OBJECT_PATH_NOT_FOUND': 'That folder is not on {name} any more.',
    'NT_STATUS_NOT_A_DIRECTORY': 'That is a file, not a folder.',
    'NT_STATUS_HOST_UNREACHABLE': 'Could not reach {name}. Is it on the same network?',
    'NT_STATUS_IO_TIMEOUT': 'Could not reach {name}. Is it on the same network?',
    'NT_STATUS_CONNECTION_REFUSED': '{name} is not sharing files right now.',
    'NT_STATUS_UNSUCCESSFUL': 'Could not reach {name}.',
    'NT_STATUS_CONNECTION_RESET': 'The connection to {name} dropped.',
    'NT_STATUS_CONNECTION_DISCONNECTED': 'The connection to {name} dropped.',
}
SMB_TRANSIENT = (
    'NT_STATUS_HOST_UNREACHABLE', 'NT_STATUS_IO_TIMEOUT', 'NT_STATUS_UNSUCCESSFUL', 'NT_STATUS_CONNECTION_RESET',
    'NT_STATUS_CONNECTION_DISCONNECTED', 'NT_STATUS_NETWORK_UNREACHABLE'
)
# smbclient -c splits commands at ';' and quotes with '"': names with either cannot be named safely.
SMB_UNSAFE = re.compile(r'[";\n\r]')


class SmbSource:
    """An SMB2/3 share read through the bundled smbclient. Nothing is mounted (the kernel has no CIFS)."""
    kind = 'smb'

    def __init__(
        self,
        ident,
        host,
        share,
        user='',
        password='',
        domain='',
        path='',
        name='',
        bundle=None,
        work=None,
        timeout=20
    ):
        self.id, self.host, self.share = ident, host, share
        self.user, self.password, self.domain = user, password, domain
        self.base, self.name = clean_path(path), name or host
        self.bundle = Path(bundle) if bundle else None
        self.work = Path(work) if work else work_dir()
        self.timeout = timeout

    def _binary(self):
        if self.bundle:
            binary = self.bundle / 'bin/smbclient'
            if not binary.is_file():
                raise SourceError('SMB is not installed on this system.')
            return str(binary)
        found = shutil.which('smbclient')
        if not found:
            raise SourceError('SMB is not installed on this system.')
        return found

    def _environment(self):
        environment = dict(os.environ, LC_ALL='C')
        if self.bundle:
            environment.update(
                LD_LIBRARY_PATH=str(self.bundle / 'lib'), RGDS_SAMBA_SAMBA_LIBEXECDIR=str(self.bundle / 'bin')
            )
        return environment

    def _remote(self, path):
        full = '/'.join(part for part in (self.base, clean_path(path)) if part)
        if SMB_UNSAFE.search(full):
            raise SourceError('Names with " or ; cannot be copied over SMB. Rename it on the server.')
        return full

    def _fail(self, output, fallback):
        found = SMB_STATUS.search(output or '')
        status = found.group(0) if found else ''
        message = SMB_MESSAGES.get(status)
        transient = status in SMB_TRANSIENT or not status
        if message:
            return SourceError(message.format(name=self.name, share=self.share), transient)
        return SourceError(fallback.format(name=self.name), transient)

    def _session(self):
        """smb.conf and the login file for one command: argv never carries the password (ps shows argv)."""
        self.work.mkdir(parents=True, exist_ok=True)
        folder = Path(tempfile.mkdtemp(prefix='smb-', dir=self.work))
        (folder / 'smb.conf').write_text(
            '[global]\n  client min protocol = SMB2_02\n  client max protocol = SMB3\n'
            '  log level = 0\n'
        )
        options = ['-s', str(folder / 'smb.conf'), '-t', str(self.timeout)]
        if self.user:
            login = folder / 'login'
            fd = os.open(login, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
            with os.fdopen(fd, 'w') as stream:
                stream.write(f'username = {self.user}\npassword = {self.password}\n')
                if self.domain: stream.write(f'domain = {self.domain}\n')
            options += ['-A', str(login)]
        else:
            options += ['-N']
        return folder, options

    def _run(self, arguments, fallback, start=False):
        folder, options = self._session()
        argv = [self._binary(), *options, *arguments]
        try:
            if start:
                return folder, subprocess.Popen(
                    argv,
                    env=self._environment(),
                    stdin=subprocess.DEVNULL,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True
                )
            result = subprocess.run(
                argv,
                env=self._environment(),
                stdin=subprocess.DEVNULL,
                capture_output=True,
                text=True,
                timeout=self.timeout + 10
            )
        except subprocess.TimeoutExpired:
            shutil.rmtree(folder, ignore_errors=True)
            raise SourceError(f'{self.name} did not answer.', transient=True) from None
        except OSError:
            shutil.rmtree(folder, ignore_errors=True)
            raise SourceError('SMB is not installed on this system.') from None
        shutil.rmtree(folder, ignore_errors=True)
        output = result.stdout + result.stderr
        if result.returncode != 0 or SMB_STATUS.search(output):
            raise self._fail(output, fallback)
        return output

    def _service(self):
        return f'//{self.host}/{self.share}'

    def shares(self):
        output = self._run(['-L', f'//{self.host}', '-g'], 'Could not list the shares on {name}.')
        return sorted(
            line.split('|')[1] for line in output.splitlines()
            if line.startswith('Disk|') and not line.split('|')[1].endswith('$')
        )

    def list(self, path=''):
        folder = self._remote(path)
        arguments = [self._service(), '-c', 'ls']
        if folder: arguments[1:1] = ['-D', folder]
        output = self._run(arguments, 'Could not open that folder on {name}.')
        entries = []
        for line in output.splitlines():
            match = SMB_LS.match(line)
            if not match or match['name'] in ('.', '..'):
                continue
            is_dir = 'D' in match['attrs']
            if 'H' in match['attrs'] or 'S' in match['attrs']:
                continue
            try:
                mtime = int(time.mktime(time.strptime(match['date'], '%a %b %d %H:%M:%S %Y')))
            except ValueError:
                mtime = 0
            entries.append(Entry(match['name'], is_dir, 0 if is_dir else int(match['size']), mtime))
        return sorted(entries, key=lambda e: (not e.is_dir, e.name.lower()))

    def stat(self, path):
        path = clean_path(path)
        if not path:
            return Entry(self.share, True, 0, 0)
        parent, _, name = path.rpartition('/')
        for entry in self.list(parent):
            if entry.name == name:
                return entry
        raise SourceError(f'{name} is not on {self.name} any more.')

    def fetch(self, path, part, stop=lambda: False):
        """reget resumes from the partial file's size, so a dropped Wi-Fi costs only the last moments."""
        remote = self._remote(path)
        folder_name, _, file_name = remote.rpartition('/')
        part = Path(part)
        if SMB_UNSAFE.search(str(part)):
            raise SourceError('The download folder has a name SMB cannot use.')
        command = f'reget "{file_name}" "{part}"' if part.exists() and part.stat().st_size else \
            f'get "{file_name}" "{part}"'
        arguments = [self._service(), '-c', command]
        if folder_name: arguments[1:1] = ['-D', folder_name]
        folder, process = self._run(arguments, '', start=True)
        try:
            while process.poll() is None:
                if stop():
                    process.terminate()
                    try:
                        process.wait(5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                    raise Cancelled()
                time.sleep(0.1)
            output = process.stdout.read()
        finally:
            if process.poll() is None: process.kill()
            process.stdout.close()
            shutil.rmtree(folder, ignore_errors=True)
        if process.returncode != 0 or SMB_STATUS.search(output):
            raise self._fail(output, 'The copy from {name} stopped.')


def work_dir():
    """Where per-command secrets (login files, a copy of the SSH key) go: /run on the handheld, which is RAM and
    honours 0600. Never the card: exFAT shows every file as 0777, and ssh refuses a key that others can read."""
    run = Path('/run/rgds')
    return run / 'net' if run.is_dir() else Path(tempfile.gettempdir()) / 'rgds-net'


def ssh_key(data):
    """The handheld's own SSH key (made on first use) and its public half, to paste into a server's
    authorized_keys."""
    key = device_root(data) / 'private/ssh/id_ed25519'
    if not key.exists():
        key.parent.mkdir(parents=True, exist_ok=True)
        pending = key.with_name('.pending-key')
        for leftover in (pending, pending.with_name(pending.name + '.pub')):
            leftover.unlink(missing_ok=True)
        try:
            subprocess.run(
                ['ssh-keygen', '-q', '-t', 'ed25519', '-N', '', '-C', 'rg-ds', '-f',
                 str(pending)],
                check=True,
                capture_output=True,
                timeout=30
            )
        except (OSError, subprocess.SubprocessError):
            raise SourceError('SSH is not installed on this system.') from None
        os.replace(pending.with_name(pending.name + '.pub'), key.with_name(key.name + '.pub'))
        os.replace(pending, key)
    return key, key.with_name(key.name + '.pub').read_text().strip()


# One line of `ls -la`: mode, links, owner, group, size, a date as ls writes it, then the path.
SFTP_LS = re.compile(
    r'^(?P<mode>[-dlbcps][-rwxsStT]{9})\S*\s+\S+\s+\S+\s+\S+\s+(?P<size>\d+)\s+'
    r'(?P<date>\w{3}\s+\d{1,2}\s+(?:\d{1,2}:\d{2}|\d{4}))\s(?P<path>.+)$'
)
# What ssh and sftp print for the failures that have an obvious fix; (pattern, message, worth trying again).
SFTP_MESSAGES = (
    (
        'IDENTIFICATION HAS CHANGED', '{name} is not the server it was before (its key changed). If you set it up '
        'again, remove this place and add it once more.', False
    ),
    ('Host key verification failed', '{name} is not the server it was before (its key changed).', False),
    ('Permission denied (', 'Wrong user name, password or key for {name}.', False),
    ('Too many authentication failures', 'Wrong user name, password or key for {name}.', False),
    ('Could not resolve hostname', 'Could not find {name}. Check the address.', True),
    ('Connection refused', '{name} is not accepting SFTP right now.', True),
    ('No route to host', 'Could not reach {name}. Is it on the same network?', True),
    ('Operation timed out', 'Could not reach {name}. Is it on the same network?', True),
    ('Connection timed out', 'Could not reach {name}. Is it on the same network?', True),
    ('Connection closed', 'The connection to {name} dropped.', True),
    ('Connection reset', 'The connection to {name} dropped.', True),
    ('Broken pipe', 'The connection to {name} dropped.', True),
    ('No such file or directory', 'That file or folder is not on {name} any more.', False),
    ('not found', 'That file or folder is not on {name} any more.', False),
    ('Permission denied', '{name} did not allow that.', False),
)
SFTP_UNSAFE = re.compile(r'["\\\n\r]')


class SftpSource:
    """A folder on any computer that runs SSH, read through OpenSSH's sftp in batch mode. Signs in with the
    handheld's own key, or a password through SSH_ASKPASS (never on argv). The server's key is remembered the first
    time (network/known_hosts) and a changed key stops every copy until the place is set up again."""
    kind = 'sftp'

    def __init__(
        self,
        ident,
        host,
        user='',
        password='',
        port='',
        path='',
        name='',
        key=None,
        known_hosts=None,
        work=None,
        timeout=20
    ):
        self.id, self.host, self.user, self.password = ident, host, user, password
        self.port = str(port or '22')
        self.base, self.name = clean_path(path
                                          ) if not str(path).startswith('/') else '/' + clean_path(path), name or host
        self.key = Path(key) if key else None
        self.known_hosts = Path(known_hosts) if known_hosts else None
        self.work = Path(work) if work else work_dir()
        self.timeout = timeout

    def _remote(self, path):
        inner = clean_path(path)
        if self.base.startswith('/'):
            full = self.base.rstrip('/') + ('/' + inner if inner else '') or '/'
        else:
            full = '/'.join(part for part in (self.base, inner) if part) or '.'
        if SFTP_UNSAFE.search(full):
            raise SourceError('Names with " or \\ cannot be copied over SFTP. Rename it on the server.')
        return full

    def _fail(self, output, fallback):
        for pattern, message, transient in SFTP_MESSAGES:
            if pattern in output:
                return SourceError(message.format(name=self.name), transient)
        return SourceError(fallback.format(name=self.name), True)

    def _session(self):
        self.work.mkdir(parents=True, exist_ok=True)
        os.chmod(self.work, 0o700)
        folder = Path(tempfile.mkdtemp(prefix='sftp-', dir=self.work))
        options = [
            '-F', '/dev/null', '-P', self.port, '-o', f'ConnectTimeout={self.timeout}', '-o', 'ServerAliveInterval=10',
            '-o', 'ServerAliveCountMax=3', '-o', 'StrictHostKeyChecking=accept-new', '-o', 'NumberOfPasswordPrompts=1',
            '-o', 'UpdateHostKeys=no', '-o', 'LogLevel=ERROR'
        ]
        if self.known_hosts:
            self.known_hosts.parent.mkdir(parents=True, exist_ok=True)
            options += ['-o', f'UserKnownHostsFile={self.known_hosts}']
        environment = dict(os.environ, LC_ALL='C')
        if self.key and self.key.exists():
            copy = folder / 'key'
            fd = os.open(copy, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
            with os.fdopen(fd, 'wb') as stream:
                stream.write(self.key.read_bytes())
            options += ['-i', str(copy), '-o', 'IdentitiesOnly=yes']
        if self.password:
            secret = folder / 'password'
            fd = os.open(secret, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
            with os.fdopen(fd, 'w') as stream:
                stream.write(self.password)
            helper = folder / 'askpass'
            helper.write_text(f'#!/bin/sh\ncat "{secret}"\n')
            helper.chmod(0o700)
            environment.update(SSH_ASKPASS=str(helper), SSH_ASKPASS_REQUIRE='force', DISPLAY=':0')
            options += ['-o', 'PreferredAuthentications=publickey,password,keyboard-interactive']
        else:
            options += ['-o', 'BatchMode=yes']
        target = f'{self.user}@{self.host}' if self.user else self.host
        return folder, options, environment, target

    def _start(self, commands):
        folder, options, environment, target = self._session()
        script = folder / 'batch'
        script.write_text(''.join(line + '\n' for line in commands))
        try:
            process = subprocess.Popen(
                ['sftp', *options, '-b', str(script), target],
                env=environment,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                start_new_session=True
            )
        except OSError:
            shutil.rmtree(folder, ignore_errors=True)
            raise SourceError('SFTP is not installed on this system.') from None
        return folder, process

    def _run(self, commands, fallback):
        folder, process = self._start(commands)
        try:
            output, _ = process.communicate(timeout=self.timeout + 15)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate()
            raise SourceError(f'{self.name} did not answer.', transient=True) from None
        finally:
            shutil.rmtree(folder, ignore_errors=True)
        if process.returncode != 0:
            raise self._fail(output, fallback)
        return output

    def list(self, path=''):
        folder = self._remote(path)
        output = self._run([f'ls -la "{folder}"'], 'Could not open that folder on {name}.')
        entries = []
        for line in output.splitlines():
            match = SFTP_LS.match(line)
            if not match:
                continue
            name = match['path'].rstrip('/').rsplit('/', 1)[-1]
            if name in ('.', '..') or name.startswith('.'):
                continue
            mode = match['mode'][0]
            if mode not in '-d':
                continue  # links, devices, sockets: nothing a game is
            entries.append(Entry(name, mode == 'd', 0 if mode == 'd' else int(match['size']), _ls_time(match['date'])))
        return sorted(entries, key=lambda e: (not e.is_dir, e.name.lower()))

    def stat(self, path):
        path = clean_path(path)
        if not path:
            return Entry(self.name, True, 0, 0)
        parent, _, name = path.rpartition('/')
        for entry in self.list(parent):
            if entry.name == name:
                return entry
        raise SourceError(f'{name} is not on {self.name} any more.')

    def fetch(self, path, part, stop=lambda: False):
        remote = self._remote(path)
        part = Path(part)
        if SFTP_UNSAFE.search(str(part)):
            raise SourceError('The download folder has a name SFTP cannot use.')
        resume = part.exists() and part.stat().st_size > 0
        folder, process = self._start([f'get {"-a " if resume else ""}"{remote}" "{part}"'])
        try:
            while process.poll() is None:
                if stop():
                    os.killpg(process.pid, 15)
                    try:
                        process.wait(5)
                    except subprocess.TimeoutExpired:
                        os.killpg(process.pid, 9)
                    raise Cancelled()
                time.sleep(0.1)
            output = process.stdout.read()
        finally:
            if process.poll() is None: process.kill()
            process.stdout.close()
            shutil.rmtree(folder, ignore_errors=True)
        if process.returncode != 0:
            raise self._fail(output, 'The copy from {name} stopped.')


def _ls_time(text):
    """ls's "Nov 22 10:19" (this year) or "Nov 22  2024"."""
    parts = text.split()
    try:
        if ':' in parts[2]:
            now = time.localtime()
            stamp = time.mktime(time.strptime(f'{parts[0]} {parts[1]} {now.tm_year} {parts[2]}', '%b %d %Y %H:%M'))
            # A date "in the future" is last year's (ls drops the year for the last six months).
            return int(
                stamp if stamp <= time.time() + 86400 else time.
                mktime(time.strptime(f'{parts[0]} {parts[1]} {now.tm_year - 1} {parts[2]}', '%b %d %Y %H:%M'))
            )
        return int(time.mktime(time.strptime(' '.join(parts), '%b %d %Y')))
    except (ValueError, IndexError):
        return 0


def config_paths(data):
    root = device_root(data)
    return root / 'network/sources.ini', root / 'private/network.ini'


def _read(path):
    parser = configparser.ConfigParser(interpolation=None)
    try:
        parser.read(path, encoding='utf-8')
    except configparser.Error:
        pass
    return parser


def _write(parser, path, private=False):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, pending = tempfile.mkstemp(prefix='.pending-', dir=path.parent)
    try:
        if private: os.fchmod(fd, 0o600)
        with os.fdopen(fd, 'w', encoding='utf-8') as stream:
            parser.write(stream)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(pending, path)
    finally:
        Path(pending).unlink(missing_ok=True)


FIELDS = {
    'local': ('name', 'root'),
    'smb': ('name', 'host', 'share', 'user', 'domain', 'path'),
    'sftp': ('name', 'host', 'port', 'user', 'path')
}


def sources(data):
    """{id: {type, name, ...}} without passwords, in the order they were added."""
    found = {}
    parser = _read(config_paths(data)[0])
    for ident in parser.sections():
        section = parser[ident]
        if section.get('type') in TYPES:
            found[ident] = dict(section)
    return found


def add_source(data, kind, password=None, **fields):
    if kind not in TYPES:
        raise SourceError('Unknown kind of source.')
    if kind == 'smb' and not (fields.get('host') and fields.get('share')):
        raise SourceError('An SMB source needs a server and a share.')
    if kind == 'sftp' and not fields.get('host'):
        raise SourceError('An SFTP source needs a server.')
    if kind == 'local' and not fields.get('root'):
        raise SourceError('A folder source needs a folder.')
    public_path, private_path = config_paths(data)
    public = _read(public_path)
    ident = kind + '-' + secrets.token_hex(3)
    while public.has_section(ident):
        ident = kind + '-' + secrets.token_hex(3)
    public[ident] = {'type': kind, **{key: str(fields.get(key) or '') for key in FIELDS[kind]}}
    _write(public, public_path)
    if password:
        hidden = _read(private_path)
        hidden[ident] = {'password': password}
        _write(hidden, private_path, private=True)
    return ident


def remove_source(data, ident):
    public_path, private_path = config_paths(data)
    for path, private in ((public_path, False), (private_path, True)):
        parser = _read(path)
        if parser.remove_section(ident):
            _write(parser, path, private)


def open_source(data, ident, bundle=None, work=None):
    public_path, private_path = config_paths(data)
    public = _read(public_path)
    if not public.has_section(ident):
        raise SourceError('That source was removed.')
    row = public[ident]
    if row.get('type') == 'local':
        return LocalSource(ident, row.get('root', ''), row.get('name', ''))
    if row.get('type') == 'smb':
        hidden = _read(private_path)
        password = hidden[ident].get('password', '') if hidden.has_section(ident) else ''
        return SmbSource(
            ident,
            row.get('host', ''),
            row.get('share', ''),
            row.get('user', ''),
            password,
            row.get('domain', ''),
            row.get('path', ''),
            row.get('name', ''),
            bundle=bundle,
            work=work
        )
    if row.get('type') == 'sftp':
        hidden = _read(private_path)
        password = hidden[ident].get('password', '') if hidden.has_section(ident) else ''
        key = device_root(data) / 'private/ssh/id_ed25519'
        return SftpSource(
            ident,
            row.get('host', ''),
            row.get('user', ''),
            password,
            row.get('port', ''),
            row.get('path', ''),
            row.get('name', ''),
            key=key if key.exists() else None,
            known_hosts=device_root(data) / 'network/known_hosts',
            work=work
        )
    raise SourceError('Unknown kind of source.')


def samba_bundle():
    bundle = Path(__file__).resolve().parents[1] / 'services/samba'
    return bundle if (bundle / 'bin/smbclient').is_file() else None


def _clean(text):
    return str(text).replace('\t', ' ').replace('\n', ' ').replace('\r', ' ')


def _publish(out, rows=(), error=''):
    """The launcher's answer file: 'ok' or 'error<TAB>message', then one tab-separated row per line."""
    lines = [f'error\t{_clean(error)}' if error else 'ok']
    lines += ['\t'.join(_clean(cell) for cell in row) for row in rows]
    text = '\n'.join(lines) + '\n'
    if out is None:
        print(text, end='')
        return
    out = Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    pending = out.with_name('.' + out.name + '.pending')
    pending.write_text(text, encoding='utf-8')
    os.replace(pending, out)


def main(argv=None):
    import argparse
    import sys
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--data', type=Path, required=True)
    parser.add_argument('--out', type=Path, help='answer file for the launcher (default: print)')
    parser.add_argument('--bundle', type=Path, help="the Samba bundle (default: the app's own)")
    commands = parser.add_subparsers(dest='command', required=True)
    commands.add_parser('sources')
    listing = commands.add_parser('ls')
    listing.add_argument('id')
    listing.add_argument('path', nargs='?', default='')
    commands.add_parser('test').add_argument('id')
    commands.add_parser('remove').add_argument('id')
    commands.add_parser('discover').add_argument('--wait', type=float, default=1.5)
    commands.add_parser('key')
    shares = commands.add_parser('shares')
    shares.add_argument('--host', required=True)
    shares.add_argument('--user', default='')
    shares.add_argument('--password-stdin', action='store_true')
    adding = commands.add_parser('add')
    adding.add_argument('kind', choices=TYPES)
    for field in ('name', 'host', 'share', 'user', 'domain', 'path', 'port', 'root'):
        adding.add_argument('--' + field, default='')
    adding.add_argument('--password-stdin', action='store_true')
    args = parser.parse_args(argv)
    bundle = args.bundle or samba_bundle()
    password = sys.stdin.readline().rstrip('\n') if getattr(args, 'password_stdin', False) else ''
    try:
        if args.command == 'sources':
            rows = [
                (ident, row.get('type', ''), row.get('name') or row.get('host', ''), row.get('host', ''))
                for ident, row in sources(args.data).items()
            ]
        elif args.command in ('ls', 'test'):
            source = open_source(args.data, args.id, bundle=bundle)
            entries = source.list(getattr(args, 'path', ''))
            rows = [(e.name, int(e.is_dir), e.size, e.mtime) for e in entries]
        elif args.command == 'remove':
            remove_source(args.data, args.id)
            rows = []
        elif args.command == 'discover':
            import mdns
            # RGDS_MDNS_TARGET=host:port asks one responder instead of the network (tests, a fixed lab setup).
            target = os.environ.get('RGDS_MDNS_TARGET', '')
            host, _, port = target.rpartition(':')
            where = (host, int(port)) if host and port.isdigit() else mdns.GROUP
            rows = [tuple(service) for service in mdns.discover(args.wait, target=where)]
        elif args.command == 'key':
            rows = [(ssh_key(args.data)[1],)]
        elif args.command == 'shares':
            source = SmbSource('setup', args.host, 'IPC$', args.user, password, bundle=bundle)
            rows = [(share,) for share in source.shares()]
        else:
            fields = {k: getattr(args, k) for k in ('name', 'host', 'share', 'user', 'domain', 'path', 'port', 'root')}
            ident = add_source(args.data, args.kind, password=password or None, **fields)
            rows = [(ident,)]
    except SourceError as error:
        _publish(args.out, error=str(error))
        return 1
    _publish(args.out, rows)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
