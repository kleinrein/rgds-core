// SPDX-License-Identifier: GPL-3.0-or-later
// Which Spectrum a tape plays on. Fuse's own choice (fuse_auto_machine) takes a 128K for any tape with "128" in its
// name, and so for one named for both machines: "Out Run (1988)(U.S. Gold)[48-128K][SpeedLock 4]" got a 128K, its
// loader reset it, and the game never started (BASIC's copyright line and nothing else). A tape that runs on a 48K
// gets one.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace play {

// "128" in "Army Moves - 128k", "[48-128K]" or "(128K)", not in "1280 bytes free": the number standing by itself.
inline bool zx_name_has_number(const std::string& name, const std::string& number) {
    auto digit = [](char c) { return c >= '0' && c <= '9'; };
    for (size_t at = name.find(number); at != std::string::npos; at = name.find(number, at + 1)) {
        const size_t end = at + number.size();
        if ((at == 0 || !digit(name[at - 1])) && (end >= name.size() || !digit(name[end]))) return true;
    }
    return false;
}

// A TZX's hardware information (block 33) saying the tape does not run on a 16K or 48K, or uses a 128K's, +2's or
// +3's own hardware. A "stop the tape if in 48K mode" block (2A) is no reason: that tape has a 48K game on it.
inline bool zx_tzx_needs_128k(const std::vector<uint8_t>& tape) {
    static const uint8_t signature[8] = {'Z', 'X', 'T', 'a', 'p', 'e', '!', 0x1a};
    const size_t size = tape.size();
    if (size < 10) return false;
    for (size_t i = 0; i < 8; ++i)
        if (tape[i] != signature[i]) return false;
    size_t pos = 10;
    auto number = [&](size_t offset, int bytes) -> size_t { // little-endian; a tape cut short reads as none
        size_t value = 0;
        for (int i = bytes - 1; i >= 0; --i)
            value = (value << 8) | (pos + offset + i < size ? tape[pos + offset + i] : 0);
        return value;
    };
    while (pos < size) {
        const uint8_t id = tape[pos++];
        size_t length = 0;
        switch (id) {
        case 0x10: length = 4 + number(2, 2); break;
        case 0x11: length = 18 + number(15, 3); break;
        case 0x12: length = 4; break;
        case 0x13: length = 1 + 2 * number(0, 1); break;
        case 0x14: length = 10 + number(7, 3); break;
        case 0x15: length = 8 + number(5, 3); break;
        case 0x20: length = 2; break;
        case 0x21: length = 1 + number(0, 1); break;
        case 0x22: length = 0; break;
        case 0x23: length = 2; break;
        case 0x24: length = 2; break;
        case 0x25: length = 0; break;
        case 0x26: length = 2 + 2 * number(0, 2); break;
        case 0x27: length = 0; break;
        case 0x28: length = 2 + number(0, 2); break;
        case 0x2A: length = 4; break;
        case 0x30: length = 1 + number(0, 1); break;
        case 0x31: length = 2 + number(1, 1); break;
        case 0x32: length = 2 + number(0, 2); break;
        case 0x33: length = 1 + 3 * number(0, 1); break;
        case 0x35: length = 20 + number(16, 4); break;
        case 0x5A: length = 9; break;
        default: length = 4 + number(0, 4); break; // 18, 19, 2B and blocks yet to come: a length first
        }
        if (length > size - pos) return false;
        if (id == 0x33) {
            for (size_t i = 0, count = number(0, 1); i < count; ++i) {
                const size_t type = number(1 + 3 * i, 1), machine = number(2 + 3 * i, 1), value = number(3 + 3 * i, 1);
                if (type != 0) continue;                                     // 0: computers
                if (machine <= 2 && value == 3) return true;                 // 16K, 48K: does not run
                if (machine >= 3 && machine <= 5 && value == 1) return true; // 128K, +2, +2A/+3: uses its hardware
            }
        }
        pos += length;
    }
    return false;
}

// A 128K for a tape named for one and not also for a 48K, or whose own header asks for one.
inline bool zx_tape_needs_128k(const std::string& path, const std::vector<uint8_t>& tape) {
    const size_t slash = path.find_last_of("/\\");
    const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    if (zx_name_has_number(name, "128") && !zx_name_has_number(name, "48")) return true;
    return zx_tzx_needs_128k(tape);
}

} // namespace play
