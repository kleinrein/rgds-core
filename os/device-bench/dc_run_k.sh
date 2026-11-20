#!/bin/sh
# Why Flycast's frame is several GPU batches: one Crazy Taxi drive (0004 core, the active release's bridge) with
# Panfrost's performance warnings on (PAN_MESA_DEBUG=perf: every early flush with its reason). Waits for the panels.
cd /data/rgds/dc-spike
while ls /run/rgds/*-hold /run/rgds-hold 2> /dev/null | grep -qv dsab-hold ||
    pidof rgds-play ctremu ppsspp rgds-experimental > /dev/null; do sleep 5; done
sh session_hold.sh on || exit 1
rm -f userdata/session/emulator.control.*
B=/usr/share/rgds/releases/$(cat /usr/share/rgds/active)/emulators/hooks/librgdsbridge.so
cp flycast_state.so flycast_libretro.so
mkdir -p case-perf
python3 dc_spike.py "/data/roms/dc/Crazy Taxi (USA) (En,Ja)/Crazy Taxi (USA) (En,Ja).cue" 130 \
    RGDS_PLAY_INPUT=/data/rgds/dc-spike/dc_crazy_taxi.input PAN_MESA_DEBUG=perf LD_PRELOAD=$B > case-perf/log 2>&1
sh session_hold.sh off
echo K DONE
