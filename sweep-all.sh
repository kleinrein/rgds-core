#!/bin/bash
# sweep-all.sh [LABELS...] : every dump comparison for the two candidates, on the host under lavapipe at 3x.
cd /Users/andreasrein/dev/rgds/build/frag-cost
MK="mk 60 -- --direct --load-state /Users/andreasrein/dev/rgds/build/frag-cost/cap/mk-ink.dss"
PH="ph 250 800 1500 2200 -- --direct"; ST="st 250 400 900 1500 2200 -- --direct"
export PRESENT=3
want="${*:-base halfpx nd1 nd2}"
for v in "base:hl-old|:hl-new|" "halfpx:hl-new|:hl-new|DS_LEAN_HALFPX=1" "nd1:hl-new|:hl-new|DS_LEAN_NODISCARD=1" "nd2:hl-new|:hl-new|DS_LEAN_NODISCARD=2"; do
  IFS=: read -r L A B <<<"$v"; case " $want " in *" $L "*) ;; *) continue;; esac
  echo "### $L: $A vs $B"
  ./sweep.sh $L "$A" "$B" $MK; ./sweep.sh $L "$A" "$B" $PH; ./sweep.sh $L "$A" "$B" $ST
done
echo "### done"
