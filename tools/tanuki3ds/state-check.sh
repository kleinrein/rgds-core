#!/bin/sh
# state-check.sh ROM ROUTE WARM ENGINE_A ENGINE_B: on the handheld, a state ENGINE_A writes must load in ENGINE_B and
# the other way round. Build ENGINE_B with -DSS_LAYOUT_TEST and -O2 so its structs and code all sit elsewhere, or use
# the engine of an earlier release. ROUTE is a SPIKE_INPUT file, WARM how long A plays first, so that the state holds a
# running game. Everything happens on copies in /data/rgds/state-check; the player's own saves are only read.
#
# Each pass shows: the state loaded, the game ran on (frames per second and no crash), and what the loading engine
# wrote straight after loading. Comparing those bytes with the file it read is the last step, on the Mac: state-diff.py.
E=/data/rgds/engines/tanuki3ds
U=/data/rgds/userdata
T=/data/rgds/state-check
rom=$1; route=$2; warm=$3; a=$4; b=$5
for h in /run/rgds-hold /run/rgds/*-hold; do
    [ -e "$h" ] && [ "$h" != /run/rgds/statecheck-hold ] && { echo "$h: another session holds the panels"; exit 1; }
done
[ -n "$(pidof ctremu)" ] && { echo "a game is running"; exit 1; }

# run NAME ENGINE SECONDS [VAR=VAL...]: ENGINE with what emulators.py gives it, its home and control files in the copy
run() {
    name=$1; engine=$2; seconds=$3; shift 3
    # a control file left behind would be obeyed at boot
    : > $T/session/emulator.control
    : > $T/session/emulator.control.result
    mkdir -p $T/shots-$name
    sed "s|exec \"\$H/ctremu\"|exec \"$engine\"|; s|^H=.*|H=$E|" $E/run.sh > $T/run-$name.sh
    env HOME=$T/home SPIKE_NOGUI=1 SPIKE_PANELS=1 SPIKE_PIN=1 MESA_NEW=1 RGDS_CORE=rgds-play \
        RGDS_CONTROL_FILE=$T/session/emulator.control RGDS_AUTOSTATE_FILE=$T/$name.dss RGDS_AUTOSAVE=0 \
        RGDS_SHARED_MENU=1 RGDS_MENU_LIVE=1 RGDS_VOLUME_DATA=$T SPIKE_SHOTS=$T/shots-$name SPIKE_SHOT_EVERY=240 "$@" \
        sh $T/run-$name.sh "$rom" > $T/$name.log 2>&1 &
    sleep "$seconds"
}

stop() {
    c=$(pidof ctremu)
    [ -n "$c" ] && kill -TERM $c
    n=0; while [ -n "$(pidof ctremu)" ] && [ $n -lt 40 ]; do sleep 0.25; n=$((n+1)); done
}

# save NAME: the bridge writes $T/NAME.dss
save() {
    echo "state-check-$1 save" > $T/session/emulator.control.t
    mv $T/session/emulator.control.t $T/session/emulator.control
    n=0; while ! grep -q "state=saved" $T/session/emulator.control.result && [ $n -lt 150 ]; do sleep 0.2; n=$((n+1)); done
    cat $T/session/emulator.control.result | tr "\n" " "; echo
    grep -q "state=saved" $T/session/emulator.control.result || echo "NOT SAVED"
}

# from A to B: A plays and saves, B loads it, writes it again straight away and plays on
pass() { # NAME WRITER READER
    run $1 "$2" "$warm" SPIKE_INPUT=/data/rgds/3ds-spike/$route
    save $1
    ls -l $T/$1.dss
    stop
    cp $T/$1.dss $T/$1-read.dss
    run $1-read "$3" 25 RGDS_NATIVE_AUTOLOAD=1 RGDS_STATE_AGAIN=$T/$1-again.dss
    alive=$(pidof ctremu)
    stop
    echo "== $1: $(basename "$2") -> $(basename "$3"): still running after 25 s: ${alive:-NO}"
    grep -aE "state at start read|state written again|^state:|spike crash|segv" $T/$1-read.log | cut -c1-120
    grep -a "FPS" $T/$1-read.log | tail -3 | cut -c1-100
    ls -l $T/$1-again.dss
}

rm -rf "${T:?}"/shots-* "${T:?}"/*.dss
mkdir -p $T/home/.local/share/Tanuki3DS $T/session
H=$U/emulators/rgds-play/3ds/.local/share/Tanuki3DS
cp $H/ctremu.ini $T/home/.local/share/Tanuki3DS/
[ -d $T/home/.local/share/Tanuki3DS/3ds ] || cp -r $H/3ds $T/home/.local/share/Tanuki3DS/
cp $U/volume.ini $T/ 2> /dev/null

touch /run/rgds/statecheck-hold
cp /usr/libexec/rgds/session /run/rgds/session.statecheck
printf '#!/bin/sh\nwhile [ -e /run/rgds/statecheck-hold ]; do sleep 1; done\nexec /bin/sh /run/rgds/session.statecheck\n' > /run/rgds/session.statecheckwait
chmod 755 /run/rgds/session.statecheck /run/rgds/session.statecheckwait
mount --bind /run/rgds/session.statecheckwait /usr/libexec/rgds/session
killall rgds-launcher 2> /dev/null
n=0; while pidof rgds-launcher > /dev/null && [ $n -lt 20 ]; do sleep 0.5; n=$((n+1)); done

pass ab "$a" "$b"
pass ba "$b" "$a"

rm -f /run/rgds/statecheck-hold
n=0; while ! pidof rgds-launcher > /dev/null && [ $n -lt 15 ]; do sleep 1; n=$((n+1)); done
umount /usr/libexec/rgds/session 2> /dev/null
pidof rgds-launcher > /dev/null && echo "launcher back" || echo "launcher not back yet"
