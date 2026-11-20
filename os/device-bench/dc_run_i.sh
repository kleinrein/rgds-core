#!/bin/sh
# One Crazy Taxi drive (0004 core, the active release's bridge) with what the player's threads poll on sampled
# mid-drive (dc_polls.py) beside the thread states (dc_states.py). Waits until the panels are free.
cd /data/rgds/dc-spike
while ls /run/rgds/*-hold /run/rgds-hold 2> /dev/null | grep -qv dsab-hold ||
    pidof rgds-play ctremu ppsspp rgds-experimental > /dev/null; do sleep 5; done
sh session_hold.sh on || exit 1
rm -f userdata/session/emulator.control.*
B=/usr/share/rgds/releases/$(cat /usr/share/rgds/active)/emulators/hooks/librgdsbridge.so
mkdir -p case-polls
cp flycast_state.so flycast_libretro.so
python3 dc_spike.py "/data/roms/dc/Crazy Taxi (USA) (En,Ja)/Crazy Taxi (USA) (En,Ja).cue" 150 \
    RGDS_PLAY_INPUT=/data/rgds/dc-spike/dc_crazy_taxi.input LD_PRELOAD=$B > case-polls/log 2>&1 &
sleep 75
P=$(pidof rgds-play)
python3 dc_polls.py $P > case-polls/polls
python3 dc_states.py $P > case-polls/states
wait
sh session_hold.sh off
echo I DONE
