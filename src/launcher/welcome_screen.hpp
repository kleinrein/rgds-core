#pragma once
// Setup: the wizard a new card opens with, and Settings > System > Run setup again. Language, colour, name, Home and
// animations, time, Wi-Fi, games. Every choice goes to the normal settings files as it is made; setup.ini only keeps
// the step, so a power cut or Use as USB drive comes back to the same place.
// Members of App in main.cpp; included once, right after the class.

// A card that has been used before never sees this: an update must not interrupt anyone. Called before anything is
// read or written, so these files can only come from an earlier run.
bool App::welcome_needed() {
    const auto saved = fields(o.data / "setup.ini");
    if (saved.count("done") && saved.at("done") == "1") return false;
    if (saved.count("step")) {
        welcome_step = std::clamp(std::atoi(saved.at("step").c_str()), 0, welcome_steps - 1);
        return true;
    }
    std::error_code ec;
    for (const char* name : {"preferences.ini", "ui.ini", "system.ini", "pictochat.ini", "ds.ini", "device/status.ini"})
        if (fs::exists(o.data / name, ec) && !ec) {
            welcome_save(true);
            return false;
        }
    return true;
}

void App::welcome_save(bool done) {
    if (o.data.empty()) return;
    try {
        fs::create_directories(o.data);
        const auto path = o.data / "setup.ini", pending = fs::path(path.string() + ".pending");
        std::ofstream out(pending);
        out << (done ? std::string("done=1\n") : "step=" + std::to_string(welcome_step) + "\n");
        out.close();
        if (out) fs::rename(pending, path);
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
    }
}

// Run setup again: the same steps with the choices already made, ending where it was opened.
void App::open_welcome() {
    cancel_navigation();
    welcome_active = true;
    welcome_fresh = false;
    welcome_return = settings.page;
    settings.page = Page::Welcome;
    welcome_preset = 0; // every step again
    welcome_import_scan();
    welcome_enter(0);
}

void App::welcome_begin() {
    settings.page = Page::Welcome;
    welcome_active = true;
    welcome_fresh = true;
    welcome_return = Page::Library;
    if (welcome_step == 0 && !preferences.show_systems) {
        // A new card has every system's folder: unless the Systems step says otherwise, its games show from the first copy.
        preferences.show_systems = true;
        remember_appearance();
        refresh_other_systems();
    }
    welcome_import_scan();
    welcome_apply_preset();
    welcome_enter(welcome_step);
}

// RG DS Flasher's Set up step: the language, time zone and name asked on the computer, put in userdata by the first
// boot's handoff (os/buildroot/rootfs/usr/libexec/rgds/handoff). They are applied as if chosen here, and their steps
// are skipped; Back still reaches them. Applied once: the file becomes flasher-setup.ini.done, which a setup that
// comes back after a restart reads only to skip the same steps. Run setup again asks everything.
void App::welcome_apply_preset() {
    std::error_code ec;
    const auto path = o.data / "flasher-setup.ini", done = fs::path(path.string() + ".done");
    if (o.data.empty()) return;
    if (!fs::exists(path, ec)) {
        if (!fs::exists(done, ec)) return;
        auto values = fields(done);
        if (!values["language"].empty()) welcome_preset |= 1u << WelcomeLanguage;
        if (!values["timezone"].empty()) welcome_preset |= 1u << WelcomeTime;
        if (!values["name"].empty()) welcome_preset |= 1u << WelcomeName;
        return;
    }
    auto values = fields(path);
    fs::rename(path, done, ec);
    const auto& languages = locale.languages();
    const std::string language = values["language"];
    if (std::any_of(languages.begin(), languages.end(), [&](const auto& l) { return l.first == language; })) {
        preferences.language = language;
        remember_appearance();
        welcome_language_defaults(); // time and date formats for the language, and its zone unless one follows
        welcome_preset |= 1u << WelcomeLanguage;
    }
    const std::string zone = values["timezone"];
    if (std::find(timezones.begin(), timezones.end(), zone) != timezones.end()) {
        system_preferences.timezone = zone;
        try {
            save_system_preferences(o.system_preferences, system_preferences);
            apply_timezone();
            welcome_preset |= 1u << WelcomeTime;
        } catch (const std::exception& e) {
            std::cerr << e.what() << std::endl;
        }
    }
    if (!values["name"].empty()) {
        save_owner_name(values["name"]);
        welcome_preset |= 1u << WelcomeName;
    }
}

void App::welcome_enter(int step, ViewTravel route) {
    welcome_step = std::clamp(step, 0, welcome_steps - 1);
    // Bring over is a step only when there is something to bring; what the flasher already answered is skipped too.
    const auto skipped = [&](int s) {
        return (s == WelcomeImport && !welcome_import_shown()) || (welcome_preset >> s & 1u);
    };
    while (skipped(welcome_step)) {
        const int next = welcome_step + (route == ViewTravel::Back ? -1 : 1);
        if (next < 0 || next >= welcome_steps) break;
        welcome_step = next;
    }
    welcome_focus = welcome_next;
    if (welcome_step == WelcomeLanguage) {
        const auto& languages = locale.languages();
        for (size_t i = 0; i < languages.size(); i++)
            if (languages[i].first == preferences.language) welcome_focus = int(i);
    } else if (welcome_step == WelcomeColour) welcome_focus = preferences.accent >= 0 ? preferences.accent : 16;
    else if (welcome_step == WelcomeName) welcome_focus = 0;
    else if (welcome_step == WelcomeImport && device_status["import_found"] == "1" &&
             device_status["import_state"] != "done")
        welcome_focus = 0;
    else if (welcome_step == WelcomeLibrary) welcome_focus = preferences.show_systems ? 1 : 0;
    else if (welcome_step == WelcomeWifi && device_status["wifi_connected"] != "1" &&
             device_status["wifi_supported"] != "0")
        welcome_focus = 0;
    if (welcome_fresh) welcome_save(false);
    if (view_transitions_enabled()) start_view_motion(s.GetTicks(), route);
    dirty = true;
}

void App::welcome_finish() {
    welcome_active = false;
    if (welcome_fresh) welcome_save(true);
    if (!welcome_fresh) {
        settings.page = welcome_return;
        settings.focus = o.rgds_os ? 8 : 9;
    } else {
        settings.page = Page::Library;
        home_menu = true;
        selected_at = page_at = s.GetTicks();
    }
    welcome_fresh = false;
    dirty = true;
}

void App::welcome_detour_end() {
    welcome_detour = false;
    settings.page = Page::Welcome;
    dirty = true;
}

// The first choice sets the rest up for where the language is mostly spoken; the Time step shows them to change.
void App::welcome_language_defaults() {
    static const std::map<std::string, std::tuple<const char*, int, int>> regions = {
        {"en-US", {"America/New_York", 1, 1}},  {"id-ID", {"Asia/Jakarta", 0, 0}},
        {"en-GB", {"Europe/London", 0, 0}},     {"da-DK", {"Europe/Copenhagen", 0, 2}},
        {"de-DE", {"Europe/Berlin", 0, 2}},     {"es-ES", {"Europe/Madrid", 0, 0}},
        {"fr-FR", {"Europe/Paris", 0, 0}},      {"it-IT", {"Europe/Rome", 0, 0}},
        {"nl-NL", {"Europe/Amsterdam", 0, 4}},  {"nb-NO", {"Europe/Oslo", 0, 2}},
        {"nn-NO", {"Europe/Oslo", 0, 2}},       {"pl-PL", {"Europe/Warsaw", 0, 2}},
        {"pt-BR", {"America/Sao_Paulo", 0, 0}}, {"pt-PT", {"Europe/Lisbon", 0, 0}},
        {"fi-FI", {"Europe/Helsinki", 0, 2}},   {"sv-SE", {"Europe/Stockholm", 0, 3}},
        {"vi-VN", {"Asia/Ho_Chi_Minh", 0, 0}},  {"tr-TR", {"Europe/Istanbul", 0, 2}},
        {"ru-RU", {"Europe/Moscow", 0, 2}},     {"uk-UA", {"Europe/Kyiv", 0, 2}},
        {"ja-JP", {"Asia/Tokyo", 0, 3}},        {"zh-CN", {"Asia/Shanghai", 0, 3}},
        {"ko-KR", {"Asia/Seoul", 0, 3}},
    };
    const auto found = regions.find(preferences.language);
    // a zone RG DS Flasher was given stays: its step is skipped, so nothing would show the change
    if (!welcome_fresh || found == regions.end() || (welcome_preset >> WelcomeTime & 1u)) return;
    system_preferences.timezone = std::get<0>(found->second);
    system_preferences.time_format = std::get<1>(found->second);
    system_preferences.date_format = std::get<2>(found->second);
    try {
        save_system_preferences(o.system_preferences, system_preferences);
        apply_timezone();
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
    }
}

// One name for this handheld: DS games read it from ds.ini (a game's own nickname still wins), PictoChat from its profile.
std::string App::ds_owner_name() const {
    auto saved = fields(o.player / "ds.ini")["nickname"];
    return saved.empty() ? "RG DS Core" : saved;
}

void App::save_owner_name(std::string name) {
    while (!name.empty() && name.back() == ' ') name.pop_back();
    if (name.empty()) return;
    try {
        fs::create_directories(o.player);
        if (!ds_ini_set(o.player / "ds.ini", "nickname", name)) status = "Could not save DS settings.";
    } catch (const std::exception&) {
        status = "Could not save DS settings.";
    }
    chat.load_profile(o.player.empty() ? fs::path() : o.player / "pictochat.ini", preferences.accent);
    chat.name = name;
    chat_save_profile();
    dirty = true;
}

size_t App::welcome_game_count() const {
    return size_t(
        std::count_if(all_games.begin(), all_games.end(), [](const Game& g) { return !is_tool_entry(g.path); }));
}

std::vector<App::WelcomeItem> App::welcome_items() {
    std::vector<WelcomeItem> items;
    auto add = [&](int id, SDL_Rect box) { items.push_back({id, box}); };
    // A row of choices: x 176 to 616.
    auto row = [&](int first, int count, int y) {
        const int gap = 8, width = (440 - gap * (count - 1)) / count;
        for (int i = 0; i < count; i++) add(first + i, {176 + i * (width + gap), y, width, 52});
    };
    switch (welcome_step) {
    case WelcomeLanguage:
        // Every language at once, nothing to scroll: three columns of eight.
        for (int i = 0; i < int(locale.languages().size()); i++)
            add(i, {20 + (i / 8) * 204, 60 + (i % 8) * 44, 192, 38});
        break;
    case WelcomeColour:
        for (int i = 0; i < 16; i++) add(i, {28 + (i % 8) * 74, 72 + (i / 8) * 74, 66, 66});
        add(16, {28, 234, 584, 52});
        add(welcome_mode, {28, 306, 288, 52});
        add(welcome_mode + 1, {324, 306, 288, 52});
        break;
    case WelcomeName: add(0, {60, 150, 520, 76}); break;
    case WelcomeImport:
        if (device_status["import_found"] == "1" && device_status["import_state"] != "copying" &&
            device_status["import_state"] != "done") {
            add(0, {28, 252, 584, 72});
            if (std::atoll(device_status["import_games_copy"].c_str()) > 0) add(1, {28, 336, 584, 72});
        }
        break;
    case WelcomeLibrary:
        add(0, {28, 70, 284, 340});
        add(1, {328, 70, 284, 340});
        break;
    case WelcomeHome:
        row(welcome_layout, 2, 72);
        row(welcome_ground, 2, 150);
        row(welcome_motion, 5, 228);
        row(welcome_face, 2, 306);
        break;
    case WelcomeTime:
        add(0, {28, 96, 584, 72});
        row(welcome_clock, 2, 196);
        break;
    case WelcomeWifi: add(0, {60, 190, 520, 72}); break;
    case WelcomeGames:
        add(0, {28, 72, 584, 72});
        add(1, {28, 156, 584, 72});
        break;
    default: break;
    }
    if (welcome_step > 0) add(welcome_back, {12, 432, 160, 40});
    add(welcome_next, {468, 432, 160, 40});
    if (welcome_step != WelcomeDone) add(welcome_skip, {452, 10, 176, 40});
    return items;
}

// The D-pad goes to the nearest thing that way, so every step's layout just works.
void App::welcome_move(int code) {
    const auto items = welcome_items();
    const auto current =
        std::find_if(items.begin(), items.end(), [&](const WelcomeItem& i) { return i.id == welcome_focus; });
    if (current == items.end()) {
        welcome_focus = welcome_next;
        return;
    }
    const int cx = current->box.x + current->box.w / 2, cy = current->box.y + current->box.h / 2;
    int best = welcome_focus, score = INT_MAX;
    for (const auto& item : items) {
        if (item.id == welcome_focus) continue;
        const int dx = item.box.x + item.box.w / 2 - cx, dy = item.box.y + item.box.h / 2 - cy;
        const int along = code == 0 ? -dx : code == 1 ? dx : code == 8 ? -dy : dy;
        // Sideways distance counts from the edges: anything lined up with this item is straight ahead of it.
        const bool level = code == 0 || code == 1;
        const int low = level ? item.box.y - current->box.y - current->box.h
                              : item.box.x - current->box.x - current->box.w,
                  high = level ? current->box.y - item.box.y - item.box.h : current->box.x - item.box.x - item.box.w;
        const int across = std::max({0, low, high});
        if (along <= 0 || across > along * 3 || (level && across > 0)) continue; // sideways stays on its row
        if (along + across * 2 < score) {
            score = along + across * 2;
            best = item.id;
        }
    }
    welcome_focus = best;
    if (welcome_step == WelcomeColour && welcome_focus <= 16) {
        preferences.accent = welcome_focus == 16 ? -1 : welcome_focus;
        select_theme();
    }
    dirty = true;
}

void App::welcome_choose_motion(int choice) {
    preferences.animations = choice != 0;
    preferences.view_animations = choice >= 2;
    if (choice >= 2)
        preferences.view_style = choice == 3 ? ViewStyle::Push : choice == 4 ? ViewStyle::Zoom : ViewStyle::Spring;
    remember_appearance();
    if (preferences.view_animations) start_view_motion(s.GetTicks());
}

void App::welcome_activate(int id) {
    welcome_focus = id;
    if (id == welcome_next) {
        if (welcome_step == WelcomeDone) welcome_finish();
        else {
            if (welcome_step == WelcomeColour) remember_appearance();
            welcome_enter(welcome_step + 1);
        }
        return;
    }
    if (id == welcome_back) {
        if (welcome_step == WelcomeColour) remember_appearance();
        welcome_enter(welcome_step - 1, ViewTravel::Back);
        return;
    }
    if (id == welcome_skip) {
        if (welcome_step == WelcomeColour) remember_appearance();
        welcome_enter(WelcomeDone);
        return;
    }
    switch (welcome_step) {
    case WelcomeLanguage:
        preferences.language = locale.languages().at(size_t(id)).first;
        remember_appearance();
        welcome_language_defaults();
        welcome_enter(WelcomeImport);
        break;
    case WelcomeImport:
        if (worker > 0 && worker_action == "device.info") stop_worker();
        // the library index gives way to a device request (start_worker), and runs again with the next scan
        if (worker > 0 && worker_action != "index") break;
        device_status["import_state"] = "copying";
        device_status["import_progress"] = "0";
        welcome_import_rescan = true;
        start_worker(id == 1 ? "device.import-games" : "device.import-apply");
        break;
    case WelcomeColour:
        if (id <= 16) {
            preferences.accent = id == 16 ? -1 : id;
            remember_appearance();
            welcome_enter(WelcomeName);
        } else {
            preferences.dark = id == welcome_mode + 1;
            remember_appearance();
        }
        break;
    case WelcomeName: begin_edit(7); break;
    case WelcomeHome:
        if (id < welcome_ground) {
            preferences.home_layout = id == welcome_layout ? 1 : 0;
            home_layout_changed();
        } else if (id < welcome_motion) preferences.background = id == welcome_ground ? 1 : 0;
        else if (id < welcome_face) {
            welcome_choose_motion(id - welcome_motion);
            return;
        } else {
            const bool pixel = id != welcome_face;
            if (!pixel) check_fonts();
            if (!pixel && !font_ready) status = "ROUNDED FONT UNAVAILABLE";
            else preferences.pixel = pixel;
        }
        remember_appearance();
        break;
    case WelcomeTime:
        if (id == 0) {
            welcome_detour = true;
            settings.page = Page::Timezone;
            const auto found = std::find(timezones.begin(), timezones.end(), system_preferences.timezone);
            settings.focus = found == timezones.end() ? 0 : int(found - timezones.begin());
        } else save_time_format(id - welcome_clock);
        break;
    case WelcomeWifi:
        if (worker > 0 && worker_action == "device.info") stop_worker();
        if (worker > 0 || device_status["wifi_supported"] == "0") break;
        if (device_status["wifi"] != "1") start_worker("device.wifi-on");
        else {
            welcome_detour = true;
            settings.page = Page::Wifi;
            settings.focus = 0;
            start_worker("device.scan");
        }
        break;
    case WelcomeGames:
        if (id == 0) {
            if (worker > 0 && worker_action == "device.info") stop_worker();
            if (worker > 0) break;
            if (device_status["usb_supported"] == "0") notify("USB data transfer is not available on this handheld.");
            else start_worker(device_status["usb"] == "1" ? "device.usb-drive" : "device.usb-on");
        } else if (id == 1) {
            if (device_status["wifi_connected"] == "1") show_sharing_picker();
            else notify("Connect to Wi-Fi first");
        }
        break;
    case WelcomeLibrary:
        preferences.show_systems = id == 1;
        remember_appearance();
        refresh_other_systems();
        summary_ready = false;
        library_summary.request(o.data, all_games);
        welcome_enter(WelcomeTime);
        break;
    default: break;
    }
    dirty = true;
}

void App::welcome_action(int code) {
    if (code == 0 || code == 1 || code == 8 || code == 9) welcome_move(code);
    else if (code == 2) welcome_activate(welcome_focus);
    else if (code == 3 && welcome_step > 0) welcome_activate(welcome_back);
    else if (code == 4) welcome_activate(welcome_next);
    else if (code == 11 && welcome_step != WelcomeDone) welcome_activate(welcome_skip);
    dirty = true;
}

void App::welcome_touch(int x, int y) {
    if (!point_in(SDL_Rect{0, 0, 640, 480}, x, y)) return;
    for (const auto& item : welcome_items())
        if (point_in(item.box, x, y) && point_in(item.box, down_x, down_y)) {
            emit_sound(MenuCue::Open);
            welcome_activate(item.id);
            return;
        }
}

SDL_Rect App::welcome_item_box(int id) {
    for (const auto& item : welcome_items())
        if (item.id == id) return item.box;
    return {};
}

void App::welcome_focus_ring(SDL_Renderer* r, const SDL_Rect& b, int radius) {
    round_rect(r, b.x - 5, b.y - 5, b.w + 10, b.h + 10, radius + 5, "accent");
    round_rect(r, b.x - 2, b.y - 2, b.w + 4, b.h + 4, radius + 2, "background");
}

void App::welcome_chip(SDL_Renderer* r, int y, int id, const std::string& text, bool selected, bool translate) {
    const SDL_Rect box = welcome_item_box(id);
    const bool focused = welcome_focus == id,
               pressed = pointer_down && point_in(box, contact_x, contact_y) && point_in(box, down_x, down_y);
    int x = box.x, top = y + box.y;
    move_element(uint32_t(id), x, top, box.w, box.h, focused);
    if (focused) welcome_focus_ring(r, {x, top, box.w, box.h}, 10);
    raised_pill(r, x, top, box.w, box.h, 10, theme.at(selected ? "accent" : "panel"), pressed ? 1 : 0);
    const std::string value = translate ? locale.text(text) : text;
    // A long name takes the smaller size rather than losing its end.
    const int size = label_width(value, 20, false) > box.w - 12 ? 16 : 20,
              width = std::min(box.w - 12, label_width(value, size, false));
    raw_label(r, value, x + (box.w - width) / 2, centred_label_y(value, size, top + box.h / 2 + (pressed ? 2 : 0)),
              size, selected ? "on_accent" : "ink", box.w - 12);
}

// A wide row: an icon, its name and what it is set to.
void App::welcome_row(SDL_Renderer* r, int y, int id, SettingsIcon icon, const std::string& title,
                      const std::string& value) {
    const SDL_Rect box = welcome_item_box(id);
    const bool focused = welcome_focus == id,
               pressed = pointer_down && point_in(box, contact_x, contact_y) && point_in(box, down_x, down_y);
    int x = box.x, top = y + box.y;
    move_element(uint32_t(id), x, top, box.w, box.h, focused);
    if (focused) welcome_focus_ring(r, {x, top, box.w, box.h}, 12);
    raised_pill(r, x, top, box.w, box.h, 12, theme.at(pressed ? "focus" : "panel"), pressed ? 1 : 0);
    ui_icon(r, icon, x + 38, top + box.h / 2, 30, "accent");
    const std::string name = locale.text(title);
    const int room = box.w - 72 - 20;
    if (value.empty()) raw_label(r, name, x + 72, centred_label_y(name, 24, top + box.h / 2), 24, "ink", room);
    else {
        raw_label(r, name, x + 72, top + box.h / 2 - 28, 24, "ink", room);
        raw_label(r, value, x + 72, top + box.h / 2 + 4, 20, "muted", room);
    }
}

void App::welcome_system_icon(SDL_Renderer* r, const std::string& id, int x, int y, int size) {
    if (preferences.rich_icons && media.draw(r, asset_path("systems/" + id + ".bmp"), {x, y, size, size}, false))
        return;
    draw_system_pixel(s, r, id, x + size / 2, y + size / 2, size, theme.at("accent"));
}

// One of the two big answers on the Systems step.
void App::welcome_library_card(SDL_Renderer* r, int y, int id, const std::string& title, const std::string& note) {
    const SDL_Rect box = welcome_item_box(id);
    const bool focused = welcome_focus == id, chosen = preferences.show_systems == (id == 1),
               pressed = pointer_down && point_in(box, contact_x, contact_y) && point_in(box, down_x, down_y);
    int x = box.x, top = y + box.y;
    move_element(uint32_t(id), x, top, box.w, box.h, focused);
    if (focused) welcome_focus_ring(r, {x, top, box.w, box.h}, 14);
    raised_pill(r, x, top, box.w, box.h, 14, theme.at(pressed ? "focus" : "panel"), pressed ? 1 : 0);
    if (id == 0) welcome_system_icon(r, "nds", x + box.w / 2 - 64, top + 28, 128);
    else {
        static const std::array<const char*, 6> ids = {"nds", "gba", "snes", "md", "psx", "n64"};
        for (int i = 0; i < 6; i++)
            welcome_system_icon(r, ids[size_t(i)], x + 34 + (i % 3) * 74, top + 24 + (i / 3) * 70, 68);
    }
    const std::string name = locale.text(title);
    const int width = std::min(box.w - 32, label_width(name, 28, false));
    raw_label(r, name, x + (box.w - width) / 2, top + 176, 28, chosen ? "accent" : "ink", box.w - 32);
    wrapped(r, note, x + 20, top + 224, 20, box.w - 40, 4);
    if (chosen) ui_icon(r, SettingsIcon::Check, x + box.w - 30, top + 30, 28, "accent");
}

void App::welcome_top_heading(SDL_Renderer* r, int y, const std::string& title, const std::string& body) {
    boot_caption(r, title, y + 72, 36, "ink");
    centered_wrapped(r, locale.text(body), y + 128, 20, 540, 3, "muted");
}

void App::welcome_top(SDL_Renderer* r, int y) {
    rect(r, 0, y, 640, 480, "background");
    if (welcome_step == WelcomeHome) {
        // Home itself, as it will be on the lower screen: it is what is being chosen.
        const Page page = settings.page;
        const bool home = home_menu;
        settings.page = Page::Library;
        home_menu = true;
        bottom(r, y);
        settings.page = page;
        home_menu = home;
        motion_hits.clear();
        return;
    }
    pattern(r, y + 48, 432);
    status_bar(r, y);
    switch (welcome_step) {
    case WelcomeLanguage:
        boot_emblem(r, y + 88);
        boot_caption(r, "Welcome!", y + 236, 36, "ink");
        centered_wrapped(r, locale.text("Let's get your handheld ready. It takes about a minute."), y + 296, 20, 520, 2,
                         "muted");
        break;
    case WelcomeImport: welcome_import_top(r, y); break;
    case WelcomeColour: {
        welcome_top_heading(r, y, "Pick your colour", "The menus take it on. You can change it whenever you like.");
        const Color accent = theme.at("accent");
        accent_swatch(r, 320, y + 280, 56, accent);
        const std::string name = accent_name(preferences.accent);
        boot_caption(r, name, y + 352, 24, "ink");
        break;
    }
    case WelcomeName: {
        welcome_top_heading(r, y, "What's your name?",
                            "PictoChat shows it to people nearby, and DS games use it as the owner's name.");
        const std::string name = ds_owner_name();
        round_rect(r, 120, y + 262, 400, 96, 16, "panel");
        const int width = label_width(name, 36, false);
        raw_label(r, name, 320 - std::min(width, 360) / 2, y + 288, 36, "accent", 360);
        break;
    }
    case WelcomeLibrary: {
        welcome_top_heading(r, y, "What do you want to play?", "You can change this later in Settings > Library.");
        if (!preferences.show_systems) welcome_system_icon(r, "nds", 256, y + 220, 128);
        else {
            // 3DS only where its engine is (the session passes its folder then): the PSP takes its place otherwise
            const bool ctr = o.preview || std::any_of(o.system_roots.begin(), o.system_roots.end(),
                                                      [](const auto& root) { return root.first == "3ds"; });
            const std::array<const char*, 8> ids = {"nds", "gba", "gb",  "snes",
                                                    "md",  "psx", "n64", ctr ? "3ds" : "psp"};
            for (int i = 0; i < 8; i++)
                welcome_system_icon(r, ids[size_t(i)], 148 + (i % 4) * 88, y + 204 + (i / 4) * 88, 80);
        }
        break;
    }
    case WelcomeTime:
        boot_caption(r, "Is this the right time?", y + 60, 28, "ink");
        home_column_top(r, y);
        break;
    case WelcomeWifi: {
        const bool connected = device_status["wifi_connected"] == "1";
        welcome_top_heading(r, y, connected ? "You're connected" : "Connect to Wi-Fi",
                            "For box art, updates and PictoChat. Everything else works without it.");
        ui_icon(r, SettingsIcon::Wifi, 320, y + 278, 72, connected ? "accent" : "muted");
        if (connected) boot_caption(r, device_status["connection"], y + 340, 24, "ink");
        else if (!device_status["wifi_joining"].empty())
            boot_caption(r, locale.text("Connecting to") + " " + device_status["wifi_joining"] + "…", y + 340, 20,
                         "muted");
        break;
    }
    case WelcomeGames: {
        boot_caption(r, "Add your games", y + 64, 36, "ink");
        const size_t count = welcome_game_count();
        const std::string found = locale.text(std::to_string(count) + (count == 1 ? " game" : " games"));
        boot_caption(r, library_ready ? found : locale.text("Searching…"), y + 118, 28, count ? "accent" : "muted");
        round_rect(r, 36, y + 172, 568, 136, 14, "panel");
        wrapped(
            r,
            "Connect a USB-C cable and choose Use as USB drive, or put this card in a computer. Open the RGDS drive and copy games into the folders in roms: nds, gba, gb, gbc, nes, snes, md and more.",
            60, y + 192, 20, 520, 5);
        if (device_status["sharing"] == "1" && !device_status["sharing_address"].empty())
            boot_caption(r, device_status["sharing_address"], y + 330, 20, "accent");
        break;
    }
    default: {
        welcome_top_heading(r, y, "You're all set!",
                            "Play at a comfortable volume, and give your eyes and hands a break.");
        boot_emblem(r, y + 230);
        break;
    }
    }
}

void App::welcome_bottom(SDL_Renderer* r, int y) {
    rect(r, 0, y, 640, 480, "background");
    pattern(r, y, 480, true);
    static const std::array<const char*, welcome_steps> titles = {
        "Language", "Bring over", "Accent colour", "Your name", "Home", "Systems", "Time", "Wi-Fi", "Games", "Done"};
    label(r, titles[size_t(welcome_step)], 24, y + 18, 24, "ink", 420);
    auto caption = [&](const std::string& text, int row) {
        const std::string value = locale.text(text);
        raw_label(r, value, 28, centred_label_y(value, 16, y + row + 26), 16, "muted", 140);
    };
    switch (welcome_step) {
    case WelcomeLanguage: {
        const auto& languages = locale.languages();
        for (int i = 0; i < int(languages.size()); i++)
            welcome_chip(r, y, i, languages[size_t(i)].second, languages[size_t(i)].first == preferences.language,
                         false);
        break;
    }
    case WelcomeImport: welcome_import_bottom(r, y); break;
    case WelcomeColour:
        for (int i = 0; i < 16; i++) {
            const SDL_Rect b = welcome_item_box(i);
            if (welcome_focus == i) welcome_focus_ring(r, {b.x, y + b.y, b.w, b.h}, 12);
            fill_round(r, b.x, y + b.y, b.w, b.h, 12, ds_colours[size_t(i)].rgb);
            if (preferences.accent == i)
                ui_icon(r, SettingsIcon::Check, b.x + b.w / 2, y + b.y + b.h / 2, 30, "on_accent");
        }
        welcome_chip(r, y, 16, accent_name(-1), preferences.accent < 0);
        welcome_chip(r, y, welcome_mode, mode_names[0], !preferences.dark);
        welcome_chip(r, y, welcome_mode + 1, mode_names[1], preferences.dark);
        break;
    case WelcomeName: {
        welcome_row(r, y, 0, SettingsIcon::Type, ds_owner_name(), "");
        break;
    }
    case WelcomeHome: {
        const bool subtle = preferences.animations && !preferences.view_animations,
                   spring = preferences.animations && preferences.view_animations &&
                            preferences.view_style == ViewStyle::Spring;
        caption("Home layout", 72);
        welcome_chip(r, y, welcome_layout, "Column", preferences.home_layout == 1);
        welcome_chip(r, y, welcome_layout + 1, "Carousel", preferences.home_layout != 1);
        caption("Background", 150);
        welcome_chip(r, y, welcome_ground, "Grid", preferences.background == 1);
        welcome_chip(r, y, welcome_ground + 1, "Plain", preferences.background != 1);
        caption("Animations", 228);
        welcome_chip(r, y, welcome_motion, "Off", !preferences.animations);
        welcome_chip(r, y, welcome_motion + 1, "Subtle", subtle);
        welcome_chip(r, y, welcome_motion + 2, "Spring", spring);
        welcome_chip(r, y, welcome_motion + 3, "Push",
                     preferences.animations && preferences.view_animations &&
                         preferences.view_style == ViewStyle::Push);
        welcome_chip(r, y, welcome_motion + 4, "Zoom",
                     preferences.animations && preferences.view_animations &&
                         preferences.view_style == ViewStyle::Zoom);
        caption("Typeface", 306);
        welcome_chip(r, y, welcome_face, typeface_names[0], !preferences.pixel);
        welcome_chip(r, y, welcome_face + 1, typeface_names[1], preferences.pixel);
        break;
    }
    case WelcomeLibrary:
        welcome_library_card(r, y, 0, "DS only", "A clean library with only DS games.");
        welcome_library_card(r, y, 1, "All systems", "DS, Game Boy, SNES, PlayStation and more, sorted by system.");
        break;
    case WelcomeTime:
        welcome_row(r, y, 0, SettingsIcon::Clock, "Timezone", timezone_summary());
        caption("Time format", 196);
        welcome_chip(r, y, welcome_clock, time_format_names[0], system_preferences.time_format == 0);
        welcome_chip(r, y, welcome_clock + 1, time_format_names[1], system_preferences.time_format == 1);
        break;
    case WelcomeWifi: {
        const bool supported = device_status["wifi_supported"] != "0", radio = device_status["wifi"] == "1";
        welcome_row(r, y, 0, SettingsIcon::Wifi,
                    !supported ? "Unavailable"
                    : radio    ? "Available networks"
                               : "Turn on Wi-Fi",
                    !supported                               ? device_status["wifi_reason"]
                    : device_status["wifi_connected"] == "1" ? device_status["connection"]
                                                             : "");
        break;
    }
    case WelcomeGames: {
        welcome_row(r, y, 0, SettingsIcon::Load, "Use as USB drive",
                    locale.text(device_status["usb"] == "1" ? "Connect the handheld to a computer first."
                                                            : "Turn on USB data transfer"));
        welcome_row(r, y, 1, SettingsIcon::Wifi, "Network file sharing",
                    locale.text(device_status["wifi_connected"] != "1"    ? "Connect to Wi-Fi first"
                                : device_status["sharing"] != "1"         ? "Off"
                                : device_status["sharing_mode"] == "open" ? "Open"
                                                                          : "Password protected"));
        break;
    }
    default: {
        auto tip = [&](SettingsIcon icon, const std::string& text, int row) {
            ui_icon(r, icon, 56, y + row + 24, 30, "accent");
            wrapped(r, text, 88, y + row + 4, 20, 524, 2);
        };
        tip(SettingsIcon::Controller, "In a game, the Anbernic button opens saves, cheats and settings.", 76);
        tip(SettingsIcon::Sleep, "Close the lid to sleep. Your game waits for you.", 176);
        tip(SettingsIcon::Cartridge, "Copy more games any time. They show up by themselves.", 276);
        break;
    }
    }
    if (welcome_step != WelcomeDone) {
        const SDL_Rect b = welcome_item_box(welcome_skip);
        if (welcome_focus == welcome_skip) welcome_focus_ring(r, {b.x, y + b.y, b.w, b.h}, b.h / 2);
        hotkey_pill(r, b.x, y + b.y, b.w, b.h, "Skip setup", KeyLook::Panel,
                    pointer_down && point_in(b, contact_x, contact_y) && point_in(b, down_x, down_y), "X");
    }
    for (int i = 0; i < welcome_steps; i++) {
        const int cx = 320 - (welcome_steps - 1) * 8 + i * 16;
        fill_round(r, cx - 4, y + 448, 8, 8, 4, theme.at(i == welcome_step ? "accent" : "slot"));
    }
    if (welcome_step > 0) {
        if (welcome_focus == welcome_back) welcome_focus_ring(r, {12, y + 432, 160, 40}, 12);
        footer_button(r, 12, y + 432, 160, "Back", "B", false, footer_pressed(true));
    }
    if (welcome_focus == welcome_next) welcome_focus_ring(r, {468, y + 432, 160, 40}, 12);
    const bool not_now = welcome_step == WelcomeImport && device_status["import_found"] == "1" &&
                         device_status["import_state"] != "done" && device_status["import_state"] != "copying";
    footer_button(r, 468, y + 432, 160,
                  welcome_step == WelcomeDone ? "Start"
                  : not_now                   ? "Not now"
                                              : "Next",
                  welcome_focus == welcome_next ? "A" : "", true, footer_pressed(false));
    if (picker_open || picker_closing()) render_picker(r, y);
}

// Bring over: the scan (device_services import-scan, runtime/migrate.py) starts with setup; its answer is in
// device_status as import_* fields. The desktop preview has no old cards: RGDS_IMPORT_PREVIEW=1 scans its folders.
void App::welcome_import_scan() {
    if (o.preview && !std::getenv("RGDS_IMPORT_PREVIEW")) return;
    device_status["import_found"] = "";
    device_status["import_state"] = "scanning";
    welcome_import_focused = false;
    // One worker at a time: a first boot is indexing its library as setup opens. The scan waits for it rather than
    // stopping it (start_worker would), and welcome_import_poll starts it once the worker is free.
    welcome_import_pending = worker > 0 && worker_action != "device.info" && worker_action != "device.update-auto";
    if (!welcome_import_pending) start_worker("device.import-scan");
}

bool App::welcome_import_shown() {
    if (welcome_import_pending || (worker > 0 && worker_action == "device.import-scan")) return true;
    const std::string state = device_status["import_state"];
    return device_status["import_found"] == "1" || state == "copying" || state == "done" ||
           (state == "error" && (worker_action == "device.import-apply" || worker_action == "device.import-games"));
}

// From poll_service: a scan that found nothing moves on by itself; a finished copy puts the focus on Next.
void App::welcome_import_poll() {
    if (welcome_import_pending && (!welcome_active || worker <= 0)) {
        welcome_import_pending = false;
        if (welcome_active) start_worker("device.import-scan");
    }
    if (welcome_import_rescan && worker <= 0) {
        // the same scan as a copy noticed on Home: new games in, and the index the copy stopped runs again
        welcome_import_rescan = false;
        rescan_games(true);
    }
    if (settings.page != Page::Welcome || !welcome_active || welcome_step != WelcomeImport) return;
    if (!welcome_import_shown()) welcome_enter(WelcomeColour);
    else if (device_status["import_state"] == "found" && !welcome_import_focused) {
        welcome_import_focused = true;
        if (welcome_focus == welcome_next) welcome_focus = 0;
        dirty = true;
    } else if (device_status["import_state"] == "done" && welcome_focus != welcome_next &&
               welcome_focus != welcome_back && welcome_focus != welcome_skip) {
        welcome_focus = welcome_next;
        dirty = true;
    }
}

void App::welcome_import_top(SDL_Renderer* r, int y) {
    const std::string source = device_status["import_source"], place = device_status["import_place"];
    if (device_status["import_found"] != "1") {
        boot_caption(r, "Looking for saves from another system…", y + 200, 24, "ink");
        return;
    }
    if (device_status["import_state"] == "done") {
        welcome_top_heading(r, y, "Welcome over!", "Your saves and play time are in place.");
        ui_icon(r, SettingsIcon::Check, 320, y + 290, 72, "accent");
        return;
    }
    const std::string where = place == "flasher" ? "RG DS Flasher kept them from your old card before it erased it."
                              : place == "slot"  ? "They are on the card in the second slot."
                              : place == "usb"   ? "They are on the USB drive."
                                                 : "They are in the import folder on this card.";
    // the stock system keeps no play time or favourites, and "Coming from the stock system?" is its own sentence
    const bool stock = device_status["import_kind"] == "stock";
    boot_caption(r, locale.text(stock ? "Coming from the stock system?" : "Coming from " + source + "?"), y + 72, 36,
                 "ink");
    const std::string what =
        stock ? "Your saves can come with you." : "Your saves, play time and favourites can come with you.";
    centered_wrapped(r, locale.text(what) + " " + locale.text(where), y + 128, 20, 540, 3, "muted");
    welcome_system_icon(r, "nds", 256, y + 230, 128);
}

void App::welcome_import_bottom(SDL_Renderer* r, int y) {
    auto count = [&](const char* key) { return std::atoll(device_status[key].c_str()); };
    const std::string state = device_status["import_state"];
    if (device_status["import_found"] != "1" && state != "error") {
        busy_ring(r, 320, y + 220, 28, 6);
        return;
    }
    if (state == "copying") {
        const int progress = std::clamp(int(count("import_progress")), 0, 100);
        boot_caption(r, "Bringing everything over…", y + 160, 24, "ink");
        fill_round(r, 60, y + 220, 520, 16, 8, theme.at("slot"));
        if (progress > 0) fill_round(r, 60, y + 220, std::max(16, 520 * progress / 100), 16, 8, theme.at("accent"));
        centered_wrapped(r, locale.text("Keep the handheld on until this is done."), y + 260, 16, 540, 2, "muted");
        return;
    }
    if (state == "error") {
        wrapped(r, device_status["message"], 28, y + 80, 20, 584, 4);
        return;
    }
    // What there is, or what came: one line each, only what is not zero.
    const bool done = state == "done";
    std::vector<std::pair<SettingsIcon, std::string>> lines;
    auto line = [&](SettingsIcon icon, long long n, const char* one, const char* many) {
        if (n > 0) lines.push_back({icon, locale.text(std::to_string(n) + " " + (n == 1 ? one : many))});
    };
    line(SettingsIcon::Load, count(done ? "import_done_saves" : "import_saves"), "save", "saves");
    line(SettingsIcon::Load, count(done ? "import_done_states" : "import_states"), "resume point", "resume points");
    const std::string hours = device_status["import_hours"];
    if (count(done ? "import_done_played" : "import_played") > 0)
        lines.push_back({SettingsIcon::Clock, locale.text(hours + " hours of play time")});
    line(SettingsIcon::Heart, count(done ? "import_done_favourites" : "import_favourites"), "favourite", "favourites");
    line(SettingsIcon::Cartridge, count(done ? "import_done_games" : "import_games_here"), "game on this card",
         "games on this card");
    line(SettingsIcon::Cartridge, count(done ? "import_done_bios" : "import_bios"), "BIOS file", "BIOS files");
    const int rows = std::min(int(lines.size()), done ? 6 : 4);
    for (int i = 0; i < rows; i++) {
        const int row = y + 64 + i * 44;
        ui_icon(r, lines[size_t(i)].first, 56, row + 18, 26, "accent");
        raw_label(r, lines[size_t(i)].second, 88, centred_label_y(lines[size_t(i)].second, 20, row + 18), 20, "ink",
                  520);
    }
    if (lines.empty()) wrapped(r, "Everything was already here.", 28, y + 80, 20, 584, 2);
    const long long away = count("import_games_away");
    if (done && away > 0) {
        // The flasher left the games on the computer: they go back in the roms folder, as they were.
        const std::string note = locale.text(
            std::to_string(away) +
            (away == 1 ? " game is still on your computer. Copy the roms folder from the backup to this card's roms "
                         "folder (the Games step shows how): its play time and saves find it there."
                       : " games are still on your computer. Copy the roms folder from the backup to this card's "
                         "roms folder (the Games step shows how): their play time and saves find them there."));
        wrapped(r, note, 28, y + 64 + rows * 44 + 12, 16, 584, 4);
    }
    if (done) return;
    if (count("import_kept") > 0)
        wrapped(r, locale.text(std::to_string(count("import_kept")) + " saves already here stay as they are."), 28,
                y + 228, 16, 584, 1);
    welcome_row(r, y, 0, SettingsIcon::Load, "Bring them over", locale.text("Press A. Nothing here is replaced."));
    if (count("import_games_copy") > 0) {
        char size[32];
        const double bytes = double(count("import_games_bytes"));
        if (bytes >= 1e9) std::snprintf(size, sizeof size, "%.1f GB", bytes / 1e9);
        else std::snprintf(size, sizeof size, "%.0f MB", std::max(1.0, bytes / 1e6));
        welcome_row(r, y, 1, SettingsIcon::Cartridge, "Bring the games too",
                    locale.text(std::to_string(count("import_games_copy")) + " games") + ", " + size);
    }
}
