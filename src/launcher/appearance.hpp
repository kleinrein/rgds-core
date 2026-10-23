#pragma once
// Themes and the appearance page.
// Members of App in main.cpp; included once, right after the class.

std::map<fs::path, std::optional<std::string>> App::read_theme_source() {
    std::map<fs::path, std::optional<std::string>> result;
    for (auto path : {o.themes / "light.ini", o.themes / "dark.ini", o.theme}) {
        if (path.empty()) continue;
        std::ifstream file(path, std::ios::binary);
        result[path] =
            file ? std::optional<std::string>(std::string(std::istreambuf_iterator<char>(file), {})) : std::nullopt;
    }
    return result;
}

void App::select_theme() {
    theme = preferences.dark ? dark_theme : light_theme;
    apply_accent(theme, preferences.accent, preferences.dark);
    if (preferences.high_contrast) apply_high_contrast(theme, preferences.dark);
}

void App::reload_themes() {
    auto light = fs::exists(o.themes / "light.ini") ? load_theme(o.themes / "light.ini") : default_theme();
    auto dark = fs::exists(o.themes / "dark.ini") ? load_theme(o.themes / "dark.ini", default_dark_theme())
                                                  : default_dark_theme();
    if (!o.theme.empty()) {
        bool is_dark = dark_theme_file(o.theme);
        auto custom = load_theme(o.theme, is_dark ? dark : light);
        if (is_dark) dark = std::move(custom);
        else light = std::move(custom);
    }
    light_theme = std::move(light);
    dark_theme = std::move(dark);
    select_theme();
}

void App::remember_appearance() {
    select_theme();
    try {
        save_preferences(o.preferences, preferences);
        status = "Settings saved";
    } catch (const std::exception& e) {
        status = "Could not save all settings. Check storage.";
        std::cerr << e.what() << std::endl;
        // Base and additive UI preferences have separate atomic files.
        // A partial failure must display what actually reached storage.
        try {
            preferences = load_preferences(o.preferences);
            select_theme();
        } catch (const std::exception&) {}
    }
    locale.load(asset_path("locales"), preferences.language);
    rounded.prefer(preferences.language);
    configure_sounds();
    dirty = true;
}

SDL_Rect App::segment_bounds(int index, int option) {
    const auto& names = index == 0 ? mode_names : typeface_names;
    int width = std::clamp(std::max(label_width(names[0], 18), label_width(names[1], 18)) + 34, 100, 150),
        left = 574 - 2 * width - 8;
    return {left + 4 + option * width, list_top + index * list_stride - list_scroll + 17, width, 32};
}

void App::segment_row(SDL_Renderer* r, int y, int index, const std::string& title, bool second) {
    int row = y + list_top + index * list_stride - list_scroll;
    bool focused = index == list_selected();
    int dx = 24;
    move_element(uint32_t(index), dx, row, 566, 66, focused, list_top, list_top + list_height, 20, 595);
    dx -= 24;
    settings_card(r, 24 + dx, row, 566, 66, focused);
    round_rect(r, 38 + dx, row + 11, 44, 44, 12, focused ? "accent" : "slot");
    ui_icon(r, index == 0 ? (second ? SettingsIcon::Moon : SettingsIcon::Sun) : SettingsIcon::Type, 60 + dx, row + 33,
            25, focused ? "on_accent" : "accent");
    const auto& names = index == 0 ? mode_names : typeface_names;
    auto first = segment_bounds(index, 0);
    first.x += dx;
    first.y += y;
    label(r, title, 98 + dx, row + 19, 24, "ink", first.x - 16 - (98 + dx));
    sunken_track(r, first.x - 4, first.y - 4, first.w * 2 + 8, first.h + 8, 13);
    const float slide = segment_position(index, second ? 1 : 0);
    raised_pill(r, first.x + int(std::lround(slide * first.w)), first.y, first.w, first.h, 10, theme.at("accent"), 0,
                false);
    for (int option = 0; option < 2; option++) {
        SDL_Rect box{first.x + option * first.w, first.y, first.w, first.h};
        bool on = std::abs(slide - option) < .5f;
        int width = std::min(box.w - 12, label_width(names[size_t(option)], 18));
        label(r, names[size_t(option)], box.x + (box.w - width) / 2,
              centred_label_y(locale.text(names[size_t(option)]), 18, box.y + box.h / 2), 18, on ? "on_accent" : "ink",
              box.w - 12);
    }
}

void App::appearance_row(SDL_Renderer* r, int y, int i) {
    switch (i) {
    case 0: segment_row(r, y, i, "Colour mode", preferences.dark); break;
    case 1: {
        Color swatch = theme.at("accent");
        list_row(r, y, i, "Accent colour", accent_name(preferences.accent), true, &swatch);
        break;
    }
    case 2: list_row(r, y, i, "High contrast", preferences.high_contrast ? "On" : "Off"); break;
    case 3: segment_row(r, y, i, "Typeface", preferences.pixel); break;
    case 4: list_row(r, y, i, "Icon style", preferences.rich_icons ? "Rich" : "Simple pixel"); break;
    case 5: list_row(r, y, i, "Screen brightness", std::to_string(system_preferences.brightness) + "%"); break;
    // Animations opens its own page, so even "Off" is a value, not a switch.
    case 6:
        list_row(r, y, i, "Animations",
                 !preferences.animations       ? "Off"
                 : preferences.view_animations ? (preferences.view_style == ViewStyle::Push   ? "Push"
                                                  : preferences.view_style == ViewStyle::Zoom ? "Zoom"
                                                                                              : "Spring")
                                               : "Subtle",
                 true, nullptr, false);
        break;
    case 7: list_row(r, y, i, "Touch indicator", preferences.touch_indicator ? "On" : "Off"); break;
    case 8: list_row(r, y, i, "Home layout", preferences.home_layout == 1 ? "Column" : "Carousel"); break;
    case 9: list_row(r, y, i, "Background", preferences.background == 1 ? "Grid" : "Plain"); break;
    default: list_row(r, y, i, "Touch gestures", preferences.gestures ? "On" : "Off"); break;
    }
}

void App::accent_swatch(SDL_Renderer* r, int cx, int cy, int radius, Color fill) {
    for (int dy = -radius; dy <= radius; dy++) {
        int half = int(std::sqrt(double(radius * radius - dy * dy)));
        tint_rect(r, cx - half, cy + dy, half * 2 + 1, 1, fill);
    }
    ring(r, cx, cy, radius, "line");
}

void App::show_sharing_picker() {
    int current = device_status["sharing"] != "1" ? 0 : device_status["sharing_mode"] == "open" ? 1 : 2;
    show_picker("Network file sharing", {"Off", "Open on this Wi-Fi", "Password protected"}, current,
                [this, current](int n) {
                    if (n == current) return;
                    start_worker(n == 0 ? "device.sharing-off" : n == 1 ? "device.sharing-open" : "device.sharing-on");
                });
}

void App::choose_appearance(int choice) {
    const int row = settings.focus;
    const bool step = choice != 2;
    switch (row) {
    case 0: preferences.dark = step ? choice == 1 : !preferences.dark; break;
    case 1:
        if (!step) {
            open_colours();
            return;
        }
        preferences.accent = std::clamp(preferences.accent + (choice == 1 ? 1 : -1), -1, int(ds_colours.size()) - 1);
        break;
    case 2:
        preferences.high_contrast = step ? choice == 1 : !preferences.high_contrast;
        emit_sound(preferences.high_contrast ? MenuCue::Enable : MenuCue::Disable);
        break;
    case 3: {
        bool pixel = step ? choice == 1 : !preferences.pixel;
        if (!pixel) check_fonts();
        if (!pixel && !font_ready) {
            status = "ROUNDED FONT UNAVAILABLE";
            dirty = true;
            return;
        }
        preferences.pixel = pixel;
        break;
    }
    case 4:
        if (!step) {
            show_picker("Icon style", {"Simple pixel", "Rich"}, preferences.rich_icons ? 1 : 0, [this](int n) {
                preferences.rich_icons = n == 1;
                remember_appearance();
            });
            return;
        }
        preferences.rich_icons = choice == 1;
        break;
    case 5:
        if (step) {
            change_system(4, choice == 0 ? -1 : 1);
            return;
        }
        settings.enter();
        dirty = true;
        return;
    case 6:
        settings.page = Page::Motion;
        settings.focus = 0;
        dirty = true;
        return;
    case 7: preferences.touch_indicator = step ? choice == 1 : !preferences.touch_indicator; break;
    case 8:
        if (!step) {
            show_picker("Home layout", {"Carousel", "Column"}, preferences.home_layout, [this](int n) {
                preferences.home_layout = n;
                home_layout_changed();
                remember_appearance();
            });
            return;
        }
        preferences.home_layout = choice;
        home_layout_changed();
        break;
    case 9:
        if (!step) {
            show_picker("Background", {"Plain", "Grid"}, preferences.background, [this](int n) {
                preferences.background = n;
                remember_appearance();
            });
            return;
        }
        preferences.background = choice;
        break;
    case 10:
        preferences.gestures = step ? choice == 1 : !preferences.gestures;
        if (preferences.gestures) gesture_intro();
        break;
    default: return;
    }
    remember_appearance();
}
