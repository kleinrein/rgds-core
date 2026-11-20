#!/bin/sh
# From the Mac: device_direct.sh [HOST]. Runs the spike on the handheld against the patched PanVK
# (tools/engine_patches/panvk-direct-indexed.patch, built in the VM at ~/3ds-spike/panvk-direct) next to the stock one.
# The patched driver goes to /data/rgds/vkspike/direct only; nothing installed changes.
# SLABS="64 1024" runs only the PANVK_VARYING_SLAB_KB comparison. Same rules as device.sh: no panels, no game data, refuses on any /run/rgds/*-hold or a running ctremu.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
H=${1:-$(python3 "$ROOT/tools/device_addr.py")}
export LIMA_HOME=$ROOT/.cache/linux-builder/state
L=$ROOT/.cache/linux-builder/lima/bin/limactl
OUT=${OUT:-$ROOT/.cache/vkspike-results/direct}
mkdir -p "$OUT"
dev() { ssh -o ConnectTimeout=5 -o StrictHostKeyChecking=no -o BatchMode=yes "root@$H" "$@"; }
busy=$(dev 'ls /run/rgds/*-hold 2>/dev/null; pidof ctremu; true')
if [ -n "$busy" ]; then
    echo "device busy: $busy"
    exit 1
fi
dev 'echo tanuki-vulkan-spike > /run/rgds/vkspike-hold'
trap "dev 'echo 0 > /sys/devices/platform/fde60000.gpu/profiling; rm -f /run/rgds/vkspike-hold'" EXIT
dev 'mkdir -p /data/rgds/vkspike/direct /data/rgds/vkspike/shots-direct'
"$L" shell rgds-image -- sh -c 'cat ~/3ds-spike/panvk-direct/build/src/panfrost/vulkan/libvulkan_panfrost.so' |
    dev 'cat > /data/rgds/vkspike/direct/libvulkan_panfrost.so'
dev 'echo 1 > /sys/devices/platform/fde60000.gpu/profiling'
n0=$(dev 'dmesg | wc -l')
D="ICD=/data/rgds/vkspike/direct/libvulkan_panfrost.so PANVK_DIRECT_INDEXED=1"
cool='for i in $(seq 1 60); do t=$(cat /sys/class/thermal/thermal_zone0/temp); [ $t -lt 58000 ] && break; sleep 2; done'
run() { # run NAME "ENV=VAL ..." ARGS...
    name=$1
    envs=$2
    shift 2
    echo "== $name: $envs $*"
    dev "$cool; cd /data/rgds/vkspike/out && env $envs ./run.sh $* 2>&1; cat /sys/class/thermal/thermal_zone0/temp" | tee "$OUT/$name.txt"
}
if [ -n "$SLABS" ]; then # only the varying-slab comparison: SLABS="64 1024 4096"
    for kb in $SLABS; do run slab-$kb "$D PANVK_VARYING_SLAB_KB=$kb" --mode vk --frames 300 || true; done
    exit 0
fi
run direct-shots "$D" --frames 60 --shots /data/rgds/vkspike/shots-direct || true
run direct-full "$D" --frames 300 || true
run stock-vk "" --mode vk --frames 300 || true
run direct-ring "$D" --mode vk --frames 300 --ibuf ring || true
run direct-tiny "$D" --frames 300 --tiny || true
run direct-draws800 "$D" --frames 200 --draws 800 || true
# soak: about 10 minutes of Vulkan frames, then any panfrost faults/timeouts since the start
[ -n "$SOAK" ] && run direct-soak "$D" --mode vk --frames "$SOAK" || true
dev "dmesg | tail -n +$((n0 + 1)) | grep -i 'panfrost\|fault\|timeout' | tail -20; echo new-fault-lines-end" | tee "$OUT/dmesg.txt" || true
for n in gl vk; do dev "cat /data/rgds/vkspike/shots-direct/$n.ppm" > "$OUT/$n.ppm" || true; done
echo "results in $OUT"
