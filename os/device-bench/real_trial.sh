#!/bin/sh
# real_trial.sh LABEL KIND [VAR=VAL ...]: Mario Kart DS as the launcher starts it, from the owner's state, every run from
# the same temperature, with the session held (session_hold.sh). Runs on its own copy of the game's files: a trial must
# never write the owner's saves or resume points (2 Nov: a rewritten .sav made the launcher skip the owner's resume).
# KIND real: emulators.py launch under the session's CPU mask; realfix: the same with a patched runtime copy in rt/;
# hand02, hand03: run_game.py with that mask or all CPUs, the performance governor set by hand.
# 20 presses, the menu for 4 s (the real one, B to resume, on real kinds), 20 presses, quit.
D=/run/rgds/dsab; O=/data/rgds/userdata; U=/data/rgds/trial-userdata; B=/data/rgds/trial-seed; K=5a1f594f6a2f82f9
if [ ! -d $B ]; then
    mkdir -p $B $U/emulators/rgds-experimental/saves $U/emulators/rgds-experimental/states $U/library/$K $U/session
    cp $O/*.ini $U/
    cp "$O/emulators/rgds-experimental/config.ini" $U/emulators/rgds-experimental/
    cp "$O/emulators/rgds-experimental/saves/Mario Kart DS.sav" $U/emulators/rgds-experimental/saves/
    cp $O/emulators/rgds-experimental/states/AMCE.0.dss $O/emulators/rgds-experimental/states/AMCE.auto.dss $B/
    cp $O/library/$K/auto-rgds-experimental.dss $B/
fi
A=/usr/share/rgds/releases/$(cat /usr/share/rgds/active)
ROM="/data/roms/nds/Mario Kart DS.nds"
label=$1; kind=$2; shift 2
i=0; while [ "$(cat /sys/class/thermal/thermal_zone0/temp)" -gt 52000 ] && [ $i -lt 170 ]; do sleep 2; i=$((i+2)); done
for f in AMCE.0.dss AMCE.auto.dss; do cp $B/$f $U/emulators/rgds-experimental/states/$f; done
cp $B/auto-rgds-experimental.dss $U/library/$K/auto-rgds-experimental.dss
rm -f "${D:?}"/latency "${D:?}"/"${label:?}".*
touch $D/running
( while [ -e $D/running ]; do echo "$(cat /sys/class/thermal/thermal_zone0/temp /sys/class/thermal/thermal_zone1/temp /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq /sys/class/devfreq/fde60000.gpu/cur_freq /sys/class/thermal/cooling_device0/cur_state /sys/class/thermal/cooling_device1/cur_state | tr '\n' ' ')" >> $D/$label.temp; sleep 5; done ) &
COMMON="RGDS_LATENCY_TRACE=$D/latency DS_CADENCE_LOG=$D/$label.cadence DS_VERBOSE=1"
case $kind in
real) env -i $(tr "\n" " " < $D/launcher.env) $COMMON "$@" taskset -c 0-2 python3 -B $A/runtime/emulators.py --app $A --data $U --core rgds-experimental --rom "$ROM" launch > $D/$label.out 2>&1 & ;;
realfix) env -i $(tr "\n" " " < $D/launcher.env) $COMMON "$@" taskset -c 0-2 python3 -B $D/rt/emulators.py --app $A --data $U --core rgds-experimental --rom "$ROM" launch > $D/$label.out 2>&1 & ;;
hand02|hand03)
    echo performance > /sys/devices/system/cpu/cpufreq/policy0/scaling_governor
    [ $kind = hand02 ] && M=0-2 || M=0-3
    env -i $(tr "\n" " " < $D/launcher.env) taskset -c $M timeout 110 python3 -B $D/run_game.py "$ROM" 100 $COMMON "$@" > $D/$label.out 2>&1 & ;;
esac
P=$!
sleep 12
echo "== $label $kind: $* (from $(head -1 $D/$label.temp | cut -c1-5))"
python3 -B $D/game_latency.py $D/latency /dev/input/event7 20 304 $D/$label.p1 | tail -2 | head -1
E=$(pidof rgds-experimental-vulkan | cut -d" " -f1)
for t in /proc/$E/task/*; do echo "$(cat $t/comm): $(grep Cpus_allowed_list $t/status | cut -f2)"; done | sort | uniq -c > $D/$label.mask
python3 -B $D/threads.py 5 > $D/$label.threads1
case $kind in
real*) printf 'RGDS\n' > $U/session/emulator.control.game-menu.new; mv $U/session/emulator.control.game-menu.new $U/session/emulator.control.game-menu; sleep 4
    python3 -B -c "
import struct, time
def emit(f, value):
    now = time.time()
    f.write(struct.pack('llHHi', int(now), int(now % 1 * 1e6), 1, 304, value))
    f.write(struct.pack('llHHi', int(now), int(now % 1 * 1e6), 0, 0, 0))
with open('/dev/input/event7', 'wb', buffering=0) as f:
    emit(f, 1); time.sleep(.1); emit(f, 0)
" ;;
*) touch $U/session/emulator.control.blocked; sleep 4; rm -f /data/rgds/userdata/session/emulator.control.blocked ;;
esac
sleep 4
python3 -B $D/game_latency.py $D/latency /dev/input/event7 20 304 $D/$label.p2 | tail -2 | head -1
python3 -B $D/threads.py 5 > $D/$label.threads2
echo "$(date +%s) quit" > $U/session/emulator.control
wait $P
rm -f /run/rgds/dsab/running
case $kind in hand*) echo schedutil > /sys/devices/system/cpu/cpufreq/policy0/scaling_governor ;; esac
LOG=$D/$label.out
case $kind in real*) cp $U/logs/rgds-experimental.log $D/$label.engine; LOG=$D/$label.engine ;; esac
echo "-- before the menu"; python3 -B $D/press_to_flip.py $D/$label.p1 $D/$label.cadence | grep -E "flip completed"
echo "-- after the menu"; python3 -B $D/press_to_flip.py $D/$label.p2 $D/$label.cadence | grep -E "flip completed"
grep -aE "^pace: display|^kms flips \(RG DS Core\)|^host:|thread layout|game held|game resumed|not permitted" $LOG | cut -c1-170
echo "adaptive holds: $(grep -ac 'adaptive: native' $LOG)"
