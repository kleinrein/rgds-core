#pragma once
// The Game info page: what the public databases know about the selected game (facts and a description) and the
// pictures kept for it (cover, title screen, screenshot). Opened from Game options > Artwork > Game info; a game
// without any of it shows how to get it. Members of App in main.cpp; included once, after settings_screen.hpp.

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
        if (info_return == Page::GameArtwork) settings.focus = 3;
    } else if (code == 8 || code == 9) game_info_scroll_by(code == 8 ? -46 : 46);
    else if (code == 0 || code == 13) game_info_step_picture(-1);
    else if (code == 1 || code == 14 || code == 2) game_info_step_picture(1);
    dirty = true;
}

// Taps on the bottom screen: the picture buttons step, the text pages up or down.
void App::game_info_touch(int x, int y) {
    if (info_pictures.size() > 1 && y >= 86 && y < 130) {
        game_info_step_picture(x < 320 ? -1 : 1);
        return;
    }
    game_info_scroll_by(y < 250 ? -160 : 160);
}

void App::game_info_top(SDL_Renderer* r, int y) {
    settings_card(r, 24, y + 56, 592, 368);
    const SDL_Rect area{48, y + 74, 544, 264};
    const std::string title = info_facts.title.empty() ? game_title(model.selected) : info_facts.title;
    if (info_pictures.empty()) {
        game_icon(r, model.selected, 320 - 64, y + 124, 128);
        centered(r, "No pictures yet", y + 292, 1, "muted");
    } else {
        const auto& picture = info_pictures[size_t(std::clamp(info_picture, 0, int(info_pictures.size()) - 1))];
        if (!media.ready(r, picture.path)) media.request(r, picture.path);
        SDL_Rect box = media.fitted_box(r, picture.path, area);
        round_rect(r, box.x - 4, box.y - 4, box.w + 8, box.h + 8, 11, "line");
        round_rect(r, box.x - 3, box.y - 3, box.w + 6, box.h + 6, 10, "panel");
        const double t =
            preferences.animations && cover_motion_at ? std::min(1.0, (s.GetTicks() - cover_motion_at) / 110.0) : 1.0;
        media.draw(r, picture.path, box, false, Uint8(std::lround(255 * (.75 + .25 * t))), false, 7);
    }
    const int width = std::min(540, label_width(title, 22, false));
    raw_label(r, title, 320 - width / 2, y + 350, 22, "ink", 540);
    if (info_pictures.empty()) return;
    // Which picture this is, and how many there are.
    const std::string caption = locale.text(info_pictures[size_t(info_picture)].name);
    const int caption_width = std::min(300, label_width(caption, 16, false));
    raw_label(r, caption, 320 - caption_width / 2, y + 380, 16, "muted", 300);
    if (info_pictures.size() > 1) {
        const int count = int(info_pictures.size()), left = 320 - (count * 14 - 6) / 2;
        for (int i = 0; i < count; i++)
            fill_round(r, left + i * 14, y + 406, 8, 8, 4, i == info_picture ? theme.at("accent") : theme.at("slot"));
    }
}

void App::game_info_bottom(SDL_Renderer* r, int y) {
    settings_header(r, y, "Game info");
    const bool pictures = info_pictures.size() > 1;
    if (pictures) {
        const auto& picture = info_pictures[size_t(info_picture)];
        for (int side = 0; side < 2; side++) {
            const int x = side ? 480 : 28;
            round_rect(r, x, y + 88, 132, 40, 20, "line");
            round_rect(r, x + 1, y + 89, 130, 38, 19, "panel");
            // A chevron drawn with lines: the pictures' step buttons.
            color(r, "ink");
            const int cx = x + 66, dx = side ? -5 : 5;
            for (int w = 0; w < 2; w++) {
                s.RenderDrawLine(r, cx + dx + w, y + 100, cx - dx + w, y + 108);
                s.RenderDrawLine(r, cx - dx + w, y + 108, cx + dx + w, y + 116);
            }
        }
        const std::string name = locale.text(picture.name);
        const std::string where =
            name + "  " + std::to_string(info_picture + 1) + " / " + std::to_string(info_pictures.size());
        raw_label(r, where, 320 - label_width(where, 18, false) / 2, y + 97, 18, "ink", 320);
    }
    const int top = y + (pictures ? 138 : 92), height = y + 420 - top;
    if (!info_facts.any()) {
        ui_icon(r, SettingsIcon::Art, 320, top + 70, 44, "muted");
        wrapped(
            r,
            "No game information yet. Turn on Game info in Settings > Library > Artwork sources, then fetch artwork.",
            52, top + 120, 20, 536, 5);
        return;
    }
    // One scrolling column: the facts, then the description.
    std::vector<std::pair<std::string, std::string>> rows;
    for (const auto& [label_text, value] : info_facts.rows()) rows.emplace_back(locale.text(label_text), value);
    const int pixels = 18, stride = 25, text_stride = pixels + 7, text_width = 536;
    auto measure = [&](const std::string& str) {
        return preferences.pixel ? float(pixel_cells(str) * std::max(2, (pixels + 6) / 10) * 6)
                                 : rounded.width(str, pixels);
    };
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
            if (!line.empty() && measure(candidate) > text_width) {
                lines.push_back(line);
                line = piece.text;
            } else line = candidate;
        }
        if (!line.empty()) lines.push_back(line);
    }
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    const int facts_height = int(rows.size()) * stride + (rows.empty() ? 0 : 12);
    const int content = facts_height + int(lines.size()) * text_stride + 28;
    info_scroll_max = std::max(0, content - height);
    info_scroll = std::clamp(info_scroll, 0, info_scroll_max);
    SDL_Rect clip{20, top, 570, height};
    s.RenderSetClipRect(r, &clip);
    int at = top - info_scroll;
    for (const auto& [name, value] : rows) {
        if (at + stride > top && at < top + height) {
            raw_label(r, name, 36, at + 2, 14, "muted", 120);
            raw_label(r, value, 164, at, pixels, "ink", 420);
        }
        at += stride;
    }
    if (!rows.empty()) at += 12;
    for (const auto& line : lines) {
        if (!line.empty() && at + text_stride > top && at < top + height)
            raw_label(r, line, 36, at, pixels, "ink", text_width);
        at += text_stride;
    }
    if (!info_facts.provider.empty() && at + 24 > top && at < top + height)
        raw_label(r, locale.text("Source") + ": " + info_facts.provider, 36, at + 8, 14, "muted", text_width);
    s.RenderSetClipRect(r, nullptr);
    if (info_scroll_max > 0) {
        const int thumb = std::max(54, height * height / content);
        draw_scrollbar(r, 596, top, height, thumb, info_scroll * (height - thumb) / info_scroll_max, false, false);
    }
}
