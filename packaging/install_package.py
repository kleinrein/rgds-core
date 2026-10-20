#!/usr/bin/env python3
"""Shared SD/SSH package installer. Checksums verify integrity, not publisher identity."""
import hashlib, json, os, platform, re, shutil, stat, sys, zipfile
from pathlib import Path
sys.dont_write_bytecode = True
import argparse
from contextlib import ExitStack


def unconfigured_trust(path):
    try:
        value=json.loads(Path(path).read_text())
    except (OSError,ValueError):
        return False
    return isinstance(value,dict) and (not value.get('feed') or not value.get('keys'))

RETIRED_APPS = ('Try RGDS.sh', 'RGDS Probe.sh', 'RGDS Inventory.sh', 'RGDS Recovery.sh')

def install_apps(source, apps, stock_apps=None):
    """Refresh menu routes; retired entries from earlier installs are removed."""
    legacy, launcher, uninstall = 'Try RGDS.sh', 'RG DS Core.sh', 'Uninstall RG DS Core.sh'
    entries = {entry.name: entry for entry in source.iterdir()}
    if legacy in entries:
        old = entries.pop(legacy)
        entries.setdefault(launcher, old)
    apps.mkdir(parents=True, exist_ok=True)
    for name, entry in entries.items():
        shutil.copy2(entry, apps / name)
    if stock_apps is None:
        stock_apps = (Path('/mnt/mmc/Roms/APPS'), Path('/mnt/SDCARD/Roms/APPS'))
    for directory in (apps, *stock_apps):
        if not directory.is_dir():
            continue
        for name in RETIRED_APPS:
            retired = directory / name
            if retired.is_file() and not retired.is_symlink():
                retired.unlink()
        if directory.resolve() == apps.resolve():
            continue
        for name in (launcher, uninstall):
            entry, destination = entries.get(name), directory / name
            # Refresh existing menu entries without following entry symlinks.
            if entry is not None and destination.is_file() and not destination.is_symlink():
                shutil.copy2(entry, destination)

def main():
    with ExitStack() as resources:
        parser = argparse.ArgumentParser(description="Verify and install an RG DS Core package on a stock Linux card.")
        parser.add_argument("root", type=Path)
        parser.add_argument("archive", type=Path)
        parser.add_argument("--check", action="store_true", help="Validate and report without writing files")
        parser.add_argument("--sha256", help="Expected whole-package checksum from the installer bundle")
        args = parser.parse_args()
        root, archive_path = args.root, args.archive
        if archive_path.is_symlink() or not archive_path.is_file():
            raise ValueError("Package must be a regular file")
        if args.sha256:
            if not re.fullmatch(r"[0-9a-f]{64}",args.sha256):raise ValueError("Invalid package checksum")
            digest=hashlib.sha256()
            with archive_path.open("rb") as stream:
                for block in iter(lambda:stream.read(1024*1024),b""):digest.update(block)
            if digest.hexdigest()!=args.sha256:raise ValueError("Package checksum mismatch")
        if sys.version_info < (3,8) or (not args.check and platform.machine() not in ('aarch64', 'arm64')):
            raise SystemExit('Need Linux AArch64 and Python 3.8+; keep stock.')
        allowed_apps = {'RG DS Core.sh','Uninstall RG DS Core.sh'}
        with zipfile.ZipFile(archive_path) as archive:
            entries = archive.infolist()
            # Private packages include emulator/driver and Samba source archives.
            # Keep extraction bounded; the actual required free space is checked below.
            if sum(e.file_size for e in entries) > 512*1024*1024:
                raise ValueError('Package exceeds size limit')
            names = [e.filename for e in entries]
            if len(names) != len(set(names)):
                raise ValueError('Duplicate archive entries')
            build = archive.read('rgds/state/active').decode().strip()
            if not re.fullmatch(r'[a-z0-9][a-z0-9.-]{0,100}',build):
                raise ValueError('Invalid build ID')
            prefix = 'rgds/releases/' + build + '/'
            manifest = json.loads(archive.read(prefix+'manifest.json'))
            if manifest.get('build') != build or manifest.get('schema') != 1 or manifest.get('project') != 'rgds' or manifest.get('target') != 'aarch64-linux-gnu':
                raise ValueError('Wrong manifest')
            for e in entries:
                p = Path(e.filename)
                if p.is_absolute() or '..' in p.parts or '\\' in e.filename or stat.S_ISLNK(e.external_attr >> 16):
                    raise ValueError('Unsafe archive path')
                permitted = e.filename.startswith(prefix) or e.filename.startswith('rgds/docs/') or e.filename in ('rgds/main.sh','rgds/state/active','rgds/update_guard.py','rgds/state/update-trust.json') or (p.parent == Path('Roms/APPS') and p.name in allowed_apps)
                if not permitted or e.is_dir():
                    raise ValueError('Unexpected package entry: '+e.filename)
            actual = {n[len(prefix):] for n in names if n.startswith(prefix) and n != prefix+'manifest.json'}
            files = manifest.get('files',{})
            if actual != set(files) or 'bin/rgds-launcher' not in files:
                raise ValueError('Manifest file list mismatch')
            for name, digest in files.items():
                if not re.fullmatch(r'[0-9a-f]{64}',digest) or hashlib.sha256(archive.read(prefix+name)).hexdigest() != digest:
                    raise ValueError('SHA-256 mismatch: '+name)
            # The archive is fully verified before any write or bundled code is executed.
            required=sum(e.file_size for e in entries)+16*1024*1024
            probe=root
            while not probe.exists():probe=probe.parent
            if args.check:
                print(json.dumps(dict(build=build,files=len(entries),required_free_bytes=required,
                                      free_bytes=shutil.disk_usage(probe).free,mode="check")))
                raise SystemExit(0)
            if shutil.disk_usage(probe).free<required:raise ValueError("Not enough space for staged installation")
            # All install routes share one lock. Never overlap an OTA staging operation.
            import fcntl
            (root/'state').mkdir(parents=True,exist_ok=True)
            install_lock=resources.enter_context((root/'state/update.lock').open('a'))
            try:fcntl.flock(install_lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
            except BlockingIOError:raise ValueError('Another update is already running.') from None
            service_update = manifest.get('service_update')
            if service_update is not None:
                if not isinstance(service_update, dict) or not isinstance(service_update.get('base_build'), str):
                    raise ValueError('Invalid service update base.')
                active_path = root / 'state/active'
                active = active_path.read_text().strip() if active_path.is_file() else ''
                if active not in (service_update['base_build'], build):
                    raise ValueError('The installed release changed. Recreate the sharing update from its current package.')
            if (root/'state/update-pending.json').exists():raise ValueError('Finish or discard the prepared update before using the SD installer.')
            stage = root/'inbox'/('stage-'+build)
            if stage.exists(): shutil.rmtree(stage)
            stage.mkdir(parents=True)
            for e in entries:
                destination = stage/e.filename
                destination.parent.mkdir(parents=True,exist_ok=True)
                destination.write_bytes(archive.read(e))
                destination.chmod(0o755 if (e.external_attr >> 16)&0o111 else 0o644)
        app = stage/'rgds/releases'/build
        sys.path.insert(0,str(app/'runtime'))
        from activate import activate, stop_session, verify_release
        verify_release(app)
        stop_session(root/'userdata')
        destination = root/'releases'/build
        destination.parent.mkdir(parents=True,exist_ok=True)
        if destination.exists():
            verify_release(destination)
        else:
            app.rename(destination)
        # Only private development deployment updates these tiny bootstrap files.
        for name in ('main.sh','update_guard.py'):
            if not (stage/'rgds'/name).exists():continue
            if (root/name).exists():
                (root/'state').mkdir(exist_ok=True)
                shutil.copy2(root/name,root/'state'/(name+'.previous'))
            shutil.copy2(stage/'rgds'/name,root/name)
        apps = root.parent/'Roms/APPS'
        install_apps(stage/'Roms/APPS', apps)
        trust=stage/'rgds/state/update-trust.json'
        if trust.is_file() and (not (root/'state/update-trust.json').exists() or unconfigured_trust(root/'state/update-trust.json')):
            # A pinned channel is never replaced by an installer; a blank one
            # (no feed or no keys, as private previews shipped) may be.
            shutil.copy2(trust,root/'state/update-trust.json')
        first_install=not (root/"state/active").exists()
        activate(root,build,stop=False)
        if first_install:
            (root/"userdata").mkdir(exist_ok=True)
            (root/"userdata/setup-required").write_text("1\n")
        # Upgrade only our owned direct-startup hook, retaining the user's choices.
        sys.path.insert(0,str(destination/'runtime'))
        from boot_control import CONTROL, SUPERVISOR, read_json, configure
        boot=read_json(CONTROL/'config.json')
        if SUPERVISOR.is_file():
            mode=boot.get('mode','rgds') if boot.get('root')==str(root) else 'rgds'
            configure(root,mode,warning=boot.get('warning',True))
            # One RG DS identity from power-on: the early splash and the boot-image
            # logo. Both keep originals and are reversible by the uninstaller; a
            # failure leaves stock artwork and never fails the installation.
            try:
                from boot_identity import apply as apply_identity
                identity=apply_identity(destination)
                print('Boot artwork: splash '+identity['splash']+', logo '+identity['logo']+'.')
                for note in identity['messages']:print('  '+note)
            except Exception as error:print('Boot artwork skipped: '+str(error))
        shutil.rmtree(stage)
        archive_path.unlink()
        print('Installed RG DS Core '+build+'. Open APPS > RG DS Core to play.')

if __name__ == "__main__": main()
