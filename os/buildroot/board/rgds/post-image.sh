#!/bin/sh
# Buildroot post-image: RG DS OS's SD card images, both from the one build (same kernel, root filesystem and U-Boot):
#   rgds-os.img(.gz)       the card for either handheld: both device trees in the slot, and extlinux.conf's FDTDIR
#                          loads the one U-Boot picked from the board's ID voltage (uboot-rgds.fragment's preboot)
#   rgds-os-plus.img(.gz)  the RG DS Plus's alone: its device tree named outright (FDT), whatever U-Boot measured;
#                          the fallback should the detection ever pick wrong on a Plus
set -e
DTBS="rk3568-anbernic-rg-ds rk3568-anbernic-rg-ds-plus"
HERE=$(dirname "$0")
cd "$BINARIES_DIR"
# The kernel U-Boot boots: Image in an LZ4 frame (half the bytes to read from the card; U-Boot's booti unpacks it
# in about 50 ms). U-Boot reads only the modern frame with independent blocks, not the kernel's own Image.lz4
# (a legacy frame, which it does not recognise and which left the slot unbootable on 27 October 2026).
"$HOST_DIR/bin/lz4" -9 -f --no-frame-crc Image Image.lz4 > /dev/null
sed -e "s/@FDT@/FDTDIR/" -e "s/@DTB@//" "$HERE/extlinux.conf.in" > extlinux.conf
sed -e "s/@FDT@/FDT/" -e "s/@DTB@/rk3568-anbernic-rg-ds-plus.dtb/" "$HERE/extlinux.conf.in" > extlinux-plus.conf
# U-Boot's boot counter, idle (upgrade-in-progress 0): it only counts while a slot switch is being tried
printf '\275\001\000\000' > bootcount
image() {  # image <name> <extlinux.conf> <device trees>
    files=; for d in $3; do files="$files\t\tfile a/$d.dtb { image = \"$d.dtb\" }\n"; done
    sed -e "s|^@DTBFILES@$|$files|" -e "s/@IMAGE@/$1.img/" -e "s/@BOOTVFAT@/$1-boot.vfat/" -e "s/@EXTLINUX@/$2/" \
        "$HERE/genimage.cfg.in" > $1-genimage.cfg
    rm -rf "$BUILD_DIR/genimage.tmp"
    genimage --rootpath "$TARGET_DIR" --tmppath "$BUILD_DIR/genimage.tmp" --inputpath "$BINARIES_DIR" \
        --outputpath "$BINARIES_DIR" --config $1-genimage.cfg
    gzip -kf6 $1.img
    echo "SD card image: $BINARIES_DIR/$1.img(.gz)"
}
image rgds-os extlinux.conf "$DTBS"
image rgds-os-plus extlinux-plus.conf rk3568-anbernic-rg-ds-plus
