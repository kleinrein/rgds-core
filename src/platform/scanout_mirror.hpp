#pragma once
// The top panel's picture, read from the display controller, for the port touch pad on the
// bottom panel (tools/scanout.c reads the same way). Root may read the framebuffer handles of
// the planes Weston scans out; the top panel's topmost plane is linear XRGB8888 on the RG DS
// and copying a 640x480 frame from it took 0.17 ms there. Compressed or tiled framebuffers
// (an AFBC modifier) cannot be read this way, and grab() then returns false.
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#ifdef __linux__
#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#endif
namespace rgds {
struct ScanoutMirror {
    int fd = -1;
    uint32_t crtc = 0;
    int width = 0, height = 0;
    bool bgr = false;            // XBGR8888 rather than XRGB8888
    std::vector<uint8_t> pixels; // width * height * 4, rows packed
    bool failed = false;

    // The CRTC driving the connector called name ("DSI-1" is the top panel).
    bool open(const char* name = "DSI-1") {
#ifdef __linux__
        if (fd >= 0) return true;
        if (failed) return false;
        for (const char* path : {"/dev/dri/card0", "/dev/dri/card1"})
            if ((fd = ::open(path, O_RDWR | O_CLOEXEC)) >= 0) break;
        if (fd < 0) return fail();
        drm_set_client_cap cap{DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1};
        io(DRM_IOCTL_SET_CLIENT_CAP, &cap);
        drm_mode_card_res res{};
        if (io(DRM_IOCTL_MODE_GETRESOURCES, &res)) return fail();
        std::vector<uint32_t> connectors(res.count_connectors), crtcs(res.count_crtcs), encoders(res.count_encoders),
            fbs(res.count_fbs);
        res.connector_id_ptr = uintptr_t(connectors.data());
        res.crtc_id_ptr = uintptr_t(crtcs.data());
        res.encoder_id_ptr = uintptr_t(encoders.data());
        res.fb_id_ptr = uintptr_t(fbs.data());
        if (io(DRM_IOCTL_MODE_GETRESOURCES, &res)) return fail();
        static const char* const types[] = {"Unknown", "VGA",  "DVI-I",     "DVI-D",   "DVI-A", "Composite",
                                            "SVIDEO",  "LVDS", "Component", "DIN",     "DP",    "HDMI-A",
                                            "HDMI-B",  "TV",   "eDP",       "Virtual", "DSI"};
        for (uint32_t id : connectors) {
            drm_mode_get_connector connector{};
            connector.connector_id = id;
            if (io(DRM_IOCTL_MODE_GETCONNECTOR, &connector) || !connector.encoder_id) continue;
            char label[32];
            const char* type =
                connector.connector_type < sizeof types / sizeof *types ? types[connector.connector_type] : "Unknown";
            snprintf(label, sizeof label, "%s-%u", type, connector.connector_type_id);
            if (std::strcmp(label, name) != 0) continue;
            drm_mode_get_encoder encoder{};
            encoder.encoder_id = connector.encoder_id;
            if (!io(DRM_IOCTL_MODE_GETENCODER, &encoder)) crtc = encoder.crtc_id;
        }
        return crtc ? true : fail();
#else
        (void)name;
        return false;
#endif
    }

    // Copies the CRTC's topmost readable full-screen plane into pixels. False when there is none.
    bool grab() {
#ifdef __linux__
        if (fd < 0 || !crtc) return false;
        drm_mode_get_plane_res res{};
        if (io(DRM_IOCTL_MODE_GETPLANERESOURCES, &res)) return false;
        std::vector<uint32_t> planes(res.count_planes);
        res.plane_id_ptr = uintptr_t(planes.data());
        if (io(DRM_IOCTL_MODE_GETPLANERESOURCES, &res)) return false;
        uint32_t best_fb = 0;
        uint64_t best_z = 0;
        bool found = false;
        for (uint32_t id : planes) {
            drm_mode_get_plane plane{};
            plane.plane_id = id;
            if (io(DRM_IOCTL_MODE_GETPLANE, &plane) || !plane.fb_id || plane.crtc_id != crtc) continue;
            const uint64_t z = property(id, "zpos");
            if (!found || z >= best_z) {
                best_fb = plane.fb_id;
                best_z = z;
                found = true;
            }
        }
        if (!found) return false;
        drm_mode_fb_cmd2 fb{};
        fb.fb_id = best_fb;
        if (io(DRM_IOCTL_MODE_GETFB2, &fb) || !fb.handles[0]) return false;
        bool ok = false;
        const bool linear = !(fb.flags & DRM_MODE_FB_MODIFIERS) || fb.modifier[0] == DRM_FORMAT_MOD_LINEAR;
        const bool rgb = fb.pixel_format == DRM_FORMAT_XRGB8888 || fb.pixel_format == DRM_FORMAT_ARGB8888;
        const bool brg = fb.pixel_format == DRM_FORMAT_XBGR8888 || fb.pixel_format == DRM_FORMAT_ABGR8888;
        if (linear && (rgb || brg) && fb.width > 0 && fb.width <= 4096 && fb.height > 0 && fb.height <= 4096) {
            drm_prime_handle prime{};
            prime.handle = fb.handles[0];
            prime.flags = DRM_CLOEXEC | DRM_RDWR;
            if (!io(DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime)) {
                const size_t size = size_t(fb.pitches[0]) * fb.height + fb.offsets[0];
                void* map = mmap(nullptr, size, PROT_READ, MAP_SHARED, prime.fd, 0);
                if (map != MAP_FAILED) {
                    width = int(fb.width);
                    height = int(fb.height);
                    bgr = brg;
                    pixels.resize(size_t(width) * height * 4);
                    const auto* source = static_cast<const uint8_t*>(map) + fb.offsets[0];
                    for (int row = 0; row < height; row++)
                        std::memcpy(pixels.data() + size_t(row) * width * 4, source + size_t(row) * fb.pitches[0],
                                    size_t(width) * 4);
                    munmap(map, size);
                    ok = true;
                }
                ::close(prime.fd);
            }
        }
        drm_gem_close close_handle{};
        close_handle.handle = fb.handles[0];
        io(DRM_IOCTL_GEM_CLOSE, &close_handle);
        return ok;
#else
        return false;
#endif
    }

    void close() {
        if (fd >= 0) ::close(fd);
        fd = -1;
        crtc = 0;
    }
    ~ScanoutMirror() { close(); }

private:
    bool fail() {
        close();
        failed = true;
        return false;
    }
#ifdef __linux__
    int io(unsigned long request, void* argument) {
        int result;
        do result = ioctl(fd, request, argument);
        while (result == -1 && (errno == EINTR || errno == EAGAIN));
        return result;
    }
    uint64_t property(uint32_t object, const char* want) {
        drm_mode_obj_get_properties list{};
        list.obj_id = object;
        list.obj_type = DRM_MODE_OBJECT_PLANE;
        if (io(DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &list)) return 0;
        std::vector<uint32_t> ids(list.count_props);
        std::vector<uint64_t> values(list.count_props);
        list.props_ptr = uintptr_t(ids.data());
        list.prop_values_ptr = uintptr_t(values.data());
        if (io(DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &list)) return 0;
        for (size_t i = 0; i < ids.size(); i++) {
            drm_mode_get_property prop{};
            prop.prop_id = ids[i];
            if (!io(DRM_IOCTL_MODE_GETPROPERTY, &prop) && !std::strcmp(prop.name, want)) return values[i];
        }
        return 0;
    }
#endif
};
} // namespace rgds
