#pragma once
// Other systems (RG DS Play): categories, the Systems page and the play view.
// Members of App in main.cpp; included once, right after the class.

void App::open_last_played() {
    const auto& last = summary.groups[0].last_game;
    if (!summary_ready || !last) {
        status = summary_ready ? "Nothing played yet" : "Preparing…";
        dirty = true;
        return;
    }
    library_view(0);
    remember_appearance();
    // The game starts over Home itself. Games opens instead only while a
    // very large card is still being listed, or if the game has left it.
    if (!collection_loading)
        for (size_t i = 0; i < model.games.size(); i++)
            if (model.games[i].path == last->path) {
                select_game(i, false);
                carousel.reset(home_selected); // the rail is Home's while Home is up
                launch(true);
                return;
            }
    home_menu = false;
}

void App::open_category() {
    cancel_navigation();
    if (home_selected == last_played_tile) open_last_played();
    else if (home_selected == pictochat_tile) open_pictochat();
    else if (home_selected == 0) settings.open();
    else if (home_selected == 1 && preferences.show_systems) open_systems();
    else {
        home_menu = false;
        library_view(int(home_selected) - 1);
        remember_appearance();
    }
    dirty = true;
}

bool App::other_system_selected() const {
    return !o.game_menu && model.selected < model.games.size() && model.games[model.selected].system != "nds";
}

void App::refresh_other_systems() {
    all_games = ds_games;
    listed_systems = {"nds"};
    if (preferences.show_systems && !o.game_menu)
        for (const auto& info : systems()) {
            if (std::string(info.id) == "nds") continue;
            std::vector<fs::path> roots;
            for (const auto& [id, path] : o.system_roots)
                if (id == info.id) roots.push_back(path);
            auto games = scan_system(roots, info.id);
            if (games.empty()) continue;
            listed_systems.push_back(info.id);
            all_games.insert(all_games.end(), games.begin(), games.end());
        }
    if (std::find(listed_systems.begin(), listed_systems.end(), shown_system) == listed_systems.end())
        shown_system = "nds";
}

std::vector<Game> App::view_source(int view) const {
    if (view != 0 || !preferences.show_systems) return all_games;
    std::vector<Game> games;
    for (const auto& game : all_games)
        if (game.system == shown_system) games.push_back(game);
    return games;
}

size_t App::system_games(const std::string& id) const {
    return size_t(std::count_if(all_games.begin(), all_games.end(), [&](const Game& g) { return g.system == id; }));
}

void App::open_systems() {
    cancel_navigation();
    settings.page = Page::Library;
    home_menu = false;
    systems_menu = true;
    auto at = std::find(listed_systems.begin(), listed_systems.end(), shown_system);
    systems_selected = at == listed_systems.end() ? 0 : size_t(at - listed_systems.begin());
    // Each card's numbers, as Home's collections show theirs.
    system_stats.assign(listed_systems.size(), {});
    for (const auto& game : all_games)
        for (size_t i = 0; i < listed_systems.size(); i++)
            if (game.system == listed_systems[i] && !game.demo) {
                const auto& played = library_cache.stats(game, true);
                system_stats[i].games++;
                system_stats[i].sessions += played.sessions;
                system_stats[i].seconds += played.seconds;
            }
    selected_at = s.GetTicks();
    carousel.reset(systems_selected);
    status.clear();
    dirty = true;
}

void App::choose_system(size_t index) {
    if (index >= listed_systems.size()) return;
    shown_system = listed_systems[index];
    systems_menu = false;
    library_view(0);
    remember_appearance();
    dirty = true;
}

void App::system_icon(SDL_Renderer* r, size_t index, int x, int y, int size) {
    const auto& info = system_info(index < listed_systems.size() ? listed_systems[index] : "nds");
    ui_icon(r, SettingsIcon::Cartridge, x + size / 2, y + size * 2 / 5, size * 5 / 8, "accent");
    const std::string badge = info.badge;
    raw_label(r, badge, x + (size - label_width(badge, 16, false)) / 2, y + size * 3 / 4, 16, "ink", size);
}

void App::load_play_view() {
    auto values = fields(play_view_path());
    static const std::array<std::string, 3> fits = {"sharp", "integer", "smooth"},
                                            seconds = {"companion", "off", "swap"};
    play_fit = int(std::find(fits.begin(), fits.end(), values["fit"]) - fits.begin()) % 3;
    play_second = int(std::find(seconds.begin(), seconds.end(), values["second"]) - seconds.begin()) % 3;
    auto choices = play_frame_choices();
    play_frame = std::find(choices.begin(), choices.end(), values["frame"]) == choices.end() ? "off" : values["frame"];
}

fs::path App::play_frames_folder() {
    const auto& game = model.games.at(0);
    std::string system = game.system;
    if (system == "nds") system = system_for_path(game.path);
    return o.data / "frames" / (system == "nds" ? "gba" : system);
}

std::vector<std::string> App::play_frame_choices() {
    std::vector<std::string> choices(play_frames.begin(), play_frames.end()), pictures;
    std::error_code ec;
    for (fs::directory_iterator it(play_frames_folder(), ec), end; !ec && it != end; it.increment(ec)) {
        const auto name = it->path().stem().string();
        if (lower(it->path().extension().string()) == ".png" && it->is_regular_file(ec) && !name.empty() &&
            name.size() <= 64 && name[0] != '.' && std::find(choices.begin(), choices.end(), name) == choices.end())
            pictures.push_back(name);
    }
    std::sort(pictures.begin(), pictures.end());
    choices.insert(choices.end(), pictures.begin(), pictures.end());
    return choices;
}

std::string App::play_frame_label(const std::string& frame) const {
    auto found = std::find(play_frames.begin(), play_frames.end(), frame);
    return found == play_frames.end() ? frame : play_frame_names[size_t(found - play_frames.begin())];
}

void App::apply_play_view(int fit, int second, const std::string& frame) {
    if (fit == play_fit && second == play_second && frame == play_frame) return;
    if (o.game_menu && !o.preview) {
        menu_request("play-graphics:" + std::to_string(fit) + ":" + std::to_string(second) + ":" + frame);
        return;
    }
    static const std::array<const char*, 3> fits = {"sharp", "integer", "smooth"},
                                            seconds = {"companion", "off", "swap"};
    std::error_code ec;
    fs::create_directories(play_view_path().parent_path(), ec);
    auto pending = fs::path(play_view_path().string() + ".pending");
    std::ofstream(pending) << "schema=1\nfit=" << fits[size_t(fit)] << "\nsecond=" << seconds[size_t(second)]
                           << "\nframe=" << frame << '\n';
    fs::rename(pending, play_view_path(), ec);
    play_fit = fit;
    play_second = second;
    play_frame = frame;
    status = "Graphics applied (preview).";
    dirty = true;
}

const char* App::play_fit_name(int fit) const {
    return fit == 1 ? "Whole pixels" : fit == 2 ? "Smooth fill" : "Sharp fill";
}

const char* App::play_second_name(int second) const {
    return second == 1 ? "Off" : second == 2 ? "Swap screens" : "Companion";
}

void App::play_graphics_action() {
    if (settings.focus == 0)
        show_picker("Game screen", {"Sharp fill", "Whole pixels", "Smooth fill"}, play_fit,
                    [this](int n) { apply_play_view(n, play_second, play_frame); });
    else if (settings.focus == 1)
        show_picker("Second screen", {"Companion", "Off", "Swap screens"}, play_second,
                    [this](int n) { apply_play_view(play_fit, n, play_frame); });
    else if (settings.focus == 2) {
        auto choices = play_frame_choices();
        std::vector<std::string> labels;
        for (const auto& choice : choices) labels.push_back(play_frame_label(choice));
        int selected = int(std::find(choices.begin(), choices.end(), play_frame) - choices.begin());
        show_picker("Frame", labels, selected < int(choices.size()) ? selected : 0, [this, choices](int n) {
            if (n >= 0 && n < int(choices.size())) apply_play_view(play_fit, play_second, choices[size_t(n)]);
        });
    } else {
        // The same shader carousel as the DS engines: the bridge's pass draws GBA and Game Boy screens too.
        open_shader_picker(true);
        shader_return = Page::PlayGraphics;
    }
}
