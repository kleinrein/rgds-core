#!/bin/sh
# Crazy Taxi drive, one Flycast option at a time; survives the SSH link dropping.
cd /data/rgds/dc-spike
sh session_hold.sh on || exit 1
for v in "$@"; do
    python3 dc_spike.py "/data/roms/dc/Crazy Taxi (USA) (En,Ja)/Crazy Taxi (USA) (En,Ja).cue" 165 RGDS_PLAY_INPUT=/data/rgds/dc-spike/dc_crazy_taxi.input "RGDS_PLAY_CORE_OPTIONS=$v" > run-$v.log 2>&1 &
    sleep 146
    echo "== $v"
    python3 dc_threads.py $(pidof rgds-play) | head -3
    wait
    grep profile run-$v.log | tail -3
    grep EXIT run-$v.log
    sleep 5
done
sh session_hold.sh off
echo ALL DONE
