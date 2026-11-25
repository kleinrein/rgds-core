#!/bin/sh
# Crazy Taxi drive for the main-thread work, one case per argument pair NAME "VAR=VALUE ..." (player environment):
# the player's own loop numbers (play: profile lines), per-thread CPU at 100 s and an upper-screen capture at 95 s. PROF=1 adds rgds_prof.so at 1 kHz
# from 60 s (case-yNAME/prof.*, live map copied at 100 s; report with rgds_prof_report.py --maps prof.livemaps).
# CORE picks the core (default flycast_sym7.so: package patches 0001-0007 with symbols), BRIDGE the bridge library
# (default the active release's). Waits for the panels to be free.
cd /data/rgds/dc-spike
while ls /run/rgds/*-hold /run/rgds-hold 2> /dev/null | grep -q . || pidof rgds-play ctremu ppsspp rgds-experimental > /dev/null; do sleep 5; done
sh session_hold.sh on || exit 1
rm -f userdata/session/emulator.control.*
B=${BRIDGE:-/usr/share/rgds/releases/$(cat /usr/share/rgds/active)/emulators/hooks/librgdsbridge.so}
cp ${CORE:-flycast_sym7.so} flycast_libretro.so
while [ $# -ge 2 ]; do
    C=/data/rgds/dc-spike/case-y$1
    rm -rf $C; mkdir -p $C
    PRE=$B
    PENV=
    if [ "$PROF" = 1 ]; then
        PRE=$B:/data/rgds/dc-spike/rgds_prof.so
        PENV="RGDS_PROF=$C/prof RGDS_PROF_START=60"
    fi
    (sleep 100; cp /proc/$(pidof rgds-play)/maps $C/prof.livemaps; python3 dc_threads.py $(pidof rgds-play) > $C/threads 2>&1) &
    (sleep 95; echo 95 > userdata/session/emulator.control.capture; sleep 3; mv userdata/session/emulator.control.capture-top.bmp $C/s95.bmp) &
    python3 dc_spike.py "/data/roms/dc/Crazy Taxi (USA) (En,Ja)/Crazy Taxi (USA) (En,Ja).cue" 150 \
        RGDS_PLAY_INPUT=/data/rgds/dc-spike/dc_crazy_taxi.input LD_PRELOAD=$PRE $PENV $2 > $C/log 2>&1
    wait
    echo "case $1 done"
    shift 2
done
cp flycast_state.so flycast_libretro.so
sh session_hold.sh off
echo Y DONE
