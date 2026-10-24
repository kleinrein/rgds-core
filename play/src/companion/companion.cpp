// SPDX-License-Identifier: GPL-3.0-or-later
#include "companion/companion.hpp"

#include <algorithm>
#include <cstdio>
#include <map>

namespace play {

Companion::Companion(Core& live, const Profile* profile, std::string title, std::string system_name)
    : profile_(profile), title_(std::move(title)), system_name_(std::move(system_name)) {
    if (profile_ && profile_->has_map) {
        capture_ = std::make_unique<MapCapture>(live, *profile_);
        if (!capture_->ready()) capture_.reset();
    }
    if (profile_ && profile_->has_party) party_ = std::make_unique<Party>(profile_->party);
    if (profile_ && profile_->has_view) {
        std::string error;
        view_ = make_view(profile_->view, error);
        if (!view_) std::fprintf(stderr, "play: %s: %s\n", profile_->file.c_str(), error.c_str());
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
    bool on = h.on.match(live) && s.y + s.h <= p.h && s.w <= p.w;
    if (!on) {
        // The last band stays up while `keep` holds (the inventory sliding in).
        hud_shown_ = hud_shown_ && !h.keep.empty() && h.keep.match(live);
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
    bool on = h.on.match(live) && map_layer_shown(live, h);
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
        else hud_shown_ = hud_shown_ && !h.keep.empty() && h.keep.match(live);
        // A kept picture (the pause menu, the results) stays laid out as it was.
        if (!hud_shown_) moves_ = false;
        return;
    }
    if (!on) {
        if (hud_shown_ && (h.keep.empty() || !h.keep.match(live))) hud_shown_ = false;
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
    if (view_) view_->update(live, frame_);
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

uint64_t Companion::visual_key() const {
    if (strip_mode()) return hud_shown_ ? content_key() ^ (strip_hash_ * 31) ^ 0x2000000000000000ull : content_key();
    if (hud_shown_) return (frame_ << 1) | 0x8000000000000001ull; // the HUD is live: every frame
    return content_key();
}

uint64_t Companion::content_key() const {
    if (!has_current_ && party_shown_) return party_->key(frame_) | 0x4000000000000000ull;
    if (!has_current_ && view_ && view_->shown()) return view_->key(frame_) ^ 0x0800000000000000ull;
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
    out.vertical_gradient(mix(look.background, look.panel, 96), look.background);
    const int margin = out.w() / 12;
    // A soft card in the middle, the DSi way.
    int card_h = out.h() * 5 / 12, card_y = (out.h() - card_h) / 2;
    out.rounded_rect(margin, card_y, out.w() - margin * 2, card_h, out.h() / 24, look.panel, 235);
    if (!font.loaded()) return;
    int big = out.h() >= 700 ? 36 : 28, small = out.h() >= 700 ? 24 : 16;
    std::string t = font.fit(title, big, out.w() - margin * 4);
    int y = card_y + card_h / 2;
    int pill = out.w() / 14;
    out.rounded_rect((out.w() - pill) / 2, y - big - small * 2 - 2, pill, 4, 2, look.accent);
    font.draw(out, (out.w() - font.measure(t, big)) / 2, y, t, big, look.ink);
    font.draw(out, (out.w() - font.measure(system, small)) / 2, y - big - small / 2, system, small, look.accent);
    if (!note.empty()) {
        // A game with a second screen of its own: the mark, then what will come here.
        const int unit = dual ? std::max(6, small / 2) : 0, mark = dual ? unit * 8 / 5 + small / 2 : 0;
        std::string n = font.fit(note, small, out.w() - margin * 4 - mark);
        const int x = (out.w() - font.measure(n, small) - mark) / 2;
        if (dual) dual_mark(out, x, y + small * 2 - unit * 2 + unit / 3, unit, look.muted, look.panel);
        font.draw(out, x + mark, y + small * 2, n, small, look.muted);
    }
}

} // namespace

void Companion::render(Canvas& out, const Look& look, const Font& font) const {
    if (strip_mode()) {
        if (hud_shown_ && !hud_image_.empty()) render_with_strip(out, look, font);
        else render_content(out, look, font);
        return;
    }
    if (render_hud(out, look)) return;
    render_content(out, look, font);
}

void Companion::render_with_strip(Canvas& out, const Look& look, const Font& font) const {
    // The band at the largest whole scale that leaves most of the screen to
    // the rest (3x for a 160-pixel band on a 640x480 panel), on its own colour.
    const int margin = std::max(4, out.h() / 40);
    int scale = std::max(1, std::min((out.w() - margin * 2) / hud_w_, out.h() / 5 / hud_h_));
    if (scale * hud_w_ > out.w() * 3 / 4 && scale > 1) --scale;
    const int band = hud_h_ * scale + margin * 2;
    Canvas content(out.w(), out.h() - band);
    render_content(content, look, font);
    std::copy_n(content.data(), static_cast<size_t>(content.w()) * content.h(), out.data());
    // The band's most common colour in its first column: the HUD's own background.
    std::map<uint32_t, int> seen;
    for (int y = 0; y < hud_h_; ++y) ++seen[hud_image_[static_cast<size_t>(y) * hud_w_] & 0xFFFFFF];
    uint32_t ground = seen.empty() ? 0 : std::max_element(seen.begin(), seen.end(), [](auto& a, auto& b) {
                                             return a.second < b.second;
                                         })->first;
    out.fill_rect(0, content.h(), out.w(), band, Rgb::from_px(ground));
    out.blit(hud_image_.data(), hud_w_, hud_h_, (out.w() - hud_w_ * scale) / 2, content.h() + margin, scale);
}

void Companion::render_content(Canvas& out, const Look& look, const Font& font) const {
    if (!has_current_ && party_shown_) {
        party_->draw(out, look, font, frame_);
        return;
    }
    if (!has_current_ && view_ && view_->shown()) {
        view_->draw(out, look, font, frame_);
        return;
    }
    if (!has_current_) {
        // What this game's second screen will show, until it shows it.
        std::string note;
        if (profile_ && !profile_->note.empty()) note = profile_->note;
        else if (capture_)
            note = ever_shown_ ? "The map comes back once you're playing" : "The map appears once you're playing";
        else if (party_) note = "Your team appears once it's loaded";
        else if (view_) note = "The map draws itself as you explore";
        else if (profile_ && profile_->has_hud) note = "The status bar moves here while you play";
        title_card(out, look, font, title_, system_name_, note, profile_ != nullptr);
        return;
    }
    // The map's own empty colour fills the panel, so it reads as the game's
    // second screen rather than a picture in a frame.
    out.fill(Rgb::from_px(current_.background));
    Placement p = place(current_, out.w(), out.h());
    uint64_t since = frame_ - shown_at_;
    if (has_previous_ && since < static_cast<uint64_t>(crossfade_frames)) {
        Placement q = place(previous_, out.w(), out.h());
        out.fill(mix(Rgb::from_px(previous_.background), Rgb::from_px(current_.background),
                     static_cast<int>(since * 256 / crossfade_frames)));
        out.blit(frame_at(previous_, frame_).data(), previous_.w, previous_.h, q.x, q.y, q.scale);
        int alpha = static_cast<int>(since * 255 / crossfade_frames);
        out.blit(frame_at(current_, frame_).data(), current_.w, current_.h, p.x, p.y, p.scale, alpha);
    } else {
        out.blit(frame_at(current_, frame_).data(), current_.w, current_.h, p.x, p.y, p.scale);
    }
    if (profile_->dim_when_unavailable && capture_ && capture_->failures() >= 2) out.dim(150);
}

} // namespace play
