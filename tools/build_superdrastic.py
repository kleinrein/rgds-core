#!/usr/bin/env python3
"""Build the pinned optional SuperDrastic backend locally; never deploy it.

Linux ARM64 needs gcc, g++, git, libdrm-dev and Python 3. On this Mac the existing
local Lima ARM64 builder runs this same script against the shared workspace.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import gzip
import hashlib
import io
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tarfile

try:
    from .elf_abi import inspect_elf
except ImportError:
    from elf_abi import inspect_elf

ROOT = Path(__file__).resolve().parents[1]
UPSTREAM = 'https://github.com/JorreFog/SuperDrastic'
COMMIT = '930c4c5bb3e904557cd8cc42a01a4e27834f5ba1'
TREE = '6d2d72fdc27ac2010ca73f3d604f1e6e9e5d7722'
INPUTS = '92d936303f349ba2a8f078c28e29d7a2f822b13894f503814b606c9b03075056'
SOURCE = ROOT / '.cache/superdrastic-source'
OUTPUT = ROOT / 'build/superdrastic'
PATCH = ROOT / 'tools/engine_patches/superdrastic/rgds-integration.patch'
ADAPTER = ROOT / 'src/experimental/superdrastic'
SDL_ARCHIVE = ROOT / '.cache/engine-sdk/SDL2-2.32.10.tar.gz'
SDL_SHA256 = '5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165'
REQUIRED = {'SDL_RenderPresent','SDL_PollEvent','SDL_LockTexture','SDL_OpenAudio',
            'rgds_sd_poll','rgds_sd_menu_toggle','pthread_create','pthread_cond_init'}


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def local_inputs():
    paths = [Path(__file__), PATCH, ROOT / 'src/platform/drastic_threads.hpp', ROOT / 'src/platform/overlay_frame.hpp',
             ROOT / 'src/platform/bridge_metrics.hpp',
             ROOT / 'tools/elf_abi.py', ROOT / 'tools/build_engines.py', *sorted(ADAPTER.glob('*'))]
    return {p.relative_to(ROOT).as_posix(): sha(p) for p in paths if p.is_file()}


def source_inputs(fetch=False):
    if not SOURCE.exists() and fetch:
        subprocess.run(['git','init',str(SOURCE)],check=True)
        subprocess.run(['git','-C',str(SOURCE),'remote','add','origin',UPSTREAM+'.git'],check=True)
        subprocess.run(['git','-C',str(SOURCE),'fetch','--depth','1','origin',COMMIT],check=True)
        subprocess.run(['git','-C',str(SOURCE),'checkout','--detach',COMMIT],check=True)
    def git(*args):
        return subprocess.check_output(['git','-C',str(SOURCE),*args],text=True).strip()
    if SOURCE.is_symlink() or git('rev-parse','HEAD') != COMMIT or git('rev-parse','HEAD^{tree}') != TREE:
        raise ValueError('SuperDrastic checkout differs from the source pin')
    if git('status','--porcelain','--untracked-files=all'):
        raise ValueError('SuperDrastic source must be an unchanged checkout')
    names = list(filter(None,git('ls-files','-z').split('\0')))
    if any((SOURCE/n).is_symlink() for n in names):
        raise ValueError('Unexpected source symlink')
    files = {n:sha(SOURCE/n) for n in sorted(names)}
    digest = hashlib.sha256(json.dumps(files,sort_keys=True,separators=(',',':')).encode()).hexdigest()
    if digest != INPUTS:raise ValueError('SuperDrastic source bytes differ from the pin')
    return files


def validate_build(output=OUTPUT):
    output = Path(output)
    record = json.loads((output/'build.json').read_text())
    if record.get('schema') != 1 or record.get('source') != dict(url=UPSTREAM,commit=COMMIT,tree=TREE,inputs=INPUTS):
        raise ValueError('Wrong SuperDrastic source/build record')
    if record.get('integration') != local_inputs():
        raise ValueError('SuperDrastic integration changed; rebuild it')
    files=record.get('files',{})
    if not {'libsuperdrastic.so','source.tar.gz','LICENSE','THIRD_PARTY.md','VERSION','sdl/libSDL2-2.0.so.0','sdl/build.json','sdl/source.tar.gz','sdl/LICENSE.txt'} <= files.keys():
        raise ValueError('Incomplete SuperDrastic output')
    for name,digest in files.items():
        p=Path(name)
        if p.is_absolute() or '..' in p.parts or (output/p).is_symlink() or sha(output/p)!=digest:
            raise ValueError('Changed or unsafe SuperDrastic payload: '+name)
    abi = inspect_elf(output/'libsuperdrastic.so','Pinned SuperDrastic with RGDS integration',symbols=True)
    if abi != record.get('abi') or abi['machine'] != 'aarch64' or not REQUIRED <= set(abi['exports']):
        raise ValueError('SuperDrastic ABI or integration exports differ')
    if not abi['glibc_minimum'] or tuple(map(int,abi['glibc_minimum'].split('.'))) > (2,38):
        raise ValueError('SuperDrastic exceeds its GLIBC 2.38 baseline')
    sdl=json.loads((output/'sdl/build.json').read_text())
    abi_sdl=inspect_elf(output/'sdl/libSDL2-2.0.so.0','Private SDL runtime for SuperDrastic')
    if sdl.get('source_sha256')!=SDL_SHA256 or sha(output/'sdl/source.tar.gz')!=SDL_SHA256 or sdl.get('abi')!=abi_sdl:
        raise ValueError('Private SDL runtime source/ABI record mismatch')
    if abi_sdl['machine']!='aarch64' or tuple(map(int,abi_sdl['glibc_minimum'].split('.')))>(2,38):raise ValueError('Unsupported private SDL ABI')
    with tarfile.open(output/'source.tar.gz') as archive:
        for name,digest in record['integration'].items():
            if hashlib.sha256(archive.extractfile('rgds/'+name).read()).hexdigest()!=digest:
                raise ValueError('Missing corresponding integration source: '+name)
        if archive.extractfile('superdrastic/LICENSE').read()!=(output/'LICENSE').read_bytes():
            raise ValueError('SuperDrastic license differs from corresponding source')
    return record


def build_sdl(stage, candidate):
    """Private runtime, distinct from DSperate's build-only SDL link SDK."""
    if sha(SDL_ARCHIVE) != SDL_SHA256:raise ValueError('Pinned SDL2 archive mismatch')
    source=stage/'sdl-source';source.mkdir()
    with tarfile.open(SDL_ARCHIVE) as archive:
        members=[]
        for member in archive:
            path=Path(member.name)
            if path.is_absolute() or '..' in path.parts:raise ValueError('Unsafe SDL source path')
            if member.isfile() or member.isdir():members.append(member)
        archive.extractall(source,members=members,filter='data')
    source=source/'SDL2-2.32.10'
    from build_engines import SDL_OPTIONS
    options=dict(SDL_OPTIONS,SDL_ALSA='ON',SDL_LIBUDEV='ON',SDL_JOYSTICK='ON',
                 SDL_ALSA_SHARED='ON',SDL_RENDER='ON',SDL_HAPTIC='ON',
                 CMAKE_C_FLAGS='-O2 -mtune=cortex-a55')
    directory=stage/'sdl-build'
    subprocess.run(['cmake','-S',str(source),'-B',str(directory),*[f'-D{k}={v}'for k,v in sorted(options.items())]],check=True,stdout=subprocess.DEVNULL)
    subprocess.run(['cmake','--build',str(directory),'--parallel','4'],check=True,stdout=subprocess.DEVNULL)
    configs=list(directory.rglob('SDL_config.h'))
    config=next((p.read_text() for p in configs if '#define SDL_VIDEO_DRIVER_DUMMY 1' in p.read_text()),'')
    for macro in ('SDL_VIDEO_DRIVER_DUMMY','SDL_JOYSTICK_LINUX','SDL_INPUT_LINUXEV','SDL_AUDIO_DRIVER_ALSA'):
        if '#define '+macro+' 1' not in config:raise ValueError('Private SDL runtime lacks '+macro)
    output=candidate/'sdl';output.mkdir()
    library=next(p for p in directory.rglob('libSDL2-2.0.so.*') if not p.is_symlink())
    shutil.copy2(library,output/'libSDL2-2.0.so.0')
    subprocess.run(['strip','--strip-unneeded',str(output/'libSDL2-2.0.so.0')],check=True)
    shutil.copy2(source/'LICENSE.txt',output/'LICENSE.txt')
    shutil.copy2(SDL_ARCHIVE,output/'source.tar.gz')
    record=dict(version='2.32.10',source='https://github.com/libsdl-org/SDL/releases/tag/release-2.32.10',
                source_sha256=SDL_SHA256,options=options,
                compiler=subprocess.check_output(['gcc','--version'],text=True).splitlines()[0],
                configuration=config,abi=inspect_elf(output/'libSDL2-2.0.so.0','Private SDL runtime for SuperDrastic'))
    (output/'build.json').write_text(json.dumps(record,indent=2,sort_keys=True)+'\n')


def build(fetch=False):
    original=source_inputs(fetch)
    try:
        validate_build();print('SuperDrastic source build is current.',flush=True);return
    except (OSError,ValueError,KeyError,TypeError):pass
    if platform.system() != 'Linux' or platform.machine() not in ('aarch64','arm64'):
        raise ValueError('Run in ARM64 Linux, or use the existing Mac Lima builder')
    inputs=local_inputs()
    stage=ROOT/'build/superdrastic-work'
    if stage.is_symlink():raise ValueError('Build staging must not be a symlink')
    if stage.exists():shutil.rmtree(stage)
    stage.mkdir(parents=True)
    source=stage/'source'
    for name in original:
        target=source/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(SOURCE/name,target)
    subprocess.run(['git','apply',str(PATCH)],cwd=source,check=True)
    # Compile the pinned generated assembly. Never mutate or execute upstream
    # build/install/update scripts in the clean source checkout.
    rc=source/'src/third_party/rcheevos'
    sources=[*sorted((source/'src').glob('*.c')),*sorted((source/'src/rast').glob('*.c')),
             *sorted((source/'src/rast/spec').glob('*.c')),source/'src/rast/rast_kern.S',
             *(rc/'src'/n for n in ('rc_client.c','rc_compat.c','rc_util.c','rc_version.c')),
             *sorted((rc/'src/rcheevos').glob('*.c')),*sorted((rc/'src/rapi').glob('*.c')),
             *sorted((rc/'src/rhash').glob('*.c')),ADAPTER/'rgds.c',ADAPTER/'threads.cpp',ADAPTER/'shared_menu.cpp']
    flags=['-O2','-fPIC','-mtune=cortex-a55','-DRC_DISABLE_LUA','-DRC_CLIENT_SUPPORTS_HASH',
           '-DDSFLIP_VERSION="'+(SOURCE/'VERSION').read_text().strip()+'-rgds"',
           '-I/usr/include/libdrm','-I'+str(rc/'include'),'-I'+str(rc/'src'),
           '-I'+str(source/'src/third_party/stb'),'-I'+str(ADAPTER),'-I'+str(ROOT/'src'),
           '-ffile-prefix-map='+str(ROOT)+'=/rgds']
    def compile_one(item):
        i,path=item;obj=stage/(str(i)+'.o');compiler='g++' if path.suffix=='.cpp' else 'gcc'
        command=[compiler,*flags,*(['-std=c++17'] if compiler=='g++' else []),'-c',str(path),'-o',str(obj)]
        result=subprocess.run(command,capture_output=True,text=True)
        if result.returncode:raise ValueError(str(path)+'\n'+result.stderr[-6000:])
        return obj
    print('Compiling pinned SuperDrastic and RGDS integration ('+str(len(sources))+' units).',flush=True)
    with ThreadPoolExecutor(max_workers=4) as pool:objects=list(pool.map(compile_one,enumerate(sources)))
    candidate=stage/'output';candidate.mkdir()
    subprocess.run(['g++','-shared','-Wl,-z,defs','-Wl,--exclude-libs,ALL',*[str(p) for p in objects],
                    '-o',str(candidate/'libsuperdrastic.so'),'-ldrm','-lpthread','-ldl','-lm',
                    '-static-libstdc++','-static-libgcc'],check=True)
    subprocess.run(['strip','--strip-unneeded',str(candidate/'libsuperdrastic.so')],check=True)
    for name in ('LICENSE','THIRD_PARTY.md','VERSION'):
        shutil.copy2(SOURCE/name,candidate/name)
    shutil.copytree(SOURCE/'shaders',candidate/'shaders')
    build_sdl(stage,candidate)
    with (candidate/'source.tar.gz').open('wb') as raw,gzip.GzipFile(fileobj=raw,mode='wb',mtime=0,filename='') as gz,tarfile.open(fileobj=gz,mode='w') as archive:
        for prefix,base,names in [('superdrastic',SOURCE,original),('rgds',ROOT,inputs)]:
            for name in sorted(names):
                data=(base/name).read_bytes();info=tarfile.TarInfo(prefix+'/'+name)
                info.size=len(data);info.mode=0o644;archive.addfile(info,io.BytesIO(data))
    record=dict(schema=1,source=dict(url=UPSTREAM,commit=COMMIT,tree=TREE,inputs=INPUTS),integration=inputs,
                compiler=subprocess.check_output(['gcc','--version'],text=True).splitlines()[0],flags=flags,
                abi=inspect_elf(candidate/'libsuperdrastic.so','Pinned SuperDrastic with RGDS integration',symbols=True),
                files={p.relative_to(candidate).as_posix():sha(p) for p in sorted(candidate.rglob('*')) if p.is_file()})
    (candidate/'build.json').write_text(json.dumps(record,indent=2,sort_keys=True)+'\n')
    validate_build(candidate)
    if OUTPUT.is_symlink():raise ValueError('Build output must not be a symlink')
    if OUTPUT.exists():shutil.rmtree(OUTPUT)
    candidate.rename(OUTPUT)
    print('SuperDrastic ARM64 build verified: '+str(OUTPUT),flush=True)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fetch',action='store_true');parser.add_argument('--check',action='store_true')
    args=parser.parse_args()
    if args.check:validate_build();return
    if platform.system()=='Darwin':
        # The pre-existing VM is a local toolchain, never the target handheld.
        source_inputs(args.fetch)
        try:validate_build();print('SuperDrastic source build is current.');return
        except (OSError,ValueError,KeyError,TypeError):pass
        lima=ROOT/'.cache/linux-builder/lima/bin/limactl'
        env=dict(os.environ,LIMA_HOME=str(ROOT/'.cache/linux-builder/state'))
        if not lima.is_file():raise ValueError('Configure an ARM64 Linux builder; see docs/SUPERDRASTIC.md')
        subprocess.run([str(lima),'start','rgds-image'],env=env,check=True)
        subprocess.run([str(lima),'shell','rgds-image','python3','-B',str(Path(__file__).resolve())],env=env,check=True)
        validate_build()
    else:build(args.fetch)


if __name__=='__main__':
    try:main()
    except (OSError,ValueError,subprocess.SubprocessError) as error:
        print('SuperDrastic build: '+str(error),file=sys.stderr);sys.exit(1)
