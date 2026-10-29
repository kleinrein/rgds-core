#pragma once
// The Game info page: what the public databases know about the selected game and the pictures kept for it.
// Top screen: the picture (with a thumbnail strip), the title, who made it and when, and chips for genre, rating,
// players and online play. Bottom screen: the description, or a details grid for a game that has none.
// Opened from Game options > Game info. A game without any of it shows how to get it. Members of App in main.cpp;
// included once, after settings_screen.hpp.

void App::open_game_info(Page back) {
    if (model.games.empty()) return;
    info_return = back;
    const auto folder = item_folder(model.selected);
    info_facts = load_game_facts(folder, preferences.language);
    info_pictures = gallery_pictures(folder);
    info_picture = info_scroll = info_scroll_max = 0;
    settings.page = Page::GameInfo;
    settings.focus = 0;
    dirty = true;
}

void App::game_info_step_picture(int step) {
    if (info_pictures.size() < 2) return;
    const int count = int(info_pictures.size());
    info_picture = (info_picture + step + count) % count;
    cover_motion_at = s.GetTicks();
    dirty = true;
}

void App::game_info_scroll_by(int delta) {
    info_scroll = std::clamp(info_scroll + delta, 0, info_scroll_max);
    dirty = true;
}

void App::game_info_key(int code) {
    if (code == 3) {
        back_to(info_return);
        if (info_return == Page::GameOptions) settings.focus = game_options_base() - 3;
    } else if (code == 8 || code == 9) game_info_scroll_by(code == 8 ? -50 : 50);
    else if (code == 0 || code == 13) game_info_step_picture(-1);
    else if (code == 1 || code == 14 || code == 2) game_info_step_picture(1);
    dirty = true;
}

bool App::game_removable() const {
    if (o.game_menu || model.games.empty() || model.selected >= model.games.size()) return false;
    const Game& game = model.games[model.selected];
    const std::string ext = lower(game.path.extension().string());
    std::error_code ec;
    return !game.demo && !is_tool_entry(game.path) && ext != ".cue" && ext != ".m3u" && ext != ".sh" &&
           fs::is_regular_file(game.path, ec);
}

void App::remove_game() {
    if (!game_removable()) return;
    const Game game = model.games[model.selected];
    std::error_code ec;
    fs::remove(game.path, ec);
    status = ec ? "Could not remove " + game.title : game.title + " removed";
    settings.page = Page::Library;
    rescan_games(false);
    refresh_other_systems();
    library_view(preferences.view);
    dirty = true;
}

// Taps on the bottom screen: the picture pill steps, the text pages up or down.
void App::game_info_touch(int x, int y) {
    if (info_pictures.size() > 1 && y >= 86 && y < 130) {
        game_info_step_picture(x < 320 ? -1 : 1);
        return;
    }
    game_info_scroll_by(y < 270 ? -170 : 170);
}

void App::game_info_top(SDL_Renderer* r, int y) {
    settings_card(r, 24, y + top_card_y, 592, top_card_h);
    y += 24; // the content, centred in the card
    const std::string title = info_facts.title.empty() ? game_title(model.selected) : info_facts.title;
    const int count = int(info_pictures.size());
    const int selected = count ? std::clamp(info_picture, 0, count - 1) : 0;

    // The picture, on the left; with several, a strip of thumbnails under it.
    const SDL_Rect frame{44, y + 76, 236, count > 1 ? 238 : 300};
    if (!count) {
        round_rect(r, frame.x, frame.y, frame.w, frame.h, 14, "slot");
        game_icon(r, model.selected, frame.x + frame.w / 2 - 56, frame.y + frame.h / 2 - 56, 112);
    } else {
        const auto& picture = info_pictures[size_t(selected)];
        if (!media.ready(r, picture.path)) media.request(r, picture.path);
        SDL_Rect box = media.fitted_box(r, picture.path, frame);
        const double t =
            preferences.animations && cover_motion_at ? std::min(1.0, (s.GetTicks() - cover_motion_at) / 110.0) : 1.0;
        alpha_round(r, box.x - 2, box.y + 4, box.w + 4, box.h + 4, 12, shadow_colour(), preferences.dark ? 70 : 28);
        round_rect(r, box.x - 4, box.y - 4, box.w + 8, box.h + 8, 12, "line");
        round_rect(r, box.x - 3, box.y - 3, box.w + 6, box.h + 6, 11, "panel");
        media.draw(r, picture.path, box, false, Uint8(std::lround(255 * (.75 + .25 * t))), false, 8);
    }
    if (count > 1) {
        const int size_w = 72, size_h = 54, gap = 10, strip = count * size_w + (count - 1) * gap;
        int x = frame.x + (frame.w - strip) / 2;
        const int top = y + 330;
        for (int i = 0; i < count; i++, x += size_w + gap) {
            const auto& picture = info_pictures[size_t(i)];
            if (!media.ready(r, picture.path)) media.request(r, picture.path);
            const SDL_Rect slot{x, top, size_w, size_h};
            const bool current = i == selected;
            round_rect(r, x - 3, top - 3, size_w + 6, size_h + 6, 10, current ? "accent" : "line");
            round_rect(r, x - 1, top - 1, size_w + 2, size_h + 2, 8, "panel");
            media.draw(r, picture.path, media.fitted_box(r, picture.path, slot), false, current ? 255 : 150, false, 4);
        }
        const std::string caption = locale.text(info_pictures[size_t(selected)].name);
        raw_label(r, caption, frame.x + frame.w / 2 - label_width(caption, 14, false) / 2, y + 394, 14, "muted", 236);
    }

    // The right column: title, who, when, then chips.
    const int left = 304, width = 296;
    int at = y + 82;
    const int title_lines = label_width(title, 26, false) > width ? 2 : 1;
    raw_wrapped(r, title, left, at, 26, width, 2);
    at += title_lines * 33 + 6;
    auto line = [&](const std::string& text, int size, const char* ink) {
        if (text.empty()) return;
        raw_label(r, text, left, at, size, ink, width);
        at += size + 9;
    };
    line(!info_facts.developer.empty() ? info_facts.developer : info_facts.publisher, 18, "ink");
    if (!info_facts.developer.empty() && !info_facts.publisher.empty() && info_facts.publisher != info_facts.developer)
        line(locale.text("Publisher") + ": " + info_facts.publisher, 15, "muted");
    line(info_facts.release, 15, "muted");
    if (!info_facts.any()) {
        wrapped(r, "No game information yet.", left, at + 6, 18, width, 2);
        return;
    }
    // Chips wrap onto a second row; the genre is the accented one.
    at += 8;
    int x = left;
    auto chip = [&](const std::string& text, bool accent) {
        if (text.empty()) return;
        const int w = std::min(width, label_width(text, 14, false) + 26);
        if (x + w > left + width) {
            x = left;
            at += 38;
        }
        if (accent) fill_round(r, x, at, w, 30, 15, theme.at("accent"));
        else {
            round_rect(r, x, at, w, 30, 15, "line");
            round_rect(r, x + 1, at + 1, w - 2, 28, 14, "slot");
        }
        raw_label_colour(r, text, x + 13, centred_label_y(text, 14, at + 15), 14,
                         accent ? theme.at("on_accent") : theme.at("ink"), w - 20);
        x += w + 8;
    };
    // GameTDB lists the main genre first, then finer ones: the main one is accented.
    for (size_t from = 0, n = 0; from <= info_facts.genre.size(); n++) {
        auto comma = info_facts.genre.find(", ", from);
        chip(info_facts.genre.substr(from, comma == std::string::npos ? std::string::npos : comma - from), n == 0);
        if (comma == std::string::npos) break;
        from = comma + 2;
    }
    chip(info_facts.rating, false);
    if (!info_facts.players.empty() && info_facts.players != "0")
        chip(info_facts.players == "1" ? locale.text("1 player") : locale.text(info_facts.players + " players"), false);
    if (info_facts.online) chip(locale.text("Online"), false);
    chip(info_facts.franchise, false);
}

void App::game_info_bottom(SDL_Renderer* r, int y) {
    settings_header(r, y, "Game info");
    const int count = int(info_pictures.size());
    if (count > 1) {
        // One pill: step back, the picture's name and place, step forward.
        const int x = 110, w = 420;
        alpha_round(r, x, y + 91, w, 40, 20, shadow_colour(), preferences.dark ? 60 : 20);
        round_rect(r, x, y + 88, w, 40, 20, "line");
        round_rect(r, x + 1, y + 89, w - 2, 38, 19, "panel");
        color(r, "ink");
        for (int side = 0; side < 2; side++) {
            const int cx = side ? x + w - 28 : x + 28, dx = side ? -5 : 5;
            for (int k = 0; k < 2; k++) {
                s.RenderDrawLine(r, cx + dx + k, y + 100, cx - dx + k, y + 108);
                s.RenderDrawLine(r, cx - dx + k, y + 108, cx + dx + k, y + 116);
            }
        }
        const std::string where = locale.text(info_pictures[size_t(info_picture)].name) + "  ·  " +
                                  std::to_string(info_picture + 1) + " / " + std::to_string(count);
        raw_label(r, where, x + w / 2 - label_width(where, 18, false) / 2, y + 97, 18, "ink", w - 100);
    }
    const int card_top = y + (count > 1 ? 140 : 92), card_height = y + 416 - card_top;
    alpha_round(r, 26, card_top + 3, 588, card_height, 14, shadow_colour(), preferences.dark ? 60 : 18);
    round_rect(r, 24, card_top, 592, card_height, 14, "panel");
    if (!info_facts.any()) {
        ui_icon(r, SettingsIcon::Info, 320, card_top + 90, 44, "muted");
        wrapped(r, "Turn on Game info in Settings > Library > Artwork sources, then fetch artwork.", 56, card_top + 138,
                20, 528, 4);
        return;
    }
    const int pixels = 18, stride = 26, inner_x = 44, inner_w = 520;
    auto measure = [&](const std::string& str) {
        return preferences.pixel ? float(pixel_cells(str) * std::max(2, (pixels + 6) / 10) * 6)
                                 : rounded.width(str, pixels);
    };
    // The description, wrapped paragraph by paragraph; without one, the details as a two-column grid.
    std::vector<std::string> lines;
    std::istringstream paragraphs(info_facts.synopsis);
    for (std::string paragraph; std::getline(paragraphs, paragraph);) {
        if (paragraph.find_first_not_of(" \t\r") == std::string::npos) {
            if (!lines.empty() && !lines.back().empty()) lines.emplace_back();
            continue;
        }
        std::string line;
        for (const auto& piece : line_pieces(paragraph)) {
            std::string candidate = line.empty() ? piece.text : line + (piece.space_before ? " " : "") + piece.text;
            if (!line.empty() && measure(candidate) > inner_w) {
                lines.push_back(line);
                line = piece.text;
            } else line = candidate;
        }
        if (!line.empty()) lines.push_back(line);
    }
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    const auto rows = info_facts.rows();
    const bool grid = lines.empty();
    const int grid_rows = int(rows.size() + 1) / 2, grid_h = grid_rows * 58;
    const int content = (grid ? grid_h : int(lines.size()) * stride) + 40;
    const int view_top = card_top + 14, view_h = card_height - 28;
    info_scroll_max = std::max(0, content - view_h);
    info_scroll = std::clamp(info_scroll, 0, info_scroll_max);
    SDL_Rect clip{26, card_top + 6, 574, card_height - 12};
    s.RenderSetClipRect(r, &clip);
    int at = view_top - info_scroll;
    if (grid) {
        for (size_t i = 0; i < rows.size(); i++) {
            const int cx = inner_x + int(i % 2) * 270, cy = at + int(i / 2) * 58;
            raw_label(r, locale.text(rows[i].first), cx, cy, 14, "muted", 250);
            raw_label(r, rows[i].second, cx, cy + 20, 20, "ink", 250);
        }
        at += grid_h;
    } else {
        for (const auto& line : lines) {
            if (!line.empty() && at + stride > view_top - 4 && at < view_top + view_h)
                raw_label(r, line, inner_x, at, pixels, "ink", inner_w);
            at += stride;
        }
    }
    if (!info_facts.provider.empty() && at + 24 > view_top - 4 && at < view_top + view_h)
        raw_label(r, locale.text("Source") + ": " + info_facts.provider, inner_x, at + 12, 14, "muted", inner_w);
    s.RenderSetClipRect(r, nullptr);
    if (info_scroll_max > 0) {
        const int thumb = std::max(40, view_h * view_h / content);
        draw_scrollbar(r, 584, view_top, view_h, thumb, info_scroll * (view_h - thumb) / info_scroll_max, false, false,
                       24);
    }
}
