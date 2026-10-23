#pragma once
// Colours: the accent colour grid, its keys and touch, and both panels.
// Members of App in main.cpp; included once, right after the class.

void App::open_colours() {
    settings.page = Page::Colours;
    settings.focus = preferences.accent >= 0 ? preferences.accent : 16;
    page_at = s.GetTicks();
    dirty = true;
}

SDL_Rect App::colour_tile(int index) const {
    return {colour_grid_x + (index % 4) * (colour_tile_w + colour_gap), colour_grid_y + (index / 4) * colour_row_pitch,
            colour_tile_w, colour_tile_h};
}

SDL_Rect App::colour_option(int index) const {
    return {index == 16 ? 28 : 332, colour_grid_y + 4 * colour_row_pitch + 4, 280, 48};
}

void App::choose_colour(int index) {
    if (index == 17) {
        preferences.high_contrast = !preferences.high_contrast;
        emit_sound(preferences.high_contrast ? MenuCue::Enable : MenuCue::Disable);
    } else {
        int next = index == 16 ? -1 : index;
        if (next == preferences.accent) {
            dirty = true;
            return;
        }
        preferences.accent = next;
        emit_sound(MenuCue::Confirm);
    }
    remember_appearance();
}

void App::colours_action(int code) {
    int& focus = settings.focus;
    if (code == 3) {
        settings.back();
        dirty = true;
        return;
    }
    if (code == 2 || code == 4) {
        choose_colour(focus);
        dirty = true;
        return;
    }
    if (focus < 16) {
        if (code == 0) focus = focus % 4 == 0 ? focus + 3 : focus - 1;
        else if (code == 1) focus = focus % 4 == 3 ? focus - 3 : focus + 1;
        else if (code == 8) focus = focus < 4 ? (focus < 2 ? 16 : 17) : focus - 4;
        else if (code == 9) focus = focus >= 12 ? (focus % 4 < 2 ? 16 : 17) : focus + 4;
    } else {
        if (code == 0 || code == 1) focus = focus == 16 ? 17 : 16;
        else if (code == 8) focus = focus == 16 ? 12 : 14;
        else if (code == 9) focus = focus == 16 ? 0 : 2;
    }
    emit_sound(MenuCue::Navigate, code == 0 || code == 8 ? -1 : 1);
    dirty = true;
}

void App::colours_touch(int x, int y) {
    for (int i = 0; i < 16; i++)
        if (point_in(colour_tile(i), x, y)) {
            settings.focus = i;
            choose_colour(i);
            dirty = true;
            return;
        }
    for (int i : {16, 17})
        if (point_in(colour_option(i), x, y)) {
            settings.focus = i;
            choose_colour(i);
            dirty = true;
            return;
        }
}

void App::colours_screen(SDL_Renderer* r, int y) {
    settings_header(r, y, "Accent colour");
    label(r, "FAVOURITE COLOUR", 30, y + 84, 13, "muted");
    for (int i = 0; i < 16; i++) {
        auto tile = colour_tile(i);
        tile.y += y;
        bool focused = settings.focus == i, selected = preferences.accent == i;
        Color fill = ds_colours[size_t(i)].rgb;
        if (focused) {
            round_rect(r, tile.x - 5, tile.y - 5, tile.w + 10, tile.h + 10, 16, "ink");
            round_rect(r, tile.x - 2, tile.y - 2, tile.w + 4, tile.h + 4, 13, "background");
        }
        // The DSi's colour squares: the same shaded button as every control.
        raised_pill(r, tile.x, tile.y, tile.w, tile.h, 12, fill);
        Color ink = tile_ink(fill);
        int width = label_width(ds_colours[size_t(i)].name, 16);
        label_colour(r, ds_colours[size_t(i)].name, tile.x + (tile.w - width) / 2,
                     centred_label_y(locale.text(ds_colours[size_t(i)].name), 16, tile.y + tile.h / 2), 16, ink,
                     tile.w - 12);
        if (selected) {
            int cx = tile.x + tile.w - 15, cy = tile.y + 15;
            fill_round(r, cx - 10, cy - 10, 20, 20, 10, ink);
            check_mark(r, cx, cy, fill);
        }
    }
    for (int i : {16, 17}) {
        auto box = colour_option(i);
        box.y += y;
        bool focused = settings.focus == i;
        settings_card(r, box.x, box.y, box.w, box.h, focused);
        if (i == 16) {
            Theme classic = preferences.dark ? dark_theme : light_theme;
            if (preferences.high_contrast) apply_high_contrast(classic, preferences.dark);
            accent_swatch(r, box.x + 28, box.y + 24, 11, classic.at("accent"));
            label(r, "Classic blue", box.x + 50, box.y + 12, 20, "ink", 170);
            if (preferences.accent < 0) ui_icon(r, SettingsIcon::Check, box.x + box.w - 26, box.y + 24, 20, "accent");
        } else {
            ui_icon(r, SettingsIcon::Sun, box.x + 26, box.y + 24, 22, "accent");
            label(r, "High contrast", box.x + 48, box.y + 13, 18, "ink", 130);
            draw_toggle(r, box.x + box.w - 56, box.y + 10, "High contrast", preferences.high_contrast);
        }
    }
}

void App::colours_top(SDL_Renderer* r, int y) {
    // Live preview of real interface parts in the current colours: the
    // name, a button pair, a switch and a bar, centred in the panel.
    const int top = y + 140, left = 48, right = 592;
    settings_card(r, 24, top, 592, 240);
    label(r, accent_name(preferences.accent), left, top + 22, 28, "accent", 300);
    if (preferences.high_contrast) {
        const int w = label_width("High contrast", 16);
        label(r, "High contrast", right - w, centred_label_y(locale.text("High contrast"), 16, top + 42), 16, "muted",
              230);
    }
    raised_pill(r, left, top + 76, 256, 48, 14, theme.at("accent"));
    label(r, "Play", left + 24, centred_label_y(locale.text("Play"), 22, top + 100), 22, "on_accent", 200);
    raised_pill(r, left + 272, top + 76, right - left - 272, 48, 14, theme.at("focus"));
    label(r, "Selected row", left + 296, centred_label_y(locale.text("Selected row"), 20, top + 100), 20, "ink", 230);
    label(r, "Switches and values", left, centred_label_y(locale.text("Switches and values"), 20, top + 162), 20, "ink",
          300);
    label(r, "60%", 410, centred_label_y("60%", 20, top + 162), 20, "accent", 60);
    draw_toggle(r, right - 44, top + 148, "Preview", true);
    sunken_track(r, left, top + 200, right - left, 14, 7);
    glossy_pill(r, left + 1, top + 201, (right - left) * 3 / 5, 12, 6, theme.at("accent"));
}
