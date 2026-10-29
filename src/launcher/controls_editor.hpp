#pragma once
// Game menu > Controls: loading, capturing and saving a button mapping.
// Members of App in main.cpp; included once, right after the class.

fs::path App::controls_file(bool per_game) {
    return per_game && !model.games.empty() ? item_folder(model.selected) / "controls.ini" : o.data / "controls.ini";
}

void App::load_controls_editor() {
    auto global = controls_file(false), local = controls_per_game ? controls_file(true) : fs::path{};
    edited_controls.load(global.string(), local.string());
    auto saved = local.empty() ? std::map<std::string, std::string>{} : fields(local);
    controls_inherit = controls_per_game && (saved.empty() || saved["inherit"] == "1");
}

void App::open_controls(bool per_game, Page parent) {
    controls_return = parent;
    controls_per_game = per_game && !model.games.empty();
    control_capture = false;
    capture_logical = -1;
    controls_notice.clear();
    settings.page = Page::Controls;
    settings.section = 6;
    settings.focus = 0;
    load_controls_editor();
    dirty = true;
}

void App::close_controls() {
    control_capture = false;
    capture_logical = -1;
    settings.page = controls_return;
    settings.focus = controls_return == Page::QuickMenu ? 9 : controls_return == Page::GameOptions ? 2 : 0;
    if (controls_return == Page::GameOptions) settings.section = 2;
    if (controls_return == Page::Settings) settings.section = 6;
    dirty = true;
}

bool App::save_controls_editor() {
    try {
        auto path = controls_file(controls_per_game);
        fs::create_directories(path.parent_path());
        auto pending = path.string() + ".pending";
        std::ofstream file(pending);
        file << "schema=1\npreset=" << edited_controls.preset << "\nshortcut=" << edited_controls.shortcut
             << "\nshortcut_action=" << edited_controls.shortcut_action << '\n';
        file << "fast_forward_button=" << physical_names[edited_controls.fast_forward_source]
             << "\nfast_forward_mode=" << edited_controls.fast_forward_mode << '\n';
        if (controls_per_game) file << "inherit=" << controls_inherit << '\n';
        for (int logical = 0; logical < 12; logical++)
            file << "bind_" << control_names[logical] << '=' << physical_names[edited_controls.sources[logical]]
                 << '\n';
        file.close();
        if (!file) throw std::runtime_error("Could not write controls");
        fs::rename(pending, path);
        controls_notice = controls_inherit    ? "Following the default controls."
                          : controls_per_game ? "Controls saved for this game."
                                              : "Default controls saved.";
        dirty = true;
        return true;
    } catch (const std::exception& error) {
        std::cerr << "Control settings: " << error.what() << '\n';
        controls_notice = "Could not save controls. Check writable storage.";
        dirty = true;
        return false;
    }
}

void App::capture_control(int source) {
    if (!control_capture || capture_logical < 0 || source < 0 || source > control_none) return;
    auto previous = edited_controls;
    bool previous_inherit = controls_inherit;
    int old = capture_logical == 12 ? edited_controls.fast_forward_source : edited_controls.sources[capture_logical];
    if (source != control_none)
        for (int logical = 0; logical < 12; logical++)
            if (logical != capture_logical && edited_controls.sources[logical] == source)
                edited_controls.sources[logical] = old;
    if (capture_logical == 12) edited_controls.fast_forward_source = source;
    else {
        if (source != control_none && edited_controls.fast_forward_source == source)
            edited_controls.fast_forward_source = old;
        edited_controls.sources[capture_logical] = source;
    }
    edited_controls.preset = "custom";
    controls_inherit = false;
    if (!save_controls_editor()) {
        edited_controls = previous;
        controls_inherit = previous_inherit;
    }
    control_capture = false;
    capture_logical = -1;
    dirty = true;
}

std::string App::controls_row_name(int row) {
    if (row == 0) return "Profile";
    if (row == 1) return "Button layout";
    if (row == 2) return "Home shortcut";
    if (row == 3) return "Shortcut action";
    if (row == 16) return controls_per_game ? "Use default controls" : "Restore defaults";
    if (row == 17) return "Fast-forward button";
    if (row == 18) return "Fast-forward style";
    return "Button " + control_title(row - 4);
}

std::string App::controls_row_value(int row) {
    if (row == 0) return controls_per_game ? "This game" : "All games";
    if (row == 1)
        return controls_inherit                          ? "Default"
               : edited_controls.preset == "default"     ? "Default"
               : edited_controls.preset == "ab-swap"     ? "A / B swapped"
               : edited_controls.preset == "left-handed" ? "Left-handed"
                                                         : "Custom";
    if (row == 2)
        return edited_controls.shortcut == "start+menu"      ? "Start + Anbernic"
               : edited_controls.shortcut == "startthenmenu" ? "Start, then Anbernic"
                                                             : "Anbernic";
    if (row == 3) return edited_controls.shortcut_action == "home" ? "Return home" : "Open menu";
    if (row == 16) return controls_per_game ? "Default" : "Reset";
    if (row == 17)
        return control_capture && capture_logical == 12 ? "Press a button…"
                                                        : control_title(edited_controls.fast_forward_source);
    if (row == 18) return edited_controls.fast_forward_mode == "hold" ? "Hold" : "Toggle";
    return control_capture && row == capture_logical + 4 ? "Press a button…"
                                                         : control_title(edited_controls.sources[row - 4]);
}

void App::controls_action(int code) {
    if (control_capture) {
        if (code == 3 || code == 7) {
            control_capture = false;
            capture_logical = -1;
            controls_notice = "Binding unchanged.";
        }
        dirty = true;
        return;
    }
    if (code == 3 || code == 7) {
        close_controls();
        return;
    }
    if (code == 8 || code == 9) {
        settings.focus = std::clamp(settings.focus + (code == 8 ? -1 : 1), 0, 18);
        dirty = true;
        return;
    }
    if (code != 0 && code != 1 && code != 2 && code != 4) return;
    int row = settings.focus, direction = code == 0 ? -1 : 1;
    auto previous = edited_controls;
    bool old_inherit = controls_inherit;
    if (row == 0) {
        if (!model.games.empty()) {
            controls_per_game = !controls_per_game;
            load_controls_editor();
            controls_notice.clear();
        }
        dirty = true;
        return;
    }
    if ((row >= 4 && row <= 15) || row == 17) {
        int target = row == 17 ? 12 : row - 4;
        if (code == 2 || code == 4) {
            control_capture = true;
            capture_logical = target;
            capture_started = capture_clock = s.GetTicks();
            controls_notice = "Press a button or direction. Anbernic / Escape cancels.";
        } else {
            int selected = target == 12 ? edited_controls.fast_forward_source : edited_controls.sources[target];
            int next = (selected + direction + 17) % 17;
            control_capture = true;
            capture_logical = target;
            capture_control(next);
        }
        dirty = true;
        return;
    }
    if (row == 1) {
        const std::array<std::string, 3> names = {"default", "ab-swap", "left-handed"};
        auto found = std::find(names.begin(), names.end(), edited_controls.preset);
        int index = found == names.end() ? -1 : int(found - names.begin());
        edited_controls.apply({{"preset", names[(index + direction + 3) % 3]}});
        controls_inherit = false;
    } else if (row == 2) {
        const std::array<std::string, 3> names = {"start+menu", "anbernic", "startthenmenu"};
        int index = int(std::find(names.begin(), names.end(), edited_controls.shortcut) - names.begin());
        edited_controls.shortcut = names[(index + direction + 3) % 3];
        controls_inherit = false;
    } else if (row == 3) {
        edited_controls.shortcut_action = edited_controls.shortcut_action == "home" ? "menu" : "home";
        controls_inherit = false;
    } else if (row == 16) {
        if (controls_per_game) {
            controls_inherit = true;
            edited_controls.load(controls_file(false).string());
        } else edited_controls = ControlMapping{};
    } else if (row == 18) {
        edited_controls.fast_forward_mode = edited_controls.fast_forward_mode == "hold" ? "toggle" : "hold";
        controls_inherit = false;
    }
    if (!save_controls_editor()) {
        edited_controls = previous;
        controls_inherit = old_inherit;
    }
    dirty = true;
}
