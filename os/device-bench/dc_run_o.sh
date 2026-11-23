#!/bin/sh
# The Crazy Taxi drive with Flycast's options really applied (they are "reicast_...": the player's "flycast_..." keys
# never reached the core before 2026-11-23). Intended set, then 320x240, frameskip "more", per-strip sorting. GPU per
# engine (dc_gpu.py), draw counts and guest frames per second (game speed) (stats core, RGDS_FLYCAST_STATS=2). Waits for the panels to be free.
cd /data/rgds/dc-spike
while ls /run/rgds/*-hold /run/rgds-hold 2> /dev/null | grep -qv dsab-hold ||
    pidof rgds-play ctremu ppsspp rgds-experimental > /dev/null; do sleep 5; done
sh session_hold.sh on || exit 1
rm -f userdata/session/emulator.control.*
B=/usr/share/rgds/releases/$(cat /usr/share/rgds/active)/emulators/hooks/librgdsbridge.so
P=/sys/bus/platform/drivers/panfrost/fde60000.gpu/profiling
echo 1 > $P
cp flycast_stats.so flycast_libretro.so
I="reicast_threaded_rendering=enabled;reicast_auto_skip_frame=some;reicast_alpha_sorting=per-triangle (normal);reicast_anisotropic_filtering=off;reicast_emulate_framebuffer=disabled;reicast_digital_triggers=enabled;reicast_per_content_vmus=VMU A1"
drive() {
    mkdir -p case-$1
    python3 dc_gpu.py rgds-play 30 130 > case-$1/gpu 2>&1 &
    (sleep 100; python3 dc_threads.py $(pidof rgds-play) > case-$1/threads 2>&1) &
    python3 dc_spike.py "/data/roms/dc/Crazy Taxi (USA) (En,Ja)/Crazy Taxi (USA) (En,Ja).cue" 170 \
        RGDS_PLAY_INPUT=/data/rgds/dc-spike/dc_crazy_taxi.input RGDS_FLYCAST_STATS=2 "RGDS_PLAY_CORE_OPTIONS=$I;$2" \
        LD_PRELOAD=$B > case-$1/log 2>&1
    wait
}
S="reicast_alpha_sorting=per-strip (fast, least accurate);reicast_auto_skip_frame=some"
drive qstrip "$S"
drive qnodsp "$S;reicast_enable_dsp=disabled"
echo 0 > $P
cp flycast_state.so flycast_libretro.so
sh session_hold.sh off
echo O DONE
