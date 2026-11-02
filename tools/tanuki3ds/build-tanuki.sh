#!/bin/sh
# Builds Tanuki3DS for aarch64 (desktop GL 3.3 core through SDL3) plus the SDL3 it links, under ~/3ds-spike.
set -e
W=$HOME/3ds-spike
mkdir -p "$W"
cd "$W"
log() { echo "[$(date +%T)] $*"; }
J=${J:-6}

log apt
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -q clang-19 lld-19 libfdk-aac-dev libopengl0 libxxhash-dev libcglm-dev libzstd-dev > apt.log 2>&1 || { tail apt.log; exit 1; }

if [ ! -d SDL ]; then
  log clone SDL3
  git clone -q --depth 1 -b release-3.2.24 https://github.com/libsdl-org/SDL SDL || git clone -q --depth 1 https://github.com/libsdl-org/SDL SDL
fi
if [ ! -d tanuki ]; then
  log clone tanuki
  git clone -q --depth 1 https://github.com/burhanr13/Tanuki3DS tanuki
fi
log "SDL $(git -C SDL describe --tags 2>/dev/null || git -C SDL rev-parse --short HEAD), tanuki $(git -C tanuki rev-parse --short HEAD)"

# SDL3 with KMSDRM (the handheld has no compositor), installed into a private prefix.
log build SDL3
cmake -S SDL -B SDL/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$W/sdl3 \
  -DSDL_KMSDRM=ON -DSDL_UNIX_CONSOLE_BUILD=ON -DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_PIPEWIRE=OFF -DSDL_PULSEAUDIO=OFF -DSDL_JACK=OFF \
  -DSDL_ALSA=ON -DSDL_VULKAN=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF > sdl-configure.log 2>&1 || { tail -30 sdl-configure.log; exit 1; }
grep -q "SDL_KMSDRM .*: ON" sdl-configure.log || { grep -i kmsdrm sdl-configure.log; echo "no KMSDRM"; exit 1; }
nice ninja -C SDL/build -j$J install > sdl-build.log 2>&1 || { tail -30 sdl-build.log; exit 1; }

log build tanuki
cd tanuki
cat > config.mk <<EOF
CC := clang-19
CXX := clang++-19
EOF
nice make -j$J USER=1 \
  CFLAGS_RELEASE="-O3 -g -mcpu=cortex-a55 -fno-pie -no-pie" \
  LIBDIRS="$W/sdl3/lib /usr/lib/aarch64-linux-gnu" \
  CPPFLAGS="-MP -MMD -D_GNU_SOURCE -isystem $W/sdl3/include -Isrc --embed-dir=sys_files -DNOCAPSTONE -DEMUVERSION=\\\"spike\\\" -DFASTMEM -DJIT_FASTMEM" \
  > ../tanuki-build.log 2>&1 || { tail -40 ../tanuki-build.log; exit 1; }
log "tanuki done: $(ls -la build/ctremu)"
