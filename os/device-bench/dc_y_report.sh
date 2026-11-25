#!/bin/sh
# One line per dc_run_y.sh case (sh dc_y_report.sh NAME...), over the drive's windows of 300 passes (the 10th to the
# 18th profile line): the core's run time per pass, passes over a refresh, the loop period (what the panel gets),
# then the two busiest threads at 100 s.
cd /data/rgds/dc-spike
for n in "$@"; do
    C=case-y$n
    thr=$(grep -E "Flycast-emu|rgds-play [0-9]+%" $C/threads | head -2 | awk '{printf "%s %s  ", $2, $3}')
    grep "play: profile" $C/log | sed 's/[(,]//g' | awk -v n="$n" -v t="$thr" 'NR>=10 && NR<=18 {r+=$4; o+=$8; f+=$17; if ($4>m) m=$4; k++}
        END {if (k) printf "%-10s run %5.2f ms (worst window %5.2f)  over 16.7: %4.1f of 300  loop %5.2f ms  %s\n", n, r/k, m, o/k, f/k, t}'
done
