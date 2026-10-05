#!/bin/sh
set -u
for RGDS_CARD in /mnt/mmc /mnt/SDCARD; do
    [ -f "$RGDS_CARD/rgds/inventory.sh" ] || continue
    mkdir -p "$RGDS_CARD/rgds/userdata"
    sh "$RGDS_CARD/rgds/inventory.sh" > "$RGDS_CARD/rgds/userdata/inventory.txt" 2>&1
    echo "Saved rgds/userdata/inventory.txt. No system settings were changed."
    exit 0
done
echo "RGDS inventory script not found on card."
exit 1
