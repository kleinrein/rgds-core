#pragma once
// DS controls, emulator and graphics options, and quick actions.
// Members of App in main.cpp; included once, right after the class.

void App::ds_controls_row(SDL_Renderer* r, int y, int i) {
    const std::array<std::string, 6> names = {"Blow into microphone", "Close and open DS lid", "DS language",
                                              "Real microphone",      "DS nickname",           "Play together"};
    const bool no_network = kid_locked() && !active_profile().allow_wireless;
    const std::array<std::string, 6> languages = {"Japanese", "English", "French", "German", "Italian", "Spanish"};
    int language = ds_settings.count("language") ? std::clamp(std::atoi(ds_settings["language"].c_str()), 0, 5) : 1;
    // Play together sums up what is on: online play wins over local wireless in the engine.
    const std::string together = no_network      ? "Off"
                                 : ds_online()   ? online_names()[ds_online()]
                                 : ds_wireless() ? locale.text("Local wireless")
                                                 : "Off";
    list_row(r, y, i, names[i],
             !ds_supported() ? "Engine managed"
             : i == 5        ? together
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
    else if (settings.focus == 5) open_play_together(Page::DSControls);
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

// ---- Play together: local wireless and online play for a DS game, on one page --------------------------------
// Reached from the library (the game's options, before it starts, so it starts straight into the session), from
// DS controls, and in a game from the session tiles: while local wireless holds a session there are no checkpoints
// to save or load, so those two tiles become Play together and Leave session. The engine reports the session
// through the bridge (session/emulator.control.net, written when the menu opens): live=1, role, player lines.

void App::load_net_session() {
    net_session.clear();
    net_players.clear();
    std::ifstream file(o.data / "session/emulator.control.net");
    std::string line;
    while (std::getline(file, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key != "player") {
            net_session[key] = value;
            continue;
        }
        NetPlayer player;
        const auto a = value.find('\t'), b = a == std::string::npos ? a : value.find('\t', a + 1);
        player.name = value.substr(0, a);
        if (a != std::string::npos) player.ping = std::atoi(value.c_str() + a + 1);
        if (b != std::string::npos) player.me = value.substr(b + 1) == "1";
        if (player.name.empty()) player.name = "DS";
        net_players.push_back(player);
    }
    // The preview shows a session with RGDS_PREVIEW_NET=1 (there is no engine behind it).
    if (o.preview && o.game_menu && net_session.empty() && std::getenv("RGDS_PREVIEW_NET")) {
        net_session = {{"live", "1"}, {"role", "host"}};
        net_players = {{"Ada", 0, true}, {"Kim", 6, false}};
    }
}

bool App::net_session_live() const {
    auto found = net_session.find("live");
    return o.game_menu && found != net_session.end() && found->second == "1";
}

void App::open_play_together(Page back) {
    ds_settings = fields(player_item_folder(model.selected) / "ds-input.ini");
    play_together_return = back;
    load_net_session();
    status.clear();
    settings.page = Page::PlayTogether;
    settings.focus = 0;
    dirty = true;
}

// Rows: Local wireless, Online play, then Play now (library) or Leave session (in a live session).
int App::play_together_rows() const { return (!o.game_menu || net_session_live()) ? 3 : 2; }

void App::play_together_row(SDL_Renderer* r, int y, int i) {
    const bool no_network = kid_locked() && !active_profile().allow_wireless;
    if (i == 0)
        list_row(r, y, i, "Local wireless",
                 !ds_supported() ? "Engine managed" : wireless_names()[no_network ? 0 : ds_wireless()],
                 ds_supported() && !no_network);
    else if (i == 1)
        list_row(r, y, i, "Online play",
                 !ds_supported() ? "Engine managed" : online_names()[no_network ? 0 : ds_online()],
                 ds_supported() && !no_network);
    else if (o.game_menu) list_row(r, y, i, "Leave session", "");
    else
        list_row(r, y, i, "Play now", ds_wireless() || ds_online() ? "" : "Turn one on first",
                 ds_supported() && (ds_wireless() || ds_online()));
}

void App::play_together_action() {
    if (!ds_supported()) return;
    if (kid_locked() && !active_profile().allow_wireless) {
        status = "Not on this profile";
        return;
    }
    if (settings.focus == 0)
        show_picker("Local wireless", wireless_names(), ds_wireless(), [this](int n) {
            if (n == ds_wireless()) return;
            save_ds_setting("wireless", n);
            ds_apply_in_game("wireless");
        });
    else if (settings.focus == 1)
        show_picker("Online play", online_names(), ds_online(), [this](int n) {
            if (n == ds_online()) return;
            save_ds_setting("online", n);
            ds_apply_in_game("online");
        });
    else if (o.game_menu) leave_session();
    else if (ds_wireless() || ds_online()) {
        back_to(play_together_return);
        launch();
    }
}

// Leaving: nothing restores a session, so there is no checkpoint either way; the game's own save stays.
void App::leave_session() {
    show_picker("Leave the session", {"Stay in the session", "Play on alone", "Return home"}, 0, [this](int n) {
        if (n == 1) {
            save_ds_setting("wireless", 0);
            menu_request("relaunch:wireless-off");
        } else if (n == 2) menu_request("quit");
    });
}

void App::play_together_top(SDL_Renderer* r, int y) {
    round_rect(r, 24, y + top_card_y, 592, top_card_h, 12, "panel");
    ui_icon(r, SettingsIcon::Wifi, 320, y + 118, 48, "accent");
    centered(r, "Play together", y + 173, 2, "ink");
    if (net_session_live()) {
        const bool host = net_session["role"] == "host";
        const int others = int(std::count_if(net_players.begin(), net_players.end(), [](auto& p) { return !p.me; }));
        centered(r,
                 others == 0
                     ? (host ? "Hosting, waiting for another DS" : "In a session, waiting for the host")
                     : (host ? "Hosting" : "Joined") + std::string("  ·  ") + std::to_string(others + 1) + " players",
                 y + 218, 1, "muted");
        int row = y + 254;
        for (const auto& p : net_players) {
            if (row > y + top_card_y + top_card_h - 34) break;
            ui_icon(r, p.me ? SettingsIcon::Controller : SettingsIcon::Wifi, 92, row + 14, 22, "accent");
            label(r, p.me ? p.name + "  (" + locale.text("you") + ")" : p.name, 116, row, 22, "ink", 330);
            if (!p.me) label(r, std::to_string(p.ping) + " ms", 470, row, 22, "muted", 90);
            row += 36;
        }
        return;
    }
    wrapped(r,
            !ds_supported() ? "Use the engine's native settings for wireless play."
            : o.game_menu
                ? "Local wireless plays with another RG DS on the same Wi-Fi; the game restarts to turn it on. Online "
                  "play uses Wiimmfi or WiiLink, from the game's Nintendo WFC menu."
                : "Local wireless plays with another RG DS on the same Wi-Fi: both pick it, then start the game. "
                  "Online play uses Wiimmfi or WiiLink, from the game's Nintendo WFC menu.",
            54, y + 234, 22, 532, 5);
}
