#!/usr/bin/env python3
"""Import owner-supplied engines/data into a verified independent vendor bundle.

OS launchers, scripts, control hooks, BIOS and ROMs are excluded. Supplied
engine/library/shader notices remain beside their payloads.
"""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import tempfile

ROOT=Path(__file__).resolve().parents[1]
DEFAULT=ROOT/'vendor/engines'
KIND='owner-provided-engine-bundle'
REQUIRED={'dsperate/dsperate','nnddss/nnddss'}
NNDS_LIBRARIES={'libGLESv2_shim.so','libOpenSLES_shim.so','libandroid.so',
                'libbionic_shim.so','libnnddss.so','libnnddss_renegade_kid.so',
                'libstdc++_stub.so','libz_shim.so'}
DIRECTORIES={'dsperate','nnddss','nnddss/lib','nnddss/ui','nnddss/fonts',
             'nnddss/shaders','nnddss/licenses','nnddss/notices','shaders'}


def digest(path):
    result=hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda:stream.read(1024*1024),b''):result.update(block)
    return result.hexdigest()


def notice(name):
    name=name.lower()
    return name.startswith(('readme','license','notice','copying','copyright','authors')) and Path(name).suffix not in ('.py','.pyc','.sh','.bash','.bin','.exe','.nds','.gba','.rom','.so','.dll') and '.so.' not in name


def safe_name(name):
    if not isinstance(name,str) or not name or '\\' in name or '\0' in name:
        raise ValueError('Unsafe engine bundle path: '+repr(name))
    relative=PurePosixPath(name)
    if relative.is_absolute() or '..' in relative.parts or relative.as_posix()!=name:
        raise ValueError('Unsafe engine bundle path: '+repr(name))
    return relative


def supported(name):
    relative=safe_name(name);parts=relative.parts
    if name in REQUIRED:return True
    parent=relative.parent.as_posix();filename=relative.name;suffix=relative.suffix.lower()
    if parent not in DIRECTORIES:return False
    if notice(filename):return True
    if parent in ('nnddss/licenses','nnddss/notices'):
        return suffix in ('','.txt','.md','.rst')
    if parent=='nnddss':return filename=='gamecontrollerdb.txt'
    if parent=='nnddss/lib':return filename in NNDS_LIBRARIES
    if parent=='nnddss/ui':return suffix in ('.png','.bmp','.jpg','.jpeg','.webp','.ttf','.otf')
    if parent=='nnddss/fonts':return suffix in ('.ttf','.otf')
    if parent=='nnddss/shaders':return suffix in ('.dsd','.dfx')
    if parent=='shaders':return suffix=='.dsd'
    return False


def tree(root):
    """Walk without following links; reject links and special filesystem nodes."""
    root=Path(root)
    if root.is_symlink():raise ValueError('Engine payload symlink: '+str(root))
    if not root.is_dir():raise ValueError('Engine payload directory missing: '+str(root))
    for path in sorted(root.rglob('*')):
        mode=path.lstat().st_mode
        if stat.S_ISLNK(mode):raise ValueError('Engine payload symlink: '+str(path))
        if not stat.S_ISREG(mode) and not stat.S_ISDIR(mode):raise ValueError('Unsupported engine payload file type: '+str(path))
        yield path,stat.S_ISDIR(mode)


def executable(name):
    relative=PurePosixPath(name)
    return name in REQUIRED or (relative.parent.as_posix()=='nnddss/lib' and relative.name in NNDS_LIBRARIES)


def payload_file(path,name):
    if executable(name):
        with Path(path).open('rb') as stream:
            if stream.read(4)!=b'\x7fELF':raise ValueError('Engine/library payload is not ELF: '+name)
    else:
        with Path(path).open('rb') as stream:
            if stream.read(2)==b'#!':raise ValueError('Imported scripts are unsupported: '+name)


def validate_bundle(directory):
    """Return a manifest only after exact inventory, paths and bytes agree."""
    directory=Path(directory)
    if directory.is_symlink():raise ValueError('Engine bundle root is a symlink: '+str(directory))
    try:
        entries=list(tree(directory))
        manifest=json.loads((directory/'payloads.json').read_text())
    except (OSError,ValueError) as error:
        raise ValueError('Engine bundle validation failed: '+str(error)) from error
    if not isinstance(manifest,dict) or type(manifest.get('schema')) is not int or manifest['schema']!=1 or manifest.get('kind')!=KIND:
        raise ValueError('Wrong engine bundle manifest')
    files=manifest.get('files')
    if not isinstance(files,dict) or not REQUIRED.issubset(files):raise ValueError('Engine bundle manifest is incomplete')
    for name,expected in files.items():
        if not supported(name):raise ValueError('Unsupported engine bundle path: '+repr(name))
        if not isinstance(expected,str) or not re.fullmatch('[0-9a-f]{64}',expected):raise ValueError('Invalid engine bundle hash: '+repr(name))
    actual=set()
    for path,is_directory in entries:
        name=path.relative_to(directory).as_posix()
        if is_directory:
            if name not in DIRECTORIES:raise ValueError('Unsupported engine bundle directory: '+name)
        elif name!='payloads.json':
            if not supported(name):raise ValueError('Unsupported engine bundle path: '+name)
            actual.add(name)
    if actual!=set(files):raise ValueError('Engine bundle file inventory does not match its manifest')
    for name,expected in files.items():
        path=directory/Path(name);payload_file(path,name)
        if digest(path)!=expected:raise ValueError('Engine bundle hash mismatch: '+name)
    return manifest


def plan(source):
    selected=[]
    def select(original,name):
        if not supported(name):return
        payload_file(original,name);selected.append((original,name,digest(original)))
    binary=source/'bin/dsperate'
    if (source/'bin').is_symlink() or binary.is_symlink():raise ValueError('Engine payload symlink: '+str(binary))
    if not binary.is_file() or not (source/'nnddss_payload/nnddss').is_file():
        raise ValueError('Select the supplied system payload directory containing bin/dsperate and nnddss_payload/nnddss.')
    select(binary,'dsperate/dsperate')
    for folder,prefix in (('dsperate','dsperate'),('nnddss_payload','nnddss'),('shaders','shaders')):
        base=source/folder
        if not base.exists() and not base.is_symlink():continue
        for original,is_directory in tree(base):
            if not is_directory:select(original,prefix+'/'+original.relative_to(base).as_posix())
    files={name:expected for _,name,expected in selected}
    if not REQUIRED.issubset(files):raise ValueError('Source engine bundle is incomplete')
    return selected,dict(schema=1,kind=KIND,files=files,
        components={'dsperate':'External upstream engine binary with supplied notices',
                    'nnddss':'External engine/loader/shim binary payload; source/build records not supplied',
                    'shaders':'Supplied community preset data; keep original notices'},
        excluded=['OS launch scripts','compatibility hooks','shell/Python helpers','BIOS','ROMs'])


def provision(source,destination=DEFAULT):
    source=Path(source);destination=Path(destination)
    if source.is_symlink() or destination.is_symlink():raise ValueError('Engine bundle roots must not be symlinks')
    source=source.resolve();destination=destination.resolve()
    selected,manifest=plan(source)
    if destination.exists():
        existing=validate_bundle(destination)
        if existing==manifest:return destination
        raise ValueError('An engine bundle already exists with different bytes or metadata. Choose a new destination; the existing bundle was left untouched.')
    destination.parent.mkdir(parents=True,exist_ok=True)
    temporary=Path(tempfile.mkdtemp(prefix='.engine-import-',dir=destination.parent))
    try:
        for original,name,expected in selected:
            target=temporary/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(original,target)
            if digest(target)!=expected:raise ValueError('Source payload changed during import: '+name)
        (temporary/'payloads.json').write_text(json.dumps(manifest,indent=2,sort_keys=True)+'\n')
        validate_bundle(temporary)
        if destination.exists() or destination.is_symlink():raise ValueError('Engine bundle destination appeared during import; it was left untouched')
        temporary.rename(destination)
        return destination
    finally:
        if temporary.exists():shutil.rmtree(temporary)


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--source',type=Path,required=True)
    parser.add_argument('--destination',type=Path,default=DEFAULT);args=parser.parse_args()
    print(provision(args.source,args.destination))


if __name__=='__main__':main()
