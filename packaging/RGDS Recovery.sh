#!/bin/sh
set -u
for RGDS_CARD in /mnt/mmc /mnt/SDCARD; do
    [ -d "$RGDS_CARD/rgds" ] || continue
    mkdir -p "$RGDS_CARD/rgds/userdata"
    touch "$RGDS_CARD/rgds/userdata/boot_stock"
    echo "RGDS test launching disabled. A normal reboot starts stock Linux."
    echo "To test RGDS again, remove rgds/userdata/boot_stock from the card."
    exit 0
done
echo "RGDS folder not found."
exit 1
