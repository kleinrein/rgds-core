#!/bin/sh
# run.sh GAME [ARGS]: Tanuki3DS on KMSDRM with the bundled Mesa (Panfrost) answering desktop GL 3.3 core.
H=$(cd "$(dirname "$0")" && pwd)
export LD_LIBRARY_PATH=$H/lib __EGL_VENDOR_LIBRARY_DIRS=$H/egl GBM_BACKENDS_PATH=$H/lib/gbm
export MESA_GL_VERSION_OVERRIDE=${MESA_GL_VERSION_OVERRIDE:-3.3} MESA_GLSL_VERSION_OVERRIDE=${MESA_GLSL_VERSION_OVERRIDE:-330}
[ -n "$MESA_NEW" ] && export LD_LIBRARY_PATH=$H/lib2:$H/lib GBM_BACKENDS_PATH=$H/lib2/gbm
[ -z "$NO_GLTHREAD" ] && export LD_LIBRARY_PATH=$H/lib3:$LD_LIBRARY_PATH mesa_glthread=true
export SDL_AUDIO_DEVICE_SAMPLE_FRAMES=512
export SDL_VIDEO_DRIVER=kmsdrm MESA_SHADER_CACHE_DIR=/data/rgds/cache/mesa-spike
export SPIKE_SKIP=3
exec "$H/ctremu" "$@"
