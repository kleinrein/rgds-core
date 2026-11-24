#pragma once
// DS controls, emulator and graphics options, and quick actions.
// Members of App in main.cpp; included once, right after the class.

void App::ds_controls_row(SDL_Renderer* r, int y, int i) {
    const std::array<std::string, 7> names = {"Blow into microphone", "Close and open DS lid", "DS language",
                                              "Real microphone",      "DS nickname",           "Local wireless",
                                              "Online play"};
    const bool no_network = kid_locked() && !active_profile().allow_wireless;
    const std::array<std::string, 6> languages = {"Japanese", "English", "French", "German", "Italian", "Spanish"};
    int language = ds_settings.count("language") ? std::clamp(std::atoi(ds_settings["language"].c_str()), 0, 5) : 1;
    list_row(r, y, i, names[i],
             !ds_supported() ? "Engine managed"
             : i == 6        ? online_names()[no_network ? 0 : ds_online()]
             : i == 5        ? wireless_names()[no_network ? 0 : ds_wireless()]
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
    // In a game every change takes effect there and then, each the only way it can (ds_apply_in_game).
    if (settings.focus == 2) {
        const int language = ds_settings.count("language") ? std::atoi(ds_settings["language"].c_str()) : 1;
        show_picker("DS language", {"Japanese", "English", "French", "German", "Italian", "Spanish"}, language,
                    [this, language](int n) {
                        if (n == language) return;
                        save_ds_setting("language", n);
                        ds_apply_in_game("language");
                    });
    } else if (settings.focus == 4) begin_edit(5);
    else if (settings.focus >= 5 && kid_locked() && !active_profile().allow_wireless) status = "Not on this profile";
    else if (settings.focus == 6)
        show_picker("Online play", online_names(), ds_online(), [this](int n) {
            if (n == ds_online()) return;
            save_ds_setting("online", n);
            ds_apply_in_game("online");
        });
    else if (settings.focus == 5)
        show_picker("Local wireless", wireless_names(), ds_wireless(), [this](int n) {
            if (n == ds_wireless()) return;
            save_ds_setting("wireless", n);
            ds_apply_in_game("wireless");
        });
    else {
        save_ds_setting("microphone", ds_settings.count("microphone") && ds_settings["microphone"] == "0");
        ds_apply_in_game("microphone");
    }
}

// A DS setting changed from the game menu takes effect in the running game, never "after a restart" left to the
// player: Online play switches live (the engine swaps the network behind its access point; when it cannot, the
// supervisor relaunches); local wireless and the microphone are set when the engine starts, so the game
// relaunches from a checkpoint and lands on the same moment; the language and nickname are read by the game
// itself when it boots, which a checkpoint would undo, so only a fresh start applies them -- that loses
// progress since the game's own last save, so it asks. Outside a game the next launch reads them anyway.
void App::ds_apply_in_game(const std::string& key) {
    if (!o.game_menu) {
        status = key == "language" || key == "nickname" ? "Applies when the game starts without resuming." : "";
        return;
    }
    status.clear();
    if (key == "online") menu_request("online:" + std::to_string(ds_online()));
    else if (key == "wireless") {
        if (ds_online()) status = "Online play is on. Local wireless applies when it is off.";
        else menu_request(ds_wireless() ? "relaunch:wireless-on" : "relaunch:wireless-off");
    } else if (key == "microphone") menu_request("relaunch:microphone");
    else
        show_picker(key == "language" ? "The game reads its language when it starts"
                                      : "The game reads your nickname when it starts",
                    {"Restart later", "Restart now, unsaved progress is lost"}, 0, [this](int n) {
                        if (n) menu_request("reset-state");
                        else status = "Applies when the game starts without resuming.";
                    });
}

// Local wireless (ds-input.ini wireless, DSCore's net.mode): 0 off, 1 join a session heard on the Wi-Fi or host one,
// 2 host, 3 join only.
std::vector<std::string> App::wireless_names() const { return {"Off", "Join or host", "Host", "Join"}; }

// Online play (ds-input.ini online): 0 off, else DSCore's net.mode = internet with net.dns naming the fan-run
// Nintendo WFC service the game's lookups go to: 1 Wiimmfi, 2 WiiLink WFC. It wins over local wireless.
std::vector<std::string> App::online_names() const { return {"Off", "Wiimmfi", "WiiLink WFC"}; }

int App::ds_online() {
    return ds_settings.count("online") ? std::clamp(std::atoi(ds_settings["online"].c_str()), 0, 2) : 0;
}

int App::ds_wireless() {
    return ds_settings.count("wireless") ? std::clamp(std::atoi(ds_settings["wireless"].c_str()), 0, 3) : 0;
}

void App::save_ds_setting(const std::string& key, const std::string& value) {
    try {
        ds_settings[key] = value;
        auto folder = player_item_folder(model.selected);
        fs::create_directories(folder);
        auto path = folder / "ds-input.ini", pending = fs::path(path.string() + ".pending");
        std::ofstream out(pending);
        for (auto& pair : ds_settings) out << pair.first << '=' << pair.second << '\n';
        out.close();
        if (out) fs::rename(pending, path);
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
