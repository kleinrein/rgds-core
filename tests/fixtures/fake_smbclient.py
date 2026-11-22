"""A stand-in for Samba's smbclient: the arguments and output RG DS relies on, served from a folder.

FAKE_SMB_ROOT/<share>/... is the server, FAKE_SMB_PASSWORD the one password it takes (empty = guests only),
FAKE_SMB_ARGV a file each call's argv is appended to, FAKE_SMB_DROP=<bytes> makes the first get stop after that
many bytes with a connection reset (once; FAKE_SMB_DROP_MARK remembers it happened).
"""
import os
from pathlib import Path
import shlex
import sys
import time


def fail(status):
    print(f'tree connect failed: {status}' if 'NAME' in status else status)
    sys.exit(1)


def main(argv):
    if os.environ.get('FAKE_SMB_ARGV'):
        with open(os.environ['FAKE_SMB_ARGV'], 'a') as stream:
            stream.write(' '.join(argv) + '\n')
    options, service, i = {}, None, 0
    while i < len(argv):
        arg = argv[i]
        if arg in ('-s', '-t', '-A', '-D', '-c', '-L'):
            options[arg] = argv[i + 1]
            i += 2
        elif arg in ('-N', '-g'):
            options[arg] = True
            i += 1
        else:
            service = arg
            i += 1
    password = os.environ.get('FAKE_SMB_PASSWORD', '')
    if '-A' in options:
        login = dict(line.split(' = ', 1) for line in Path(options['-A']).read_text().splitlines() if ' = ' in line)
        if login.get('password') != password:
            fail('session setup failed: NT_STATUS_LOGON_FAILURE')
    elif password:
        fail('session setup failed: NT_STATUS_ACCESS_DENIED')
    root = Path(os.environ['FAKE_SMB_ROOT'])
    if '-L' in options:
        for share in sorted(p.name for p in root.iterdir() if p.is_dir()):
            print(f'Disk|{share}|')
        print('IPC|IPC$|IPC Service')
        print('Disk|print$|Printer Drivers')
        return
    share = service.strip('/').split('/', 1)[1]
    folder = root / share
    if not folder.is_dir():
        fail('NT_STATUS_BAD_NETWORK_NAME')
    if '-D' in options:
        folder = folder / options['-D']
        if not folder.is_dir():
            fail('NT_STATUS_OBJECT_PATH_NOT_FOUND')
    words = shlex.split(options['-c'])
    if words[0] == 'ls':
        stamp = time.strftime('%a %b %e %H:%M:%S %Y', time.localtime(1700000000))
        for name, attrs, size in [('.', 'D', 0), ('..', 'D', 0)] + [
            (
                p.name, 'D' if p.is_dir() else
                ('AH' if p.name.startswith('.') else 'A'), 0 if p.is_dir() else p.stat().st_size
            ) for p in sorted(folder.iterdir())
        ]:
            print(f'  {name:<35} {attrs:>2} {size:>10}  {stamp}')
        print('\n\t\t61202244 blocks of size 1024. 1234 blocks available')
        return
    if words[0] in ('get', 'reget'):
        remote, local = folder / words[1], Path(words[2])
        if not remote.is_file():
            fail('NT_STATUS_OBJECT_NAME_NOT_FOUND opening remote file \\' + words[1])
        offset = local.stat().st_size if words[0] == 'reget' and local.exists() else 0
        data = remote.read_bytes()[offset:]
        drop = int(os.environ.get('FAKE_SMB_DROP', '0') or 0)
        mark = os.environ.get('FAKE_SMB_DROP_MARK')
        dropping = drop and mark and not Path(mark).exists()
        with open(local, 'ab' if offset else 'wb') as stream:
            stream.write(data[:drop] if dropping else data)
        if dropping:
            Path(mark).write_text('dropped')
            print('getting file ... NT_STATUS_CONNECTION_RESET')
            sys.exit(1)
        print(f'getting file \\{words[1]} of size {len(data)} as {local}')
        return
    fail('NT_STATUS_NOT_SUPPORTED')


if __name__ == '__main__':
    main(sys.argv[1:])
