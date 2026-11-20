#pragma once
// The player's CPU canvas and console frames, compiled into the launcher for photo mode (photo_mode.hpp).
// Picture frames (PNGs in userdata/frames) are the player's alone: the launcher has no PNG decoder, so
// play::read_png here only says so; photo mode offers the built-in frames.
#include "ui/canvas.cpp"
#include "ui/frame.cpp"

namespace play {
bool read_png(const std::string&, std::vector<uint32_t>&, int&, int&, std::string& error) {
    error = "Picture frames are not offered in photo mode";
    return false;
}
} // namespace play
