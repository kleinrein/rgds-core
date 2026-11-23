#!/bin/sh
# What each Flycast feature costs in the Crazy Taxi drive with the player's defaults (per-strip, frameskip some): the
# stats core (game and shown frames a second) plus GPU time per engine, one option changed per drive. Waits for the
# panels to be free.
cd /data/rgds/dc-spike
while ls /run/rgds/*-hold /run/rgds-hold 2> /dev/null | grep -q . || pidof rgds-play ctremu ppsspp rgds-experimental > /dev/null; do sleep 5; done
sh session_hold.sh on || exit 1
rm -f userdata/session/emulator.control.*
B=/usr/share/rgds/releases/$(cat /usr/share/rgds/active)/emulators/hooks/librgdsbridge.so
P=/sys/bus/platform/drivers/panfrost/fde60000.gpu/profiling
echo 1 > $P
cp flycast_stats.so flycast_libretro.so
drive() {
    mkdir -p case-$1
    python3 dc_gpu.py rgds-play 70 60 > case-$1/gpu 2>&1 &
    (sleep 100; python3 dc_threads.py $(pidof rgds-play) > case-$1/threads 2>&1) &
    python3 dc_spike.py "/data/roms/dc/Crazy Taxi (USA) (En,Ja)/Crazy Taxi (USA) (En,Ja).cue" 150 \
        RGDS_PLAY_INPUT=/data/rgds/dc-spike/dc_crazy_taxi.input RGDS_FLYCAST_STATS=2 "RGDS_PLAY_CORE_OPTIONS=$2" \
        LD_PRELOAD=$B > case-$1/log 2>&1
    wait
}
drive rswap "reicast_delay_frame_swapping=disabled"
drive rmodvol "reicast_volume_modifier_enable=disabled"
drive rfog "reicast_fog=disabled"
drive rdsp "reicast_enable_dsp=disabled"
echo 0 > $P
cp flycast_state.so flycast_libretro.so
sh session_hold.sh off
echo Q DONE
