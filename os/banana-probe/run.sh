#!/bin/sh
# RG DS Core on BananaLinux, no compositor: a 90 s probe of both panels and touch (docs/OWN_OS_PLAN.md, Phase 2).
# Banana's menu runs this as a "system" (banana/systems.conf) after giving up the panels. It logs every step to
# rgds-probe/logs/<n>/ on the card and syncs after each one, so a hard power-off keeps everything up to the hang.
# Nothing outside rgds-probe/ and /tmp is written.
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
n=1
while [ -e "$HERE/logs/$n" ]; do n=$((n + 1)); done
LOG="$HERE/logs/$n"
mkdir -p "$LOG"
step() { echo "$(cut -d' ' -f1 /proc/uptime) $*" >> "$LOG/steps.txt"; sync; }

step "start (run $n)"
{
    uname -a
    cat /proc/cmdline
    for c in /sys/class/drm/card*-*; do echo "$c $(cat "$c/status" 2>/dev/null) $(head -1 "$c/modes" 2>/dev/null)"; done
    for e in /sys/class/input/event*; do echo "$e $(cat "$e/device/name") $(readlink -f "$e/device" | sed 's#.*/platform/##')"; done
    ls -la /usr/lib/libSDL2* /usr/lib/libEGL* /usr/lib/libGLESv2* 2>&1
    mount | grep -E ' /data | /tmp '
} > "$LOG/facts.txt" 2>&1
dmesg > "$LOG/dmesg-before.txt" 2>&1
step "facts"

# The bottom panel's touchscreen sits on i2c-5 (fe5e0000) on the RG DS.
TOUCH=
for e in /sys/class/input/event*; do
    case "$(readlink -f "$e/device")" in *fe5e0000.i2c*) TOUCH=/dev/input/$(basename "$e");; esac
done
step "touch=$TOUCH"

# The card is exFAT: run the launcher from /tmp so its execute bit holds. rgds-probe/rgds-launcher, when there, is a
# newer build tried against the release in app/.
APP="$HERE/app"
LAUNCHER="$APP/bin/rgds-launcher"
[ -f "$HERE/rgds-launcher" ] && LAUNCHER="$HERE/rgds-launcher"
cp "$LAUNCHER" /tmp/rgds-launcher && chmod 755 /tmp/rgds-launcher
mkdir -p "$HERE/data"
step "launcher copied"

# Naming the render driver turns SDL2's batching off unless it is asked for: without it each draw call flushed on its
# own and a frame took 35-170 ms a panel.
export SDL_VIDEODRIVER=kmsdrm SDL_KMSDRM_FIRST=DSI-2 SDL_RENDER_DRIVER=opengles2 SDL_RENDER_BATCHING=1
export RGDS_SESSION_PLATFORM=independent RGDS_INPUT_PROFILE=mainline-rg-ds
mkdir -p "$HERE/cache"
export MESA_SHADER_CACHE_DIR="$HERE/cache"
# RGDS_ALSA: an ALSA configuration to try instead of Banana's plughw:0 default (whine tests)
[ -n "$RGDS_ALSA" ] && export ALSA_CONFIG_PATH="$RGDS_ALSA"
export SDL_TOUCH_MOUSE_EVENTS=0 SDL_MOUSE_TOUCH_EVENTS=0 RGDS_SDL_LIBRARY=/usr/lib/libSDL2-2.0.so.0
[ -n "$TOUCH" ] && export RGDS_TOUCH_DEV="$TOUCH"
# Banana's bananad keeps the menu at a powersave GPU (200 MHz) and a 1.4 GHz CPU cap: full speed while we run.
GPU=/sys/class/devfreq/fde60000.gpu
CPU=/sys/devices/system/cpu/cpufreq/policy0
GPU_GOV=$(cat $GPU/governor) CPU_GOV=$(cat $CPU/scaling_governor) CPU_MAX=$(cat $CPU/scaling_max_freq)
echo performance > $GPU/governor
echo performance > $CPU/scaling_governor
cat $CPU/cpuinfo_max_freq > $CPU/scaling_max_freq
LIMIT=
command -v timeout > /dev/null && LIMIT="timeout ${RGDS_PROBE_LIMIT:-120}"
# RGDS_PROBE_ARGS: the launcher's mode (default its 90 s input probe; "--seconds 600" runs the real menu that long).
$LIMIT /tmp/rgds-launcher --app "$APP" --data "$HERE/data" --top-display 0 --bottom-display 1 \
    ${RGDS_PROBE_ARGS:---probe --seconds 90} > "$LOG/launcher.txt" 2>&1
step "launcher exited $?"
echo "$GPU_GOV" > $GPU/governor
echo "$CPU_GOV" > $CPU/scaling_governor
echo "$CPU_MAX" > $CPU/scaling_max_freq

dmesg > "$LOG/dmesg-after.txt" 2>&1
grep -iE 'panfrost|vop|dsi|flip_done|underflow|timeout|goodix' "$LOG/dmesg-after.txt" | tail -40 > "$LOG/dmesg-display.txt"
rm -f /tmp/rgds-launcher
step "done"
