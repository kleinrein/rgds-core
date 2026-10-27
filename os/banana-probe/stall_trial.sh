#!/bin/sh
# A hand-run game on RG DS OS for 40 s with the cadence log and a clock/thermal sample per second; the session held.
# Usage: stall_trial.sh LABEL [ENV=VALUE ...]
label=$1
shift
rm -f /run/rgds/cadence
timeout 70 python3 /run/rgds/run_game.py "/data/roms/nds/Mario Kart DS.nds" 60 \
    LD_PRELOAD=/run/rgds/librgdsbridge.so DS_CADENCE_LOG=/run/rgds/cadence DS_VERBOSE=1 "$@" > "/run/rgds/stall-$label.out" 2>&1 &
: > /run/rgds/clocks-$label
for i in $(seq 1 40); do
    sleep 1
    echo "$i $(cat /sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq) $(cat /sys/class/devfreq/fde60000.gpu/cur_freq) $(cat /sys/class/thermal/thermal_zone0/temp) $(cat /sys/class/thermal/thermal_zone1/temp) $(cut -d' ' -f1 /proc/loadavg)" >> /run/rgds/clocks-$label
done
echo "$(date +%s) quit" > /data/rgds/userdata/session/emulator.control
wait
echo "== $label: $*"
cp /run/rgds/cadence /run/rgds/cadence-$label
python3 /run/rgds/cadence_stats.py /run/rgds/cadence
awk '{c[$2]++; g[$3]++; if ($4 > t) t = $4} END {for (k in c) printf "cpu %s MHz x%d; ", k / 1000, c[k]; for (k in g) printf "gpu %s MHz x%d; ", k / 1e6, g[k]; printf "cpu temp max %.0f C\n", t / 1000}' /run/rgds/clocks-$label
grep -E "^pace|^adaptive|^gpu|not presented|^EXIT|^TIMEOUT" "/run/rgds/stall-$label.out" | head -12
