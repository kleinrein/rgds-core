#!/bin/sh
# Summary of dc_run_w.sh cases (sh dc_w_report.sh NAME...): mean game and shown frames a second over the drive
# (stats lines 7 onwards that are not the menu's 59.8+/59.8+), busiest threads, GPU totals.
cd /data/rgds/dc-spike
for n in "$@"; do
    C=case-w$n
    echo "== $n"
    grep rgds-stats $C/log | awk 'NR>6 && NR<=20 {g+=$6; s+=$8; k++} END {printf "drive lines %d: game %.1f shown %.1f\n", k, g/k, s/k}'
    grep rgds-stats $C/log | awk 'NR>6 && NR<=20 {printf "%s/%s ", $6+0, $8+0} END {print ""}'
    grep -E "Flycast-emu|rgds-play [0-9]+%" $C/threads | grep -v " 0%" | tr "\n" ";"; echo
    tail -1 $C/gpu
done
