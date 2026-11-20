#!/bin/sh
# One latency trial of a hand-run game on RG DS OS, with the session held (see run_game.py, game_latency.py and
# press_to_flip.py in /run/rgds). Usage: pace_trial.sh LABEL [ENV=VALUE ...]; the extra variables go to the game
# (PRE: ones before launch_plan). The game is asked to quit after the presses, so DSCore prints its pacing stats.
label=$1
shift
rm -f /run/rgds/latency /run/rgds/presses /run/rgds/cadence
timeout 90 python3 /run/rgds/run_game.py "/data/roms/nds/Mario Kart DS.nds" 80 \
    RGDS_LATENCY_TRACE=/run/rgds/latency LD_PRELOAD=/run/rgds/librgdsbridge.so DS_CADENCE_LOG=/run/rgds/cadence \
    DS_VERBOSE=1 "$@" > "/run/rgds/trial-$label.out" 2>&1 &
sleep 14
echo "== $label: $*"
python3 /run/rgds/game_latency.py /run/rgds/latency /dev/input/event7 20 304 /run/rgds/presses
echo "$(date +%s) quit" > /data/rgds/userdata/session/emulator.control
wait
python3 /run/rgds/press_to_flip.py /run/rgds/presses /run/rgds/cadence
grep -iE "^pace|repeat|dropped|not presented|^EXIT|^TIMEOUT|error" "/run/rgds/trial-$label.out" | grep -v underrun | head -8
