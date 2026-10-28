#pragma once
// The settings picker and detail rail: opening a choice list for a settings
// row, its keys, touch and scrolling, and drawing it on the bottom panel.
// Members of App in main.cpp; included once, right after the class.

void App::show_picker(std::string title, std::vector<std::string> options, int selected,
                      std::function<void(int)> apply) {
    picker_title = std::move(title);
    picker_options = std::move(options);
    picker_selected = picker_focus = std::clamp(selected, 0, std::max(0, int(picker_options.size()) - 1));
    picker_apply = std::move(apply);
    picker_core_offset = -1;
    picker_detail = false;
    picker_open = true;
    picker_scroll = 0;
    picker_rail = picker_swipe = scroll_hover = false;
    reveal_picker_focus();
    picker_at = s.GetTicks();
    hold.clear();
    dirty = true;
}

void App::show_emulator_picker(bool per_game) {
    std::vector<std::string> choices;
    if (per_game) choices.push_back("Use default (" + std::string(core_names[preferences.core]) + ")");
    std::vector<int> cores;
    for (int i = 0; i < core_count; i++)
        if (core_offered(i)) {
            cores.push_back(i);
            choices.push_back(std::string(core_names[i]) + (i == dscore_core ? " (recommended)" : ""));
        }
    const int current = per_game ? game_preferences.core : int(preferences.core);
    const int row = int(std::find(cores.begin(), cores.end(), current) - cores.begin());
    show_picker(per_game ? "Game emulator" : "Default emulator", choices, per_game ? (current < 0 ? 0 : row + 1) : row,
                [this, per_game, cores](int n) {
                    const int core = per_game ? (n == 0 ? -1 : cores.at(n - 1)) : cores.at(n);
                    if (per_game) choose_game_emulator(core);
                    else if (int(preferences.core) != core) {
                        preferences.core = unsigned(core);
                        remember_appearance();
                    }
                });
    picker_core_offset = per_game ? 1 : 0;
    picker_cores = cores;
}

const char* App::drastic_backend_name(int value) const {
    return value < 0 ? "Default" : value == 1 ? "SuperDrastic" : "Standard";
}

void App::show_drastic_backend_picker(bool per_game) {
    std::vector<std::string> choices;
    if (per_game)
        choices.push_back("Use default (" + std::string(drastic_backend_name(preferences.drastic_backend)) + ")");
    choices.push_back("Standard");
    choices.push_back("SuperDrastic (experimental)");
    show_picker("DraStic backend", choices,
                per_game ? game_preferences.drastic_backend + 1 : preferences.drastic_backend, [this, per_game](int n) {
                    if (per_game) {
                        if (game_preferences.drastic_backend == n - 1) return;
                        auto next = game_preferences;
                        next.drastic_backend = n - 1;
                        apply_game_preferences(next, "Saved. Applies on your next DraStic launch.");
                    } else if (preferences.drastic_backend != n) {
                        preferences.drastic_backend = n;
                        remember_appearance();
                    }
                });
}

void App::show_drastic_screen_picker() {
    show_picker("Standard screen order", {"Normal", "Swapped"}, game_preferences.drastic_screen_swap == 1,
                [this](int n) {
                    if (n == (game_preferences.drastic_screen_swap == 1)) return;
                    auto next = game_preferences;
                    next.drastic_screen_swap = n;
                    apply_game_preferences(next, o.game_menu ? "Saved. Save & restart applies screen order."
                                                             : "Saved for the next Standard DraStic launch.");
                });
}

std::string App::emulator_choice_hint() const {
    const int row = picker_focus - picker_core_offset;
    if (row < 0 || row >= int(picker_cores.size()))
        return "Follows your Library default. Each emulator keeps its own saves.";
    const int core = picker_cores[row];
    static const std::array<const char*, ds_core_count> hints = {
        "DraStic: an alternative for games that need it. Saves stay separate.",
        "DSperate: the original upstream engine. DSCore adds fixes and speed.",
        "NNDDSS: retired. A saved choice of it plays in DSCore.",
        "melonDS: experimental. RG DS game testing is still pending.",
        "DSCore: recommended. Fastest, with lid, stylus and 3D fixes and optional 2x–5x 3D."};
    return hints.at(core);
}

bool App::open_setting_picker() {
    if ((settings.page == Page::GameOptions && settings.focus >= game_options_base() - 3) ||
        (settings.page == Page::EmulatorOptions && settings.focus >= list_count() - 2))
        return false;
    int row = settings.focus;
    // Other systems play in RG DS Play: no DS engine to pick for them.
    if (settings.page == Page::GameOptions && other_system_selected() && row == 1) return false;
    if (settings.page == Page::System && row == 0) {
        std::vector<std::string> names;
        int selected = 0;
        for (auto& item : locale.languages()) {
            if (item.first == preferences.language) selected = int(names.size());
            names.push_back(item.second);
        }
        show_picker("Language", names, selected, [this](int n) {
            preferences.language = locale.languages().at(n).first;
            locale.load(asset_path("locales"), preferences.language);
            rounded.prefer(preferences.language);
            remember_appearance();
        });
        return true;
    }
    if (settings.page == Page::System && row == 2) {
        show_picker("Daylight saving", {dst_names[0], dst_names[1], dst_names[2]}, system_preferences.dst,
                    [this](int n) { save_dst(n); });
        return true;
    }
    if (settings.page == Page::System && row == 3) {
        show_picker("Date format",
                    {date_format_names[0], date_format_names[1], date_format_names[2], date_format_names[3],
                     date_format_names[4]},
                    system_preferences.date_format, [this](int n) { save_date_format(n); });
        return true;
    }
    if (settings.page == Page::System && row == 4) {
        show_picker("Time format", {time_format_names[0], time_format_names[1]}, system_preferences.time_format,
                    [this](int n) { save_time_format(n); });
        return true;
    }
    if (settings.page == Page::EmulatorOptions && !emulator_option_enabled(row)) return false;
    if (settings.page == Page::LibraryOptions && library_row(row) == 0) {
        show_emulator_picker(false);
    } else if (settings.page == Page::LibraryOptions && library_row(row) == 5) {
        choose_browser_layout();
    } else if (settings.page == Page::LibraryOptions && library_row(row) == 6) {
        show_drastic_backend_picker(false);
    } else if (settings.page == Page::GameOptions && row == 5 && experimental_game()) {
        show_experimental_resolution_picker();
    } else if ((settings.page == Page::GameOptions && row == 5) ||
               (settings.page == Page::EmulatorOptions && row == 8)) {
        show_drastic_backend_picker(true);
    } else if ((settings.page == Page::GameOptions && row == 6) ||
               (settings.page == Page::EmulatorOptions && row == 9)) {
        if (settings.page == Page::GameOptions && superdrastic_game()) return true;
        show_drastic_screen_picker();
    } else if (settings.page == Page::GameOptions && row == 7 && superdrastic_game()) {
        show_superdrastic_resolution_picker();
    } else if (settings.page == Page::Scraper && row == 0) {
        if (worker > 0 && worker_action == "scrape") {
            service_notice = "Stop scraping before changing source";
            return true;
        }
        show_picker("Artwork source", {"Automatic", "GameTDB", "Libretro"}, preferences.artwork_provider,
                    [this](int n) {
                        stop_worker();
                        preferences.artwork_provider = n;
                        remember_appearance();
                        start_worker("status");
                    });
    } else if (settings.page == Page::Scraper && row == 4) {
        show_picker("Extra pictures", {"Off", "Screenshots", "Title screens", "Screenshots and title screens"},
                    preferences.extra_pictures, [this](int n) {
                        preferences.extra_pictures = n;
                        remember_appearance();
                    });
    } else if (settings.page == Page::GameOptions && row == 1) {
        show_emulator_picker(true);
    } else if (settings.page == Page::Power && power_rows[size_t(row)] == 7) {
        if (!hardware.brightness) return false;
        show_picker("Dim when idle", {dim_name(0), dim_name(1), dim_name(2), dim_name(3), dim_name(4)},
                    system_preferences.dim, [this](int n) { change_system(7, n - system_preferences.dim); });
    } else if (settings.page == Page::Power && power_rows[size_t(row)] == 8) {
        if (!hardware.sleep) return false;
        show_picker(
            "Sleep timer in games",
            {game_sleep_name(0), game_sleep_name(1), game_sleep_name(2), game_sleep_name(3), game_sleep_name(4)},
            system_preferences.game_sleep, [this](int n) { change_system(8, n - system_preferences.game_sleep); });
    } else if ((settings.page == Page::Power && row == 0) || (settings.page == Page::EmulatorOptions && row == 2)) {
        if (!hardware.power) return false;
        show_picker("Power mode", {power_names[0], power_names[1], power_names[2], power_names[3]},
                    system_preferences.power, [this](int n) { change_system(0, n - system_preferences.power); });
    } else if (settings.page == Page::Power && row == 1) {
        if (!hardware.sleep) return false;
        show_picker("Menu idle sleep", {idle_name(0), idle_name(1), idle_name(2), idle_name(3), idle_name(4)},
                    system_preferences.idle, [this](int n) { change_system(1, n - system_preferences.idle); });
    } else if (settings.page == Page::Controls && !control_capture && row == 0) {
        // Without a game there is nothing to attach a profile to: the list holds only Global so A still answers.
        std::vector<std::string> profiles = {"Global"};
        if (!model.games.empty()) profiles.push_back("This game");
        show_picker("Profile", profiles, controls_per_game, [this](int n) {
            controls_per_game = n;
            load_controls_editor();
            controls_notice.clear();
        });
    } else if (settings.page == Page::Controls && !control_capture && row >= 1 && row <= 3) {
        if (row == 1) {
            int selected = edited_controls.preset == "ab-swap" ? 1 : edited_controls.preset == "left-handed" ? 2 : 0;
            show_picker("Button layout", {"Default", "A / B swapped", "Left-handed"}, selected, [this](int n) {
                auto previous = edited_controls;
                bool inherit = controls_inherit;
                const char* values[] = {"default", "ab-swap", "left-handed"};
                edited_controls.apply({{"preset", values[n]}});
                controls_inherit = false;
                if (!save_controls_editor()) {
                    edited_controls = previous;
                    controls_inherit = inherit;
                }
            });
        } else if (row == 2) {
            int selected = edited_controls.shortcut == "anbernic"        ? 1
                           : edited_controls.shortcut == "startthenmenu" ? 2
                                                                         : 0;
            show_picker("Home shortcut", {"Start + Anbernic", "Anbernic", "Start, then Anbernic"}, selected,
                        [this](int n) {
                            auto previous = edited_controls;
                            bool inherit = controls_inherit;
                            const char* values[] = {"start+menu", "anbernic", "startthenmenu"};
                            edited_controls.shortcut = values[n];
                            controls_inherit = false;
                            if (!save_controls_editor()) {
                                edited_controls = previous;
                                controls_inherit = inherit;
                            }
                        });
        } else
            show_picker("Shortcut action", {"Open menu", "Return home"}, edited_controls.shortcut_action == "home",
                        [this](int n) {
                            auto previous = edited_controls;
                            bool inherit = controls_inherit;
                            edited_controls.shortcut_action = n ? "home" : "menu";
                            controls_inherit = false;
                            if (!save_controls_editor()) {
                                edited_controls = previous;
                                controls_inherit = inherit;
                            }
                        });
    } else if (settings.page == Page::EmulatorOptions && row == 6) {
        show_emulator_picker(true);
    } else if (settings.page == Page::EmulatorOptions && row == 5) {
        show_picker("Frameskip", {"Off", "1 frame", "2 frames", "3 frames"}, std::max(0, game_preferences.frameskip),
                    [this](int n) { change_emulator_option(n - std::max(0, game_preferences.frameskip)); });
    } else if (settings.page == Page::EmulatorOptions && row == 4) {
        if (experimental_game()) {
            show_experimental_resolution_picker();
            return true;
        }
        if (superdrastic_game()) {
            show_superdrastic_resolution_picker();
            return true;
        }
        show_picker("3D resolution", {"Native", "2x"}, game_preferences.hires == 1, [this](int n) {
            if (n != (game_preferences.hires == 1)) change_emulator_option(1);
        });
    } else return false;
    return true;
}

std::string App::device_info_name(int row) const {
    static const std::array<const char*, 5> names = {"RGDS build", "Stock firmware", "Kernel", "Free space",
                                                     "Device address"};
    return names.at(row);
}

std::string App::device_info_value(int row) {
    static const std::array<const char*, 5> keys = {"build", "firmware", "kernel", "free_gb", "ip"};
    auto value = device_status[keys.at(row)];
    return value.empty() ? "Not available" : value + (row == 3 ? " GB" : "");
}

void App::show_detail(const std::string& title, const std::string& value) {
    picker_core_offset = -1;
    picker_title = title;
    std::string line;
    detail_lines.clear();
    detail_scroll = 0;
    // Break even unspaced build/kernel identifiers, without cutting UTF-8.
    for (size_t at = 0; at < value.size();) {
        size_t end = at + 1;
        while (end < value.size() && (static_cast<unsigned char>(value[end]) & 0xc0) == 0x80) end++;
        auto letter = value.substr(at, end - at);
        at = end;
        if (letter == "\n") {
            detail_lines.push_back(line);
            line.clear();
            continue;
        }
        if (letter == "\r") continue;
        if (letter == "\t") letter = " ";
        if (!line.empty() && label_width(line + letter, 20) > 516) {
            auto space = line.find_last_of(' ');
            if (space != std::string::npos) {
                detail_lines.push_back(line.substr(0, space));
                line.erase(0, space + 1);
            } else {
                detail_lines.push_back(line);
                line.clear();
            }
        }
        line += letter;
    }
    if (!line.empty()) detail_lines.push_back(line);
    picker_options.clear();
    picker_apply = {};
    picker_detail = picker_open = true;
    picker_at = s.GetTicks();
    hold.clear();
    dirty = true;
}

void App::scroll_detail_rail(int y) {
    detail_scroll = std::clamp((y - 164) * detail_max_scroll() / 224, 0, detail_max_scroll());
    dirty = true;
}

int App::picker_max_scroll() const {
    return std::max(0, int(picker_options.size()) * picker_stride() - picker_height());
}

int App::picker_thumb() const {
    return std::max(54, picker_height() * picker_height() / std::max(1, int(picker_options.size()) * picker_stride()));
}

int App::picker_thumb_offset() const {
    return picker_max_scroll() ? picker_scroll * (picker_height() - picker_thumb()) / picker_max_scroll() : 0;
}

void App::reveal_picker_focus() {
    if (!picker_scrollable()) return;
    int row = picker_focus * picker_stride();
    if (row < picker_scroll) picker_scroll = row;
    if (row + picker_stride() > picker_scroll + picker_height())
        picker_scroll = row + picker_stride() - picker_height();
    picker_scroll = std::clamp(picker_scroll, 0, picker_max_scroll());
}

void App::scroll_picker(int position) {
    picker_scroll = std::clamp(position, 0, picker_max_scroll());
    int first = (picker_scroll + picker_stride() - 1) / picker_stride();
    int last = (picker_scroll + picker_height()) / picker_stride() - 1;
    picker_focus = std::clamp(picker_focus, first, std::min(last, int(picker_options.size()) - 1));
    dirty = true;
}

void App::seek_picker(int y) {
    scroll_picker(int(std::lround((y - picker_top() - 64 - picker_grab) * double(picker_max_scroll()) /
                                  std::max(1, picker_height() - picker_thumb()))));
}

int App::picker_first() const {
    if (picker_scrollable()) return picker_scroll / picker_stride();
    return std::clamp(picker_focus - picker_visible() + 1, 0,
                      std::max(0, int(picker_options.size()) - picker_visible()));
}

bool App::picker_paged() const {
    return !picker_detail && !picker_scrollable() && picker_title != "Jump to letter" &&
           int(picker_options.size()) > picker_visible();
}

int App::picker_top() const {
    return picker_title == "Jump to letter" ? 32 : picker_detail ? 96 : 410 - picker_visible() * picker_stride() - 64;
}

void App::picker_action(int code) {
    motion_input(code);
    if (picker_detail) {
        if (code == 2 || code == 3 || code == 4 || code == 7) {
            picker_open = false;
            detail_drag = detail_rail = false;
        } else if (code == 0 || code == 8) detail_scroll = std::max(0, detail_scroll - 1);
        else if (code == 1 || code == 9) detail_scroll = std::min(detail_max_scroll(), detail_scroll + 1);
        dirty = true;
        return;
    }
    if (picker_title == "Jump to letter") {
        if (code == 12 || (picker_options.empty() && (code == 2 || code == 4))) {
            quick_search();
            return;
        }
        if (code == 11) {
            picker_open = false;
            picker_apply = {};
            open_filter_sheet(true);
            return;
        }
        if (picker_options.empty() && code != 3 && code != 7) return;
    }
    if (picker_title == "Jump to letter" && (code == 8 || code == 9)) {
        picker_focus = std::clamp(picker_focus + (code == 8 ? -7 : 7), 0, int(picker_options.size()) - 1);
        dirty = true;
        return;
    }
    if (code == 3 || code == 7) {
        picker_open = false;
        picker_apply = {};
    } else if (code == 0 || code == 8) picker_focus = std::max(0, picker_focus - 1);
    else if (code == 1 || code == 9) picker_focus = std::min(int(picker_options.size()) - 1, picker_focus + 1);
    else if (code == 2 || code == 4) {
        auto apply = std::move(picker_apply);
        int selected = picker_focus;
        picker_open = false;
        if (apply) apply(selected);
    }
    if (code == 0 || code == 1 || code == 8 || code == 9) reveal_picker_focus();
    dirty = true;
}

void App::picker_touch(int x, int y) {
    if (y >= 424) {
        if (x < 200) picker_action(3);
        else if (x >= 440) picker_action(2);
        else if (picker_title == "Jump to letter") picker_action(12);
        return;
    }
    if (y < picker_top()) {
        picker_action(3);
        return;
    }
    if (picker_detail) return;
    if (picker_paged() && y >= picker_top() + 16 && y < picker_top() + 56) {
        if (x >= 480 && x < 528) picker_focus = std::max(0, picker_first() - 1);
        else if (x >= 544 && x < 592)
            picker_focus = std::min(int(picker_options.size()) - 1, picker_first() + 2 * picker_visible() - 1);
        dirty = true;
        return;
    }
    if (picker_title == "Jump to letter") {
        if (point_in({40, 96, 364, 52}, x, y)) {
            picker_action(12);
            return;
        }
        if (point_in({416, 96, 184, 52}, x, y)) {
            picker_action(11);
            return;
        }
        if (x >= 40 && x < 600 && y >= 164 && y < 404) {
            int index = (y - 164) / 60 * 7 + (x - 40) / 80;
            if (index < int(picker_options.size()) && (x - 40) % 80 < 72 && (y - 164) % 60 < 52) {
                picker_focus = index;
                picker_action(2);
            }
        }
        return;
    }
    if (picker_scrollable()) {
        int local = y - picker_top() - 64;
        if (picker_max_scroll() > 0 && x >= 572 && x < 604 && local >= 0 && local < picker_height()) {
            int offset = picker_thumb_offset();
            picker_grab = local >= offset && local < offset + picker_thumb() ? local - offset : picker_thumb() / 2;
            seek_picker(y);
            return;
        }
        int at = local + picker_scroll, row = at / picker_stride();
        if (x >= 40 && x < 560 && local >= 0 && local < picker_height() && at % picker_stride() < picker_stride() - 6 &&
            row < int(picker_options.size())) {
            picker_focus = row;
            picker_action(2);
        }
        return;
    }
    int row = (y - picker_top() - 64) / picker_stride() + picker_first();
    if (x >= 40 && x <= 600 && y >= picker_top() + 64 && row >= picker_first() &&
        row < picker_first() + picker_visible() && row < int(picker_options.size())) {
        picker_focus = row;
        picker_action(2);
    }
}

void App::render_picker(SDL_Renderer* r, int y) {
    render_picker_body(r, y);
    s.RenderSetClipRect(r, nullptr);
}

void App::render_picker_body(SDL_Renderer* r, int y) {
    const bool closing = !picker_open;
    if (drawing_elements && !closing) motion_hits.clear(); // The modal owns input, including its empty spaces.
    // A sheet: it rises in and slides away when closed, both scaled by the view Speed.
    // Input and persistence never wait for either.
    const Uint32 now = s.GetTicks();
    float progress = !preferences.animations ? 1.f
                     : closing               ? 1.f - std::min(1.f, (now - picker_closed_at) / sheet_out_ms())
                                             : std::min(1.f, (now - picker_at) / sheet_in_ms());
    const float ease = progress * progress * progress * (progress * (6 * progress - 15) + 10);
    int top = y + picker_top() + int(std::lround((closing ? 420 : 28) * (1 - ease)));
    s.SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    s.SetRenderDrawColor(r, 0, 0, 0, Uint8(std::lround(92 * ease)));
    SDL_Rect shade{0, y, 640, 424};
    s.RenderFillRect(r, &shade);
    s.SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    if (closing) {
        SDL_Rect clip{0, y, 640, 424};
        s.RenderSetClipRect(r, &clip);
    }
    settings_card(r, 24, top, 592, 414 - (top - y));
    round_rect(r, 294, top + 8, 52, 4, 2, "line");
    bool library_modal = picker_title == "Browser layout" || picker_title == "Jump to letter";
    if (library_modal) library_choice_icon(r, picker_title == "Jump to letter" ? 6 : 0, 62, top + 38, 28, "accent");
    label(r, picker_title, library_modal ? 88 : 48, top + 24, 24, "ink",
          picker_paged()  ? 416
          : library_modal ? 500
                          : 540);
    if (picker_paged()) {
        const bool before = picker_first() > 0, after = picker_first() + picker_visible() < int(picker_options.size());
        secondary_button(r, 480, top + 16, 48, 40, 10, false, before);
        secondary_button(r, 544, top + 16, 48, 40, 10, false, after);
        triangle(r, 504, top + 30, 8, 12, before ? "accent" : "line", false);
        triangle(r, 568, top + 30, 8, 12, after ? "accent" : "line");
    }
    if (picker_detail) {
        for (int i = detail_scroll; i < std::min(int(detail_lines.size()), detail_scroll + 8); i++)
            label(r, detail_lines[i], 48, top + 68 + (i - detail_scroll) * 28, 20, "ink", 516);
        if (detail_max_scroll() > 0) {
            int thumb = std::max(36, 224 * 8 / int(detail_lines.size()));
            draw_scrollbar(r, 582, top + 68, 224, thumb, detail_scroll * (224 - thumb) / detail_max_scroll(),
                           detail_rail, false, 30);
        }
        rect(r, 0, y + 424, 640, 56, "background");
        rect(r, 24, y + 424, 592, 1, "line");
        footer_button(r, 12, y + 432, 160, "Close", "B", false, footer_pressed(true));
        footer_button(r, 468, y + 432, 160, "Done", "A", true, footer_pressed(false));
        return;
    }
    if (picker_title == "Jump to letter") {
        raised_pill(r, 40, y + 96, 364, 52, 10, theme.at("accent"));
        search_icon(r, 65, y + 121, 22, "on_accent");
        key_hints(r, 92, y + 122, {{"Y", "Quick search"}}, -1, 22, 296, "on_accent", true);
        secondary_button(r, 416, y + 96, 184, 52, 10, false, true);
        ui_icon(r, SettingsIcon::Sliders, 439, y + 122, 24, "accent");
        key_hints(r, 461, y + 122, {{"X", "Filter"}}, -1, 20, 130, "accent");
        if (picker_options.empty())
            wrapped(r, "No letters to jump to. Search for another game.", 56, y + 196, 22, 528, 3);
        for (int i = 0; i < int(picker_options.size()); i++) {
            int x = 40 + (i % 7) * 80, row = y + 164 + (i / 7) * 60;
            if (i == picker_focus) raised_pill(r, x, row, 72, 52, 8, theme.at("accent"));
            else round_rect(r, x, row, 72, 52, 8, "focus");
            label(r, picker_options[i], x + (72 - label_width(picker_options[i], 24)) / 2,
                  centred_label_y(picker_options[i], 24, row + 26), 24, i == picker_focus ? "on_accent" : "ink", 64);
        }
    } else {
        SDL_Rect clip{40, top + 64, 564, picker_height()};
        if (picker_scrollable()) s.RenderSetClipRect(r, &clip);
        int end =
            std::min(int(picker_options.size()), picker_first() + picker_visible() + (picker_scrollable() ? 1 : 0));
        for (int i = picker_first(); i < end; i++) {
            int stride = picker_stride(),
                row = top + 64 + (picker_scrollable() ? i * stride - picker_scroll : (i - picker_first()) * stride),
                dx = 40, width = picker_scrollable() ? 520 : 560;
            bool focused = i == picker_focus;
            if (!picker_scrollable()) move_element(uint32_t(i + 100), dx, row, width, stride - 6, focused, 0, 424);
            dx -= 40;
            if (focused)
                alpha_round(r, 40 + dx, row + 3, width, stride - 6, 10, shadow_colour(), preferences.dark ? 70 : 24);
            round_rect(r, 40 + dx, row, width, stride - 6, 10, focused ? "focus" : "panel");
            if (focused) round_rect(r, 40 + dx, row + (stride - 34) / 2, 4, 28, 2, "accent");
            bool layout = picker_title == "Browser layout", swatch = picker_title == "Accent colour",
                 palette = picker_title == "Screen colours" && play_colour_kind() == 1 && i < play::gb_palette_count;
            if (layout) library_choice_icon(r, i + 3, 70 + dx, row + (stride - 6) / 2, 28, "accent");
            if (swatch) {
                Theme preview = preferences.dark ? dark_theme : light_theme;
                apply_accent(preview, i - 1, preferences.dark);
                accent_swatch(r, 74 + dx, row + (stride - 6) / 2, 12, preview.at("accent"));
            }
            if (palette) {
                // The palette's four colours, lightest to darkest, as one outlined strip.
                const int top_y = row + (stride - 6) / 2 - 9;
                rect(r, 61 + dx, top_y - 1, 74, 20, "line");
                for (int shade = 0; shade < 4; shade++) {
                    const uint32_t c = play::gb_palettes[i].shade[shade];
                    tint_rect(r, 62 + dx + shade * 18, top_y, 18, 18,
                              Color{uint8_t(c >> 16), uint8_t(c >> 8), uint8_t(c)});
                }
            }
            const int text_left = palette ? 148 : layout || swatch ? 98 : 58;
            const int text_trim = palette ? 184 : layout || swatch ? 134 : 94;
            label(r, picker_options[i], text_left + dx, row + (stride - 30) / 2, 22, "ink", width - text_trim);
            if (i == picker_selected)
                ui_icon(r, SettingsIcon::Check, 40 + width - 29 + dx, row + (stride - 6) / 2, 22, "accent");
            else ring(r, 40 + width - 29 + dx, row + (stride - 6) / 2, 8, focused ? "accent" : "line");
        }
        if (picker_scrollable()) {
            s.RenderSetClipRect(r, nullptr);
            if (picker_max_scroll() > 0)
                draw_scrollbar(r, 572, top + 64, picker_height(), picker_thumb(), picker_thumb_offset(), picker_rail,
                               scroll_hover);
        }
    }
    rect(r, 0, y + 424, 640, 56, "background");
    rect(r, 24, y + 424, 592, 1, "line");
    footer_button(r, 12, y + 432, 160, "Cancel", "B", false, footer_pressed(true));
    if (picker_title == "Jump to letter") key_hints(r, 320, y + 452, {{"Y", "Search"}, {"X", "Filter"}}, 0, 16, 280);
    else label(r, "Choose an option", 202, y + 445, 16, "muted", 252);
    footer_button(r, 468, y + 432, 160,
                  picker_title == "Jump to letter" ? (picker_options.empty() ? "Search" : "Jump") : "Apply", "A", true,
                  footer_pressed(false));
}
