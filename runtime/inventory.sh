#!/bin/sh
# Read-only inventory: deliberately excludes Wi-Fi profiles and private keys.
set -u
section() { printf '\n[%s]\n' "$1"; }
section system
uname -a
for f in /etc/os-release /etc/version /etc/issue; do [ ! -f "$f" ] || cat "$f"; done
getconf GNU_LIBC_VERSION 2>/dev/null || true
python3 --version 2>&1 || true
section mounts
cat /proc/mounts
df -h
section displays
for f in /sys/class/drm/card*-*/status /sys/class/drm/card*-*/modes; do
    [ -r "$f" ] || continue
    printf '%s: ' "$f"; cat "$f"
done
section inputs
cat /proc/bus/input/devices
section backlights
for f in /sys/class/backlight/*/brightness /sys/class/backlight/*/max_brightness; do
    [ -r "$f" ] || continue
    printf '%s: ' "$f"; cat "$f"
done
section power
for f in /sys/class/power_supply/*/type /sys/class/power_supply/*/status /sys/class/power_supply/*/capacity /sys/power/state /sys/power/mem_sleep /sys/class/anbernic_misc/hallkey /sys/devices/platform/anbernic_misc/power/wakeup; do
    [ -r "$f" ] || continue
    printf '%s: ' "$f"; cat "$f"
done
section clocks
for f in /sys/devices/system/cpu/cpufreq/policy*/scaling_available_frequencies /sys/devices/system/cpu/cpufreq/policy*/scaling_available_governors /sys/devices/system/cpu/cpufreq/policy*/scaling_min_freq /sys/devices/system/cpu/cpufreq/policy*/scaling_max_freq /sys/devices/system/cpu/cpufreq/policy*/scaling_governor /sys/class/devfreq/*/available_frequencies /sys/class/devfreq/*/available_governors /sys/class/devfreq/*/governor; do
    [ -r "$f" ] || continue
    printf '%s: ' "$f"; cat "$f"
done
section runtime_tools
for tool in python3 rsync tar unzip sha256sum nmcli weston-info wayland-info readelf; do command -v "$tool" 2>/dev/null || true; done
section wayland_sockets
for d in /run/user/0 /var/run /run /tmp; do
    for f in "$d"/wayland-*; do [ ! -S "$f" ] || ls -l "$f"; done
done
section emulators_and_sdl
for d in /mnt/vendor/deep/drastic_aarch64 /mnt/vendor/deep/drastic64 /mnt/mmc/nnddss /mnt/mmc/simpleos/system/bin; do
    [ ! -d "$d" ] || ls -l "$d"
done
for f in /usr/lib/libSDL2* /mnt/vendor/deep/drastic*/libs/libSDL2* /usr/lib/libdrastouch*; do
    [ ! -f "$f" ] || ls -l "$f"
done
section network_interfaces
ip -brief address 2>/dev/null || ifconfig 2>/dev/null || true
