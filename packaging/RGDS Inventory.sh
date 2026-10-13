#!/bin/sh
set -u
for RGDS_CARD in /mnt/mmc /mnt/SDCARD; do
    [ -f "$RGDS_CARD/rgds/inventory.sh" ] || continue
    if mkdir -p "$RGDS_CARD/rgds/userdata" &&
        sh "$RGDS_CARD/rgds/inventory.sh" > "$RGDS_CARD/rgds/userdata/inventory.txt" 2>&1; then
        echo "Saved rgds/userdata/inventory.txt. No system settings were changed."
        exit 0
    fi
    echo "Could not save the inventory on $RGDS_CARD; check whether the card is read-only." >&2
done
echo "No inventory report was saved. Check the RGDS installation and card."
exit 1
