// SPDX-License-Identifier: GPL-3.0-or-later
#include "companion/dock.hpp"

#include "launcher/system_icons.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <dirent.h>
#include <fstream>

namespace play {
namespace {

constexpr uint64_t open_ms = 280, fade_after_ms = 15000, fade_ms = 1500, faint_after_ms = 4000, toast_ms = 1800,
                   close_after_ms = 15000;

// The handheld's battery charge in percent, or -1 (no battery: a desktop).
int read_battery(bool& charging) {
    DIR* dir = opendir("/sys/class/power_supply");
    if (!dir) return -1;
    int found = -1;
    while (dirent* e = readdir(dir)) {
        if (e->d_name[0] == '.') continue;
        const std::string base = std::string("/sys/class/power_supply/") + e->d_name;
        std::ifstream type(base + "/type"), capacity(base + "/capacity");
        std::string t;
        int c = -1;
        if (type >> t && t == "Battery" && capacity >> c && c >= 0 && c <= 100) {
            found = c;
            std::ifstream status(base + "/status");
            std::string state;
            charging = status >> state && state == "Charging";
            break;
        }
    }
    closedir(dir);
    return found;
}

std::string clock_text(bool twelve) {
    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    char text[16];
    if (twelve)
        std::snprintf(text, sizeof text, "%d:%02d %s", local.tm_hour % 12 ? local.tm_hour % 12 : 12, local.tm_min,
                      local.tm_hour < 12 ? "AM" : "PM");
    else std::snprintf(text, sizeof text, "%02d:%02d", local.tm_hour, local.tm_min);
    return text;
}

// What the controls' colours and type depend on, for the drawing kept while they open.
uint64_t look_key(const Look& look) {
    uint64_t k = uint64_t(look.dark) | uint64_t(look.grid) << 1 | uint64_t(look.pixel_text) << 2 |
                 uint64_t(look.rich_icons) << 3 | uint64_t(look.high_contrast) << 4;
    for (Rgb c : {look.background, look.panel, look.ink, look.muted, look.accent, look.focus})
        k = k * 0x100000001B3ull ^ c.px();
    return k ^ std::hash<std::string>()(look.language);
}

// A picture shrunk to fit a w x h box, keeping its shape: each pixel the mean
// of the source pixels under it (alpha-weighted, so see-through edges stay clean).
Dock::Image fitted(const Dock::Image& src, int w, int h) {
    Dock::Image out;
    if (src.empty() || w <= 0 || h <= 0) return out;
    const double k = std::min(double(w) / src.w, double(h) / src.h);
    out.w = std::max(1, int(std::lround(src.w * k)));
    out.h = std::max(1, int(std::lround(src.h * k)));
    out.px.resize(size_t(out.w) * out.h);
    for (int y = 0; y < out.h; ++y) {
        const int y0 = y * src.h / out.h, y1 = std::max(y0 + 1, (y + 1) * src.h / out.h);
        for (int x = 0; x < out.w; ++x) {
            const int x0 = x * src.w / out.w, x1 = std::max(x0 + 1, (x + 1) * src.w / out.w);
            uint64_t r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int sy = y0; sy < y1; ++sy)
                for (int sx = x0; sx < x1; ++sx) {
                    const uint32_t v = src.px[size_t(sy) * src.w + sx], va = v >> 24;
                    r += (v & 0xFF) * va, g += (v >> 8 & 0xFF) * va, b += (v >> 16 & 0xFF) * va, a += va, ++n;
                }
            out.px[size_t(y) * out.w + x] =
                a ? uint32_t(r / a) | uint32_t(g / a) << 8 | uint32_t(b / a) << 16 | uint32_t(a / n) << 24 : 0;
        }
    }
    return out;
}

// A picture at x, y, blended by its own alpha.
void paint(Canvas& out, const Dock::Image& img, int x, int y, int alpha = 255) {
    for (int j = 0; j < img.h; ++j)
        for (int i = 0; i < img.w; ++i) {
            const uint32_t v = img.px[size_t(j) * img.w + i];
            const int a = int(v >> 24) * alpha / 255;
            if (a) out.fill_rect(x + i, y + j, 1, 1, Rgb::from_px(v), a);
        }
}

// A box blur of `radius`, rows then columns, in place.
void blur(Canvas& c, int radius) {
    const int W = c.w(), H = c.h();
    std::vector<uint32_t> line(size_t(std::max(W, H)));
    auto pass = [&](uint32_t* first, int count, int step) {
        for (int i = 0; i < count; ++i) line[size_t(i)] = first[size_t(i) * step];
        uint32_t r = 0, g = 0, b = 0;
        auto add = [&](int i, int sign) {
            const uint32_t v = line[size_t(std::clamp(i, 0, count - 1))];
            r += sign * (v & 0xFF), g += sign * (v >> 8 & 0xFF), b += sign * (v >> 16 & 0xFF);
        };
        for (int i = -radius; i <= radius; ++i) add(i, 1);
        const uint32_t n = uint32_t(radius * 2 + 1);
        for (int i = 0; i < count; ++i) {
            first[size_t(i) * step] = (r / n) | (g / n) << 8 | (b / n) << 16 | 0xFF000000u;
            add(i + radius + 1, 1);
            add(i - radius, -1);
        }
    };
    for (int y = 0; y < H; ++y) pass(c.data() + size_t(y) * W, W, 1);
    for (int x = 0; x < W; ++x) pass(c.data() + x, H, W);
}

int clock_minute() {
    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    return local.tm_hour * 60 + local.tm_min;
}

} // namespace

Dock::Dock(std::string title, std::string system) : title_(std::move(title)), system_(std::move(system)) {}

void Dock::set_art(const std::string& system_id, Image system_picture, Image cover) {
    system_id_ = system_id;
    system_picture_ = std::move(system_picture);
    cover_ = std::move(cover);
    thumb_ = {};
    system_small_ = fitted(system_picture_, 32, 32);
    ground_key_ = 0;
    layer_key_ = 0;
}

void Dock::set_full(bool full) {
    if (full == full_) return;
    full_ = full;
    open_ = false;
    touched_at_ = now_;
}

void Dock::set_brightness(int percent) {
    percent = std::clamp(percent, 10, 100);
    if (percent != brightness_) brightness_dirty_ = true;
    brightness_ = percent;
}

Dock::Action Dock::take_action() {
    // A hold completes on time, finger still down: the frame it does, the action goes out.
    if (held_item(held_) && !fired_ && now_ - held_at_ >= hold_ms) {
        fired_ = true;
        action_ = Action::Home;
    }
    // Controls left open over the companion close by themselves.
    if (open_ && !full_ && held_ == Item::None && !dragging_ && now_ - touched_at_ > close_after_ms) {
        open_ = false;
        closed_at_ = now_ | 1;
    }
    Action a = action_;
    action_ = Action::None;
    return a;
}

void Dock::report(const std::string& message) {
    toast_ = message;
    toast_at_ = now_;
}

void Dock::wake() { touched_at_ = now_; }

double Dock::openness() const {
    if (full_) return 1;
    if (!open_ && !closed_at_ && !opened_at_) return 0; // never opened
    const double t = double(now_ - (open_ ? opened_at_ : closed_at_)) / open_ms;
    const double e = t >= 1 ? 1 : 1 - (1 - t) * (1 - t) * (1 - t);
    return open_ ? e : 1 - e;
}

int Dock::shown_brightness() const {
    if (!full_) return brightness_;
    const uint64_t idle = now_ - touched_at_;
    if (idle < fade_after_ms) return brightness_;
    const int rest = std::max(10, brightness_ * 35 / 100);
    const double t = std::min(1.0, double(idle - fade_after_ms) / fade_ms);
    return int(std::lround(brightness_ + (rest - brightness_) * t));
}

bool Dock::moving() const {
    if (held_item(held_) && !fired_) return true;
    if (now_ - toast_at_ < toast_ms && !toast_.empty()) return true;
    if (!full_ && now_ - (open_ ? opened_at_ : closed_at_) < open_ms) return true;
    const uint64_t idle = now_ - touched_at_;
    if (full_ && idle >= fade_after_ms && idle < fade_after_ms + fade_ms) return true;
    if (!full_ && !open_ && idle >= faint_after_ms && idle < faint_after_ms + 700) return true;
    return false;
}

Box Dock::box(Item item, int w, int h) const {
    const int m = kit::margin, g = kit::gap, half = (w - m * 2 - g) / 2;
    switch (item) {
    case Item::Button: return {w - m - 56, h - m - 56, 56, 56};
    case Item::Switcher: return {m, 156, half, 96};
    case Item::Home: return {m + half + g, 156, half, 96};
    case Item::Bright: return {m, 264, w - m * 2, 88};
    // Over the companion the button keeps its corner.
    case Item::Off: return {m, 364, w - m * 2 - (full_ ? 0 : 56 + g), 72};
    default: return {};
    }
}

bool Dock::touch(const Touch& t) {
    const int W = 640, H = 480;
    if (off_) {
        // A touch on the black screen brings it back, and does nothing else.
        if (t.kind == Touch::Down) {
            off_ = false;
            wake();
        }
        return true;
    }
    auto inside = [&](Item item) { return box(item, W, H).contains(t.x, t.y); };
    const bool faded = full_ && now_ - touched_at_ >= fade_after_ms;
    if (!full_ && !open_) {
        if (t.kind == Touch::Down && inside(Item::Button)) {
            open_ = true;
            opened_at_ = now_ | 1;
            wake();
            return true;
        }
        if (t.kind == Touch::Down) wake(); // the button brightens while the companion is used
        return false;
    }
    if (t.kind == Touch::Down) {
        wake();
        if (faded) return true; // the first touch on a dimmed screen only wakes it
        if (!full_ && inside(Item::Button)) {
            open_ = false;
            closed_at_ = now_ | 1;
            return true;
        }
        for (Item item : {Item::Switcher, Item::Home, Item::Off})
            if (inside(item)) {
                held_ = item;
                held_at_ = now_;
                fired_ = false;
                return true;
            }
        if (inside(Item::Bright)) dragging_ = true;
        else if (!full_) {
            // A tap outside the controls closes them.
            open_ = false;
            closed_at_ = now_ | 1;
            return true;
        }
    }
    if (dragging_ && (t.kind == Touch::Down || t.kind == Touch::Move)) {
        const Box b = box(Item::Bright, W, H);
        const int left = b.x + 16 + kit::badge + 20, right = b.x + b.w - 92;
        set_brightness(10 + int(std::lround(90.0 * std::clamp(double(t.x - left) / (right - left), 0.0, 1.0))));
        wake();
        return true;
    }
    if (held_ != Item::None && t.kind == Touch::Move && !inside(held_)) held_ = Item::None; // slid off: cancelled
    if (t.kind == Touch::Up) {
        wake();
        if (held_ == Item::Switcher && inside(Item::Switcher)) {
            action_ = Action::Switcher;
            // The switcher opens over both screens; the companion is what comes back.
            if (!full_) {
                open_ = false;
                opened_at_ = closed_at_ = 0;
            }
        } else if (held_item(held_) && !fired_) report("Hold to return home"); // a tap is not enough
        if (held_ == Item::Off && inside(Item::Off)) off_ = true;
        held_ = Item::None;
        dragging_ = false;
    }
    return true;
}

uint64_t Dock::key() const {
    if (off_) return 0x0FF0FF;
    uint64_t k = (uint64_t(full_) << 1) | uint64_t(open_) | (uint64_t(brightness_) << 8) |
                 (uint64_t(shown_brightness()) << 16) | (uint64_t(dragging_) << 24);
    k ^= uint64_t(clock_minute()) << 32 ^ (played_ / 60) << 44;
    if (charging_ && open()) k ^= (1 + now_ / 420 % 4) << 26; // the charging bars fill in turn, as in the status bar
    k ^= std::hash<std::string>()(toast_) * (now_ - toast_at_ < toast_ms ? 3 : 0);
    if (!full_ && !open_) k ^= now_ - touched_at_ < faint_after_ms ? 0x5000 : 0;
    if (moving()) k ^= now_ * 0x9E3779B97F4A7C15ull;
    return k;
}

uint64_t Dock::controls_key() const {
    if (battery_ == -2 || now_ - battery_at_ > 30000) {
        battery_ = read_battery(charging_);
        battery_at_ = now_;
    }
    uint64_t k = uint64_t(brightness_) | uint64_t(dragging_) << 8 | uint64_t(held_) << 9 | uint64_t(fired_) << 13 |
                 uint64_t(twelve_hour_) << 14 | uint64_t(full_) << 15 | uint64_t(battery_ + 2) << 16 |
                 uint64_t(charging_ ? 1 + now_ / 420 % 4 : 0) << 24;
    k ^= uint64_t(clock_minute()) << 32 ^ (played_ / 60) << 44;
    if (held_item(held_) && !fired_) k ^= (now_ - held_at_ + 1) * 0x9E3779B97F4A7C15ull; // the ring fills
    return k;
}

void Dock::reveal(Canvas& out, const Canvas& layer, double cx, double cy, double radius) const {
    // Row by row: the span well inside the circle is copied as it is, only
    // the edge's two-pixel band is blended (a whole-screen distance test per
    // pixel cost the handheld most of a frame).
    const int W = out.w(), H = out.h();
    uint32_t* dst = out.data();
    const uint32_t* src = layer.data();
    auto blend = [](uint32_t d, uint32_t s, uint32_t a) {
        const uint32_t rb = (((s & 0xFF00FFu) * a + (d & 0xFF00FFu) * (256 - a)) >> 8) & 0xFF00FFu;
        const uint32_t g = (((s & 0x00FF00u) * a + (d & 0x00FF00u) * (256 - a)) >> 8) & 0x00FF00u;
        return 0xFF000000u | rb | g;
    };
    const double outer_r = radius + 1, inner_r = std::max(0.0, radius - 1);
    for (int y = 0; y < H; ++y) {
        const double dy = std::abs(y + .5 - cy);
        if (dy >= outer_r) continue;
        const double outer = std::sqrt(outer_r * outer_r - dy * dy);
        const double inner = inner_r > dy ? std::sqrt(inner_r * inner_r - dy * dy) : -1;
        const int x0 = std::max(0, int(std::floor(cx - outer))), x1 = std::min(W, int(std::ceil(cx + outer)));
        int i0 = x1, i1 = x1;
        if (inner >= 0) {
            i0 = std::clamp(int(std::ceil(cx - inner)), x0, x1);
            i1 = std::clamp(int(std::floor(cx + inner)), i0, x1);
        }
        const size_t row = size_t(y) * W;
        std::copy(src + row + i0, src + row + i1, dst + row + i0);
        for (int x = x0; x < x1; ++x) {
            if (x == i0) x = i1;
            if (x >= x1) break;
            const double d = std::hypot(x + .5 - cx, dy);
            if (d >= outer_r) continue;
            const uint32_t a = d <= inner_r ? 256 : uint32_t(256 * (outer_r - d) / 2);
            dst[row + x] = blend(dst[row + x], src[row + x], std::min(256u, a));
        }
    }
}

void Dock::draw(Canvas& out, const Look& look, const Font& font, const Words& words) const {
    if (!reachable_ && !full_) return;
    if (off_) {
        out.fill({0, 0, 0});
        return;
    }
    const double open = openness();
    if (full_) {
        ground(out, look);
        draw_controls(out, look, font, words);
    } else if (open > 0) {
        // The controls open out of the button, inside a circle that grows
        // from the button's centre over the companion. They are drawn once
        // for the whole opening, not again for every frame of it.
        const uint64_t k = controls_key() ^ look_key(look);
        if (layer_.w() != out.w() || layer_.h() != out.h() || k != layer_key_) {
            layer_.resize(out.w(), out.h());
            ground(layer_, look);
            draw_controls(layer_, look, font, words);
            layer_key_ = k;
        }
        const Box b = box(Item::Button, out.w(), out.h());
        const double cx = b.x + b.w / 2.0, cy = b.y + b.h / 2.0;
        const double R = look.animations && open < 1 ? open * (std::hypot(cx, cy) + 2) : 1e6;
        if (R >= 1e6) std::copy(layer_.data(), layer_.data() + size_t(out.w()) * out.h(), out.data());
        else reveal(out, layer_, cx, cy, R);
    }
    if (!full_) draw_button(out, look);
    if (!toast_.empty() && now_ - toast_at_ < toast_ms) {
        // A note of what happened, over everything, near the bottom.
        const std::string text = words(toast_);
        const int w = font.measure(text, kit::body) + 40, h = 44, x = (out.w() - w) / 2,
                  y = full_ || open_ ? out.h() - kit::margin - h - 2 : out.h() - kit::margin - h - 6;
        raised(out, look, x, y, w, h, h / 2, look.ink, 0);
        font.draw(out, x + 20, font.centred(kit::body, y + h / 2), text, kit::body, look.background);
    }
    if (const int level = shown_brightness(); level < 100) out.dim(level * 256 / 100);
}

void Dock::draw_button(Canvas& out, const Look& look) const {
    const Box b = box(Item::Button, out.w(), out.h());
    touch_target(b);
    // Faint once left alone; full again while the companion is touched.
    const uint64_t idle = now_ - touched_at_;
    double t = open_ ? 1 : idle < faint_after_ms ? 1 : 1 - std::min(1.0, double(idle - faint_after_ms) / 700);
    if (!look.animations && !open_) t = idle < faint_after_ms ? 1 : 0;
    const int alpha = int(90 + 165 * t);
    Canvas pill(b.w, b.h + 4);
    for (int y = 0; y < pill.h(); ++y)
        for (int x = 0; x < pill.w(); ++x) pill.data()[size_t(y * pill.w() + x)] = out.at(b.x + x, b.y + y);
    raised(pill, look, 0, 0, b.w, b.h, b.h / 2, open_ ? look.accent : look.panel, 0, true, false);
    icon(pill, look, open_ ? Icon::Close : Icon::More, b.w / 2, b.h / 2, 24, open_ ? look.on_accent : look.accent);
    for (int y = 0; y < pill.h(); ++y)
        for (int x = 0; x < pill.w(); ++x)
            out.fill_rect(b.x + x, b.y + y, 1, 1, Rgb::from_px(pill.data()[size_t(y * pill.w() + x)]), alpha);
}

void Dock::hold_button(Canvas& out, const Look& look, const Font& font, const Words& words, Item item, Icon glyph,
                       const std::string& name) const {
    const Box b = box(item, out.w(), out.h());
    const bool held = held_ == item && !fired_;
    const double progress = held ? std::min(1.0, double(now_ - held_at_) / hold_ms) : 0;
    raised(out, look, b.x, b.y, b.w, b.h, kit::radius, held ? look.focus : look.panel, held ? 1 : 0, true, false);
    const int sink = held ? 2 : 0, cx = b.x + 16 + kit::badge / 2, cy = b.y + b.h / 2 + sink;
    icon_badge(out, look, cx - kit::badge / 2, cy - kit::badge / 2, glyph, held);
    // The hold fills a ring round the badge; at the top, it is done.
    if (held && held_item(item)) ring(out, cx, cy, kit::badge / 2 + 7, 4, progress, look.accent);
    const int tx = b.x + 16 + kit::badge + 16;
    font.draw(out, tx, font.centred(kit::title, cy), font.fit(words(name), kit::title, b.x + b.w - 12 - tx), kit::title,
              look.ink);
}

void Dock::ground(Canvas& out, const Look& look) const {
    if (cover_.empty()) {
        backdrop(out, look);
        return;
    }
    const uint64_t k = look_key(look) ^ uint64_t(out.w()) << 48 ^ uint64_t(out.h()) << 36;
    if (ground_.w() != out.w() || ground_.h() != out.h() || k != ground_key_) {
        // The box art over the whole screen, soft and faint: a colour of the game, not a picture to read.
        ground_.resize(out.w(), out.h());
        backdrop(ground_, look);
        Canvas art(out.w(), out.h());
        art.cover(cover_.px.data(), cover_.w, cover_.h, cover_.w, 256);
        blur(art, 6);
        blur(art, 6);
        const int alpha = look.high_contrast ? 0 : look.dark ? 72 : 60;
        for (size_t i = 0; i < size_t(out.w()) * out.h(); ++i)
            ground_.data()[i] = mix(Rgb::from_px(ground_.data()[i]), Rgb::from_px(art.data()[i]), alpha).px();
        ground_key_ = k;
    }
    std::copy(ground_.data(), ground_.data() + size_t(out.w()) * out.h(), out.data());
}

void Dock::art(Canvas& out, const Look& look, const Box& b) const {
    if (cover_.empty()) {
        out.rounded_rect(b.x, b.y, b.w, b.h, 12, look.slot);
        system_icon(out, look, b.x + b.w / 12, b.y + b.h / 12, b.w - b.w / 6);
        return;
    }
    if (std::max(thumb_.w, thumb_.h) != std::min(b.w, b.h)) thumb_ = fitted(cover_, b.w, b.h);
    // A shadow under the box and a thin rim round it, so a pale cover still has an edge.
    const int ax = b.x + (b.w - thumb_.w) / 2, ay = b.y + (b.h - thumb_.h) / 2;
    soft_round(out, ax, ay + 3, thumb_.w, thumb_.h, 6, look.shadow(), look.dark ? 90 : 40);
    out.fill_rect(ax - 1, ay - 1, thumb_.w + 2, thumb_.h + 2, look.line);
    paint(out, thumb_, ax, ay);
}

void Dock::system_icon(Canvas& out, const Look& look, int x, int y, int size) const {
    const std::string_view glyph = rgds::system_pixel_glyph(system_id_);
    if (look.rich_icons && !system_picture_.empty()) {
        const Image& img = size == system_small_.h ? system_small_ : fitted(system_picture_, size, size);
        paint(out, img, x + (size - img.w) / 2, y + (size - img.h) / 2);
    } else if (glyph.size() == 256) {
        // The launcher's pixel style: a 16 x 16 glyph at a whole scale, '+' as a half tone.
        const int k = std::max(1, size / 16), left = x + (size - 16 * k) / 2, top = y + (size - 16 * k) / 2;
        for (int j = 0; j < 16; ++j)
            for (int i = 0; i < 16; ++i)
                if (const char c = glyph[size_t(j * 16 + i)]; c == '#' || c == '+')
                    out.fill_rect(left + i * k, top + j * k, k, k, look.ink, c == '#' ? 255 : 110);
    } else icon(out, look, Icon::Cartridge, x + size / 2, y + size / 2, size, look.accent);
}

void Dock::battery(Canvas& out, const Look& look, int x, int y) const {
    // As the launcher's status bar draws it: a shell, four bars, a nub; the bars fill in turn while charging.
    out.rounded_rect(x, y, 38, 20, 3, look.muted);
    out.rounded_rect(x + 2, y + 2, 34, 16, 2, look.slot);
    out.fill_rect(x + 38, y + 6, 3, 8, look.muted);
    int bars = battery_ > 75 ? 4 : battery_ > 45 ? 3 : battery_ > 15 ? 2 : battery_ > 0 ? 1 : 0;
    if (charging_) bars = 1 + int(now_ / 420 % 4);
    const Rgb level = battery_ <= 15 && !charging_ ? Rgb{239, 129, 75} : look.accent;
    for (int i = 0; i < 4; ++i)
        out.fill_rect(x + 5 + i * 7, y + 5, 5, 10, i < bars ? level : mix(look.slot, look.muted, 41));
    if (charging_)
        for (int j = 0; j < 19; ++j) {
            // The status bar's lightning, two strokes and a step.
            const int px = j < 9 ? x - 10 - j * 6 / 9 : j == 9 ? x - 16 : x - 9 - (j - 10) * 6 / 9;
            out.fill_rect(px, y + 1 + j, j == 9 ? 8 : 2, 1, look.accent);
        }
}

void Dock::draw_controls(Canvas& out, const Look& look, const Font& font, const Words& words) const {
    const int W = out.w(), m = kit::margin;
    for (Item item : {Item::Switcher, Item::Home, Item::Bright, Item::Off}) touch_target(box(item, W, out.h()));
    // The game: its box art (else its system), name, system and this session's time; the clock and battery.
    const Box top{m, kit::margin, W - m * 2, 120};
    card(out, look, top);
    const Box art{top.x + 12, top.y + 12, 96, 96};
    this->art(out, look, art);
    const std::string clock = clock_text(twelve_hour_);
    const int clock_w = font.measure(clock, kit::number), right = top.x + top.w - 18;
    const int tx = art.x + art.w + 16, text_w = right - clock_w - 20 - tx;
    font.draw(out, tx, font.centred(kit::large, top.y + 32), font.fit(title_, kit::large, text_w), kit::large,
              look.ink);
    int sx = tx;
    if (!cover_.empty()) {
        system_icon(out, look, tx, top.y + 50, 32);
        sx += 40;
    }
    font.draw(out, sx, font.centred(kit::small, top.y + 66), font.fit(system_, kit::small, right - sx), kit::small,
              look.ink);
    const unsigned long long minutes = played_ / 60;
    const std::string played = minutes < 60 ? std::to_string(minutes) + " " + words("min")
                                            : std::to_string(minutes / 60) + " " + words("h") + " " +
                                                  std::to_string(minutes % 60) + " " + words("min");
    font.draw(out, tx, font.centred(kit::small, top.y + 98),
              font.fit(words("Played") + " " + played, kit::small, right - tx - 100), kit::small, look.muted);
    font.draw(out, right - clock_w, font.centred(kit::number, top.y + 32), clock, kit::number, look.ink);
    controls_key(); // reads the battery now and then
    if (battery_ >= 0) {
        const std::string b = std::to_string(battery_) + "%";
        const int bw = font.measure(b, kit::small);
        battery(out, look, right - 41, top.y + 88);
        font.draw(out, right - 41 - (charging_ ? 22 : 8) - bw, font.centred(kit::small, top.y + 98), b, kit::small,
                  look.muted);
    }
    hold_button(out, look, font, words, Item::Switcher, Icon::Library, "Game switcher");
    hold_button(out, look, font, words, Item::Home, Icon::Home, "Return home");
    // Brightness: a slider, dragged.
    const Box br = box(Item::Bright, W, out.h());
    card(out, look, br, dragging_);
    icon_badge(out, look, br.x + 16, br.y + (br.h - kit::badge) / 2, Icon::Sun, dragging_);
    const int left = br.x + 16 + kit::badge + 20, track_r = br.x + br.w - 92, cy = br.y + br.h / 2 + 8;
    font.draw(out, left, br.y + 28, words("Brightness"), kit::small, look.muted);
    const double f = (brightness_ - 10) / 90.0;
    meter(out, look, {left, cy - 7, track_r - left, 14}, f, look.accent);
    const int knob = left + int(std::lround((track_r - left) * f));
    raised(out, look, knob - 14, cy - 14, 28, 28, 14, look.panel, dragging_ ? 1 : 0);
    const std::string pct = std::to_string(brightness_) + "%";
    font.draw(out, br.x + br.w - 18 - font.measure(pct, kit::body), font.centred(kit::body, br.y + br.h / 2), pct,
              kit::body, look.ink);
    // Screen off.
    const Box off = box(Item::Off, W, out.h());
    raised(out, look, off.x, off.y, off.w, off.h, kit::radius, held_ == Item::Off ? look.focus : look.panel,
           held_ == Item::Off ? 1 : 0, true, false);
    icon_badge(out, look, off.x + 16, off.y + (off.h - kit::badge) / 2, Icon::Sleep);
    const int ox = off.x + 16 + kit::badge + 16;
    font.draw(out, ox, font.centred(kit::title, off.y + off.h / 2), words("Screen off"), kit::title, look.ink);
}

} // namespace play
