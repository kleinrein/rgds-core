#!/bin/sh
# Waits (up to an hour) for a Dreamcast game in RG DS Play started from the launcher, then watches it for 90 s with
# dc_watch.py and Panfrost's profiling on. Takes nothing from the game: reads /proc and sysfs only.
cd /data/rgds/dc-spike
P=/sys/bus/platform/drivers/panfrost/fde60000.gpu/profiling
for i in $(seq 1 720); do
    for p in $(pidof rgds-play); do
        if tr "\0" " " < /proc/$p/cmdline | grep -q -- "--system dc"; then
            echo "game $p: $(tr '\0' ' ' < /proc/$p/cmdline | cut -c1-300)"
            tr "\0" "\n" < /proc/$p/environ | grep -E "^RGDS|^LD_PRELOAD|^MESA|^PAN|^SDL" | sort
            sleep 20
            echo 1 > $P
            python3 dc_watch.py 90
            echo 0 > $P
            echo WATCH DONE
            exit 0
        fi
    done
    sleep 5
done
echo WATCH TIMEOUT
