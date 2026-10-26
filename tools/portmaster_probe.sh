#!/bin/sh
# Read-only check of what PortMaster needs on an RG DS card (docs/PORTMASTER_PLAN.md, Phase 0).
# Run on the handheld: ssh root@<address> 'nice -n 10 sh -s' < tools/portmaster_probe.sh
# Reads files, /proc and /sys, and runs version queries. Writes nothing.

section() { printf '\n== %s\n' "$1"; }
where() {
    for name in "$@"; do
        path=$(command -v "$name" 2>/dev/null)
        if [ -z "$path" ]; then printf '%-12s missing\n' "$name"; continue; fi
        real=$(readlink -f "$path" 2>/dev/null)
        case "$real" in */busybox*) kind=busybox ;; *) kind=own ;; esac
        printf '%-12s %s (%s)\n' "$name" "$path" "$kind"
    done
}
library() {
    for name in "$@"; do
        found=$(ls /usr/lib/$name* /lib/$name* /usr/lib/aarch64-linux-gnu/$name* /lib/aarch64-linux-gnu/$name* 2>/dev/null | head -n 1)
        printf '%-22s %s\n' "$name" "${found:-missing}"
    done
}

section system
uname -a
cat /etc/os-release 2>/dev/null
tr -d '\0' < /proc/device-tree/model 2>/dev/null; echo
tr '\0' ' ' < /proc/device-tree/compatible 2>/dev/null; echo
grep -E '^(MemTotal|SwapTotal|CmaTotal)' /proc/meminfo
nproc 2>/dev/null
for marker in /opt/muos /opt/system/Tools /opt/tools /roms /storage /userdata /mnt/SDCARD/spruce /usr/share/plymouth/themes/text.plymouth; do
    [ -e "$marker" ] && echo "present: $marker"
done
ls -ld /opt /opt/* 2>/dev/null
getconf GNU_LIBC_VERSION 2>/dev/null || /lib/libc.so.6 2>/dev/null | head -n 1

section shell
ls -l /bin/sh /bin/bash /usr/bin/bash 2>/dev/null
bash --version 2>/dev/null | head -n 1
readlink -f /bin/sh
ls -l /bin 2>/dev/null | head -n 3
where bash sed grep awk gawk find xargs tar unzip gzip gunzip xz unxz zstd 7z 7za cp mv ln chmod tee md5sum sha256sum
where pkill pidof killall kill ps mount umount losetup unshare nsenter chroot setsid timeout sudo doas systemctl
where unsquashfs mksquashfs squashfuse fusermount zramctl mkswap swapon dd strings file ldd readelf strace gdb
where python3 aplay amixer pactl pw-cli weston weston-screenshooter dos2unix rsync dialog innoextract ffmpeg curl wget
grep -P 'a' /dev/null 2>&1 | head -n 1 && echo 'grep -P: ok' || echo 'grep -P: refused'
sed --version 2>&1 | head -n 1

section kernel
grep -E 'squashfs|exfat|vfat|fuse|overlay|cgroup|tmpfs|ext4|f2fs' /proc/filesystems
ls /dev/loop* 2>/dev/null | tr '\n' ' '; echo
cat /sys/module/loop/parameters/max_loop 2>/dev/null
ls -l /dev/uinput /dev/fuse /dev/zram0 2>/dev/null
cat /proc/swaps
[ -r /proc/config.gz ] && zcat /proc/config.gz | grep -E '^CONFIG_(SQUASHFS|BLK_DEV_LOOP|INPUT_UINPUT|ZRAM|SWAP|CGROUP|CGROUP_FREEZER|MEMCG|COMPAT|NAMESPACES|USER_NS|PID_NS|FUSE_FS|EXFAT_FS|OVERLAY_FS|PERF_EVENTS)'
[ -r /proc/config.gz ] || echo 'no /proc/config.gz'
ls /lib/ld-linux-armhf.so.3 /usr/lib/ld-linux-armhf.so.3 /lib/ld-linux-aarch64.so.1 2>&1
ls -d /lib32 /usr/lib32 /usr/lib/arm-linux-gnueabihf 2>/dev/null

section cgroups-and-mounts
mount | grep -E ' /( |tmp|dev/shm|run|mnt|sys/fs/cgroup|userdata|oem)'
cat /sys/fs/cgroup/cgroup.controllers 2>/dev/null
cat /sys/fs/cgroup/cgroup.subtree_control 2>/dev/null
ls /sys/fs/cgroup 2>/dev/null | head -n 30 | tr '\n' ' '; echo
df -h / /mnt/mmc /tmp /dev/shm 2>/dev/null
grep -E '/mnt/mmc|/mnt/SDCARD' /proc/mounts

section python
python3 - <<'EOF'
import sys, ssl, sqlite3
print(sys.version)
for module in ("lzma", "bz2", "zlib", "ssl", "ctypes", "sqlite3", "json", "urllib.request"):
    try:
        __import__(module)
        print(module, "ok")
    except Exception as error:
        print(module, "missing:", error)
print(ssl.OPENSSL_VERSION)
import os
print("os.unshare" if hasattr(os, "unshare") else "no os.unshare")
EOF

section libraries
library libSDL2-2.0.so libSDL2_image libSDL2_ttf libSDL2_mixer libSDL2_net libGLESv2 libEGL libmali libGL.so libgbm libvulkan
library libwayland-client libwayland-egl libdrm libasound libopenal libpulse libfreetype libfontconfig libpng16 libjpeg libz.so libstdc++ libtheoradec libvorbis libogg libFLAC libmpg123 libSDL2_gfx libevdev libudev libcurl libssl libcrypto liblzma libbz2 libxkbcommon libX11 libicu
strings -n 6 $(ls /usr/lib/libSDL2-2.0.so.0* 2>/dev/null | head -n 1) 2>/dev/null | grep -E '^(wayland|KMSDRM|x11|offscreen|alsa|pulseaudio|pipewire)$' | sort -u | tr '\n' ' '; echo
ls -l /usr/lib/libSDL2-2.0.so* 2>/dev/null
ls /mnt/vendor/deep/drastic_aarch64/libs 2>/dev/null | head -n 20 | tr '\n' ' '; echo

section graphics-and-display
ls /dev/dri 2>/dev/null
for d in /sys/class/drm/card*-*; do printf '%s %s %s\n' "$d" "$(cat $d/status 2>/dev/null)" "$(head -n 1 $d/modes 2>/dev/null)"; done
cat /sys/class/drm/card*-*/panel_orientation 2>/dev/null
ls /sys/module | grep -E -i 'mali|panfrost|bifrost'
cat /sys/module/mali_kbase/version 2>/dev/null
weston --version 2>&1 | head -n 1
for f in /etc/xdg/weston/weston.ini /etc/weston.ini /root/.config/weston.ini; do [ -f "$f" ] && { echo "--- $f"; cat "$f"; }; done
ls /usr/lib/weston /usr/lib/libweston* 2>/dev/null | tr '\n' ' '; echo
ls /run/user/0 /var/run 2>/dev/null | grep -i wayland
tail -n 15 /var/log/weston.log 2>/dev/null

section input
cat /proc/bus/input/devices | grep -E '^(N:|H:)'
ls -l /dev/input/by-path 2>/dev/null
ls /dev/input 2>/dev/null | tr '\n' ' '; echo

section audio
cat /proc/asound/cards
ls /etc/asound.conf 2>/dev/null && head -n 40 /etc/asound.conf

section stock-frontend
ls /mnt/mmc/Roms 2>/dev/null | tr '\n' ' '; echo
ls -la /mnt/mmc/Roms/PORTS /mnt/mmc/Roms/ports 2>/dev/null | head -n 20
ls /mnt/mmc 2>/dev/null | tr '\n' ' '; echo
ls /mnt/vendor 2>/dev/null | tr '\n' ' '; echo
ls /mnt/vendor/deep 2>/dev/null | tr '\n' ' '; echo
grep -l -i -a -E 'PORTS|\.sh' /mnt/vendor/bin/*.bin /mnt/vendor/*.bin 2>/dev/null | head -n 5
for f in /mnt/vendor/bin/dmenu.bin /mnt/vendor/bin/muos2.bin; do [ -f "$f" ] && { echo "--- $f"; strings -n 5 "$f" 2>/dev/null | grep -E -i 'ports|\.sh"|Roms/' | head -n 20; }; done
find / -xdev -maxdepth 4 -iname 'PortMaster*' 2>/dev/null | head -n 10
find /mnt/vendor -maxdepth 3 -iname '*.bin' 2>/dev/null | head -n 20

section network
ip -4 addr 2>/dev/null | grep inet
cat /etc/resolv.conf 2>/dev/null | head -n 3
