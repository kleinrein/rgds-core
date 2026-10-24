#pragma once
// Library: the cover and icon stage, favourite badges, cover captions, the
// library header controls and the letter rail.
// Members of App in main.cpp; included once, right after the class.

bool App::game_loved(size_t index) {
    const auto& game = model.games[index];
    return !game.demo && library_cache.options(game).favourite;
}

void App::favourite_badge(SDL_Renderer* r, int cx, int cy, int size) {
    alpha_round(r, cx - size / 2, cy - size / 2 + 2, size, size, size / 2, shadow_colour(), preferences.dark ? 80 : 36);
    round_rect(r, cx - size / 2 - 2, cy - size / 2 - 2, size + 4, size + 4, size / 2 + 2, "panel");
    glossy_pill(r, cx - size / 2, cy - size / 2, size, size, size / 2, Color{232, 72, 96});
    draw_settings_icon(s, r, SettingsIcon::Heart, cx, cy + 1, size * 11 / 20, Color{255, 255, 255});
}

bool App::game_dual(size_t index) {
    const auto& game = model.games[index];
    if (game.demo) return false;
    const fs::path profiles = o.app.empty() ? fs::path("play/profiles") : o.app / "emulators/rgds-play/profiles";
    return dual_screen_game(game.path, game.system, profiles);
}

void App::dual_badge(SDL_Renderer* r, int x, int y) {
    // Two little stacked screens on a quiet chip: noticed when looked for, not a label.
    const int w = 28, h = 34;
    alpha_round(r, x, y + 2, w, h, 9, shadow_colour(), preferences.dark ? 70 : 28);
    round_rect(r, x - 1, y - 1, w + 2, h + 2, 10, "line");
    round_rect(r, x, y, w, h, 9, "panel");
    for (int screen = 0; screen < 2; ++screen) {
        const int sy = y + 6 + screen * 12;
        round_rect(r, x + 6, sy, 16, 10, 3, "muted");
        round_rect(r, x + 8, sy + 2, 12, 6, 1, "panel");
    }
}

void App::icon_stage(SDL_Renderer* r, int y, const std::string& developer) {
    const int size = 128, box = size + 40, left = 320 - box / 2, top = y + 118;
    const Color shade = preferences.dark ? Color{0, 0, 0} : Color{24, 40, 60};
    for (int layer = 4; layer >= 1; layer--)
        alpha_round(r, left - layer * 3, top - layer * 3 + 8, box + layer * 6, box + layer * 6, 20 + layer * 3, shade,
                    preferences.dark ? 34 : 16);
    round_rect(r, left - 1, top - 1, box + 2, box + 2, 21, "line");
    round_rect(r, left, top, box, box, 20, "panel");
    game_icon(r, model.selected, 320 - size / 2, top + 20, size);
    if (stage_favourite) favourite_badge(r, left + box - 6, top + 6, 36);
    if (stage_dual) dual_badge(r, left + 10, top + box - 42);
    centered_wrapped(r, game_title(model.selected), y + 322, 26, 560, 2, "ink");
    if (!developer.empty()) {
        int w = std::min(560, label_width(developer, 18, false));
        raw_label(r, developer, 320 - w / 2, y + 394, 18, "muted", 560);
    }
}

bool App::cover_stage(SDL_Renderer* r, int y, const fs::path& path) {
    if (!media.ready(r, path)) return false;
    SDL_Rect box = media.fitted_box(r, path, {100, y + 70, 440, 344});
    // A new cover only settles in with a brief, still fade (no zoom or
    // lift): browsing quickly stays calm on the eyes.
    double t = preferences.animations && cover_motion_at ? std::min(1.0, (s.GetTicks() - cover_motion_at) / 110.0) : 1,
           ease = 1 - std::pow(1 - t, 2);
    const Color shade = preferences.dark ? Color{0, 0, 0} : Color{24, 40, 60};
    for (int layer = 4; layer >= 1; layer--)
        alpha_round(r, box.x - 6 - layer * 3, box.y - 6 - layer * 3 + 8, box.w + 12 + layer * 6, box.h + 12 + layer * 6,
                    14 + layer * 3, shade, preferences.dark ? 34 : 16);
    round_rect(r, box.x - 7, box.y - 7, box.w + 14, box.h + 14, 15, "line");
    round_rect(r, box.x - 6, box.y - 6, box.w + 12, box.h + 12, 14, "panel");
    bool drawn = media.draw(r, path, box, false, Uint8(std::lround(255 * (.8 + .2 * ease))), false, 8);
    if (stage_favourite) favourite_badge(r, box.x + box.w - 4, box.y, 36);
    if (stage_dual) dual_badge(r, box.x + 8, box.y + box.h - 40);
    // A faint glass sheen fading down from the top edge, like the DSi's shelves.
    s.SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    const int sheen = std::max(8, box.h * 2 / 5);
    for (int row = 0; row < sheen; row++) {
        int inset = row < 8 ? 8 - int(std::sqrt(64 - (8 - row - .5) * (8 - row - .5))) : 0;
        double fade = 1 - double(row) / sheen;
        s.SetRenderDrawColor(r, 255, 255, 255, Uint8(std::lround(34 * fade * fade)));
        SDL_Rect line{box.x + inset, box.y + row, box.w - 2 * inset, 1};
        s.RenderFillRect(r, &line);
    }
    return drawn;
}

std::string App::cover_notice() {
    if (return_writes_pending()) return "Restoring settings…";
    if (o.boot_startup && (!boot_hardware_ready || system.volume_pending())) return "Preparing controls…";
    if (status.find("ERROR") != std::string::npos) return status;
    return "";
}

void App::cover_line(SDL_Renderer* r, int y, uint64_t seconds) {
    const int cy = y + 455;
    const bool tips = !home_menu && settings.page == Page::Library && !picker_open && !launching,
               shoulders = tips && model.games.size() > 1;
    const std::string notice = cover_notice(), previous = locale.text("Previous"), next = locale.text("Next");
    const std::vector<KeyHint> view{{"Y", "View"}}, options{{"X", "Options"}};
    const bool faces = tips && notice.empty();
    const int middle = notice.empty() ? caption_width(seconds) : std::min(400, label_width(notice, 16)),
              view_w = faces ? key_hints_width(view, 14) : 0, options_w = faces ? key_hints_width(options, 14) : 0;
    bool words = true;
    const auto shoulder = [&](const std::string& key, const std::string& text) {
        return shoulders ? key_badge_width(key) + (words ? 8 + label_width(text, 14, false) : 0) + 18 : 0;
    };
    // Where each half ends: the pill keeps 16 px from both.
    const auto left_end = [&] { return 14 + shoulder("L", previous) + view_w + 16; };
    const auto right_start = [&] { return 640 - 14 - shoulder("R", next) - options_w - 16; };
    const auto fits = [&] { return 320 - middle / 2 >= left_end() && 320 + (middle + 1) / 2 <= right_start(); };
    if (middle && !fits()) words = false;
    if (shoulders) {
        const int l = key_badge_width("L"), rw = key_badge_width("R");
        key_badge(r, 14, cy, "L");
        key_badge(r, 640 - 14 - rw, cy, "R");
        if (words) {
            const int pw = label_width(previous, 14, false), nw = label_width(next, 14, false);
            raw_label(r, previous, 14 + l + 8, centred_label_y(previous, 14, cy), 14, "muted", pw + 2);
            raw_label(r, next, 640 - 14 - rw - 8 - nw, centred_label_y(next, 14, cy), 14, "muted", nw + 2);
        }
    }
    if (faces) {
        key_hints(r, 14 + shoulder("L", previous), cy, view, -1, 14, view_w + 1);
        key_hints(r, 626 - shoulder("R", next), cy, options, 1, 14, options_w + 1);
    }
    if (!middle) return;
    // Centred on the screen; only a pill too wide for that slides into the room that is left.
    const int lo = left_end(), hi = right_start();
    const int x = fits() ? 320 - middle / 2 : std::max(lo, (lo + hi - middle) / 2);
    if (!notice.empty()) {
        label(r, notice, x, centred_label_y(locale.text(notice), 16, cy), 16, "muted", middle);
        return;
    }
    cover_caption(r, x, cy, seconds);
}

std::string App::caption_time(uint64_t seconds) const {
    return std::to_string(seconds / 3600) + "h " + std::to_string((seconds / 60) % 60) + "m";
}

int App::caption_width(uint64_t seconds) {
    const uint64_t sessions = library_cache.stats(model.games[model.selected]).sessions;
    if (!sessions && !seconds) return 0;
    return 24 + label_width(caption_plays(sessions), 16, false) + 28 + 24 +
           label_width(caption_time(seconds), 16, false) + 28;
}

void App::cover_caption(SDL_Renderer* r, int x, int cy, uint64_t seconds) {
    const uint64_t sessions = library_cache.stats(model.games[model.selected]).sessions;
    const std::string plays = caption_plays(sessions), time = caption_time(seconds);
    const int a = label_width(plays, 16, false), b = label_width(time, 16, false);
    round_rect(r, x, cy - 16, caption_width(seconds), 32, 16, "panel");
    ui_icon(r, SettingsIcon::Play, x + 20, cy, 14, "accent");
    raw_label(r, plays, x + 34, centred_label_y(plays, 16, cy), 16, "ink", a + 4);
    x += 34 + a + 18;
    ui_icon(r, SettingsIcon::Clock, x + 10, cy, 14, "accent");
    raw_label(r, time, x + 24, centred_label_y(time, 16, cy), 16, "ink", b + 4);
}

App::LibraryHeader App::library_header_layout() {
    constexpr int margin = 8, padding = 20, gap = 8, height = 44;
    SDL_Rect card{margin, 12, 640 - margin * 2, 118};
    const int left = card.x + padding, right = card.x + card.w - padding, top = card.y + 12;
    // View and Options are button hints on the top screen (Y / X), as on
    // the DSi, and search lives on R2 (Q / E on a keyboard). For touch:
    // the first row (collection, count, search glyph) opens search, the
    // title row opens the view sheet, and a long press on the selected
    // game opens its options.
    SDL_Rect options{0, 0, 0, 0};
    // Touch target is a little wider than the shared 44 px back button.
    SDL_Rect back{left, top, back_size + 8, height};
    int category_x = back.x + back.w + gap;
    const std::array<std::string, 3> views = {
        preferences.show_systems ? std::string(system_info(shown_system).name) : "All games", "Favourites", "Recents"};
    int category_width = std::clamp(label_width(views[preferences.view], 16), 64, 124);
    SDL_Rect clear{right - 44, top, 44, height};
    SDL_Rect search{category_x, top, (discovery_active() ? clear.x : right) - category_x, height};
    SDL_Rect title{left, 84, right - left, 30};
    SDL_Rect view = title;
    return {card, back, view, options, title, search, clear, category_x, category_width};
}

bool App::library_control_active(LibraryPress control, SDL_Rect bounds) const {
    if (library_press != control) return false;
    if (pointer_down) return point_in(bounds, down_x, down_y) && point_in(bounds, contact_x, contact_y);
    return preferences.animations && s.GetTicks() - library_press_at < 120;
}

bool App::update_rail_feedback(Uint32 now) {
    const bool available = !probe && !picker_open && !launching && settings.page == Page::Library &&
                           (home_menu || browse_layout() != 2) && !(home_menu && preferences.home_layout == 1) &&
                           navigation_count() > 1;
    const std::array<bool, 3> held = {available && pointer_down && library_press == LibraryPress::LeftArrow &&
                                          point_in(rail_left_bounds(), contact_x, contact_y),
                                      available && pointer_down && library_press == LibraryPress::RightArrow &&
                                          point_in(rail_right_bounds(), contact_x, contact_y),
                                      available && pointer_down && carousel.dragging && carousel.range};
    bool active = false;
    for (size_t i = 0; i < rail_press.size(); i++) {
        if (rail_press[i].set(held[i], now, preferences.animations)) dirty = true;
        active = rail_press[i].animating(now, preferences.animations) || active;
    }
    // Paint the exact resting state before the event loop goes idle.
    if (active != rail_feedback_active) {
        rail_feedback_active = active;
        dirty = true;
    }
    return active;
}

Color App::rail_shelf(int row) {
    return row >= 26 ? theme.at("slot") : blend(theme.at("panel"), theme.at("slot"), std::max(0, row) * 100 / 26);
}

void App::draw_rail(SDL_Renderer* r, int y, size_t count, double position, std::array<double, 3> press_state,
                    bool bubble) {
    constexpr int control = RailGeometry::control, top_edge = RailGeometry::top, height = RailGeometry::height;
    constexpr int rail_left = RailGeometry::left, rail_width = RailGeometry::width;
    const bool dark = preferences.dark, enabled = count > 1;
    const int strip = y + top_edge;
    // The track: the whole strip between the blocks, shaded under its top edge.
    const Color well = blend(theme.at("background"), theme.at("slot"), 70);
    rect(r, 0, strip - 1, 640, 1, "line");
    tint_rect(r, rail_left, strip, rail_width, height, well);
    for (int row = 0; row < 5; row++)
        tint_rect(r, rail_left, strip + row, rail_width, 1, blend(well, shadow_colour(), (5 - row) * (dark ? 7 : 3)));
    for (int x : {0, 640 - control}) {
        const bool right = x > 320;
        const double press = press_state[right ? 1 : 0];
        const int sink = int(std::lround(2 * press));
        // A square block: its rim shows only along the top and the edge
        // facing the track; the outer sides run off the panel.
        SDL_Rect clip{x, strip, control, height};
        s.RenderSetClipRect(r, &clip);
        if (enabled) glossy_pill(r, right ? x : x - 1, strip, control + 1, height + 1, 0, theme.at("accent"), press);
        else {
            rect(r, x, strip, control, height, "line");
            rect(r, right ? x + 1 : x, strip + 1, control - 1, height - 1, "panel");
        }
        s.RenderSetClipRect(r, nullptr);
        // The DSi's solid triangle, pointing outward.
        const int direction = right ? 1 : -1, cx = x + control / 2 + direction * int(std::lround(2 * press)),
                  cy = strip + height / 2 + sink;
        for (int pass = 0; pass < 2; pass++) {
            if (pass == 0) {
                if (!enabled) continue;
                tint_colour(r, blend(theme.at("accent"), Color{0, 0, 0}, 30));
            } else color(r, enabled ? "on_accent" : "muted");
            const int shift = 1 - pass;
            for (int i = 0; i <= 14; i++) {
                const int half = i * 8 / 14;
                s.RenderDrawLine(r, cx + direction * (8 - i), cy - half + shift, cx + direction * (8 - i),
                                 cy + half + shift);
            }
        }
    }
    const auto rail = RailGeometry::at(count, position);
    const int middle = strip + height / 2;
    // Ticks: one per item while they fit, else a fixed texture. Those
    // behind the current position are dark, the rest quiet.
    const int travel = rail_width - rail.thumb_width, first = rail_left + rail.thumb_width / 2;
    const double pitch = enabled ? double(travel) / double(count - 1) : 0;
    const Color quiet = blend(well, theme.at("muted"), dark ? 55 : 45),
                done = blend(theme.at("muted"), theme.at("ink"), 35);
    auto ticks = [&](int from, int to, bool inside) {
        SDL_Rect clip{from, strip, to - from, height};
        s.RenderSetClipRect(r, &clip);
        if (pitch >= 7) {
            for (size_t i = 0; i < count; i++) {
                const int tx = first + int(std::lround(i * pitch));
                tint_rect(r, tx - 2, middle - 2, 4, 4, i < size_t(std::lround(position)) ? done : quiet);
            }
        } else
            for (int tx = first; tx <= first + travel; tx += 8)
                tint_rect(r, tx - 2, middle - 2, 4, 4, tx < rail.thumb_left + rail.thumb_width / 2 ? done : quiet);
        if (inside && enabled) {
            const int cx = rail.thumb_left + rail.thumb_width / 2;
            tint_rect(r, cx - 3, middle - 3, 6, 6, theme.at("accent"));
        }
        s.RenderSetClipRect(r, nullptr);
    };
    ticks(rail_left, rail_left + rail_width, false);
    // The grabber: a square white block the full height of the strip,
    // its top rim on the strip's top line and its bottom off the panel,
    // like the DSi's. Holding it widens it a little, adds an accent glow
    // at its sides and turns its shade over.
    const double press = press_state[2];
    const int grow = int(std::lround(2 * press)), top = strip - 1, grab_height = height + 2;
    const int thumb_left = rail.thumb_left - grow, thumb_width = rail.thumb_width + 2 * grow;
    if (press > 0)
        alpha_round(r, thumb_left - 3, top, thumb_width + 6, grab_height, 0, theme.at("accent"), int(60 * press));
    glossy_pill(r, thumb_left, top, thumb_width, grab_height, 0, dark ? theme.at("focus") : Color{255, 255, 255},
                press * .6);
    ticks(thumb_left + 4, thumb_left + thumb_width - 4, true);
    if (press > .05 && bubble && enabled) rail_bubble(r, y, rail.thumb_left + rail.thumb_width / 2, press);
}

void App::rail_bar(SDL_Renderer* r, int y) {
    rect(r, 0, y + 407, 640, 73, "slot");
    const Color top = theme.at("panel");
    for (int row = 0; row < 26; row++) tint_rect(r, 0, y + 408 + row, 640, 1, rail_shelf(row));
    rect(r, 0, y + 407, 640, 1, "line");
    tint_rect(r, 0, y + 408, 640, 1, blend(top, Color{255, 255, 255}, preferences.dark ? 6 : 60));
}

void App::rail_groove(SDL_Renderer* r, int y, int from, int to, size_t count, const RailGeometry& rail) {
    constexpr int rail_left = RailGeometry::left, rail_width = RailGeometry::width;
    SDL_Rect clip{from, y + 430, to - from, 30};
    s.RenderSetClipRect(r, &clip);
    round_rect(r, rail_left + 4, y + 436, rail_width - 8, 15, 7, "line");
    round_rect(r, rail_left + 5, y + 438, rail_width - 10, 12, 6, "background");
    const int pip_span = rail_width - rail.thumb_width - 4;
    const int pitch = count > 1 ? pip_span / int(count - 1) : 0, pip = pitch >= 9 ? 6 : 4;
    if (count > 1 && pitch >= 5) {
        const Color quiet = blend(theme.at("background"), theme.at("muted"), 60);
        for (size_t i = 0; i < count; i++) {
            int cx = rail_left + 2 + rail.thumb_width / 2 + int(std::lround(double(i) * pip_span / (count - 1)));
            bool inside = cx >= rail.window_left + 4 && cx < rail.window_left + rail.window_width - 4;
            fill_round(r, cx - pip / 2, y + 441, pip, 6, 1, inside ? theme.at("muted") : quiet);
        }
    }
    s.RenderSetClipRect(r, nullptr);
}

void App::rail_bubble(SDL_Renderer* r, int y, int cx, double press) {
    const std::string text = std::to_string(model.selected + 1) + " / " + std::to_string(model.games.size());
    const int width = label_width(text, 18, false) + 28, height = 34, left = std::clamp(cx - width / 2, 8, 632 - width);
    const int top = y + 380 + int(std::lround((1 - press) * 8));
    const Color shade = preferences.dark ? Color{0, 0, 0} : Color{24, 40, 60};
    alpha_round(r, left + 1, top + 4, width, height, 12, shade, int(40 * press));
    round_rect(r, left - 1, top - 1, width + 2, height + 2, 13, "line");
    round_rect(r, left, top, width, height, 12, "panel");
    for (int row = 0; row < 7; row++)
        tint_rect(r, cx - row, top + height + row, row * 2 + 1, 1, theme.at(row == 6 ? "line" : "panel"));
    raw_label(r, text, left + 14, top + 7, 18, "accent", width - 20);
}

void App::mark_library_press(int x, int y) {
    library_press = LibraryPress::None;
    library_press_row = -1;
    if (settings.page != Page::Library || picker_open) return;
    auto header = library_header_layout();
    if (!home_menu && point_in(header.back, x, y)) library_press = LibraryPress::Back;
    else if (!home_menu && !systems_menu && point_in(header.view, x, y)) library_press = LibraryPress::View;
    else if (point_in(header.options, x, y)) library_press = LibraryPress::Options;
    else if (!home_menu && !systems_menu && discovery_active() && point_in(header.clear, x, y))
        library_press = LibraryPress::Clear;
    else if (!home_menu && !systems_menu && point_in(header.search, x, y)) library_press = LibraryPress::Search;
    else if (browse_layout() == 2 && y >= LibraryViewport::top && y < LibraryViewport::top + LibraryViewport::height) {
        if (x >= 592 && x < 628) library_press = LibraryPress::Vertical;
        else if (x >= 24 && x < 592) {
            library_press_row = browser_list.row_at(y, navigation_count());
            if (library_press_row >= 0) library_press = LibraryPress::Row;
        }
    } else if ((home_menu || browse_layout() != 2) && y >= 409 && y < 480) {
        if (x >= 0 && x < RailGeometry::left) library_press = LibraryPress::LeftArrow;
        else if (x >= RailGeometry::left + RailGeometry::width && x < 640) library_press = LibraryPress::RightArrow;
        else library_press = LibraryPress::Horizontal;
    }
    if (library_press != LibraryPress::None) {
        library_press_at = s.GetTicks();
        micro_until = library_press_at + 120;
        // Seed on input so a complete tap queued between frames still has
        // release feedback, independent of the optional touch indicator.
        if (!probe && navigation_count() > 1) {
            int index = library_press == LibraryPress::LeftArrow    ? 0
                        : library_press == LibraryPress::RightArrow ? 1
                        : library_press == LibraryPress::Horizontal ? 2
                                                                    : -1;
            if (index >= 0) rail_press[size_t(index)].set(true, library_press_at, preferences.animations);
        }
    }
}

void App::library_header_action(SDL_Renderer* r, int y, SDL_Rect bounds, const std::string& title,
                                const std::string& button, bool enabled) {
    auto hit = bounds;
    bounds.y += y + 4;
    bounds.h -= 8;
    bool pressed = library_control_active(title == "View" ? LibraryPress::View : LibraryPress::Options, hit);
    int offset = pressed ? 1 : 0;
    round_rect(r, bounds.x, bounds.y + 1, bounds.w, bounds.h, 10, "slot");
    round_rect(r, bounds.x + offset, bounds.y + offset, bounds.w, bounds.h, 10, pressed ? "focus" : "panel");
    const int badge_x = bounds.x + 10, badge_y = bounds.y + (bounds.h - 20) / 2;
    round_rect(r, badge_x + offset, badge_y + offset, 20, 20, 10, enabled ? (pressed ? "accent" : "slot") : "slot");
    label(r, button, badge_x + offset + (20 - label_width(button, 14)) / 2,
          badge_y + offset - (preferences.pixel ? 3 : 0), 14, enabled ? (pressed ? "on_accent" : "accent") : "muted",
          20);
    label(r, title, badge_x + 28 + offset, bounds.y + offset + (bounds.h - 14) / 2 - (preferences.pixel ? 6 : 3), 14,
          enabled ? "ink" : "muted", label_width(title, 14));
}
