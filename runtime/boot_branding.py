#!/usr/bin/env python3
"""Plan, install or restore proven RG DS boot branding with durable originals."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import stat
import struct
import sys
import zlib

import boot_image
from boot_control import atomic, location

STORE = Path('/userdata/rgds-boot/branding')
BOOT_ALIASES = (Path('/dev/disk/by-partlabel/boot'), Path('/dev/block/by-name/boot'))
BOOT_SOURCE = Path('/sys/class/anbernic_misc/boot_device')
USER_LOGOS = ((Path('/mnt/vendor/ctrl/waiting/bootlogo.bmp'), 'waiting.bmp'),) + tuple(
    (Path('/mnt/vendor/resource') / ('res' + str(number)) / 'boot' / name, 'logo.png')
    for number in (1, 2, 3) for name in ('logo.png', 'logo2.png'))
MAX_IMAGE = boot_image.MAX_IMAGE
BLKGETSIZE64 = 0x80081272


def sha256(content):
    return hashlib.sha256(content).hexdigest()


def regular(path, maximum=MAX_IMAGE):
    path = Path(path)
    if path.is_symlink() or not path.is_file():
        raise ValueError('Expected an existing regular, non-symlink file: ' + str(path))
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
    with os.fdopen(descriptor, 'rb') as stream:
        info = os.fstat(stream.fileno())
        if not stat.S_ISREG(info.st_mode):raise ValueError('File changed while opening: ' + str(path))
        if info.st_size > maximum:raise ValueError('File exceeds the supported size: ' + str(path))
        content = stream.read(maximum + 1)
    if len(content) > maximum:raise ValueError('File exceeds the supported size: ' + str(path))
    return content


def source(value):
    if isinstance(value, bytes):
        if len(value) > MAX_IMAGE:raise ValueError('Boot image exceeds the supported size.')
        return value
    return regular(value)


def validate_png(content):
    if content[:8] != b'\x89PNG\r\n\x1a\n':raise ValueError('Userspace logo must be a PNG.')
    position = 8;seen = set();ended = False
    while position < len(content):
        if position + 12 > len(content):raise ValueError('Truncated logo PNG.')
        size = struct.unpack_from('>I', content, position)[0]
        end = position + 12 + size
        if end > len(content):raise ValueError('Truncated logo PNG chunk.')
        name = content[position + 4:position + 8]
        payload = content[position + 8:end - 4]
        checksum = struct.unpack_from('>I', content, end - 4)[0]
        if zlib.crc32(name + payload) & 0xffffffff != checksum:
            raise ValueError('Logo PNG checksum mismatch.')
        if position == 8:
            if name != b'IHDR' or size != 13:raise ValueError('Invalid logo PNG header.')
            width, height = struct.unpack_from('>II', payload)
            if (width, height) != (640, 480):raise ValueError('Userspace logo must be 640x480.')
        elif name == b'IHDR':raise ValueError('Duplicate logo PNG header.')
        seen.add(name);position = end
        if name == b'IEND':
            if size or position != len(content):raise ValueError('Invalid logo PNG end.')
            ended = True;break
    if not ended or b'IDAT' not in seen:raise ValueError('Incomplete logo PNG.')


def validate_waiting(content, replacement=False):
    if len(content) < 54 or content[:2] != b'BM':raise ValueError('Waiting logo must be a BMP.')
    length = struct.unpack_from('<I', content, 2)[0]
    offset = struct.unpack_from('<I', content, 10)[0]
    header, width, height, planes, depth, compression, pixels = struct.unpack_from('<IiiHHII', content, 14)
    size = ((330 * 3 + 3) & ~3) * 50
    if (length != len(content) or offset != 54 or header != 40 or width != 330 or
            abs(height) != 50 or (replacement and height != 50) or planes != 1 or
            depth != 24 or compression or pixels not in (0, size) or len(content) != 54 + size):
        raise ValueError('Waiting logo must be a 330x50 uncompressed 24-bit BMP.')


def assets_content(assets):
    assets = Path(assets)
    contents = {name:regular(assets / name, 2 * 1024 * 1024)
                for name in ('logo.bmp', 'logo.png', 'waiting.bmp')}
    boot_image.validate_logo(contents['logo.bmp'])
    validate_png(contents['logo.png']);validate_waiting(contents['waiting.bmp'], replacement=True)
    return contents


def sysfs_text(path):
    path = Path(path)
    if path.is_symlink() or not path.is_file():raise ValueError('Expected a real sysfs attribute: ' + str(path))
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
    with os.fdopen(descriptor, 'rb') as stream:
        if not stat.S_ISREG(os.fstat(stream.fileno()).st_mode):raise ValueError('Sysfs attribute changed while opening.')
        content = stream.read(33)
    if len(content) > 32:raise ValueError('Sysfs attribute exceeds the supported length.')
    return content.decode('ascii').strip().lower()


def discover_boot_device(system_root='/'):
    """Prove the active boot alias and partition; never invent a partition number."""
    root = Path(system_root).resolve()
    selected = sysfs_text(location(root, BOOT_SOURCE))
    parents = {'0':'mmcblk0', 'emmc':'mmcblk0', '1':'mmcblk1', 'sd':'mmcblk1'}
    if selected not in parents:raise ValueError('The active boot medium is unknown.')
    targets = []
    for alias in BOOT_ALIASES:
        path = location(root, alias)
        if path.exists() or path.is_symlink():
            try:target = path.resolve(strict=True)
            except OSError:raise ValueError('The boot partition alias is broken.')
            if root != Path('/'):
                try:target.relative_to(root)
                except ValueError:raise ValueError('The fixture boot alias escapes its system root.')
            targets.append(target)
    if not targets or len(set(targets)) != 1:
        raise ValueError('The boot partition aliases are missing or ambiguous.')
    target = targets[0];parent = parents[selected]
    if not re.fullmatch(parent + r'p[1-9][0-9]*', target.name):
        raise ValueError('The boot alias does not match the active boot medium.')
    entry = location(root, Path('/sys/class/block') / target.name)
    try:
        partition = sysfs_text(entry / 'partition')
        entry_parent = entry.resolve(strict=True).parent.name
    except OSError:raise ValueError('The boot alias has no proven sysfs partition.')
    if not partition.isdigit() or int(partition) < 1 or entry_parent != parent:
        raise ValueError('The boot alias has no proven sysfs partition.')
    return target


def open_device(boot_device, system_root='/', write=False):
    root = Path(system_root).resolve();path = Path(boot_device)
    if not path.is_absolute() or '..' in path.parts:
        raise ValueError('--boot-device must be an explicit absolute boot partition path.')
    path = path.resolve(strict=True)
    fixture = root != Path('/')
    if fixture:
        try:path.relative_to(root)
        except ValueError:raise ValueError('The boot device must stay inside the explicit fixture root.')
        if not path.is_file():raise ValueError('The fixture boot device must be a regular file.')
    else:
        if path != discover_boot_device(root):raise ValueError('The explicit device is not the proven active boot partition.')
        if write and (sys.platform != 'linux' or os.geteuid() != 0 or platform.machine() not in ('aarch64','arm64')):
            raise ValueError('Boot branding writes require root on the Linux AArch64 RG DS.')
        if not stat.S_ISBLK(path.stat().st_mode):raise ValueError('The boot target must be a block partition.')
    flags = os.O_RDWR if write else os.O_RDONLY
    expected = path.stat()
    fd = os.open(path, flags | os.O_NOFOLLOW | os.O_CLOEXEC)
    try:
        info = os.fstat(fd)
        if (info.st_dev,info.st_ino,info.st_rdev) != (expected.st_dev,expected.st_ino,expected.st_rdev):
            raise ValueError('Boot target changed while opening.')
        if fixture:
            if not stat.S_ISREG(info.st_mode):raise ValueError('Fixture boot target changed while opening.')
            size = info.st_size
        else:
            if not stat.S_ISBLK(info.st_mode):raise ValueError('Boot target changed while opening.')
            size = struct.unpack('=Q', fcntl.ioctl(fd, BLKGETSIZE64, bytes(8)))[0]
        if not 0 < size <= MAX_IMAGE:raise ValueError('Boot partition size exceeds the supported 128 MiB bound.')
        if write:fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        return fd, size, path
    except BaseException:
        os.close(fd);raise


def read_device(fd, size):
    os.lseek(fd, 0, os.SEEK_SET);chunks = [];remaining = size
    while remaining:
        chunk = os.read(fd, min(remaining, 1024 * 1024))
        if not chunk:raise ValueError('Boot partition read was incomplete.')
        chunks.append(chunk);remaining -= len(chunk)
    return b''.join(chunks)


def write_device(fd, content):
    current = read_device(fd, len(content));spans = []
    # Flash only sectors containing changed logo pixels/hash fields. Kernel,
    # device tree and partition-tail sectors are never rewritten needlessly.
    for start in range(0, len(content), 512):
        end = min(start + 512, len(content))
        if current[start:end] != content[start:end]:
            if spans and spans[-1][1] == start:spans[-1][1] = end
            else:spans.append([start,end])
    for start,end in spans:
        os.lseek(fd, start, os.SEEK_SET);pending = memoryview(content)[start:end]
        while pending:
            count = os.write(fd, pending[:1024 * 1024])
            if count <= 0:raise OSError('Boot partition write was incomplete.')
            pending = pending[count:]
    os.fsync(fd)
    if read_device(fd, len(content)) != content:raise ValueError('Boot partition readback did not match.')


def user_files(root, assets):
    root = Path(root).resolve()
    result = []
    for index, (absolute, asset) in enumerate(USER_LOGOS):
        path = location(root, absolute)
        if not path.exists() and not path.is_symlink():continue
        if path.resolve(strict=True) != path:raise ValueError('Vendor logo paths must not contain symlinks.')
        original = regular(path, 2 * 1024 * 1024)
        if asset == 'logo.png':validate_png(original)
        else:validate_waiting(original)
        result.append(dict(path=str(absolute), backup='original-user-%02d.bin' % index,
                           mode=stat.S_IMODE(path.stat().st_mode), original_sha256=sha256(original),
                           installed_sha256=sha256(assets[asset]), asset=asset,
                           original=original, installed=assets[asset]))
    return result


def proposal(boot_device, image, original, assets, system_root='/'):
    system_root = Path(system_root).resolve()
    original = source(original);image = source(image);contents = assets_content(assets)
    prepared, details = boot_image.patch(original, contents['logo.bmp'])
    if image != prepared:raise ValueError('Prepared image contains changes beyond the approved RGDS logo patch.')
    fd, size, device = open_device(boot_device, system_root)
    try:
        if size != len(original):raise ValueError('Original must be an exact snapshot of the complete boot partition.')
        if read_device(fd, size) != original:raise ValueError('Current boot partition no longer matches the original snapshot.')
    finally:os.close(fd)
    return dict(schema=1, state='prepared', boot_device=str(device), size=size,
                original_sha256=sha256(original), installed_sha256=sha256(image),
                assets={name:sha256(content) for name,content in contents.items()},
                files=user_files(system_root, contents), details=details,
                original=original, installed=image)


def metadata(proposed):
    record = {key:([{name:value for name,value in item.items() if name not in ('original','installed')}
                   for item in value] if key == 'files' else value)
            for key,value in proposed.items() if key not in ('original','installed')}
    return json.loads(json.dumps(record))


def plan(boot_device, image, original, assets, system_root='/'):
    result = metadata(proposal(boot_device, image, original, assets, system_root))
    result['backup_directory'] = str(STORE)
    result['preserves'] = ['partition size and tail', 'kernel and device tree', 'vendor init', 'Wi-Fi', 'Weston']
    return result


def prepare_current(assets, boot_device, system_root='/'):
    contents = assets_content(assets);fd, size, device = open_device(boot_device, system_root)
    try:original = read_device(fd, size)
    finally:os.close(fd)
    prepared, details = boot_image.patch(original, contents['logo.bmp'])
    return dict(original=original, image=prepared, boot_device=device, details=details)


def store_path(system_root):
    path = location(Path(system_root).resolve(), STORE)
    if path.is_symlink() or any(parent.is_symlink() for parent in path.parents):
        raise ValueError('Branding backup directory must not contain symlinks.')
    return path


def save_record(store, record):
    atomic(store / 'manifest.json', (json.dumps(record, sort_keys=True) + '\n').encode())


def durable_store(store):
    missing = [];parent = store
    while not parent.exists():
        missing.append(parent);parent = parent.parent
    store.mkdir(parents=True, exist_ok=True, mode=0o700)
    # Persist each newly created directory entry, including branding itself.
    for directory in missing + [parent]:
        descriptor = os.open(directory, os.O_RDONLY | os.O_DIRECTORY)
        try:os.fsync(descriptor)
        finally:os.close(descriptor)


def load_record(store):
    record = json.loads(regular(store / 'manifest.json', 1024 * 1024))
    if not isinstance(record, dict) or record.get('schema') != 1 or not isinstance(record.get('files'), list):
        raise ValueError('Invalid branding backup manifest.')
    allowed = {str(path):('original-user-%02d.bin' % index,asset) for index,(path,asset) in enumerate(USER_LOGOS)}
    seen = set()
    for item in record['files']:
        if (not isinstance(item, dict) or item.get('path') not in allowed or item['path'] in seen or
                (item.get('backup'), item.get('asset')) != allowed[item['path']] or
                not isinstance(item.get('mode'), int) or not 0 <= item['mode'] <= 0o7777):
            raise ValueError('Invalid userspace branding provenance.')
        seen.add(item['path'])
    return record


def original_backups(store, record):
    original = regular(store / 'original-boot.bin')
    if len(original) != record.get('size') or sha256(original) != record.get('original_sha256'):
        raise ValueError('The original boot backup does not match its provenance.')
    boot_image.inspect(original)
    files = []
    for item in record['files']:
        content = regular(store / item['backup'], 2 * 1024 * 1024)
        if sha256(content) != item.get('original_sha256'):
            raise ValueError('An original logo backup does not match its provenance.')
        files.append((item, content))
    return original, files


def replace_user(path, content, mode, expected=None):
    path = Path(path)
    if path.resolve(strict=True) != path:raise ValueError('Vendor logo paths must not contain symlinks.')
    current = regular(path, 2 * 1024 * 1024)
    if expected is not None and current != expected:raise ValueError('Vendor logo changed externally before writing.')
    atomic(path, content, mode)
    if regular(path, 2 * 1024 * 1024) != content:raise ValueError('Userspace logo readback did not match.')


def apply(boot_device, image, original, assets, system_root='/'):
    system_root = Path(system_root).resolve()
    proposed = proposal(boot_device, image, original, assets, system_root)
    store = store_path(system_root);record = metadata(proposed)
    fd, size, device = open_device(boot_device, system_root, write=True)
    partition_started = False;changed = []
    try:
        if size != proposed['size'] or str(device) != proposed['boot_device'] or read_device(fd, size) != proposed['original']:
            raise ValueError('Boot partition changed before installation.')
        durable_store(store)
        manifest = store / 'manifest.json'
        if manifest.exists() or manifest.is_symlink():
            saved = load_record(store);original_backups(store, saved)
            if saved.get('state') != 'restored' or any(saved.get(key) != record.get(key) for key in
                ('boot_device','size','original_sha256','installed_sha256','assets','files')):
                raise ValueError('Existing branding backups belong to another installation; restore or inspect them first.')
        backups = [('original-boot.bin', proposed['original'])] + [(item['backup'], item['original']) for item in proposed['files']]
        for name, content in backups:
            path = store / name
            if path.exists() or path.is_symlink():
                if regular(path) != content:raise ValueError('Existing original backup differs; refusing to overwrite it.')
            else:atomic(path, content)
        save_record(store, record)  # All originals + provenance are durable before the first vendor write.
        if read_device(fd, size) != proposed['original']:raise ValueError('Boot partition changed before installation.')
        partition_started = True;write_device(fd, proposed['installed'])
        for item in proposed['files']:
            path = location(system_root, item['path'])
            if regular(path) != item['original']:raise ValueError('A vendor logo changed before installation.')
            changed.append(item);replace_user(path, item['installed'], item['mode'], expected=item['original'])
        record['state'] = 'installed';save_record(store, record)
        return record
    except BaseException as error:
        if partition_started:
            failures = []
            try:write_device(fd, proposed['original'])
            except (OSError, ValueError) as failure:failures.append(str(failure))
            for item in reversed(changed):
                try:
                    path = location(system_root, item['path']);current = regular(path)
                    if current not in (item['original'], item['installed']):raise ValueError('Vendor logo changed externally during rollback.')
                    replace_user(path, item['original'], item['mode'], expected=current)
                except (OSError, ValueError) as failure:failures.append(str(failure))
            record['state'] = 'recovery_required' if failures else 'restored'
            try:save_record(store, record)
            except OSError:pass
            if failures:raise ValueError(str(error) + '; original backups retained; rollback failed: ' + '; '.join(failures)) from error
        raise
    finally:os.close(fd)


def verify_installed(boot_device, system_root='/', assets=None):
    system_root = Path(system_root).resolve()
    store = store_path(system_root);record = load_record(store);original_backups(store, record)
    if record.get('state') != 'installed':raise ValueError('Branding is not fully installed.')
    if assets is not None and record.get('assets') != {name:sha256(content) for name,content in assets_content(assets).items()}:
        raise ValueError('Installed branding differs from the requested assets.')
    fd, size, device = open_device(boot_device, system_root)
    try:
        if str(device) != record.get('boot_device') or size != record.get('size') or sha256(read_device(fd, size)) != record.get('installed_sha256'):
            raise ValueError('Installed boot partition changed externally; refusing to overwrite it.')
    finally:os.close(fd)
    recorded = {item['path'] for item in record['files']}
    if any((location(system_root, path).exists() or location(system_root, path).is_symlink()) !=
           (str(path) in recorded) for path,_asset in USER_LOGOS):
        raise ValueError('Vendor logo layout changed externally; refusing to overwrite it.')
    for item in record['files']:
        path = location(system_root, item['path'])
        if path.resolve(strict=True) != path or sha256(regular(path)) != item.get('installed_sha256'):
            raise ValueError('Installed vendor logo changed externally; refusing to overwrite it.')
    return record


def rebrand_current(assets, boot_device, system_root='/'):
    """Rotate an owned installation via verified stock bytes; retain every backup.

    Each boundary is bootable. A failure leaves either the previous branding or
    stock artwork, with unchanged kernel/DTB and durable original provenance.
    """
    system_root = Path(system_root).resolve()
    verify_installed(boot_device, system_root)
    store = store_path(system_root);saved = load_record(store)
    contents = assets_content(assets)
    if saved['assets'] == {name:sha256(content) for name,content in contents.items()}:
        return verify_installed(boot_device, system_root, assets)
    original, _files = original_backups(store, saved)
    prepared, _details = boot_image.patch(original, contents['logo.bmp'])
    archive = store.with_name(store.name + '.previous-' + saved['installed_sha256'][:16])
    if archive.exists() or archive.is_symlink():
        raise ValueError('Previous branding archive already exists; inspect retained backups first.')
    # restore() verifies ownership again immediately before any writes.
    restore(boot_device, system_root)
    store.rename(archive)
    parent_fd = os.open(str(archive.parent), os.O_RDONLY | os.O_DIRECTORY)
    try:os.fsync(parent_fd)
    finally:os.close(parent_fd)
    return apply(boot_device, prepared, original, assets, system_root)


def configure_current(assets, boot_device=None, system_root='/'):
    device = boot_device if boot_device is not None else discover_boot_device(system_root)
    store = store_path(system_root)
    if (store / 'manifest.json').exists():
        saved = load_record(store)
        if saved.get('state') == 'installed':return rebrand_current(assets, device, system_root)
    prepared = prepare_current(assets, device, system_root)
    return apply(device, prepared['image'], prepared['original'], assets, system_root)


def restore(boot_device, system_root='/'):
    system_root = Path(system_root).resolve()
    store = store_path(system_root);record = load_record(store);original, files = original_backups(store, record)
    fd, size, device = open_device(boot_device, system_root, write=True)
    started = False;changed = [];installed = None
    try:
        if str(device) != record.get('boot_device') or size != len(original):raise ValueError('Restore target differs from the recorded boot partition.')
        installed = read_device(fd, size)
        expected = 'original_sha256' if record.get('state') == 'restored' else 'installed_sha256'
        if sha256(installed) != record.get(expected):raise ValueError('Boot partition changed externally; refusing restore.')
        current_files = []
        for item, content in files:
            path = location(system_root, item['path'])
            if path.resolve(strict=True) != path:raise ValueError('Vendor logo paths must not contain symlinks.')
            current = regular(path)
            if sha256(current) != item.get(expected):raise ValueError('Vendor logo changed externally; refusing restore.')
            current_files.append((item, content, current))
        if record.get('state') == 'restored':return record
        if record.get('state') != 'installed':raise ValueError('Incomplete branding installation requires inspection of retained originals.')
        started = True;write_device(fd, original)
        for item, content, current in current_files:
            changed.append((item,current));replace_user(location(system_root, item['path']), content, item['mode'], expected=current)
        record['state'] = 'restored';save_record(store, record)
        return record
    except BaseException as error:
        if started:
            failures = []
            try:write_device(fd, installed)
            except (OSError, ValueError) as failure:failures.append(str(failure))
            for item,current in reversed(changed):
                try:
                    path = location(system_root, item['path']);now = regular(path)
                    original_user = next(content for candidate,content in files if candidate['path'] == item['path'])
                    if now not in (original_user, current):raise ValueError('Vendor logo changed externally during restore rollback.')
                    replace_user(path, current, item['mode'], expected=now)
                except (OSError, ValueError) as failure:failures.append(str(failure))
            record['state'] = 'recovery_required' if failures else 'installed'
            try:save_record(store, record)
            except OSError:pass
            if failures:raise ValueError(str(error) + '; restore rollback failed; retained originals: ' + '; '.join(failures)) from error
        raise
    finally:os.close(fd)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('plan','apply','restore'))
    parser.add_argument('--boot-device', type=Path, required=True)
    parser.add_argument('--image', type=Path)
    parser.add_argument('--original', type=Path)
    parser.add_argument('--assets', type=Path)
    parser.add_argument('--system-root', type=Path, default=Path('/'))
    args = parser.parse_args()
    if args.action != 'restore' and any(value is None for value in (args.image,args.original,args.assets)):
        parser.error('--image, --original and --assets are required for plan/apply')
    try:
        result = restore(args.boot_device,args.system_root) if args.action == 'restore' else (
            plan if args.action == 'plan' else apply)(args.boot_device,args.image,args.original,args.assets,args.system_root)
        print(json.dumps(result, indent=2, sort_keys=True));return 0
    except (OSError, ValueError) as error:
        print('RGDS boot branding: ' + str(error), file=sys.stderr);return 1


if __name__ == '__main__':sys.exit(main())
