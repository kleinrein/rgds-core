#pragma once
#include <array>
#include <cstdint>
#include <SDL.h>

namespace rgds {
// Validate dimensions before SDL allocates its decoded surface. Our texture
// LRU cannot prevent an oversized downloaded/imported BMP from exhausting RAM
// during decoding. Rewind this same stream for SDL on success.
inline bool bmp_within_budget(SDL_RWops* file, uint64_t byte_limit) {
    std::array<unsigned char, 54> bytes{};
    auto read = file->read(file, bytes.data(), 1, bytes.size());
    auto u16 = [&](int n) { return unsigned(bytes[n]) | (unsigned(bytes[n + 1]) << 8); };
    auto u32 = [&](int n) { return uint32_t(u16(n)) | (uint32_t(u16(n + 2)) << 16); };
    if (read < 26 || bytes[0] != 'B' || bytes[1] != 'M') return false;
    uint32_t dib = u32(14);
    int64_t width = 0, height = 0;
    unsigned planes = 0, depth = 0;
    if (dib == 12) {
        width = u16(18);
        height = u16(20);
        planes = u16(22);
        depth = u16(24);
    } else if (read == bytes.size() && (dib == 40 || dib == 52 || dib == 56 || dib == 108 || dib == 124)) {
        width = int32_t(u32(18));
        height = int32_t(u32(22));
        planes = u16(26);
        depth = u16(28);
        if (height < 0) height = -height;
    } else return false;
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096 || planes != 1) return false;
    if (depth != 1 && depth != 4 && depth != 8 && depth != 16 && depth != 24 && depth != 32) return false;
    if (uint64_t(width) * uint64_t(height) > byte_limit / 4) return false;
    return file->seek(file, 0, RW_SEEK_SET) == 0;
}
}
