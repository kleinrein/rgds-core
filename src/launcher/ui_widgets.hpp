#pragma once
// Shared drawing widgets: text, frames, footers, list rows, toggles, scrollbars, rounded rects and icons.
// Members of App in main.cpp; included once, right after the class.

void App::text(SDL_Renderer* r, std::string value, int x, int y, int size, const std::string& key, int max_chars,
               bool translate) {
    if (translate) value = locale.text(value);
    if (!preferences.pixel && font_ready) {
        rounded.draw(r, value, x, y, point_size(size), theme.at(key), max_chars, std::max(1, 612 - x));
        return;
    }
    color(r, key);
    pixel_dots(
        value, x, y, size, max_chars, [&](const SDL_Rect& p) { s.RenderFillRect(r, &p); },
        [&](unsigned c, int cell_x, int baseline_y) {
            // The rounded glyph, centred in two cells and standing on the dot baseline.
            try {
                const auto glyph = utf8_encode(c);
                const int points = size * 10;
                const int w = static_cast<int>(std::lround(rounded.width(glyph, points)));
                rounded.draw(r, glyph, cell_x + size * 6 - w / 2, baseline_y + size * 7 - rounded.ascent(points),
                             points, theme.at(key), 1, 1000);
            } catch (const std::exception&) {}
        });
}

void App::centered(SDL_Renderer* r, const std::string& source, int y, int size, const std::string& key, int limit) {
    auto value = locale.text(source);
    auto count = std::min(static_cast<int>(RoundedFont::codepoints(value).size()), limit);
    if (!preferences.pixel && font_ready) {
        auto width = std::min(rounded.width(value, point_size(size), limit), 584.0f);
        text(r, value, static_cast<int>(320 - width / 2), y, size, key, limit);
        return;
    }
    while (count > 0 && pixel_cells(value, count) * size * 6 > 584) count--;
    text(r, value, 320 - pixel_cells(value, count) * size * 3, y, size, key, count);
}

void App::label(SDL_Renderer* r, const std::string& source, int x, int y, int pixels, const std::string& key, int width,
                bool translate) {
    auto value = translate ? locale.text(source) : source;
    if (!preferences.pixel && font_ready) {
        rounded.draw(r, value, x, y, pixels, theme.at(key), 1000, width);
        return;
    }
    int scale = pixel_scale(pixels);
    text(r, value, x, y, scale, key, std::min(80, width / (scale * 6)), false);
}

void App::raw_label(SDL_Renderer* r, const std::string& value, int x, int y, int pixels, const std::string& key,
                    int width) {
    label(r, value, x, y, pixels, key, width, false);
}

void App::raw_wrapped(SDL_Renderer* r, const std::string& value, int x, int y, int pixels, int width, int rows) {
    wrapped(r, value, x, y, pixels, width, rows, false);
}

void App::start_frame(SDL_Renderer* r, int x, int row, int tile, const std::string& word, int band) {
    const Color accent = theme.at("accent");
    raised_pill(r, x - 9, row - 9, tile + 18, tile + band + 17, 8, accent);
    fill_round(r, x - 3, row - 3, tile + 6, tile + 8, 6, blend(accent, Color{0, 0, 0}, 30));
    round_rect(r, x - 1, row - 1, tile + 2, tile + 2, 4, "panel");
    const int width = label_width(word, 20), word_y = centred_label_y(locale.text(word), 20, row + tile + 8 + band / 2);
    label_colour(r, word, x + (tile - width) / 2, word_y + 1, 20, blend(accent, Color{0, 0, 0}, 40), tile);
    label(r, word, x + (tile - width) / 2, word_y, 20, "on_accent", tile);
}

std::string App::category_title(size_t index) {
    static const std::array<std::string, home_count> titles = {"Settings", "Games",     "Favourites",
                                                               "Recents",  "PictoChat", "Last played"};
    return titles.at(index);
}

void App::category_icon(SDL_Renderer* r, size_t index, int x, int y, int size) {
    static const std::array<SettingsIcon, home_count> symbols = {SettingsIcon::Sliders, SettingsIcon::Cartridge,
                                                                 SettingsIcon::Heart,   SettingsIcon::Clock,
                                                                 SettingsIcon::Chat,    SettingsIcon::Play};
    ui_icon(r, symbols.at(index), x + size / 2, y + size / 2, size, "accent");
}

void App::settings_footer(SDL_Renderer* r, int y) {
    if (settings.page == Page::ShaderPicker) return;
    rect(r, 0, y + 424, 640, 56, "background");
    rect(r, 24, y + 424, 592, 1, "line");
    const bool pressed = footer_pressed(true), a_pressed = footer_pressed(false);
    footer_button(r, 12, y + 432, 160, "Back", "B", false, pressed);
    if (settings.page == Page::Keyboard) {
        const bool searching = edit_field == 4 || edit_field == 6;
        key_hints(r, 320, y + 452, {{"X", "Delete"}, {"Start", searching ? "Search" : "Done"}}, 0, 16, 280);
        footer_button(r, 468, y + 432, 160, searching ? "Search" : "Done", "", true, a_pressed);
        return;
    }
    if (settings.page == Page::Statistics || settings.page == Page::Microphone) return;
    if (settings.page == Page::GameInfo) {
        if (!picker_open && info_pictures.size() > 1)
            key_hints(r, 320, y + 452, {{"L R", "Picture"}, {"A", "Next picture"}}, 0, 16, 280);
        return;
    }
    if (settings.page == Page::GameSwitcher) key_hints(r, 320, y + 452, {{"X", "Game menu"}}, 0, 16, 280);
    if (settings.page == Page::SaveGallery) {
        if (!picker_open) {
            std::vector<KeyHint> keys{{"Y", "More"}};
            if (slot_occupied(settings.focus)) keys.insert(keys.begin(), {"X", "Delete"});
            key_hints(r, 320, y + 452, keys, 0, 16, 280);
        }
        if (slot_primary_enabled(settings.focus))
            footer_button(r, 468, y + 432, 160, slot_primary_name(), "A", true, a_pressed);
        return;
    }
    if (settings.page == Page::Cheats && !picker_open)
        key_hints(r, 320, y + 452, {{"L R", "Tabs"}, {"X", "Search"}}, 0, 16, 280);
    if (settings.page == Page::Maintenance && !picker_open) {
        const auto stage = update_stage();
        if (stage != UpdateStage::None && stage != UpdateStage::Running)
            hotkey_pill(r, 184, y + 432, 272, 40, update_action_name(stage), KeyLook::Panel,
                        pointer_down && contact_y >= 424 && contact_x >= 200 && contact_x < 440);
    }
    if ((settings.page == Page::EmulatorOptions && !emulator_option_enabled(settings.focus)) ||
        (settings.page == Page::Cheats && cheat_view != CheatView::Groups && !cheat_togglable(settings.focus)) ||
        (settings.page == Page::GameOptions && settings.focus == 2 && superdrastic_game()) ||
        (settings.page == Page::Motion && settings.focus >= 2))
        return;
    footer_button(r, 468, y + 432, 160,
                  settings.page == Page::ShaderPicker ? "Apply"
                  : settings.page == Page::Cheats     ? (cheat_view == CheatView::Groups ? "Open" : "Toggle")
                  : settings.page == Page::Settings   ? "Open"
                  : settings.page == Page::DeviceInfo ? "View"
                  : settings.page == Page::Sound      ? (settings.focus == 2            ? "Open"
                                                         : settings.focus == 1          ? "Test"
                                                         : hardware.volume_percent == 0 ? "Unmute"
                                                                                        : "Mute")
                  : settings.page == Page::Display    ? "Done"
                  : settings.page == Page::Motion     ? "Toggle"
                                                      : "Select",
                  "A", true, a_pressed);
}

void App::ui_icon(SDL_Renderer* r, SettingsIcon symbol, int x, int y, int size, const std::string& key) {
    if (drawing_elements && view_transitions_enabled() && view_motion.active()) {
        double pop = view_motion.accent(uint32_t(symbol), view_drawn_at);
        size = int(std::round(size * (1 + .14 * pop)));
        y -= int(std::round(3 * pop));
    }
    if (!preferences.rich_icons && draw_pixel_icon(s, r, symbol, x, y, size, theme.at(key))) return;
    auto name = icon_asset(symbol);
    if (preferences.rich_icons && name && size >= 20 &&
        media.draw(r, asset_path(std::string("icons/") + name + ".bmp"), {x - size / 2, y - size / 2, size, size},
                   false))
        return;
    draw_settings_icon(s, r, symbol, x, y, size, theme.at(key));
}

void App::settings_card(SDL_Renderer* r, int x, int y, int w, int h, bool focused) {
    round_rect(r, x, y + 3, w, h, 14, "slot");
    round_rect(r, x, y, w, h, 14, focused ? "accent" : "panel");
    if (focused) round_rect(r, x + 2, y + 2, w - 4, h - 4, 12, "focus");
}

void App::list_row(SDL_Renderer* r, int y, int index, const std::string& title, const std::string& value, bool enabled,
                   const Color* swatch, bool switches) {
    int row = y + list_top + index * list_stride - list_scroll;
    bool focused = index == list_selected();
    int dx = 24;
    move_element(uint32_t(index), dx, row, 566, 66, focused, list_top, list_top + list_height, 20, 595);
    dx -= 24;
    settings_card(r, 24 + dx, row, 566, 66, focused);
    round_rect(r, 38 + dx, row + 11, 44, 44, 12, focused && enabled ? "accent" : "slot");
    ui_icon(r,
            settings.page == Page::SaveGallery    ? gallery_icon()
            : settings.page == Page::ShaderPicker ? SettingsIcon::Shader
            : settings.page == Page::Cheats       ? SettingsIcon::Spark
            : settings.page == Page::Timezone     ? SettingsIcon::Clock
                                                  : settings_icon_for(title),
            60 + dx, row + 33, 25, enabled ? (focused ? "on_accent" : "accent") : "muted");
    const bool toggle = switches && (value == "On" || value == "Off");
    const int dot = swatch ? 22 : 0;
    const int value_width = value.empty() ? 0
                            : toggle      ? 82
                                          : std::min(224, label_width(value, 18) + (enabled ? 46 : 28) + dot);
    const int text_width = 464 - (value_width ? value_width + 16 : 28);
    label(r, title, 98 + dx, row + 19, 24, enabled ? "ink" : "muted", text_width,
          settings.page != Page::Wifi && settings.page != Page::Cheats &&
              settings.page != Page::Timezone); // network and cheat names are data
    if (toggle) {
        draw_toggle(r, 524 + dx, row + 19, title, value == "On", enabled);
    } else if (!value.empty()) {
        // Even 14 px margins; text, swatch and chevron on one centre line.
        int x = 574 - value_width + dx;
        round_rect(r, x, row + 18, value_width, 30, 15, focused ? "panel" : "slot");
        if (swatch) accent_swatch(r, x + 22, row + 33, 8, *swatch);
        label(r, value, x + 14 + dot, centred_label_y(locale.text(value), 18, row + 33), 18,
              enabled ? "accent" : "muted", value_width - (enabled ? 46 : 28) - dot);
        if (enabled) ui_icon(r, SettingsIcon::Chevron, x + value_width - 17, row + 33, 12, "accent");
    } else if (enabled) ui_icon(r, SettingsIcon::Chevron, 570 + dx, row + 33, 14, focused ? "accent" : "muted");
}

void App::draw_toggle(SDL_Renderer* r, int x, int y, const std::string& name, bool value, bool enabled) {
    auto key = std::make_pair(int(settings.page), name);
    auto now = s.GetTicks();
    auto found = toggle_motion.find(key);
    if (found == toggle_motion.end())
        found = toggle_motion.emplace(key, ToggleMotion{value, float(value), now - 100}).first;
    auto& motion = found->second;
    auto position = [&]() {
        float t = preferences.animations ? std::min(1.f, (now - motion.at) / 90.f) : 1.f;
        t = 1 - (1 - t) * (1 - t);
        return motion.from + (float(motion.value) - motion.from) * t;
    };
    if (motion.value != value) {
        motion.from = position();
        motion.value = value;
        motion.at = now;
        micro_until = now + 100;
    }
    sunken_track(r, x, y, 44, 28, 14);
    if (enabled && value) raised_pill(r, x + 1, y + 1, 42, 26, 13, theme.at("accent"), 0, false);
    const int knob = x + 5 + int(14 * position());
    alpha_round(r, knob, y + 7, 18, 18, 9, shadow_colour(), preferences.dark ? 90 : 40);
    round_rect(r, knob, y + 5, 18, 18, 9, "panel");
    alpha_round(r, knob + 4, y + 7, 10, 5, 2, Color{255, 255, 255}, preferences.dark ? 40 : 150);
    label(r, value ? "On" : "Off", x - 42, centred_label_y(locale.text(value ? "On" : "Off"), 16, y + 14), 16,
          enabled ? "ink" : "muted", 36);
}

void App::draw_scrollbar(SDL_Renderer* r, int x, int y, int height, int thumb, int offset, bool dragging, bool hover,
                         int width) {
    (void)hover;
    const int cx = x + width / 2;
    round_rect(r, x, y, width, height, width / 2, "slot");
    round_rect(r, cx - 4, y + 10, 8, height - 20, 4, "line");
    round_rect(r, cx - 3, y + 11, 6, height - 22, 3, "background");
    const int top = y + offset, tw = width - 10, left = cx - tw / 2;
    alpha_round(r, left, top + 3, tw, thumb, tw / 2, shadow_colour(), preferences.dark ? 70 : 30);
    if (dragging)
        alpha_round(r, left - 3, top - 3, tw + 6, thumb + 6, tw / 2 + 3, theme.at("accent"),
                    preferences.dark ? 70 : 50);
    glossy_pill(r, left, top, tw, thumb, tw / 2, theme.at("accent"), dragging ? .5 : 0);
    for (int line = -1; line <= 1; line++) {
        tint_rect(r, cx - 6, top + thumb / 2 + line * 6 + 1, 12, 2, blend(theme.at("accent"), Color{0, 0, 0}, 28));
        round_rect(r, cx - 6, top + thumb / 2 + line * 6, 12, 2, 1, "on_accent");
    }
}

void App::round_rect(SDL_Renderer* r, int x, int y, int w, int h, int radius, const std::string& key) {
    color(r, key);
    SDL_Rect center{x + radius, y, w - 2 * radius, h};
    s.RenderFillRect(r, &center);
    for (int dy = 0; dy < radius; dy++) {
        int inset = radius - static_cast<int>(std::sqrt(radius * radius - (radius - dy - 0.5) * (radius - dy - 0.5)));
        SDL_Rect a{x + inset, y + dy, w - inset * 2, 1}, b{x + inset, y + h - 1 - dy, w - inset * 2, 1};
        s.RenderFillRect(r, &a);
        s.RenderFillRect(r, &b);
    }
    rect(r, x, y + radius, w, h - radius * 2, key);
}

void App::fill_round(SDL_Renderer* r, int x, int y, int w, int h, int radius, Color fill) {
    theme["swatch"] = fill;
    round_rect(r, x, y, w, h, radius, "swatch");
}

void App::label_colour(SDL_Renderer* r, const std::string& value, int x, int y, int pixels, Color ink, int width) {
    theme["swatch_ink"] = ink;
    label(r, value, x, y, pixels, "swatch_ink", width);
}

void App::raw_label_colour(SDL_Renderer* r, const std::string& value, int x, int y, int pixels, Color ink, int width) {
    theme["swatch_ink"] = ink;
    raw_label(r, value, x, y, pixels, "swatch_ink", width);
}

void App::check_mark(SDL_Renderer* r, int cx, int cy, Color ink) {
    s.SetRenderDrawColor(r, ink.r, ink.g, ink.b, 255);
    for (int w = 0; w < 2; w++) {
        s.RenderDrawLine(r, cx - 5, cy + w, cx - 1, cy + 4 + w);
        s.RenderDrawLine(r, cx - 1, cy + 4 + w, cx + 6, cy - 4 + w);
    }
}

void App::ring(SDL_Renderer* r, int x, int y, int radius, const std::string& key) {
    color(r, key);
    for (int i = 0; i < 48; i++) {
        double a = i * 6.2831853 / 48, b = (i + 1) * 6.2831853 / 48;
        s.RenderDrawLine(r, x + int(std::cos(a) * radius), y + int(std::sin(a) * radius), x + int(std::cos(b) * radius),
                         y + int(std::sin(b) * radius));
    }
}

void App::icon(SDL_Renderer* r, int kind, int x, int y, const std::string& key) {
    color(r, key);
    if (kind == 0) {
        ring(r, x, y, 11, key);
        ring(r, x - 4, y - 3, 2, key);
        ring(r, x + 4, y - 3, 2, key);
        ring(r, x, y + 5, 2, key);
    } else if (kind == 1) {
        ring(r, x, y + 2, 10, key);
        rect(r, x - 3, y - 12, 6, 15, "panel");
        color(r, key);
        s.RenderDrawLine(r, x, y - 12, x, y + 1);
    } else if (kind == 2) {
        rect(r, x - 11, y - 9, 24, 18, key, true);
        s.RenderDrawLine(r, x - 4, y - 9, x - 4, y + 9);
        s.RenderDrawLine(r, x + 4, y - 9, x + 4, y + 9);
    } else if (kind == 3) {
        rect(r, x - 10, y - 4, 5, 8, key);
        s.RenderDrawLine(r, x - 5, y - 4, x + 1, y - 9);
        s.RenderDrawLine(r, x + 1, y - 9, x + 1, y + 9);
        s.RenderDrawLine(r, x + 1, y + 9, x - 5, y + 4);
        s.RenderDrawLine(r, x + 6, y - 7, x + 10, y);
        s.RenderDrawLine(r, x + 10, y, x + 6, y + 7);
    } else if (kind == 4) {
        for (int i = 0; i < 3; i++) {
            int d = 5 + i * 5;
            s.RenderDrawLine(r, x - d, y - i * 5, x, y - 5 - i * 5);
            s.RenderDrawLine(r, x, y - 5 - i * 5, x + d, y - i * 5);
        }
        rect(r, x - 1, y + 5, 3, 3, key);
    } else {
        ring(r, x, y, 10, key);
        s.RenderDrawLine(r, x, y - 1, x, y + 6);
        rect(r, x - 1, y - 6, 2, 2, key);
    }
}

void App::gear(SDL_Renderer* r, int x, int y) {
    ring(r, x, y, 9, "accent");
    ring(r, x, y, 3, "accent");
    color(r, "accent");
    for (int i = 0; i < 8; i++) {
        double a = i * 6.2831853 / 8;
        s.RenderDrawLine(r, x + int(std::cos(a) * 9), y + int(std::sin(a) * 9), x + int(std::cos(a) * 13),
                         y + int(std::sin(a) * 13));
    }
}
