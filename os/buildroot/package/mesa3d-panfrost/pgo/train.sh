#!/bin/bash
# Mesa's profile for package/mesa3d-panfrost, run in the build VM from ~/own-os (see README.md in this folder).
#   train.sh generate      instrumented libgallium in mesa-opt/out/gen
#   train.sh pack GCDA.TAR profile from the handheld -> mesa-opt/out/profile.tar.gz
set -e
O=~/own-os/output/rgds-os
M=~/own-os/mesa-opt
T=$M/tree
BR=$O/build/mesa3d-panfrost-25.0.7
HERE=$(cd "$(dirname "$0")" && pwd)
export PATH=$O/host/bin:$O/host/sbin:/usr/bin:/bin PYTHONNOUSERSITE=y CC_FOR_BUILD=/usr/bin/gcc CXX_FOR_BUILD=/usr/bin/g++
case $1 in
generate)
    # The same layout as Buildroot's (build folder inside the source, ../src/...): GCC hashes source paths into the
    # checksums the profile is matched by.
    mkdir -p "$T"
    rsync -a --delete --exclude buildroot-build --exclude '.stamp_*' "$BR/" "$T/"
    patch -d "$T" -p1 < "$HERE/dump-thread.patch"
    mkdir -p "$T/buildroot-build"
    cp "$BR/buildroot-build/cross-compilation.conf" "$M/cross.conf"
    A='["-D_LARGEFILE_SOURCE","-D_LARGEFILE64_SOURCE","-D_FILE_OFFSET_BITS=64","-O2","-g0","-D_FORTIFY_SOURCE=1","-DRGDS_PGO_TRAIN","-fprofile-update=prefer-atomic"]'
    meson setup "$T/buildroot-build" "$T" --cross-file "$M/cross.conf" --prefix=/usr --libdir=lib --buildtype=release \
        --default-library=shared -Db_pie=false -Db_staticpic=true -Dstrip=false -Db_lto=true -Db_lto_threads=6 -Db_pgo=generate \
        "-Dc_args=$A" "-Dcpp_args=$A" -Dgallium-drivers=panfrost -Dvulkan-drivers= -Dplatforms= -Dllvm=disabled -Dglx=disabled \
        -Dglvnd=disabled -Degl=enabled -Dgbm=enabled -Dopengl=true -Dgles1=enabled -Dgles2=enabled -Dshared-glapi=enabled \
        -Dgallium-va=disabled -Dgallium-vdpau=disabled -Dgallium-xa=disabled -Dgallium-nine=false -Dgallium-opencl=disabled \
        -Dgallium-rusticl=false -Dmicrosoft-clc=disabled -Dvideo-codecs= -Dxmlconfig=disabled -Dexpat=disabled -Dzstd=disabled \
        -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled -Dbuild-tests=false -Dtools= | tail -2
    L=src/gallium/targets/dri/libgallium-25.0.7.so
    (cd "$T/buildroot-build" && nice ninja -j6 "$L" | tail -1)
    mkdir -p "$M/out/gen"
    aarch64-buildroot-linux-gnu-strip -o "$M/out/gen/libgallium-25.0.7.so" "$T/buildroot-build/$L"
    ls -la "$M/out/gen" ;;
pack)
    # Names only inside the build folder, so the package unpacks it into its own.
    D=$(mktemp -d)
    tar -xf "$2" -C "$D"
    tar -C "$D" --sort=name --mtime=@0 --owner=0 --group=0 -czf "$M/out/profile.tar.gz" .
    find "$D" -name '*.gcda' | wc -l
    ls -la "$M/out/profile.tar.gz" ;;
esac
