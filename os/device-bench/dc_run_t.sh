#!/bin/sh
# Which game code the SH4 thread runs in the Crazy Taxi drive: jit_peek samples Flycast-emu from 85 s for ~40 s and
# dumps the busiest pages of translated code; guest RAM is copied beside them. Stats core for game/shown fps, GPU per
# engine and per-thread CPU as in the other runs. Optional $1 = extra core options, $2 = case name.
cd /data/rgds/dc-spike
while ls /run/rgds/*-hold /run/rgds-hold 2> /dev/null | grep -q . || pidof rgds-play ctremu ppsspp rgds-experimental > /dev/null; do sleep 5; done
sh session_hold.sh on || exit 1
rm -f userdata/session/emulator.control.*
B=/usr/share/rgds/releases/$(cat /usr/share/rgds/active)/emulators/hooks/librgdsbridge.so
P=/sys/bus/platform/drivers/panfrost/fde60000.gpu/profiling
echo 1 > $P
cp ${CORE:-flycast_stats.so} flycast_libretro.so
C=case-${2:-jit}
rm -rf $C; mkdir -p $C
python3 dc_gpu.py rgds-play 70 60 > $C/gpu 2>&1 &
(sleep 85; python3 dc_threads.py $(pidof rgds-play) > $C/threads 2>&1
 ./jit_peek $(pidof rgds-play) Flycast-emu 6000 5000 $C/jit > $C/jit.top 2>&1
 python3 dc_guest_ram.py $(pidof rgds-play) $C/guest.mem; grep -n "rwxp" /proc/$(pidof rgds-play)/maps > $C/maps) &
python3 dc_spike.py "/data/roms/dc/Crazy Taxi (USA) (En,Ja)/Crazy Taxi (USA) (En,Ja).cue" 150 \
    RGDS_PLAY_INPUT=/data/rgds/dc-spike/dc_crazy_taxi.input RGDS_FLYCAST_STATS=2 \
    "RGDS_PLAY_CORE_OPTIONS=$1" LD_PRELOAD=$B > $C/log 2>&1
wait
echo 0 > $P
cp flycast_state.so flycast_libretro.so
sh session_hold.sh off
echo T DONE
