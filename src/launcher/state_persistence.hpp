#pragma once
// Saving and restoring launcher state.
// Members of App in main.cpp; included once, right after the class.

void App::restore_state() {
    if (o.state.empty()) return;
    std::ifstream navigation(o.state);
    std::string navigation_line;
    bool saved_home = false;
    while (std::getline(navigation, navigation_line)) {
        std::istringstream row(navigation_line);
        std::string marker;
        int category = 1;
        if (row >> marker >> category && marker == "home" && category >= 0 && category < int(home_count)) {
            saved_home = true;
            home_menu = true;
            home_selected = size_t(category);
        }
    }
    std::ifstream file(o.state);
    unsigned schema = 0, core = 0;
    size_t selected = 0;
    std::string path;
    if (!(file >> schema >> selected >> core >> std::quoted(path)) || schema != 1 || core >= core_count) return;
    if (!saved_home) home_menu = false;
    model.core = core;
    if (!model.games.empty()) {
        model.selected = std::min(selected, model.games.size() - 1);
        for (size_t i = 0; i < model.games.size(); i++)
            if (!path.empty() && model.games[i].path == fs::path(path)) {
                model.selected = i;
                break;
            }
    }
    int page = 0, section = 0, focus = 0;
    if (file >> page >> section >> focus && page == int(Page::ShaderPicker) && focus >= 0 && focus <= shader_count) {
        std::string marker;
        int per_game = 0;
        if (file >> marker >> per_game && marker == "shader" && (per_game == 0 || per_game == 1) &&
            (!per_game || !model.games.empty())) {
            if (per_game) game_preferences = library_cache.options(model.games[model.selected]);
            open_shader_picker(per_game);
            if (o.game_menu) shader_return = Page::QuickMenu;
            settings.focus = std::min(focus, int(shader_choices.size()) - 1);
            shader_carousel.reset(size_t(std::max(0, settings.focus)));
            int scene = 0, mode = 2;
            float split = .5f;
            if (file >> marker >> scene >> mode >> split && marker == "shader-preview" && scene >= 0 &&
                scene < ShaderPreview::scene_count && mode >= 0 && mode <= 2 && split >= .1f && split <= .9f) {
                shader_preview_scene = scene;
                shader_preview_mode = ShaderPreviewMode(mode);
                shader_preview_split = split;
            }
            return;
        }
    }
    if (file && page == int(Page::Controls) && focus >= 0 && focus <= 18) {
        std::string marker, tail;
        int per_game = 0, parent = int(o.game_menu ? Page::QuickMenu : Page::Settings), saved_parent = parent;
        file >> marker >> per_game;
        std::getline(file, tail);
        std::istringstream extra(tail);
        if (extra >> saved_parent && (saved_parent == int(Page::Settings) || saved_parent == int(Page::QuickMenu) ||
                                      saved_parent == int(Page::GameOptions)))
            parent = saved_parent;
        open_controls(marker == "controls" && per_game == 1, static_cast<Page>(parent));
        settings.focus = focus;
        return;
    }
    if (file && page >= 0 &&
        (page <= int(Page::GameOptions) || page == int(Page::Statistics) ||
         page == int(Page::RomSetup) || page == int(Page::Motion) || page == int(Page::Colours) ||
         page == int(Page::MenuSounds) || page == int(Page::Haptics) || page == int(Page::DisplayOptions) ||
         page == int(Page::Bluetooth) || page == int(Page::Homebrew)) &&
        page != int(Page::Keyboard) && page != int(Page::QuickMenu) && page != int(Page::LeaveGame) &&
        page != int(Page::Confirm) && section >= 0 && section <= 10 && focus >= 0 &&
        focus <= (page == int(Page::Motion)           ? 3
                  : page == int(Page::Timezone)       ? int(timezones.size()) - 1
                  : page == int(Page::Maintenance)    ? 6
                  : page == int(Page::LibraryOptions) ? 10
                  : page == int(Page::Appearance)     ? 5
                  : page == int(Page::System)         ? (o.rgds_os ? 7 : 8)
                  : page == int(Page::Wifi)      ? std::max(0, std::atoi(device_status["network_count"].c_str()) - 1)
                  : page == int(Page::Bluetooth) ? std::max(0, std::atoi(device_status["bt_count"].c_str()) - 1)
                  : page == int(Page::Homebrew)  ? std::max(0, std::atoi(device_status["hb_count"].c_str()) - 1)
                                                 : 4))
        settings = {static_cast<Page>(page), section, focus};
    if (settings.page == Page::Power) {
        settings.section = 1;
    }
    if (settings.page == Page::Placeholder && settings.section == 1) settings.page = Page::Power;
    if (settings.page == Page::Placeholder && settings.section == 0) settings.page = Page::Appearance;
    if (settings.page == Page::Placeholder && settings.section >= 2)
        settings.page = settings.section == 2   ? Page::LibraryOptions
                        : settings.section == 3 ? Page::Sound
                        : settings.section == 4 ? Page::Network
                                                : Page::System;
    if (settings.page == Page::Appearance || settings.page == Page::Colours) settings.section = 0;
    if (settings.page == Page::DisplayOptions || settings.page == Page::Display || settings.page == Page::Motion)
        settings.section = 10;
    if (settings.page == Page::Display) settings.focus = 0;
    if (settings.page == Page::Sound || settings.page == Page::MenuSounds || settings.page == Page::Haptics) {
        settings.section = 3;
        settings.focus = 0;
    }
    if (settings.page == Page::System || settings.page == Page::Timezone) settings.section = 5;
    if (settings.page == Page::System) {
        settings.focus = std::min(settings.focus, list_count() - 1);
        start_worker("device.startup-info");
    }
    if (settings.page == Page::Network) settings.focus = std::min(settings.focus, 6);
    if (settings.page == Page::Maintenance) settings.focus = std::min(settings.focus, 6);
    if (settings.page == Page::Statistics) settings.section = 7;
    if (settings.page == Page::GameOptions) {
        if (model.games.empty()) settings.page = Page::Library;
        else game_preferences = library_cache.options(model.games[model.selected]);
        settings.focus = std::min(settings.focus, 5);
    }
}

void App::save_state() {
    if (o.state.empty() || !session_initialized || !library_ready || settings.page == Page::BootWarning) return;
    try {
        if (!o.state.parent_path().empty()) fs::create_directories(o.state.parent_path());
        auto pending = o.state.string() + ".tmp";
        std::ofstream file(pending);
        file << 1 << ' ' << model.selected << ' ' << model.core << ' '
             << std::quoted(model.games.empty() ? std::string() : model.games[model.selected].path.string()) << '\n';
        if (settings.page != Page::Library && settings.page != Page::Keyboard)
            file << static_cast<int>(settings.page) << ' ' << settings.section << ' ' << settings.focus << '\n';
        if (settings.page == Page::ShaderPicker) file << "shader " << shader_per_game << '\n';
        if (settings.page == Page::ShaderPicker)
            file << "shader-preview " << shader_preview_scene << ' ' << int(shader_preview_mode) << ' '
                 << shader_preview_split << '\n';
        if (settings.page == Page::Controls)
            file << "controls " << controls_per_game << ' ' << int(controls_return) << '\n';
        if (home_menu && !o.game_menu) file << "home " << home_selected << '\n';
        file.close();
        if (!file) throw std::runtime_error("Cannot save launcher state");
        fs::rename(pending, o.state);
    } catch (const std::exception& e) {
        std::cerr << "State: " << e.what() << std::endl;
    }
}
