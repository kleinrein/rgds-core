#!/usr/bin/env python3
"""Validate and assemble the optional RG DS Vulkan engine, private driver and source."""
import gzip
import hashlib
import io
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
try:
    from . import build_rgds_experimental as fork
    from . import build_panvk as panvk
except ImportError:
    import build_rgds_experimental as fork
    import build_panvk as panvk
ROOT = fork.ROOT
LOADER_VERSION = '1.3.275.0-1build1'
LOADER_DEB_SHA256 = '9459488a53ead304d00587c874f9b31d56d59c9aeae5c46b6f78801a89d53e63'


def archive(output, entries):
    with output.open('wb') as raw, gzip.GzipFile(fileobj=raw, mode='wb', filename='', mtime=0, compresslevel=3) as zipped:
        with tarfile.open(fileobj=zipped, mode='w') as target:
            for name, path in entries:
                content = path.read_bytes(); item = tarfile.TarInfo(name)
                item.size = len(content); item.mode = path.stat().st_mode & 0o777
                target.addfile(item, io.BytesIO(content))


def validate_payload(directory):
    directory = Path(directory)
    record = json.loads((directory/'vulkan-build.json').read_text())
    if record.get('kind') != 'source-built-rgds-vulkan' or record.get('base_source_hash') != fork.SOURCE_HASH:
        raise ValueError('Stale RG DS Vulkan payload; rebuild and run tools/package_rgds_vulkan.py')
    if record.get('recipe_sha256') != fork.base.digest(Path(__file__)):
        raise ValueError('Vulkan packaging recipe changed')
    for name, checksum in record['inputs'].items():
        if fork.base.digest(ROOT/name) != checksum: raise ValueError('Vulkan input changed: '+name)
    actual = {p.relative_to(directory).as_posix():fork.base.digest(p) for p in directory.rglob('*') if p.is_file() and p.name != 'vulkan-build.json'}
    if actual != record['files']: raise ValueError('Vulkan payload hash mismatch')
    if any(p.is_symlink() for p in directory.rglob('*')): raise ValueError('Vulkan payload contains symlinks')
    return record


def main():
    source = ROOT/'build/rgds-vulkan-game/source'; built = ROOT/'build/rgds-vulkan-game'
    engine = json.loads((built/'build.json').read_text())
    files = {p.relative_to(source).as_posix():fork.base.digest(p) for p in source.rglob('*') if p.is_file()}
    if engine['base_source_hash'] != fork.SOURCE_HASH or engine['source_hash'] != fork.fingerprint(files):
        raise ValueError('Vulkan source no longer matches its compiled build')
    if engine['recipe_sha256'] != fork.base.digest(ROOT/'tools/build_rgds_vulkan.py') or engine['hires_patch_sha256'] != fork.base.digest(ROOT/'tools/engine_patches/rgds-vulkan-hires.patch'):
        raise ValueError('Vulkan build recipe changed')
    if engine['binary_sha256'] != fork.base.digest(built/'rgds-experimental-vulkan'): raise ValueError('Vulkan binary changed')
    driver = ROOT/'build/panvk-private'; drecord = json.loads((driver/'build.json').read_text())
    if drecord['commit'] != panvk.COMMIT or drecord['patch_sha256'] != panvk.sha(panvk.PATCH) or drecord['recipe_sha256'] != panvk.sha(Path(panvk.__file__)):
        raise ValueError('PanVK recipe changed')
    for name, checksum in drecord['files'].items():
        if fork.base.digest(driver/name) != checksum: raise ValueError('PanVK binary changed: '+name)
    mesa = ROOT/'.cache/panvk-kbase-source'
    if subprocess.check_output(['git','-C',str(mesa),'rev-parse','HEAD'],text=True).strip() != panvk.COMMIT:
        raise ValueError('PanVK source revision changed')
    changes = subprocess.check_output(['git','-C',str(mesa),'status','--porcelain','--untracked-files=all'],text=True).splitlines()
    if changes != [' M meson.build']: raise ValueError('Unexpected PanVK source modifications')
    loader = ROOT/'build/vulkan-loader'
    deb = loader/('libvulkan1_'+LOADER_VERSION+'_arm64.deb')
    if fork.base.digest(deb) != LOADER_DEB_SHA256: raise ValueError('Pinned Vulkan loader package differs')
    target = ROOT/'build/engines/rgds-vulkan'; target.parent.mkdir(exist_ok=True)
    pending = Path(tempfile.mkdtemp(prefix='.vulkan-',dir=target.parent))
    extras = ['tools/build_rgds_vulkan.py','tools/build_rgds_vulkan_shaders.py','tools/package_rgds_vulkan.py',
              'tools/build_rgds_experimental.py','tools/build_engines.py','tools/elf_abi.py','tools/build_panvk.py',
              'src/experimental/rgds/stylus.hpp','src/experimental/rgds/frontend_ui.hpp','src/experimental/rgds/sample_plan.hpp','src/experimental/rgds/README.md','src/experimental/rgds/hires_compositor_test.cpp','src/experimental/rgds/firmware_identity_test.cpp',
              'tools/engine_patches/rgds-experimental.patch','tools/engine_patches/rgds-sleep-ui.patch','tools/engine_patches/rgds-owned-ui.patch',
              'tools/engine_patches/rgds-vulkan-hires.patch','tools/engine_patches/rgds-vulkan-resolve.patch',
              fork.base.PATCH.relative_to(ROOT).as_posix(),'tools/engine_patches/panvk-kbase-linux.patch']
    try:
        shutil.copy2(built/'rgds-experimental-vulkan',pending/'rgds-experimental-vulkan')
        (pending/'lib').mkdir()
        for name in drecord['files']: shutil.copy2(driver/name,pending/'lib'/name)
        shutil.copy2(loader/'extracted/usr/lib/aarch64-linux-gnu/libvulkan.so.1',pending/'lib/libvulkan.so.1')
        shutil.copy2(loader/'extracted/usr/share/doc/libvulkan1/copyright',pending/'lib/libvulkan-copyright')
        # The Debian copyright file refers to this full text by system path.
        apache = mesa/'docs/license.rst'
        shutil.copy2(apache,pending/'lib/Mesa-license.rst')
        for path in (mesa/'licenses').iterdir():
            if path.is_file(): shutil.copy2(path,pending/'lib'/('Mesa-'+path.name))
        archive(pending/'vulkan-source.tar.gz', [('DSperate/'+name,source/name) for name in sorted(files)] + [('rgds/'+name,ROOT/name) for name in extras])
        names = subprocess.check_output(['git','-C',str(mesa),'ls-files','-z']).decode().split('\0')
        archive(pending/'panvk-source.tar.gz', [('PanVK/'+name,mesa/name) for name in names if name and (mesa/name).is_file()])
        inputs = {name:fork.base.digest(ROOT/name) for name in extras}
        (pending/'VULKAN-NOTICE.txt').write_text(
            'RG DS Experimental enhanced 3D: modified DSperate, GPL-3.0-or-later.\n'
            'Native software raster remains authoritative for guest capture and states.\n'
            'Enhanced graphics: private PanVK/kbase driver; no system driver replacement.\n'
            f'PanVK upstream {panvk.UPSTREAM} at {panvk.COMMIT}; Linux patch in vulkan-source.tar.gz.\n'
            f'Vulkan loader: Ubuntu arm64 libvulkan1 {LOADER_VERSION}, Apache-2.0 and MIT.\n'
            'Complete modified emulator source: vulkan-source.tar.gz; driver source and notices: panvk-source.tar.gz.\n'
            'Runtime library notices are in lib/. Native is the default. 2x-5x are experimental.\n')
        record = dict(schema=1,kind='source-built-rgds-vulkan',base_source_hash=fork.SOURCE_HASH,
                      recipe_sha256=fork.base.digest(Path(__file__)),inputs=inputs,engine=engine,driver=drecord,
                      loader=dict(package='libvulkan1',version=LOADER_VERSION,deb_sha256=LOADER_DEB_SHA256),
                      files={p.relative_to(pending).as_posix():fork.base.digest(p) for p in pending.rglob('*') if p.is_file()})
        (pending/'vulkan-build.json').write_text(json.dumps(record,indent=2,sort_keys=True)+'\n')
        validate_payload(pending)
        fork.base.publish_build(pending,target)
    finally:
        if pending.exists(): shutil.rmtree(pending)
    print(target)


if __name__ == '__main__': main()
