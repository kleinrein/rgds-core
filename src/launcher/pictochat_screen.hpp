#pragma once
// PictoChat: the lobby, naming and room screens, the chat keyboard hooks and
// the network polling that feeds them.
// Members of App in main.cpp; included once, right after the class.

void App::pictochat_top(SDL_Renderer* r, int y) {
    category_icon(r, pictochat_tile, 272, y + 104, 96);
    centered(r, "PictoChat", y + 216, 2, "ink");
    centered(r, "Draw and chat with nearby handhelds", y + 262, 1, "muted", 72);
    const bool on = device_status["wifi_connected"] == "1";
    const std::string chip = on ? "Wi-Fi connected" : "No Wi-Fi: you can still draw";
    const int width = label_width(chip, 18) + 48, left = 320 - width / 2;
    round_rect(r, left, y + 302, width, 38, 19, on ? "accent" : "slot");
    label_colour(r, chip, left + 24, y + 310, 18, on ? theme.at("on_accent") : theme.at("muted"), width - 30);
    key_hints(r, 320, y + 432, {{"A", "Open"}}, 0);
}

int App::chat_accent_index() const {
    return preferences.accent >= 0 && preferences.accent < 16 ? preferences.accent : 11;
}

fs::path App::chat_helper_script() const {
    if (!o.app.empty()) return o.app / "runtime/pictochat_net.py";
    std::error_code ec;
    return fs::weakly_canonical(o.executable.parent_path() / "../../runtime/pictochat_net.py",
                                ec); // a development build beside the sources
}

void App::chat_save_profile() {
    if (!o.data.empty()) chat.save_profile(o.data / "pictochat.ini");
}

void App::open_pictochat() {
    if (chat_open || launching || probe) return;
    cancel_navigation();
    chat_open = true;
    chat.glyphs = &chat_glyphs;
    chat.load_profile(o.data.empty() ? fs::path() : o.data / "pictochat.ini", preferences.accent);
    chat.view = pictochat::View::Lobby;
    chat_focus = 0;
    chat_seen_serial = chat_slid_serial = chat.serial;
    chat_scroll = 0;
    key_layer = 0;
    key_caps = false;
    key_accents = key_stamps = false;
    key_focus_visible = false;
    std::vector<std::string> extra;
    if (const char* more = std::getenv("RGDS_PICTOCHAT_ARGS")) {
        std::istringstream words(more);
        std::string word;
        while (words >> word) extra.push_back(word);
    }
    chat.helper.start(chat_helper_script(), extra); // without it the pad still works, on this handheld alone
    s.StartTextInput();
    emit_sound(MenuCue::Open);
    dirty = true;
}

void App::close_pictochat() {
    if (!chat_open) return;
    chat.close();
    chat_open = false;
    chat_shoulder = 0;
    s.StopTextInput();
    edit_buffer.clear();
    for (auto& entry : chat_cards)
        if (entry.second) s.DestroyTexture(entry.second);
    chat_cards.clear();
    for (auto& entry : chat_pads)
        if (entry.second) s.DestroyTexture(entry.second);
    chat_pads.clear();
    for (auto& entry : chat_sheets)
        if (entry.second) s.DestroyTexture(entry.second);
    chat_sheets.clear();
    pointer_down = false;
    key_focus_visible = true;
    emit_sound(MenuCue::Back);
    dirty = true;
}

SDL_Texture* App::chat_upload(SDL_Renderer* r, const std::vector<uint32_t>& pixels, int w, int h, bool blend) {
    auto* texture = s.CreateTexture(r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
    if (!texture) return nullptr;
    if (s.UpdateTexture(texture, nullptr, pixels.data(), w * 4) != 0) {
        s.DestroyTexture(texture);
        return nullptr;
    }
    s.SetTextureBlendMode(texture, blend ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
    return texture;
}

SDL_Texture* App::chat_card(SDL_Renderer* r, const pictochat::Entry& entry) {
    auto key = std::make_pair(r, entry.serial);
    auto found = chat_cards.find(key);
    if (found != chat_cards.end()) return found->second;
    pictochat::Canvas canvas;
    pictochat::render(canvas, entry.ops, &chat_glyphs);
    return chat_cards[key] = chat_upload(r, canvas.px, pictochat::canvas_w, pictochat::canvas_h, false);
}

SDL_Texture* App::chat_pad_texture(SDL_Renderer* r) {
    auto& texture = chat_pads[r];
    if (!texture) {
        texture = chat_upload(r, chat.pad.px, pictochat::canvas_w, pictochat::canvas_h, false);
        chat.pad_dirty = {};
        return texture;
    }
    if (!chat.pad_dirty.empty()) {
        auto d = chat.pad_dirty;
        d.x0 = std::max(0, d.x0);
        d.y0 = std::max(0, d.y0);
        d.x1 = std::min(pictochat::canvas_w, d.x1);
        d.y1 = std::min(pictochat::canvas_h, d.y1);
        if (d.x1 > d.x0 && d.y1 > d.y0) {
            SDL_Rect area{d.x0, d.y0, d.x1 - d.x0, d.y1 - d.y0};
            s.UpdateTexture(texture, &area, &chat.pad.px[size_t(d.y0) * pictochat::canvas_w + d.x0],
                            pictochat::canvas_w * 4);
        }
        chat.pad_dirty = {};
    }
    return texture;
}

SDL_Texture* App::chat_sheet(SDL_Renderer* r) {
    auto& texture = chat_sheets[r];
    if (texture) return texture;
    const int total = pictochat::ui_icon_count + pictochat::stamp_count, width = chat_sheet_left(total);
    std::vector<uint32_t> pixels(size_t(width) * chat_stamp_cell, 0);
    for (int id = 0; id < total; id++) {
        const int cell = chat_cell(id), left = chat_sheet_left(id);
        for (int py = 0; py < cell; py++)
            for (int px = 0; px < cell; px++) {
                const float u = (px + .5f - cell / 2.f) / (cell / 2.f), v = (py + .5f - cell / 2.f) / (cell / 2.f);
                const float d =
                    (id < pictochat::ui_icon_count ? pictochat::icon_distance(id, u, v)
                                                   : pictochat::stamp_distance(id - pictochat::ui_icon_count, u, v)) *
                    (cell / 2.f);
                pixels[size_t(py) * width + left + px] =
                    pictochat::pack(255, 255, 255, unsigned(std::clamp((.5f - d) * 255.f, 0.f, 255.f)));
            }
    }
    return texture = chat_upload(r, pixels, width, chat_stamp_cell, true);
}

void App::chat_icon(SDL_Renderer* r, int id, int cx, int cy, Color ink) {
    auto* texture = chat_sheet(r);
    if (!texture) return;
    const int cell = chat_cell(id);
    s.SetTextureColorMod(texture, ink.r, ink.g, ink.b);
    SDL_Rect source{chat_sheet_left(id), 0, cell, cell}, target{cx - cell / 2, cy - cell / 2, cell, cell};
    s.RenderCopy(r, texture, &source, &target);
}

bool App::chat_down(const pictochat::Box& b) const {
    return pointer_down && b.has(contact_x, contact_y) && b.has(down_x, down_y);
}

void App::chat_brackets(SDL_Renderer* r, int x, int y, int w, int h, Color c, int grow) {
    const int t = 3, l = 16;
    x -= grow;
    y -= grow;
    w += grow * 2;
    h += grow * 2;
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++) {
            const int px = i ? x + w - l : x, py = j ? y + h - t : y, qx = i ? x + w - t : x, qy = j ? y + h - l : y;
            tint_rect(r, px, py, l, t, c);
            tint_rect(r, qx, qy, t, l, c);
        }
}

void App::chat_key_face(SDL_Renderer* r, const SDL_Rect& b, int radius, bool lit, bool down, bool enabled) {
    const int sink = down ? 2 : 0;
    round_rect(r, b.x, b.y + 3, b.w, b.h, radius, "slot");
    if (lit) fill_round(r, b.x, b.y + sink, b.w, b.h, radius, chat_tint());
    else round_rect(r, b.x, b.y + sink, b.w, b.h, radius, down ? "focus" : enabled ? "panel" : "slot");
}

void App::chat_button(SDL_Renderer* r, int y, const pictochat::Box& box, bool lit, bool enabled, int icon,
                      const std::string& text, Color* ink_out) {
    const bool down = enabled && chat_down(box);
    SDL_Rect b{box.x, y + box.y, box.w, box.h};
    chat_key_face(r, b, 10, lit && enabled, down, enabled);
    const Color ink = !enabled ? theme.at("line") : lit ? tile_ink(chat_tint()) : theme.at("ink");
    const int sink = down ? 2 : 0;
    if (ink_out) *ink_out = ink;
    if (icon >= 0) chat_icon(r, icon, b.x + b.w / 2, b.y + (text.empty() ? b.h / 2 : b.h / 2 - 9) + sink, ink);
    if (!text.empty())
        label_colour(r, text, b.x + (b.w - label_width(text, 14)) / 2, b.y + b.h - 22 + sink, 14, ink, b.w - 2);
}

void App::chat_footer(SDL_Renderer* r, int y, const std::string& left, const std::string& right,
                      const std::string& middle) {
    rect(r, 0, y + 424, 640, 56, "background");
    rect(r, 24, y + 424, 592, 1, "line");
    footer_button(r, 12, y + 432, 160, left, "B", false, footer_pressed(true));
    if (!middle.empty()) label(r, middle, 184, y + 446, 14, "muted", 276);
    if (!right.empty()) {
        Color tint = chat_tint();
        footer_button(r, 468, y + 432, 160, right, "A", true, footer_pressed(false), &tint);
    }
}

void App::chat_enter_naming() {
    edit_field = 6;
    edit_buffer = chat.name;
    edit_cursor = edit_buffer.size();
    key_layer = 0;
    key_caps = false;
    key_accents = false;
    key_stamps = false;
    key_row = 1;
    key_col = 1;
    key_focus_visible = false;
    chat.view = pictochat::View::Naming;
    emit_sound(MenuCue::Open);
    dirty = true;
}

void App::chat_finish_name(bool keep) {
    if (keep) {
        std::string text = edit_buffer;
        while (!text.empty() && text.back() == ' ') text.pop_back();
        if (!text.empty()) {
            chat.name = text;
            chat_save_profile();
        }
    }
    edit_buffer.clear();
    edit_field = 0;
    chat.view = pictochat::View::Lobby;
    dirty = true;
}

void App::chat_set_colour(int chip) {
    if (chip == 0) {
        chat.colour_auto = true;
        chat.colour = chat_accent_index();
    } else {
        chat.colour_auto = false;
        chat.colour = chip - 1;
    }
    chat_save_profile();
    emit_sound(MenuCue::Navigate);
    dirty = true;
}

void App::chat_join(int room) {
    chat.join(room);
    chat_scroll = 0;
    chat_slid_serial = chat.serial;
    key_layer = 0;
    key_caps = false;
    key_accents = key_stamps = false;
    key_focus_visible = false;
    emit_sound(MenuCue::Open);
    dirty = true;
}

void App::chat_leave_room() {
    chat.leave();
    emit_sound(MenuCue::Back);
    dirty = true;
}

void App::chat_lobby_bottom(SDL_Renderer* r, int y) {
    using namespace pictochat;
    const Color tint = chat_tint();
    const Uint32 now = s.GetTicks();
    for (int i = 0; i < 4; i++) {
        const auto box = layout::room(i);
        const bool focus = pointer_down ? chat_down(box) : chat_focus == i;
        const int n = chat.people(i), cy = y + box.y + box.h / 2;
        alpha_round(r, box.x, y + box.y + 3, box.w, box.h, 14, shadow_colour(), preferences.dark ? 70 : 22);
        if (focus) fill_round(r, box.x, y + box.y, box.w, box.h, 14, mix_colour(theme.at("panel"), tint, .16));
        else round_rect(r, box.x, y + box.y, box.w, box.h, 14, "panel");
        // The floor plan on its own tile, centred; the name beside it.
        round_rect(r, box.x + 12, y + box.y + 10, 56, 56, 12, focus ? "panel" : "slot");
        chat_icon(r, 9 + i, box.x + 40, cy, focus ? tint : theme.at("muted"));
        const auto colours = chat.colours_in(i);
        const std::string title = locale.text("Chat Room") + " " + std::string(1, char('A' + i));
        raw_label(r, title, box.x + 84, colours.empty() ? centred_label_y(title, 24, cy) : y + box.y + 9, 24, "ink",
                  300);
        int dot = 0;
        for (int colour : colours) {
            fill_round(r, box.x + 84 + dot * 18, y + box.y + 47, 13, 13, 6, ds_colours[size_t(colour)].rgb);
            if (++dot >= 14) break;
        }
        // The lighter block: how many are in, and whether the room is reachable.
        {
            const int bx = box.x + box.w - 200, bw = 188;
            round_rect(r, bx, y + box.y + 12, bw, 52, 12, focus ? "panel" : "focus");
            chat_icon(r, 7, bx + 30, cy, theme.at("muted"));
            const std::string heads = std::to_string(n) + "/16";
            raw_label(r, heads, bx + 52, centred_label_y(heads, 24, cy), 24, n >= 16 ? "muted" : "ink", 80);
            chat_icon(r, 8, bx + bw - 30, cy, chat.online ? tint : theme.at("line"));
        }
        if (focus) {
            const int grow = preferences.animations ? 2 + int(std::lround(2 * std::sin(now * .006))) : 3;
            chat_brackets(r, box.x, y + box.y, box.w, box.h, tint, grow);
        }
    }
    // Your profile.
    const auto f = layout::profile;
    const bool pressed = chat_down(f), focus = !pointer_down && chat_focus == 4;
    alpha_round(r, f.x, y + f.y + 3, f.w, f.h, 14, shadow_colour(), preferences.dark ? 70 : 22);
    if (focus) fill_round(r, f.x, y + f.y, f.w, f.h, 14, mix_colour(theme.at("panel"), tint, .16));
    else round_rect(r, f.x, y + f.y + (pressed ? 2 : 0), f.w, f.h, 14, pressed ? "focus" : "panel");
    fill_round(r, f.x + 18, y + f.y + 15, 26, 26, 13, tint);
    raw_label(r, chat.name, f.x + 58, y + f.y + 14, 26, "ink", 360);
    label(r, "Edit", f.x + f.w - 96, y + f.y + 18, 18, "muted", 60);
    ui_icon(r, SettingsIcon::Chevron, f.x + f.w - 26, y + f.y + f.h / 2, 18, "muted");
    if (focus)
        chat_brackets(r, f.x, y + f.y, f.w, f.h, tint,
                      preferences.animations ? 2 + int(std::lround(2 * std::sin(now * .006))) : 3);
    chat_footer(r, y, "Leave", "Join", "");
}

void App::chat_naming_bottom(SDL_Renderer* r, int y) {
    using namespace pictochat;
    const Color tint = chat_tint();
    label(r, "Your name", 24, y + 8, 16, "muted", 300);
    const auto f = layout::field;
    fill_round(r, f.x, y + f.y, f.w, f.h, 12, tint);
    round_rect(r, f.x + 2, y + f.y + 2, f.w - 4, f.h - 4, 10, "panel");
    const auto text = edit_buffer;
    size_t cursor = edit_cursor;
    raw_label(r, text.empty() ? locale.text("Type your name") : text, 38, y + f.y + 13, 26,
              text.empty() ? "muted" : "ink", 520);
    if (!text.empty()) tint_rect(r, 38 + label_width(text.substr(0, cursor), 26, false) + 2, y + f.y + 13, 2, 30, tint);
    if (!text.empty()) {
        round_rect(r, 568, y + f.y + 9, 40, 38, 10, "slot");
        label(r, "X", 582, y + f.y + 18, 18, "muted", 24);
    }
    label(r, "Your colour", 24, y + 94, 16, "muted", 260);
    label(r, chat.colour_auto ? "Matches your accent" : ds_colours[size_t(chat.colour)].name, 200, y + 94, 16, "muted",
          300);
    for (int i = 0; i < 17; i++) {
        const auto b = layout::chip(i);
        const bool picked = i == 0 ? chat.colour_auto : !chat.colour_auto && chat.colour == i - 1;
        if (picked) round_rect(r, b.x - 3, y + b.y - 3, b.w + 6, b.h + 6, 11, "ink");
        round_rect(r, b.x - 1, y + b.y - 1, b.w + 2, b.h + 2, 9, "line");
        if (i == 0) {
            fill_round(r, b.x, y + b.y, b.w, b.h, 8, theme.at("accent"));
            ui_icon(r, SettingsIcon::Spark, b.x + b.w / 2, y + b.y + b.h / 2, 18, "on_accent");
        } else fill_round(r, b.x, y + b.y, b.w, b.h, 8, ds_colours[size_t(i - 1)].rgb);
    }
    draw_keyboard(r, y, keyboard_rows(), key_focus_visible);
    chat_footer(r, y, "Cancel", "Done", "");
}

void App::chat_lobby_top(SDL_Renderer* r, int y) {
    using namespace pictochat;
    const Color tint = chat_tint();
    category_icon(r, pictochat_tile, 272, y + 66, 96);
    centered(r, "PictoChat", y + 174, 2, "ink");
    centered(r,
             !chat.status.empty()        ? chat.status
             : chat.view == View::Naming ? "Type your name"
                                         : "Choose a chat room to join",
             y + 218, 1, chat.status.empty() ? "muted" : "accent", 72);
    round_rect(r, 100, y + 258, 440, 76, 16, "panel");
    fill_round(r, 122, y + 282, 28, 28, 14, tint);
    raw_label(r, chat.view == View::Naming ? edit_buffer : chat.name, 164, y + 266, 26, "ink", 360);
    label(r, "You appear like this to others", 164, y + 302, 14, "muted", 360);
    const bool up = chat.helper_up, on = chat.online;
    const std::string state = up                      ? (on ? "Wi-Fi connected" : "No Wi-Fi: you can still draw")
                              : chat.helper.running() ? "Starting…"
                                                      : "Chat is unavailable: you can still draw";
    const int w = label_width(state, 18) + 60, left = 320 - w / 2;
    round_rect(r, left, y + 356, w, 38, 19, on ? "accent" : "slot");
    chat_icon(r, 8, left + 26, y + 375, on ? theme.at("on_accent") : theme.at("muted"));
    label_colour(r, state, left + 46, y + 364, 18, on ? theme.at("on_accent") : theme.at("muted"), w - 54);
    if (on) {
        const int n = int(chat.users.size());
        centered(r,
                 n == 0   ? "Nobody else is nearby yet"
                 : n == 1 ? "1 person nearby"
                          : std::to_string(n) + " people nearby",
                 y + 414, 1, "muted", 72);
    }
}

std::pair<int, int> App::chat_pad_point(int x, int y) const {
    const auto p = pictochat::layout::pad;
    return {p.x + int(std::lround((x - p.x) / pictochat::layout::pad_scale)),
            p.y + int(std::lround((y - p.y) / pictochat::layout::pad_scale))};
}

void App::chat_room_bottom(SDL_Renderer* r, int y) {
    using namespace pictochat;
    const Color tint = chat_tint(), on_tint = tile_ink(tint);
    const auto p = layout::pad;
    // Your name tab on the frame, then the ruled paper (scaled up from the message canvas).
    const std::string you = chat.name;
    const int tab = std::clamp(label_width(you, 16, false) + 32, 80, 320);
    alpha_round(r, p.x - 5, y + p.y - 1, p.w + 10, p.h + 10, 12, shadow_colour(), preferences.dark ? 70 : 24);
    fill_round(r, p.x - 5, y + p.y - 5, p.w + 10, p.h + 10, 12, tint);
    if (auto* paper = chat_pad_texture(r)) {
        SDL_Rect target{p.x, y + p.y, p.w, p.h};
        s.RenderCopy(r, paper, nullptr, &target);
    }
    fill_round(r, p.x - 5, y + p.y - 25, tab, 26, 8, tint);
    raw_label_colour(r, you, p.x + 9, centred_label_y(you, 16, y + p.y - 13), 16, on_tint, tab - 22);
    if (!chat.drawing) {
        const int cx = p.x + int(chat.caret_x * layout::pad_scale),
                  top = p.y + int((Chat::text_y(chat.caret_line) - 2) * layout::pad_scale);
        tint_rect(r, cx, y + top, 2, int(28 * layout::pad_scale), tint);
    }
    // Under the pad, as on the DS: who else is here as coloured name chips, or the pad's own notice.
    {
        const auto n = layout::names;
        const int cy = y + n.y + n.h / 2;
        if (!chat.status.empty())
            raw_label(r, chat.status, n.x, centred_label_y(chat.status, 14, cy), 14, "accent", n.w);
        else {
            const int count = chat.people(chat.room);
            const std::string heads = std::to_string(count) + "/16";
            const int heads_w = label_width(heads, 16, false) + 34;
            int x = n.x;
            for (const auto& u : chat.users) {
                if (u.second.room != chat.room) continue;
                const Color c = ds_colours[size_t(u.second.colour)].rgb;
                const int w = std::min(150, label_width(u.second.name, 14, false) + 22);
                if (x + w > n.x + n.w - heads_w - 8) break;
                fill_round(r, x, y + n.y, w, n.h, 11, c);
                raw_label_colour(r, u.second.name, x + 11, centred_label_y(u.second.name, 14, cy), 14, tile_ink(c),
                                 w - 20);
                x += w + 6;
            }
            if (x == n.x) {
                const std::string alone = locale.text(chat.online ? "Nobody else here yet" : "Drawing on your own");
                raw_label(r, alone, n.x, centred_label_y(alone, 14, cy), 14, "muted", 400);
            }
            chat_icon(r, 7, n.x + n.w - heads_w + 10, cy, theme.at("muted"));
            raw_label(r, heads, n.x + n.w - heads_w + 26, centred_label_y(heads, 16, cy), 16, "ink", heads_w);
        }
    }
    // The tool column on the left: pen, eraser and the two sizes; undo and clear end the ink row.
    chat_button(r, y, layout::tool(0), !chat.eraser && !chat.stamp_armed, true, 0);
    chat_button(r, y, layout::tool(1), chat.eraser, true, 1);
    for (int i = 0; i < 2; i++) {
        const auto box = layout::tool(2 + i);
        const bool on = chat.thick == (i == 1);
        chat_button(r, y, box, on, true, -1);
        const int size = chat.eraser ? (i ? 18 : 10) : (i ? 13 : 6);
        const Color ink = on ? on_tint : theme.at("ink");
        fill_round(r, box.x + box.w / 2 - size / 2, y + box.y + box.h / 2 - size / 2, size, size, size / 2, ink);
    }
    chat_button(r, y, layout::undo, false, chat.inked(), 3);
    chat_button(r, y, layout::clear, false, chat.inked(), 2);
    // Inks.
    for (int i = 0; i <= rainbow; i++) {
        const auto b = layout::swatch(i);
        const bool picked = chat.ink == i && !chat.eraser;
        if (picked) round_rect(r, b.x - 3, y + b.y - 3, b.w + 6, b.h + 6, 11, "ink");
        round_rect(r, b.x - 1, y + b.y - 1, b.w + 2, b.h + 2, 9,
                   "line"); // keeps the black ink visible on dark themes
        if (i < rainbow) fill_round(r, b.x, y + b.y, b.w, b.h, 8, chat_ink(i));
        else {
            for (int x = 0; x < b.w; x++) tint_rect(r, b.x + x, y + b.y + 3, 1, b.h - 6, chat_hue(x * 11.f));
            round_rect(r, b.x, y + b.y, b.w, 3, 2, "panel");
            round_rect(r, b.x, y + b.y + b.h - 3, b.w, 3, 2, "panel");
        }
    }
    // Beside the keyboard, as the DS keeps them: Send, the scroll arrows and Leave.
    chat_button(r, y, layout::send, true, chat.inked(), 4, "Send");
    const bool older = chat.scrolled + 1 < int(chat.log.size()), newer = chat.scrolled > 0;
    chat_button(r, y, layout::scroll_up, false, older, 5);
    chat_button(r, y, layout::scroll_down, false, newer, 6);
    if (chat.unread)
        fill_round(r, layout::scroll_down.x + layout::scroll_down.w - 12, y + layout::scroll_down.y + 4, 8, 8, 4, tint);
    {
        const auto b = layout::leave;
        const bool down = chat_down(b);
        chat_key_face(r, {b.x, y + b.y, b.w, b.h}, 10, false, down);
        ui_icon(r, SettingsIcon::Back, b.x + b.w / 2, y + b.y + 17 + (down ? 2 : 0), 20, "accent");
        const std::string text = locale.text("Leave");
        raw_label(r, text, b.x + (b.w - label_width(text, 12, false)) / 2, y + b.y + b.h - 18 + (down ? 2 : 0), 12,
                  "muted", b.w);
    }
    draw_keyboard(r, y, keyboard_rows(), key_focus_visible);
}

void App::chat_bottom(SDL_Renderer* r, int y) {
    rect(r, 0, y, 640, 480, "background");
    pattern(r, y, 480, true);
    if (chat.view == pictochat::View::Lobby) {
        chat_lobby_bottom(r, y);
        return;
    }
    if (chat.view == pictochat::View::Naming) {
        chat_naming_bottom(r, y);
        return;
    }
    chat_room_bottom(r, y);
}

std::string App::top_signature(Uint32 now) {
    if (settings.page != Page::Library || chat_open || o.game_menu || probe || launching ||
        embedded_menu || (o.boot_startup && !library_ready) || now - top_drawn_at >= 2000)
        return {};
    std::ostringstream k;
    const auto accent = theme.at("accent"), background = theme.at("background");
    k << home_menu << '|' << home_selected << '|' << model.selected << '|' << model.games.size() << '|' << library_ready
      << collection_loading << summary_ready << summary.generation << picker_open << discovery_active() << '|'
      << status_stamp() << '|' << hardware.battery << hardware.charging
      << (hardware.charging ? int((now / 420) % 4) : -1) << hardware.external_power << hardware.volume
      << hardware.volume_percent << '|' << wifi_asset() << '|' << artwork_busy()
      << (artwork_busy() ? loader_phase(now) : 0) << '|' << preferences.dark << preferences.pixel
      << preferences.rich_icons << preferences.accent << preferences.high_contrast << preferences.background
      << preferences.home_layout << preferences.view << preferences.layout << preferences.language << '|'
      << int(accent.r) << ',' << int(accent.g) << ',' << int(accent.b) << ',' << int(background.r) << ','
      << int(background.g) << ',' << int(background.b) << '|' << status << '|' << return_writes_pending()
      << boot_hardware_ready << system.volume_pending() << '|' << update_status["state"] << update_status["prepared"]
      << update_status["offer"] << update_status["progress"] << int(update_stage()) << banner_frame(now) << '|'
      << device_status["wifi_connected"] << '|' << (volume_toast && now < volume_toast ? now : 0) << '|'
      << (preferences.animations && cover_motion_at && now - cover_motion_at < 110 ? now : 0) << '|'
      << (home_menu && preferences.home_layout == 1 ? std::time(nullptr) : 0);
    if (!home_menu && !model.games.empty() && !surfaces.empty()) {
        const auto& game = model.games[model.selected];
        auto item = item_folder(model.selected);
        auto* r = surfaces.front().renderer;
        k << '|' << game_title(model.selected) << '|' << library_cache.metadata(game).info["developer"] << '|'
          << media.ready(r, item / "box.bmp") << media.ready(r, game.path, true) << media.ready(r, item / "icon.bmp")
          << '|';
        const auto& stats = library_cache.stats(game);
        k << stats.seconds << ',' << stats.sessions;
    }
    if (home_menu && !surfaces.empty()) {
        const auto& last = summary.groups[0].last_game;
        if (last) k << '|' << media.ready(surfaces.front().renderer, last->path, true);
    }
    return k.str();
}

std::string App::chat_top_signature() const {
    std::ostringstream key;
    const auto accent = theme.at("accent");
    key << int(chat.view) << '|' << chat.serial << '|' << chat.users.size() << '|' << chat.online << chat.helper_up
        << chat.helper.running() << '|' << chat.scrolled << '|' << int(chat_scroll * 2) << chat.unread << '|'
        << chat.status << '|' << chat.name << '|' << edit_buffer << '|' << chat.colour << chat.colour_auto
        << int(accent.r) << ',' << int(accent.g) << ',' << int(accent.b) << preferences.dark << preferences.pixel
        << preferences.rich_icons << '|' << chat.room << '|' << (volume_toast && s.GetTicks() < volume_toast) << '|'
        << s.GetTicks() / 15000;
    return key.str();
}

double App::chat_scroll_goal() const {
    double px = 0;
    int n = 0;
    for (auto it = chat.log.rbegin(); it != chat.log.rend() && n < chat.scrolled; ++it, ++n)
        px += chat_entry_height(*it);
    return px;
}

double App::chat_content_height() const {
    double px = 0;
    for (const auto& e : chat.log) px += chat_entry_height(e);
    return px;
}

void App::chat_top(SDL_Renderer* r, int y) {
    using namespace pictochat;
    rect(r, 0, y, 640, 480, "background");
    pattern(r, y + 48, 432);
    status_bar(r, y);
    if (chat.view != View::Room) {
        chat_lobby_top(r, y);
        return;
    }
    const Color tint = chat_tint(), on_tint = tile_ink(tint);
    // Header: the room's letter badge and name on the left; who is here and the signal on the right.
    {
        const std::string letter(1, char('A' + chat.room));
        fill_round(r, 24, y + 56, 40, 40, 11, tint);
        alpha_round(r, 28, y + 59, 32, 12, 5, Color{255, 255, 255}, 50);
        raw_label_colour(r, letter, 44 - label_width(letter, 24, false) / 2, centred_label_y(letter, 24, y + 76), 24,
                         on_tint, 30);
    }
    {
        const std::string title = locale.text("Chat Room") + " " + std::string(1, char('A' + chat.room));
        raw_label(r, title, 76, centred_label_y(title, 24, y + 76), 24, "ink", 320);
    }
    const std::string count = std::to_string(chat.people(chat.room)) + "/16";
    chat_icon(r, 8, 610, y + 76, chat.online ? tint : theme.at("line"));
    raw_label(r, count, 586 - label_width(count, 22, false), centred_label_y(count, 22, y + 76), 22, "ink", 70);
    chat_icon(r, 7, 566 - label_width(count, 22, false), y + 76, theme.at("muted"));
    // The rail on the left: an arrow above and below a recessed track and a glossy thumb in the pad's colour.
    constexpr int track_top = 130, track_height = 256, view_top = 104, view_height = 376;
    const bool more = chat.scrolled + 1 < int(chat.log.size()), less = chat.scrolled > 0;
    for (int i = 0; i < 2; i++) {
        const bool on = i ? less : more;
        const int by = i ? y + track_top + track_height + 8 : y + track_top - 38;
        if (on) raised_pill(r, 16, by, 28, 26, 8, theme.at("panel"));
        else round_rect(r, 16, by, 28, 26, 8, "slot");
        chat_icon(r, 5 + i, 30, by + 13, on ? theme.at("ink") : theme.at("line"));
    }
    sunken_track(r, 22, y + track_top, 16, track_height, 8);
    const double total = std::max<double>(chat_content_height(), view_height), span = total - view_height;
    const int thumb =
        span <= 0 ? track_height - 4 : std::clamp(int(track_height * view_height / total), 34, track_height - 4);
    const double offset = span <= 0 ? 0 : std::clamp(chat_scroll, 0.0, span) / span; // 0 at the newest message
    {
        const int ty = y + track_top + 2 + int(std::lround((track_height - 4 - thumb) * (1 - offset)));
        alpha_round(r, 24, ty + 2, 12, thumb, 6, shadow_colour(), preferences.dark ? 60 : 24);
        glossy_pill(r, 24, ty, 12, thumb, 6, span <= 0 ? mix_colour(theme.at("slot"), tint, .35) : tint);
    }
    {
        const auto colours = chat.colours_in(chat.room);
        for (size_t i = 0; i < colours.size() && i < 6; i++) {
            fill_round(r, 16 + int(i % 2) * 16, y + 428 + int(i / 2) * 16, 12, 12, 3,
                       ds_colours[size_t(colours[i])].rgb);
        }
    }
    chat_top_seen = chat_top_signature();
    // Drop pictures of messages that have left the log.
    {
        std::set<uint32_t> keep;
        for (const auto& e : chat.log) keep.insert(e.serial);
        for (auto it = chat_cards.begin(); it != chat_cards.end();) {
            if (!keep.count(it->first.second)) {
                if (it->second) s.DestroyTexture(it->second);
                it = chat_cards.erase(it);
            } else ++it;
        }
    }
    // The log: newest at the bottom, slid by the eased scroll, clipped to its window.
    SDL_Rect window{56, y + view_top, 584, view_height};
    s.RenderSetClipRect(r, &window);
    const int card_w = int(pictochat::canvas_w * chat_log_scale), card_h = int(pictochat::canvas_h * chat_log_scale),
              card_x = 70;
    double cursor = view_top + view_height + chat_scroll;
    for (int i = int(chat.log.size()) - 1; i >= 0 && cursor > view_top - 130; i--) {
        const auto& e = chat.log[size_t(i)];
        const int h = chat_entry_height(e);
        cursor -= h;
        const int top = int(std::lround(cursor));
        if (top > 480) continue;
        if (!e.notice.empty()) {
            const int w = label_width(e.notice, 16) + 28;
            round_rect(r, 348 - w / 2, y + top + 4, w, 24, 12, "slot");
            label(r, e.notice, 348 - w / 2 + 14, centred_label_y(locale.text(e.notice), 16, y + top + 16), 16, "muted",
                  w);
            continue;
        }
        // A card in the sender's colour with the name on a tab, as on the DS.
        const Color colour = e.mine && chat.colour_auto ? chat_tint() : ds_colours[size_t(e.colour)].rgb,
                    ink = tile_ink(colour);
        const std::string name = e.mine ? locale.text("You") : e.name;
        const int tab = std::clamp(label_width(name, 16, false) + 32, 80, 330);
        alpha_round(r, card_x, y + top + 25, card_w + 8, card_h + 8, 10, shadow_colour(), preferences.dark ? 60 : 18);
        fill_round(r, card_x, y + top + 22, card_w + 8, card_h + 8, 10, colour);
        if (auto* card = chat_card(r, e)) {
            SDL_Rect target{card_x + 4, y + top + 26, card_w, card_h};
            s.RenderCopy(r, card, nullptr, &target);
        }
        fill_round(r, card_x, y + top + 2, tab, 26, 9, colour);
        raw_label_colour(r, name, card_x + 14, centred_label_y(name, 16, y + top + 14), 16, ink, tab - 24);
    }
    s.RenderSetClipRect(r, nullptr);
    if (chat.scrolled > 0) {
        const std::string text = locale.text(chat.unread ? "New messages below" : "Older messages");
        const int w = label_width(text, 16, false) + 32;
        alpha_round(r, 348 - w / 2, y + 447, w, 28, 14, shadow_colour(), preferences.dark ? 60 : 20);
        round_rect(r, 348 - w / 2, y + 444, w, 28, 14, chat.unread ? "accent" : "panel");
        raw_label_colour(r, text, 348 - w / 2 + 16, centred_label_y(text, 16, y + 458), 16,
                         chat.unread ? theme.at("on_accent") : theme.at("muted"), w);
    }
}

void App::chat_type(const std::string& piece) {
    if (chat.view == pictochat::View::Naming) {
        insert_text(piece);
        dirty = true;
        return;
    }
    if (chat.view != pictochat::View::Room) return;
    for (size_t i = 0; i < piece.size();) {
        size_t n = 1;
        while (i + n < piece.size() && (static_cast<unsigned char>(piece[i + n]) & 0xc0) == 0x80) n++;
        chat.type(piece.substr(i, n));
        i += n;
    }
    dirty = true;
}

void App::chat_send() {
    if (chat.send(s.GetTicks() / 1000.0)) emit_sound(MenuCue::Confirm);
    else if (!chat.status.empty()) emit_sound(MenuCue::Error);
    dirty = true;
}

bool App::chat_event(const SDL_Event& e) {
    if (e.type == SDL_TEXTINPUT) {
        if (chat.view != pictochat::View::Lobby) chat_type(e.text.text);
        return true;
    }
    if (e.type == SDL_JOYBUTTONDOWN && chat.view == pictochat::View::Room &&
        (joystick_source(e.jbutton.button) == 12 || joystick_source(e.jbutton.button) == 13)) {
        chat_action(joystick_source(e.jbutton.button) == 12 ? 15 : 16);
        return true; // L2 / R2: previous / next ink
    }
    // The d-pad and A drive the keyboard, so the shoulder buttons scroll the log: a tap steps, holding repeats.
    if ((e.type == SDL_JOYBUTTONDOWN || e.type == SDL_JOYBUTTONUP) &&
        (e.jbutton.button == 4 || e.jbutton.button == 5)) {
        if (e.type == SDL_JOYBUTTONUP) {
            chat_shoulder = 0;
            return false;
        }
        if (chat.view != pictochat::View::Room) return false;
        last_activity = s.GetTicks();
        chat_shoulder = e.jbutton.button == 4 ? 1 : -1;
        chat_shoulder_at = chat_repeat_at = s.GetTicks();
        chat_scroll_step(chat_shoulder);
        return true;
    }
    if (e.type == SDL_KEYDOWN) {
        const auto key = e.key.keysym.sym;
        if (chat.view == pictochat::View::Lobby) return false;
        if (key == SDLK_BACKSPACE) {
            if (chat.view == pictochat::View::Naming) erase_character();
            else chat.backspace();
            dirty = true;
            return true;
        }
        if (key == SDLK_DELETE && chat.view == pictochat::View::Naming) {
            delete_character();
            return true;
        }
        if (key == SDLK_LEFT && chat.view == pictochat::View::Naming) {
            move_edit_cursor(-1);
            dirty = true;
            return true;
        }
        if (key == SDLK_RIGHT && chat.view == pictochat::View::Naming) {
            move_edit_cursor(1);
            dirty = true;
            return true;
        }
        if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
            if (chat.view == pictochat::View::Naming) chat_finish_name(true);
            else {
                chat.type("\n");
                dirty = true;
            }
            return true;
        }
        if (key == SDLK_ESCAPE) {
            chat_action(3);
            return true;
        }
        if ((key == SDLK_PAGEUP || key == SDLK_PAGEDOWN) && chat.view == pictochat::View::Room) {
            chat_scroll_step(key == SDLK_PAGEUP ? 1 : -1);
            return true;
        }
        if (key >= 32 && key < 127) return true; // typed letters arrive as text, not as buttons
    }
    return false;
}

void App::chat_press(int x, int y) {
    using namespace pictochat;
    last_activity = s.GetTicks();
    dirty = true;
    chat_repeat_at = s.GetTicks();
    if (chat.view == View::Lobby) {
        for (int i = 0; i < 4; i++)
            if (layout::room(i).has(x, y)) {
                chat_focus = i;
                key_focus_visible = false;
                chat_join(i);
                return;
            }
        if (layout::profile.has(x, y)) {
            chat_focus = 4;
            chat_enter_naming();
            return;
        }
        if (x >= 12 && x < 172 && y >= 432 && y < 472) close_pictochat();
        else if (x >= 468 && x < 628 && y >= 432 && y < 472) {
            if (chat_focus == 4) chat_enter_naming();
            else chat_join(chat_focus);
        }
        return;
    }
    if (chat.view == View::Naming) {
        if (x >= 568 && x < 608 && y >= layout::field.y + 9 && y < layout::field.y + 47 && !edit_buffer.empty()) {
            edit_buffer.clear();
            edit_cursor = 0;
            return;
        }
        for (int i = 0; i < 17; i++)
            if (layout::chip(i).has(x, y)) {
                chat_set_colour(i);
                return;
            }
        if (x >= 468 && x < 628 && y >= 432 && y < 472) {
            chat_finish_name(true);
            return;
        }
        if (x >= 12 && x < 172 && y >= 432 && y < 472) {
            chat_finish_name(false);
            return;
        }
        keyboard_touch(x, y);
        return;
    }
    // In a room.
    if (layout::leave.has(x, y)) {
        chat_leave_room();
        return;
    }
    if (keyboard_touch(x, y)) return;
    if (layout::pad.has(x, y)) {
        key_focus_visible = false;
        auto at = chat_pad_point(x, y);
        chat.begin_stroke(at.first, at.second);
        return;
    }
    if (layout::tool(0).has(x, y)) {
        chat.eraser = false;
        chat.stamp_armed = false;
        emit_sound(MenuCue::Navigate);
        return;
    }
    if (layout::tool(1).has(x, y)) {
        chat.eraser = true;
        chat.stamp_armed = false;
        emit_sound(MenuCue::Navigate);
        return;
    }
    if (layout::tool(2).has(x, y)) {
        chat.thick = false;
        return;
    }
    if (layout::tool(3).has(x, y)) {
        chat.thick = true;
        return;
    }
    if (layout::send.has(x, y)) {
        chat_send();
        return;
    }
    if (layout::undo.has(x, y)) {
        chat.undo();
        return;
    }
    if (layout::clear.has(x, y)) {
        chat.clear_pad();
        return;
    }
    for (int i = 0; i <= rainbow; i++)
        if (layout::swatch(i).has(x, y)) {
            chat.ink = i;
            chat.eraser = false;
            chat.stamp_armed = false;
            emit_sound(MenuCue::Navigate);
            return;
        }
    if (layout::scroll_up.has(x, y)) {
        chat_scroll_step(1);
        return;
    }
    if (layout::scroll_down.has(x, y)) {
        chat_scroll_step(-1);
        return;
    }
}

void App::chat_scroll_step(int direction) {
    const int before = chat.scrolled;
    chat.scrolled = std::clamp(chat.scrolled + direction, 0, std::max(0, int(chat.log.size()) - 1));
    if (chat.scrolled == 0) chat.unread = false;
    if (before != chat.scrolled) emit_sound(MenuCue::Navigate);
    dirty = true;
}

void App::chat_drag(int x, int y) {
    if (chat.drawing) {
        auto at = chat_pad_point(x, y);
        chat.extend_stroke(at.first, at.second);
        dirty = true;
    }
}

void App::chat_release() {
    if (chat.drawing) {
        chat.end_stroke();
        dirty = true;
    }
}

void App::chat_action(int code) {
    using namespace pictochat;
    last_activity = s.GetTicks();
    dirty = true;
    if (chat.view == View::Naming) {
        if (code == 3 || code == 7) {
            chat_finish_name(false);
            return;
        }
        if (code == 4) {
            chat_finish_name(true);
            return;
        }
        if (code == 2) {
            key_focus_visible = true;
            keyboard_choose();
            return;
        }
        if (code == 11) {
            erase_character();
            return;
        }
        if (code == 12) {
            key_layer = key_layer == 0 ? 1 : 0;
            return;
        }
        keyboard_move(code);
        return;
    }
    if (chat.view == View::Lobby) {
        if (code == 3 || code == 7) {
            close_pictochat();
            return;
        }
        key_focus_visible = true;
        if (code == 8) chat_focus = std::max(0, chat_focus - 1);
        else if (code == 9) chat_focus = std::min(4, chat_focus + 1);
        else if (code == 2 || code == 4) (chat_focus == 4 ? chat_enter_naming() : chat_join(chat_focus));
        else if (code == 11) chat_enter_naming();
        return;
    }
    switch (code) {
    case 3:
    case 7: chat_leave_room(); break; // B, or the Anbernic button
    case 4: chat_send(); break;       // Start
    case 2:
        key_focus_visible = true;
        keyboard_choose();
        break; // A presses the key under the focus
    case 0:
    case 1:
    case 8:
    case 9: keyboard_move(code); break;
    case 11: chat.undo(); break;
    case 12:
        chat.eraser = !chat.eraser;
        chat.stamp_armed = false;
        break;
    case 13: chat_scroll_step(1); break;
    case 14: chat_scroll_step(-1); break;
    case 15:
        chat.ink = (chat.ink + rainbow) % (rainbow + 1);
        chat.eraser = false;
        break;
    case 16:
        chat.ink = (chat.ink + 1) % (rainbow + 1);
        chat.eraser = false;
        break;
    default: break;
    }
}

void App::poll_chat() {
    if (!chat_open) return;
    chat.poll();
    const Uint32 now = s.GetTicks();
    // A message that arrives slides up from below; while you read older ones the page stays put.
    if (chat.serial != chat_slid_serial) {
        for (auto it = chat.log.rbegin(); it != chat.log.rend() && it->serial > chat_slid_serial; ++it)
            chat_scroll += chat.scrolled > 0 ? chat_entry_height(*it) : -chat_entry_height(*it);
        chat_slid_serial = chat.serial;
    }
    const double dt = std::min(.05, (now - chat_scroll_at) / 1000.0);
    chat_scroll_at = now;
    const double goal = chat_scroll_goal();
    if (!preferences.animations || std::fabs(chat_scroll - goal) < .4) chat_scroll = goal;
    else {
        chat_scroll += (goal - chat_scroll) * (1 - std::exp(-dt * 16));
        if (now - chat_anim_at >= 16) {
            chat_anim_at = now;
            dirty = true;
        }
    }
    if (chat.view == pictochat::View::Room && chat_shoulder && now - chat_shoulder_at > 380 &&
        now - chat_repeat_at > 130) {
        chat_repeat_at = now;
        chat_scroll_step(chat_shoulder);
    }
    // Holding an arrow keeps scrolling.
    if (chat.view == pictochat::View::Room && pointer_down && now - pointer_at > 380 && now - chat_repeat_at > 130) {
        if (chat_down(pictochat::layout::scroll_up)) {
            chat_repeat_at = now;
            chat_scroll_step(1);
        } else if (chat_down(pictochat::layout::scroll_down)) {
            chat_repeat_at = now;
            chat_scroll_step(-1);
        }
    }
    if (chat.view == pictochat::View::Lobby && preferences.animations && now - chat_anim_at >= 33) {
        chat_anim_at = now;
        dirty = true;
    } // the selection brackets breathe, at about 30 frames a second
    if (chat.serial != chat_seen_serial || chat.users.size() != chat_seen_users || chat.online != chat_seen_online ||
        chat.helper_up != chat_seen_up) {
        if (chat.serial != chat_seen_serial && !chat.log.empty() && !chat.log.back().mine &&
            chat.log.back().notice.empty())
            emit_sound(MenuCue::Enable);
        chat_seen_serial = chat.serial;
        chat_seen_users = chat.users.size();
        chat_seen_online = chat.online;
        chat_seen_up = chat.helper_up;
        dirty = true;
    }
}
