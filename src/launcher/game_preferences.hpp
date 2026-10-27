#pragma once
// Per-game preferences and resolution pickers.
// Members of App in main.cpp; included once, right after the class.

void App::open_game_options() {
    if (model.games.empty() || is_port_entry(model.games[model.selected].path)) return; // a tool, no options
    game_preferences = library_cache.options(model.games[model.selected], true);
    status.clear();
    settings.page = Page::GameOptions;
    settings.focus = 0;
    settings.section = 2;
    dirty = true;
}

bool App::apply_game_preferences(const GameOptions& next, const char* message) {
    try {
        save_game_options(item_folder(model.selected) / "game.ini", next);
        game_preferences = next;
        library_cache.saved_options(model.games[model.selected], next);
        if (!o.game_menu) library_summary.request(o.data, all_games);
        status = message;
    } catch (const std::exception& e) {
        status = "Could not save game settings. Previous values kept.";
        std::cerr << e.what() << std::endl;
        dirty = true;
        return false;
    }
    dirty = true;
    return true;
}

void App::choose_game_emulator(int core) {
    if (core == game_preferences.core) return;
    auto next = game_preferences;
    next.core = core;
    apply_game_preferences(next, o.game_menu ? "Saved. Save & restart changes the emulator." : "Game emulator saved.");
}

void App::change_game_option(int delta, bool toggle) {
    auto next = game_preferences;
    if (settings.focus == 0) next.favourite = toggle ? !next.favourite : delta > 0;
    else if (settings.focus == 1) {
        int& value = next.core;
        const int step = toggle || delta > 0 ? 1 : -1;
        do
            value = toggle ? (value >= ds_core_count - 1 ? -1 : value + 1)
                           : std::clamp(value + step, -1, ds_core_count - 1);
        while (core_retired(value));
    } else {
        open_shader_picker(true);
        return;
    }
    apply_game_preferences(next, "Game options saved");
}

std::string App::shader_name(int value) {
    return value < 0 ? "Default" : shaders.at(std::clamp(value, 0, shader_count - 1)).name;
}

bool App::experimental_game() const {
    if (o.game_menu) return o.game_core == "rgds-experimental";
    if (other_system_selected()) return false;
    return (game_preferences.core < 0 ? int(preferences.core) : game_preferences.core) == 4;
}

std::string App::experimental_resolution_name() const {
    return game_preferences.rgds_scale == 1 ? "Native" : std::to_string(game_preferences.rgds_scale) + "x";
}

void App::show_experimental_resolution_picker() {
    show_picker("DSCore 3D resolution",
                {"Native (fastest)", "2x sharper", "3x sharper", "4x (experimental)", "5x (experimental)"},
                game_preferences.rgds_scale - 1, [this](int n) {
                    auto next = game_preferences;
                    next.rgds_scale = n + 1;
                    apply_experimental_graphics(next);
                });
}

bool App::superdrastic_game() const {
    int core = game_preferences.core < 0 ? int(preferences.core) : game_preferences.core;
    int backend = game_preferences.drastic_backend < 0 ? preferences.drastic_backend : game_preferences.drastic_backend;
    if (o.game_menu) return o.game_core == "drastic" && std::getenv("RGDS_SD_SHARED_MENU");
    return core == 0 && backend == 1 && !other_system_selected();
}

int App::superdrastic_resolution() const {
    return game_preferences.hires == 1 ? (game_preferences.gengis_scale >= 2 ? game_preferences.gengis_scale : 1) : 0;
}

const char* App::superdrastic_resolution_name() const {
    static const char* names[] = {"Native", "2x", "2x Gengis", "3x Gengis"};
    return names[superdrastic_resolution()];
}

void App::show_superdrastic_resolution_picker() {
    show_picker("3D resolution", {"Native", "2x (DraStic)", "2x Gengis (experimental)", "3x Gengis (experimental)"},
                superdrastic_resolution(), [this](int n) {
                    auto next = game_preferences;
                    next.hires = n > 0;
                    next.gengis_scale = n >= 2 ? n : 0;
                    apply_game_preferences(next, "Saved. Applies on your next SuperDrastic launch.");
                });
}
