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

kms: librgds-drm-lease.so (aarch64 and armhf) for RG DS OS, preloaded into a port and its menu helper: opening the
display card (RGDS_DRM_CARD) gives a copy of the panel's DRM lease (RGDS_DRM_LEASE_FD) instead (runtime/drm_lease.py).

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

# Whoever opens the display card gets a lease instead: SDL (the port's own copy too), GBM, libdrm. A port's processes
# each get their own from the runner (RGDS_DRM_LEASE_SOCKET), asked once per process and handed out again for every
# later open in it, since a DRM file's events and buffers belong to the file (runtime/drm_lease.py). The helper,
# one process, has its lease as an inherited descriptor (RGDS_DRM_LEASE_FD).
LEASE_SOURCE = r"""/* RG DS Core: the display card's open() answered with a panel's DRM lease (runtime/drm_lease.py). */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int asked(const char *path) {
    int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (s < 0) return -1;
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    strncpy(address.sun_path, path, sizeof address.sun_path - 1);
    int fd = -1;
    char byte = 'L', reply;
    union {
        struct cmsghdr header;
        char space[CMSG_SPACE(sizeof(int))];
    } control;
    struct iovec io = {&reply, 1};
    struct msghdr message = {.msg_iov = &io, .msg_iovlen = 1, .msg_control = control.space,
                             .msg_controllen = sizeof control.space};
    if (connect(s, (struct sockaddr *)&address, sizeof address) == 0 && write(s, &byte, 1) == 1 &&
        recvmsg(s, &message, MSG_CMSG_CLOEXEC) > 0) {
        struct cmsghdr *header = CMSG_FIRSTHDR(&message);
        if (header && header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS)
            memcpy(&fd, CMSG_DATA(header), sizeof fd);
    }
    close(s);
    return fd;
}

static int leased(const char *path, int flags) {
    const char *card = getenv("RGDS_DRM_CARD");
    if (!card || !path || strcmp(path, card) != 0) return -1;
    static pid_t owner;
    static int lease = -1;
    if (lease < 0 || owner != getpid()) {
        const char *runner = getenv("RGDS_DRM_LEASE_SOCKET"), *inherited = getenv("RGDS_DRM_LEASE_FD");
        int fd = runner ? asked(runner) : inherited ? fcntl(atoi(inherited), F_DUPFD_CLOEXEC, 0) : -1;
        if (fd < 0) return -1; /* the card itself, then: it fails as it would have without us */
        lease = fd;
        owner = getpid();
    }
    return fcntl(lease, (flags & O_CLOEXEC) ? F_DUPFD_CLOEXEC : F_DUPFD, 0);
}

static mode_t mode_of(int flags, va_list args) {
    return (flags & (O_CREAT | O_TMPFILE)) ? (mode_t)va_arg(args, int) : 0;
}

#define OPEN(name)                                                                  \
    int name(const char *path, int flags, ...) {                                    \
        static int (*real)(const char *, int, ...);                                 \
        va_list args;                                                               \
        va_start(args, flags);                                                      \
        mode_t mode = mode_of(flags, args);                                         \
        va_end(args);                                                               \
        int fd = leased(path, flags);                                               \
        if (fd >= 0) return fd;                                                     \
        if (!real) real = (int (*)(const char *, int, ...))dlsym(RTLD_NEXT, #name); \
        return real(path, flags, mode);                                             \
    }
#define OPENAT(name)                                                                     \
    int name(int dir, const char *path, int flags, ...) {                                \
        static int (*real)(int, const char *, int, ...);                                 \
        va_list args;                                                                    \
        va_start(args, flags);                                                           \
        mode_t mode = mode_of(flags, args);                                              \
        va_end(args);                                                                    \
        int fd = path && path[0] == '/' ? leased(path, flags) : -1;                      \
        if (fd >= 0) return fd;                                                          \
        if (!real) real = (int (*)(int, const char *, int, ...))dlsym(RTLD_NEXT, #name); \
        return real(dir, path, flags, mode);                                             \
    }
#define OPEN2(name)                                                        \
    int name(const char *path, int flags) {                                \
        static int (*real)(const char *, int);                             \
        int fd = leased(path, flags);                                      \
        if (fd >= 0) return fd;                                            \
        if (!real) real = (int (*)(const char *, int))dlsym(RTLD_NEXT, #name); \
        return real(path, flags);                                          \
    }
OPEN(open)
OPEN(open64)
OPENAT(openat)
OPENAT(openat64)
OPEN2(__open_2)
OPEN2(__open64_2)
"""


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
    with tempfile.TemporaryDirectory() as temp:
        source = Path(temp) / 'lease.c'
        source.write_text(LEASE_SOURCE)
        # Only the open calls are exported: zig links compiler-rt in for armhf, whose builtins must not interpose.
        exports = Path(temp) / 'exports.map'
        exports.write_text('{ global: open; open64; openat; openat64; __open_2; __open64_2; local: *; };\n')
        # In the folders the loaders' $LIB names on RG DS OS (port_runner.lease_shims): one LD_PRELOAD entry then
        # loads the right one in 64-bit and 32-bit programs alike, with no wrong-class warning from either.
        for folder, target in (
            ('lib64', 'aarch64-linux-gnu.2.17'), ('lib/arm-linux-gnueabihf', 'arm-linux-gnueabihf.2.17')
        ):
            out = ROOT / 'runtime/portmaster_files/kms' / folder / 'librgds-drm-lease.so'
            out.parent.mkdir(parents=True, exist_ok=True)
            zig_cc(
                target, '-shared', '-fPIC', '-O2', '-s', '-Wl,--version-script=' + str(exports), '-o', str(out),
                str(source), '-ldl'
            )
            print('kms', folder, out.name, out.stat().st_size, 'bytes')
    return 0


if __name__ == '__main__':
    sys.exit(main())
