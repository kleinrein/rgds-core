#pragma once
// Game menu > Rewind (docs/TIME_TRAVEL_PLAN.md): the running game's moments on the player's Rewind screen
// (play/src/companion/rewind.hpp), the same screen RG DS Play shows from its dock, for every system the bridge can
// ask (DSCore and RG DS Play export rgds_rewind_*).
//
// The engine lists its moments beside the control file (rewind-open). The strip scrolls here, from the pictures read
// once; only the moment it settles on is asked for (rewind-show:<id>), whose own screens then fill the top panel.
// Play from here sends rewind-play:<id> (the runtime closes the menu); leaving any other way sends rewind-back:<now>,
// so the game is never left on a moment only looked at.

namespace {
std::vector<uint8_t> rewind_read_bytes(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), {});
}
} // namespace

void App::open_rewind() {
    if (menu_status["state"] == "busy" || !menu_pending_token.empty()) return;
    rewind_opening = true;
    rewind_open_ = false;
    rewind_asked = rewind_shown_id = 0;
    rewind_final.clear();
    rewind_shown = {};
    rewind_moments = {};
    settings.page = Page::Rewind;
    if (!photo_font) {
        photo_font = std::make_unique<play::Font>();
        photo_font->load(asset_path("fonts").string());
    }
    rewind_words.load(asset_path("locales").string(), preferences.language);
    menu_request("rewind-open");
    emit_sound(MenuCue::Open);
    dirty = true;
}

// Every frame on the page: the engine's answers, the screen's springs, the next request.
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
        rewind_files = rewind_moments.files.empty() ? control.string() + ".rewind" : rewind_moments.files;
        const fs::path thumbs(rewind_files + ".thumbs");
        const bool listed = menu_status["rewind"] == "rewind-ready" && rewind_moments.parse(text, rewind_read_bytes(thumbs));
        std::error_code ignored;
        fs::remove(thumbs, ignored);
        if (!listed) {
            settings.page = Page::QuickMenu;
            settings.focus = 13;
            show_detail("Rewind", locale.text(menu_status["rewind"] == "rewind-unavailable"
                                                  ? "There is nothing to rewind to yet. Play on for a few seconds."
                                                  : "Rewind is available when a game runs with DSCore or RG DS Play."));
            return;
        }
        rewind_screen.open(rewind_moments, rewind_moments.now);
        rewind_screen.set_time(now, preferences.animations);
        rewind_screen.pad(0); // nothing held: the pad works at once
        rewind_open_ = true;
    }
    if (!rewind_open_) return;
    rewind_screen.set_time(now, preferences.animations);
    uint32_t target = 0;
    switch (rewind_screen.take(target)) {
    case play::RewindScreen::Result::Jump: rewind_final = "rewind-play:" + std::to_string(target); break;
    case play::RewindScreen::Result::Back: rewind_final = "rewind-back:" + std::to_string(rewind_moments.now); break;
    case play::RewindScreen::Result::None: break;
    }
    if (!idle) return;
    // The engine's answer to the last look: its screens, if they are the moment asked for.
    if (rewind_asked && rewind_shown_id != rewind_asked && menu_status["rewind"] == "rewind-shown") {
        play::ShownMoment shown;
        const fs::path file(rewind_files + ".shown");
        if (shown.parse(rewind_read_bytes(file)) && shown.id == rewind_asked) {
            rewind_shown = std::move(shown);
            rewind_shown_id = rewind_asked;
            rewind_top_version++;
        }
        std::error_code ignored;
        fs::remove(file, ignored);
    }
    if (!rewind_final.empty()) {
        const bool leaving = rewind_final.rfind("rewind-back:", 0) == 0;
        menu_request(rewind_final);
        rewind_final.clear();
        rewind_open_ = false;
        // Back to the game menu either way (playing on closes it, and it opens on the menu next time).
        settings.page = Page::QuickMenu;
        settings.focus = leaving ? 13 : 0;
        dirty = true;
        return;
    }
    // A look at the moment the strip rests on (not while it glides or a finger scrubs past others).
    if (rewind_screen.settled() && rewind_screen.chosen() != rewind_asked) {
        rewind_asked = rewind_screen.chosen();
        menu_request("rewind-show:" + std::to_string(rewind_asked));
    }
}

void App::rewind_action(int code) {
    if (!rewind_open_) {
        if (code == 3 || code == 7) {
            // Left before the moments came: nothing was looked at, and the engine lets "now" go by itself.
            rewind_opening = false;
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
    rewind_poll();
    dirty = true;
}

void App::rewind_pointer(int x, int y, int phase) {
    if (!rewind_open_) return;
    rewind_screen.touch({phase == 0 ? play::Touch::Down : phase == 1 ? play::Touch::Move : play::Touch::Up, x, y});
    dirty = true;
}

SDL_Texture* App::rewind_texture(SDL_Renderer* r, bool top) {
    auto& texture = rewind_textures[{r, top}];
    if (!texture) {
        texture = s.CreateTexture(r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, 640, 480);
        if (!texture) return nullptr;
        rewind_uploaded[{r, top}] = ~0ull;
    }
    const uint64_t version = top ? rewind_top_version ^ uint64_t(rewind_screen.chosen()) << 32 : rewind_screen.key();
    if (rewind_uploaded[{r, top}] != version) {
        play::Canvas& canvas = top ? rewind_top_canvas : rewind_canvas;
        canvas.resize(640, 480);
        if (top) rewind_draw_top(canvas);
        else if (photo_font) rewind_screen.draw(canvas, rewind_look, *photo_font, rewind_words);
        s.UpdateTexture(texture, nullptr, canvas.data(), 640 * 4);
        rewind_uploaded[{r, top}] = version;
    }
    return texture;
}

// The top panel: the moment in the middle of the strip, its own screen at full size once the engine has shown it,
// its small picture until then.
void App::rewind_draw_top(play::Canvas& out) {
    out.fill(play::Rgb{0, 0, 0});
    const uint32_t chosen = rewind_screen.chosen();
    const uint32_t* px = nullptr;
    int w = 0, h = 0;
    if (rewind_shown_id == chosen && !rewind_shown.px.empty()) {
        px = rewind_shown.px.data(); // a DS: the top screen is the first
        w = rewind_shown.w;
        h = rewind_shown.h;
    } else if (const play::Moment* m = rewind_moments.find(chosen); m && !m->thumb.empty()) {
        px = m->thumb.px.data();
        w = m->thumb.w;
        // A DS moment's picture is both screens, the top over the bottom: the top panel shows the top one.
        h = m->thumb.h * 10 > m->thumb.w * 14 ? m->thumb.h / 2 : m->thumb.h;
    }
    if (!px || w <= 0 || h <= 0) return;
    const double scale = std::min(640.0 / w, 480.0 / h);
    const int dw = int(w * scale), dh = int(h * scale), x0 = (640 - dw) / 2, y0 = (480 - dh) / 2;
    for (int y = 0; y < dh; y++) {
        const int sy = std::min(h - 1, int(y / scale));
        uint32_t* row = out.data() + size_t(y0 + y) * 640 + x0;
        for (int x = 0; x < dw; x++) row[x] = px[size_t(sy) * w + std::min(w - 1, int(x / scale))] | 0xFF000000u;
    }
}

void App::rewind_top(SDL_Renderer* r, int y) {
    rewind_poll();
    SDL_Rect all{0, y, 640, 480};
    if (auto* texture = rewind_texture(r, true)) s.RenderCopy(r, texture, nullptr, &all);
}

void App::rewind_bottom(SDL_Renderer* r, int y) {
    rewind_poll();
    SDL_Rect all{0, y, 640, 480};
    if (!rewind_open_) {
        rect(r, 0, y, 640, 480, "background");
        label(r, locale.text("Rewind"), 28, y + 40, 30, "ink", 580);
        label(r, locale.text(rewind_opening ? "Gathering the moments…" : "Returning…"), 28, y + 92, 20, "muted", 580);
        return;
    }
    if (auto* texture = rewind_texture(r, false)) s.RenderCopy(r, texture, nullptr, &all);
    if (rewind_screen.moving()) dirty = true; // the springs draw every frame until they rest
}
