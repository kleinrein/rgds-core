#pragma once
// Text editing fields and the scraper action.
// Members of App in main.cpp; included once, right after the class.

void App::begin_edit(int field) {
    if (field == 4) search_return = settings.page;
    edit_field = field;
    const auto* profile = edited_profile();
    edit_buffer = field == 9 && profile    ? profile->name
                  : field == 10 && profile ? profile->ds_name
                  : field == 5             ? (ds_settings.count("nickname") ? ds_settings["nickname"] : ds_owner_name())
                  : field == 7             ? (ds_owner_name() == "RG DS Core" ? std::string() : ds_owner_name())
                  : field == 4             ? search_query
                  : field == 6             ? cheat_query
                  : field == 11            ? games_query
                  : field == 0             ? username
                  : field == 1             ? password
                  : field == 12            ? (achievements_name.empty() ? achievements_user : achievements_name)
                  : field == 3             ? artwork_match
                                           : "";
    key_row = 1;
    key_col = 1;
    key_layer = 0;
    key_caps = false;
    secret_shown = false;
    key_focus_visible = true;
    edit_cursor = edit_buffer.size();
    settings.page = Page::Keyboard;
    s.StartTextInput();
    search_changed();
    dirty = true;
}

void App::finish_edit(bool commit) {
    if (edit_field >= 8 && edit_field <= 10) {
        const auto name = profile_clean_name(edit_buffer);
        const int field = edit_field;
        edit_buffer.clear();
        s.StopTextInput();
        settings.page = field == 8 ? Page::Profiles : Page::ProfileEdit;
        const auto* profile = edited_profile();
        if (commit && !name.empty() && field == 8) {
            // Each new profile starts on a picture and a colour of its own.
            const int number = int(profiles.profiles().size());
            const auto id = profiles.add(name, new_profile_restricted, number % int(profile_picture_count));
            if (id.empty()) status = "Could not save all settings. Check storage.";
            else {
                profile_editing = id;
                if (const auto* added = edited_profile()) profile_set_colour(*added, (number * 5 + 2) % 16);
                settings.page = Page::ProfileEdit;
                settings.focus = 0;
                if (new_profile_restricted) {
                    new_profile_flow = true; // an age, then the games waiting for a decision
                    open_profile_choose(ProfileChoice::Age);
                    dirty = true;
                    return;
                }
            }
        } else if (commit && !name.empty() && field == 9 && profile) {
            Profile changed = *profile;
            changed.name = name;
            if (!profiles.save(changed)) status = "Could not save all settings. Check storage.";
        } else if (commit && !name.empty() && field == 10 && profile) profile_set_ds_name(*profile, name);
        if (field != 8) settings.focus = profile && profile->owner() ? field - 9 : field - 8;
        dirty = true;
        return;
    }
    if (edit_field == 7) {
        if (commit) save_owner_name(edit_buffer);
        edit_buffer.clear();
        s.StopTextInput();
        settings.page = Page::Welcome;
        if (commit) welcome_enter(WelcomeHome);
        dirty = true;
        return;
    }
    if (edit_field == 11) {
        if (commit) {
            games_query = trimmed_query(edit_buffer);
            games_refilter();
        }
        edit_buffer.clear();
        s.StopTextInput();
        settings.page = Page::ProfileGames;
        dirty = true;
        return;
    }
    if (edit_field == 6) {
        // An empty search lists every code flat, which has its own use.
        if (commit) {
            cheat_query = trimmed_query(edit_buffer);
            show_cheat_view(CheatView::All);
        }
        edit_buffer.clear();
        s.StopTextInput();
        settings.page = Page::Cheats;
        dirty = true;
        return;
    }
    if (edit_field == 5) {
        if (commit) save_ds_setting("nickname", edit_buffer.empty() ? ds_owner_name() : edit_buffer);
        edit_buffer.clear();
        s.StopTextInput();
        settings.page = Page::DSControls;
        settings.focus = 4;
        dirty = true;
        return;
    }
    if (edit_field == 4) {
        search_preview.cancel();
        search_preview_waiting = search_preview_loading = false;
        search_matches.clear();
        if (commit) {
            search_query = trimmed_query(edit_buffer);
            apply_discovery();
        } else {
            settings.page = search_return;
            settings.focus = 0;
        }
        edit_buffer.clear();
        s.StopTextInput();
        dirty = true;
        return;
    }
    if (edit_field == 3) {
        if (commit && save_artwork_match(edit_buffer)) service_notice = "Matching title saved. Choose Fetch artwork.";
        edit_buffer.clear();
        s.StopTextInput();
        settings.page = Page::GameArtwork;
        settings.focus = 0;
        dirty = true;
        return;
    }
    if (edit_field == 2) {
        if (commit) {
            std::ostringstream payload;
            payload << "{\"network\":" << std::quoted(network_id) << ",\"password\":" << std::quoted(edit_buffer)
                    << '}';
            start_worker("device.connect", payload.str());
        }
        std::fill(edit_buffer.begin(), edit_buffer.end(), '\0');
        edit_buffer.clear();
        s.StopTextInput();
        if (welcome_detour) welcome_detour_end();
        else settings.page = Page::Network;
        settings.focus = 1;
        dirty = true;
        return;
    }
    // RetroAchievements sign-in: the same two steps as ScreenScraper's, in fields of their own. The password goes to
    // achievements.py on stdin and only the token the site gives back is kept.
    if (edit_field == 12 || edit_field == 13) {
        if (commit && edit_field == 12 && !edit_buffer.empty()) {
            achievements_name = edit_buffer;
            edit_buffer.clear();
            begin_edit(13);
            return;
        }
        if (commit && edit_field == 13 && !edit_buffer.empty() && !achievements_name.empty()) {
            std::ostringstream payload;
            payload << "{\"user\":" << std::quoted(achievements_name) << ",\"password\":" << std::quoted(edit_buffer)
                    << '}';
            if (worker > 0 && (worker_action == "index" || worker_action == "status")) stop_worker();
            if (worker > 0 && worker_action != "device.info" && worker_action != "device.update-auto")
                service_notice = "Wait for the current task to finish, then sign in.";
            else start_worker("achievements.login", payload.str());
            auto sent = payload.str();
            std::fill(sent.begin(), sent.end(), '\0');
        }
        std::fill(edit_buffer.begin(), edit_buffer.end(), '\0');
        edit_buffer.clear();
        s.StopTextInput();
        settings.page = Page::Achievements;
        settings.focus = 1;
        dirty = true;
        return;
    }
    // ScreenScraper sign-in: the username, then the password; the worker checks both with ScreenScraper before
    // keeping them (scraper.py login), and the password reaches it on stdin, never on its command line.
    if (commit && edit_field == 0 && !edit_buffer.empty()) {
        username = edit_buffer;
        edit_buffer.clear();
        begin_edit(1);
        return;
    }
    if (commit && edit_field == 1 && !edit_buffer.empty() && !username.empty()) {
        std::ostringstream payload;
        payload << "{\"ssid\":" << std::quoted(username) << ",\"sspassword\":" << std::quoted(edit_buffer) << '}';
        if (worker > 0 && worker_action != "scrape") stop_worker();
        if (worker > 0) service_notice = "Stop scraping before signing in";
        else {
            service_notice.clear();
            start_worker("login", payload.str());
        }
        auto sent = payload.str();
        std::fill(sent.begin(), sent.end(), '\0');
    }
    std::fill(edit_buffer.begin(), edit_buffer.end(), '\0');
    edit_buffer.clear();
    std::fill(password.begin(), password.end(), '\0');
    password.clear();
    s.StopTextInput();
    settings.page = Page::Scraper;
    settings.focus = 6;
    dirty = true;
}

void App::move_edit_cursor(int direction) {
    if (direction < 0 && edit_cursor) {
        edit_cursor--;
        while (edit_cursor && (static_cast<unsigned char>(edit_buffer[edit_cursor]) & 0xc0) == 0x80) edit_cursor--;
    } else if (direction > 0 && edit_cursor < edit_buffer.size()) {
        edit_cursor++;
        while (edit_cursor < edit_buffer.size() &&
               (static_cast<unsigned char>(edit_buffer[edit_cursor]) & 0xc0) == 0x80)
            edit_cursor++;
    }
    dirty = true;
}

void App::insert_text(const std::string& value) {
    if ((edit_field == 6 || edit_field == 8 || edit_field == 9) &&
        RoundedFont::codepoints(edit_buffer + value).size() > 16)
        return;
    if (edit_field == 5 || edit_field == 7 || edit_field == 10) {
        auto text = edit_buffer;
        text.insert(edit_cursor, value);
        auto points = RoundedFont::codepoints(text);
        size_t units = 0;
        for (auto c : points) units += c > 0xFFFF ? 2 : 1;
        if (units > 10) return;
    }
    if (edit_buffer.size() + value.size() <= 256 && value.find_first_of("\r\n\t") == std::string::npos) {
        edit_buffer.insert(edit_cursor, value);
        edit_cursor += value.size();
        search_changed();
    }
    dirty = true;
}

void App::erase_character() {
    if (!edit_cursor) return;
    size_t end = edit_cursor;
    move_edit_cursor(-1);
    edit_buffer.erase(edit_cursor, end - edit_cursor);
    search_changed();
    dirty = true;
}

void App::delete_character() {
    if (edit_cursor >= edit_buffer.size()) return;
    size_t begin = edit_cursor;
    move_edit_cursor(1);
    edit_buffer.erase(begin, edit_cursor - begin);
    edit_cursor = begin;
    search_changed();
    dirty = true;
}

// Settings > Library > RetroAchievements: row 0 the switch (A, Left/Right), row 1 the account.
void App::achievements_action(int code) {
    if (settings.focus == 0) {
        if (code != 0 && code != 1 && code != 2) return;
        preferences.achievements = code == 2 ? !preferences.achievements : code == 1;
        sound_intent(preferences.achievements ? MenuCue::Enable : MenuCue::Disable);
        remember_appearance();
        return;
    }
    if (code != 2) return;
    if (achievements_user.empty()) {
        begin_edit(12);
        return;
    }
    show_picker("RetroAchievements account", {"Change account", "Sign out"}, 0, [this](int n) {
        if (n == 0) begin_edit(12);
        else {
            if (worker > 0 && (worker_action == "index" || worker_action == "status")) stop_worker();
            if (worker > 0 && worker_action != "device.info" && worker_action != "device.update-auto")
                service_notice = "Wait for the current task to finish, then sign out.";
            else start_worker("achievements.logout");
        }
    });
}

void App::scraper_action() {
    if (settings.focus == 6) {
        if (screenscraper_user.empty()) {
            begin_edit(0);
            return;
        }
        show_picker("ScreenScraper account", {"Change account", "Sign out"}, 0, [this](int n) {
            if (n == 0) begin_edit(0);
            else {
                if (worker > 0 && worker_action != "scrape") stop_worker();
                if (worker > 0) service_notice = "Stop scraping before signing out";
                else start_worker("logout");
            }
        });
        return;
    }
    if (settings.focus == 5) {
        preferences.auto_scrape = !preferences.auto_scrape;
        if (!preferences.auto_scrape) auto_scrape_pending = false;
        remember_appearance();
        return;
    }
    if (settings.focus == 3) {
        preferences.game_info = !preferences.game_info;
        remember_appearance();
        return;
    }
    if (settings.focus == 4) {
        open_setting_picker();
        return;
    }
    if (settings.focus == 2) {
        review_artwork();
        return;
    }
    if (settings.focus == 0) {
        open_setting_picker();
        return;
    }
    if (worker > 0 && (worker_action == "index" || worker_action == "device.info" || worker_action == "status"))
        stop_worker();
    if (worker > 0) {
        std::ofstream(o.data / "scraper/cancel").put('\n');
        service_notice = "Stopping after this request";
        return;
    }
    start_worker("scrape");
}
