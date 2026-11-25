#!/bin/sh
# Crazy Taxi drive, one case per argument pair NAME "VAR=VALUE ..." (added to the player's environment; the GPU
# experiments are RGDS_X_*, build/dc-spike/gpux): game/shown frames a second from the stats core, per-thread CPU,
# GPU per engine, an upper-screen capture at 95 and 110 s. CORE picks the core (default flycast_gpux_stats.so).
# Report with dc_x_report.sh. Waits for the panels to be free.
cd /data/rgds/dc-spike
while ls /run/rgds/*-hold /run/rgds-hold 2> /dev/null | grep -q . || pidof rgds-play ctremu ppsspp rgds-experimental > /dev/null; do sleep 5; done
sh session_hold.sh on || exit 1
rm -f userdata/session/emulator.control.*
B=/usr/share/rgds/releases/$(cat /usr/share/rgds/active)/emulators/hooks/librgdsbridge.so
P=/sys/bus/platform/drivers/panfrost/fde60000.gpu/profiling
echo 1 > $P
cp ${CORE:-flycast_gpux_stats.so} flycast_libretro.so
U=userdata/session
while [ $# -ge 2 ]; do
    C=case-x$1
    rm -rf $C; mkdir -p $C
    python3 dc_gpu.py rgds-play 70 60 > $C/gpu 2>&1 &
    (sleep 85; python3 dc_threads.py $(pidof rgds-play) > $C/threads 2>&1
     sleep 5; echo 95 > $U/emulator.control.capture; sleep 3; mv $U/emulator.control.capture-top.bmp $C/s95.bmp
     sleep 12; echo 110 > $U/emulator.control.capture; sleep 3; mv $U/emulator.control.capture-top.bmp $C/s110.bmp) &
    python3 dc_spike.py "/data/roms/dc/Crazy Taxi (USA) (En,Ja)/Crazy Taxi (USA) (En,Ja).cue" 150 \
        RGDS_PLAY_INPUT=/data/rgds/dc-spike/dc_crazy_taxi.input RGDS_FLYCAST_STATS=2 LD_PRELOAD=$B $2 > $C/log 2>&1
    wait
    echo "case $1 done"
    shift 2
done
echo 0 > $P
cp flycast_state.so flycast_libretro.so
sh session_hold.sh off
echo X DONE
