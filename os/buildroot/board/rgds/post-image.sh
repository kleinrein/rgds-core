#!/bin/sh
# Buildroot post-image: RG DS OS's SD card image (rgds-os.img, and .gz to flash).
set -e
DTB=rk3568-anbernic-rg-ds
HERE=$(dirname "$0")
cd "$BINARIES_DIR"
# The kernel U-Boot boots: Image in an LZ4 frame (half the bytes to read from the card; U-Boot's booti unpacks it
# in about 50 ms). U-Boot reads only the modern frame with independent blocks, not the kernel's own Image.lz4
# (a legacy frame, which it does not recognise and which left the slot unbootable on 27 October 2026).
"$HOST_DIR/bin/lz4" -9 -f --no-frame-crc Image Image.lz4 > /dev/null
sed "s/@DTB@/$DTB/g" "$HERE/extlinux.conf.in" > extlinux.conf
# U-Boot's boot counter, idle (upgrade-in-progress 0): it only counts while a slot switch is being tried
printf '\275\001\000\000' > bootcount
sed "s/@DTB@/$DTB/g" "$HERE/genimage.cfg.in" > genimage.cfg
rm -rf "$BUILD_DIR/genimage.tmp"
genimage --rootpath "$TARGET_DIR" --tmppath "$BUILD_DIR/genimage.tmp" --inputpath "$BINARIES_DIR" \
    --outputpath "$BINARIES_DIR" --config genimage.cfg
gzip -kf6 rgds-os.img
echo "SD card image: $BINARIES_DIR/rgds-os.img(.gz)"
