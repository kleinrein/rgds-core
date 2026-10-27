#!/bin/sh
# Buildroot post-build: a root filesystem for a handheld that boots to one program.
#   post-build.sh <target dir>
set -e
T=$1
# rcS starts everything; Buildroot's own init scripts go
rm -f "$T"/etc/init.d/S[0-9]*
mkdir -p "$T/data" "$T/run" "$T/boot"
V=$(cat "$BR2_EXTERNAL_RGDS_PATH/../../VERSION")
printf 'NAME="RG DS OS"\nID=rgds\nID_LIKE=buildroot\nVERSION="%s"\nVERSION_ID=%s\nPRETTY_NAME="RG DS OS %s"\n' \
    "$V" "$V" "$V" > "$T/usr/lib/os-release"
ln -sf ../usr/lib/os-release "$T/etc/os-release"
rm -rf "$T/usr/share/man" "$T/usr/share/doc" "$T/usr/share/info"
# Bonjour (rgds.local, and "RG DS" under Network in Finder while sharing is on): file_sharing.py writes its
# service file at runtime, so the services folder lives in /run
rm -rf "$T/etc/avahi/services"
ln -sfn /run/rgds/avahi-services "$T/etc/avahi/services"
