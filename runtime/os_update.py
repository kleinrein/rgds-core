"""RG DS OS updates: a signed image written to the slot that is not running (docs/OTA.md, "RG DS OS").

The feed, the download and its signature are ota.py's; what differs is what an update is. A release is a zip holding
<build>/rootfs.squashfs, Image.lz4, the device tree, extlinux.conf (both LABEL blocks) and manifest.json (their
hashes). Staging writes the root filesystem to the other slot's partition and its kernel and device tree to that
slot's folder on the boot partition, and checks every byte back; nothing the running slot uses is touched. Applying
switches extlinux.conf's DEFAULT to the new slot with the running one as FALLBACK and arms U-Boot's boot counter
(/usr/libexec/rgds/trial), then restarts: if the new slot does not come up and commit itself within three boots, U-Boot
boots the old one again and the launcher is told. The trust file and the update guard ship with the image (app/os/).
"""
import hashlib
import os
from pathlib import Path
import shutil
import stat
import subprocess
import zipfile

import ota
from ota import Updater, guard_module

TARGET = 'rgds-os-rk3566'
BASE = 'rgds-os'
DTB = 'rk3568-anbernic-rg-ds.dtb'
PAYLOAD = ('rootfs.squashfs', 'Image.lz4', DTB, 'extlinux.conf')
PARTITIONS = {'a': 2, 'b': 3}
TRIAL = Path('/usr/libexec/rgds/trial')
BOOT = Path('/boot')
RUN = Path('/run/rgds')
BLOCK = 1024 * 1024


def replace_label(conf, slot, template):
    """extlinux.conf with `slot`'s LABEL block taken from the template (every other line unchanged)."""
    def blocks(text):
        result, current = {}, None
        for line in text.splitlines():
            if line.startswith('LABEL '):
                current = line.split()[1]
                result[current] = [line]
            elif current and line.startswith(' '):
                result[current].append(line)
            else:
                current = None
        return result

    wanted = blocks(template)[slot]
    out, skipping, done = [], False, False
    for line in conf.splitlines():
        if line.startswith('LABEL '):
            skipping = line.split()[1] == slot
            if skipping:
                out.extend(wanted)
                done = True
                continue
        elif skipping and line.startswith(' '):
            continue
        skipping = False
        out.append(line)
    if not done:
        out.extend(wanted)
    return '\n'.join(out) + '\n'


def hash_file(path, size=None, report=None):
    h = hashlib.sha256()
    done = 0
    with open(path, 'rb') as stream:
        while size is None or done < size:
            block = stream.read(BLOCK if size is None else min(BLOCK, size - done))
            if not block: break
            h.update(block)
            done += len(block)
            if report: report(done)
    return h.hexdigest()


class OsUpdater(Updater):
    target = TARGET
    base = BASE

    def __init__(self, app, data, preview=False, opener=None, boot=BOOT, run_dir=RUN, trial=TRIAL):
        super().__init__(app, data, preview, opener)
        self.boot = Path(boot)
        self.run_dir = Path(run_dir)  # not self.run: that is Updater.run(), the launcher's entry point
        self.trial = Path(trial)

    # -- what ships with the image: the trust file (the owner's copy in state/ wins) and the guard --
    def config(self):
        if (self.root / 'state/update-trust.json').is_file():
            config = super().config()
        else:
            try:
                config = ota.strict_json((self.app / 'os/update-trust.json').read_bytes())
            except FileNotFoundError:
                return {}
            if not isinstance(config, dict) or config.get('schema') != 1:
                raise ValueError('Update configuration is invalid.')
        feed = config.get('feed')
        if feed and not feed.endswith('-os.json'):
            # the OS feed sits beside the application's: stable.json -> stable-os.json. Also for a trust file in
            # state/, which a card set up from a stock rgds/ folder carries with the application's feed in it.
            config = dict(config, feed=feed.rsplit('.json', 1)[0] + '-os.json')
        return config

    def load_guard(self):
        # always the image's: an update_guard.py on the data partition is a stock installation's leftover
        return guard_module(self.app / 'os')

    def staging_space(self, value):
        return 0  # the payload goes to the other slot, not the data partition

    # -- the slot switch on trial, finished here once the launcher asks about updates --
    def commit_trial(self):
        if not self.trial.is_file(): return
        status = subprocess.run([str(self.trial), 'status'], capture_output=True, text=True).stdout.strip()
        if status.startswith('trial'):
            subprocess.run([str(self.trial), 'commit'], check=False)
        guard = self.load_guard()
        pending = guard.read(self.root / 'state/update-pending.json')
        if pending.get('phase') != 'trial': return
        result = guard.read(self.root / 'state/update-result.json')
        (self.root / 'state/update-pending.json').unlink(missing_ok=True)
        if result.get('state') == 'rolled-back' and result.get('build') == pending.get('build'):
            self.publish(
                'rolled-back', 'The update could not start. Your previous version was restored.', prepared=0, offer=''
            )
        else:
            self.publish(
                'idle',
                'You are up to date.',
                prepared=0,
                offer='',
                version=pending.get('version', ''),
                current=self.app.name
            )

    def info(self):
        self.commit_trial()
        return super().info()

    def auto(self):
        self.commit_trial()
        return super().auto()

    def power_check(self):
        from system_controls import Hardware
        values = Hardware(self.data).status()
        if not values.get('external_power') and (values.get('battery', -1) < 20):
            raise ValueError('Connect the charger before installing an update.')

    # -- the slots --
    def slots(self):
        running = (self.run_dir / 'slot').read_text().strip()
        disk = (self.run_dir / 'disk').read_text().strip()
        if running not in PARTITIONS or not disk.startswith('/dev/'):
            raise ValueError('Update failed. Check Wi-Fi and try again.')
        other = 'b' if running == 'a' else 'a'
        return running, other, Path(f'{disk}p{PARTITIONS[other]}')

    def inventory(self, z, value):
        build = value['build']
        entries = {e.filename: e for e in z.infolist()}
        if len(entries) != len(z.infolist()) or len(entries) > 16 or sum(e.file_size for e in entries.values()
                                                                         ) != value['unpacked_size']:
            raise ValueError('Update archive inventory is invalid.')
        for name, entry in entries.items():
            p = Path(name)
            if p.is_absolute() or '..' in p.parts or '\\' in name or p.as_posix() != name or len(
                p.parts
            ) != 2 or p.parts[0] != build or entry.is_dir() or stat.S_ISLNK(entry.external_attr >> 16):
                raise ValueError('Update contains an unsafe path.')
        manifest = ota.strict_json(z.read(f'{build}/manifest.json'))
        files = manifest.get('files') if isinstance(manifest, dict) else None
        if manifest.get('schema') != 1 or manifest.get('build') != build or manifest.get(
            'target'
        ) != TARGET or not isinstance(files, dict) or set(files) != set(PAYLOAD) or set(entries) != {
            f'{build}/{n}'
            for n in (*PAYLOAD, 'manifest.json')
        }:
            raise ValueError('Update archive inventory is invalid.')
        for name, digest in files.items():
            if not isinstance(digest, str) or len(digest) != 64 or any(c not in '0123456789abcdef' for c in digest):
                raise ValueError('Update archive inventory is invalid.')
        return {name: (entries[f'{build}/{name}'], files[name]) for name in PAYLOAD}

    def partition_size(self, device):
        return int(Path('/sys/class/block', device.name, 'size').read_text()) * 512

    def write_partition(self, z, entry, digest, device):
        size = entry.file_size
        if self.partition_size(device) < size: raise ValueError('There is not enough free space for this update.')
        h = hashlib.sha256()
        done = 0
        with z.open(entry) as source, open(device, 'wb', buffering=0) as out:
            for block in iter(lambda: source.read(BLOCK), b''):
                out.write(block)
                h.update(block)
                done += len(block)
                self.publish('staging', 'Writing the new system…', progress=done * 90 // (size * 2))
            out.flush()
            os.fsync(out.fileno())
        if h.hexdigest() != digest: raise ValueError('The downloaded update failed verification. Please retry.')
        read_back = hash_file(
            device, size,
            lambda n: self.publish('staging', 'Checking the new system…', progress=45 + n * 90 // (size * 2))
        )
        if read_back != digest: raise ValueError('The new system did not write correctly. Please retry.')

    def write_boot_file(self, z, entry, digest, target):
        target.parent.mkdir(parents=True, exist_ok=True)
        new = target.with_name(target.name + '.new')
        with z.open(entry) as source, new.open('wb') as out:
            shutil.copyfileobj(source, out, BLOCK)
            out.flush()
            os.fsync(out.fileno())
        if hash_file(new) != digest:
            new.unlink()
            raise ValueError('The downloaded update failed verification. Please retry.')
        os.replace(new, target)
        if hash_file(target) != digest: raise ValueError('The new system did not write correctly. Please retry.')

    def remount(self, mode):
        subprocess.run(['sync'], check=False)
        subprocess.run(['mount', '-o', f'remount,{mode}', str(self.boot)], check=True)

    def stage(self, archive, value):
        from emulators import activity_lock, core_running
        running, other, device = self.slots()
        self.publish('staging', 'Preparing update…', progress=0)
        with zipfile.ZipFile(archive) as z:
            payload = self.inventory(z, value)
            with activity_lock(self.data):
                if core_running(self.data): raise ValueError('Exit your game before installing an update.')
                entry, digest = payload['rootfs.squashfs']
                self.write_partition(z, entry, digest, device)
                self.remount('rw')
                try:
                    for name in ('Image.lz4', DTB):
                        entry, digest = payload[name]
                        self.write_boot_file(z, entry, digest, self.boot / other / name)
                    template = z.read(payload['extlinux.conf'][0]).decode('utf-8')
                    conf = self.boot / 'extlinux/extlinux.conf'
                    new = replace_label(conf.read_text(), other, template)
                    conf.with_name('extlinux.conf.new').write_text(new)
                    os.replace(conf.with_name('extlinux.conf.new'), conf)
                finally:
                    self.remount('ro')
                self.publish('staging', 'Preparing update…', progress=100)
                self.guard.save(
                    self.root / 'state/update-pending.json',
                    dict(
                        schema=1,
                        phase='staged',
                        build=value['build'],
                        previous=self.app.name,
                        slot=other,
                        running=running,
                        sequence=value['sequence'],
                        version=value['version']
                    )
                )

    def apply(self):
        self.setup()
        from emulators import activity_lock, core_running
        with self.guard.update_lock(self.root), activity_lock(self.data):
            if core_running(self.data): raise ValueError('Exit your game before installing an update.')
            self.power_check()
            pending = self.guard.read(self.root / 'state/update-pending.json')
            running, other, _ = self.slots()
            if pending.get('phase') != 'staged' or pending.get('previous') != self.app.name or pending.get(
                'slot'
            ) != other or pending.get('running') != running:
                raise ValueError('Check for updates again.')
            subprocess.run(
                [str(self.trial), 'arm', other, running, pending['build'],
                 pending.get('version', '')], check=True
            )
            self.guard.save(self.root / 'state/update-pending.json', dict(pending, phase='trial'))
            self.publish('restarting', 'Restarting into the update…', progress=100, offer='')
            self.restart()

    def restart(self):
        # The launcher fades out on the 'restarting' state; the handheld restarts behind it.
        subprocess.Popen(['sh', '-c', 'sleep 3; exec /sbin/reboot'], start_new_session=True)
