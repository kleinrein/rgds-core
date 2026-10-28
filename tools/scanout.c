// What the RG DS panels really show: every plane being scanned out (after
// Weston's composition), with its CRTC, framebuffer format and position, and a
// BMP of each linear 32-bit framebuffer. Renderer readbacks and the bridge
// capture read an app's own buffers; this reads the display controller.
// Root reads the framebuffer handles while Weston is DRM master.
//
//   zig cc -target aarch64-linux-gnu.2.17 -O2 -o build/scanout tools/scanout.c
//   scp build/scanout root@anbernic.local:/tmp/ && ssh root@anbernic.local /tmp/scanout /tmp/scan
//
// On the RG DS, CRTC 130 is the top panel (DSI-1) and CRTC 90 the bottom one.
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <drm/drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static int fd;
static int io(unsigned long req, void* arg) {
    int r;
    do r = ioctl(fd, req, arg);
    while (r == -1 && (errno == EINTR || errno == EAGAIN));
    return r;
}
static uint64_t prop(uint32_t obj, uint32_t type, const char* want) {
    struct drm_mode_obj_get_properties gp = {.obj_id = obj, .obj_type = type};
    if (io(DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &gp)) return ~0ull;
    uint32_t* ids = calloc(gp.count_props, 4);
    uint64_t* vals = calloc(gp.count_props, 8);
    gp.props_ptr = (uintptr_t)ids;
    gp.prop_values_ptr = (uintptr_t)vals;
    uint64_t out = ~0ull;
    if (!io(DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &gp))
        for (uint32_t i = 0; i < gp.count_props; i++) {
            struct drm_mode_get_property p = {.prop_id = ids[i]};
            if (!io(DRM_IOCTL_MODE_GETPROPERTY, &p) && !strcmp(p.name, want)) out = vals[i];
        }
    free(ids);
    free(vals);
    return out;
}
static void bmp(const char* path, const uint8_t* px, int w, int h, int pitch) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    uint32_t size = 54 + (uint32_t)w * h * 4;
    uint8_t hdr[54] = {'B', 'M'};
    memcpy(hdr + 2, &size, 4);
    uint32_t v = 54;
    memcpy(hdr + 10, &v, 4);
    v = 40;
    memcpy(hdr + 14, &v, 4);
    int32_t sw = w, sh = -h;
    memcpy(hdr + 18, &sw, 4);
    memcpy(hdr + 22, &sh, 4);
    uint16_t planes = 1, bits = 32;
    memcpy(hdr + 26, &planes, 2);
    memcpy(hdr + 28, &bits, 2);
    fwrite(hdr, 1, 54, f);
    for (int y = 0; y < h; y++) fwrite(px + (size_t)y * pitch, 1, (size_t)w * 4, f);
    fclose(f);
}
int main(int argc, char** argv) {
    const char* out = argc > 1 ? argv[1] : "/tmp/scanout";
    fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) fd = open("/dev/dri/card1", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror("open");
        return 1;
    }
    struct drm_set_client_cap cap = {DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1};
    io(DRM_IOCTL_SET_CLIENT_CAP, &cap);
    struct drm_mode_get_plane_res res = {0};
    io(DRM_IOCTL_MODE_GETPLANERESOURCES, &res);
    uint32_t* planes = calloc(res.count_planes, 4);
    res.plane_id_ptr = (uintptr_t)planes;
    io(DRM_IOCTL_MODE_GETPLANERESOURCES, &res);
    for (uint32_t i = 0; i < res.count_planes; i++) {
        struct drm_mode_get_plane p = {.plane_id = planes[i]};
        if (io(DRM_IOCTL_MODE_GETPLANE, &p) || !p.fb_id) continue;
        struct drm_mode_fb_cmd2 fb = {.fb_id = p.fb_id};
        int got = io(DRM_IOCTL_MODE_GETFB2, &fb);
        uint64_t cx = prop(p.plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X"),
                 cy = prop(p.plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y"),
                 cw = prop(p.plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W"),
                 ch = prop(p.plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H"),
                 z = prop(p.plane_id, DRM_MODE_OBJECT_PLANE, "zpos");
        char four[5] = {0};
        memcpy(four, &fb.pixel_format, 4);
        printf("plane %u crtc %u fb %u %ux%u %s mod %llx pitch %u at %lld,%lld %lldx%lld zpos %lld%s\n", p.plane_id,
               p.crtc_id, p.fb_id, fb.width, fb.height, four, (unsigned long long)fb.modifier[0], fb.pitches[0],
               (long long)cx, (long long)cy, (long long)cw, (long long)ch, (long long)z, got ? " (no handle)" : "");
        if (got || !fb.handles[0]) continue;
        if (fb.modifier[0] != 0 && fb.modifier[0] != DRM_FORMAT_MOD_INVALID && !(fb.flags & 0)) {
            if (fb.flags & DRM_MODE_FB_MODIFIERS) {
                printf("  compressed/tiled, not dumped\n");
                continue;
            }
        }
        if (fb.pixel_format != DRM_FORMAT_XRGB8888 && fb.pixel_format != DRM_FORMAT_ARGB8888 &&
            fb.pixel_format != DRM_FORMAT_XBGR8888 && fb.pixel_format != DRM_FORMAT_ABGR8888) {
            printf("  format not dumped\n");
            continue;
        }
        struct drm_prime_handle prime = {.handle = fb.handles[0], .flags = DRM_CLOEXEC | DRM_RDWR};
        if (io(DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime)) {
            perror("  prime");
            continue;
        }
        size_t size = (size_t)fb.pitches[0] * fb.height + fb.offsets[0];
        uint8_t* map = mmap(NULL, size, PROT_READ, MAP_SHARED, prime.fd, 0);
        if (map == MAP_FAILED) {
            perror("  mmap");
            close(prime.fd);
            continue;
        }
        uint8_t* copy = malloc(size);
        memcpy(copy, map, size);
        if (fb.pixel_format == DRM_FORMAT_XBGR8888 || fb.pixel_format == DRM_FORMAT_ABGR8888)
            for (size_t k = fb.offsets[0]; k + 3 < size; k += 4) {
                uint8_t t = copy[k];
                copy[k] = copy[k + 2];
                copy[k + 2] = t;
            }
        char path[256];
        snprintf(path, sizeof path, "%s-crtc%u-plane%u.bmp", out, p.crtc_id, p.plane_id);
        bmp(path, copy + fb.offsets[0], fb.width, fb.height, fb.pitches[0]);
        printf("  wrote %s\n", path);
        free(copy);
        munmap(map, size);
        close(prime.fd);
        struct drm_gem_close gc = {.handle = fb.handles[0]};
        io(DRM_IOCTL_GEM_CLOSE, &gc);
    }
    return 0;
}
