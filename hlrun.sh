#!/bin/bash
# hlrun.sh BIN TAG ROM FRAMES DUMPSPEC [extra args] : headless run under lavapipe in the VM -> build/frag-cost/runs/TAG/
# BIN is hl-old or hl-new (build/frag-cost/BIN-headless); ROM is a file in build/frag-cost/cap or build/zelda-cap.
# HLENV: engine environment for the run (e.g. "DS_LEAN_HALFPX=1"); PRESENT: the 3D scale (default 3).
# The dumps go straight to the run folder on the Mac (/tmp/hd in the VM is a symlink to it); each frame's
# sha256 is recorded in hashes.txt and, unless KEEP=1, the .raw files are deleted at once (disk space).
BIN=$1 TAG=$2 ROM=$3 FRAMES=$4 DUMP=$5; shift 5
ROOT=/Users/andreasrein/dev/rgds; Z=$ROOT/build/frag-cost
R=$Z/cap/$ROM; [ -f $R ] || R=$ROOT/build/zelda-cap/$ROM
export LIMA_HOME=$ROOT/.cache/linux-builder/state
$ROOT/.cache/linux-builder/lima/bin/limactl shell rgds-image -- bash -c "
O=$Z/runs/$TAG; rm -rf \$O; mkdir -p \$O; rm -rf /tmp/hd; ln -s \$O /tmp/hd
cd $Z
env $HLENV VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json DS_HIRES_DUMP=$DUMP DS_HIRES_DEBUG=1 timeout 1200 $Z/$BIN-headless $R --frames $FRAMES --present ${PRESENT:-3} --hash-frames \$O/hash.txt $* > \$O/log.txt 2>&1
echo exit \$?
rm -f /tmp/hd; cd \$O && ls *.raw 2>/dev/null | sort | xargs -r sha256sum > hashes.txt; [ '${KEEP:-0}' = 1 ] || rm -f *.raw; wc -l < hashes.txt
"
