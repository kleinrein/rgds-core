#!/bin/sh
# Where the GPU's ~21.5 ms a frame goes: the same Crazy Taxi drive (0004 core, the active release's bridge) as
# baseline, at half resolution (pixel cost), and with per-strip translucent sorting (fewer draws than per-triangle).
cd /data/rgds/dc-spike
while ls /run/rgds/*-hold /run/rgds-hold 2> /dev/null | grep -qv dsab-hold ||
    pidof rgds-play ctremu ppsspp rgds-experimental > /dev/null; do sleep 5; done
sh session_hold.sh on || exit 1
rm -f userdata/session/emulator.control.*
B=/usr/share/rgds/releases/$(cat /usr/share/rgds/active)/emulators/hooks/librgdsbridge.so
cp flycast_state.so flycast_libretro.so
drive() {
    mkdir -p case-$1
    python3 dc_spike.py "/data/roms/dc/Crazy Taxi (USA) (En,Ja)/Crazy Taxi (USA) (En,Ja).cue" 150 \
        RGDS_PLAY_INPUT=/data/rgds/dc-spike/dc_crazy_taxi.input "RGDS_PLAY_CORE_OPTIONS=$2" LD_PRELOAD=$B \
        > case-$1/log 2>&1
}
drive gbase ""
drive ghalf "flycast_internal_resolution=320x240"
drive gstrip "flycast_alpha_sorting=per-strip (fast, least accurate)"
sh session_hold.sh off
echo L DONE
