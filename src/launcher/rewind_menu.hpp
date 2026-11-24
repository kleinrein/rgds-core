#pragma once
// Game menu > Rewind (docs/TIME_TRAVEL_PLAN.md): the running game's moments, for every engine the bridge can ask
// (DSCore and RG DS Play export rgds_rewind_*). A page like the others: the Save gallery's card on the top screen
// (the moment's picture, how long ago, what to do), a row of cards on the lower one, and the footer's Back and A.
// play::RewindScreen (play/src/companion/rewind.hpp) keeps the row's layout, motion, touch and pad, so RG DS Play's
// own Rewind screen behaves the same.
//
// The engine lists its moments once (rewind-open), each with its picture, which becomes a texture the first time its
// card shows: scrolling only moves textures. Nothing is asked of the engine while browsing (a look loads nothing);
// Play from here sends rewind-play:<id> (the runtime closes the menu), leaving any other way rewind-back:<now>.

namespace {
// Read in one go at the file's size (tens of MB of pictures for a Play game: no doubling buffer).
std::vector<uint8_t> rewind_read_bytes(const fs::path& path) {
    std::vector<uint8_t> bytes;
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return bytes;
    if (std::fseek(file, 0, SEEK_END) == 0) {
        const long size = std::ftell(file);
        if (size > 0 && size < (256l << 20) && std::fseek(file, 0, SEEK_SET) == 0) {
            bytes.resize(size_t(size));
            bytes.resize(std::fread(bytes.data(), 1, bytes.size(), file));
        }
    }
    std::fclose(file);
    return bytes;
}
} // namespace

void App::rewind_drop_textures() {
    for (auto& [key, texture] : rewind_textures)
        if (texture) s.DestroyTexture(texture);
    rewind_textures.clear();
}

// Off the page, however it was left (B, Play from here, the Menu button, the runtime's own view change): the
// textures and pictures go, and a list still on its way is read and deleted when it lands (it is RAM on RG DS OS).
void App::rewind_leave() {
    rewind_screen.close();
    rewind_drop_textures();
    rewind_moments = {};
    if (rewind_opening) rewind_discard = true;
    rewind_opening = rewind_open_ = false;
    rewind_final.clear();
}

// Every frame, any page: what the Rewind page left behind.
void App::rewind_tidy() {
    if (settings.page != Page::Rewind && (rewind_opening || rewind_open_ || !rewind_textures.empty() ||
                                          !rewind_moments.moments.empty()))
        rewind_leave();
    if (!rewind_discard || !menu_pending_token.empty() || menu_status["state"] == "busy") return;
    rewind_discard = false;
    const fs::path control = o.data / "session/emulator.control";
    play::MomentList list;
    const auto bytes = rewind_read_bytes(fs::path(control.string() + ".rewind"));
    list.parse(std::string(bytes.begin(), bytes.end()), {});
    std::error_code ignored;
    fs::remove(fs::path((list.files.empty() ? control.string() + ".rewind" : list.files) + ".thumbs"), ignored);
}

void App::open_rewind() {
    if (menu_status["state"] == "busy" || !menu_pending_token.empty()) return;
    rewind_drop_textures();
    rewind_discard = false;
    menu_status["rewind"] = ""; // only this open's answer counts (a request that could not be written leaves it)
    rewind_opening = true;
    rewind_open_ = false;
    rewind_final.clear();
    rewind_moments = {};
    settings.page = Page::Rewind;
    rewind_words.load(asset_path("locales").string(), preferences.language);
    menu_request("rewind-open");
    emit_sound(MenuCue::Open);
    dirty = true;
}

// Every frame on the page: the engine's list once it is there, the row's spring, the request to send.
void App::rewind_poll() {
    const uint32_t now = s.GetTicks();
    const bool idle = menu_pending_token.empty() && menu_status["state"] != "busy";
    const fs::path control = o.data / "session/emulator.control";
    if (rewind_opening && idle) {
        rewind_opening = false;
        const auto bytes = rewind_read_bytes(fs::path(control.string() + ".rewind"));
        const std::string text(bytes.begin(), bytes.end());
        // The list names where the pictures are (RAM on RG DS OS); read once, then gone.
        rewind_moments.parse(text, {});
        const fs::path thumbs((rewind_moments.files.empty() ? control.string() + ".rewind" : rewind_moments.files) +
                              ".thumbs");
        const bool listed =
            menu_status["rewind"] == "rewind-ready" && rewind_moments.parse(text, rewind_read_bytes(thumbs));
        std::error_code ignored;
        fs::remove(thumbs, ignored);
        if (!listed) {
            rewind_moments = {};
            settings.page = Page::QuickMenu;
            settings.focus = 13;
            show_detail("Rewind", locale.text(menu_status["rewind"] == "rewind-unavailable"
                                                  ? "There is nothing to rewind to yet. Play on for a few seconds."
                                                  : "Rewind is available when a game runs with DSCore or RG DS Play."));
            return;
        }
        rewind_screen.open(rewind_moments, rewind_moments.now);
        rewind_screen.set_time(now, false);
        rewind_screen.pad(0); // nothing held: the pad works at once
        rewind_open_ = true;
    }
    if (!rewind_open_) return;
    // No glide here: over a game the menu draws in software (37-100 ms a frame on the handheld), so a spring would
    // be a run of slow frames. The row steps to its place, as every list in the game menu does; a finger still drags.
    rewind_screen.set_time(now, false);
    uint32_t target = 0;
    switch (rewind_screen.take(target)) {
    case play::RewindScreen::Result::Jump: rewind_final = "rewind-play:" + std::to_string(target); break;
    case play::RewindScreen::Result::Back: rewind_final = "rewind-back:" + std::to_string(rewind_moments.now); break;
    case play::RewindScreen::Result::None: break;
    }
    if (rewind_final.empty() || !idle) return;
    const bool leaving = rewind_final.rfind("rewind-back:", 0) == 0;
    emit_sound(leaving ? MenuCue::Back : MenuCue::Load);
    menu_request(rewind_final);
    rewind_leave();
    // Back to the game menu either way (playing on closes it, and it opens on the menu next time).
    settings.page = Page::QuickMenu;
    settings.focus = leaving ? 13 : 0;
    dirty = true;
}

void App::rewind_action(int code) {
    if (!rewind_open_) {
        if (code == 3 || code == 7) {
            // Left before the moments came: nothing was looked at, and the engine lets "now" go by itself.
            rewind_leave();
            settings.page = Page::QuickMenu;
            settings.focus = 13;
        }
        dirty = true;
        return;
    }
    // The menu's codes as the pad the screen reads: pressed, then let go.
    const uint32_t button = code == 0    ? play::Left
                            : code == 1  ? play::Right
                            : code == 8  ? play::Up
                            : code == 9  ? play::Down
                            : code == 2  ? play::A
                            : code == 3  ? play::B
                            : code == 7  ? play::B
                            : code == 13 ? play::L
                            : code == 14 ? play::R
                                         : 0;
    if (!button) return;
    rewind_screen.pad(button);
    rewind_screen.pad(0);
    if (code == 0 || code == 1 || code == 13 || code == 14) emit_sound(MenuCue::Navigate);
    rewind_poll();
    dirty = true;
}

void App::rewind_pointer(int x, int y, int phase) {
    if (!rewind_open_) {
        if (phase == 2 && x < 96 && y < 80) rewind_action(3); // the header's back arrow
        return;
    }
    rewind_screen.touch({phase == 0 ? play::Touch::Down : phase == 1 ? play::Touch::Move : play::Touch::Up, x, y});
    dirty = true;
}

// The moment's picture as a texture of exactly the size it is drawn at, made once: the menu's software renderer
// copies it 1:1 every frame (its fast path) instead of scaling it (its slowest).
// One texture per moment and renderer: a card grows and shrinks as the row scrolls, and a texture kept for every size
// it passed through filled memory for as long as the screen stayed open.
SDL_Texture* App::rewind_texture(SDL_Renderer* r, uint32_t id, int w, int h) {
    const uint64_t first = uint64_t(id) << 32;
    const std::pair<SDL_Renderer*, uint64_t> key{r, first | uint64_t(w & 0xFFFF) << 16 | uint64_t(h & 0xFFFF)};
    if (auto it = rewind_textures.find(key); it != rewind_textures.end()) return it->second;
    const play::Moment* m = rewind_moments.find(id);
    if (!m || m->thumb.empty() || w <= 0 || h <= 0) return nullptr;
    // The page draws a moment at three sizes at most (the top card, the middle card, the others): past that, the
    // moment's textures start again.
    size_t sizes = 0;
    for (auto it = rewind_textures.lower_bound({r, first});
         it != rewind_textures.end() && it->first.first == r && it->first.second >> 32 == id; ++it)
        sizes++;
    if (sizes >= 3)
        for (auto it = rewind_textures.lower_bound({r, first});
             it != rewind_textures.end() && it->first.first == r && it->first.second >> 32 == id;) {
            if (it->second) s.DestroyTexture(it->second);
            it = rewind_textures.erase(it);
        }
    auto& texture = rewind_textures[key];
    std::vector<uint32_t> px(size_t(w) * h);
    const auto& t = m->thumb;
    for (int y = 0; y < h; y++) {
        const uint32_t* row = t.px.data() + size_t(std::min(t.h - 1, y * t.h / h)) * t.w;
        for (int x = 0; x < w; x++) px[size_t(y) * w + x] = row[std::min(t.w - 1, x * t.w / w)] | 0xFF000000u;
    }
    texture = s.CreateTexture(r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
    if (texture) s.UpdateTexture(texture, nullptr, px.data(), w * 4);
    return texture;
}

// The moment's picture in its own shape inside `box` (a DS moment is both screens, upright).
void App::rewind_picture(SDL_Renderer* r, uint32_t id, const play::Box& box) {
    const play::Moment* m = rewind_moments.find(id);
    const play::Box pic = m ? play::RewindScreen::fit(m->thumb, box) : box;
    SDL_Rect dst{pic.x, pic.y, pic.w, pic.h};
    if (auto* texture = rewind_texture(r, id, pic.w, pic.h)) s.RenderCopy(r, texture, nullptr, &dst);
    else round_rect(r, pic.x, pic.y, pic.w, pic.h, 6, "panel");
}

// The top screen: the Save gallery's card, for the moment in the middle of the row.
void App::rewind_top(SDL_Renderer* r, int y) {
    rewind_poll();
    settings_card(r, 24, y + top_card_y, 592, top_card_h);
    if (!rewind_open_) {
        ui_icon(r, SettingsIcon::Clock, 320, y + 200, 64, "muted");
        centered(r, rewind_opening ? "Gathering the moments…" : "Rewind", y + 260, 2, "ink");
        return;
    }
    const uint32_t chosen = rewind_screen.chosen();
    rewind_picture(r, chosen, {40, y + 116, 320, 240});
    raw_label(r, rewind_screen.label(chosen, rewind_words), 388, y + 134, 28, "ink", 208);
    label(r, chosen == rewind_moments.now ? "Where you are" : "Earlier", 388, y + 185, 20, "accent", 208);
    if (const int n = rewind_screen.branch_count())
        raw_label(r,
                  locale.text("Future") + " " + std::to_string(rewind_screen.branch_index() + 1) + "/" +
                      std::to_string(n),
                  388, y + 231, 20, "ink", 208);
    wrapped(r, "Pick a moment, then play on from it. B goes back to where you are.", 388, y + 284, 16, 208, 5);
}

// The lower screen: header, the row of cards, the branch row, the footer.
void App::rewind_bottom(SDL_Renderer* r, int y) {
    rewind_poll();
    rect(r, 0, y, 640, 480, "background");
    pattern(r, y, 480, true);
    settings_header(r, y, "Rewind");
    if (!rewind_open_) return;
    using Screen = play::RewindScreen;
    const auto& line = rewind_screen.line();
    // The far cards first, so the nearer ones lie over them.
    std::vector<size_t> order;
    for (size_t i = 0; i < line.size(); i++) {
        const play::Box b = rewind_screen.picture_box(i);
        if (b.x + b.w > -16 && b.x < 656) order.push_back(i);
    }
    const double middle = double(rewind_screen.selected());
    std::sort(order.begin(), order.end(),
              [&](size_t a, size_t b) { return std::abs(double(a) - middle) > std::abs(double(b) - middle); });
    for (size_t i : order) {
        // Two sizes only, the middle card's and the others': a drag moves cards without making a texture a frame.
        const play::Box at = rewind_screen.picture_box(i);
        const bool middle_card = i == rewind_screen.selected();
        const int bw = middle_card ? Screen::big_w : Screen::small_w, bh = middle_card ? Screen::big_h : Screen::small_h;
        const play::Box b{at.x + at.w / 2 - bw / 2, Screen::row_y - bh / 2, bw, bh};
        const play::Moment* m = rewind_moments.find(line[i]);
        const play::Box pic = m ? Screen::fit(m->thumb, b) : b;
        const bool focused = i == rewind_screen.selected();
        settings_card(r, pic.x - 6, y + pic.y - 6, pic.w + 12, pic.h + 12, focused);
        rewind_picture(r, line[i], {b.x, y + b.y, b.w, b.h});
        const std::string text = rewind_screen.label(line[i], rewind_words);
        const int tw = std::min(b.w, label_width(text, 16, false));
        raw_label(r, text, b.x + (b.w - tw) / 2, y + Screen::row_y + Screen::big_h / 2 + 12, 16,
                  focused ? "ink" : "muted", b.w);
    }
    if (const int n = rewind_screen.branch_count()) {
        const play::Box b = Screen::box(Screen::Part::Branch);
        settings_card(r, b.x, y + b.y, b.w, b.h, pointer_down && b.contains(contact_x, contact_y));
        const std::string text =
            locale.text("Future") + " " + std::to_string(rewind_screen.branch_index() + 1) + "/" + std::to_string(n);
        const int tw = std::min(b.w - 16, label_width(text, 20, false));
        raw_label(r, text, b.x + (b.w - tw) / 2, y + b.y + 12, 20, "ink", b.w - 16);
    } else if (line.size() <= 1)
        centered(r, "Play on for a few seconds, then come back", y + 360, 1, "muted");
    // The footer: Back (to where you are), the keys, Play from here.
    rect(r, 0, y + 424, 640, 56, "background");
    rect(r, 24, y + 424, 592, 1, "line");
    footer_button(r, 12, y + 432, 160, "Back", "B", false, footer_pressed(true));
    std::vector<KeyHint> keys{{"L R", "Skip 10"}};
    if (rewind_screen.branch_count()) keys.push_back({"Up Down", "Future"});
    key_hints(r, 320, y + 452, keys, 0, 16, 260);
    footer_button(r, 468, y + 432, 160, "Play from here", "A", true, footer_pressed(false));
    if (rewind_screen.moving()) dirty = true; // the spring draws every frame until it rests
}
