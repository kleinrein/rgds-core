// SPDX-License-Identifier: GPL-3.0-or-later
// Display-only sampling for the RG DS 640x480 panel. Guest pixels are untouched.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>

namespace rgds {
struct SamplePlan {
    static constexpr uint16_t absent = 0xffff;
    struct Tap { uint16_t first, second; };
    std::array<std::array<Tap, 256>, 3> phase{};
    unsigned key = ~0u;

    // DS HOFS wraps at 512 native pixels; the second half is transparent.
    // Keeping that hole in the plan avoids a modulus and two bounds checks
    // per output sample. Most games keep the same offset across whole frames.
    void update(unsigned scale, unsigned offset, bool smooth) {
        offset &= 511;
        const unsigned next = scale | (offset << 3) | (unsigned(smooth) << 12);
        if (key == next) return;
        key = next;
        for (unsigned p = 0; p < 3; ++p) for (unsigned x = 0; x < 256; ++x) {
            const unsigned begin = x * 5 / 2, count = (x + 1) * 5 / 2 - begin;
            unsigned a, b;
            if (smooth) {
                const unsigned dx = begin + std::min(p, count - 1);
                a = (4 * dx + 1) * scale / 10 + offset * scale;
                b = (4 * dx + 3) * scale / 10 + offset * scale;
                if (a >= 512 * scale) a -= 512 * scale;
                if (b >= 512 * scale) b -= 512 * scale;
            } else {
                const unsigned sx = (x + offset) & 511;
                a = b = sx * scale + std::min(scale - 1, (2 * p + 1) * scale / (2 * count));
            }
            phase[p][x] = {a < 256 * scale ? uint16_t(a) : absent,
                           b < 256 * scale ? uint16_t(b) : absent};
        }
    }
};
}
