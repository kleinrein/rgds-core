#!/bin/sh
# Where the GPU's time goes in the Crazy Taxi drive, measured instead of inferred: Panfrost's per-engine busy time
# (fdinfo, profiling on) for the baseline, half resolution (pixels), per-strip sorting (draws) and frameskip "more"
# (game speed when frames are dropped). Same drive and core as dc_run_l.sh. Waits for the panels to be free.
cd /data/rgds/dc-spike
while ls /run/rgds/*-hold /run/rgds-hold 2> /dev/null | grep -qv dsab-hold ||
    pidof rgds-play ctremu ppsspp rgds-experimental > /dev/null; do sleep 5; done
sh session_hold.sh on || exit 1
rm -f userdata/session/emulator.control.*
B=/usr/share/rgds/releases/$(cat /usr/share/rgds/active)/emulators/hooks/librgdsbridge.so
P=/sys/bus/platform/drivers/panfrost/fde60000.gpu/profiling
echo 1 > $P
cp flycast_state.so flycast_libretro.so
drive() {
    mkdir -p case-$1
    python3 dc_gpu.py rgds-play 30 130 > case-$1/gpu 2>&1 &
    python3 dc_spike.py "/data/roms/dc/Crazy Taxi (USA) (En,Ja)/Crazy Taxi (USA) (En,Ja).cue" 200 \
        RGDS_PLAY_INPUT=/data/rgds/dc-spike/dc_crazy_taxi.input "RGDS_PLAY_CORE_OPTIONS=$2" LD_PRELOAD=$B \
        > case-$1/log 2>&1
    wait
}
# drive nbase ""
# drive nhalf "flycast_internal_resolution=320x240"
drive ndouble "flycast_internal_resolution=1280x960"
# drive mstrip "flycast_alpha_sorting=per-strip (fast, least accurate)"
# drive mskip "flycast_auto_skip_frame=more"
echo 0 > $P
sh session_hold.sh off
echo M DONE
