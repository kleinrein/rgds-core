#!/bin/sh
# On the handheld: run.sh [vk_spike options]. GL through the Tanuki engine's own Mesa (lib2n, as
# run.sh of the engine sets it up, glthread on), Vulkan through RG DS OS's PanVK, no Vulkan loader.
# Uses no panels (EGL surfaceless, offscreen Vulkan) and no game data.
H=$(cd "$(dirname "$0")" && pwd)
E=${ENGINE:-/data/rgds/engines/tanuki3ds}
[ -d "$E" ] || E=/usr/share/rgds/engines/tanuki3ds
M=${MESA_DIR:-lib2n}
[ -d "$E/$M" ] || M=lib2
export LD_LIBRARY_PATH=$E/$M:$E/lib __EGL_VENDOR_LIBRARY_DIRS=$E/egl
export MESA_GL_VERSION_OVERRIDE=3.3 MESA_GLSL_VERSION_OVERRIDE=330
[ -z "$NO_GLTHREAD" ] && export mesa_glthread=true
export PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1
# a cache dir of the spike's own, so the engine's shader cache is never touched
export MESA_SHADER_CACHE_DIR=${MESA_SHADER_CACHE_DIR:-/data/rgds/vkspike/mesa-cache}
exec "$H/vk_spike" --vert "$H/gpu.vert" --icd "${ICD:-/usr/lib/rgds-panvk/libvulkan_panfrost.so}" "$@"
