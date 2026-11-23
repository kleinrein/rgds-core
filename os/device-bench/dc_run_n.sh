#!/bin/sh
# Which part of Flycast's frame costs the GPU's ~17 ms in the Crazy Taxi drive: the stats core (RGDS_FLYCAST_STATS,
# 0001-0004 + per-list counters) once with a fence wait after each list (=1: GPU time per list) and once counting only
# (=2: draws and indices at normal speed). Waits for the panels to be free.
cd /data/rgds/dc-spike
while ls /run/rgds/*-hold /run/rgds-hold 2> /dev/null | grep -qv dsab-hold ||
    pidof rgds-play ctremu ppsspp rgds-experimental > /dev/null; do sleep 5; done
sh session_hold.sh on || exit 1
rm -f userdata/session/emulator.control.*
B=/usr/share/rgds/releases/$(cat /usr/share/rgds/active)/emulators/hooks/librgdsbridge.so
cp flycast_stats.so flycast_libretro.so
drive() {
    mkdir -p case-$1
    python3 dc_spike.py "/data/roms/dc/Crazy Taxi (USA) (En,Ja)/Crazy Taxi (USA) (En,Ja).cue" ${SECS:-160} \
        RGDS_PLAY_INPUT=/data/rgds/dc-spike/dc_crazy_taxi.input RGDS_FLYCAST_STATS=$2 "RGDS_PLAY_CORE_OPTIONS=$3" LD_PRELOAD=$B \
        > case-$1/log 2>&1
}
drive shalf 2 flycast_internal_resolution=320x240
# drive scount 2
cp flycast_state.so flycast_libretro.so
sh session_hold.sh off
echo N DONE
