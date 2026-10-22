#pragma once
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>

namespace rgds {
// A bounded structural check, not a substitute for the emulator's state loader.
// The reviewed v4 writer appends VIEW after every core chunk and may append CHVO.
// Reject truncated writes even if fclose failed after a successful fwrite.
inline bool complete_dsperate_state(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    const auto end = file.tellg();
    if (end < 8 || end > 128 * 1024 * 1024) return false;
    const auto size = static_cast<std::uint64_t>(end);
    auto word = [](const unsigned char* b) {
        return std::uint32_t(b[0]) | std::uint32_t(b[1]) << 8 | std::uint32_t(b[2]) << 16 | std::uint32_t(b[3]) << 24;
    };
    unsigned char header[8]{};
    file.seekg(0);
    file.read(reinterpret_cast<char*>(header), 8);
    if (!file || std::memcmp(header, "DSST", 4) || word(header + 4) != 4) return false;
    std::uint64_t offset = 8;
    bool head = false, view = false;
    for (unsigned chunks = 0; offset < size && chunks < 128; ++chunks) {
        if (size - offset < 8) return false;
        file.seekg(static_cast<std::streamoff>(offset));
        file.read(reinterpret_cast<char*>(header), 8);
        const auto length = word(header + 4);
        if (!file || length > size - offset - 8) return false;
        if (!head) {
            if (std::memcmp(header, "HEAD", 4) || length < 48) return false;
            head = true;
        } else if (!std::memcmp(header, "VIEW", 4)) {
            if (view || length != 19) return false;
            view = true;
        } else if (view && (std::memcmp(header, "CHVO", 4) || length < 8 || offset + 8 + length != size)) return false;
        offset += 8 + length;
    }
    return offset == size && head && view;
}
}
