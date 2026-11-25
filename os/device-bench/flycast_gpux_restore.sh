#!/bin/bash
# Back to the package state (0001-0007) in the VM build tree: undo the gpux edits, then the stats patch, and rebuild.
set -e
T=$(ls -d ~/own-os/output/rgds-os/build/libretro-flycast-3a5267a*/); cd $T
for f in core/rend/gles/*.pre-gpux; do mv "$f" "${f%.pre-gpux}"; touch "${f%.pre-gpux}"; done
patch -s -R -p1 < /Users/andreasrein/dev/rgds/os/device-bench/flycast-stats.patch
rm -f core/rend/gles/*.orig
cd buildroot-build && make -j6 flycast_libretro 2>&1 | grep -E "error|Linking"; md5sum flycast_libretro.so
