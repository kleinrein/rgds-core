#!/bin/sh
# Crazy Taxi drive on the wait-loop core (flycast_idle_stats.so), one case per argument pair NAME OPTIONS
# (RGDS_PLAY_CORE_OPTIONS on top of the player's set): game/shown frames a second from the stats core, per-thread
# CPU at 85, 100 and 115 s, GPU per engine. Waits for the panels to be free.
#   sh dc_run_w.sh some "" more "reicast_auto_skip_frame=more"
# CORE=other.so picks the core, EXTRA="VAR=VALUE ..." adds to the player's environment for every case.
cd /data/rgds/dc-spike
while ls /run/rgds/*-hold /run/rgds-hold 2> /dev/null | grep -q . || pidof rgds-play ctremu ppsspp rgds-experimental > /dev/null; do sleep 5; done
sh session_hold.sh on || exit 1
rm -f userdata/session/emulator.control.*
B=/usr/share/rgds/releases/$(cat /usr/share/rgds/active)/emulators/hooks/librgdsbridge.so
P=/sys/bus/platform/drivers/panfrost/fde60000.gpu/profiling
echo 1 > $P
cp ${CORE:-flycast_idle_stats.so} flycast_libretro.so
while [ $# -ge 2 ]; do
    C=case-w$1
    rm -rf $C; mkdir -p $C
    python3 dc_gpu.py rgds-play 70 60 > $C/gpu 2>&1 &
    (sleep 85; python3 dc_threads.py $(pidof rgds-play) > $C/threads 2>&1
     sleep 10; python3 dc_threads.py $(pidof rgds-play) >> $C/threads 2>&1
     sleep 10; python3 dc_threads.py $(pidof rgds-play) >> $C/threads 2>&1) &
    python3 dc_spike.py "/data/roms/dc/Crazy Taxi (USA) (En,Ja)/Crazy Taxi (USA) (En,Ja).cue" 150 \
        RGDS_PLAY_INPUT=/data/rgds/dc-spike/dc_crazy_taxi.input RGDS_FLYCAST_STATS=2 \
        "RGDS_PLAY_CORE_OPTIONS=$2" LD_PRELOAD=$B $EXTRA > $C/log 2>&1
    wait
    shift 2
done
echo 0 > $P
cp flycast_state.so flycast_libretro.so
sh session_hold.sh off
echo W DONE
