#!/bin/sh
# From the Mac: device.sh [HOST]. Copies the VM's build to /data/rgds/vkspike on the handheld and runs
# the measurement set there. Takes no panels and reads no game data: GL is EGL surfaceless, Vulkan
# offscreen. Refuses while a game runs or another session holds the device (/run/rgds/*-hold).
# A shaders list (SHADER_LIST=/path/on/device) adds the game's real programs to the build timings;
# it is only read.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
H=${1:-$(python3 "$ROOT/tools/device_addr.py")}
export LIMA_HOME=$ROOT/.cache/linux-builder/state
L=$ROOT/.cache/linux-builder/lima/bin/limactl
OUT=${OUT:-$ROOT/.cache/vkspike-results}
mkdir -p "$OUT"
dev() { ssh -o ConnectTimeout=5 -o StrictHostKeyChecking=no -o BatchMode=yes "root@$H" "$@"; }
busy=$(dev 'ls /run/rgds/*-hold 2>/dev/null; pidof ctremu; true')
if [ -n "$busy" ]; then
    echo "device busy: $busy"
    exit 1
fi
"$L" shell rgds-image -- sh -c 'cat ~/3ds-spike/vkspike/vkspike.tar' | dev 'mkdir -p /data/rgds/vkspike && tar -C /data/rgds/vkspike -xf -'
dev "ls -la /usr/lib/rgds-panvk/; cat /sys/class/thermal/thermal_zone0/temp; cat /sys/class/devfreq/*/cur_freq 2>/dev/null; cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq" | tee "$OUT/device-state.txt"
LIST=${SHADER_LIST:+--list $SHADER_LIST}
# cool down between runs: the chip throttles at 70-80 C
cool='for i in $(seq 1 60); do t=$(cat /sys/class/thermal/thermal_zone0/temp); [ $t -lt 58000 ] && break; sleep 2; done'
run() { # run NAME "ENV=VAL ..." ARGS...
    name=$1
    envs=$2
    shift 2
    echo "== $name: $envs $*"
    dev "$cool; cd /data/rgds/vkspike/out && env $envs ./run.sh $* 2>&1; cat /sys/class/thermal/thermal_zone0/temp" | tee "$OUT/$name.txt"
}
dev 'mkdir -p /data/rgds/vkspike/shots'
# cold shader builds (no Mesa disk cache), real list programs if given, picture comparison, dma-buf
run cold "MESA_SHADER_CACHE_DISABLE=true" --frames 60 --shots /data/rgds/vkspike/shots $LIST || true
run dmabuf "" --mode none --dmabuf || true
run full "" --frames 300 || true
run tiny "" --frames 300 --tiny || true
run full-noglthread "NO_GLTHREAD=1" --mode gl --frames 300 || true
run draws800 "" --frames 200 --draws 800 || true
for n in gl vk; do dev "cat /data/rgds/vkspike/shots/$n.ppm" > "$OUT/$n.ppm" || true; done
echo "results in $OUT"
