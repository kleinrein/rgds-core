#pragma once
// DS controls, emulator and graphics options, and quick actions.
// Members of App in main.cpp; included once, right after the class.

void App::ds_controls_row(SDL_Renderer* r, int y, int i) {
    const std::array<std::string, 6> names = {"Blow into microphone", "Close and open DS lid", "DS language",
                                              "Real microphone",      "DS nickname",           "Local wireless"};
    const std::array<std::string, 6> languages = {"Japanese", "English", "French", "German", "Italian", "Spanish"};
    int language = ds_settings.count("language") ? std::clamp(std::atoi(ds_settings["language"].c_str()), 0, 5) : 1;
    list_row(r, y, i, names[i],
             !ds_supported() ? "Engine managed"
             : i == 5        ? wireless_names()[ds_wireless()]
             : i == 4        ? (ds_settings.count("nickname") ? ds_settings["nickname"] : ds_owner_name())
             : i == 2        ? languages[language]
             : i == 3        ? (ds_settings.count("microphone") && ds_settings["microphone"] == "0") ? "Off" : "On"
             : o.game_menu   ? "2 seconds"
                             : "In game",
             ds_supported() && (i >= 2 || o.game_menu));
}

void App::ds_controls_action() {
    if (!ds_supported()) return;
    if (settings.focus < 2) {
        if (o.game_menu) menu_request(settings.focus == 0 ? "ds:blow" : "ds:lid");
        return;
    }
    if (settings.focus == 2)
        show_picker("DS language", {"Japanese", "English", "French", "German", "Italian", "Spanish"},
                    ds_settings.count("language") ? std::atoi(ds_settings["language"].c_str()) : 1,
                    [this](int n) { save_ds_setting("language", n); });
    else if (settings.focus == 4) begin_edit(5);
    else if (settings.focus == 5)
        show_picker("Local wireless", wireless_names(), ds_wireless(),
                    [this](int n) { save_ds_setting("wireless", n); });
    else save_ds_setting("microphone", ds_settings.count("microphone") && ds_settings["microphone"] == "0");
}

// Local wireless (ds-input.ini wireless, DSCore's net.mode): 0 off, 1 join a session heard on the Wi-Fi or host one,
// 2 host, 3 join only.
std::vector<std::string> App::wireless_names() const { return {"Off", "Join or host", "Host", "Join"}; }

int App::ds_wireless() {
    return ds_settings.count("wireless") ? std::clamp(std::atoi(ds_settings["wireless"].c_str()), 0, 3) : 0;
}

void App::save_ds_setting(const std::string& key, const std::string& value) {
    try {
        ds_settings[key] = value;
        auto folder = item_folder(model.selected);
        fs::create_directories(folder);
        auto path = folder / "ds-input.ini", pending = fs::path(path.string() + ".pending");
        std::ofstream out(pending);
        for (auto& pair : ds_settings) out << pair.first << '=' << pair.second << '\n';
        out.close();
        if (out) {
            fs::rename(pending, path);
            status = "Restart the game to apply DS settings.";
        }
    } catch (const std::exception&) {
        status = "Could not save DS settings.";
    }
    dirty = true;
}

void App::rescan_games(bool stay) {
    stop_library_scan();
    library_bytes.clear();
    library_eof = false;
    library_ready = false;
    scan_stay = stay;
    if (!stay) {
        home_menu = true;
        settings.page = Page::Library;
    }
    begin_library_scan();
    dirty = true;
}

void App::confirm_reset_state() {
    show_picker("Restart without loading a state?", {"Cancel", "Reset state"}, 0, [this](int n) {
        if (n) menu_request("reset-state");
    });
}
