#!/bin/bash
# sweep.sh LABEL "A_BIN|A_ENV" "B_BIN|B_ENV" GAME START... [-- extra headless args]
# N-frame (WIN, default 12) DS_HIRES_DUMP windows from each START, run A against run B by per-frame sha256
# (build/frag-cost/runs/LABEL-GAME-{a,b}-START/hashes.txt); prints identical/different counts per tag.
# GAME.nds comes from build/frag-cost/cap or build/zelda-cap; Zelda windows need --direct, the ink window --load-state.
set -u
cd /Users/andreasrein/dev/rgds/build/frag-cost
L=$1; A=$2; B=$3; G=$4; shift 4
starts=(); while [ $# -gt 0 ] && [ "$1" != -- ]; do starts+=("$1"); shift; done; [ $# -gt 0 ] && shift
AB=${A%%|*}; AE=${A#*|}; BB=${B%%|*}; BE=${B#*|}; W=${WIN:-12}
for s in "${starts[@]}"; do
  HLENV="$AE" ./hlrun.sh $AB $L-$G-a-$s $G.nds $((s+W+1)) $s:$W "$@" >/dev/null
  HLENV="$BE" ./hlrun.sh $BB $L-$G-b-$s $G.nds $((s+W+1)) $s:$W "$@" >/dev/null
  na=$(wc -l < runs/$L-$G-a-$s/hashes.txt); nb=$(wc -l < runs/$L-$G-b-$s/hashes.txt)
  echo "== $L $G window $s ($W frames): $na / $nb dumps; finished a: $(grep -c 'ran .* frames' runs/$L-$G-a-$s/log.txt) b: $(grep -c 'ran .* frames' runs/$L-$G-b-$s/log.txt); $(grep -h 'hires dbg' runs/$L-$G-b-$s/log.txt | tail -1 | grep -o 'gpu ok [0-9]*')"
  python3 - runs/$L-$G-a-$s/hashes.txt runs/$L-$G-b-$s/hashes.txt <<'PY'
import sys, collections
def load(p): return {l.split()[1]: l.split()[0] for l in open(p) if l.strip()}
a, b = load(sys.argv[1]), load(sys.argv[2]); st = collections.defaultdict(lambda: [0, 0, []])
for n in sorted(set(a) | set(b)):
    tag = n.split('-', 1)[1][:-4]
    if tag == 'meta': continue
    if n in a and n in b and a[n] == b[n]: st[tag][0] += 1
    else: st[tag][1] += 1; st[tag][2].append(n.split('-')[0])
for t, (s, d, f) in sorted(st.items()): print(f"  {t}: {s} identical, {d} different" + (f" (frames {' '.join(f[:6])}{'...' if len(f) > 6 else ''})" if d else ''))
PY
done
