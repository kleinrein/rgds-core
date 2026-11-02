#!/bin/sh
# Packs Tanuki3DS with SDL3 and the VM's Mesa (desktop GL through glvnd EGL) into ~/3ds-spike/tanuki-bundle.tar.
set -e
W=$HOME/3ds-spike
B=$W/tanuki-bundle
rm -rf "$B"
mkdir -p "$B/lib/dri" "$B/lib/gbm" "$B/egl"
A=/usr/lib/aarch64-linux-gnu
cp "$W/tanuki/build/ctremu" "$B/"
cp -L "$W"/sdl3/lib/libSDL3.so.0 "$B/lib/"
roots="$B/ctremu $B/lib/libSDL3.so.0 $A/libEGL.so.1 $A/libEGL_mesa.so.0 $A/libOpenGL.so.0 $A/libGLdispatch.so.0 $A/libgbm.so.1 $A/gbm/dri_gbm.so"
for g in $A/libgallium-*.so; do roots="$roots $g"; done
for f in $A/libEGL.so.1 $A/libEGL_mesa.so.0 $A/libOpenGL.so.0 $A/libGLdispatch.so.0 $A/libgbm.so.1 $A/libgallium-*.so; do cp -L "$f" "$B/lib/"; done
cp -L $A/gbm/dri_gbm.so "$B/lib/gbm/"
# every library they pull in, but the C library itself (the handheld's glibc 2.42 is newer than the VM's 2.39)
for r in $roots; do ldd "$r"; done | awk '/=> \//{print $3}' | sort -u | while read l; do
    case "${l##*/}" in libc.so*|libm.so*|libpthread*|libdl.so*|librt.so*|ld-linux*) continue;; esac
    cp -Ln "$l" "$B/lib/" 2>/dev/null || true
done
printf '{"file_format_version":"1.0.0","ICD":{"library_path":"libEGL_mesa.so.0"}}\n' > "$B/egl/50_mesa.json"
# run.sh beside this script: Mesa from lib2 and lib3 (see README), glthread on, the device opened with a 512-frame buffer
cp "$(dirname "$0")/run.sh" "$B/run.sh"
chmod +x "$B/run.sh"
du -sh "$B"; ls "$B/lib" | wc -l
tar -C "$W" -cf "$W/tanuki-bundle.tar" tanuki-bundle
ls -la "$W/tanuki-bundle.tar"
