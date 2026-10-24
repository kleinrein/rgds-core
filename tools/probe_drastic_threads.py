#!/usr/bin/env python3
"""Linux-only, original synthetic executable exercising the shipped thread hook.

No DraStic or game code is run. Fake ELF build IDs and two explicitly placed
functions exercise the exact preload gate, mismatch bypass and repeated starts.
Run inside the local Linux builder; never on the handheld during play.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
BUILD_ID = '7a5e0e5fc6e52e6e8f5499c3d4d667ef51db0748'
SOURCE = r'''
#ifdef NDEBUG
#error This fixture requires active assertions
#endif
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <unistd.h>
struct Result { std::atomic<int> stage{0},observed{-1}; };
void* sample(void* value){auto& r=*static_cast<Result*>(value);r.observed.store(r.stage.load());return value;}
__attribute__((section(".probe_lines"),aligned(4),noinline)) void* lines(void* p){return sample(p);}
__attribute__((section(".probe_bins"),aligned(4),noinline)) void* bins(void* p){return sample(p);}
void cycle(bool guarded,int index){
 Result result;pthread_t child;pthread_mutex_t a,b;pthread_cond_t c,d;
 assert(pthread_create(&child,nullptr,index%2?lines:bins,&result)==0);
 usleep(3000);
 if(guarded)assert(result.observed.load()==-1);
 else {for(int n=0;n<1000 && result.observed.load()<0;n++)usleep(100);assert(result.observed.load()==0);}
 assert(pthread_mutex_init(&a,nullptr)==0);assert(pthread_mutex_init(&b,nullptr)==0);
 result.stage.store(1);assert(pthread_cond_init(&c,nullptr)==0);usleep(1000);
 if(guarded)assert(result.observed.load()==-1);
 result.stage.store(2);assert(pthread_cond_init(&d,nullptr)==0);
 void* returned=nullptr;assert(pthread_join(child,&returned)==0 && returned==&result);
 assert(result.observed.load()==(guarded?2:0));
 pthread_cond_destroy(&c);pthread_cond_destroy(&d);pthread_mutex_destroy(&a);pthread_mutex_destroy(&b);
}
void* creator(void*){for(int i=0;i<16;i++)cycle(true,i);return nullptr;}
int main(int argc,char** argv){
 assert(argc==2);bool guarded=!std::strcmp(argv[1],"guarded");
 if(!std::strcmp(argv[1],"timeout")){
  Result r;pthread_t child;assert(pthread_create(&child,nullptr,lines,&r)==0);pthread_join(child,nullptr);assert(r.observed.load()==0);puts("timeout fallback returned");return 0;
 }
 for(int i=0;i<96;i++)cycle(guarded,i);
 // An unrelated start routine keeps its arguments, attributes and return.
 Result other;pthread_attr_t attr;pthread_attr_init(&attr);pthread_attr_setstacksize(&attr,1024*1024);
 pthread_t plain;assert(pthread_create(&plain,&attr,sample,&other)==0);void* returned=nullptr;
 assert(pthread_join(plain,&returned)==0 && returned==&other && other.observed.load()==0);pthread_attr_destroy(&attr);
 if(guarded){pthread_t parents[4];for(auto& p:parents)assert(pthread_create(&p,nullptr,creator,nullptr)==0);for(auto p:parents)pthread_join(p,nullptr);}
 puts(guarded?"160 gated helper starts passed; unrelated thread unchanged":"96 identity/engine/opt-out bypass starts passed");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--bridge', type=Path, default=ROOT / 'build/device/librgdsbridge.so')
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument('--build-only', action='store_true', help='Cross-compile fixtures on the Mac with Zig')
    modes.add_argument('--run-only', action='store_true', help='Run previously compiled fixtures in Linux')
    args = parser.parse_args()
    if not args.build_only and platform.system() != 'Linux': parser.error('Run only in the local Linux builder')
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=True)
    source = root / 'original-fixture.cpp'
    if not args.run_only:
        source.write_text(SOURCE)
        compiler = ['g++'] if platform.system() == 'Linux' else ['zig', 'c++', '-target', 'aarch64-linux-gnu.2.17']
        layout = root / 'layout.ld'
        layout.write_text(
            'SECTIONS { .probe_lines 0x2fa50 : { *(.probe_lines) } .probe_bins 0x58e50 : { *(.probe_bins) } } INSERT BEFORE .text;\n'
        )
        flags = [
            *compiler, '-std=c++17', '-O2', '-UNDEBUG', '-pthread', '-fPIE', '-pie',
            str(source), '-Wl,-T,' + str(layout)
        ]
        for name, identity in [('recognized', BUILD_ID), ('unknown', '00' * 20)]:
            subprocess.run(
                [*flags, '-Wl,--build-id=0x' + identity, '-o',
                 str(root / name)],
                check=True,
                env=dict(
                    os.environ,
                    ZIG_GLOBAL_CACHE_DIR=str(ROOT / '.cache/zig'),
                    ZIG_LOCAL_CACHE_DIR=str(ROOT / '.cache/zig-local')
                )
            )
        record = dict(
            source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
            binaries={
                name: hashlib.sha256((root / name).read_bytes()).hexdigest()
                for name in ('recognized', 'unknown')
            }
        )
        (root / 'fixtures.json').write_text(json.dumps(record, indent=2) + '\n')
    elif source.read_text() != SOURCE:
        raise ValueError('Fixture source changed; rebuild before probing')
    if args.build_only: return
    record = json.loads((root / 'fixtures.json').read_text())
    if record != dict(
        source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
        binaries={name: hashlib.sha256((root / name).read_bytes()).hexdigest()
                  for name in ('recognized', 'unknown')}
    ):
        raise ValueError('Fixture binaries changed; rebuild before probing')
    tests = [
        ('recognized', 'drastic', '1', 'guarded'), ('unknown', 'drastic', '1', 'bypass'),
        ('recognized', 'dsperate', '1', 'bypass'),
        ('recognized', 'melonds', '1', 'bypass'), ('recognized', 'drastic', '0', 'bypass'),
        ('recognized', 'drastic', '1', 'timeout')
    ]
    results = []
    for name, core, enabled, mode in tests:
        began = time.monotonic()
        result = subprocess.run(
            [str(root / name), mode],
            env=dict(
                os.environ, LD_PRELOAD=str(args.bridge.resolve()), RGDS_CORE=core, RGDS_DRASTIC_HELPER_GATE=enabled
            ),
            capture_output=True,
            text=True,
            timeout=12
        )
        elapsed = time.monotonic() - began
        entry = dict(
            executable=name,
            core=core,
            enabled=enabled,
            mode=mode,
            seconds=elapsed,
            exit_code=result.returncode,
            stdout=result.stdout,
            stderr=result.stderr
        )
        results.append(entry)
        if result.returncode:
            (root / 'failure.json').write_text(json.dumps(results, indent=2) + '\n')
            raise RuntimeError(str(entry))
        if mode == 'timeout' and (not 1.8 <= elapsed < 8 or 'initialization timed out' not in result.stderr):
            raise RuntimeError('Expected bounded timeout diagnostic: ' + str(entry))
    report = dict(
        scope='Synthetic Linux pthread/preload integration; no emulator or handheld execution',
        bridge_sha256=hashlib.sha256(args.bridge.read_bytes()).hexdigest(),
        fixtures=record,
        cases=results
    )
    (root / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__': main()
