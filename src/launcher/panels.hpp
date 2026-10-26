#pragma once
// The two panels: top() and bottom() pick what each page draws on the upper
// and lower screen; the pages themselves live in the *_screen headers.
// Members of App in main.cpp; included once, right after the class.

void App::top(SDL_Renderer* r, int y) {
    if (o.port_menu) return; // under the port, which covers this panel
    if (card_repair) {
        card_top(r, y);
        return;
    }
    if (settings.page == Page::BootWarning) {
        boot_top(r, y);
        return;
    }
    if (o.boot_startup && !library_ready) {
        boot_scene(r, y);
        return;
    }
    if (chat_open) {
        chat_top(r, y);
        return;
    }
    if (o.game_menu) {
        rect(r, 0, y, 640, 480, "background");
        media.draw(r, o.data / "session/menu-background.bmp", {0, y + 48, 640, 432}, false, 70);
        status_bar(r, y);
        if (settings.page != Page::LeaveGame) {
            if (settings.page == Page::Keyboard && edit_field == 6) {
                cheat_search_top(r, y);
                return;
            }
            settings_top(r, y);
            if (settings.page != Page::ShaderPicker && settings.page != Page::SaveGallery &&
                settings.page != Page::Cheats && !model.games.empty()) {
                round_rect(r, 24, y + 320, 592, 76, 14, "panel");
                raw_wrapped(r, game_title(0), 44, y + 336, 24, 552, 2);
            }
            return;
        }
        round_rect(r, 24, y + 366, 596, 94, 10, "panel");
        raw_label(r, model.games.empty() ? locale.text("Game menu") : game_title(0), 40, y + 377, 24, "ink", 560);
        std::string message = menu_status["message"];
        if (message.empty())
            message = std::string(core_names[active_engine()]) + "  ·  " +
                      (menu_status["live"] == "1" ? "Live background" : "Paused");
        wrapped(r, message, 40, y + 416, 16, 560, 2);
        return;
    }
    rect(r, 0, y, 640, 480, "background");
    pattern(r, y + 48, 432);
    status_bar(r, y);
    if (probe) {
        label(r, "Top display", 30, y + 96, 36, "ink");
        label(r, top_name, 30, y + 145, 20, "accent");
        for (int i = 0; i < 7; i++) rect(r, 44 + i * 78, y + 210, 60, 56, i % 2 ? "accent" : "ink");
        label(r, "Check panel order and controller input", 30, y + 340, 20, "muted");
        label(r, "B / Escape returns to stock", 30, y + 410, 20, "ink");
        return;
    }
    if (settings.page == Page::Keyboard && edit_field == 4) {
        search_keyboard_top(r, y);
        return;
    }
    if (settings.page == Page::Keyboard && edit_field == 6) {
        cheat_search_top(r, y);
        return;
    }
    if (settings.page != Page::Library) {
        settings_top(r, y);
        return;
    }
    if (home_menu && preferences.home_layout == 1) {
        home_column_top(r, y);
        return;
    }
    if (systems_menu) {
        // As Home's collections: the system and its three numbers, centred in the card.
        round_rect(r, 22, y + 82, 596, 320, 10, "line");
        round_rect(r, 24, y + 84, 592, 316, 8, "panel");
        system_icon(r, systems_selected, 264, y + 100, 112);
        centered(r, system_info(listed_systems[systems_selected]).name, y + 228, 2, "ink");
        const SystemStats& stats = system_stats_for(systems_selected);
        const std::array<std::string, 3> values = {std::to_string(stats.games), std::to_string(stats.sessions),
                                                   std::to_string(stats.seconds / 3600) + "h " +
                                                       std::to_string((stats.seconds / 60) % 60) + "m"};
        const std::array<std::string, 3> captions = {"Games", "Times played", "Time played"};
        for (int i = 0; i < 3; i++) {
            int x = 52 + i * 182;
            raw_label(r, values[i], x + (172 - label_width(values[i], 24)) / 2, y + 282, 24, "ink", 172);
            label(r, captions[i], x + (172 - label_width(captions[i], 16)) / 2, y + 316, 16, "muted", 172);
        }
        return;
    }
    if (home_menu) {
        round_rect(r, 22, y + 82, 596, 320, 10, "line");
        round_rect(r, 24, y + 84, 592, 316, 8, "panel");
        if (home_selected == 0) {
            category_icon(r, 0, 282, y + 162, 76);
            centered(r, "Settings", y + 261, 2, "ink");
            return;
        }
        if (home_selected == pictochat_tile) {
            pictochat_top(r, y);
            return;
        }
        // The collection and its three numbers, centred in the card.
        category_icon(r, home_selected, 288, y + 148, 64);
        centered(r, category_title(home_selected), y + 228, 2, "ink");
        const auto& stats = summary.groups[home_selected > 1 ? home_selected - 1 : 0];
        std::array<std::string, 3> values = {summary_ready ? std::to_string(stats.games) : "…",
                                             summary_ready ? std::to_string(stats.sessions) : "…",
                                             summary_ready ? std::to_string(stats.seconds / 3600) + "h " +
                                                                 std::to_string((stats.seconds / 60) % 60) + "m"
                                                           : "…"};
        const std::array<std::string, 3> captions = {"Games", "Times played", "Time played"};
        for (int i = 0; i < 3; i++) {
            int x = 52 + i * 182;
            raw_label(r, values[i], x + (172 - label_width(values[i], 24)) / 2, y + 282, 24, "ink", 172);
            label(r, captions[i], x + (172 - label_width(captions[i], 16)) / 2, y + 316, 16, "muted", 172);
        }
        if (banner_shown()) update_banner(r, y + 408);
        else if (!library_ready) centered(r, "Preparing your cartridges…", y + 423, 1, "muted");
        else if (return_writes_pending()) centered(r, "Restoring settings…", y + 423, 1, "muted");
        return;
    }
    if (model.games.empty()) {
        label(r,
              library_ready && !collection_loading ? (discovery_active() ? "No matching games" : "No games")
                                                   : "Preparing the library…",
              30, y + 160, 28, "ink");
        return;
    }
    auto item = item_folder(model.selected);
    auto& info = library_cache.metadata(model.games[model.selected]).info;
    auto seconds = library_cache.stats(model.games[model.selected]).seconds;
    stage_favourite = game_loved(model.selected);
    stage_dual = game_dual(model.selected);
    if (!cover_stage(r, y, item / "box.bmp")) icon_stage(r, y, info["developer"]);
    cover_line(r, y, seconds);
}

void App::bottom(SDL_Renderer* r, int y) {
    if (o.port_menu && port_idle) {
        port_companion(r, y);
        return;
    }
    if (o.port_menu && port_manager() && settings.page == Page::QuickMenu) {
        port_manager_menu(r, y);
        return;
    }
    if (card_repair) {
        card_bottom(r, y);
        return;
    }
    if (settings.page == Page::BootWarning) {
        boot_bottom(r, y);
        return;
    }
    if (o.boot_startup && !library_ready) {
        boot_scene(r, y);
        return;
    }
    rect(r, 0, y, 640, 480, "background");
    if (!probe && filter_sheet_active()) {
        filter_sheet_render(r, y);
        if (picker_open || picker_closing()) render_picker(r, y);
        return;
    }
    if (!probe && settings.page != Page::Library) {
        settings_bottom(r, y);
        if (!(o.game_menu && menu_status["state"] == "busy")) settings_footer(r, y);
        if (picker_open || picker_closing()) render_picker(r, y);
        return;
    }
    pattern(r, y, 480, true);

    if (probe) {
        label(r, "Bottom display", 28, y + 29, 24, "ink", 440);
        label(r, bottom_name, 28, y + 85, 16, "muted", 480);
        for (int row = 0; row < 4; row++)
            for (int col = 0; col < 8; col++)
                rect(r, 32 + col * 72, y + 146 + row * 46, 66, 40, (col + row) % 2 ? "slot" : "panel");
        if (touch_x >= 0) {
            color(r, "accent");
            s.RenderDrawLine(r, touch_x - 20, y + touch_y, touch_x + 20, y + touch_y);
            s.RenderDrawLine(r, touch_x, y + touch_y - 20, touch_x, y + touch_y + 20);
        }
        label(r, last_input, 30, y + 355, 20, "ink");
        label(r, "Home / F1 switches probe", 30, y + 410, 16, "muted");
        return;
    }
    if (home_menu && preferences.home_layout == 1) {
        home_column_bottom(r, y);
        if (picker_open || picker_closing()) render_picker(r, y);
        return;
    }
    auto header = library_header_layout();
    auto card = header.card;
    round_rect(r, card.x, y + card.y + 3, card.w, card.h, 14, "slot");
    round_rect(r, card.x, y + card.y, card.w, card.h, 14, "line");
    round_rect(r, card.x + 1, y + card.y + 1, card.w - 2, card.h - 2, 13, "panel");
    triangle(r, 320, y + 128, 17, 19, "line");
    triangle(r, 320, y + 126, 15, 19, "panel");
    if (home_menu) centered(r, category_title(home_selected), y + 56, 3, "ink");
    else if (systems_menu) {
        auto back = header.back;
        back_button(r, back.x, y + back.y, library_control_active(LibraryPress::Back, back));
        centered(r, system_info(listed_systems[systems_selected]).name, y + 56, 3, "ink");
    } else {
        // With Show all systems, All shows one system's games: name it.
        const std::array<std::string, 3> views = {preferences.show_systems ? std::string(system_info(shown_system).name)
                                                                           : "All games",
                                                  "Favourites", "Recents"};
        auto back = header.back;
        bool back_pressed = library_control_active(LibraryPress::Back, back);
        back_button(r, back.x, y + back.y, back_pressed);
        // One quiet row: the collection (or the active search), the
        // count and a search glyph. The row is the touch target for search.
        auto search = header.search;
        bool pressed = library_control_active(LibraryPress::Search, search);
        if (pressed) round_rect(r, search.x - 6, y + search.y + 4, search.w + 12, search.h - 8, 10, "focus");
        const int row_cy = y + back.y + back.h / 2;
        const std::string collection =
            discovery_active() ? discovery_description() : locale.text(views[preferences.view]);
        const std::string count = collection_loading
                                      ? locale.text("Searching…")
                                      : locale.text(std::to_string(listed_games()) +
                                                    (discovery_active() ? (listed_games() == 1 ? " match" : " matches")
                                                                        : (listed_games() == 1 ? " game" : " games")));
        const int right_edge = discovery_active() ? header.clear.x - 6 : search.x + search.w - 40,
                  count_width = label_width(count, 14, false);
        raw_label(r, count, right_edge - count_width, centred_label_y(count, 14, row_cy), 14, "muted", count_width + 4);
        raw_label(r, collection, header.category_x, centred_label_y(collection, 16, row_cy), 16,
                  discovery_active() ? "accent" : "ink",
                  std::max(40, right_edge - count_width - 12 - header.category_x));
        if (discovery_active()) {
            auto clear = header.clear;
            bool active = library_control_active(LibraryPress::Clear, clear);
            round_rect(r, clear.x + 4, y + clear.y + 6, clear.w - 8, clear.h - 12, 10, active ? "accent" : "slot");
            int cx = clear.x + clear.w / 2, cy = y + clear.y + clear.h / 2;
            color(r, active ? "on_accent" : "accent");
            for (int w = 0; w < 2; w++) {
                s.RenderDrawLine(r, cx - 4, cy - 4 + w, cx + 4, cy + 4 + w);
                s.RenderDrawLine(r, cx + 4, cy - 4 + w, cx - 4, cy + 4 + w);
            }
        } else search_icon(r, search.x + search.w - 18, row_cy, 18, "accent");
        rect(r, card.x + 20, y + 73, card.w - 40, 1, "slot");
        auto title = header.view;
        bool title_pressed = library_control_active(LibraryPress::View, title);
        if (title_pressed) round_rect(r, title.x - 8, y + title.y - 5, title.w + 16, title.h + 10, 8, "focus");
        if (!model.games.empty())
            raw_label(r, game_title(model.selected), header.title.x, y + header.title.y, 20, "ink", header.title.w);
    }

    if (!home_menu && model.games.empty() && library_ready && !collection_loading && discovery_active()) {
        search_icon(r, 320, y + 200, 36, "muted");
        centered(r, "No matching games", y + 243, 2, "ink");
        centered(r, "Try another title or clear your filters", y + 282, 1, "muted");
        raised_pill(r, 80, y + 326, 232, 48, 12, theme.at("accent"));
        label(r, "Edit search", 124, y + 338, 20, "on_accent", 200);
        raised_pill(r, 328, y + 326, 232, 48, 12, theme.at("panel"));
        label(r, "Clear filters", 368, y + 338, 20, "accent", 200);
    }
    size_t count = navigation_count(), selection = navigation_selected();
    if (count && !home_menu && browse_layout() == 2) {
        SDL_Rect clip{24, y + LibraryViewport::top, 568, LibraryViewport::height};
        s.RenderSetClipRect(r, &clip);
        for (int index = browser_list.first(); index < browser_list.last(count); index++) {
            int row = y + LibraryViewport::top + index * LibraryViewport::stride - browser_list.scroll;
            bool selected = size_t(index) == model.selected;
            SDL_Rect bounds{24, row - y, 568, 48};
            bool pressed = library_press_row == index && library_control_active(LibraryPress::Row, bounds);
            int dx = 24;
            move_element(uint32_t(index), dx, row, 568, 48, selected, LibraryViewport::top,
                         LibraryViewport::top + LibraryViewport::height, 24, 592);
            dx -= 24;
            int inset = pressed ? 1 : 0;
            round_rect(r, 24 + dx + inset, row + inset, 568 - 2 * inset, 48 - 2 * inset, 7,
                       selected  ? "focus"
                       : pressed ? "line"
                                 : "panel");
            if (selected || pressed) rect(r, 24 + dx, row + 8, 4, 32, "accent");
            game_icon(r, size_t(index), 38 + dx, row + 6, 36);
            raw_label(r, game_title(size_t(index)), 88 + dx, row + 10, 20, "ink",
                      game_loved(size_t(index)) ? 456 : 488);
            if (game_loved(size_t(index))) favourite_badge(r, 568 + dx, row + 24, 20);
        }
        s.RenderSetClipRect(r, nullptr);
        if (browser_list.maximum(count) > 0) {
            bool pressed = library_control_active(LibraryPress::Vertical,
                                                  {596, LibraryViewport::top, 32, LibraryViewport::height});
            draw_scrollbar(r, 594, y + LibraryViewport::top, LibraryViewport::height, browser_list.thumb(count),
                           browser_list.thumb_top(count) - LibraryViewport::top, browser_rail || pressed, browser_hover,
                           36);
        }
    } else if (count && !home_menu && browse_layout() == 1) {
        SDL_Rect clip{0, y + LibraryViewport::top, 640, LibraryViewport::height};
        s.RenderSetClipRect(r, &clip);
        std::vector<int> visible;
        int first = int(std::floor(carousel.position)) - 3;
        for (int index = std::max(0, first); index < std::min(int(count), first + 7); index++) visible.push_back(index);
        // Draw distant covers first so the focused box owns every overlapping pixel.
        std::stable_sort(visible.begin(), visible.end(), [&](int a, int b) {
            return std::abs(a - carousel.position) > std::abs(b - carousel.position);
        });
        for (int index : visible) {
            double distance = index - carousel.position, depth = std::min(2.0, std::abs(distance));
            int size = int(std::lround(224 - depth * 34)),
                x = int(std::lround(320 + distance * carousel.item_spacing)) - size / 2,
                row = y + 151 + int(depth * 18);
            bool selected = index == int(selection);
            move_element(uint32_t(index), x, row, size, size, selected, LibraryViewport::top,
                         LibraryViewport::top + LibraryViewport::height);
            if (auto path = item_folder(size_t(index)) / "box.bmp";
                !cover_art(r, path, {x, row, size, size}, selected)) {
                game_icon(r, size_t(index), x + (size - 64) / 2, row + 20, 64);
                if (!media.pending(path)) label(r, "No cover yet", x + 10, row + size - 43, 16, "muted", size - 20);
            }
            if (game_loved(size_t(index))) favourite_badge(r, x + size - 14, row + 14, selected ? 28 : 22);
        }
        // A stable action badge sits below the artwork, clear of its frame.
        // Midway between the cover and the rail.
        int play_width = std::max(92, label_width("Play", 16) + 48), play_x = 320 - play_width / 2;
        raised_pill(r, play_x, y + 386, play_width, 28, 14, theme.at("accent"));
        ring(r, play_x + 16, y + 400, 8, "on_accent");
        raw_label(r, "A", play_x + 16 - label_width("A", 12) / 2, centred_label_y("A", 12, y + 400), 12, "on_accent",
                  18);
        label(r, "Play", play_x + 32, centred_label_y(locale.text("Play"), 16, y + 400), 16, "on_accent",
              play_width - 38);
        s.RenderSetClipRect(r, nullptr);
    } else if (count) {
        SDL_Rect clip{0, y + LibraryViewport::top, 640, LibraryViewport::height};
        s.RenderSetClipRect(r, &clip);
        int first = int(std::floor(carousel.position)) - 3;
        for (int tile_pass = 0; tile_pass < 2; tile_pass++)
            for (int offset = 0; offset < 7; offset++) {
                const int tile_size = 128, icon_size = 72;
                int index = first + offset;
                if ((index == int(selection)) != (tile_pass == 1)) continue;
                double distance = index - carousel.position;
                int x = int(std::round(320 + distance * carousel.item_spacing)) - tile_size / 2;
                bool selected = index == int(selection);
                bool pressed = selected && pointer_down && carousel.dragging && !carousel.moved && !carousel.range &&
                               carousel.tapped(down_x, count) == selection;
                int lift = pressed ? -3
                           : selected && preferences.animations
                               ? int(std::max(0.0, 1.0 - (s.GetTicks() - selected_at) / 190.0) * 4)
                               : 0,
                    row = y + 196 - lift;
                if (index < 0 || index >= int(count)) {
                    round_rect(r, x, row, tile_size, tile_size, 5, "line");
                    round_rect(r, x + 2, row + 2, tile_size - 4, tile_size - 4, 3, "background");
                    continue;
                }
                move_element(uint32_t(index), x, row, tile_size, tile_size, selected, LibraryViewport::top,
                             LibraryViewport::top + LibraryViewport::height);
                if (selected)
                    start_frame(r, x, row, tile_size,
                                home_menu || systems_menu || is_port_entry(model.games[size_t(index)].path) ? "OPEN"
                                                                                                            : "PLAY");
                round_rect(r, x, row + 4, tile_size, tile_size, 5, "line");
                round_rect(r, x, row, tile_size, tile_size, 5, "line");
                round_rect(r, x + 2, row + 2, tile_size - 4, tile_size - 4, 4, "panel");
                rect(r, x + 9, row + 7, tile_size - 18, 1, "background");
                rect(r, x + 10, row + tile_size - 6, tile_size - 20, 1, "slot");
                if (home_menu)
                    category_icon(r, size_t(index), x + (tile_size - icon_size) / 2, row + (tile_size - icon_size) / 2,
                                  icon_size);
                else if (systems_menu)
                    system_icon(r, size_t(index), x + (tile_size - icon_size) / 2, row + (tile_size - icon_size) / 2,
                                icon_size);
                else {
                    game_icon(r, size_t(index), x + (tile_size - icon_size) / 2, row + (tile_size - icon_size) / 2,
                              icon_size);
                    if (game_loved(size_t(index))) favourite_badge(r, x + tile_size - 14, row + 14, 22);
                }
            }
        s.RenderSetClipRect(r, nullptr);
    }
    // The list uses this space for rows; paging controls belong to the
    // layouts that actually page horizontally.
    if (home_menu || browse_layout() != 2) {
        // One edge-to-edge rail for Home, cartridges and covers, after the
        // DSi menu: glass end buttons, a pip per item in a recessed groove,
        // a glass window over the visible range and a glossy thumb that
        // lifts while held, with the position floating above it.
        // Shared hit bounds.
        const auto now_ticks = s.GetTicks();
        draw_rail(r, y, count, carousel.position,
                  {rail_press[0].value(now_ticks, preferences.animations),
                   rail_press[1].value(now_ticks, preferences.animations),
                   rail_press[2].value(now_ticks, preferences.animations)},
                  !home_menu && !systems_menu);
    }

    if (picker_open || picker_closing()) render_picker(r, y);
}
