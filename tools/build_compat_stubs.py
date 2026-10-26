#!/usr/bin/env python3
"""Build the stub libraries of the PortMaster compat layer (runtime/portmaster_compat.py).

armhf: the 32-bit stubs that point armhf ports at Rockchip's 32-bit libmali.

The stock card's own 64-bit libEGL.so.1 and libGLESv2.so.2 are small libraries
that only name libmali.so.1; the driver itself holds every EGL, GLES, GBM and
wayland-egl symbol. The armhf layer (runtime/portmaster_compat.py) works the
same way: each stub here has its soname and one DT_NEEDED entry, libmali.so.1,
so a 32-bit program asking for libEGL.so.1 ends up in the driver. Building
needs no driver: a placeholder libmali.so.1 only gives the linker the name.

gl4es: libOpenGL.so.0 and libGLX.so.0 for ports linked against glvnd (Bugdom, Nanosaur), each naming only
libGL.so.1, which is gl4es in the same folder; gl4es exports the GL and GLX functions glvnd would dispatch.

    python3 tools/build_compat_stubs.py       # writes runtime/portmaster_files/armhf/ and gl4es/
"""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
# (folder, zig target, library every stub names, stubs)
SETS = (
    (
        'armhf', 'arm-linux-gnueabihf.2.17', 'libmali.so.1',
        ('libEGL.so.1', 'libGLESv2.so.2', 'libGLESv1_CM.so.1', 'libgbm.so.1', 'libwayland-egl.so.1')
    ),
    ('gl4es', 'aarch64-linux-gnu.2.17', 'libGL.so.1', ('libOpenGL.so.0', 'libGLX.so.0')),
)


def zig_cc(target, *args):
    subprocess.run(['zig', 'cc', '-target', target, *args], check=True)


def main():
    if not shutil.which('zig'): sys.exit('zig is needed (it cross-compiles for ARM)')
    for name, target, real, stubs in SETS:
        out = ROOT / 'runtime/portmaster_files' / name
        out.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory() as temp:
            temp = Path(temp)
            source = temp / 'stub.c'
            source.write_text(
                '/* RG DS Core compat stub: every symbol is in %s, named as DT_NEEDED. */\n'
                'int rgds_compat_stub;\n' % real
            )
            # A placeholder only gives the linker the name to record.
            zig_cc(target, '-shared', '-fPIC', '-nostdlib', '-Wl,-soname,' + real, '-o', str(temp / real), str(source))
            for stub in stubs:
                zig_cc(
                    target, '-shared', '-fPIC', '-nostdlib', '-Wl,-soname,' + stub, '-Wl,--no-as-needed', '-o',
                    str(out / stub), str(source), str(temp / real)
                )
                print(name, stub, (out / stub).stat().st_size, 'bytes')
    # Desktop-GL ports ask SDL for what Wayland EGL on Mali cannot give: a compatibility or core profile (Bugdom: EGL
    # fails with EGL_BAD_ATTRIBUTE; without a profile, SDL binds the desktop API and libmali's context draws nothing)
    # or a 32-bit depth buffer (no config). An ES profile gets the GLES context gl4es draws desktop GL on. Preloaded
    # for GL ports only (portmaster_compat.gl_environment).
    shim = ROOT / 'runtime/portmaster_files/gl4es/librgds-gl-attrs.so'
    with tempfile.TemporaryDirectory() as temp:
        source = Path(temp) / 'attrs.c'
        source.write_text(
            '''/* RG DS Core: SDL GL calls for gl4es on Mali: desktop profiles become ES, depth at most 24 bits, gl4es's lookup. */
#define _GNU_SOURCE
#include <dlfcn.h>
typedef int (*set_attribute)(int, int);
int SDL_GL_SetAttribute(int attribute, int value) {
    static set_attribute real;
    if (!real) real = (set_attribute)dlsym(RTLD_NEXT, "SDL_GL_SetAttribute");
    if (attribute == 6 && value > 24) value = 24;  /* SDL_GL_DEPTH_SIZE */
    if (attribute == 21 && (value == 1 || value == 2)) value = 4;  /* core/compatibility profile -> ES */
    return real ? real(attribute, value) : -1;
}
/* Under EGL, SDL looks GL functions up in the Mali driver, which bypasses gl4es; gl4es has its own lookup. */
typedef void *(*lookup)(const char *);
void *SDL_GL_GetProcAddress(const char *name) {
    static lookup real, gl4es;
    if (!real) real = (lookup)dlsym(RTLD_NEXT, "SDL_GL_GetProcAddress");
    if (!gl4es) gl4es = (lookup)dlsym(RTLD_DEFAULT, "glXGetProcAddressARB");
    void *found = gl4es ? gl4es(name) : 0;
    return found ? found : real ? real(name) : 0;
}
'''
        )
        zig_cc('aarch64-linux-gnu.2.17', '-shared', '-fPIC', '-O2', '-o', str(shim), str(source), '-ldl')
        print('gl4es', shim.name, shim.stat().st_size, 'bytes')
    return 0


if __name__ == '__main__':
    sys.exit(main())
