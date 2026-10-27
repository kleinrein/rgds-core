// SPDX-License-Identifier: GPL-3.0-or-later
#include "companion/companion.hpp"
#include "ui/kit.hpp"

#include <algorithm>
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
                views_.push_back(std::move(view));
                auto alt = spec.values.find("alternative");
                alternative_.push_back(alt != spec.values.end() && (alt->second == "yes" || alt->second == "true"));
            } else std::fprintf(stderr, "play: %s: %s\n", profile_->file.c_str(), error.c_str());
        }
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
    ever_shown_ = true;
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

Picture Companion::compose_game(const Picture& game, bool whole) {
    if (strip_mode()) {
        // The band leaves the picture only where it spans it, at its top or bottom edge.
        const Rect& s = profile_->hud.strip;
        if (whole || !hud_on_ || s.w != game.w || s.h >= game.h) return game;
        if (s.y == 0) return {game.px + static_cast<size_t>(s.h) * game.stride, game.w, game.h - s.h, game.stride};
        if (s.y + s.h == game.h) return {game.px, game.w, game.h - s.h, game.stride};
        return game;
    }
    // Only while split: otherwise the game draws its own HUD, item box and place included.
    if (!hud_on_ || !moves_ || hud_image_.empty() || game.w != hud_w_ || game.h != hud_h_) return game;
    composed_.resize(static_cast<size_t>(game.w) * game.h);
    for (int y = 0; y < game.h; ++y)
        std::copy_n(game.px + static_cast<size_t>(y) * game.stride, game.w,
                    composed_.begin() + static_cast<size_t>(y) * game.w);
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
    int scale = std::max(1, std::min(out.w() / hud_w_, out.h() / hud_h_));
    int x0 = (out.w() - hud_w_ * scale) / 2, y0 = (out.h() - hud_h_ * scale) / 2;
    // Clear of the dock's corner: up a little when there is room above.
    if (look.reserve_w && x0 + hud_w_ * scale > out.w() - look.reserve_w &&
        y0 + hud_h_ * scale > out.h() - look.reserve_h)
        y0 = std::max(0, out.h() - look.reserve_h - hud_h_ * scale);
    if (!moves_) {
        out.blit_keyed(hud_image_.data(), hud_w_, hud_h_, x0, y0, scale, hud_key_);
        return true;
    }
    out.blit_keyed(rest.data(), hud_w_, hud_h_, x0, y0, scale, hud_key_);
    return true;
}

void Companion::after_frame(Core& live) {
    ++frame_;
    live_ = &live;
    if (profile_ && profile_->has_hud) update_hud(live);
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

bool Companion::has_switch() const { return alternative_view() && (has_current_ || shown_view() || party_shown_); }

bool Companion::showing_alternative() const {
    // The alternative when chosen, or when there is nothing else to show.
    return alternative_view() && (alt_ || !(has_current_ || shown_view() || party_shown_));
}

Box Companion::switch_box(int w) const { return {w - kit::margin - 56, kit::margin, 56, 56}; }

bool Companion::touch(const Touch& t) {
    const uint64_t before = dock_.key();
    if (dock_.touch(t)) return dock_.key() != before;
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
    return screen_key() ^ (dock_.key() * 0x100000001B3ull);
}

uint64_t Companion::screen_key() const {
    if (strip_mode()) return hud_shown_ ? content_key() ^ (strip_hash_ * 31) ^ 0x2000000000000000ull : content_key();
    if (hud_shown_) return (frame_ << 1) | 0x8000000000000001ull; // the HUD is live: every frame
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

// Two little stacked screens, the launcher's mark for a game with a second screen of its own.
void dual_mark(Canvas& out, int x, int y, int unit, Rgb colour, Rgb ground) {
    for (int screen = 0; screen < 2; ++screen) {
        const int sy = y + screen * unit * 6 / 5;
        out.rounded_rect(x, sy, unit * 8 / 5, unit, std::max(1, unit / 4), colour);
        out.rounded_rect(x + unit / 5, sy + unit / 5, unit * 6 / 5, unit * 3 / 5, 1, ground);
    }
}

void title_card(Canvas& out, const Look& look, const Font& font, const std::string& title, const std::string& system,
                const std::string& note, bool dual) {
    // The launcher's DSi note: a card with an accent rim in the middle of the
    // touch screen's grid, the system as a chip, the game's name, and what
    // will come here.
    backdrop(out, look);
    const int x = kit::margin, w = out.w() - kit::margin * 2, h = 220, y = (out.h() - h) / 2;
    soft_round(out, x, y + 3, w, h, 18, look.shadow(), look.dark ? 60 : 20);
    out.rounded_rect(x, y, w, h, 18, look.accent);
    out.rounded_rect(x + 2, y + 2, w - 4, h - 4, 16, look.panel);
    if (!font.loaded()) return;
    const int cx = out.w() / 2;
    const std::string chip_text = system;
    const int cw = font.measure(chip_text, kit::small) + 24;
    chip(out, look, font, cx - cw / 2, y + 48, chip_text, true);
    const std::string t = font.fit(title, kit::large, w - 64);
    font.draw(out, cx - font.measure(t, kit::large) / 2, y + 116, t, kit::large, look.ink);
    if (!note.empty()) {
        // A game with a second screen of its own: the mark, then what will come here.
        const int unit = dual ? 10 : 0, mark = dual ? unit * 8 / 5 + 10 : 0;
        const std::string n = font.fit(note, kit::body, w - 64 - mark);
        const int nx = cx - (font.measure(n, kit::body) + mark) / 2, base = y + 172;
        if (dual) dual_mark(out, nx, base - font.ascent(kit::body) / 2 - unit * 11 / 10, unit, look.muted, look.panel);
        font.draw(out, nx + mark, base, n, kit::body, look.muted);
    }
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
    if (strip_mode()) {
        View* v = shown_view();
        const bool covered = !has_current_ && v && v->covers_hud();
        if (hud_shown_ && !hud_image_.empty() && profile_->hud.strip_fill) render_strip_filled(out, look);
        else if (hud_shown_ && !hud_image_.empty() && !covered) render_with_strip(out, look, font);
        else render_content(out, look, font);
        return;
    }
    if (render_hud(out, look)) return;
    render_content(out, look, font);
}

void Companion::render_strip_filled(Canvas& out, const Look& look) const {
    // The band as large as the panel takes it with its shape kept, centred on
    // its own ground: the second picture the game draws under the first.
    std::map<uint32_t, int> seen;
    for (int x = 0; x < hud_w_; ++x) ++seen[hud_image_[static_cast<size_t>(hud_h_ - 1) * hud_w_ + x] & 0xFFFFFF];
    const uint32_t ground = std::max_element(seen.begin(), seen.end(), [](auto& a, auto& b) {
                                return a.second < b.second;
                            })->first;
    out.fill(Rgb::from_px(shown_px(ground | 0xFF000000u)));
    const int scale = std::max(1, std::min(out.w() / hud_w_, out.h() / hud_h_));
    const int x = (out.w() - hud_w_ * scale) / 2, y = (out.h() - hud_h_ * scale) / 2;
    out.blit(shown(hud_image_.data(), hud_w_, hud_h_, tint_a_), hud_w_, hud_h_, x, y, scale);
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
        // What this game's second screen will show, until it shows it.
        std::string note;
        if (profile_ && !profile_->note.empty()) note = profile_->note;
        else if (capture_)
            note = ever_shown_ ? "The map comes back once you're playing" : "The map appears once you're playing";
        else if (party_) note = "Your team appears once it's loaded";
        else if (!views_.empty()) note = "The map draws itself as you explore";
        else if (profile_ && profile_->has_hud) note = "The status bar moves here while you play";
        title_card(out, look, font, title_, system_name_, note, profile_ != nullptr);
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
