#pragma once
#include "platform/bridge_metrics.hpp"
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
namespace rgds {
struct OverlayHeader {
    uint32_t magic, width, height, sequence, visible, slot;
    uint32_t reserved[10];
};
constexpr uint32_t overlay_magic = 0x52474453;
constexpr size_t overlay_panel_bytes = 640 * 480 * 4;
constexpr size_t overlay_bytes = sizeof(OverlayHeader) + overlay_panel_bytes * 4;
// Double-buffered premultiplied RGBA. The UI needs no compositor window.
class OverlayFrame {
    int fd = -1;
    unsigned char* mapping = nullptr;
    bool writer = false;
    uint32_t seen = 0;
    std::vector<unsigned char> pixels, staging;

public:
    ~OverlayFrame() { close(); }
    bool open(const std::string& path, bool write) {
        if (mapping) return true;
        fd = ::open(path.c_str(), O_RDWR | (write ? O_CREAT : 0) | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0) return false;
        struct stat info{};
        if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
            close();
            return false;
        }
        if (write && ftruncate(fd, overlay_bytes) != 0) {
            close();
            return false;
        }
        if (!write && size_t(info.st_size) != overlay_bytes) {
            close();
            return false;
        }
        auto* memory = mmap(nullptr, overlay_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (memory == MAP_FAILED) {
            close();
            return false;
        }
        mapping = static_cast<unsigned char*>(memory);
        writer = write;
        if (write && header()->magic != overlay_magic) {
            std::memset(mapping, 0, overlay_bytes);
            header()->width = 640;
            header()->height = 480;
            header()->magic = overlay_magic;
        }
        return true;
    }
    OverlayHeader* header() { return reinterpret_cast<OverlayHeader*>(mapping); }
    unsigned char* begin() {
        auto* h = header();
        auto seq = __atomic_load_n(&h->sequence, __ATOMIC_ACQUIRE);
        if (seq & 1) seq++;
        __atomic_store_n(&h->sequence, seq + 1, __ATOMIC_RELEASE);
        h->slot = h->slot == 0 ? 1 : 0;
        return mapping + sizeof(OverlayHeader) + h->slot * overlay_panel_bytes * 2;
    }
    void publish() {
        auto* h = header();
        __atomic_store_n(&h->visible, 1u, __ATOMIC_RELEASE);
        __atomic_add_fetch(&h->sequence, 1u, __ATOMIC_RELEASE);
    }
    bool refresh() {
        if (!mapping || header()->magic != overlay_magic || header()->width != 640 || header()->height != 480)
            return false;
        auto* h = header();
        auto seq = __atomic_load_n(&h->sequence, __ATOMIC_ACQUIRE);
        if (!__atomic_load_n(&h->visible, __ATOMIC_ACQUIRE)) return false;
        // A publisher can be halfway through the next frame at presentation
        // time. Keep the last complete frame instead of flashing the game.
        if (!seq || (seq & 1)) return !pixels.empty();
        if (seq != seen) {
            if (staging.size() != overlay_panel_bytes * 2) {
                staging.resize(overlay_panel_bytes * 2);
                RGDS_PROFILE(frame_allocations, 1);
            }
            auto slot = __atomic_load_n(&h->slot, __ATOMIC_ACQUIRE);
            if (slot > 1) return false;
            std::memcpy(staging.data(), mapping + sizeof(OverlayHeader) + slot * overlay_panel_bytes * 2,
                        staging.size());
            RGDS_PROFILE(frame_copies, 1);
            if (__atomic_load_n(&h->sequence, __ATOMIC_ACQUIRE) != seq) return !pixels.empty();
            pixels.swap(staging);
            seen = seq;
        }
        return !pixels.empty();
    }
    const unsigned char* panel(int index) const { return pixels.data() + index * overlay_panel_bytes; }
    uint32_t version() const { return seen; }
    void acknowledge() { __atomic_store_n(&header()->reserved[0], seen, __ATOMIC_RELEASE); }
    void close() {
        if (mapping) {
            if (writer) __atomic_store_n(&header()->visible, 0u, __ATOMIC_RELEASE);
            munmap(mapping, overlay_bytes);
            mapping = nullptr;
        }
        if (fd >= 0) ::close(fd);
        fd = -1;
        seen = 0;
        pixels.clear();
    }
};
}
