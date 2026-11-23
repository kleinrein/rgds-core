#!/bin/sh
# Depth from the vertex shader (RGDS_FLYCAST_VTXDEPTH=1) against Flycast's per-pixel gl_FragDepth, same Crazy Taxi
# drive and player defaults: game/shown frames a second (stats core), GPU per engine, and upper-screen captures at
# 95 and 115 s to compare for depth faults. Waits for the panels to be free.
cd /data/rgds/dc-spike
while ls /run/rgds/*-hold /run/rgds-hold 2> /dev/null | grep -q . || pidof rgds-play ctremu ppsspp rgds-experimental > /dev/null; do sleep 5; done
sh session_hold.sh on || exit 1
rm -f userdata/session/emulator.control.*
B=/usr/share/rgds/releases/$(cat /usr/share/rgds/active)/emulators/hooks/librgdsbridge.so
P=/sys/bus/platform/drivers/panfrost/fde60000.gpu/profiling
echo 1 > $P
cp flycast_stats.so flycast_libretro.so
U=userdata/session
drive() {
    mkdir -p case-$1
    python3 dc_gpu.py rgds-play 70 60 > case-$1/gpu 2>&1 &
    (sleep 95; echo 95 > $U/emulator.control.capture; sleep 3; mv $U/emulator.control.capture-top.bmp case-$1/s95.bmp
     sleep 17; echo 115 > $U/emulator.control.capture; sleep 3; mv $U/emulator.control.capture-top.bmp case-$1/s115.bmp) &
    python3 dc_spike.py "/data/roms/dc/Crazy Taxi (USA) (En,Ja)/Crazy Taxi (USA) (En,Ja).cue" 150 \
        RGDS_PLAY_INPUT=/data/rgds/dc-spike/dc_crazy_taxi.input RGDS_FLYCAST_STATS=2 RGDS_FLYCAST_VTXDEPTH=$2 \
        LD_PRELOAD=$B > case-$1/log 2>&1
    wait
}
drive sfrag 0
drive svtx 1
echo 0 > $P
cp flycast_state.so flycast_libretro.so
sh session_hold.sh off
echo S DONE
