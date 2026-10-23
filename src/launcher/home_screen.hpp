#pragma once
// Home: the DSi-style column of tiles on the lower screen and its top panel.
// Members of App in main.cpp; included once, right after the class.

bool App::home_column_move(int code) {
    static constexpr std::array<std::array<int, 2>, 4> rows = {{{5, 5}, {1, 2}, {3, 4}, {0, 0}}};
    int row = 0, column = home_column_memory;
    for (int i = 0; i < 4; i++) {
        if (rows[size_t(i)][0] == int(home_selected) || rows[size_t(i)][1] == int(home_selected)) {
            row = i;
            if (rows[size_t(i)][0] != rows[size_t(i)][1]) column = rows[size_t(i)][1] == int(home_selected) ? 1 : 0;
            break;
        }
    }
    if (code == 8 || code == 9) row = std::clamp(row + (code == 8 ? -1 : 1), 0, 3);
    else if (code == 0 || code == 1) column = code == 0 ? 0 : 1;
    else return false;
    if (rows[size_t(row)][0] != rows[size_t(row)][1]) home_column_memory = column;
    select_navigation(size_t(rows[size_t(row)][size_t(column)]), false);
    return true;
}

void App::home_column_touch(int x, int y) {
    for (const auto& tile : home_column_tiles())
        if (point_in(tile.box, x, y)) {
            select_navigation(tile.index, false);
            open_category();
            return;
        }
    if (point_in(home_bar_left, x, y)) {
        settings.section = 0;
        settings.page = Page::Display;
        settings.focus = 0;
        dirty = true;
        return;
    }
    if (point_in(home_bar_right, x, y)) {
        settings.section = 7;
        settings.page = Page::Statistics;
        settings.focus = 0;
        dirty = true;
        return;
    }
    if (point_in(home_bar_mark, x, y) && update_now()) return;
}

void App::home_column_bottom(SDL_Renderer* r, int y) {
    for (const auto& tile : home_column_tiles()) {
        const SDL_Rect b = tile.box;
        const bool selected = home_selected == tile.index;
        int bx = b.x, by = y + b.y;
        move_element(uint32_t(tile.index), bx, by, b.w, b.h, selected);
        const bool pressed = pointer_down && point_in(b, contact_x, contact_y) && point_in(b, down_x, down_y);
        const int sink = pressed ? 2 : 0;
        alpha_round(r, bx, by + 3, b.w, b.h, 12, shadow_colour(), preferences.dark ? 60 : 20);
        glossy_pill(r, bx, by + sink, b.w, b.h, 12, theme.at("panel"), pressed ? 1 : 0); // the brackets mark it
        if (tile.index == last_played_tile) {
            const auto& group = summary.groups[0];
            if (summary_ready && group.last_game) {
                game_icon(r, *group.last_game, bx + 20, by + 12 + sink, 64);
                label(r, "Last played", bx + 104, by + 16 + sink, 14, "muted", 460);
                raw_label(r, group.last_title, bx + 104, by + 38 + sink, 24, "ink", 460);
            } else {
                round_rect(r, bx + 20, by + 12 + sink, 64, 64, 8, "slot");
                ui_icon(r, SettingsIcon::Cartridge, bx + 52, by + 44 + sink, 32, "muted");
                label(r, summary_ready ? "Nothing played yet" : "Preparing…", bx + 104, by + 22 + sink, 22, "muted",
                      460);
                if (summary_ready)
                    label(r, "Your last game will appear here", bx + 104, by + 52 + sink, 14, "muted", 460);
            }
        } else {
            const int icon = tile.index == 0 ? 36 : 48;
            category_icon(r, tile.index, bx + (tile.index == 0 ? 24 : 18), by + (b.h - icon) / 2 + sink, icon);
            const std::string title = locale.text(category_title(tile.index));
            const bool note_row = tile.index != 0;
            raw_label(r, title, bx + (tile.index == 0 ? 76 : 84),
                      centred_label_y(title, 22, by + b.h / 2 + sink - (note_row ? 9 : 0)), 22, "ink", b.w - 100);
            std::string note;
            if (tile.index >= 1 && tile.index <= 3) {
                const auto& g = summary.groups[tile.index - 1];
                note = summary_ready ? locale.text(std::to_string(g.games) + (g.games == 1 ? " game" : " games")) : "…";
            } else if (tile.index == pictochat_tile)
                note = locale.text(device_status["wifi_connected"] == "1" ? "Wi-Fi connected" : "Draw and chat");
            if (!note.empty()) raw_label(r, note, bx + 84, by + b.h / 2 + 8 + sink, 14, "muted", b.w - 100);
        }
        if (selected) chat_brackets(r, bx, by + sink, b.w, b.h, theme.at("accent"), 4);
    }
    // The DSi's bottom bar: brightness on the left, the mark in the middle, the clock (statistics) on the right.
    rail_bar(r, y);
    for (int i = 0; i < 2; i++) {
        const SDL_Rect b = i ? home_bar_right : home_bar_left;
        const bool pressed = pointer_down && point_in(b, contact_x, contact_y) && point_in(b, down_x, down_y);
        raised_pill(r, b.x, y + b.y, b.w, b.h, 12, theme.at(pressed ? "focus" : "panel"), pressed ? 1 : 0);
        ui_icon(r, i ? SettingsIcon::Clock : SettingsIcon::Sun, b.x + b.w / 2, y + b.y + b.h / 2 + (pressed ? 2 : 0),
                26, "accent");
    }
    brand(r, 296, y + 421, 48);
    // An update waiting: a small accent glass badge on the mark, like a DSi notice.
    if (update_stage() != UpdateStage::None) {
        const int bx = 332, by = y + 414;
        const Color accent = theme.at("accent");
        alpha_round(r, bx, by + 2, 22, 22, 11, shadow_colour(), preferences.dark ? 70 : 36);
        fill_round(r, bx - 2, by - 2, 26, 26, 13, theme.at("panel"));
        glossy_pill(r, bx, by, 22, 22, 11, accent);
        update_glyph(r, bx + 11, by + 11, 22, tile_ink(accent), update_stage() == UpdateStage::Ready);
        s.SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    }
}

void App::home_column_top(SDL_Renderer* r, int y) {
    const auto now = std::time(nullptr);
    const std::tm t = *std::localtime(&now);
    const int cx = 168, cy = y + 241, radius = 135; // the card spans y 96..386, like the calendar
    alpha_round(r, cx - radius - 10, cy - radius - 6, 2 * radius + 20, 2 * radius + 20, 22, shadow_colour(),
                preferences.dark ? 60 : 18);
    round_rect(r, cx - radius - 10, cy - radius - 10, 2 * radius + 20, 2 * radius + 20, 22, "line");
    round_rect(r, cx - radius - 9, cy - radius - 9, 2 * radius + 18, 2 * radius + 18, 21, "panel");
    for (int i = 0; i < 12; i++) {
        const double a = i * 3.14159265358979 / 6;
        const int mx = cx + int(std::lround(std::sin(a) * (radius - 24))),
                  my = cy - int(std::lround(std::cos(a) * (radius - 24)));
        if (i % 3 == 0) {
            const std::string n = i == 0 ? "12" : std::to_string(i);
            raw_label(r, n, mx - label_width(n, 34, false) / 2, centred_label_y(n, 34, my), 34, "muted", 48);
        } else rect(r, mx - 3, my - 3, 6, 6, "line"); // the DSi's small square marks
    }
    auto hand = [&](double angle, int length, int weight, Color c) {
        s.SetRenderDrawColor(r, c.r, c.g, c.b, 255);
        const int ex = cx + int(std::lround(std::sin(angle) * length)),
                  ey = cy - int(std::lround(std::cos(angle) * length));
        for (int w = -weight / 2; w <= weight / 2; w++)
            for (int v = -weight / 2; v <= weight / 2; v++) s.RenderDrawLine(r, cx + w, cy + v, ex + w, ey + v);
    };
    hand((t.tm_hour % 12 + t.tm_min / 60.0) * 3.14159265358979 / 6, int(radius * .5), 4, theme.at("ink"));
    hand((t.tm_min + t.tm_sec / 60.0) * 3.14159265358979 / 30, int(radius * .78), 3, theme.at("ink"));
    hand(t.tm_sec * 3.14159265358979 / 30, int(radius * .84), 2, theme.at("accent"));
    rect(r, cx - 4, cy - 4, 8, 8, "ink");
    rect(r, cx - 2, cy - 2, 4, 4, "accent");
    // The calendar: month and year, weekday row, one cell per day, today lit.
    const int left = 330, top = y + 96, cell = 40;
    alpha_round(r, left, top + 3, 282, 290, 16, shadow_colour(), preferences.dark ? 60 : 18);
    round_rect(r, left, top, 282, 290, 16, "line");
    round_rect(r, left + 1, top + 1, 280, 288, 15, "panel");
    {
        char month[16];
        std::snprintf(month, sizeof(month), "%02d/%04d", t.tm_mon + 1, t.tm_year + 1900);
        raw_label(r, month, left + 141 - label_width(month, 22, false) / 2, top + 12, 22, "ink", 200);
    }
    static const std::array<const char*, 7> days = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};
    const Color sunday = preferences.dark ? Color{235, 110, 110} : Color{200, 56, 56},
                saturday = preferences.dark ? Color{120, 160, 240} : Color{56, 96, 200};
    for (int i = 0; i < 7; i++) {
        const int x = left + 1 + i * cell;
        tint_rect(r, x, top + 46, cell, 26, i == 0 ? sunday : i == 6 ? saturday : theme.at("slot"));
        const std::string d = locale.text(days[size_t(i)]);
        raw_label_colour(r, d, x + cell / 2 - label_width(d, 14, false) / 2, centred_label_y(d, 14, top + 59), 14,
                         i == 0 || i == 6 ? Color{255, 255, 255} : theme.at("ink"), cell);
    }
    std::tm first = t;
    first.tm_mday = 1;
    first.tm_hour = 12;
    std::mktime(&first);
    std::tm next = t;
    next.tm_mon += 1;
    next.tm_mday = 0;
    next.tm_hour = 12;
    std::mktime(&next);
    const int start = first.tm_wday, days_in_month = next.tm_mday, rows = (start + days_in_month + 6) / 7;
    // The weeks share the card down to its bottom edge, as on the DSi: the
    // last row takes the odd pixels and the washes follow the corners.
    const int weeks_top = top + 72, weeks_bottom = top + 289, row_height = (weeks_bottom - weeks_top) / rows;
    auto row_top = [&](int row) { return row == rows ? weeks_bottom : weeks_top + row * row_height; };
    auto wash = [&](int x, Color c) {
        for (int py = weeks_top; py < weeks_bottom; py++) {
            const int dy = py - (weeks_bottom - 15);
            const int inset = dy < 0 ? 0 : 15 - int(std::sqrt(std::max(0, 225 - (dy + 1) * (dy + 1))));
            const int from = x == left + 1 ? x + inset : x, to = x + cell == left + 281 ? x + cell - inset : x + cell;
            if (to > from) tint_rect(r, from, py, to - from, 1, c);
        }
    };
    // Like the DSi: the Sunday column washed pink, Saturday pale blue, a
    // fine grid between the days and today in a dotted frame.
    const Color grid = blend(theme.at("panel"), theme.at("line"), preferences.dark ? 70 : 55);
    wash(left + 1, blend(theme.at("panel"), sunday, preferences.dark ? 14 : 12));
    wash(left + 1 + 6 * cell, blend(theme.at("panel"), saturday, preferences.dark ? 14 : 12));
    for (int i = 1; i < 7; i++) tint_rect(r, left + 1 + i * cell, top + 46, 1, weeks_bottom - top - 46, grid);
    for (int row = 0; row < rows; row++) tint_rect(r, left + 1, row_top(row), 7 * cell, 1, grid);
    for (int day = 1; day <= days_in_month; day++) {
        const int slot = start + day - 1, col = slot % 7, row = slot / 7;
        const int x = left + 1 + col * cell, ry = row_top(row), rh = row_top(row + 1) - ry;
        const bool today = day == t.tm_mday;
        if (today) {
            fill_round(r, x + 3, ry + 3, cell - 5, rh - 5, 3, theme.at("panel"));
            const Color mark = theme.at("accent");
            for (int d = 0; d < cell - 6; d += 3) {
                tint_rect(r, x + 3 + d, ry + 3, 2, 2, mark);
                tint_rect(r, x + 3 + d, ry + rh - 4, 2, 2, mark);
            }
            for (int d = 0; d < rh - 5; d += 3) {
                tint_rect(r, x + 3, ry + 3 + d, 2, 2, mark);
                tint_rect(r, x + cell - 4, ry + 3 + d, 2, 2, mark);
            }
        }
        const std::string n = std::to_string(day);
        raw_label_colour(r, n, x + cell / 2 - label_width(n, 16, false) / 2, centred_label_y(n, 16, ry + rh / 2 + 1),
                         16,
                         today      ? theme.at("accent")
                         : col == 0 ? sunday
                         : col == 6 ? saturday
                                    : theme.at("ink"),
                         cell);
    }
    // The clock and calendar speak for themselves; the tiles below already
    // say what they hold. Only a first scan and an update get a line.
    if (banner_shown()) {
        update_banner(r, y + 402);
        return;
    }
    if (!library_ready) {
        const std::string line = locale.text("Preparing your cartridges…");
        const int w = std::min(600, label_width(line, 16, false));
        raw_label(r, line, 320 - w / 2, y + 432, 16, "muted", 600);
    }
}
