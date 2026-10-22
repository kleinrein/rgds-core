#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
namespace rgds {
// The DSi scroll bar: square arrow buttons, a recessed tick track between them
// and a glass grabber over the ticks, all `top` px down the bottom panel's rail.
struct RailGeometry {
    // `control` is the square end button, `grab` the wider glass grabber.
    static constexpr int control = 64, grab = 64, top = 424, height = 56, left = 64, width = 512;
    int window_left, window_width, thumb_left, thumb_width, marker;
    static RailGeometry at(size_t count, double position) {
        double fraction = count > 1 ? std::clamp(position / (count - 1), 0.0, 1.0) : .5;
        int thumb = grab;
        int thumb_x = left + int(std::lround(fraction * (width - thumb))); // flush with the arrow blocks at the ends
        int window = std::clamp(width * 3 / std::max(1, int(count)), thumb + 8, width);
        int window_x = std::clamp(thumb_x + thumb / 2 - window / 2, left, left + width - window);
        return {window_x, window, thumb_x, thumb, left + int(std::lround(fraction * (width - 4)))};
    }
};
// Interruptible press/release easing. The control stays still once settled.
struct RailPress {
    bool held = false;
    double from = 0;
    unsigned began = 0;
    double value(unsigned now, bool animations = true) const {
        if (!animations) return held ? 1 : 0;
        double t = std::min(1.0, (now - began) / (held ? 100.0 : 180.0));
        return from + ((held ? 1.0 : 0.0) - from) * (1 - std::pow(1 - t, 3));
    }
    bool set(bool next, unsigned now, bool animations) {
        if (next == held) return false;
        from = value(now, animations);
        held = next;
        began = now;
        if (held) from = std::max(.18, from); // Visible on the very first touch frame.
        return true;
    }
    bool animating(unsigned now, bool animations) const {
        return animations && now - began < (held ? 100u : 180u) && std::abs(from - (held ? 1 : 0)) > .001;
    }
};
struct Carousel {
    double position = 0, from = 0, target = 0;
    unsigned began = 0;
    bool moving = false, dragging = false, moved = false, range = false;
    int start_x = 0, last_x = 0, start_y = 0;
    double range_left = 106;
    int range_width = 428;
    double start_position = 0;
    static constexpr double spacing = 136;
    double item_spacing = spacing;
    void reset(size_t selected) {
        position = from = target = static_cast<double>(selected);
        moving = false;
    }
    void select(size_t selected, unsigned now) {
        from = position;
        target = static_cast<double>(selected);
        began = now;
        moving = std::abs(from - target) > 0.001;
    }
    bool update(unsigned now) {
        if (!moving) return false;
        double t = std::min(1.0, (now - began) / 190.0);
        position = from + (target - from) * (1 - std::pow(1 - t, 3));
        if (t >= 1) {
            position = target;
            moving = false;
        }
        return true;
    }
    void sync_input(unsigned now) {
        if (!dragging) update(now);
    }
    void down(int x, int y, bool seek, unsigned now, int seek_left = 106, int seek_width = 428) {
        // Input can arrive after a stalled frame. Advance wall-clock animation
        // before freezing its position for hit testing or the start of a drag.
        sync_input(now);
        dragging = true;
        moved = false;
        range = seek;
        range_left = seek_left;
        range_width = std::max(1, seek_width);
        start_x = last_x = x;
        start_y = y;
        start_position = position;
        moving = false;
    }
    void seek_down(int x, int y, size_t count, unsigned now) {
        sync_input(now);
        const auto rail = RailGeometry::at(count, position);
        const int travel = RailGeometry::width - rail.thumb_width;
        // Retain the exact grab point, including subpixel position, so touching
        // either edge of the thumb does not change the current selection.
        const bool grabbed = x >= rail.thumb_left && x < rail.thumb_left + rail.thumb_width;
        down(x, y, true, now, RailGeometry::left + rail.thumb_width / 2, travel);
        if (grabbed) range_left = x - (count > 1 ? position / (count - 1) : 0) * travel;
    }
    size_t drag(int x, size_t count) {
        last_x = x;
        moved = moved || std::abs(x - start_x) > 8;
        if (!count) return 0;
        position = range
                       ? std::clamp((x - range_left) / double(range_width), 0.0, 1.0) * (count - 1)
                       : std::clamp(start_position + (start_x - x) / item_spacing, 0.0, static_cast<double>(count - 1));
        return static_cast<size_t>(std::llround(position));
    }
    size_t tapped(int x, size_t count) const {
        if (!count) return 0;
        auto index = static_cast<long>(std::llround(position + (x - 320) / item_spacing));
        return static_cast<size_t>(std::clamp(index, 0L, static_cast<long>(count) - 1));
    }
};
}
