#!/bin/sh
set -u
RGDS_RECOVERED=0
# Disable our global route even if the SD card has become read-only. This only
# removes RGDS's marker; original init, Wi-Fi, displays and supervisor stay intact.
if [ -d /userdata/rgds-boot ]; then
    if rm -f -- /userdata/rgds-boot/enabled; then
        RGDS_RECOVERED=1
        echo "RGDS direct startup disabled. The next boot uses Anbernic."
    else
        echo "Could not disable the RGDS startup marker." >&2
    fi
fi
# Return the stock hourly SD trim that RGDS deferred while it owned startup
# (runtime/system_tuning.py). Its bytes were never changed, only the mode.
# The final register power-off step is likewise returned to stock inittab.
RGDS_TUNING_OK=1
if [ -f /userdata/rgds-boot/tuning.json ] && grep -q '"fstrim"' /userdata/rgds-boot/tuning.json; then
    if [ -f /etc/init.d/S99fstrim ] && chmod 0755 /etc/init.d/S99fstrim; then
        echo "Stock SD trim service restored."
    else
        RGDS_TUNING_OK=0
    fi
fi
if grep -q rgds-final-poweroff /etc/inittab 2>/dev/null && [ -s /userdata/rgds-boot/inittab.original ]; then
    if cp /userdata/rgds-boot/inittab.original /etc/inittab.rgds-restore && mv /etc/inittab.rgds-restore /etc/inittab; then
        rm -f -- /usr/local/sbin/rgds-final-poweroff /userdata/rgds-boot/inittab.original
        echo "Stock shutdown sequence restored."
    else
        RGDS_TUNING_OK=0
    fi
fi
[ "$RGDS_TUNING_OK" = 0 ] || rm -f -- /userdata/rgds-boot/tuning.json
RGDS_APPS=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd) || exit 1
for RGDS_ROOT in "$RGDS_APPS/../../rgds" /mnt/mmc/rgds /mnt/SDCARD/rgds /userdata/rgds-device-test/rgds; do
    [ -d "$RGDS_ROOT" ] || continue
    if mkdir -p "$RGDS_ROOT/userdata" && touch "$RGDS_ROOT/userdata/boot_stock"; then
        RGDS_RECOVERED=1
        echo "RGDS APPS launching disabled at $RGDS_ROOT."
        echo "Remove its userdata/boot_stock file to launch RGDS again."
    else
        echo "Could not write the recovery file at $RGDS_ROOT; check its filesystem." >&2
    fi
done
[ "$RGDS_RECOVERED" = 0 ] || exit 0
echo "No writable RGDS installation or startup marker was found." >&2
exit 1
