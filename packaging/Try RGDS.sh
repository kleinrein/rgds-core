#!/bin/sh
set -u
for RGDS_CARD in /mnt/mmc /mnt/SDCARD; do
    [ -f "$RGDS_CARD/rgds/main.sh" ] || continue
    exec sh "$RGDS_CARD/rgds/main.sh"
done
echo "Copy the complete RGDS test package next to Roms/ first."
exit 1
