// SPDX-License-Identifier: GPL-3.0-or-later
#include "companion/companion.hpp"
#include "companion/mode7.hpp"
#include "ui/kit.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>

namespace play {

Companion::Companion(Core& live, const Profile* profile, std::string title, std::string system_name)
    : profile_(profile), title_(std::move(title)), system_name_(std::move(system_name)), dock_(title_, system_name_) {
    if (profile_ && profile_->has_map) {
        capture_ = std::make_unique<MapCapture>(live, *profile_);
        if (!capture_->ready()) capture_.reset();
    }
    if (profile_ && profile_->has_party) party_ = std::make_unique<Party>(profile_->party);
    if (profile_)
        for (const auto& spec : profile_->views) {
            std::string error;
            auto view = make_view(spec, error);
            if (view) {
                view->set_hud(&hud_picture_);
                views_.push_back(std::move(view));
                auto alt = spec.values.find("alternative");
                alternative_.push_back(alt != spec.values.end() && (alt->second == "yes" || alt->second == "true"));
            } else std::fprintf(stderr, "play: %s: %s\n", profile_->file.c_str(), error.c_str());
        }
    // Last, so a profile's own view stays the one kept beside the save.
    if (auto keyboard = make_keyboard(live.system())) {
        views_.push_back(std::move(keyboard));
        alternative_.push_back(true);
    }
    hud_picture_.tint = [this](uint32_t v) { return shown_px(v); };
}

Companion::~Companion() {
    if (backdrop_thread_.joinable()) backdrop_thread_.join();
}

void Companion::take(MapImage img) {
    if (has_current_) {
        // Only a real change cross-fades; a marker that moved one room just swaps.
        bool same_size = img.w == current_.w && img.h == current_.h;
        int changed = 0;
        if (same_size)
            for (size_t i = 0; i < img.frames[0].size(); ++i)
                changed += (img.frames[0][i] & 0xFFFFFF) != (current_.frames[0][i] & 0xFFFFFF);
        if (!same_size || changed > static_cast<int>(img.frames[0].size() / 40)) {
            previous_ = std::move(current_);
            has_previous_ = true;
            shown_at_ = frame_;
        }
    } else {
        shown_at_ = frame_;
        has_previous_ = false;
    }
    current_ = std::move(img);
    has_current_ = true;
}

namespace {

// The BG layers that are HUD now: the profile's mask, or (games that move
// their menus between layers) the enabled layers below a priority.
unsigned hud_layers(Core& live, const HudSpec& h) {
    if (h.bg_priority_below <= 0) return h.bg_mask;
    const uint32_t display = live.peek(0x04000000, 2);
    unsigned mask = 0;
    for (int i = 0; i < 4; ++i)
        if ((display >> (8 + i)) & 1 && int(live.peek(0x04000008 + uint32_t(i) * 2, 2) & 3) < h.bg_priority_below)
            mask |= 1u << i;
    return mask;
}

bool map_layer_shown(Core& live, const HudSpec& h) {
    if (!h.map_cnt) return true;
    for (int i = 0; i < 4; ++i)
        if (live.peek(0x04000008 + uint32_t(i) * 2, 2) == h.map_cnt) return true;
    return false;
}

} // namespace

uint32_t Companion::machine_buttons(uint32_t pad) {
    if (!screens_mode() || profile_->screens.pointer) return pad;
    if (pad & Start) pad |= L;
    if (pad & Select) pad |= R;
    pad &= ~uint32_t(Start | Select);
    pad |= finger_;
    const uint32_t same = profile_->screens.same;
    if (pad & same) pad |= same;
    click_frames_ = pad & ~held_ ? 3 : std::max(0, click_frames_ - 1);
    held_ = pad;
    return pad;
}

bool Companion::picture_matches(int w, int h) const {
    const auto& s = profile_->screens;
    return !menu_over_game_ && (!s.match_w || (s.match_w == w && s.match_h == h));
}

void Companion::touch_top(const Touch& t) {
    if (!finger_core_ || !top_is_touched()) return;
    const Rect r = picture_matches(lower_w_, lower_h_) ? profile_->screens.top : Rect{0, 0, lower_w_, lower_h_};
    if (r.w <= 0 || r.h <= 0) return;
    const double fit = std::min(640.0 / r.w, 480.0 / r.h);
    const int x0 = int(640 - r.w * fit) / 2, y0 = int(480 - r.h * fit) / 2;
    finger_core_->set_pointer(r.x + int((t.x - x0) / fit), r.y + int((t.y - y0) / fit), t.kind != Touch::Up);
}

void Companion::update_screens(Core& live) {
    const Picture p = live.picture();
    const Picture& q = p;
    menu_over_game_ = live.menu_over_game();
    lower_w_ = q.w, lower_h_ = q.h;
    const Rect r = picture_matches(q.w, q.h) ? profile_->screens.bottom : Rect{0, 0, q.w, q.h};
    lower_rect_ = r;
    lower_.assign(static_cast<size_t>(r.w) * r.h, 0xFF000000u);
    uint64_t hash = 1469598103934665603ull;
    for (int y = std::max(0, -r.y); y < r.h && r.y + y < p.h; ++y) {
        const int from = std::max({0, -r.x, profile_->screens.bottom_from - r.x});
        const int to = std::min({r.w, p.w - r.x, profile_->screens.bottom_to - r.x});
        for (int x = from; x < to; ++x) {
            const uint32_t v = p.at(r.x + x, r.y + y) | 0xFF000000u;
            lower_[static_cast<size_t>(y) * r.w + x] = v;
            hash = (hash ^ v) * 1099511628211ull;
        }
    }
    lower_hash_ = hash;
}

void Companion::update_strip(Core& live) {
    const HudSpec& h = profile_->hud;
    const Picture p = live.picture();
    const Rect& s = h.strip;
    // With the second screen off the band stays in the game picture.
    bool on = shown_ && h.on.match(live) && s.y + s.h <= p.h && s.w <= p.w;
    if (!on) {
        // The last band stays up while `keep` holds (the inventory sliding in).
        hud_shown_ = shown_ && hud_shown_ && !h.keep.empty() && h.keep.match(live);
        hud_on_ = false;
        return;
    }
    hud_on_ = hud_shown_ = true;
    hud_w_ = s.w;
    hud_h_ = s.h;
    if (h.extend_sprites) {
        strip_sprites_.resize(544);
        for (uint32_t i = 0; i < 544; ++i) strip_sprites_[i] = uint8_t(peek_address(live, h.extend_sprites + i, 1));
        strip_places_.resize(size_t(h.places));
        for (int i = 0; i < h.places; ++i)
            strip_places_[size_t(i)] = {peek_address(live, h.places_x + h.places_step * uint32_t(i), 3) / 256.0,
                                        peek_address(live, h.places_y + h.places_step * uint32_t(i), 3) / 256.0};
    }
    hud_image_.resize(static_cast<size_t>(s.w) * s.h);
    uint64_t hash = 1469598103934665603ull;
    for (int y = 0; y < s.h; ++y)
        for (int x = 0; x < s.w; ++x) {
            uint32_t v = p.at(s.x + x, s.y + y) | 0xFF000000u;
            hud_image_[static_cast<size_t>(y) * s.w + x] = v;
            hash = (hash ^ v) * 1099511628211ull;
        }
    strip_hash_ = hash;
}

void Companion::update_hud(Core& live) {
    if (strip_mode()) {
        update_strip(live);
        return;
    }
    const HudSpec& h = profile_->hud;
    // With the second screen off the game keeps its whole HUD: nothing is split.
    bool on = shown_ && h.on.match(live) && map_layer_shown(live, h);
    const unsigned layers = hud_layers(live, h);
    if (on && hud_on_ && layers != hud_layers_) {
        // The game moved its menus to other layers: split those from the next frame.
        live.hud_split(true, layers, h.sprite_priority);
        hud_layers_ = layers;
    }
    if (on != hud_on_) {
        // From the next frame on; the HUD picture is good once one is drawn.
        if (on) {
            live.hud_objects(h.sprite_tiles);
            live.hud_window_effects_off(h.window_effects_off);
        }
        hud_layers_ = layers;
        if (!live.hud_split(on, layers, h.sprite_priority)) on = false;
        hud_on_ = on;
        hud_frames_ = 0;
        if (on) moves_ = false;
        else hud_shown_ = shown_ && hud_shown_ && !h.keep.empty() && h.keep.match(live);
        // A kept picture (the pause menu, the results) stays laid out as it was.
        if (!hud_shown_) moves_ = false;
        return;
    }
    if (!on) {
        if (hud_shown_ && (!shown_ || h.keep.empty() || !h.keep.match(live))) hud_shown_ = false;
        if (!hud_shown_) moves_ = false;
        return;
    }
    if (hud_frames_ == 0) drawn_.assign(h.panels.size(), Rect{});
    ++hud_frames_;
    Picture p = live.hud();
    if (!p.px) return;
    hud_w_ = p.w;
    hud_h_ = p.h;
    hud_key_ = live.hud_key();
    hud_image_.resize(static_cast<size_t>(p.w) * p.h);
    for (int y = 0; y < p.h; ++y)
        std::copy_n(p.px + static_cast<size_t>(y) * p.stride, p.w, hud_image_.begin() + static_cast<size_t>(y) * p.w);
    hud_shown_ = true;
    hud_picture_.px = hud_image_.data();
    hud_picture_.w = hud_w_;
    hud_picture_.h = hud_h_;
    hud_picture_.key = hud_key_;
    moves_ = !h.to_game.empty() && (h.moves_when.empty() || h.moves_when.match(live));
    if (!moves_) return;
    if (h.backdrop_game && h.backdrop_layers && (backdrop_.empty() || frame_ >= backdrop_due_)) make_backdrop(live);
    // Grow each trimmed panel's box to what the HUD drew in it (never shrinks
    // during a race, so the layout holds still as the dots move).
    drawn_.resize(h.panels.size());
    for (size_t i = 0; i < h.panels.size(); ++i) {
        if (!h.panels[i].trim) continue;
        const Rect& f = h.panels[i].from;
        int x0 = hud_w_, y0 = hud_h_, x1 = -1, y1 = -1;
        for (int y = std::max(0, f.y); y < std::min(hud_h_, f.y + f.h); ++y)
            for (int x = std::max(0, f.x); x < std::min(hud_w_, f.x + f.w); ++x)
                if ((hud_image_[static_cast<size_t>(y) * hud_w_ + x] & 0xFFFFFF) != hud_key_) {
                    x0 = std::min(x0, x);
                    x1 = std::max(x1, x);
                    y0 = std::min(y0, y);
                    y1 = std::max(y1, y);
                }
        if (x1 < 0) continue;
        Rect& d = drawn_[i];
        if (d.empty()) d = {x0, y0, x1 - x0 + 1, y1 - y0 + 1};
        else {
            int dx1 = std::max(d.x + d.w - 1, x1), dy1 = std::max(d.y + d.h - 1, y1);
            d.x = std::min(d.x, x0);
            d.y = std::min(d.y, y0);
            d.w = dx1 - d.x + 1;
            d.h = dy1 - d.y + 1;
        }
    }
}

void Companion::make_backdrop(Core& live) {
    const HudSpec& h = profile_->hud;
    if (backdrop_thread_.joinable()) backdrop_thread_.join(); // the last one is long done by now
    backdrop_due_ = frame_ + 600;                             // every ten seconds or so, so it follows the course round
    if (!backdrop_shadow_) backdrop_shadow_ = live.shadow();
    std::vector<uint8_t> state;
    if (!backdrop_shadow_ || !live.save_state(state)) return;
    backdrop_working_ = true;
    backdrop_thread_ =
        std::thread([this, state = std::move(state), layers = h.backdrop_layers, rows = h.backdrop_rows] {
            struct Done {
                std::atomic<bool>& flag;
                ~Done() { flag = false; }
            } done{backdrop_working_};
            if (!backdrop_shadow_->load_state(state)) return;
            backdrop_shadow_->set_layers(layers);
            backdrop_shadow_->run_frame(0);
            Picture p = backdrop_shadow_->picture();
            int h = rows > 0 ? std::min(rows, p.h) : p.h;
            std::vector<uint32_t> px(static_cast<size_t>(p.w) * h);
            for (int y = 0; y < h; ++y)
                std::copy_n(p.px + static_cast<size_t>(y) * p.stride, p.w, px.begin() + static_cast<size_t>(y) * p.w);
            std::lock_guard<std::mutex> g(backdrop_lock_);
            backdrop_.swap(px);
            backdrop_w_ = p.w;
            backdrop_h_ = h;
            ++backdrop_serial_;
        });
}

namespace {

// The karts and items behind the player, which the game leaves out of its race
// view and shows only on its map (the next Mode 7 run, the lower picture). A
// sprite's feet there are a place on the course; the extension's row that looks
// at that place is where it stands, as much larger as that row is nearer than
// the race view's last one (mats[0], the rest the extension's rows). Drawn far
// to near, from the sprite's own tiles in video memory, in the sprite palette.
void draw_behind(const Core::Mode7& m, const uint8_t* oam, const std::vector<std::array<double, 2>>& places,
                 const std::vector<int>& tiles, int e, int kept, const std::vector<std::array<int, 4>>& mats,
                 const uint32_t* colours, std::vector<uint32_t>& out) {
    using namespace mode7;
    int ms = 0, me = 0;
    if (!map_lines(m, e, ms, me)) return;
    const int extra = int(mats.size()) - 1;
    std::vector<Ray> rays(mats.size());
    for (size_t i = 0; i < mats.size(); ++i) rays[i] = ray(mats[i].data(), line(m, e), e + int(i) + 1);
    struct Thing {
        Sprite sprite;
        bool on_map = false;
        double px = 0, py = 0; // its place on the course
        int place = -1;        // the kept place it stands on
        double row = 0, at = 0, scale = 1;
    };
    std::vector<Thing> things;
    for (Sprite& sprite : sprites(oam, m.obj_sizes)) things.push_back({std::move(sprite)});
    // Where each is on the course: the map centres a sprite on its place (a
    // sprite at Y starts on row Y), to a map pixel: several course units.
    for (Thing& t : things) {
        const int place = t.sprite.y + t.sprite.h / 2 + m.first_line;
        if (place < ms || place > me) continue;
        course_at(m, place, t.sprite.x + t.sprite.w / 2.0, t.px, t.py);
        t.on_map = true;
    }
    // Where the game keeps a place exactly, that: the sprite that shows it is
    // the one drawn from its tiles.
    for (Thing& t : things)
        for (size_t q = 0; t.on_map && q < places.size() && q < tiles.size(); ++q)
            if (t.sprite.drawn_from(tiles[q])) {
                t.place = int(q);
                t.px = places[q][0], t.py = places[q][1];
            }
    // It stands between the two rows of the extension its place lies between.
    const double unit = std::hypot(mats[0][0], mats[0][2]);
    std::vector<Thing*> standing;
    for (Thing& t : things) {
        // The first place is the player's, whose kart the race view draws itself.
        if (!t.on_map || t.place == 0) continue;
        auto side = [&](size_t j) {
            const Ray& q = rays[j];
            return mats[j][0] * (t.py - (q.cc + q.dd) / 256.0) - mats[j][2] * (t.px - (q.aa + q.bb) / 256.0);
        };
        const double first = side(0);
        for (size_t j = 1; j < rays.size(); ++j) {
            const double now = side(j), before = side(j - 1);
            if ((now < 0) == (first < 0)) continue;
            const Ray& q = rays[j];
            const double dx = mats[j][0] / 256.0, dy = mats[j][2] / 256.0;
            t.row = double(j - 1) + before / (before - now);
            t.at = ((t.px - (q.aa + q.bb) / 256.0) * dx + (t.py - (q.cc + q.dd) / 256.0) * dy) / (dx * dx + dy * dy);
            t.scale = std::clamp(unit / std::max(1.0, std::hypot(mats[j][0], mats[j][2])), 1.0, 4.0);
            // Without the places, the race view's own kart is the one on its last rows.
            if (!places.empty() || t.row >= 3) standing.push_back(&t);
            break;
        }
    }
    std::sort(standing.begin(), standing.end(), [](const Thing* a, const Thing* b) { return a->row < b->row; });
    const int rows = kept + extra;
    for (const Thing* t : standing) {
        const Sprite& sp = t->sprite;
        const int W = int(std::lround(sp.w * t->scale)), H = int(std::lround(sp.h * t->scale));
        // The game stands a racer a quarter of its height under its place, 3 pixels
        // right of where the road has it (Mario's own kart, measured).
        const int left = int(std::lround(t->at + 3 * t->scale - W / 2.0));
        const int top = kept - 1 + int(std::lround(t->row + H / 4.0)) - H + 1;
        for (int dy = std::max(0, -top); dy < H && top + dy < rows; ++dy)
            for (int dx = std::max(0, -left); dx < W && left + dx < 256; ++dx)
                if (const int c = pixel(m, sp, dx * sp.w / W, dy * sp.h / H))
                    out[size_t(top + dy) * 256 + size_t(left + dx)] = colours[c];
    }
}

// More of a Mode 7 race than the game draws (Super Mario Kart's road under its
// race view). The race (mode7::race_lines) is a run of scanlines that share
// one centre and scroll; its matrix shrinks with distance as
// a / (line - l0), so the rows after it continue that curve from the course
// in video memory, with the core's own arithmetic (its rounding to 1/64 and
// line + 1), in its palette. With `oam`, the lower picture's sprites stand on
// it (draw_behind). `kept` is how many of the game's rows stay (the race itself).
bool extend_mode7(const Core::Mode7& m, const uint8_t* oam, const std::vector<std::array<double, 2>>& places,
                  const std::vector<int>& tiles, const Picture& game, int extra, std::vector<uint32_t>& out,
                  int& kept) {
    if (game.w != 256 || extra <= 0) return false;
    using namespace mode7;
    int s = -1, e = -1;
    if (!race_lines(m, s, e)) return false;
    kept = std::min(game.h, e - m.first_line + 1);
    // One l0 for the four, then a for each by least squares: the game's lines
    // are a plain perspective (no constant: 1 / size falls on a straight line),
    // and a constant fitted from the far lines bends the near rows away.
    // Each value's squared error at its best a is sum(v²) - sxv² / sxx, and sum(v²) is the same for every l0: the
    // best l0 has the largest sum of sxv² / sxx, one pass over the lines each (a race frame's 2.8 ms on the
    // handheld's A55 with the error summed out, 29 October 2026).
    const int from = s + 2;
    double best = -1, best_l0 = 0, fa[4] = {};
    for (double l0 = s - 60; l0 < s - 0.2; l0 += 0.25) {
        double sxx = 0, sxv[4] = {};
        for (int l = from; l <= e; ++l) {
            const double u = 1 / (l - l0);
            sxx += u * u;
            for (int k = 0; k < 4; ++k) sxv[k] += line(m, l)[k] * u;
        }
        const double fit = (sxv[0] * sxv[0] + sxv[1] * sxv[1] + sxv[2] * sxv[2] + sxv[3] * sxv[3]) / sxx;
        if (fit > best) {
            best = fit, best_l0 = l0;
            for (int k = 0; k < 4; ++k) fa[k] = sxv[k] / sxx;
        }
    }
    // The palette as the picture has it (the core's BGR555 through its RGB565).
    uint32_t colours[256];
    for (int i = 0; i < 256; ++i) colours[i] = colour(m.palette[i]);
    out.resize(size_t(game.w) * size_t(kept + extra));
    for (int y = 0; y < kept; ++y)
        std::copy_n(game.px + size_t(y) * game.stride, game.w, out.begin() + size_t(y) * game.w);
    std::vector<std::array<int, 4>> mats(size_t(extra) + 1);
    for (int j = 0; j < 4; ++j) mats[0][size_t(j)] = line(m, e)[j];
    for (int i = 0; i < extra; ++i) {
        const int l = e + 1 + i;
        int* mat = mats[size_t(i) + 1].data();
        for (int j = 0; j < 4; ++j) mat[j] = int(std::lround(std::clamp(fa[j] / (l - best_l0), -32768.0, 32767.0)));
        const Ray r = ray(mat, line(m, e), l + 1);
        int aa = r.aa, cc = r.cc;
        uint32_t* row = out.data() + size_t(kept + i) * game.w;
        for (int x = 0; x < 256; ++x, aa += mat[0], cc += mat[2])
            row[x] = colours[course_index(m.vram, (aa + r.bb) >> 8, (cc + r.dd) >> 8)];
    }
    if (oam && m.objsel) draw_behind(m, oam, places, tiles, e, kept, mats, colours, out);
    return true;
}

} // namespace

Picture Companion::compose_game(const Picture& game, bool whole) {
    if (screens_mode()) {
        if (!shown_ || !picture_matches(game.w, game.h)) return game; // the whole picture here
        const Rect& r = profile_->screens.top;
        const int keep_from = profile_->screens.top_from, keep_to = profile_->screens.top_to;
        if (r.x >= keep_from && r.y >= 0 && r.x + r.w <= std::min(game.w, keep_to) && r.y + r.h <= game.h)
            return {game.px + static_cast<size_t>(r.y) * game.stride + r.x, r.w, r.h, game.stride};
        composed_.assign(static_cast<size_t>(r.w) * r.h, 0xFF000000u);
        for (int y = std::max(0, -r.y); y < r.h && r.y + y < game.h; ++y) {
            const int from = std::max({0, -r.x, keep_from - r.x}), to = std::min({r.w, game.w - r.x, keep_to - r.x});
            if (to > from)
                std::copy_n(game.px + static_cast<size_t>(r.y + y) * game.stride + r.x + from, to - from,
                            composed_.data() + static_cast<size_t>(y) * r.w + from);
        }
        return {composed_.data(), r.w, r.h, r.w};
    }
    if (strip_mode()) {
        // The band leaves the picture only where it spans it, at its top or bottom edge.
        const Rect& s = profile_->hud.strip;
        if (whole || !hud_on_ || s.w != game.w || s.h >= game.h) return game;
        // The race view with the road the game never draws under it, where the core can show it.
        Core::Mode7 m;
        int kept = 0;
        if (profile_->hud.extend > 0 && s.y + s.h == game.h && live_ && live_->mode7(m) &&
            extend_mode7(m, strip_sprites_.empty() ? nullptr : strip_sprites_.data(), strip_places_,
                         profile_->hud.place_tiles, game, profile_->hud.extend, composed_, kept))
            return {composed_.data(), game.w, kept + profile_->hud.extend, game.w};
        // What is left, less a gap between the two pictures (game_rows).
        const int rest = profile_->hud.game_rows > 0 ? std::min(profile_->hud.game_rows, game.h - s.h) : game.h - s.h;
        if (s.y == 0) return {game.px + static_cast<size_t>(game.h - rest) * game.stride, game.w, rest, game.stride};
        if (s.y + s.h == game.h) return {game.px, game.w, rest, game.stride};
        return game;
    }
    // Only while split: otherwise the game draws its own HUD, item box and place included.
    if (!hud_on_ || hud_image_.empty() || game.w != hud_w_ || game.h != hud_h_) return game;
    // A board that lays the HUD out itself claims its boxes; the rest of the HUD layer stays here.
    View* board = hud_covered() ? shown_view() : nullptr;
    if (!board && !moves_) return game;
    composed_.resize(static_cast<size_t>(game.w) * game.h);
    for (int y = 0; y < game.h; ++y)
        std::copy_n(game.px + static_cast<size_t>(y) * game.stride, game.w,
                    composed_.begin() + static_cast<size_t>(y) * game.w);
    if (board) {
        const std::vector<Rect> claims = board->hud_claims();
        for (int y = 0; y < game.h; ++y)
            for (int x = 0; x < game.w; ++x) {
                const uint32_t v = hud_image_[static_cast<size_t>(y) * hud_w_ + x];
                if ((v & 0xFFFFFF) == hud_key_) continue;
                bool claimed = false;
                for (const auto& c : claims)
                    if (x >= c.x && y >= c.y && x < c.x + c.w && y < c.y + c.h) claimed = true;
                if (!claimed) composed_[static_cast<size_t>(y) * game.w + x] = v;
            }
        return {composed_.data(), game.w, game.h, game.w};
    }
    for (const auto& m : profile_->hud.to_game)
        for (int y = 0; y < m.from.h; ++y)
            for (int x = 0; x < m.from.w; ++x) {
                int sx = m.from.x + x, sy = m.from.y + y, tx = m.x + x, ty = m.y + y;
                if (sx < 0 || sy < 0 || sx >= hud_w_ || sy >= hud_h_ || tx < 0 || ty < 0 || tx >= game.w ||
                    ty >= game.h)
                    continue;
                uint32_t v = hud_image_[static_cast<size_t>(sy) * hud_w_ + sx];
                if ((v & 0xFFFFFF) != hud_key_) composed_[static_cast<size_t>(ty) * game.w + tx] = v;
            }
    return {composed_.data(), game.w, game.h, game.w};
}

namespace {

// A picture at a size that isn't a whole multiple, the way the game screen's
// sharp fit does it: each pixel grown to the next whole size, then smoothed
// down to tw x th, so edges stay crisp and columns stay even. With `keyed`,
// pixels of the key colour are see-through, and edges blend into what is under.
void sharp_blit(Canvas& out, const uint32_t* px, int w, int h, int ox, int oy, int tw, int th, bool keyed,
                uint32_t key) {
    if (tw <= 0 || th <= 0) return;
    const int k = std::max(1, int(std::max(double(tw) / w, double(th) / h)) + 1), bw = w * k, bh = h * k;
    struct Tap {
        int at, next, w; // the two source pixels, and how much of the second (of 256)
    };
    auto taps = [k](int out_n, int big_n) {
        std::vector<Tap> t(static_cast<size_t>(out_n));
        for (int i = 0; i < out_n; ++i) {
            const double f = (i + 0.5) * big_n / out_n - 0.5;
            const int b0 = std::clamp(int(f), 0, big_n - 1), b1 = std::min(b0 + 1, big_n - 1);
            t[size_t(i)] = {b0 / k, b1 / k, int(std::clamp(f - b0, 0.0, 1.0) * 256 + 0.5)};
        }
        return t;
    };
    const std::vector<Tap> cols = taps(tw, bw), rows = taps(th, bh);
    uint32_t* dst = out.data();
    for (int y = 0; y < th; ++y) {
        if (oy + y < 0 || oy + y >= out.h()) continue;
        const Tap& r = rows[size_t(y)];
        const uint32_t* a = px + size_t(r.at) * size_t(w);
        const uint32_t* c = px + size_t(r.next) * size_t(w);
        uint32_t* line = dst + size_t(oy + y) * size_t(out.w());
        for (int x = 0; x < tw; ++x) {
            if (ox + x < 0 || ox + x >= out.w()) continue;
            const Tap& t = cols[size_t(x)];
            const uint32_t p[4] = {a[t.at], a[t.next], c[t.at], c[t.next]};
            const int wt[4] = {(256 - t.w) * (256 - r.w), t.w * (256 - r.w), (256 - t.w) * r.w, t.w * r.w};
            uint32_t sum[3] = {0, 0, 0}, weight = 0;
            for (int i = 0; i < 4; ++i) {
                if (keyed && (p[i] & 0xFFFFFF) == key) continue;
                weight += uint32_t(wt[i]);
                for (int ch = 0; ch < 3; ++ch) sum[ch] += ((p[i] >> (8 * ch)) & 255) * uint32_t(wt[i]);
            }
            if (!weight) continue;
            uint32_t& d = line[ox + x];
            uint32_t v = 0xFF000000u;
            for (int ch = 0; ch < 3; ++ch) {
                // Seen through as much as the key colour made up of this pixel.
                const uint32_t src = sum[ch] / weight, under = (d >> (8 * ch)) & 255;
                v |= ((src * weight + under * (65536 - weight)) >> 16) << (8 * ch);
            }
            d = v;
        }
    }
}

} // namespace

bool Companion::render_hud(Canvas& out, const Look& look) const {
    if (!hud_shown_ || hud_image_.empty()) return false;
    const HudSpec& h = profile_->hud;
    Picture game = live_ ? live_->picture() : Picture{};
    // Behind the HUD: the race's own sky, dark, so the second screen moves with it.
    int rows = h.backdrop_rows > 0 ? std::min(h.backdrop_rows, game.h) : game.h;
    bool have_backdrop = false;
    if (h.backdrop_game && h.backdrop_layers) {
        std::lock_guard<std::mutex> g(backdrop_lock_);
        if (!backdrop_.empty() && (backdrop_panel_serial_ != backdrop_serial_ || backdrop_panel_.w() != out.w() ||
                                   backdrop_panel_.h() != out.h())) {
            backdrop_panel_.resize(out.w(), out.h());
            backdrop_panel_.cover(backdrop_.data(), backdrop_w_, backdrop_h_, backdrop_w_, 90);
            backdrop_panel_serial_ = backdrop_serial_;
        }
        have_backdrop = !backdrop_.empty();
    }
    if (have_backdrop) std::copy_n(backdrop_panel_.data(), static_cast<size_t>(out.w()) * out.h(), out.data());
    else if (h.backdrop_game && game.px) out.cover(game.px, game.w, rows, game.stride, 90);
    else if (h.backdrop_theme) backdrop(out, look);
    else out.fill(h.backdrop_game ? look.background : Rgb::hex(h.backdrop));
    // What went back to the game screen is not shown twice.
    std::vector<uint32_t> rest;
    if (moves_) {
        rest = hud_image_;
        for (const auto& m : h.to_game)
            for (int y = std::max(0, m.from.y); y < std::min(hud_h_, m.from.y + m.from.h); ++y)
                std::fill_n(rest.begin() + static_cast<size_t>(y) * hud_w_ + std::max(0, m.from.x),
                            std::max(0, std::min(hud_w_, m.from.x + m.from.w) - std::max(0, m.from.x)), hud_key_);
    }
    if (moves_ && !h.panels.empty()) {
        // The profile's layout: each box at the largest whole scale its slot allows.
        const double sx = out.w() / 640.0, sy = out.h() / 480.0;
        std::vector<uint32_t> box;
        for (size_t i = 0; i < h.panels.size(); ++i) {
            const HudPanel& panel = h.panels[i];
            Rect f = panel.trim ? (i < drawn_.size() ? drawn_[i] : Rect{}) : panel.from;
            f.x = std::max(0, f.x);
            f.y = std::max(0, f.y);
            f.w = std::min(f.w, hud_w_ - f.x);
            f.h = std::min(f.h, hud_h_ - f.y);
            if (f.empty()) continue;
            int tx = int(panel.to.x * sx), ty = int(panel.to.y * sy), tw = int(panel.to.w * sx),
                th = int(panel.to.h * sy);
            // The dock's corner stays free: the box gives up its right or its bottom, whichever costs less.
            const int cx = out.w() - look.reserve_w, cy = out.h() - look.reserve_h;
            if (look.reserve_w && tx + tw > cx && ty + th > cy) {
                const int narrower = (cx - kit::gap / 2 - tx) * th, shorter = tw * (cy - kit::gap / 2 - ty);
                if (narrower >= shorter) tw = cx - kit::gap / 2 - tx;
                else th = cy - kit::gap / 2 - ty;
            }
            int scale = std::max(1, std::min(tw / f.w, th / f.h));
            box.resize(static_cast<size_t>(f.w) * f.h);
            for (int y = 0; y < f.h; ++y)
                std::copy_n(rest.begin() + static_cast<size_t>(f.y + y) * hud_w_ + f.x, f.w,
                            box.begin() + static_cast<size_t>(y) * f.w);
            out.blit_keyed(box.data(), f.w, f.h, tx + (tw - f.w * scale) / 2, ty + (th - f.h * scale) / 2, scale,
                           hud_key_);
        }
        return true;
    }
    // The whole HUD as large as the panel's width takes it (2.67x for a GBA screen on 640x480).
    const double fit = std::min(double(out.w()) / hud_w_, double(out.h()) / hud_h_);
    const int tw = std::min(out.w(), int(hud_w_ * fit + 0.5)), th = std::min(out.h(), int(hud_h_ * fit + 0.5));
    int x0 = (out.w() - tw) / 2, y0 = (out.h() - th) / 2;
    // Clear of the dock's corner: up a little when there is room above.
    if (look.reserve_w && x0 + tw > out.w() - look.reserve_w && y0 + th > out.h() - look.reserve_h)
        y0 = std::max(0, out.h() - look.reserve_h - th);
    sharp_blit(out, moves_ ? rest.data() : hud_image_.data(), hud_w_, hud_h_, x0, y0, tw, th, true, hud_key_);
    return true;
}

void Companion::after_frame(Core& live) {
    ++frame_;
    live_ = &live;
    finger_core_ = &live;
    if (screens_mode()) update_screens(live);
    if (profile_ && profile_->has_hud) update_hud(live);
    if (!hud_shown_) hud_picture_.px = nullptr;
    if (party_ && frame_ % 15 == 0) {
        party_->read(live);
        party_shown_ = !party_->empty() && (profile_->party.on.empty() || profile_->party.on.match(live));
    }
    for (auto& v : views_) v->update(live, frame_);
    dock_.set_played(frame_ / 60);
    if (!capture_) return;
    MapImage img;
    if (capture_->poll(current_.serial, img)) take(std::move(img));

    if ((frame_ & 7) == 0 && !profile_->menu.empty()) {
        bool menu = profile_->menu.match(live);
        if (menu && !menu_) {
            // Back on the title or file screens: the old map belongs to another run.
            has_current_ = has_previous_ = false;
        }
        menu_ = menu;
    }
    if (menu_) return;

    if ((frame_ & 3) == 0) {
        bool dark = picture_brightness(live.picture()) < 10;
        if (dark) in_fade_ = true;
        else if (in_fade_) {
            in_fade_ = false;
            due_ = std::min(due_, frame_ + 12); // a room transition just ended
        }
        if (!profile_->room.empty()) {
            uint64_t key = 1469598103934665603ull;
            for (const auto& r : profile_->room) key = (key ^ r.read(live)) * 1099511628211ull;
            if (key != room_key_) {
                if (room_key_) due_ = std::min(due_, frame_ + 10);
                room_key_ = key;
            }
        }
    }
    if (frame_ >= next_periodic_) due_ = std::min(due_, frame_);
    if (frame_ < due_ || in_fade_) return;
    if (!profile_->live.empty() && !profile_->live.match(live)) return;
    if (capture_->request(live)) {
        ++requests_;
        next_periodic_ = frame_ + static_cast<uint64_t>(profile_->interval);
        due_ = UINT64_MAX;
    }
}

bool Companion::capture_now(Core& live) {
    if (!capture_) return false;
    MapImage img = capture_->capture_now(live);
    if (!img.ok) return false;
    take(std::move(img));
    return true;
}

const uint32_t* Companion::shown(const uint32_t* px, int w, int h, std::vector<uint32_t>& buffer) const {
    if (!colour_.active()) return px;
    const Picture p = colour_.apply({px, w, h, w});
    buffer.assign(p.px, p.px + static_cast<size_t>(w) * h);
    return buffer.data();
}

uint32_t Companion::shown_px(uint32_t v) const {
    if (!colour_.active()) return v;
    return *colour_.apply({&v, 1, 1, 1}).px;
}

View* Companion::shown_view() const {
    for (size_t i = 0; i < views_.size(); ++i)
        if (!alternative_[i] && views_[i]->shown()) return views_[i].get();
    return nullptr;
}

View* Companion::alternative_view() const {
    for (size_t i = 0; i < views_.size(); ++i)
        if (alternative_[i] && views_[i]->shown()) return views_[i].get();
    return nullptr;
}

bool Companion::hud_covered() const {
    // A board laid out from the HUD's pieces and the game's memory, in place of the HUD itself.
    View* v = shown_view();
    return !has_current_ && v && v->covers_hud();
}

bool Companion::has_switch() const { return alternative_view() && (has_current_ || shown_view() || party_shown_); }

bool Companion::showing_alternative() const {
    // The alternative when chosen, or when there is nothing else to show.
    return alternative_view() && (alt_ || !(has_current_ || shown_view() || party_shown_));
}

Box Companion::switch_box(int w) const { return {w - kit::margin - 56, kit::margin, 56, 56}; }

bool Companion::touch(const Touch& t) {
    const uint64_t before = dock_.key();
    if (dock_.touch(t)) return dock_.key() != before;
    if (screens_mode()) {
        const Rect& r = lower_rect_.w ? lower_rect_ : profile_->screens.bottom;
        const double fit = std::min(640.0 / r.w, 480.0 / r.h);
        const double tall = std::min(480.0 / r.h, fit * (lower_w_ == r.w && lower_h_ == r.h ? 1 : profile_->screens.stretch));
        const int x = r.x + int((t.x - (640 - r.w * fit) / 2) / fit), y = r.y + int((t.y - (480 - r.h * tall) / 2) / tall);
        if (profile_->screens.pointer) {
            if (finger_core_) finger_core_->set_pointer(x, y, t.kind != Touch::Up);
            return false;
        }
        finger_ = 0;
        if (t.kind != Touch::Up)
            for (const auto& zone : profile_->screens.touch)
                if (x >= zone.box.x && y >= zone.box.y && x < zone.box.x + zone.box.w && y < zone.box.y + zone.box.h)
                    finger_ |= zone.buttons;
        return false;
    }
    if (t.kind == Touch::Down && has_switch() && switch_box(640).contains(t.x, t.y)) {
        // The switch flips between the game's own screen (map, team) and the status board.
        alt_ = !alt_;
        return true;
    }
    if (View* a = showing_alternative() ? alternative_view() : nullptr; a && a->touch(t)) {
        ++touch_serial_;
        return true;
    }
    View* v = has_current_ ? nullptr : shown_view();
    if (v && v->touch(t)) {
        ++touch_serial_;
        return true;
    }
    return dock_.key() != before; // the corner button brightened
}

uint64_t Companion::visual_key() const {
    if (dock_.full() || dock_.screen_off()) return dock_.key() ^ 0x0200000000000000ull;
    if (info_) return (dock_.key() * 0x100000001B3ull) ^ 0x0400000000000000ull;
    return screen_key() ^ (dock_.key() * 0x100000001B3ull);
}

uint64_t Companion::screen_key() const {
    if (screens_mode()) return lower_hash_ ^ 0x1000000000000000ull;
    if (strip_mode()) return hud_shown_ ? content_key() ^ (strip_hash_ * 31) ^ 0x2000000000000000ull : content_key();
    if (hud_shown_ && !hud_covered()) return (frame_ << 1) | 0x8000000000000001ull; // the HUD is live: every frame
    return content_key();
}

uint64_t Companion::content_key() const {
    if (View* a = alternative_view(); a && showing_alternative())
        return a->key(frame_) ^ 0x0400000000000000ull ^ touch_serial_ * 0x9E37 ^ (has_switch() ? 0x77 : 0);
    return main_key() ^ (has_switch() ? 0x0300000000000000ull : 0);
}

uint64_t Companion::main_key() const {
    if (View* v = shown_view(); !has_current_ && v)
        return v->key(frame_) ^ 0x0800000000000000ull ^ (uint64_t(size_t(v)) << 3) ^ touch_serial_ * 0x9E37;
    if (!has_current_ && party_shown_) return party_->key(frame_) | 0x4000000000000000ull;
    if (!has_current_) return menu_ ? 1 : 2;
    uint64_t since = frame_ - shown_at_;
    uint64_t fading = has_previous_ && since < static_cast<uint64_t>(crossfade_frames) ? since + 1 : 0;
    uint64_t anim = current_.frames.size() > 1
                        ? (frame_ / static_cast<uint64_t>(std::max(1, current_.frame_step))) % current_.frames.size()
                        : 0;
    bool dim = profile_->dim_when_unavailable && capture_ && capture_->failures() >= 2;
    return (uint64_t(current_.serial) << 24) ^ (fading << 8) ^ (anim << 1) ^ (dim ? 1 : 0) ^ 0x1000000000ull;
}

std::string Companion::hud_probes(Core& live) const {
    return profile_ && profile_->has_hud ? profile_->hud.on.describe(live) : std::string();
}

CompanionStatus Companion::status() const {
    CompanionStatus s;
    s.has_profile = profile_ != nullptr;
    s.has_map = capture_ != nullptr;
    s.showing_map = has_current_;
    s.showing_hud = hud_shown_;
    s.hud_split = hud_on_;
    s.serial = current_.serial;
    s.requests = requests_;
    if (capture_) s.failures = capture_->failures(&s.last_failure);
    return s;
}

namespace {

// The second screen's ground from row y down (under a band).
void backdrop_rows(Canvas& out, const Look& look, int y) {
    Canvas ground(out.w(), out.h());
    backdrop(ground, look);
    std::copy(ground.data() + size_t(y) * out.w(), ground.data() + size_t(out.w()) * out.h(),
              out.data() + size_t(y) * out.w());
}

struct Placement {
    int scale, x, y;
};
Placement place(const MapImage& m, int w, int h) {
    int scale = std::max(1, std::min(w / std::max(1, m.w), h / std::max(1, m.h)));
    return {scale, (w - m.w * scale) / 2, (h - m.h * scale) / 2};
}

const std::vector<uint32_t>& frame_at(const MapImage& m, uint64_t tick) {
    size_t n = m.frames.size();
    if (n <= 1) return m.frames[0];
    return m.frames[(tick / static_cast<uint64_t>(std::max(1, m.frame_step))) % n];
}

// The launcher's dual-screen badge (library_screen.hpp): a round glossy accent
// badge in a panel ring with two little screens stacked on a hinge, the upper
// one dark with the game, the lower one lit with a stylus dot.
void dual_badge(Canvas& out, const Look& look, int cx, int cy, int size) {
    const Rgb white{255, 255, 255}, deep = mix(look.accent, Rgb{0, 0, 0}, 115), lit = mix(look.accent, white, 200);
    soft_round(out, cx - size / 2, cy - size / 2 + 2, size, size, size / 2, look.shadow(), look.dark ? 80 : 36);
    out.rounded_rect(cx - size / 2 - 2, cy - size / 2 - 2, size + 4, size + 4, size / 2 + 2, look.panel);
    glossy(out, cx - size / 2, cy - size / 2, size, size, size / 2, look.accent);
    const int sw = size * 11 / 20, sh = size * 7 / 20, gap = std::max(2, size / 14), edge = std::max(2, size / 16);
    const int sx = cx - sw / 2, top = cy - (2 * sh + gap) / 2;
    for (int screen = 0; screen < 2; ++screen) {
        const int sy = top + screen * (sh + gap);
        out.rounded_rect(sx, sy, sw, sh, std::max(2, size / 14), white);
        out.rounded_rect(sx + edge, sy + edge, sw - 2 * edge, sh - 2 * edge, 1, screen ? lit : deep);
    }
    const int dot = std::max(4, size / 9) & ~1;
    out.rounded_rect(cx + sw / 5 - dot / 2, top + sh + gap + (sh - dot) / 2, dot, dot, 0, look.accent);
}

void title_card(Canvas& out, const Look& look, const Font& font, const Dock& dock, const std::string& title,
                const std::string& system, bool dual) {
    // The launcher's DSi note over the game's faded box art: a card with an
    // accent rim, the box art (else the system's picture), the system with
    // its icon, the game's name, and for a game with a second screen of its
    // own, the dual-screen badge.
    dock.ground(out, look);
    const int x = kit::margin, w = out.w() - kit::margin * 2, h = dual ? 328 : 270, y = (out.h() - h) / 2;
    soft_round(out, x, y + 3, w, h, 18, look.shadow(), look.dark ? 60 : 20);
    out.rounded_rect(x, y, w, h, 18, look.accent);
    out.rounded_rect(x + 2, y + 2, w - 4, h - 4, 16, look.panel);
    const int cx = out.w() / 2, art = 128;
    dock.art(out, look, {cx - art / 2, y + 22, art, art});
    const int sy = y + 22 + art + 26;
    if (dual) dual_badge(out, look, cx, sy + 98, 44);
    if (!font.loaded()) return;
    // The system: its icon (when the box art took the picture's place), then its name.
    const std::string name = font.fit(system, kit::small, w - 64 - 36);
    const int icon = dock.has_cover() ? 28 : 0, gap = icon ? 8 : 0, sw = icon + gap + font.measure(name, kit::small);
    if (icon) dock.system_icon(out, look, cx - sw / 2, sy - icon / 2, icon);
    font.draw(out, cx - sw / 2 + icon + gap, font.centred(kit::small, sy), name, kit::small, look.muted);
    const std::string t = font.fit(title, kit::large, w - 64);
    font.draw(out, cx - font.measure(t, kit::large) / 2, font.centred(kit::large, sy + 40), t, kit::large, look.ink);
}

} // namespace

void Companion::render(Canvas& out, const Look& look, const Font& font, const Words& words) const {
    // Wide open, the dock's controls cover the whole screen and the companion is not seen.
    if (!dock_.full() && !dock_.screen_off() && dock_.openness() < 1) {
        const uint64_t k = screen_key();
        const size_t n = size_t(out.w()) * out.h();
        if (dock_.moving() && k == screen_cache_key_ && screen_cache_.w() == out.w() && screen_cache_.h() == out.h())
            std::copy(screen_cache_.data(), screen_cache_.data() + n, out.data());
        else {
            Look l = look;
            l.words = &words;
            const Box b = dock_.box(Dock::Item::Button, out.w(), out.h());
            l.reserve_w = out.w() - b.x + kit::gap / 2;
            l.reserve_h = out.h() - b.y + kit::gap / 2;
            render_screen(out, l, font);
            // Kept for the frames that follow while the dock moves over the same picture.
            if (dock_.moving()) {
                screen_cache_.resize(out.w(), out.h());
                std::copy(out.data(), out.data() + n, screen_cache_.data());
                screen_cache_key_ = k;
            } else screen_cache_key_ = ~k;
        }
    }
    dock_.draw(out, look, font, words);
}

void Companion::render_screen(Canvas& out, const Look& look, const Font& font) const {
    Look l = look;
    if (info_) {
        // As a game with no second screen of its own: the card, no note, and the dock's button.
        title_card(out, l, font, dock_, title_, system_name_, false);
        return;
    }
    const bool flip = has_switch();
    if (flip) {
        l.reserve_top_w = out.w() - switch_box(out.w()).x + kit::gap / 2;
        l.reserve_top_h = kit::margin + 56 + kit::gap / 2;
    }
    render_layers(out, l, font);
    if (!flip) return;
    // The switch: what a tap brings up (the board, or back to the map or the team).
    const Box b = switch_box(out.w());
    touch_target(b);
    raised(out, l, b.x, b.y, b.w, b.h, b.h / 2, alt_ ? l.panel : l.accent, 0);
    const Icon back = has_current_ ? Icon::Map : party_shown_ ? Icon::Life : Icon::Back;
    icon(out, l, alt_ ? back : Icon::Star, b.x + b.w / 2, b.y + b.h / 2, 24, alt_ ? l.accent : l.on_accent);
}

void Companion::render_layers(Canvas& out, const Look& look, const Font& font) const {
    if (screens_mode()) return render_screens(out);
    if (strip_mode()) {
        View* v = shown_view();
        const bool covered = !has_current_ && v && v->covers_hud();
        // A view drawn in place of the band (Super Mario Kart's course from above) comes first.
        if (covered) render_content(out, look, font);
        else if (hud_shown_ && !hud_image_.empty() && profile_->hud.strip_fill) render_strip_filled(out, look);
        else if (hud_shown_ && !hud_image_.empty() && !covered) render_with_strip(out, look, font);
        else render_content(out, look, font);
        return;
    }
    if (!hud_covered() && render_hud(out, look)) return;
    render_content(out, look, font);
}

void Companion::render_screens(Canvas& out) const {
    const Rect& r = lower_rect_;
    if (lower_.empty()) return out.fill(Rgb{0, 0, 0});
    const double fit = std::min(double(out.w()) / r.w, double(out.h()) / r.h);
    const double tall = std::min(double(out.h()) / r.h, fit * (lower_w_ == r.w && lower_h_ == r.h ? 1 : profile_->screens.stretch));
    const int tw = std::min(out.w(), int(r.w * fit + 0.5)), th = std::min(out.h(), int(r.h * tall + 0.5));
    out.fill(Rgb{0, 0, 0});
    sharp_blit(out, lower_.data(), r.w, r.h, (out.w() - tw) / 2, (out.h() - th) / 2, tw, th, false, 0);
}

void Companion::render_strip_filled(Canvas& out, const Look& look) const {
    // The band as large as the panel takes it with its shape kept, centred on
    // its own ground: the second picture the game draws under the first.
    std::map<uint32_t, int> seen;
    for (int x = 0; x < hud_w_; ++x) ++seen[hud_image_[static_cast<size_t>(hud_h_ - 1) * hud_w_ + x] & 0xFFFFFF];
    const uint32_t ground =
        std::max_element(seen.begin(), seen.end(), [](auto& a, auto& b) { return a.second < b.second; })->first;
    out.fill(Rgb::from_px(shown_px(ground | 0xFF000000u)));
    const uint32_t* px = shown(hud_image_.data(), hud_w_, hud_h_, tint_a_);
    const double fit = std::min(double(out.w()) / hud_w_, double(out.h()) / hud_h_);
    const int whole = std::max(1, int(fit));
    if (fit - whole < 0.05) {
        out.blit(px, hud_w_, hud_h_, (out.w() - hud_w_ * whole) / 2, (out.h() - hud_h_ * whole) / 2, whole);
        return;
    }
    // As large as the panel takes it, like the game screen's sharp fit.
    const int tw = std::min(out.w(), int(hud_w_ * fit + 0.5)), th = std::min(out.h(), int(hud_h_ * fit + 0.5));
    sharp_blit(out, px, hud_w_, hud_h_, (out.w() - tw) / 2, (out.h() - th) / 2, tw, th, false, 0);
    (void)look;
}

void Companion::render_with_strip(Canvas& out, const Look& look, const Font& font) const {
    // The band in a card at the bottom, at the largest whole scale that leaves
    // most of the screen to the rest (3x for a 160-pixel band on a 640x480
    // panel), on its own colour like a strip of the game's screen.
    const int pad = 10;
    const int room = out.w() - look.reserve_w - (look.reserve_w ? kit::gap : kit::margin);
    int scale = std::max(1, std::min((room - kit::margin - pad * 2) / hud_w_, out.h() / 5 / hud_h_));
    if (scale * hud_w_ > out.w() * 3 / 4 && scale > 1) --scale;
    const int bw = hud_w_ * scale + pad * 2, bh = hud_h_ * scale + pad * 2;
    const int band = bh + kit::margin + kit::gap;
    Canvas content(out.w(), out.h() - band + kit::gap);
    render_content(content, look, font);
    std::copy_n(content.data(), static_cast<size_t>(content.w()) * content.h(), out.data());
    out.fill_rect(0, content.h(), out.w(), out.h() - content.h(), look.background);
    backdrop_rows(out, look, content.h());
    // The band's most common colour in its first column: the HUD's own background.
    std::map<uint32_t, int> seen;
    for (int y = 0; y < hud_h_; ++y) ++seen[hud_image_[static_cast<size_t>(y) * hud_w_] & 0xFFFFFF];
    uint32_t ground = seen.empty() ? 0 : std::max_element(seen.begin(), seen.end(), [](auto& a, auto& b) {
                                             return a.second < b.second;
                                         })->first;
    // Centred in what the dock's corner leaves of the row.
    const int x = std::max(kit::margin, (out.w() - look.reserve_w - bw) / 2), y = out.h() - kit::margin - bh;
    card(out, look, {x - 4, y - 4, bw + 8, bh + 8}, false, kit::radius + 4);
    out.rounded_rect(x, y, bw, bh, kit::radius, Rgb::from_px(shown_px(ground | 0xFF000000u)));
    out.blit(shown(hud_image_.data(), hud_w_, hud_h_, tint_a_), hud_w_, hud_h_, x + pad, y + pad, scale);
}

void Companion::render_content(Canvas& out, const Look& look, const Font& font) const {
    if (View* a = alternative_view(); a && showing_alternative()) {
        a->draw(out, look, font, frame_);
        return;
    }
    // A view that shows (a Pokémon battle) comes before the team.
    if (View* v = shown_view(); !has_current_ && v) {
        v->draw(out, look, font, frame_);
        return;
    }
    if (!has_current_ && party_shown_) {
        party_->draw(out, look, font, frame_);
        return;
    }
    if (!has_current_) {
        // Until the second screen has something to show: the card with the dual-screen badge.
        title_card(out, look, font, dock_, title_, system_name_, profile_ != nullptr);
        return;
    }
    // The map's own empty colour fills the panel, so it reads as the game's
    // second screen rather than a picture in a frame.
    out.fill(Rgb::from_px(shown_px(current_.background)));
    Placement p = place(current_, out.w(), out.h());
    uint64_t since = frame_ - shown_at_;
    if (has_previous_ && since < static_cast<uint64_t>(crossfade_frames)) {
        Placement q = place(previous_, out.w(), out.h());
        out.fill(mix(Rgb::from_px(shown_px(previous_.background)), Rgb::from_px(shown_px(current_.background)),
                     static_cast<int>(since * 256 / crossfade_frames)));
        out.blit(shown(frame_at(previous_, frame_).data(), previous_.w, previous_.h, tint_b_), previous_.w, previous_.h,
                 q.x, q.y, q.scale);
        int alpha = static_cast<int>(since * 255 / crossfade_frames);
        out.blit(shown(frame_at(current_, frame_).data(), current_.w, current_.h, tint_a_), current_.w, current_.h, p.x,
                 p.y, p.scale, alpha);
    } else {
        out.blit(shown(frame_at(current_, frame_).data(), current_.w, current_.h, tint_a_), current_.w, current_.h, p.x,
                 p.y, p.scale);
    }
    if (profile_->dim_when_unavailable && capture_ && capture_->failures() >= 2) out.dim(150);
}

} // namespace play
