#pragma once
// On-screen keyboards: the settings search keyboard and the shared keyboard
// (rows, keys, touch) used for names and search.
// Members of App in main.cpp; included once, right after the class.

void App::keyboard_screen(SDL_Renderer* r, int y) {
    settings_header(r, y,
                    edit_field == 5   ? "DS nickname"
                    : edit_field == 4 ? "Search games"
                    : edit_field == 6 ? "Search cheats"
                    : edit_field == 3 ? "Matching title"
                    : edit_field == 0 ? "Username"
                                      : "Password");
    round_rect(r, 20, y + 91, 600, 62, 12, "accent");
    round_rect(r, 22, y + 93, 596, 58, 10, "panel");
    std::string value = (edit_field == 0 || edit_field == 3 || edit_field == 4 || edit_field == 5 || edit_field == 6)
                            ? edit_buffer
                            : std::string(RoundedFont::codepoints(edit_buffer).size(), '*');
    bool hidden = edit_field == 1 || edit_field == 2;
    size_t cursor = hidden ? RoundedFont::codepoints(edit_buffer.substr(0, edit_cursor)).size() : edit_cursor;
    while (cursor && label_width(value.substr(0, cursor), 24, false) > 480) {
        size_t next = 1;
        while (next < value.size() && (static_cast<unsigned char>(value[next]) & 0xc0) == 0x80) next++;
        value.erase(0, next);
        cursor -= next;
    }
    const bool searching = edit_field == 4 || edit_field == 6;
    int x = searching ? 64 : 38;
    if (searching) search_icon(r, 43, y + 121, 22, "accent");
    raw_label(r,
              value.empty() ? locale.text(edit_field == 4   ? "Type a game title…"
                                          : edit_field == 6 ? "Type a cheat name…"
                                                            : "Enter here")
                            : value,
              x, y + 109, 24, value.empty() ? "muted" : "ink", 490);
    if (!edit_buffer.empty())
        rect(r, x + label_width(value.substr(0, cursor), 24, false) + 2, y + 109, 2, 28, "accent");
    if (!edit_buffer.empty()) {
        round_rect(r, 568, y + 103, 40, 38, 10, "slot");
        label(r, "X", 582, y + 112, 18, "muted", 24);
    }
    label(r,
          edit_field == 4 ? (search_preview_loading ? "Finding games…"
                                                    : locale.text(std::to_string(search_matches.size()) +
                                                                  (search_matches.size() == 1 ? " match" : " matches")))
          : edit_field == 6 ? locale.text(std::to_string(cheat_match_count(edit_buffer)) +
                                          (cheat_match_count(edit_buffer) == 1 ? " match" : " matches"))
                            : "Tap to type, or use the D-pad and A",
          24, y + 160, 14, "muted", 580);
    draw_keyboard(r, y, keyboard_rows(), true);
}

void App::search_keyboard_top(SDL_Renderer* r, int y) {
    label(r, "Search in", 28, y + 78, 16, "muted", 580);
    static const std::array<std::string, 3> views = {"All games", "Favourites", "Recently played"};
    label(r, views[preferences.view], 28, y + 102, 30, "ink", 580);
    if (const auto filters = discovery_filters_text(); !filters.empty())
        raw_label(r, filters, 28, y + 146, 18, "accent", 580);
    std::string message = search_preview_loading
                              ? "Finding games…"
                              : locale.text(std::to_string(search_matches.size()) +
                                            (search_matches.size() == 1 ? " matching game" : " matching games"));
    label(r, message, 28, y + 185, 22, "accent", 580);
    if (!search_preview_loading && search_matches.empty())
        wrapped(r, "Try a shorter title or clear your filter.", 28, y + 236, 22, 570, 2);
    for (size_t i = 0; i < std::min(size_t(4), search_matches.size()); i++) {
        round_rect(r, 24, y + 228 + int(i) * 49, 592, 42, 9, "panel");
        raw_label(r, library_title(search_matches[i].title), 42, y + 237 + int(i) * 49, 20, "ink", 554);
    }
    key_hints(r, 28, y + 451, {{"A", "Type"}, {"Start", "Search"}, {"B", "Cancel"}});
}

std::vector<std::vector<KeyboardKey>> App::keyboard_rows() const {
    const bool room = chat_open && chat.view == pictochat::View::Room;
    return keyboard_layout(key_layer,
                           room                                                 ? KeyboardMode::Chat
                           : (edit_field == 4 || edit_field == 6) && !chat_open ? KeyboardMode::Search
                                                                                : KeyboardMode::Edit,
                           key_caps, key_accents, room && key_stamps, room ? pictochat::layout::keyboard_top : 186,
                           room ? pictochat::layout::keyboard_scale : 1.f);
}

void App::keyboard_clamp() {
    auto rows = keyboard_rows();
    key_row = std::clamp(key_row, 0, int(rows.size()) - 1);
    key_col = std::clamp(key_col, 0, int(rows[size_t(key_row)].size()) - 1);
}

bool App::key_down(const SDL_Rect& b) const {
    return pointer_down && point_in(b, contact_x, contact_y) && point_in(b, down_x, down_y);
}

void App::draw_keyboard(SDL_Renderer* r, int y, const std::vector<std::vector<KeyboardKey>>& rows, bool show_focus) {
    const bool room = chat_open && chat.view == pictochat::View::Room;
    const Color tint = room ? chat_tint() : theme.at("accent"), on_tint = room ? tile_ink(tint) : theme.at("on_accent");
    int bottom = 0, top = 480, left = 640, right = 0;
    for (const auto& row : rows)
        for (const auto& key : row) {
            bottom = std::max(bottom, key.bounds.y + key.bounds.h);
            top = std::min(top, key.bounds.y);
            left = std::min(left, key.bounds.x);
            right = std::max(right, key.bounds.x + key.bounds.w);
        }
    round_rect(r, left - 6, y + top - 6, right - left + 12, bottom - top + 12, 10, "slot");
    for (size_t row = 0; row < rows.size(); row++)
        for (size_t col = 0; col < rows[row].size(); col++) {
            auto key = rows[row][col];
            auto b = key.bounds;
            b.y += y;
            const bool chosen = show_focus && key_row == int(row) && key_col == int(col),
                       submit = key.action == KeyAction::Submit;
            const bool armed =
                key.action == KeyAction::Stamp && chat.stamp_armed && std::to_string(chat.stamp) == key.label;
            const bool lit = (key.action == KeyAction::Shift && key_layer == 1) ||
                             (key.action == KeyAction::Caps && key_caps) ||
                             (key.action == KeyAction::Accents && key_accents) ||
                             (key.action == KeyAction::Stamps && key_stamps) || armed;
            const bool down = key_down(b), filled = chosen || submit || lit;
            const int sink = down ? 2 : 0;
            round_rect(r, b.x, b.y + 2, b.w, b.h, 7, "line");
            if (filled) fill_round(r, b.x, b.y + sink, b.w, b.h, 7, tint);
            else
                round_rect(r, b.x, b.y + sink, b.w, b.h, 7,
                           down                                                                   ? "focus"
                           : key.action == KeyAction::Character || key.action == KeyAction::Stamp ? "panel"
                                                                                                  : "focus");
            const Color ink = filled ? on_tint : theme.at("ink");
            const std::string ink_key = filled ? "on_accent" : "ink";
            const int cx = b.x + b.w / 2, cy = b.y + sink + b.h / 2;
            if (key.action == KeyAction::Stamp) {
                chat_icon(r, pictochat::ui_icon_count + std::stoi(key.label), cx, cy,
                          armed ? on_tint : chat_ink(chat.ink == pictochat::rainbow ? 0 : chat.ink));
                continue;
            }
            if (key.action == KeyAction::Stamps) {
                chat_icon(r, 13, cx, cy, ink);
                continue;
            }
            color(r, ink_key);
            if (key.action == KeyAction::Shift) {
                s.RenderDrawLine(r, cx, cy - 10, cx - 11, cy + 1);
                s.RenderDrawLine(r, cx, cy - 10, cx + 11, cy + 1);
                s.RenderDrawLine(r, cx - 11, cy + 1, cx - 5, cy + 1);
                s.RenderDrawLine(r, cx + 11, cy + 1, cx + 5, cy + 1);
                s.RenderDrawLine(r, cx - 5, cy + 1, cx - 5, cy + 11);
                s.RenderDrawLine(r, cx + 5, cy + 1, cx + 5, cy + 11);
                s.RenderDrawLine(r, cx - 5, cy + 11, cx + 5, cy + 11);
            } else if (key.action == KeyAction::Backspace) {
                s.RenderDrawLine(r, cx - 18, cy, cx - 8, cy - 11);
                s.RenderDrawLine(r, cx - 8, cy - 11, cx + 17, cy - 11);
                s.RenderDrawLine(r, cx + 17, cy - 11, cx + 17, cy + 11);
                s.RenderDrawLine(r, cx + 17, cy + 11, cx - 8, cy + 11);
                s.RenderDrawLine(r, cx - 8, cy + 11, cx - 18, cy);
                s.RenderDrawLine(r, cx - 3, cy - 5, cx + 7, cy + 5);
                s.RenderDrawLine(r, cx - 3, cy + 5, cx + 7, cy - 5);
            } else if (key.action == KeyAction::Left || key.action == KeyAction::Right) {
                const int sign = key.action == KeyAction::Left ? -1 : 1;
                s.RenderDrawLine(r, cx - 8 * sign, cy, cx + 8 * sign, cy);
                s.RenderDrawLine(r, cx + 8 * sign, cy, cx + 2 * sign, cy - 6);
                s.RenderDrawLine(r, cx + 8 * sign, cy, cx + 2 * sign, cy + 6);
            } else if (key.action == KeyAction::Space && room) {
                tint_rect(r, cx - 70, cy + 4, 140, 3, filled ? on_tint : theme.at("muted"));
            } else if (key.action == KeyAction::Newline) {
                // The return arrow of the original's Enter key.
                s.RenderDrawLine(r, cx + 12, cy - 9, cx + 12, cy + 3);
                s.RenderDrawLine(r, cx + 12, cy + 3, cx - 10, cy + 3);
                s.RenderDrawLine(r, cx - 10, cy + 3, cx - 4, cy - 3);
                s.RenderDrawLine(r, cx - 10, cy + 3, cx - 4, cy + 9);
            } else {
                const int size = key.action == KeyAction::Character ? 22 : 16;
                const bool legend = key.action == KeyAction::Character || key.action == KeyAction::Caps ||
                                    key.action == KeyAction::Cancel || key.action == KeyAction::Delete ||
                                    key.action == KeyAction::Accents || key.label == "Enter";
                const auto value = legend ? key.label : locale.text(key.label);
                const int width = label_width(value, size, false);
                raw_label(r, value, b.x + (b.w - width) / 2,
                          b.y + sink + (b.h - size) / 2 + (key.alternate.empty() ? -2 : 4), size, ink_key, b.w - 4);
                if (!key.alternate.empty())
                    raw_label(r, key.alternate, b.x + 4, b.y + sink + 2, 11, filled ? "on_accent" : "muted", b.w - 8);
            }
        }
}

void App::keyboard_apply(const KeyboardKey& key) {
    const bool room = chat_open && chat.view == pictochat::View::Room,
               naming = chat_open && chat.view == pictochat::View::Naming;
    switch (key.action) {
    case KeyAction::Shift: key_layer = key_layer == 0 ? 1 : 0; break;
    case KeyAction::Caps: key_caps = !key_caps; break;
    case KeyAction::Tab: {
        auto at = keyboard_submit(keyboard_rows());
        key_row = at.first;
        key_col = at.second;
        break;
    }
    case KeyAction::Accents:
        key_accents = !key_accents;
        keyboard_clamp();
        break;
    case KeyAction::Letters:
        key_stamps = false;
        key_row = 1;
        key_col = 1;
        keyboard_clamp();
        break;
    case KeyAction::Stamps:
        key_stamps = true;
        key_row = 0;
        key_col = 0;
        break;
    case KeyAction::Stamp:
        chat.stamp = std::stoi(key.label);
        chat.stamp_armed = true;
        chat.eraser = false;
        break;
    case KeyAction::Backspace:
        if (room) chat.backspace();
        else erase_character();
        break;
    case KeyAction::Delete: delete_character(); break;
    case KeyAction::Cancel:
        if (naming) chat_finish_name(false);
        else finish_edit(false);
        return;
    case KeyAction::Left: move_edit_cursor(-1); break;
    case KeyAction::Right: move_edit_cursor(1); break;
    case KeyAction::Submit:
        if (naming) chat_finish_name(true);
        else if (room) chat_send();
        else finish_edit(true);
        return;
    case KeyAction::Space:
        if (room) chat.type(" ");
        else insert_text(" ");
        break;
    case KeyAction::Newline: chat.type("\n"); break;
    case KeyAction::Character:
        if (room) chat.type(key.label);
        else insert_text(key.label);
        key_layer = 0;
        break;
    }
    dirty = true;
}

void App::keyboard_choose() {
    keyboard_clamp();
    auto rows = keyboard_rows();
    keyboard_apply(rows.at(size_t(key_row)).at(size_t(key_col)));
}

void App::keyboard_move(int code) {
    auto rows = keyboard_rows();
    keyboard_clamp();
    if (code == 8 || code == 9) {
        int x = rows[size_t(key_row)][size_t(key_col)].bounds.x + rows[size_t(key_row)][size_t(key_col)].bounds.w / 2;
        key_row = std::clamp(key_row + (code == 8 ? -1 : 1), 0, int(rows.size()) - 1);
        key_col = keyboard_nearest(rows[size_t(key_row)], x);
    } else
        key_col = std::clamp(key_col + (code == 0 ? -1 : code == 1 ? 1 : 0), 0, int(rows[size_t(key_row)].size()) - 1);
    key_focus_visible = true;
}

bool App::keyboard_touch(int x, int y) {
    auto rows = keyboard_rows();
    for (size_t row = 0; row < rows.size(); row++)
        for (size_t col = 0; col < rows[row].size(); col++)
            if (point_in(rows[row][col].bounds, x, y)) {
                key_row = int(row);
                key_col = int(col);
                key_focus_visible = false;
                emit_sound(MenuCue::Type);
                keyboard_choose();
                return true;
            }
    return false;
}
