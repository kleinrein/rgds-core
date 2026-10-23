#pragma once
// Text editing fields and the scraper action.
// Members of App in main.cpp; included once, right after the class.

void App::begin_edit(int field) {
    if (field == 4) search_return = settings.page;
    edit_field = field;
    edit_buffer = field == 5   ? (ds_settings.count("nickname") ? ds_settings["nickname"] : "RG DS Core")
                  : field == 4 ? search_query
                  : field == 6 ? cheat_query
                  : field == 0 ? username
                  : field == 1 ? password
                  : field == 3 ? artwork_match
                               : "";
    key_row = 1;
    key_col = 1;
    key_layer = 0;
    key_caps = false;
    edit_cursor = edit_buffer.size();
    settings.page = Page::Keyboard;
    s.StartTextInput();
    search_changed();
    dirty = true;
}

void App::finish_edit(bool commit) {
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
        if (commit) save_ds_setting("nickname", edit_buffer.empty() ? std::string("RG DS Core") : edit_buffer);
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
        settings.page = Page::Network;
        settings.focus = 1;
        dirty = true;
        return;
    }
    if (commit) {
        if (edit_field == 0) username = edit_buffer;
        else password = edit_buffer;
    }
    std::fill(edit_buffer.begin(), edit_buffer.end(), '\0');
    edit_buffer.clear();
    s.StopTextInput();
    settings.page = Page::Scraper;
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
    if (edit_field == 6 && RoundedFont::codepoints(edit_buffer + value).size() > 16) return;
    if (edit_field == 5) {
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

void App::scraper_action() {
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
