#!/bin/sh
# Waits for the panels (no other session's hold, no game), then three profiled Crazy Taxi drives with the spike's own
# Flycast (run_case.sh: baseline = patches 0001-0003, state = + 0004, nodsp2 = + 0004 with the sound DSP off), then
# lets the 3DS session go by removing its gate file.
cd /data/rgds/dc-spike
while ls /run/rgds/*-hold /run/rgds-hold 2> /dev/null | grep -qv dsab-hold ||
    pidof rgds-play ctremu ppsspp rgds-experimental > /dev/null; do sleep 5; done
sleep 10
sh session_hold.sh on || {
    rm -f /run/rgds/3ds-wait
    exit 1
}
rm -f userdata/session/emulator.control.*
sh run_case.sh base2 flycast_orphan.so
sh run_case.sh state2 flycast_state.so
sh run_case.sh nodsp2 flycast_state.so RGDS_PLAY_CORE_OPTIONS=flycast_enable_dsp=disabled
sh session_hold.sh off
rm -f /run/rgds/3ds-wait
echo F DONE
