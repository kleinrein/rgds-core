#pragma once
// The music player's symbols (play, pause, shuffle, a heart, a note...), each described once as a shape in a
// 24 x 24 box. The rounded icon style draws them smooth at any size; the pixel icon style reads the same shapes on
// a 12 x 12 grid, so the two styles can never drift apart. No SDL: the launcher turns the coverage into textures.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace rgds {
enum class MusicGlyph {
    Play,
    Pause,
    Next,
    Previous,
    Shuffle,
    Repeat,
    RepeatOne,
    Queue,
    Equaliser,
    Bars,
    Heart,
    HeartLine,
    Note,
    Folder,
    Disc,
    Person,
    List,
    Speaker,
    Bluetooth,
    Headphones,
    Expand,
    Film,
    Book,
    Count
};

namespace music_shape {
struct P {
    float x, y;
};
inline float box(P p, P centre, P half, float radius) {
    const float qx = std::fabs(p.x - centre.x) - half.x + radius, qy = std::fabs(p.y - centre.y) - half.y + radius;
    return std::hypot(std::max(qx, 0.f), std::max(qy, 0.f)) + std::min(std::max(qx, qy), 0.f) - radius;
}
inline float circle(P p, P centre, float radius) { return std::hypot(p.x - centre.x, p.y - centre.y) - radius; }
inline float segment(P p, P a, P b, float width) {
    const float ex = b.x - a.x, ey = b.y - a.y;
    const float t = std::clamp(((p.x - a.x) * ex + (p.y - a.y) * ey) / std::max(1e-6f, ex * ex + ey * ey), 0.f, 1.f);
    return std::hypot(p.x - (a.x + t * ex), p.y - (a.y + t * ey)) - width / 2;
}
inline float line(P p, std::initializer_list<P> points, float width) {
    float d = 1e9f;
    for (auto a = points.begin(), b = a + 1; b != points.end(); a = b, ++b) d = std::min(d, segment(p, *a, *b, width));
    return d;
}
inline float triangle(P p, P a, P b, P c) {
    const P corners[3] = {a, b, c};
    float d = 1e9f;
    bool positive = true, negative = true;
    for (int i = 0; i < 3; i++) {
        const P u = corners[i], v = corners[(i + 1) % 3];
        d = std::min(d, segment(p, u, v, 0));
        const float cross = (v.x - u.x) * (p.y - u.y) - (v.y - u.y) * (p.x - u.x);
        positive = positive && cross >= 0;
        negative = negative && cross <= 0;
    }
    return positive || negative ? -d : d;
}
// A convex shape with four corners.
inline float quad(P p, P a, P b, P c, P d4) {
    const P corners[4] = {a, b, c, d4};
    float d = 1e9f;
    bool positive = true, negative = true;
    for (int i = 0; i < 4; i++) {
        const P u = corners[i], v = corners[(i + 1) % 4];
        d = std::min(d, segment(p, u, v, 0));
        const float cross = (v.x - u.x) * (p.y - u.y) - (v.y - u.y) * (p.x - u.x);
        positive = positive && cross >= 0;
        negative = negative && cross <= 0;
    }
    return positive || negative ? -d : d;
}
inline float heart(P p) {
    return std::min({circle(p, {8.1f, 9.2f}, 4.7f), circle(p, {15.9f, 9.2f}, 4.7f),
                     triangle(p, {4.1f, 11.7f}, {19.9f, 11.7f}, {12, 20.6f})});
}
inline float loop(P p) {
    // A rounded track with a gap before each arrowhead.
    float d = std::fabs(box(p, {12, 12}, {8, 5}, 4.2f)) - 1;
    d = std::max(d, -box(p, {14.6f, 7}, {2.4f, 2.4f}, 0));
    d = std::max(d, -box(p, {9.4f, 17}, {2.4f, 2.4f}, 0));
    d = std::min(d, triangle(p, {13.4f, 3.6f}, {18, 7}, {13.4f, 10.4f}));
    return std::min(d, triangle(p, {10.6f, 13.6f}, {6, 17}, {10.6f, 20.4f}));
}
} // namespace music_shape

// Signed distance to the symbol in its 24 x 24 box: negative inside.
inline float music_glyph_distance(MusicGlyph glyph, float x, float y) {
    using namespace music_shape;
    const P p{x, y};
    switch (glyph) {
    case MusicGlyph::Play: return triangle(p, {8, 5.4f}, {8, 18.6f}, {19.4f, 12}) - .8f;
    case MusicGlyph::Pause: return std::min(box(p, {8.4f, 12}, {2.3f, 6.8f}, 1), box(p, {15.6f, 12}, {2.3f, 6.8f}, 1));
    case MusicGlyph::Next:
        return std::min(triangle(p, {5.4f, 6.4f}, {5.4f, 17.6f}, {14.6f, 12}) - .7f,
                        box(p, {18, 12}, {1.5f, 6.3f}, .8f));
    case MusicGlyph::Previous:
        return std::min(triangle(p, {18.6f, 6.4f}, {18.6f, 17.6f}, {9.4f, 12}) - .7f,
                        box(p, {6, 12}, {1.5f, 6.3f}, .8f));
    case MusicGlyph::Shuffle: {
        float d = line(p, {{3, 7}, {7.6f, 7}, {14.6f, 17}, {19, 17}}, 2);
        d = std::min(d, line(p, {{3, 17}, {7.6f, 17}, {9.8f, 13.9f}}, 2));
        d = std::min(d, line(p, {{12.4f, 10.1f}, {14.6f, 7}, {19, 7}}, 2));
        d = std::min(d, triangle(p, {18, 3.8f}, {22.2f, 7}, {18, 10.2f}));
        return std::min(d, triangle(p, {18, 13.8f}, {22.2f, 17}, {18, 20.2f}));
    }
    case MusicGlyph::Repeat: return loop(p);
    case MusicGlyph::RepeatOne:
        return std::min(loop(p), line(p, {{10.9f, 10.6f}, {12.5f, 9.6f}, {12.5f, 14.6f}}, 1.5f));
    case MusicGlyph::Queue: {
        float d = std::min(box(p, {11, 6.5f}, {8, 1.1f}, 1), box(p, {11, 11.5f}, {8, 1.1f}, 1));
        d = std::min(d, box(p, {7.5f, 16.5f}, {4.5f, 1.1f}, 1));
        return std::min(d, triangle(p, {15.6f, 13.8f}, {15.6f, 20.4f}, {21.2f, 17.1f}) - .4f);
    }
    case MusicGlyph::Equaliser: {
        float d = 1e9f;
        const float knob[3] = {15.5f, 8, 13};
        for (int i = 0; i < 3; i++) {
            const float cx = 6 + float(i) * 6;
            d = std::min(d, segment(p, {cx, 4}, {cx, 20}, 1.6f));
            d = std::min(d, circle(p, {cx, knob[i]}, 2.7f));
        }
        return d;
    }
    case MusicGlyph::Bars: {
        float d = 1e9f;
        const float tall[4] = {8, 15, 11, 5.5f};
        for (int i = 0; i < 4; i++)
            d = std::min(d, box(p, {4.8f + float(i) * 4.8f, 20 - tall[i] / 2}, {1.6f, tall[i] / 2}, .8f));
        return d;
    }
    case MusicGlyph::Heart: return heart(p);
    case MusicGlyph::HeartLine:
        // The heart with a smaller heart taken out of it.
        return std::max(heart(p), -heart({12 + (p.x - 12) / .68f, 12.6f + (p.y - 12.6f) / .68f}) * .68f);
    case MusicGlyph::Note: {
        float d = std::min(segment(p, {9.6f, 5.6f}, {9.6f, 17}, 1.9f), segment(p, {19, 3.4f}, {19, 15}, 1.9f));
        d = std::min(d, std::min(circle(p, {7.1f, 17.3f}, 3.2f), circle(p, {16.5f, 15.3f}, 3.2f)));
        return std::min(d, quad(p, {8.7f, 4.6f}, {19.9f, 2.1f}, {19.9f, 5.9f}, {8.7f, 8.4f}));
    }
    case MusicGlyph::Folder:
        return std::min(box(p, {12, 13.5f}, {9, 6.5f}, 2), box(p, {7.6f, 7.4f}, {4.6f, 2.6f}, 1.6f));
    case MusicGlyph::Disc: {
        float d = std::max(circle(p, {12, 12}, 9.2f), -circle(p, {12, 12}, 2.1f));
        return std::max(d, -(std::fabs(circle(p, {12, 12}, 5.6f)) - .45f));
    }
    case MusicGlyph::Person:
        return std::min(circle(p, {12, 8}, 4), std::max(box(p, {12, 20.5f}, {7.4f, 5.5f}, 5.5f), p.y - 20.4f));
    case MusicGlyph::List: {
        float d = 1e9f;
        for (int i = 0; i < 3; i++) {
            const float cy = 6.5f + float(i) * 5.5f;
            d = std::min(d, std::min(circle(p, {4.6f, cy}, 1.5f), box(p, {14, cy}, {6.4f, 1.1f}, 1)));
        }
        return d;
    }
    case MusicGlyph::Speaker: {
        float d =
            std::min(box(p, {5.6f, 12}, {2.4f, 3.2f}, .8f), quad(p, {6.4f, 9}, {12, 4.6f}, {12, 19.4f}, {6.4f, 15}));
        // Two waves: arcs of rings, to the right only.
        for (float radius : {4.2f, 7.6f}) {
            const float ring = std::fabs(circle(p, {12, 12}, radius)) - .85f;
            d = std::min(d, std::max(ring, std::fabs(p.y - 12) * .9f - (p.x - 12)));
        }
        return d;
    }
    case MusicGlyph::Bluetooth:
        return line(p, {{6.6f, 7.6f}, {17, 16.2f}, {12, 20.6f}, {12, 3.4f}, {17, 7.8f}, {6.6f, 16.4f}}, 1.8f);
    case MusicGlyph::Headphones: {
        float d = std::max(std::fabs(circle(p, {12, 13}, 8)) - 1, p.y - 13);
        d = std::min(d, box(p, {4.6f, 16.4f}, {2.2f, 3.8f}, 1.5f));
        return std::min(d, box(p, {19.4f, 16.4f}, {2.2f, 3.8f}, 1.5f));
    }
    case MusicGlyph::Film: // a screen with a play mark in it
        return std::min(std::fabs(box(p, {12, 12.5f}, {9.4f, 7}, 2.4f)) - 1,
                        triangle(p, {9.8f, 9.2f}, {9.8f, 15.8f}, {15.6f, 12.5f}));
    case MusicGlyph::Book: // open, two pages either side of the spine
        return std::min(box(p, {6.9f, 12}, {4.7f, 7.4f}, 1.3f), box(p, {17.1f, 12}, {4.7f, 7.4f}, 1.3f));
    case MusicGlyph::Expand: {
        float d = 1e9f;
        for (int corner = 0; corner < 4; corner++) {
            const float sx = corner % 2 ? 1.f : -1.f, sy = corner / 2 ? 1.f : -1.f;
            d = std::min(
                d, line(p, {{12 + sx * 3.5f, 12 + sy * 8}, {12 + sx * 8, 12 + sy * 8}, {12 + sx * 8, 12 + sy * 3.5f}},
                        1.9f));
        }
        return d;
    }
    default: return 1e9f;
    }
}

// Coverage (0-255) of the symbol drawn `size` pixels square.
inline std::vector<uint8_t> music_glyph_coverage(MusicGlyph glyph, int size) {
    std::vector<uint8_t> out(size_t(size) * size_t(size));
    const float unit = 24.f / float(size);
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            const float d = music_glyph_distance(glyph, (float(x) + .5f) * unit, (float(y) + .5f) * unit) / unit;
            out[size_t(y) * size_t(size) + size_t(x)] = uint8_t(std::clamp(.5f - d, 0.f, 1.f) * 255 + .5f);
        }
    return out;
}

// The same symbol on the pixel style's 12 x 12 grid: '#' where the shape covers a third of the cell or more, so a
// thin line that runs between cell middles still lands on one of them.
inline std::string music_glyph_pixels(MusicGlyph glyph) {
    // Three sliders need their knobs to stand clear of their tracks, which the grid alone does not give them.
    if (glyph == MusicGlyph::Equaliser)
        return "............"
               ".#....#...#."
               ".#...###..#."
               ".#...###..#."
               ".#....#...#."
               ".#....#..###"
               "###...#..###"
               "###...#...#."
               ".#....#...#."
               ".#....#...#."
               ".#....#...#."
               "............";
    std::string out(144, '.');
    for (int y = 0; y < 12; y++)
        for (int x = 0; x < 12; x++) {
            int inside = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++)
                    inside += music_glyph_distance(glyph, float(x) * 2 + .25f + float(sx) * .5f,
                                                   float(y) * 2 + .25f + float(sy) * .5f) <= 0;
            if (inside >= 6) out[size_t(y) * 12 + size_t(x)] = '#';
        }
    return out;
}
} // namespace rgds
