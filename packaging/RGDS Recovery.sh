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
