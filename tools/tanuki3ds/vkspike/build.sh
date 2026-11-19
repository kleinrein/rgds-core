#!/bin/sh
# Builds vk_spike in the Lima VM (aarch64, clang-19, static glslang from glslang-dev) against the
# Tanuki fork's shader generators. From the Mac: tools/tanuki3ds/vkspike/build.sh
# Output: ~/3ds-spike/vkspike/out/{vk_spike,gpu.vert,run.sh} in the VM, and a tar of it.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
FORK=${FORK:-$HOME/dev/Tanuki3DS-rgds}
if [ "$(uname)" = Darwin ]; then
    export LIMA_HOME=$ROOT/.cache/linux-builder/state
    L=$ROOT/.cache/linux-builder/lima/bin/limactl
    # a fresh tree each time: limactl copy keeps Mac mtimes, a tar of the sources is simplest
    COPYFILE_DISABLE=1 tar --no-xattrs -C "$FORK" -cf - src/video src/common.h src/dynstring.h src/glad \
        | "$L" shell rgds-image -- sh -c 'rm -rf ~/3ds-spike/vkspike/fork && mkdir -p ~/3ds-spike/vkspike/fork && tar -C ~/3ds-spike/vkspike/fork -xf -'
    COPYFILE_DISABLE=1 tar --no-xattrs -C "$HERE" -cf - vk_spike.c vk_glsl.c vk_glsl.h build.sh run.sh \
        | "$L" shell rgds-image -- sh -c 'mkdir -p ~/3ds-spike/vkspike/src && tar -C ~/3ds-spike/vkspike/src -xf - && sh ~/3ds-spike/vkspike/src/build.sh'
    exit
fi
W=$HOME/3ds-spike/vkspike
cd "$W"
find fork src -name '._*' -delete
mkdir -p out
F=fork/src
CFLAGS="-std=gnu23 -O2 -g -mcpu=cortex-a55 -D_GNU_SOURCE -I$F -I$F/video -Isrc -Wall -Wno-unused-function -Wno-unused-variable"
A=/usr/lib/aarch64-linux-gnu
clang-19 $CFLAGS -c src/vk_spike.c -o out/vk_spike.o
clang-19 $CFLAGS -c src/vk_glsl.c -o out/vk_glsl.o
clang-19 $CFLAGS -w -c $F/video/shadergen_fs.c -o out/shadergen_fs.o
clang-19 $CFLAGS -w -c $F/glad/glad.c -o out/glad.o
clang-19 -o out/vk_spike out/vk_spike.o out/vk_glsl.o out/shadergen_fs.o out/glad.o \
    -Wl,--start-group $A/libglslang.a $A/libMachineIndependent.a $A/libGenericCodeGen.a \
    $A/libOSDependent.a $A/libSPIRV.a $A/libglslang-default-resource-limits.a \
    $A/libSPIRV-Tools-opt.a $A/libSPIRV-Tools.a -Wl,--end-group \
    -lEGL -ldl -lpthread -lm -l:libstdc++.a
cp $F/video/hostshaders/gpu.vert out/
cp src/run.sh out/
chmod +x out/run.sh
tar -C "$W" -cf "$W/vkspike.tar" out
ls -la out/vk_spike "$W/vkspike.tar"
