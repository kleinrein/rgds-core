#pragma once
// Library zoom: one finger on each screen pinches the library out to a wall of games, and spreading them brings it
// back. Pure state and geometry; the App's drawing and input are in library_zoom_screen.hpp.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace rgds {

// Two fingers, one on each panel, measured in one space: the upper panel above the lower one with the hinge between
// them. Coordinates are each panel's own 640x480 pixels. Kinds are TouchPoint's: 0 down, 1 move, 2 up, 3 cancel.
struct CrossPinch {
    static constexpr int hinge = 120;         // the gap between the panels, in panel pixels
    static constexpr double dead = 16;        // how far the span must change before the fingers are a pinch
    static constexpr double per_level = 1.45; // closing the span by this factor zooms out one level
    bool top = false, bottom = false, engaged = false;
    int top_x = 0, top_y = 0, bottom_x = 0, bottom_y = 0;
    double start = 0;
    bool both() const { return top && bottom; }
    double span() const {
        const double dx = bottom_x - top_x, dy = 480 + hinge + bottom_y - top_y;
        return std::sqrt(dx * dx + dy * dy);
    }
    // True when this event joined the two fingers (the pinch began) or parted them (it ended).
    bool touch(bool upper, int kind, int x, int y) {
        const bool was = both();
        bool& down = upper ? top : bottom;
        if (kind == 0 || kind == 1) {
            if (kind == 0) down = true;
            if (!down) return false;
            (upper ? top_x : bottom_x) = x;
            (upper ? top_y : bottom_y) = y;
        } else down = false;
        if (both() && !was) {
            start = span();
            engaged = false;
        }
        if (both() && !engaged && std::abs(span() - start) >= dead) engaged = true;
        if (!both()) engaged = false;
        return was != both();
    }
    // Levels the fingers ask for since they met: positive as they close in (zoom out), negative as they spread.
    double levels() const {
        if (!both() || !engaged || start <= 0) return 0;
        // Measured from the edge of the dead zone, so the wall starts moving from where it rests.
        const double now = span(), from = now < start ? start - dead : start + dead;
        return std::log(from / std::max(1.0, now)) / std::log(per_level);
    }
};

// The zoom level: 0 is the library exactly as it is, 1 a wall of covers, 2 a dense wall of icons.
// Fractions are the morph between two levels; a little past either end is the rubber band.
struct LibraryZoom {
    static constexpr int deepest = 2;
    static constexpr double settle_ms = 220; // quick, with a small overshoot
    double level = 0, from = 0, base = 0;
    int target = 0;
    uint32_t began = 0;
    bool pinching = false, animating = false;
    static double band(double excess) { return .22 * (1 - 1 / (excess * 2.5 + 1)); }
    bool shown() const { return pinching || animating || level > .0005 || target > 0; }
    // At rest on a wall: its taps, keys and scrolling are live.
    bool resting() const { return !pinching && !animating && target > 0; }
    // Headed for a wall, still settling or not: keys and taps act at once, against where it lands.
    bool live() const { return !pinching && target > 0; }
    void pinch_begin() {
        pinching = true;
        animating = false;
        base = level;
    }
    void pinch(double levels) {
        const double raw = base + levels;
        level = raw < 0 ? -band(-raw) : raw > deepest ? deepest + band(raw - deepest) : raw;
    }
    // Where the fingers left it: the nearest level, or the next one over when the pinch went a fair way toward it.
    int release(uint32_t now, bool animations) {
        pinching = false;
        const int start = int(std::lround(base));
        int to = int(std::lround(std::clamp(level, 0.0, double(deepest))));
        if (to == start && std::abs(level - base) > .22) to = start + (level > base ? 1 : -1);
        go(std::clamp(to, 0, deepest), now, animations);
        return target;
    }
    void go(int to, uint32_t now, bool animations) {
        pinching = false;
        from = level;
        target = std::clamp(to, 0, deepest);
        began = now;
        animating = animations && std::abs(from - target) > .0005;
        if (!animating) level = target;
    }
    void reset() {
        level = from = base = 0;
        target = 0;
        pinching = animating = false;
    }
    // Eases to the target with a small overshoot; true while a repaint is due.
    bool update(uint32_t now) {
        if (!animating) return false;
        const double t = std::min(1.0, (now - began) / settle_ms), c = 1.25, u = t - 1;
        const double ease = 1 + (c + 1) * u * u * u + c * u * u;
        level = from + (target - from) * ease;
        if (t >= 1) {
            level = target;
            animating = false;
        }
        return true;
    }
};

// One wall's grid on the lower panel, below the header card.
struct ZoomWall {
    static constexpr int top = 146, bottom = 474, height = bottom - top;
    int columns = 6, tile = 88, gap = 10;
    static ZoomWall at(int level) { return level <= 1 ? ZoomWall{6, 88, 10} : ZoomWall{10, 50, 6}; }
    int stride() const { return tile + gap; }
    int left() const { return (640 - columns * tile - (columns - 1) * gap) / 2; }
    int rows(size_t count) const { return int((count + size_t(columns) - 1) / size_t(columns)); }
    int maximum(size_t count) const { return std::max(0, rows(count) * stride() - gap - height + 8); }
    int x(size_t index) const { return left() + int(index % size_t(columns)) * stride(); }
    int y(size_t index, int scroll) const { return top + 4 + int(index / size_t(columns)) * stride() - scroll; }
    // The scroll that puts this game's row in the middle of the wall.
    int centred(size_t index, size_t count) const {
        const int row = int(index / size_t(columns)) * stride();
        return std::clamp(row + tile / 2 - height / 2, 0, maximum(count));
    }
    // The smallest scroll change that shows this game's whole row.
    int revealed(size_t index, size_t count, int scroll) const {
        const int row = int(index / size_t(columns)) * stride() + 4;
        if (row - 4 < scroll) scroll = row - 4;
        if (row + tile + 4 > scroll + height) scroll = row + tile + 4 - height;
        return std::clamp(scroll, 0, maximum(count));
    }
    // The game under a point, or -1 (gaps between tiles count as the nearest tile, so taps never fall through).
    int hit(int px, int py, int scroll, size_t count) const {
        if (py < top || py >= bottom || px < left() - gap / 2 || px >= 640 - left() + gap / 2) return -1;
        const int column = std::clamp((px - left() + gap / 2) / stride(), 0, columns - 1),
                  row = std::max(0, (py - top - 4 + scroll + gap / 2) / stride());
        const size_t index = size_t(row) * size_t(columns) + size_t(column);
        return index < count ? int(index) : -1;
    }
    int first(int scroll) const { return std::max(0, (scroll - 4) / stride()) * columns; }
    int last(int scroll, size_t count) const {
        return std::min(int(count), ((scroll + height) / stride() + 2) * columns);
    }
};

}
