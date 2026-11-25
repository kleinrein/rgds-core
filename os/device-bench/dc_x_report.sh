#!/bin/sh
# One line per dc_run_x.sh case (sh dc_x_report.sh NAME...): mean game and shown frames a second over the drive's
# stats lines, GPU fragment and vertex-tiler ms/s over the drive, fragment ms per shown frame, busiest two threads.
cd /data/rgds/dc-spike
for n in "$@"; do
    C=case-x$n
    fps=$(grep rgds-stats $C/log | awk 'NR>6 && NR<=18 {g+=$6; s+=$8; k++} END {if (k) printf "%.1f %.1f", g/k, s/k}')
    gpu=$(tail -1 $C/gpu | sed 's/.*engine-fragment=\([0-9.]*\)ms.*engine-vertex-tiler=\([0-9.]*\)ms.*/\1 \2/')
    thr=$(grep -E "Flycast-emu|rgds-play [0-9]+%" $C/threads | head -2 | awk '{printf "%s %s  ", $2, $3}')
    echo "$n $fps $gpu" | awk -v t="$thr" '{printf "%-10s game %5.1f shown %5.1f  frag %6.1f ms/s  vt %5.1f  frag/frame %5.2f ms  %s\n", $1, $2, $3, $4, $5, $3 ? $4/$3 : 0, t}'
done
