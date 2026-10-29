#pragma once
// Per-game preferences and resolution pickers.
// Members of App in main.cpp; included once, right after the class.

void App::open_game_options() {
    if (model.games.empty() || is_tool_entry(model.games[model.selected].path)) return; // a tool, no options
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

void App::change_game_option(int delta, bool toggle) {
    auto next = game_preferences;
    if (settings.focus == 0) next.favourite = toggle ? !next.favourite : delta > 0;
    else {
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
