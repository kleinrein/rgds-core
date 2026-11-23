#!/bin/sh
# Profiled Crazy Taxi drive with the player's defaults (per-strip, frameskip some): rgds_prof.so at 1 kHz from 60 s
# (the drive is ~65-135 s), Flycast unstripped (flycast_sym.so, same objects as the image's) so the report can name
# its functions, the live memory map at 100 s; then two drives at 180 and 160 MHz SH4. Waits for the panels to be free.
cd /data/rgds/dc-spike
while ls /run/rgds/*-hold /run/rgds-hold 2> /dev/null | grep -q . || pidof rgds-play ctremu ppsspp rgds-experimental > /dev/null; do sleep 5; done
sh session_hold.sh on || exit 1
rm -f userdata/session/emulator.control.* prof.samples prof.maps prof.threads prof.livemaps
B=/usr/share/rgds/releases/$(cat /usr/share/rgds/active)/emulators/hooks/librgdsbridge.so
cp flycast_sym.so flycast_libretro.so
python3 dc_spike.py "/data/roms/dc/Crazy Taxi (USA) (En,Ja)/Crazy Taxi (USA) (En,Ja).cue" 150 \
    RGDS_PLAY_INPUT=/data/rgds/dc-spike/dc_crazy_taxi.input RGDS_PROF=/data/rgds/dc-spike/prof RGDS_PROF_START=60 \
    LD_PRELOAD=$B:/data/rgds/dc-spike/rgds_prof.so > prof.log 2>&1 &
sleep 100; cp /proc/$(pidof rgds-play)/maps prof.livemaps
wait
# Then the SH4 underclocked (reicast_sh4clock, never applied before 2026-11-23): game speed from the stats core.
cp flycast_stats.so flycast_libretro.so
for mhz in 180 160; do
    mkdir -p case-clock$mhz
    (sleep 100; python3 dc_threads.py $(pidof rgds-play) > case-clock$mhz/threads 2>&1) &
    python3 dc_spike.py "/data/roms/dc/Crazy Taxi (USA) (En,Ja)/Crazy Taxi (USA) (En,Ja).cue" 150 \
        RGDS_PLAY_INPUT=/data/rgds/dc-spike/dc_crazy_taxi.input RGDS_FLYCAST_STATS=2 \
        RGDS_PLAY_CORE_OPTIONS=reicast_sh4clock=$mhz LD_PRELOAD=$B > case-clock$mhz/log 2>&1
    wait
done
cp flycast_state.so flycast_libretro.so
sh session_hold.sh off
echo P DONE
