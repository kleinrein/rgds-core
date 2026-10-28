#pragma once
// The game switcher and library search.
// Members of App in main.cpp; included once, right after the class.

// The Home key: the game search, wherever the launcher is; pressed again while typing it closes.
void App::launcher_search_shortcut() {
    if (o.game_menu || !session_initialized || settings.page == Page::BootWarning) return;
    if (settings.page == Page::Keyboard && edit_field == 4) {
        finish_edit(false);
        return;
    }
    leave_for_shortcut();
    if (settings.page == Page::GameSwitcher && home_switcher) close_home_switcher(); // so B comes back to that page
    quick_search();
}

// The Anbernic button: the game switcher, as the button opens it in a game; pressed again it closes.
void App::launcher_switcher_shortcut() {
    if (o.game_menu || !session_initialized || settings.page == Page::BootWarning) return;
    leave_for_shortcut();
    if (settings.page == Page::GameSwitcher && home_switcher) close_home_switcher();
    else open_home_switcher();
}

// Whatever was half done (a picker, a capture, a test, typing) is left before either shortcut.
void App::leave_for_shortcut() {
    cancel_navigation();
    picker_open = false;
    picker_apply = {};
    picker_detail = false;
    probe = false;
    if (settings.page == Page::Keyboard) finish_edit(false);
    if (settings.page == Page::Microphone) {
        stop_microphone();
        settings.page = Page::Sound;
        configure_sounds();
    }
    if (settings.page == Page::Confirm) settings.page = confirm_return;
    control_capture = false;
    capture_logical = -1;
    filter_sheet_open = false;
}

void App::open_home_switcher() {
    switch_games.clear();
    for (const auto& game : summary.recent_games)
        if (!game.demo && !is_tool_entry(game.path) && switch_games.size() < 9) switch_games.push_back(game);
    if (switch_games.empty()) {
        status = summary_ready ? "Nothing played yet" : "Preparing…";
        dirty = true;
        return;
    }
    if (settings.page != Page::GameSwitcher) {
        switcher_return = settings.page;
        switcher_return_focus = settings.focus;
    }
    home_switcher = true;
    settings.page = Page::GameSwitcher;
    settings.focus = 0;
    status.clear();
    dirty = true;
}

void App::close_home_switcher() {
    home_switcher = false;
    settings.page = switcher_return;
    settings.focus = switcher_return_focus;
    dirty = true;
}

void App::play_game(Game game) {
    home_switcher = false;
    settings.page = Page::Library;
    // With every system shown, the games list holds one system's games: this game's own.
    if (preferences.show_systems &&
        std::find(listed_systems.begin(), listed_systems.end(), game.system) != listed_systems.end())
        shown_system = game.system;
    library_view(0);
    remember_appearance();
    // The game starts over Home itself. Games opens instead only while a
    // very large card is still being listed, or if the game has left it.
    if (!collection_loading)
        for (size_t i = 0; i < model.games.size(); i++)
            if (model.games[i].path == game.path) {
                select_game(i, false);
                if (home_menu) carousel.reset(home_selected); // the rail is Home's while Home is up
                launch(true);
                return;
            }
    home_menu = false;
}

void App::save_switcher_library() {
    auto path = o.data / "session/switcher-library.ini", pending = fs::path(path.string() + ".pending");
    fs::create_directories(path.parent_path());
    std::ofstream out(pending);
    std::set<fs::path> seen;
    int count = 0;
    auto add = [&](const Game& game) {
        auto rom = game.path.string();
        if (game.demo || count >= 9 || rom.find_first_of("\r\n") != std::string::npos || !seen.insert(game.path).second)
            return;
        auto title = library_title(game.title);
        std::replace(title.begin(), title.end(), '\n', ' ');
        std::replace(title.begin(), title.end(), '\r', ' ');
        out << "game_" << count << "_rom=" << rom << "\ngame_" << count << "_title=" << title << '\n';
        count++;
    };
    if (!model.games.empty()) add(model.games[model.selected]);
    for (const auto& game : summary.recent_games) add(game);
    // A fresh library can still switch games before it has play history.
    for (const auto& game : model.games) {
        if (count >= 9) break;
        add(game);
    }
    out << "count=" << count << '\n';
    out.close();
    if (out) fs::rename(pending, path);
}

void App::load_switcher() {
    switch_games.clear();
    auto values = fields(o.data / "session/game-switcher.ini");
    for (int i = 0; i < std::clamp(std::atoi(values["count"].c_str()), 0, 9); i++) {
        auto prefix = "game_" + std::to_string(i);
        fs::path path = values[prefix + "_rom"];
        if (!path.empty()) {
            std::error_code ec;
            auto canonical = fs::weakly_canonical(path, ec);
            if (!ec) path = canonical;
        }
        if (!path.empty())
            switch_games.push_back(
                {values[prefix + "_title"].empty() ? path.stem().string() : values[prefix + "_title"], path, false,
                 system_for_path(path)});
    }
    if (switch_games.empty() && !model.games.empty()) switch_games.push_back(model.games[0]);
}

std::string App::discovery_filter_name() const {
    static const std::array<std::string, 4> names = {"All games", "Unplayed", "Played", "Missing artwork"};
    return names.at(discovery_filter);
}

std::string App::discovery_filters_text() const {
    std::string value = discovery_filter ? locale.text(discovery_filter_name()) : "";
    if (discovery_facets.any()) value += (value.empty() ? "" : "  ·  ") + discovery_facets.summary();
    return value;
}

std::string App::discovery_description() const {
    std::string value = search_query;
    const auto filters = discovery_filters_text();
    if (!filters.empty()) value += (value.empty() ? "" : "  ·  ") + filters;
    return value;
}

void App::apply_discovery() {
    home_menu = false;
    settings.page = Page::Library;
    settings.focus = 0;
    library_view(preferences.view);
    dirty = true;
}

void App::clear_discovery() {
    search_query.clear();
    discovery_filter = 0;
    discovery_facets = {};
    filter_sheet_open = false;
    apply_discovery();
}

void App::quick_search() {
    picker_open = false;
    picker_apply = {};
    begin_edit(4);
}

void App::search_changed() {
    if (edit_field != 4) return;
    search_preview.cancel();
    search_matches.clear();
    search_preview_waiting = true;
    search_preview_loading = true;
    search_preview_at = s.GetTicks();
    dirty = true;
}

void App::poll_search_preview(Uint32 now) {
    if (settings.page != Page::Keyboard || edit_field != 4) return;
    try {
        if (search_preview_waiting && now - search_preview_at >= 150 && library_ready) {
            search_preview_waiting = false;
            search_preview.request(o.data, all_games, preferences.view, trimmed_query(edit_buffer), discovery_filter,
                                   discovery_facets);
        }
        if (auto result = search_preview.poll()) {
            search_matches = std::move(result->games);
            search_preview_loading = false;
            dirty = true;
        }
    } catch (const std::exception&) {
        search_preview_loading = false;
        dirty = true;
    }
}
