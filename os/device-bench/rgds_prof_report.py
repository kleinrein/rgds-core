#!/usr/bin/env python3
"""Functions and libraries by share of CPU samples, per thread, from rgds_prof.so's output.

    rgds_prof_report.py PREFIX [--from S] [--to S] [--lib NAME=UNSTRIPPED.so ...] [--top N]

PREFIX.samples / .maps / .threads come from the handheld. A library is named from the unstripped copy given with
--lib (its file name as the handheld maps it = the local path), otherwise from the mapped file itself if it is here,
otherwise by its offset. Symbols come from `nm` (any ELF-aware nm: Apple's is llvm-nm), so .bss blocks such as a JIT's
code buffer get a name too.
"""
import argparse
import bisect
import collections
import subprocess
from pathlib import Path


def read_maps(path):
    maps = []
    for line in Path(path).read_text().splitlines():
        parts = line.split(None, 5)
        if len(parts) < 5: continue
        start, end = (int(x, 16) for x in parts[0].split('-'))
        maps.append((start, end, int(parts[2], 16), parts[5] if len(parts) > 5 else '[anon]'))
    maps.sort()
    return maps


def symbols(path):
    try:
        out = subprocess.run(['nm', '-C', '--defined-only', str(path)], capture_output=True, text=True).stdout
    except OSError:
        return [], []
    found = []
    for line in out.splitlines():
        parts = line.split(' ', 2)
        if len(parts) == 3 and parts[1] in 'tTwWbBdD':
            found.append((int(parts[0], 16), parts[2]))
    found.sort()
    return [a for a, _ in found], [n for _, n in found]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('prefix')
    parser.add_argument('--from', dest='start', type=float, default=0)
    parser.add_argument('--to', type=float, default=1e9)
    parser.add_argument('--lib', action='append', default=[])
    parser.add_argument('--top', type=int, default=25)
    parser.add_argument(
        '--maps',
        help='a /proc/PID/maps copied while the libraries were loaded (default PREFIX.maps, '
        'written at exit: a core already unloaded is missing from it)'
    )
    args = parser.parse_args()
    maps = read_maps(args.maps or args.prefix + '.maps')
    starts = [m[0] for m in maps]
    threads = dict(
        line.split(' ', 1) for line in Path(args.prefix + '.threads').read_text().splitlines() if ' ' in line
    )
    local = dict(item.split('=', 1) for item in args.lib)
    tables = {}

    def where(pc):
        i = bisect.bisect_right(starts, pc) - 1
        if i < 0 or pc >= maps[i][1]: return '[unmapped]', '?'
        start, _, offset, name = maps[i]
        short = name.rsplit('/', 1)[-1]
        if name.startswith('/'):
            if short not in tables:
                source = local.get(short) or (name if Path(name).is_file() else None)
                tables[short] = symbols(source) if source else ([], [])
            addresses, names = tables[short]
            vaddr = pc - start + offset
            j = bisect.bisect_right(addresses, vaddr) - 1
            if j >= 0: return short, names[j]
            return short, f'+{vaddr:#x}'
        return name, '?'

    per_thread = collections.defaultdict(collections.Counter)
    per_lib = collections.defaultdict(collections.Counter)
    totals = collections.Counter()
    for line in Path(args.prefix + '.samples').read_text().splitlines():
        ms, tid, pc, lr = line.split()
        if not args.start <= int(ms) / 1000 < args.to: continue
        name = threads.get(tid, tid)
        lib, function = where(int(pc, 16))
        per_thread[name][f'{lib}: {function}'] += 1
        per_lib[name][lib] += 1
        totals[name] += 1
    every = sum(totals.values())
    print(f'{every} samples ({every / 1000:.1f} s of CPU at 1 kHz) from {args.start:g} s to {min(args.to, 1e8):g} s')
    for name, count in totals.most_common():
        print(f'\n== {name}: {count} samples ({100 * count / every:.0f}%)')
        print('  by library:', ', '.join(f'{lib} {100 * n / count:.0f}%' for lib, n in per_lib[name].most_common(8)))
        for function, n in per_thread[name].most_common(args.top):
            print(f'  {100 * n / count:5.1f}%  {function}')


main()
