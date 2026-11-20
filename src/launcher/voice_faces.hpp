#pragma once
// The sound toy's faces, one per voice, painted into pixels with signed distances: every edge anti-aliased, bodies
// shaded top to bottom with a darker rim, a soft shadow and a glassy highlight, like the launcher's other icons.
// Pure C++ (no SDL): voice_toy_screen.hpp bakes each face once per size and mouth opening into a texture.
#include "launcher/voice_toy.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace voice_faces {

struct Rgb {
    float r, g, b;
};
constexpr Rgb hex(uint32_t v) { return {float(v >> 16 & 255) / 255, float(v >> 8 & 255) / 255, float(v & 255) / 255}; }
inline Rgb mix(Rgb a, Rgb b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }

// ---- distances, in face units (the round faces are radius 1)
inline float len(float x, float y) { return std::sqrt(x * x + y * y); }
inline float circle(float x, float y, float cx, float cy, float r) { return len(x - cx, y - cy) - r; }
inline float ellipse(float x, float y, float cx, float cy, float rx, float ry) {
    const float px = (x - cx) / rx, py = (y - cy) / ry, k0 = len(px, py), k1 = len(px / rx, py / ry);
    return k1 < 1e-6f ? -std::min(rx, ry) : k0 * (k0 - 1) / k1;
}
inline float box(float x, float y, float cx, float cy, float hw, float hh, float r) {
    const float qx = std::abs(x - cx) - hw + r, qy = std::abs(y - cy) - hh + r;
    return len(std::max(qx, 0.f), std::max(qy, 0.f)) + std::min(std::max(qx, qy), 0.f) - r;
}
inline float segment(float x, float y, float ax, float ay, float bx, float by, float w) {
    const float px = x - ax, py = y - ay, dx = bx - ax, dy = by - ay;
    const float h = std::clamp((px * dx + py * dy) / std::max(1e-6f, dx * dx + dy * dy), 0.f, 1.f);
    return len(px - dx * h, py - dy * h) - w;
}
inline float triangle(float x, float y, float ax, float ay, float bx, float by, float cx, float cy) {
    const float e0x = bx - ax, e0y = by - ay, e1x = cx - bx, e1y = cy - by, e2x = ax - cx, e2y = ay - cy;
    const float v0x = x - ax, v0y = y - ay, v1x = x - bx, v1y = y - by, v2x = x - cx, v2y = y - cy;
    const auto part = [](float vx, float vy, float ex, float ey, float& qx, float& qy) {
        const float h = std::clamp((vx * ex + vy * ey) / (ex * ex + ey * ey), 0.f, 1.f);
        qx = vx - ex * h;
        qy = vy - ey * h;
    };
    float p0x, p0y, p1x, p1y, p2x, p2y;
    part(v0x, v0y, e0x, e0y, p0x, p0y);
    part(v1x, v1y, e1x, e1y, p1x, p1y);
    part(v2x, v2y, e2x, e2y, p2x, p2y);
    const float s = e0x * e2y - e0y * e2x > 0 ? 1.f : -1.f;
    const float d = std::min({p0x * p0x + p0y * p0y, p1x * p1x + p1y * p1y, p2x * p2x + p2y * p2y});
    const float k = std::min({s * (v0x * e0y - v0y * e0x), s * (v1x * e1y - v1y * e1x), s * (v2x * e2y - v2y * e2x)});
    return -std::sqrt(d) * (k > 0 ? 1.f : -1.f);
}
// A stroke along a circle from angle a0 to a1 (radians; 0 points right, positive turns down the screen).
inline float arc(float x, float y, float cx, float cy, float r, float a0, float a1, float w) {
    float best = 1e9f;
    const int steps = 16;
    float px = cx + r * std::cos(a0), py = cy + r * std::sin(a0);
    for (int i = 1; i <= steps; i++) {
        const float a = a0 + (a1 - a0) * float(i) / steps, qx = cx + r * std::cos(a), qy = cy + r * std::sin(a);
        best = std::min(best, segment(x, y, px, py, qx, qy, w));
        px = qx;
        py = qy;
    }
    return best;
}

// An RGBA canvas, `size` pixels square, premultiplied while painting.
class Canvas {
    int n;
    float unit, ox, oy;
    std::vector<float> px;

public:
    explicit Canvas(int size) : n(std::max(8, size)), unit(n * .4f), ox(n * .5f), oy(n * .53f), px(size_t(n) * n * 4) {}
    int size() const { return n; }
    float pixel() const { return 1.f / unit; } // one pixel, in face units
    // Paints where `shape` (a distance in face units) is inside, coloured by `paint(x, y)`. `blur` softens the edge
    // over that many face units (shadows and glows).
    template <class Shape, class Paint> void fill(Shape&& shape, Paint&& paint, float alpha = 1, float blur = 0) {
        const float soft = std::max(1.f, blur * unit);
        for (int y = 0; y < n; y++)
            for (int x = 0; x < n; x++) {
                const float ux = (x + .5f - ox) / unit, uy = (y + .5f - oy) / unit;
                const float d = shape(ux, uy) * unit;
                float cover = blur > 0 ? std::clamp(.5f - d / soft, 0.f, 1.f) : std::clamp(.5f - d, 0.f, 1.f);
                if (blur > 0) cover = cover * cover * (3 - 2 * cover);
                if (cover <= 0) continue;
                const Rgb c = paint(ux, uy);
                const float a = cover * alpha;
                float* p = &px[(size_t(y) * n + size_t(x)) * 4];
                p[0] = c.r * a + p[0] * (1 - a);
                p[1] = c.g * a + p[1] * (1 - a);
                p[2] = c.b * a + p[2] * (1 - a);
                p[3] = a + p[3] * (1 - a);
            }
    }
    template <class Shape> void fill(Shape&& shape, Rgb colour, float alpha = 1, float blur = 0) {
        fill(shape, [colour](float, float) { return colour; }, alpha, blur);
    }
    // Straight-alpha ARGB8888, as SDL textures take it.
    std::vector<uint32_t> argb() const {
        std::vector<uint32_t> out(size_t(n) * n);
        for (size_t i = 0; i < out.size(); i++) {
            const float a = px[i * 4 + 3];
            const auto channel = [&](float v) {
                return uint32_t(std::clamp(a > 0 ? v / a : 0.f, 0.f, 1.f) * 255 + .5f);
            };
            out[i] = uint32_t(std::clamp(a, 0.f, 1.f) * 255 + .5f) << 24 | channel(px[i * 4]) << 16 |
                     channel(px[i * 4 + 1]) << 8 | channel(px[i * 4 + 2]);
        }
        return out;
    }
};

// The parts every face is made of.
class Painter {
    Canvas& c;
    float rim;

public:
    float mouth; // 0 closed to 1 wide open
    explicit Painter(Canvas& canvas, float open)
        : c(canvas), rim(std::max(.045f, canvas.pixel() * 1.3f)), mouth(open) {}
    Canvas& canvas() { return c; }
    float line() const { return rim; }
    static constexpr Rgb ink = hex(0x2A2233), white = hex(0xFFFFFF), tongue = hex(0xF2788A), dark_mouth = hex(0x4A1E2A);

    // A shaded body: shadow under it, a darker rim, a top-to-bottom gradient and a glassy highlight.
    template <class Shape>
    void body(Shape&& shape, Rgb top, Rgb bottom, Rgb edge, bool shadow = true, bool gloss = true, float top_y = -1,
              float bottom_y = 1) {
        if (shadow) c.fill([&](float x, float y) { return shape(x, y - .07f); }, hex(0x101828), .28f, .1f);
        c.fill([&](float x, float y) { return shape(x, y) - rim; }, edge);
        c.fill(shape, [&](float, float y) {
            return mix(top, bottom, std::clamp((y - top_y) / (bottom_y - top_y), 0.f, 1.f));
        });
        if (gloss)
            c.fill(
                [&](float x, float y) { return std::max(shape(x, y) + .06f, ellipse(x, y, -.28f, -.55f, .5f, .26f)); },
                white, .32f);
    }
    template <class Shape> void part(Shape&& shape, Rgb top, Rgb bottom, Rgb edge, float top_y, float bottom_y) {
        body(shape, top, bottom, edge, false, false, top_y, bottom_y);
    }
    // A plain dark eye with a glint.
    void dot_eye(float x, float y, float r) {
        c.fill([=](float px, float py) { return ellipse(px, py, x, y, r, r * 1.25f); }, ink);
        c.fill([=](float px, float py) { return circle(px, py, x + r * .35f, y - r * .45f, r * .38f); }, white);
    }
    // A big shiny eye: white, a coloured iris, a pupil and two glints.
    void shiny_eye(float x, float y, float r, Rgb iris_top, Rgb iris_bottom, float look = 0) {
        c.fill([=](float px, float py) { return circle(px, py, x, y, r) - rim * .8f; }, hex(0x3A3045));
        c.fill(
            [=](float px, float py) { return circle(px, py, x, y, r); },
            [=](float, float py) { return mix(white, hex(0xE4E8F2), std::clamp((py - y + r) / (2 * r), 0.f, 1.f)); });
        const float ix = x + look * r * .2f, iy = y + r * .08f;
        c.fill([=](float px, float py) { return circle(px, py, ix, iy, r * .62f); },
               [=](float, float py) {
                   return mix(iris_top, iris_bottom, std::clamp((py - iy + r * .6f) / (1.2f * r), 0.f, 1.f));
               });
        c.fill([=](float px, float py) { return circle(px, py, ix, iy, r * .32f); }, ink);
        c.fill([=](float px, float py) { return circle(px, py, ix + r * .22f, iy - r * .25f, r * .17f); }, white);
        c.fill([=](float px, float py) { return circle(px, py, ix - r * .2f, iy + r * .22f, r * .08f); }, white, .8f);
    }
    // Closed, happy eyes: little upturned arcs.
    void happy_eye(float x, float y, float r) {
        c.fill([=](float px, float py) { return arc(px, py, x, y + r * .5f, r, -2.6f, -.54f, rim * .9f); }, ink);
    }
    void cheeks(float x, float y, float r, Rgb colour = hex(0xFF7A9A)) {
        c.fill([=](float px, float py) { return ellipse(px, py, -x, y, r, r * .62f); }, colour, .38f);
        c.fill([=](float px, float py) { return ellipse(px, py, x, y, r, r * .62f); }, colour, .38f);
    }
    // A mouth that smiles when quiet and opens with the sound.
    void talk(float x, float y, float w, bool frown = false) {
        if (mouth < .06f) {
            if (frown)
                c.fill([=](float px, float py) { return arc(px, py, x, y + w * .55f, w * .62f, -2.5f, -.64f, rim); },
                       ink);
            else
                c.fill([=](float px, float py) { return arc(px, py, x, y - w * .55f, w * .62f, .64f, 2.5f, rim); },
                       ink);
            return;
        }
        const float h = w * (.25f + .75f * mouth);
        const auto shape = [=](float px, float py) { return ellipse(px, py, x, y, w * .5f, h * .5f); };
        c.fill([&](float px, float py) { return shape(px, py) - rim * .6f; }, ink);
        c.fill(shape, dark_mouth);
        c.fill([&](float px, float py) { return std::max(shape(px, py), circle(px, py, x, y + h * .55f, w * .34f)); },
               tongue);
    }
    // A round "oo" mouth (singing, blowing, a ghost's boo).
    void oo(float x, float y, float r) {
        const float rr = r * (1 + mouth * .7f);
        c.fill([=](float px, float py) { return ellipse(px, py, x, y, rr * .85f, rr) - rim * .6f; }, ink);
        c.fill([=](float px, float py) { return ellipse(px, py, x, y, rr * .85f, rr); }, dark_mouth);
    }
    void tooth(float ax, float ay, float bx, float by, float cx, float cy) {
        c.fill([=](float px, float py) { return triangle(px, py, ax, ay, bx, by, cx, cy) - rim * .5f; }, hex(0x6B5A50));
        c.fill([=](float px, float py) { return triangle(px, py, ax, ay, bx, by, cx, cy); }, white);
    }
    void note(float x, float y, float s, Rgb colour) {
        c.fill(
            [=](float px, float py) {
                return std::min({ellipse(px, py, x, y, s * .2f, s * .15f),
                                 segment(px, py, x + s * .18f, y, x + s * .18f, y - s * .7f, s * .05f),
                                 segment(px, py, x + s * .18f, y - s * .7f, x + s * .42f, y - s * .5f, s * .06f)});
            },
            colour);
    }
    // A ring (bubbles, sound waves).
    void ring(float x, float y, float r, float w, Rgb colour, float alpha = 1) {
        c.fill([=](float px, float py) { return std::abs(circle(px, py, x, y, r)) - w; }, colour, alpha);
    }
};

// The face of a voice. `mouth` 0 is the quiet face; up to 1 it talks.
inline Canvas paint(voice_toy::Voice voice, int size, float mouth) {
    using voice_toy::Voice;
    Canvas canvas(size);
    Painter p(canvas, std::clamp(mouth, 0.f, 1.f));
    const float L = p.line();
    const auto round = [](float x, float y) { return circle(x, y, 0, 0, 1); };
    switch (voice) {
    case Voice::Chipmunk: {
        for (float s : {-1.f, 1.f}) {
            p.body([=](float x, float y) { return circle(x, y, s * .68f, -.74f, .3f); }, hex(0xD9955F), hex(0xB06A3C),
                   hex(0x6E3F22));
            canvas.fill([=](float x, float y) { return circle(x, y, s * .68f, -.74f, .16f); }, hex(0xF6B6A0));
        }
        p.body(round, hex(0xE0A06A), hex(0xA8653A), hex(0x6E3F22));
        canvas.fill(
            [&](float x, float y) {
                return std::max(round(x, y) + .02f, std::min({segment(x, y, -.17f, -1.1f, -.17f, -.62f, .06f),
                                                              segment(x, y, 0, -1.1f, 0, -.56f, .07f),
                                                              segment(x, y, .17f, -1.1f, .17f, -.62f, .06f)}));
            },
            hex(0x7A4524));
        canvas.fill(
            [](float x, float y) {
                return std::min(
                    {ellipse(x, y, 0, .38f, .5f, .36f), circle(x, y, -.36f, .3f, .3f), circle(x, y, .36f, .3f, .3f)});
            },
            [](float, float y) { return mix(hex(0xFFF6E8), hex(0xF3DCC0), std::clamp(y + .1f, 0.f, 1.f)); });
        for (float s : {-1.f, 1.f}) p.shiny_eye(s * .38f, -.2f, .17f, hex(0x5B3A26), hex(0x2A1A10));
        canvas.fill([](float x, float y) { return ellipse(x, y, 0, .12f, .11f, .08f); }, hex(0x5B2C1A));
        canvas.fill([](float x, float y) { return circle(x, y, .03f, .09f, .025f); }, Painter::white, .8f);
        p.talk(0, .36f, .3f);
        for (float s : {-1.f, 1.f}) {
            const float y0 = p.mouth < .06f ? .44f : .36f + .3f * (.25f + .75f * p.mouth) * .5f - .02f;
            canvas.fill([=](float x, float y) { return box(x, y, s * .055f, y0 + .08f, .045f, .08f, .02f) - L * .5f; },
                        hex(0x8A7A70));
            canvas.fill([=](float x, float y) { return box(x, y, s * .055f, y0 + .08f, .045f, .08f, .02f); },
                        Painter::white);
        }
        break;
    }
    case Voice::Robot: {
        canvas.fill([](float x, float y) { return segment(x, y, 0, -.78f, 0, -1.06f, .035f); }, hex(0x6E7A8A));
        canvas.fill([](float x, float y) { return circle(x, y, 0, -1.12f, .22f); }, hex(0xFF5A5A), .35f + p.mouth * .4f,
                    .18f);
        p.part([](float x, float y) { return circle(x, y, 0, -1.12f, .1f); }, hex(0xFF9A90), hex(0xE53935),
               hex(0x8A1C1C), -1.22f, -1.02f);
        for (float s : {-1.f, 1.f})
            p.part([=](float x, float y) { return box(x, y, s * .96f, 0, .1f, .22f, .05f); }, hex(0x9AA6B6),
                   hex(0x6E7A8A), hex(0x3E4856), -.22f, .22f);
        const auto head = [](float x, float y) { return box(x, y, 0, 0, .86f, .8f, .24f); };
        p.body(head, hex(0xE4EBF3), hex(0x97A4B6), hex(0x4E5A6B), true, true, -.8f, .8f);
        canvas.fill([](float x, float y) { return box(x, y, 0, -.18f, .64f, .27f, .14f); }, hex(0x1F2933));
        for (float s : {-1.f, 1.f}) {
            canvas.fill([=](float x, float y) { return box(x, y, s * .3f, -.18f, .13f, .09f, .05f); }, hex(0x5CEBFF),
                        .6f, .12f);
            canvas.fill([=](float x, float y) { return box(x, y, s * .3f, -.18f, .12f, .08f, .05f); }, hex(0x9AF6FF));
        }
        const float hh = .08f + .12f * p.mouth;
        canvas.fill([=](float x, float y) { return box(x, y, 0, .44f, .4f, hh, .06f); }, hex(0x1F2933));
        for (int i = -2; i <= 2; i++)
            canvas.fill(
                [=](float x, float y) { return box(x, y, i * .14f, .44f, .025f, std::max(.02f, hh - .035f), .02f); },
                p.mouth > .06f ? hex(0x9AF6FF) : hex(0x6B7A8C));
        for (float sx : {-1.f, 1.f})
            for (float sy : {-1.f, 1.f})
                p.part([=](float x, float y) { return circle(x, y, sx * .72f, sy * .64f, .045f); }, hex(0xC9D2DE),
                       hex(0x7D8899), hex(0x4E5A6B), sy * .64f - .05f, sy * .64f + .05f);
        break;
    }
    case Voice::Monster: {
        for (float s : {-1.f, 1.f})
            p.part([=](float x, float y) { return triangle(x, y, s * .74f, -.5f, s * .3f, -.8f, s * .88f, -1.24f); },
                   hex(0xFFF6DE), hex(0xE2C48F), hex(0x8A6A3A), -1.24f, -.5f);
        p.body(round, hex(0xA4EC6E), hex(0x4FA83A), hex(0x2E6B22));
        for (auto s : {std::array<float, 3>{.6f, .3f, .13f},
                       {-.64f, .1f, .09f},
                       {.36f, -.64f, .08f},
                       {-.3f, .66f, .07f},
                       {.66f, -.18f, .05f}})
            canvas.fill([&](float x, float y) { return std::max(round(x, y) + .03f, circle(x, y, s[0], s[1], s[2])); },
                        hex(0x5DB044), .7f);
        p.shiny_eye(0, -.22f, .37f, hex(0xFFC35A), hex(0xF06A1A), .3f);
        canvas.fill([](float x, float y) { return arc(x, y, 0, -.22f, .46f, -2.4f, -.74f, .045f); }, hex(0x2E6B22));
        const float h = p.mouth < .06f ? .12f : .18f + .3f * p.mouth, top = .5f - h / 2;
        const auto lips = [=](float x, float y) { return box(x, y, 0, .5f, .36f, h / 2, std::min(.12f, h / 2)); };
        canvas.fill([&](float x, float y) { return lips(x, y) - L * .6f; }, Painter::ink);
        canvas.fill(lips, hex(0x3B1230));
        if (p.mouth >= .06f)
            canvas.fill([&](float x, float y) { return std::max(lips(x, y), circle(x, y, 0, .5f + h * .55f, .2f)); },
                        Painter::tongue);
        for (float s : {-1.f, 1.f}) p.tooth(s * .26f - .07f, top, s * .26f + .07f, top, s * .26f, top + .19f);
        p.tooth(-.06f, .5f + h / 2, .06f, .5f + h / 2, 0, .5f + h / 2 - .12f);
        break;
    }
    case Voice::Backwards: {
        p.body(round, hex(0xD3B2FA), hex(0x8B5CF6), hex(0x5B30B0));
        for (float s : {-1.f, 1.f}) p.dot_eye(s * .33f, .24f, .1f);
        p.cheeks(.6f, .5f, .17f);
        p.talk(0, -.36f, .44f, true);
        canvas.fill([](float x, float y) { return circle(x, y, .7f, .72f, .3f); }, hex(0x101828), .25f, .08f);
        p.part([](float x, float y) { return circle(x, y, .7f, .7f, .28f); }, Painter::white, hex(0xE6DDF8),
               hex(0x5B30B0), .42f, .98f);
        for (float dx : {-.08f, .1f})
            canvas.fill(
                [=](float x, float y) { return triangle(x, y, .64f + dx, .7f, .78f + dx, .6f, .78f + dx, .8f); },
                hex(0x7C4DDB));
        break;
    }
    case Voice::Echo: {
        for (int i = 2; i >= 0; i--)
            canvas.fill([=](float x, float y) { return arc(x, y, -.28f, 0, .95f + i * .19f, -.62f, .62f, .05f); },
                        hex(0x3D9BE0), 1 - i * .28f);
        p.body([](float x, float y) { return circle(x, y, -.28f, 0, .74f); }, hex(0xA8E0FF), hex(0x3D9BE0),
               hex(0x1E5F9E));
        for (float s : {-1.f, 1.f}) p.dot_eye(-.28f + s * .26f, -.16f, .085f);
        p.oo(-.28f, .3f, .1f);
        break;
    }
    case Voice::Alien: {
        for (float s : {-1.f, 1.f}) {
            canvas.fill([=](float x, float y) { return segment(x, y, s * .3f, -.72f, s * .56f, -1.12f, .035f); },
                        hex(0x2B9A6A));
            p.part([=](float x, float y) { return circle(x, y, s * .58f, -1.15f, .1f); }, hex(0xD9FFE9), hex(0x6FE0A8),
                   hex(0x1E7A52), -1.25f, -1.05f);
        }
        const auto head = [](float x, float y) {
            const float a = circle(x, y, 0, -.18f, .9f), b = ellipse(x, y, 0, .4f, .5f, .58f), k = .3f;
            const float h = std::clamp(.5f + .5f * (b - a) / k, 0.f, 1.f);
            return b + (a - b) * h - k * h * (1 - h);
        };
        p.body(head, hex(0xB6F5D2), hex(0x3CC48A), hex(0x1E7A52));
        for (auto e : {std::array<float, 4>{-.4f, -.12f, .21f, .3f}, {.4f, -.12f, .21f, .3f}, {0, -.62f, .13f, .17f}}) {
            canvas.fill([=](float x, float y) { return ellipse(x, y, e[0], e[1], e[2], e[3]); }, hex(0x0E2A1E));
            canvas.fill(
                [=](float x, float y) {
                    return ellipse(x, y, e[0] + e[2] * .3f, e[1] - e[3] * .35f, e[2] * .32f, e[3] * .22f);
                },
                Painter::white, .9f);
        }
        p.talk(0, .5f, .22f);
        break;
    }
    case Voice::Giant: {
        p.body(round, hex(0xF8C99A), hex(0xD98A4E), hex(0x8A4A22));
        canvas.fill([&](float x, float y) { return std::max(round(x, y) - .02f, -(y - .2f) + .15f * std::cos(x * 6)); },
                    hex(0x8A5432));
        canvas.fill(
            [&](float x, float y) { return std::max(round(x, y) - .02f, -(y - .2f) + .15f * std::cos(x * 6)) + .04f; },
            hex(0xA0653D));
        canvas.fill([](float x, float y) { return segment(x, y, -.55f, -.42f, .55f, -.42f, .09f); }, hex(0x5A341C));
        for (float s : {-1.f, 1.f}) p.dot_eye(s * .3f, -.2f, .08f);
        p.part([](float x, float y) { return ellipse(x, y, 0, .05f, .17f, .14f); }, hex(0xF0B07A), hex(0xC9783F),
               hex(0x8A4A22), -.1f, .2f);
        p.talk(0, .48f, .42f);
        break;
    }
    case Voice::Ghost: {
        const auto ghost = [](float x, float y) {
            float shape = std::min(circle(x, y, 0, -.2f, .82f), box(x, y, 0, .32f, .82f, .58f, .02f));
            for (int k = -2; k <= 2; k++) shape = std::max(shape, -circle(x, y, k * .41f, .95f, .18f));
            return shape;
        };
        canvas.fill([](float x, float y) { return ellipse(x, y, 0, 1.08f, .7f, .12f); }, hex(0x101828), .2f, .1f);
        p.body(ghost, Painter::white, hex(0xCCD6EE), hex(0x7D8CB0), false, true, -1, .9f);
        for (float s : {-1.f, 1.f})
            canvas.fill([=](float x, float y) { return ellipse(x, y, s * .28f, -.22f, .12f, .18f); }, Painter::ink);
        for (float s : {-1.f, 1.f})
            canvas.fill([=](float x, float y) { return circle(x, y, s * .28f + .04f, -.28f, .04f); }, Painter::white);
        p.cheeks(.5f, .02f, .13f, hex(0xA0B4FF));
        p.oo(0, .26f, .1f);
        break;
    }
    case Voice::Underwater: {
        for (auto b : {std::array<float, 3>{-.92f, -.62f, .1f}, {-.78f, -.95f, .07f}, {-1.02f, -1.1f, .05f}}) {
            canvas.fill([=](float x, float y) { return circle(x, y, b[0], b[1], b[2]); }, hex(0xBDE8FF), .35f);
            p.ring(b[0], b[1], b[2], .016f, hex(0x5AB4E8));
            canvas.fill(
                [=](float x, float y) { return circle(x, y, b[0] + b[2] * .35f, b[1] - b[2] * .35f, b[2] * .28f); },
                Painter::white);
        }
        const auto tail = [](float x, float y) { return triangle(x, y, .6f, 0, 1.18f, -.5f, 1.18f, .5f); };
        p.part(tail, hex(0xFFB25C), hex(0xF0601E), hex(0xB24A10), -.5f, .5f);
        p.part([](float x, float y) { return triangle(x, y, -.25f, -.5f, .35f, -.52f, .1f, -.88f); }, hex(0xFFB25C),
               hex(0xF57A2E), hex(0xB24A10), -.88f, -.5f);
        const auto fish = [](float x, float y) { return ellipse(x, y, -.1f, 0, .82f, .6f); };
        p.body(fish, hex(0xFFC27A), hex(0xFF6E2A), hex(0xB24A10), true, true, -.6f, .6f);
        for (float bx : {.08f, .38f})
            canvas.fill([&](float x, float y) { return std::max(fish(x, y) + .02f, box(x, y, bx, 0, .07f, 1, .03f)); },
                        Painter::white, .9f);
        p.shiny_eye(-.48f, -.14f, .17f, hex(0x3A6BD6), hex(0x14306E), -.5f);
        p.oo(-.82f, .14f, .06f);
        break;
    }
    case Voice::Choir: {
        const auto singer = [&](float cx, float cy, float r, Rgb top, Rgb bottom, Rgb edge) {
            p.body([=](float x, float y) { return circle(x, y, cx, cy, r); }, top, bottom, edge, true, true, cy - r,
                   cy + r);
            for (float s : {-1.f, 1.f}) p.happy_eye(cx + s * r * .36f, cy - r * .12f, r * .14f);
            canvas.fill(
                [=](float x, float y) { return ellipse(x, y, cx - r * .62f, cy + r * .25f, r * .16f, r * .1f); },
                hex(0xFF7A9A), .4f);
            canvas.fill(
                [=](float x, float y) { return ellipse(x, y, cx + r * .62f, cy + r * .25f, r * .16f, r * .1f); },
                hex(0xFF7A9A), .4f);
            p.oo(cx, cy + r * .42f, r * .14f);
        };
        singer(-.62f, .2f, .5f, hex(0xFFC4D6), hex(0xF08AAE), hex(0xA84870));
        singer(.62f, .2f, .5f, hex(0xBFD8FF), hex(0x7FA8F0), hex(0x3C64A8));
        singer(0, -.02f, .64f, hex(0xFFE2B8), hex(0xF5B07A), hex(0xA86A38));
        p.note(-.82f, -.6f, .42f, hex(0x7C4DDB));
        p.note(.72f, -.72f, .36f, hex(0xE0458A));
        break;
    }
    case Voice::EightBit: {
        const auto screen = [](float x, float y) { return box(x, y, 0, 0, .92f, .92f, .14f); };
        p.body(screen, hex(0x3A3F5C), hex(0x1C1F30), hex(0x0E0F18), true, false, -.9f, .9f);
        // An 8 x 8 face in big pixels; the mouth row opens as it talks.
        static const char* rows[8] = {"..####..", ".#....#.", "#.#..#.#", "#......#",
                                      "#.#..#.#", "#..##..#", ".#....#.", "..####.."};
        static const char* open[8] = {"..####..", ".#....#.", "#.#..#.#", "#......#",
                                      "#.####.#", "#.#..#.#", ".#.##.#.", "..####.."};
        const char** grid = p.mouth > .06f ? open : rows;
        const float cell = .2f;
        for (int gy = 0; gy < 8; gy++)
            for (int gx = 0; gx < 8; gx++)
                if (grid[gy][gx] == '#') {
                    const float cx = (gx - 3.5f) * cell, cy = (gy - 3.5f) * cell;
                    canvas.fill([=](float x, float y) { return box(x, y, cx, cy, cell * .5f, cell * .5f, .01f); },
                                hex(0x7CFF6B), .35f, .12f);
                    canvas.fill([=](float x, float y) { return box(x, y, cx, cy, cell * .44f, cell * .44f, .02f); },
                                [=](float, float y) {
                                    return mix(hex(0xC8FFB0), hex(0x5EDB4A),
                                               std::clamp((y - cy + cell / 2) / cell, 0.f, 1.f));
                                });
                }
        break;
    }
    case Voice::Telephone: {
        p.body([](float x, float y) { return circle(x, y, -.12f, .06f, .8f); }, hex(0xFFE27A), hex(0xF5A623),
               hex(0xC77D1A), true, true, -.74f, .86f);
        for (float s : {-1.f, 1.f}) p.dot_eye(-.12f + s * .28f, -.12f, .085f);
        p.talk(-.2f, .34f, .3f);
        const auto handset = [](float x, float y) {
            return std::min({segment(x, y, .62f, -.62f, .86f, .5f, .11f), ellipse(x, y, .58f, -.74f, .2f, .14f),
                             ellipse(x, y, .86f, .64f, .2f, .14f)});
        };
        p.body(handset, hex(0xFF7A70), hex(0xD93636), hex(0x7A1414), true, true, -.9f, .8f);
        for (int i = 0; i < 4; i++)
            canvas.fill([=](float x, float y) { return std::abs(circle(x, y, 1.0f, .82f + i * .1f, .07f)) - .018f; },
                        hex(0xD93636));
        break;
    }
    case Voice::Megaphone: {
        for (int i = 0; i < 3; i++)
            canvas.fill([=](float x, float y) { return arc(x, y, .55f, -.05f, .62f + i * .2f, -.6f, .6f, .05f); },
                        hex(0xFF9A3C), 1 - i * .28f);
        p.part([](float x, float y) { return box(x, y, -.32f, .42f, .1f, .26f, .06f); }, hex(0x5A5F70), hex(0x2E3140),
               hex(0x15171F), .16f, .7f);
        const auto cone = [](float x, float y) {
            return std::min(triangle(x, y, -.75f, -.2f, .55f, -.62f, .55f, .52f),
                            triangle(x, y, -.75f, -.2f, -.75f, .1f, .55f, .52f));
        };
        p.body(cone, hex(0xFF6F61), hex(0xD32F2F), hex(0x7A1414), true, true, -.62f, .52f);
        canvas.fill(
            [&](float x, float y) { return std::max(cone(x, y) + .02f, box(x, y, -.1f, -.05f, .08f, 1, .02f)); },
            Painter::white, .9f);
        p.part([](float x, float y) { return ellipse(x, y, .56f, -.05f, .14f, .6f); }, hex(0xFFE0DC), hex(0xF5B5AE),
               hex(0x7A1414), -.65f, .55f);
        canvas.fill([](float x, float y) { return ellipse(x, y, .58f, -.05f, .08f, .48f); }, hex(0x5A1010), .8f);
        p.part([](float x, float y) { return box(x, y, -.82f, -.05f, .1f, .2f, .05f); }, hex(0x5A5F70), hex(0x2E3140),
               hex(0x15171F), -.25f, .15f);
        break;
    }
    case Voice::Cave: {
        const auto wings = [](float x, float y) {
            float shape = 1e9f;
            for (float s : {-1.f, 1.f}) {
                float w = triangle(x, y, s * .2f, -.35f, s * 1.18f, -.62f, s * 1.0f, .42f);
                for (int k = 0; k < 3; k++)
                    w = std::max(w, -circle(x, y, s * (.45f + k * .27f), .42f - k * .12f, .17f));
                shape = std::min(shape, w);
            }
            return shape;
        };
        p.body(wings, hex(0x7E6CA8), hex(0x46386E), hex(0x241A40), true, false, -.62f, .42f);
        for (float s : {-1.f, 1.f})
            p.part([=](float x, float y) { return triangle(x, y, s * .16f, -.48f, s * .5f, -.4f, s * .42f, -.92f); },
                   hex(0x8C7AB8), hex(0x5A4A88), hex(0x241A40), -.92f, -.4f);
        p.body([](float x, float y) { return circle(x, y, 0, 0, .55f); }, hex(0x9A88C8), hex(0x5A4A88), hex(0x241A40),
               false, true, -.55f, .55f);
        for (float s : {-1.f, 1.f}) {
            canvas.fill([=](float x, float y) { return circle(x, y, s * .2f, -.08f, .17f); }, hex(0xFFF27A), .5f, .1f);
            p.shiny_eye(s * .2f, -.08f, .13f, hex(0xFFE45A), hex(0xF5A623));
        }
        p.talk(0, .22f, .2f);
        for (float s : {-1.f, 1.f}) p.tooth(s * .09f - .04f, .22f, s * .09f + .04f, .22f, s * .09f, .33f);
        break;
    }
    case Voice::Jelly: {
        const auto blob = [](float x, float y) {
            const float a = std::atan2(y, x);
            return len(x, y * 1.05f) - (.92f + .06f * std::sin(5 * a) + .03f * std::sin(9 * a + 1));
        };
        p.body(blob, hex(0xFFB3D6), hex(0xE8559A), hex(0xA82765));
        canvas.fill([&](float x, float y) { return std::max(blob(x, y) + .1f, ellipse(x, y, .4f, .5f, .2f, .12f)); },
                    Painter::white, .25f);
        for (float s : {-1.f, 1.f}) p.dot_eye(s * .32f, -.16f, .1f);
        p.cheeks(.58f, .14f, .15f, hex(0xFF5A8A));
        p.talk(0, .32f, .36f);
        break;
    }
    case Voice::Scratch: {
        p.body(round, hex(0x3A3A48), hex(0x14141C), hex(0x050508), true, false);
        for (float r : {.88f, .78f, .68f, .58f, .48f}) p.ring(0, 0, r, .01f, hex(0x5A5A6E), .7f);
        canvas.fill(
            [](float x, float y) { return std::max(circle(x, y, 0, 0, .97f), ellipse(x, y, -.35f, -.5f, .55f, .25f)); },
            Painter::white, .14f);
        p.part([](float x, float y) { return circle(x, y, 0, 0, .38f); }, hex(0xFF7AA2), hex(0xE0306A), hex(0x7A1034),
               -.38f, .38f);
        for (float s : {-1.f, 1.f}) p.dot_eye(s * .13f, -.07f, .045f);
        p.talk(0, .13f, .14f);
        canvas.fill([](float x, float y) { return segment(x, y, .95f, -.95f, .45f, .35f, .045f); }, hex(0xC9D2DE));
        p.part([](float x, float y) { return box(x, y, .45f, .4f, .09f, .06f, .03f); }, hex(0xE4EBF3), hex(0x97A4B6),
               hex(0x4E5A6B), .34f, .46f);
        p.part([](float x, float y) { return circle(x, y, .95f, -.95f, .12f); }, hex(0xE4EBF3), hex(0x97A4B6),
               hex(0x4E5A6B), -1.07f, -.83f);
        break;
    }
    case Voice::Whisper: {
        p.body(round, hex(0xD4F7EA), hex(0x7FD1B0), hex(0x2F8A68));
        for (float s : {-1.f, 1.f}) {
            canvas.fill([=](float x, float y) { return arc(x, y, s * .32f, -.26f, .13f, .3f, 2.84f, .035f); },
                        Painter::ink);
            canvas.fill([=](float x, float y) { return segment(x, y, s * .2f, -.16f, s * .14f, -.08f, .015f); },
                        Painter::ink);
        }
        p.cheeks(.56f, .16f, .15f);
        canvas.fill([](float x, float y) { return ellipse(x, y, 0, .4f, .12f, .07f); }, Painter::dark_mouth);
        p.part([](float x, float y) { return box(x, y, 0, .3f, .09f, .34f, .09f); }, hex(0xFFE0C2), hex(0xF2B98A),
               hex(0x9A6038), -.04f, .64f);
        canvas.fill([](float x, float y) { return box(x, y, 0, .06f, .055f, .07f, .05f); }, hex(0xFFF3E6));
        for (int i = 0; i < 3; i++)
            canvas.fill([=](float x, float y) { return arc(x, y, .3f, .3f, .32f + i * .16f, -.5f, .5f, .025f); },
                        hex(0x2F8A68), .8f - i * .22f);
        break;
    }
    case Voice::Sloth: {
        p.body(round, hex(0xD9C3A0), hex(0x9C7E58), hex(0x5E4630));
        canvas.fill([](float x, float y) { return ellipse(x, y, 0, .08f, .78f, .62f); }, hex(0xF5EBD8));
        for (float s : {-1.f, 1.f}) {
            canvas.fill([=](float x, float y) { return ellipse(x, y, s * .36f, -.06f, .24f, .15f); }, hex(0x5C4632));
            p.happy_eye(s * .36f, -.06f, .09f);
        }
        p.part([](float x, float y) { return ellipse(x, y, 0, .16f, .13f, .09f); }, hex(0x6E5536), hex(0x3E2E1E),
               hex(0x2A1E12), .07f, .25f);
        p.talk(0, .38f, .3f);
        canvas.fill([&](float x, float y) { return std::max(round(x, y) + .02f, y + .62f - .05f * std::cos(x * 14)); },
                    hex(0x7A5E3E), .9f);
        break;
    }
    case Voice::Speedy: {
        for (int i = 0; i < 3; i++)
            canvas.fill(
                [=](float x, float y) {
                    return segment(x, y, -1.2f + i * .08f, -.3f + i * .3f, -.95f + i * .05f, -.3f + i * .3f, .035f);
                },
                hex(0x9AA6C0), .8f);
        for (float s : {-1.f, 1.f}) {
            p.part([=](float x, float y) { return ellipse(x, y, s * .34f, -.88f, .16f, .4f); }, Painter::white,
                   hex(0xDCDCEA), hex(0x8A8AA6), -1.28f, -.48f);
            canvas.fill([=](float x, float y) { return ellipse(x, y, s * .34f, -.86f, .075f, .28f); }, hex(0xFFB6C8));
        }
        p.body(round, Painter::white, hex(0xD6D8E8), hex(0x8A8AA6));
        for (float s : {-1.f, 1.f}) p.shiny_eye(s * .34f, -.16f, .14f, hex(0x6BA8FF), hex(0x2050B0));
        canvas.fill([](float x, float y) { return triangle(x, y, -.08f, .1f, .08f, .1f, 0, .2f); }, hex(0xFF7A9A));
        for (float s : {-1.f, 1.f})
            for (float dy : {-.04f, .06f})
                canvas.fill(
                    [=](float x, float y) { return segment(x, y, s * .2f, .16f + dy, s * .7f, .1f + dy * 2, .012f); },
                    hex(0x8A8AA6));
        p.talk(0, .4f, .26f);
        for (float s : {-1.f, 1.f})
            canvas.fill([=](float x, float y) { return box(x, y, s * .05f, .54f, .04f, .07f, .02f); }, Painter::white);
        break;
    }
    case Voice::Astronaut: {
        canvas.fill([](float x, float y) { return segment(x, y, .5f, -.82f, .62f, -1.12f, .03f); }, hex(0x8A94A6));
        canvas.fill([](float x, float y) { return circle(x, y, .63f, -1.15f, .06f); }, hex(0xFF5A5A));
        p.body(round, hex(0xFAFBFD), hex(0xC2CAD8), hex(0x6E7887));
        const auto visor = [](float x, float y) { return box(x, y, 0, -.02f, .68f, .5f, .4f); };
        canvas.fill([&](float x, float y) { return visor(x, y) - .05f; }, hex(0x8A94A6));
        canvas.fill(visor, [](float x, float y) {
            return mix(hex(0x34508A), hex(0x0A1028), std::clamp((y + .5f) + x * .2f, 0.f, 1.f));
        });
        for (float s : {-1.f, 1.f})
            canvas.fill([=](float x, float y) { return circle(x, y, s * .22f, -.05f, .07f); }, hex(0xBFE6FF), .9f);
        canvas.fill([](float x, float y) { return arc(x, y, 0, .02f, .18f, .5f, 2.64f, .03f); }, hex(0xBFE6FF), .9f);
        canvas.fill([&](float x, float y) { return std::max(visor(x, y), ellipse(x, y, -.32f, -.3f, .26f, .1f)); },
                    Painter::white, .6f);
        for (auto st : {std::array<float, 2>{.38f, -.28f}, {.5f, .2f}})
            canvas.fill(
                [=](float x, float y) {
                    return std::min(box(x, y, st[0], st[1], .05f, .008f, .004f),
                                    box(x, y, st[0], st[1], .008f, .05f, .004f));
                },
                Painter::white, .9f);
        for (float s : {-1.f, 1.f})
            p.part([=](float x, float y) { return box(x, y, s * .92f, .1f, .08f, .2f, .05f); }, hex(0xFF8A3C),
                   hex(0xE0601A), hex(0x7A3010), -.1f, .3f);
        break;
    }
    case Voice::Dragon: {
        for (float s : {-1.f, 1.f})
            for (int k = 0; k < 3; k++)
                p.part(
                    [=](float x, float y) {
                        const float bx = s * (.1f + k * .22f);
                        return triangle(x, y, bx - .1f, -.86f + k * .08f, bx + .1f, -.9f + k * .1f, bx + s * .02f,
                                        -1.14f + k * .1f);
                    },
                    hex(0xFFD45A), hex(0xF59A1A), hex(0x8A4A10), -1.14f, -.8f);
        for (float s : {-1.f, 1.f})
            p.part([=](float x, float y) { return triangle(x, y, s * .52f, -.62f, s * .82f, -.42f, s * 1.12f, -1.0f); },
                   hex(0xFFF6DE), hex(0xE2C48F), hex(0x8A6A3A), -1.0f, -.42f);
        p.body(round, hex(0xFF9A6A), hex(0xD8432A), hex(0x7A1F12));
        p.part([](float x, float y) { return ellipse(x, y, 0, .38f, .56f, .38f); }, hex(0xFFC8A0), hex(0xF28A5A),
               hex(0x9A3A1A), 0, .76f);
        for (float s : {-1.f, 1.f}) {
            p.part([=](float x, float y) { return ellipse(x, y, s * .36f, -.28f, .17f, .13f); }, hex(0xFFF27A),
                   hex(0xF5C023), hex(0x7A1F12), -.41f, -.15f);
            canvas.fill([=](float x, float y) { return ellipse(x, y, s * .36f, -.28f, .035f, .11f); }, Painter::ink);
            canvas.fill([=](float x, float y) { return segment(x, y, s * .16f, -.48f, s * .52f, -.42f, .04f); },
                        hex(0x7A1F12));
            canvas.fill([=](float x, float y) { return ellipse(x, y, s * .14f, .2f, .05f, .03f); }, hex(0x6A1A10));
        }
        if (p.mouth > .06f) {
            canvas.fill([&](float x, float y) { return ellipse(x, y, 0, .86f, .26f, .1f + .18f * p.mouth); },
                        hex(0xFF9A2A), .55f, .12f);
            canvas.fill([&](float x, float y) { return ellipse(x, y, 0, .84f, .14f, .06f + .1f * p.mouth); },
                        hex(0xFFE45A), .9f);
        }
        p.talk(0, .5f, .4f);
        for (float s : {-1.f, 1.f}) p.tooth(s * .14f - .05f, .45f, s * .14f + .05f, .45f, s * .14f, .58f);
        break;
    }
    case Voice::Baby: {
        p.body(round, hex(0xFFE6D0), hex(0xF5B48A), hex(0xB97A4E));
        canvas.fill([](float x, float y) { return arc(x, y, .08f, -.92f, .14f, -3.4f, 1.0f, .035f); }, hex(0xA0653D));
        for (float s : {-1.f, 1.f}) p.shiny_eye(s * .34f, -.12f, .16f, hex(0x7AB8FF), hex(0x2E6AD6));
        p.cheeks(.6f, .2f, .17f);
        if (p.mouth < .06f) {
            p.part([](float x, float y) { return ellipse(x, y, 0, .44f, .3f, .12f); }, hex(0xFFB6D0), hex(0xF07AA8),
                   hex(0x9A3A64), .32f, .56f);
            p.part([](float x, float y) { return circle(x, y, 0, .44f, .07f); }, hex(0xC8F0FF), hex(0x7AC8F0),
                   hex(0x2E7AA8), .37f, .51f);
            p.ring(0, .66f, .1f, .025f, hex(0xF07AA8));
        } else p.oo(0, .42f, .1f);
        break;
    }
    case Voice::Opera: {
        for (float s : {-1.f, 1.f})
            p.part(
                [=](float x, float y) { return triangle(x, y, s * .58f, -.48f, s * .82f, -.62f, s * 1.08f, -1.18f); },
                hex(0xFFF6DE), hex(0xE2C48F), hex(0x8A6A3A), -1.18f, -.48f);
        p.body(round, hex(0xFFE0C4), hex(0xF2B080), hex(0xA86A38));
        const auto helmet = [&](float x, float y) { return std::max(round(x, y) - .04f, y + .3f); };
        p.part(helmet, hex(0xE8EEF6), hex(0x8E9AAE), hex(0x3E4856), -1.05f, -.3f);
        canvas.fill([&](float x, float y) { return std::max(helmet(x, y), std::abs(x) - .06f); }, hex(0xF5C842));
        canvas.fill([](float x, float y) { return box(x, y, 0, -.32f, .97f, .06f, .05f); }, hex(0xF5C842));
        for (float s : {-1.f, 1.f}) p.happy_eye(s * .32f, -.08f, .1f);
        p.cheeks(.56f, .14f, .15f);
        const float saved = p.mouth;
        p.mouth = std::max(.25f, saved);
        p.oo(0, .42f, .12f);
        p.mouth = saved;
        p.note(.78f, .2f, .4f, hex(0xE0458A));
        break;
    }
    default: {
        p.body(round, hex(0xFFE36E), hex(0xF5A623), hex(0xC77D1A));
        for (float s : {-1.f, 1.f}) p.dot_eye(s * .33f, -.18f, .1f);
        p.cheeks(.6f, .18f, .17f);
        p.talk(0, .34f, .44f);
        break;
    }
    }
    return canvas;
}

} // namespace voice_faces
