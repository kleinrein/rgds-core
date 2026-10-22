#pragma once
#include "platform/hud_style.hpp"
#include <array>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unistd.h>

namespace rgds {
// Presentation cadence, not emulation speed. Count one display only and exclude
// the menu. Fixed histograms keep the frame path allocation- and I/O-free.
class FrameStats {
    std::array<uint64_t, 1001> histogram{};
    uint64_t first = 0, previous = 0, window = 0, published = 0, count = 0, window_count = 0, total_us = 0, slow = 0;
    double rate = 0;

public:
    std::atomic<bool> enabled{false};
    double fps() const { return rate; }
    uint64_t frames() const { return count; }
    void reset() {
        first = previous = window = published = count = window_count = total_us = slow = 0;
        rate = 0;
        histogram.fill(0);
    }
    void pause() {
        previous = window = 0;
        window_count = 0;
    }
    double percentile(double fraction) const {
        if (!count) return 0;
        auto wanted = uint64_t(std::ceil(count * fraction));
        uint64_t sum = 0;
        for (size_t i = 0; i < histogram.size(); i++)
            if ((sum += histogram[i]) >= wanted) return i * .1;
        return 100;
    }
    bool present(uint64_t us, bool blocked) {
        if (!enabled || blocked) {
            pause();
            return false;
        }
        if (!previous) {
            if (!first) first = us;
            previous = window = us;
            return false;
        }
        auto delta = us - previous;
        previous = us;
        ++histogram[std::min<uint64_t>(1000, (delta + 50) / 100)];
        ++count;
        ++window_count;
        total_us += delta;
        if (delta > 20000) ++slow;
        if (us - window < 1000000) return false;
        rate = window_count * 1000000.0 / (us - window);
        window = us;
        window_count = 0;
        published = us;
        return true;
    }
    void write(const std::string& path, double game_fps = -1) const {
        if (path.empty() || !count) return;
        auto pending = path + ".pending";
        auto* file = std::fopen(pending.c_str(), "w");
        if (!file) return;
        int result = std::fprintf(
            file,
            "schema=1\nmetric=presentation\nfirst_present_us=%llu\nfps=%.1f\naverage_fps=%.2f\nframes=%llu\nseconds=%.3f\np50_ms=%.1f\np95_ms=%.1f\np99_ms=%.1f\nover_20ms=%llu\nsample_us=%llu\n",
            (unsigned long long)first, rate, count * 1000000.0 / total_us, (unsigned long long)count,
            total_us / 1000000.0, percentile(.5), percentile(.95), percentile(.99), (unsigned long long)slow,
            (unsigned long long)published);
        if (game_fps >= 0) std::fprintf(file, "game_fps=%.2f\n", game_fps);
        int closed = std::fclose(file);
        if (result > 0 && !closed) std::rename(pending.c_str(), path.c_str());
        else ::unlink(pending.c_str());
    }
};
inline void draw_fps_hud(unsigned char* pixels, double fps, bool game = false) {
    if (hud::fps(pixels, fps, game)) return;
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f FPS", std::clamp(fps, 0.0, 999.9));
    auto glyph = [](char c) -> uint64_t {
        switch (c) {
        case '0': return 0x1f11111111111full;
        case '1': return 0x040c040404040eull;
        case '2': return 0x1e01011e10101full;
        case '3': return 0x1e01010e01011eull;
        case '4': return 0x1111111f010101ull;
        case '5': return 0x1f10101e01011eull;
        case '6': return 0x0f10101e11111full;
        case '7': return 0x1f010204080808ull;
        case '8': return 0x1f11111f11111full;
        case '9': return 0x1f11111f01011eull;
        case 'G': return 0x0f10101711110full;
        case 'A': return 0x0e11111f111111ull;
        case 'M': return 0x111b1511111111ull;
        case 'E': return 0x1f10101e10101full;
        case 'D': return 0x1e11111111111eull;
        case 'I': return 0x0e04040404040eull;
        case 'R': return 0x1e11111e141211ull;
        case ' ': return 0;
        case 'F': return 0x1f10101e101010ull;
        case 'P': return 0x1e11111e101010ull;
        case 'S': return 0x0f10100e01011eull;
        case '.': return 0x00000000000c0cull;
        default: return 0;
        }
    };
    auto put = [&](int x, int y, int r, int g, int b, int alpha = 255) {
        auto* p = pixels + (y * 640 + x) * 4;
        p[0] = (r * alpha + 127) / 255;
        p[1] = (g * alpha + 127) / 255;
        p[2] = (b * alpha + 127) / 255;
        p[3] = alpha;
    };
    for (int y = 24; y < 76; y++)
        for (int x = 24; x < 206; x++) {
            int dx = std::max({31 - x, 0, x - 198}), dy = std::max({31 - y, 0, y - 68});
            if (dx * dx + dy * dy > 49) continue;
            bool edge = dx * dx + dy * dy > 36 || x == 24 || x == 205 || y == 24 || y == 75;
            put(x, y, edge ? 45 : 23, edge ? 67 : 32, edge ? 90 : 46, 245);
        }
    auto label = [&](const char* value, int x, int y, int size, bool accent) {
        for (; *value; ++value, x += 6 * size) {
            auto bits = glyph(*value);
            for (int row = 0; row < 7; row++)
                for (int col = 0; col < 5; col++)
                    if ((bits >> ((6 - row) * 8)) & (1 << (4 - col)))
                        for (int yy = 0; yy < size; yy++)
                            for (int xx = 0; xx < size; xx++)
                                put(x + col * size + xx, y + row * size + yy, accent ? 78 : 233, accent ? 187 : 244,
                                    255);
        }
    };
    label(game ? "RG DS  GAME" : "RG DS  DISP", 36, 33, 1, true);
    label(text, 36, 50, 2, false);
}
}
