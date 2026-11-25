#!/bin/sh
# Wait-loop skip (package patch 0006, RGDS_FLYCAST_IDLESKIP) off against on, same Crazy Taxi drive and stats core
# (flycast_idle_stats.so): game/shown frames a second, per-thread CPU at 85, 100 and 115 s, GPU per engine, and an
# upper-screen capture at 95 s. $1 = extra core options for both drives. Waits for the panels to be free.
cd /data/rgds/dc-spike
while ls /run/rgds/*-hold /run/rgds-hold 2> /dev/null | grep -q . || pidof rgds-play ctremu ppsspp rgds-experimental > /dev/null; do sleep 5; done
sh session_hold.sh on || exit 1
rm -f userdata/session/emulator.control.*
B=/usr/share/rgds/releases/$(cat /usr/share/rgds/active)/emulators/hooks/librgdsbridge.so
P=/sys/bus/platform/drivers/panfrost/fde60000.gpu/profiling
echo 1 > $P
cp flycast_idle_stats.so flycast_libretro.so
U=userdata/session
drive() {
    rm -rf case-$1; mkdir -p case-$1
    python3 dc_gpu.py rgds-play 70 60 > case-$1/gpu 2>&1 &
    (sleep 85; python3 dc_threads.py $(pidof rgds-play) > case-$1/threads 2>&1
     sleep 5; echo 95 > $U/emulator.control.capture; sleep 3; mv $U/emulator.control.capture-top.bmp case-$1/s95.bmp
     sleep 2; python3 dc_threads.py $(pidof rgds-play) >> case-$1/threads 2>&1
     sleep 10; python3 dc_threads.py $(pidof rgds-play) >> case-$1/threads 2>&1) &
    python3 dc_spike.py "/data/roms/dc/Crazy Taxi (USA) (En,Ja)/Crazy Taxi (USA) (En,Ja).cue" 150 \
        RGDS_PLAY_INPUT=/data/rgds/dc-spike/dc_crazy_taxi.input RGDS_FLYCAST_STATS=2 RGDS_FLYCAST_IDLESKIP=$2 \
        "RGDS_PLAY_CORE_OPTIONS=$3" LD_PRELOAD=$B > case-$1/log 2>&1
    wait
}
drive ${2:-u}off 0 "$1"
drive ${2:-u}on 2 "$1"
echo 0 > $P
cp flycast_state.so flycast_libretro.so
sh session_hold.sh off
echo U DONE
