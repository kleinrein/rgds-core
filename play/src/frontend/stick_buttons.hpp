// SPDX-License-Identifier: GPL-3.0-or-later
// The analog sticks as buttons for systems whose pad has none (GBA, NES, Mega Drive, ...): each direction presses
// the button Controls gives it (controls.ini's stick_* keys; by default the left stick is the D-pad). Systems with a
// stick of their own (N64, PSP, the Atari 5200's analog joystick), ScummVM's pointer, the Amiga's mouse and DOS (DOSBox Pure maps them per game: a
// joystick, the mouse, keys) keep the sticks as sticks.
#pragma once
#include "core/core.hpp"
#include "platform/control_mapping.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace play {

inline bool system_has_stick(const std::string& system) {
    return system == "n64" || system == "psp" || system == "dc" || system == "scummvm" || system == "atari5200" ||
           system == "amiga" ||                  // the Amiga's right stick: its mouse
           system == "dos" || system == "flash"; // Flash: the stick moves the pointer
}

class StickButtons {
public:
    void set_targets(const std::array<int, 8>& targets) { targets_ = targets; }
    const std::array<int, 8>& targets() const { return targets_; }

    // The buttons the sticks press now; axes are left x, y, right x, y with right and down positive (SDL's).
    uint32_t update(int16_t lx, int16_t ly, int16_t rx, int16_t ry) {
        const int along[8] = {-ly, ly, -lx, lx, -ry, ry, -rx, rx};
        uint32_t bits = 0;
        for (int d = 0; d < 8; ++d) {
            held_[d] = rgds::stick_direction_down(along[d], held_[d]);
            if (held_[d]) bits |= bit(targets_[d]);
        }
        return bits;
    }

    // A logical button of control_mapping.hpp (a, b, x, y, l, r, start, select, up, down, left, right) as a Button.
    static uint32_t bit(int logical) {
        static constexpr uint32_t bits[12] = {A, B, X, Y, L, R, Start, Select, Up, Down, Left, Right};
        return logical >= 0 && logical < 12 ? bits[logical] : 0;
    }

private:
    std::array<int, 8> targets_ = rgds::default_stick;
    std::array<bool, 8> held_{};
};

} // namespace play
