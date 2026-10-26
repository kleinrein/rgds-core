// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/colour.hpp"
#include "ui/canvas.hpp"
#include "ui/gb_palettes.hpp"

#include <algorithm>
#include <cmath>

namespace play {
namespace {

// The Game Boy Color's screen mixes its channels (blue leaks into red and
// green, red and green into blue) and is dimmer than its raw values say. The
// weights are the ones the Gambatte emulator made common; each row sums to
// one, so white stays white.
Rgb correct(Rgb in, bool dim) {
    const int r = in.r, g = in.g, b = in.b;
    int out[3] = {(13 * r + 2 * g + b) / 16, (3 * g + b) / 4, (3 * r + 2 * g + 11 * b) / 16};
    for (int& v : out) {
        if (dim) v = int(std::lround(255.0 * std::pow(v / 255.0, 1.25)));
        v = std::clamp(v, 0, 255);
    }
    return {uint8_t(out[0]), uint8_t(out[1]), uint8_t(out[2])};
}

} // namespace

int palette_count() { return gb_palette_count; }
int correction_count() { return gb_correction_count; }
const char* palette_slug(int index) { return gb_palettes[std::clamp(index, 0, gb_palette_count - 1)].slug; }
const char* correction_slug(int index) { return gb_corrections[std::clamp(index, 0, gb_correction_count - 1)].slug; }
int parse_palette(const std::string& slug) {
    for (int i = 0; i < palette_count(); ++i)
        if (slug == gb_palettes[i].slug) return i;
    return 0;
}
int parse_correction(const std::string& slug) {
    for (int i = 0; i < correction_count(); ++i)
        if (slug == gb_corrections[i].slug) return i;
    return 0;
}

void Colour::set(Core& core, int palette, int correction) {
    const bool colour_hardware = core.colour_hardware();
    core.set_gbc_colours(!colour_hardware && std::clamp(palette, 0, palette_count() - 1) == gb_palette_gbc);
    set(core.system(), colour_hardware, palette, correction);
}

void Colour::set(const std::string& system, bool colour_hardware, int palette, int correction) {
    palette_ = std::clamp(palette, 0, palette_count() - 1);
    correction_ = std::clamp(correction, 0, correction_count() - 1);
    mode_ = Mode::None;
    const bool colour_game = system == "gbc" || (system == "gb" && colour_hardware);
    // An original game in the Game Boy Color's colours is seen through that handheld's screen too.
    const int screen = colour_game ? correction_ : system == "gb" && palette_ == gb_palette_gbc ? 0 : -1;
    if (system == "gb" && !colour_game && palette_ != gb_palette_gbc && palette_ != gb_palette_greys) {
        mode_ = Mode::Palette;
        for (int i = 0; i < 4; ++i) shades_[i] = Rgb::hex(gb_palettes[palette_].shade[i]).px();
    } else if (screen >= 0 && screen != gb_correction_vivid) {
        mode_ = Mode::Correction;
        lut_.resize(32768);
        for (uint32_t key = 0; key < 32768; ++key) {
            // Back to eight bits the way a full-range five-bit channel expands (31 -> 255).
            auto wide = [](uint32_t v) { return uint8_t((v << 3) | (v >> 2)); };
            lut_[key] = correct({wide(key & 31), wide((key >> 5) & 31), wide(key >> 10)}, screen == 1).px();
        }
    }
}

Picture Colour::apply(const Picture& in) {
    if (mode_ == Mode::None || !in.px || in.w <= 0 || in.h <= 0) return in;
    out_.resize(static_cast<size_t>(in.w) * in.h);
    for (int y = 0; y < in.h; ++y) {
        const uint32_t* src = in.px + static_cast<size_t>(y) * in.stride;
        uint32_t* dst = out_.data() + static_cast<size_t>(y) * in.w;
        if (mode_ == Mode::Palette) {
            for (int x = 0; x < in.w; ++x) {
                const uint32_t v = src[x];
                const uint32_t r = v & 0xFF, g = (v >> 8) & 0xFF, b = (v >> 16) & 0xFF;
                // Only the four greys (anything else is not an original Game Boy picture).
                if (r != g || g != b) {
                    dst[x] = v;
                    continue;
                }
                dst[x] = shades_[r >= 212 ? 0 : r >= 125 ? 1 : r >= 41 ? 2 : 3];
            }
        } else {
            for (int x = 0; x < in.w; ++x) {
                const uint32_t v = src[x];
                dst[x] = lut_[((v & 0xFF) >> 3) | (((v >> 8) & 0xFF) >> 3) << 5 | (((v >> 16) & 0xFF) >> 3) << 10];
            }
        }
    }
    return {out_.data(), in.w, in.h, in.w};
}

} // namespace play
