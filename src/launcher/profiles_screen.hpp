#pragma once
// Profiles: Who's playing?, Settings > Profiles and a profile's own page (docs/PROFILES_PLAN.md).
// Members of App in main.cpp; included once, right after the class.

// What belongs to the player, read again for whoever is now playing: the same steps as coming back from a game.
void App::read_look() {
    try {
        preferences = load_preferences(o.preferences);
    } // reads ui.ini even without the base file
    catch (const std::exception& e) {
        std::cerr << "Preferences fallback: " << e.what() << std::endl;
        preferences = Preferences{};
        try {
            load_ui_preferences(o.preferences.parent_path() / "ui.ini", preferences);
        } catch (const std::exception&) {}
    }
}

void App::apply_profile() {
    cancel_navigation();
    collection_filter.cancel();
    collection_loading = false;
    search_query.clear();
    discovery_filter = 0;
    discovery_facets = {};
    home_bar_focus = -1;
    o.player = player_root(o.data);
    if (look_follows_player) o.preferences = o.player / "preferences.ini";
    read_look();
    if (!o.mode.empty()) preferences.dark = o.mode == "dark";
    if (!o.font.empty()) preferences.pixel = o.font == "pixel";
    locale.load(asset_path("locales"), preferences.language);
    rounded.prefer(preferences.language);
    if (!preferences.pixel) check_fonts();
    try {
        reload_themes();
    } catch (const std::exception& e) {
        std::cerr << "Theme fallback: " << e.what() << std::endl;
        theme = preferences.dark ? default_dark_theme() : default_theme();
    }
    library_cache.reset(o.data);
    chat.load_profile(o.player / "pictochat.ini", preferences.accent);
    if (library_ready) {
        refresh_other_systems(); // whether other systems are shown is the player's choice
        summary_ready = false;
        library_summary.request(o.data, all_games);
    }
    home_selected = home_fit(home_selected);
    carousel.reset(navigation_selected());
    configure_sounds();
    selected_at = page_at = s.GetTicks();
    dirty = true;
}

// ---- Who's playing? ------------------------------------------------------------------------------------------------

bool App::profile_picker_wanted() const {
    // A PIN on whoever played last is not to be walked past by turning the question off.
    return profiles.active_unclear() ||
           (profiles.enabled() && (profiles.ask_at_boot() || !active_profile().pin.empty()));
}

void App::open_profile_picker(bool boot) {
    if (!boot && profile_removing) { // nobody walks into the folder that is about to move
        status = "One moment.";
        dirty = true;
        return;
    }
    profiles.load(o.data);
    picker_at_boot = boot;
    picker_return = settings.page;
    settings.page = Page::ProfilePicker;
    settings.focus = profiles.active_index();
    page_at = s.GetTicks();
    status.clear();
    dirty = true;
}

SDL_Rect App::profile_tile_box(int index) const {
    // Three to a row, each row centred: 128 px tiles, with the name below.
    constexpr int cell = 176, tile = 128, row_height = 168;
    const int count = int(profiles.profiles().size()), rows = count > 3 ? 2 : 1;
    const int row = index / 3, in_row = row == 0 ? std::min(count, 3) : count - 3;
    const int left = 320 - in_row * cell / 2, top = rows == 1 ? 150 : 80;
    return {left + (index % 3) * cell + (cell - tile) / 2, top + row * row_height, tile, tile};
}

void App::draw_profile_tile(SDL_Renderer* r, const Profile& profile, int x, int y, int size) {
    const Color fill = profile.colour >= 0 && profile.colour < 16
                           ? ds_colours[size_t(profile.colour)].rgb
                           : (preferences.dark ? dark_theme : light_theme).at("accent");
    raised_pill(r, x, y, size, size, std::max(4, size / 12), fill);
    const Color ink = tile_ink(fill);
    const std::string_view pixels = profile_picture(profile.picture);
    const int scale = std::max(1, size * 3 / 4 / 16), left = x + (size - 16 * scale) / 2,
              top = y + (size - 16 * scale) / 2;
    s.SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    const auto smooth = asset_path("profiles/" + std::string(profile_picture_file(profile.picture)) + ".bmp");
    if (profile_picture_rich(profile.picture) && media.warm(r, smooth) && media.draw(r, smooth, {x, y, size, size})) {
        // The smooth character, drawn in its own sticker edge.
    } else if (profile_picture_rich(profile.picture)) {
        // Its pixel version, in its own colours, filling the tile as a Netflix face does.
        const int big = std::max(1, size * 7 / 8 / 16), bx = x + (size - 16 * big) / 2, by = y + (size - 16 * big) / 2;
        for (size_t i = 0; i < pixels.size(); i++) {
            const auto colour = profile_palette(pixels[i]);
            if (!colour) continue;
            s.SetRenderDrawColor(r, Uint8(*colour >> 16), Uint8(*colour >> 8), Uint8(*colour), 255);
            SDL_Rect cell{bx + int(i % 16) * big, by + int(i / 16) * big, big, big};
            s.RenderFillRect(r, &cell);
        }
    } else
        for (char tone : {'#', '+'}) {
            s.SetRenderDrawColor(r, ink.r, ink.g, ink.b, tone == '#' ? 255 : 110);
            for (size_t i = 0; i < pixels.size(); i++)
                if (pixels[i] == tone) {
                    SDL_Rect cell{left + int(i % 16) * scale, top + int(i / 16) * scale, scale, scale};
                    s.RenderFillRect(r, &cell);
                }
        }
    s.SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    if (profile.restricted && size >= 64) kid_badge(r, x + size / 2, y + size, size);
}

// Netflix's Kids mark, our way: the word in bright letters, each its own colour, on a white pill across the tile's
// bottom edge, half on the picture and half below it.
void App::kid_badge(SDL_Renderer* r, int centre, int bottom, int size) {
    static constexpr std::array<Color, 6> letters = {
        {{229, 56, 59}, {245, 124, 0}, {214, 160, 0}, {46, 158, 68}, {31, 122, 224}, {142, 63, 200}}};
    const std::string word = locale.text("Kids");
    const int text = size >= 150 ? 26 : size >= 100 ? 20 : 14, h = text + text / 2 + 4;
    const int w = std::min(size - 8, label_width(word, text, false) + text + 4), left = centre - w / 2,
              top = bottom - h / 2;
    alpha_round(r, left, top + 2, w, h, h / 2, shadow_colour(), 60);
    fill_round(r, left, top, w, h, h / 2, {255, 255, 255});
    int at = centre - label_width(word, text, false) / 2, n = 0;
    for (size_t i = 0; i < word.size();) {
        const auto lead = static_cast<unsigned char>(word[i]);
        const size_t bytes = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
        const std::string one = word.substr(i, bytes);
        raw_label_colour(r, one, at, centred_label_y(one, text, top + h / 2), text,
                         letters[size_t(n++) % letters.size()], w);
        at += label_width(one, text, false);
        i += bytes;
    }
}

// Which PIN stands between this moment and that profile. The owner, once in, goes anywhere. At boot nobody is in yet.
// A child cannot walk into a grown-up's profile just because that one has no PIN of its own: the owner's stands guard.
const std::string& App::required_pin(int index) const {
    static const std::string none;
    const auto& list = profiles.profiles();
    const auto& target = list[size_t(index)];
    const bool nobody_in = picker_at_boot || profiles.active_unclear();
    if (!nobody_in && target.id == profiles.active()) return none;
    if (!nobody_in && active_profile().owner()) return none;
    if (!target.pin.empty()) return target.pin;
    if (!target.restricted && (nobody_in || active_profile().restricted)) return list.front().pin;
    return none;
}

void App::choose_profile(int index) {
    const auto& list = profiles.profiles();
    if (index < 0 || index >= int(list.size())) return;
    const auto& pin = required_pin(index);
    if (!pin.empty()) {
        pin_profile = index;
        open_pin(PinPurpose::Enter, pin);
        return;
    }
    commit_profile(index);
}

void App::commit_profile(int index) {
    const auto& list = profiles.profiles();
    settings_unlocked = false;
    const bool changed = list[size_t(index)].id != profiles.active() || profiles.active_unclear();
    if (changed && !profiles.select(list[size_t(index)].id)) {
        status = "Could not change profile. Check storage.";
        emit_sound(MenuCue::Error);
        dirty = true;
        return;
    }
    emit_sound(MenuCue::Confirm);
    if (changed) apply_profile();
    settings.page = Page::Library;
    settings.focus = 0;
    if (changed || picker_at_boot) home_menu = !o.library;
    else if (picker_return != Page::Library) settings.page = picker_return;
    systems_menu = false;
    if (!home_menu && library_ready) library_view(preferences.view);
    picker_at_boot = false;
    selected_at = s.GetTicks();
    dirty = true;
}

void App::profile_picker_action(int code) {
    const int count = int(profiles.profiles().size());
    int& focus = settings.focus;
    if (code == 3 && !picker_at_boot && !profiles.active_unclear()) {
        settings.page = picker_return;
        settings.focus = 0;
        dirty = true;
    } else if (code == 2 || code == 4) choose_profile(focus);
    else if (code == 0 || code == 1 || code == 8 || code == 9) {
        const int next = code == 0 ? focus - 1 : code == 1 ? focus + 1 : code == 8 ? focus - 3 : focus + 3;
        // Down from a tile with nothing below lands on the last one.
        const int moved = code == 9 && focus + 3 >= count && focus / 3 + 1 < (count + 2) / 3 ? count - 1 : next;
        if (moved >= 0 && moved < count && moved != focus) {
            focus = moved;
            emit_sound(MenuCue::Navigate, code == 0 || code == 8 ? -1 : 1);
        }
        dirty = true;
    }
}

void App::profile_picker_touch(int x, int y) {
    for (int i = 0; i < int(profiles.profiles().size()); i++) {
        const auto box = profile_tile_box(i);
        if (point_in({box.x - 16, box.y - 8, box.w + 32, box.h + 48}, x, y)) {
            settings.focus = i;
            choose_profile(i);
            return;
        }
    }
}

void App::profile_picker_bottom(SDL_Renderer* r, int y) {
    // Not the settings header: at boot there is no back.
    const std::string title = locale.text("Who's playing?");
    const int title_width = label_width(title, 28, false);
    raw_label(r, title, 320 - title_width / 2, y + 24, 28, "ink", 600);
    const auto& list = profiles.profiles();
    for (int i = 0; i < int(list.size()); i++) {
        const auto box = profile_tile_box(i);
        const bool pressed = pointer_down && point_in(box, contact_x, contact_y) && point_in(box, down_x, down_y);
        draw_profile_tile(r, list[size_t(i)], box.x, y + box.y + (pressed ? 2 : 0), box.w);
        if (settings.focus == i)
            chat_brackets(r, box.x - 2, y + box.y - 2, box.w + 4, box.h + 4, theme.at("accent"), 4);
        if (!required_pin(i).empty()) {
            // A padlock in the tile's corner.
            fill_round(r, box.x + box.w - 30, y + box.y + 6, 24, 24, 12, theme.at("panel"));
            ui_icon(r, SettingsIcon::Key, box.x + box.w - 18, y + box.y + 18, 16, "accent");
        }
        const std::string& name = list[size_t(i)].name;
        const int width = std::min(168, label_width(name, 20, false));
        // Low enough to clear the Kids mark on the tile's edge.
        raw_label(r, name, box.x + box.w / 2 - width / 2, y + box.y + box.h + 22, 20, "ink", 168);
    }
    std::vector<KeyHint> hints = {{"A", "Play"}};
    if (!picker_at_boot && !profiles.active_unclear()) hints.push_back({"B", "Back"});
    key_hints(r, 320, y + 456, hints, 0);
    if (!status.empty()) {
        const int status_width = std::min(600, label_width(status, 16));
        label(r, status, 320 - status_width / 2, y + 428, 16, "muted", 600);
    }
}

void App::profile_card_top(SDL_Renderer* r, int y, const Profile& profile) {
    const int top = y + top_card_y + (top_card_h - 240) / 2;
    settings_card(r, 24, top, 592, 240);
    draw_profile_tile(r, profile, 48, top + 32, 176);
    raw_label(r, profile.name, 248, top + 34, 36, "ink", 344);
    std::string kind = locale.text(profile.owner() ? "Owner" : profile.restricted ? "Kid" : "Adult");
    if (profile.age != no_age_limit) kind += "  ·  " + age_limit_text(profile.age);
    raw_label(r, kind, 248, top + 84, 18, "accent", 344);
    if (!profile.last_game.empty()) {
        // The game as Home's Last played tile shows it: its icon, or its system's cartridge for an older file.
        round_rect(r, 248, top + 124, 344, 92, 10, "slot");
        Game last{profile.last_game, profile.last_path, false, profile.last_system};
        if (!last.path.empty() && !last.system.empty()) {
            warm_icon(r, last);
            game_icon(r, last, 260, top + 138, 64);
        } else ui_icon(r, SettingsIcon::Cartridge, 292, top + 170, 36, "muted");
        label(r, "Last played", 340, top + 138, 14, "muted", 240);
        raw_wrapped(r, profile.last_game, 340, top + 160, 20, 240, 2);
    }
}

void App::profile_picker_top(SDL_Renderer* r, int y) {
    const auto& list = profiles.profiles();
    if (!list.empty()) profile_card_top(r, y, list[size_t(std::clamp(settings.focus, 0, int(list.size()) - 1))]);
}

// ---- Settings > Profiles -------------------------------------------------------------------------------------------

void App::open_profiles() {
    profiles.load(o.data);
    settings.section = 11;
    settings.page = Page::Profiles;
    settings.focus = 0;
    status.clear();
    page_at = s.GetTicks();
    dirty = true;
}

// A profile's index, or one of these for the rows that are not a profile.
std::vector<int> App::profiles_rows() const {
    std::vector<int> rows;
    const int count = int(profiles.profiles().size());
    if (profiles.enabled()) rows.push_back(profile_row_switch);
    for (int i = 0; i < count; i++) rows.push_back(i);
    if (profiles.can_add()) rows.push_back(profile_row_add);
    if (profiles.enabled()) rows.push_back(profile_row_ask);
    return rows;
}

// As tall as the rows fit above the bottom edge, up to 52: up to three people and the other rows get the full height.
SDL_Rect App::profiles_row_box(int row) const {
    const int count = std::max(1, int(profiles_rows().size())), stride = std::min(58, 390 / count);
    return {28, 84 + row * stride, 584, stride - 6};
}

void App::profiles_activate(int row) {
    if (profile_removing) { // the folder is about to move: nobody starts using it, or changes it, meanwhile
        status = "One moment.";
        dirty = true;
        return;
    }
    if (row == profile_row_switch) open_profile_picker(false);
    else if (row == profile_row_add) open_profile_choose(ProfileChoice::Kind);
    else if (row == profile_row_ask) {
        // Not a page, so the checkpoint cannot see it.
        if (!active_profile().owner() && !settings_unlocked && !owner_pin().empty()) {
            pin_target_page = Page::Profiles;
            pin_target_focus = settings.focus;
            open_pin(PinPurpose::Unlock, owner_pin());
            return;
        }
        if (!profiles.set_ask_at_boot(!profiles.ask_at_boot())) status = "Could not save all settings. Check storage.";
        emit_sound(profiles.ask_at_boot() ? MenuCue::Enable : MenuCue::Disable);
    } else if (row >= 0) {
        profile_editing = profiles.profiles()[size_t(row)].id;
        settings.page = Page::ProfileEdit;
        settings.focus = 0;
        page_at = s.GetTicks();
    }
    dirty = true;
}

void App::profiles_action(int code) {
    const auto rows = profiles_rows();
    settings.focus = std::clamp(settings.focus, 0, int(rows.size()) - 1);
    if (code == 3) {
        settings.page = Page::Settings;
        settings.section = 11;
        settings.focus = 0;
    } else if (code == 8 || code == 9) {
        const int next = std::clamp(settings.focus + (code == 8 ? -1 : 1), 0, int(rows.size()) - 1);
        if (next != settings.focus) emit_sound(MenuCue::Navigate, code == 8 ? -1 : 1);
        settings.focus = next;
    } else if (code == 2 || code == 4) profiles_activate(rows[size_t(settings.focus)]);
    else if ((code == 0 || code == 1) && rows[size_t(settings.focus)] == profile_row_ask)
        profiles_activate(profile_row_ask);
    dirty = true;
}

void App::profiles_touch(int x, int y) {
    const auto rows = profiles_rows();
    for (int i = 0; i < int(rows.size()); i++)
        if (point_in(profiles_row_box(i), x, y)) {
            settings.focus = i;
            profiles_activate(rows[size_t(i)]);
            return;
        }
}

void App::profiles_screen(SDL_Renderer* r, int y) {
    settings_header(r, y, "Profiles");
    const auto rows = profiles_rows();
    for (int i = 0; i < int(rows.size()); i++) {
        const auto box = profiles_row_box(i);
        const int row = rows[size_t(i)], top = y + box.y, middle = top + box.h / 2;
        settings_card(r, box.x, top, box.w, box.h, settings.focus == i);
        const int tile = box.h - 12, text = box.h >= 50 ? 22 : 20, text_x = box.x + tile + 26;
        if (row >= 0) {
            const auto& profile = profiles.profiles()[size_t(row)];
            draw_profile_tile(r, profile, box.x + 10, top + 6, tile);
            raw_label(r, profile.name, text_x, centred_label_y(profile.name, text, middle), text, "ink", 300);
            const std::string tag = locale.text(profile.id == profiles.active() ? "Playing now"
                                                : profile.owner()               ? "Owner"
                                                : profile.restricted            ? "Kid"
                                                                                : "Adult");
            const int width = label_width(tag, 16, false);
            raw_label(r, tag, box.x + box.w - 16 - width, centred_label_y(tag, 16, middle), 16, "muted", 200);
        } else {
            const std::string title = row == profile_row_switch ? "Change who's playing"
                                      : row == profile_row_add  ? "Add a profile"
                                                                : "Ask who's playing at startup";
            round_rect(r, box.x + 10, top + 6, tile, tile, std::max(4, tile / 6), "slot");
            ui_icon(r,
                    row == profile_row_switch ? SettingsIcon::Refresh
                    : row == profile_row_add  ? SettingsIcon::Spark
                                              : SettingsIcon::Power,
                    box.x + 10 + tile / 2, middle, std::min(26, tile * 2 / 3), "accent");
            label(r, title, text_x, centred_label_y(locale.text(title), text, middle), text, "ink", 440);
            if (row == profile_row_ask) draw_toggle(r, box.x + box.w - 60, middle - 14, title, profiles.ask_at_boot());
        }
    }
}

void App::profiles_top(SDL_Renderer* r, int y) {
    if (profile_removing) { // the backup comes first, and takes a minute or more
        const int top = y + top_card_y + (top_card_h - 240) / 2;
        settings_card(r, 24, top, 592, 240);
        busy_ring(r, 320, top + 90, 26, 4);
        const char* text = "Backing up your saves…";
        label(r, text, 320 - label_width(locale.text(text), 20, false) / 2, top + 150, 20, "ink", 544);
        return;
    }
    const auto rows = profiles_rows();
    const int row = rows.empty() ? -1 : rows[size_t(std::clamp(settings.focus, 0, int(rows.size()) - 1))];
    if (row >= 0) {
        profile_card_top(r, y, profiles.profiles()[size_t(row)]);
        return;
    }
    const int top = y + top_card_y + (top_card_h - 240) / 2;
    settings_card(r, 24, top, 592, 240);
    label(r, "Profiles", 48, top + 30, 28, "accent", 544);
    wrapped(r, "Everyone who plays gets their own saves, favourites and playtime, and their own look.", 48, top + 84,
            20, 544, 4);
}

// ---- One profile ---------------------------------------------------------------------------------------------------

const Profile* App::edited_profile() const {
    for (const auto& profile : profiles.profiles())
        if (profile.id == profile_editing) return &profile;
    return nullptr;
}

std::vector<int> App::profile_edit_rows() const {
    // Who it's for 0, Name 1, DS name 2, Picture 3, Colour 4, Language 18, Age limit 6, No rating 7, Games 8, Games opens 17,
    // Ports 9,
    // Homebrew 10, PictoChat 11, Cheats 12, Local wireless 13, PIN 14, Remove the PIN 15, Erase removed 16, Remove 5: the
    // numbers stay, the order is what reads best.
    const auto* profile = edited_profile();
    std::vector<int> rows;
    if (profile && profile->owner()) rows = {1, 2, 3, 4, 18, 8, 17, 14};
    else if (profile && !profile->restricted) rows = {0, 1, 2, 3, 4, 18, 6, 7, 8, 17, 14};
    else rows = {0, 1, 2, 3, 4, 18, 6, 7, 8, 17, 9, 10, 11, 12, 13, 14};
    if (profile && !profile->pin.empty()) rows.push_back(15);
    if (profile && profile->owner() && profiles.removed_count() > 0) rows.push_back(16);
    if (!profile || !profile->owner()) rows.push_back(5);
    return rows;
}

void App::profile_edit_focus(int row) {
    const auto rows = profile_edit_rows();
    const auto at = std::find(rows.begin(), rows.end(), row);
    settings.focus = at == rows.end() ? 0 : int(at - rows.begin());
}

void App::profile_edit_activate(int row) {
    const auto* profile = edited_profile();
    if (!profile) return;
    Profile changed = *profile;
    const auto save = [&] {
        if (!profiles.save(changed)) status = "Could not save all settings. Check storage.";
        if (changed.id == profiles.active()) visibility_changed = true;
        emit_sound(MenuCue::Confirm);
    };
    if (row == 0) {
        // A grown-up's profile keeps what was set; one that becomes a Kid's starts safe.
        changed.restricted = !changed.restricted;
        if (changed.restricted) {
            if (changed.age == no_age_limit) changed.age = 7;
            changed.unrated_hidden = true;
        } else {
            changed.age = no_age_limit;
            changed.unrated_hidden = false;
        }
        save();
        profile_edit_focus(0);
    } else if (row == 1) begin_edit(9);
    else if (row == 2) begin_edit(10);
    else if (row == 3) open_profile_choose(ProfileChoice::Picture);
    else if (row == 4) open_profile_choose(ProfileChoice::Colour);
    else if (row == 6) open_profile_choose(ProfileChoice::Age);
    else if (row == 7) {
        changed.unrated_hidden = !changed.unrated_hidden;
        save();
    } else if (row == 8) open_profile_games(0);
    else if (row == 9) {
        changed.allow_ports = !changed.allow_ports;
        save();
    } else if (row == 10) {
        changed.allow_homebrew = !changed.allow_homebrew;
        save();
    } else if (row == 11) {
        changed.allow_pictochat = !changed.allow_pictochat;
        save();
    } else if (row == 12) {
        changed.allow_cheats = !changed.allow_cheats;
        save();
    } else if (row == 13) {
        changed.allow_wireless = !changed.allow_wireless;
        save();
    } else if (row == 14) {
        pin_owner_id = profile->id;
        open_pin(PinPurpose::Set, "");
        pin_message.clear();
    } else if (row == 15) {
        changed.pin.clear();
        save();
        profile_edit_focus(14);
    } else if (row == 18) {
        std::vector<std::string> names;
        int selected = 0;
        for (const auto& item : locale.languages()) {
            if (item.first == profile->language) selected = int(names.size());
            names.push_back(item.second);
        }
        const std::string id = profile->id;
        show_picker("Language", names, selected, [this, id](int n) {
            const auto language = locale.languages().at(size_t(n)).first;
            for (const auto& each : profiles.profiles())
                if (each.id == id) {
                    profile_change_look(each, [&](Preferences& look) { look.language = language; });
                    break;
                }
            if (id == profiles.active()) { // whoever is playing reads it at once, as from Settings > System
                locale.load(asset_path("locales"), language);
                rounded.prefer(language);
                if (!preferences.pixel) check_fonts();
            }
            profile_edit_focus(18);
        });
        return;
    } else if (row == 17) {
        // Every game in one list, or a Systems page first. Either way other systems are on: that is what it chooses
        // between.
        const bool all = !profile->all_games;
        profile_change_look(*profile, [all](Preferences& look) {
            look.show_systems = true;
            look.all_games = all;
        });
        if (profile->id == profiles.active()) {
            browse_system.clear();
            refresh_other_systems();
            summary_ready = false;
            library_summary.request(o.data, all_games);
        }
        emit_sound(MenuCue::Confirm);
    } else if (row == 16) {
        confirm("profile-erase", Page::ProfileEdit);
    } else if (row == 5) {
        if (profile->id == profiles.active()) {
            status = "Switch to another profile first";
            emit_sound(MenuCue::Error);
        } else confirm("profile-remove", Page::ProfileEdit);
    }
    dirty = true;
}

void App::profile_edit_action(int code) {
    const auto rows = profile_edit_rows();
    settings.focus = std::clamp(settings.focus, 0, int(rows.size()) - 1);
    if (code == 3) {
        const auto& list = profiles.profiles();
        const auto all = profiles_rows();
        settings.page = Page::Profiles;
        settings.focus = 0;
        for (int i = 0; i < int(all.size()); i++)
            if (all[size_t(i)] >= 0 && list[size_t(all[size_t(i)])].id == profile_editing) settings.focus = i;
        status.clear();
    } else if (code == 8 || code == 9) {
        settings.focus = std::clamp(settings.focus + (code == 8 ? -1 : 1), 0, int(rows.size()) - 1);
        status.clear();
    } else if (code == 2 || code == 4) profile_edit_activate(rows[size_t(settings.focus)]);
    dirty = true;
}

std::string App::age_limit_text(int age) const {
    return age == no_age_limit        ? locale.text("No limit")
           : age == only_picked_games ? locale.text("Only games I pick")
                                      : std::to_string(age) + "+";
}

void App::profile_edit_row(SDL_Renderer* r, int y, int i) {
    const auto* profile = edited_profile();
    const auto rows = profile_edit_rows();
    if (!profile || i >= int(rows.size())) return;
    const int row = rows[size_t(i)];
    switch (row) {
    case 0: list_row(r, y, i, "Who it's for", profile->restricted ? "Kid" : "Adult"); break;
    case 1: list_row(r, y, i, "Name", profile->name); break;
    case 2: list_row(r, y, i, "DS name", profile->ds_name); break;
    case 3: list_row(r, y, i, "Picture", std::string(profile_picture_name(profile->picture))); break;
    case 4: {
        const Color colour = profile->colour >= 0 && profile->colour < 16 ? ds_colours[size_t(profile->colour)].rgb
                                                                          : light_theme.at("accent");
        list_row(r, y, i, "Colour",
                 profile->colour >= 0 && profile->colour < 16 ? ds_colours[size_t(profile->colour)].name
                                                              : "Classic blue",
                 true, &colour);
        break;
    }
    case 6: list_row(r, y, i, "Age limit", age_limit_text(profile->age)); break;
    case 7: list_row(r, y, i, "Games with no rating", profile->unrated_hidden ? "Hidden" : "Shown"); break;
    case 8: list_row(r, y, i, "Games", ""); break;
    case 9: list_row(r, y, i, "Ports", profile->allow_ports ? "On" : "Off"); break;
    case 10: list_row(r, y, i, "Homebrew", profile->allow_homebrew ? "On" : "Off"); break;
    case 11: list_row(r, y, i, "PictoChat", profile->allow_pictochat ? "On" : "Off"); break;
    case 12: list_row(r, y, i, "Cheats", profile->allow_cheats ? "On" : "Off"); break;
    case 13: list_row(r, y, i, "Local wireless", profile->allow_wireless ? "On" : "Off"); break;
    case 14:
        list_row(r, y, i, profile->pin.empty() ? "Set a PIN" : "Change the PIN", profile->pin.empty() ? "None" : "Set");
        break;
    case 15: list_row(r, y, i, "Remove the PIN", ""); break;
    case 16: list_row(r, y, i, "Erase removed profiles", std::to_string(profiles.removed_count())); break;
    case 17: list_row(r, y, i, "Games opens", profile->all_games ? "All games" : "Systems"); break;
    case 18: list_row(r, y, i, "Language", language_name(profile->language)); break;
    default: list_row(r, y, i, "Remove this profile", "", profile->id != profiles.active()); break;
    }
}

void App::profile_edit_top(SDL_Renderer* r, int y) {
    const auto* profile = edited_profile();
    if (!profile) return;
    profile_card_top(r, y, *profile);
    // Said plainly: without the owner's PIN a Kid profile is filtered, not locked.
    const std::string note = !status.empty() ? status
                             : profile->restricted && owner_pin().empty()
                                 ? "Not locked: set a PIN on the owner's profile."
                                 : "";
    if (!note.empty()) {
        const int width = std::min(584, label_width(note, 16));
        label(r, note, 320 - width / 2, y + top_card_y + top_card_h - 20, 16, "muted", 584);
    }
}

// ---- Choosing: Adult or Kid, a picture, a colour ----------------------------------------------------------------------

void App::open_profile_choose(ProfileChoice kind) {
    profile_choice = kind;
    const auto* profile = edited_profile();
    settings.page = Page::ProfileChoose;
    settings.focus = kind == ProfileChoice::Kind      ? 0
                     : kind == ProfileChoice::Picture ? (profile ? profile->picture : 0)
                     : kind == ProfileChoice::Age
                         ? (!profile                            ? 4
                            : profile->age == only_picked_games ? 14
                            : profile->age == no_age_limit      ? 15
                                                                : std::clamp(profile->age - 3, 0, 13))
                         : (profile ? std::clamp(profile->colour, 0, 15) : 0);
    page_at = s.GetTicks();
    dirty = true;
}

int App::profile_choice_count() const {
    return profile_choice == ProfileChoice::Kind      ? 2
           : profile_choice == ProfileChoice::Picture ? int(profile_picture_count)
                                                      : 16; // the colours; or ages 3 to 16, only picked, no limit
}

SDL_Rect App::profile_choice_box(int index) const {
    if (profile_choice == ProfileChoice::Kind) return {28 + index * 300, 130, 284, 210};
    if (profile_choice == ProfileChoice::Colour || profile_choice == ProfileChoice::Age) return colour_tile(index);
    return {33 + (index % 6) * 100, 96 + (index / 6) * 80, 74, 74}; // six across: 24 pictures in four rows
}

void App::profile_set_colour(const Profile& profile, int colour) {
    profile_change_look(profile, [colour](Preferences& look) { look.accent = colour; });
}

// A setting of the profile's own look: live for whoever is playing, in that profile's files for anyone else.
void App::profile_change_look(const Profile& profile, const std::function<void(Preferences&)>& change) {
    if (profile.id == profiles.active()) {
        change(preferences);
        remember_appearance();
    } else {
        Preferences look;
        try {
            look = load_preferences(profile.root / "preferences.ini");
        } catch (const std::exception&) {
            try {
                load_ui_preferences(profile.root / "ui.ini", look);
            } catch (const std::exception&) {}
        }
        change(look);
        try {
            save_preferences(profile.root / "preferences.ini", look);
        } catch (const std::exception&) {
            status = "Could not save all settings. Check storage.";
        }
    }
    profiles.load(o.data);
}

void App::profile_set_ds_name(const Profile& profile, const std::string& name) {
    if (profile.id == profiles.active()) {
        save_owner_name(name);
    } else {
        auto chat_fields = profile_fields(profile.root / "pictochat.ini");
        chat_fields["name"] = name;
        if (!profile_write(profile.root / "ds.ini", "nickname=" + name + "\n") ||
            !profile_write_fields(profile.root / "pictochat.ini", chat_fields))
            status = "Could not save DS settings.";
    }
    profiles.load(o.data);
}

void App::choose_profile_option(int index) {
    emit_sound(MenuCue::Confirm);
    if (profile_choice == ProfileChoice::Kind) {
        new_profile_restricted = index == 1;
        begin_edit(8);
        return;
    }
    const auto* profile = edited_profile();
    if (profile && profile_choice == ProfileChoice::Picture) {
        // Applied at once and the page stays, so one picture after another can be tried; B leaves.
        Profile changed = *profile;
        changed.picture = index;
        if (!profiles.save(changed)) status = "Could not save all settings. Check storage.";
        dirty = true;
        return;
    } else if (profile && profile_choice == ProfileChoice::Age) {
        Profile changed = *profile;
        changed.age = index == 15 ? no_age_limit : index == 14 ? only_picked_games : index + 3;
        if (!profiles.save(changed)) status = "Could not save all settings. Check storage.";
        if (changed.id == profiles.active()) visibility_changed = true;
        if (new_profile_flow) {
            // Without the owner's PIN nothing is locked, so a new Kid profile asks for one, once. Then its last step:
            // most games have no rating, and these wait for a decision.
            if (owner_pin().empty()) {
                pin_owner_id = "owner";
                open_pin(PinPurpose::Set, "");
                return;
            }
            new_profile_flow = false;
            open_profile_games(3);
            return;
        }
    } else if (profile) {
        profile_set_colour(*profile, index);
        dirty = true;
        return;
    }
    settings.page = Page::ProfileEdit;
    profile_edit_focus(profile_choice == ProfileChoice::Picture ? 3 : profile_choice == ProfileChoice::Age ? 6 : 4);
    dirty = true;
}

void App::profile_choose_action(int code) {
    int& focus = settings.focus;
    const int count = profile_choice_count(), wide = profile_choice == ProfileChoice::Kind      ? 2
                                                     : profile_choice == ProfileChoice::Picture ? 6
                                                                                                : 4;
    if (code == 3) {
        if (profile_choice == ProfileChoice::Kind) {
            settings.page = Page::Profiles;
            settings.focus = 0;
        } else {
            new_profile_flow = false;
            settings.page = Page::ProfileEdit;
            profile_edit_focus(profile_choice == ProfileChoice::Picture ? 3
                               : profile_choice == ProfileChoice::Age   ? 6
                                                                        : 4);
        }
    } else if (code == 2 || code == 4) choose_profile_option(focus);
    else if (code == 0 || code == 1 || code == 8 || code == 9) {
        const int next = focus + (code == 0 ? -1 : code == 1 ? 1 : code == 8 ? -wide : wide);
        if (next >= 0 && next < count) {
            focus = next;
            emit_sound(MenuCue::Navigate, code == 0 || code == 8 ? -1 : 1);
        }
    }
    dirty = true;
}

void App::profile_choose_touch(int x, int y) {
    for (int i = 0; i < profile_choice_count(); i++)
        if (point_in(profile_choice_box(i), x, y)) {
            settings.focus = i;
            choose_profile_option(i);
            return;
        }
}

void App::profile_choose_screen(SDL_Renderer* r, int y) {
    settings_header(r, y,
                    profile_choice == ProfileChoice::Kind      ? "Who is this profile for?"
                    : profile_choice == ProfileChoice::Picture ? "Picture"
                    : profile_choice == ProfileChoice::Age     ? "Age limit"
                                                               : "Colour");
    const auto* profile = edited_profile();
    for (int i = 0; i < profile_choice_count(); i++) {
        auto box = profile_choice_box(i);
        box.y += y;
        const bool focused = settings.focus == i;
        if (focused) {
            round_rect(r, box.x - 5, box.y - 5, box.w + 10, box.h + 10, 16, "ink");
            round_rect(r, box.x - 2, box.y - 2, box.w + 4, box.h + 4, 13, "background");
        }
        if (profile_choice == ProfileChoice::Kind) {
            settings_card(r, box.x, box.y, box.w, box.h);
            const char* title = i == 0 ? "Adult" : "Kid";
            const int width = label_width(title, 28);
            label(r, title, box.x + (box.w - width) / 2, box.y + 28, 28, "accent", box.w - 24);
            wrapped(r, i == 0 ? "Own saves and look. Settings stay open." : "Own saves, and a library you choose.",
                    box.x + 20, box.y + 84, 18, box.w - 40, 4);
        } else if (profile_choice == ProfileChoice::Picture) {
            Profile shown;
            if (profile) shown = *profile;
            shown.picture = i;
            shown.restricted = false; // the Kids mark belongs on the profile, not on each choice
            draw_profile_tile(r, shown, box.x, box.y, box.w);
            if (profile && profile->picture == i) chosen_tick(r, box.x + box.w - 14, box.y + 14);
        } else if (profile_choice == ProfileChoice::Age) {
            const int age = i == 15 ? no_age_limit : i == 14 ? only_picked_games : i + 3;
            raised_pill(r, box.x, box.y, box.w, box.h, 12, theme.at("panel"));
            const std::string text = age_limit_text(age);
            const int size = i >= 14 ? 14 : 24, width = std::min(box.w - 8, label_width(text, size, false));
            raw_label(r, text, box.x + (box.w - width) / 2, centred_label_y(text, size, box.y + box.h / 2), size, "ink",
                      box.w - 8);
        } else {
            const Color fill = ds_colours[size_t(i)].rgb;
            raised_pill(r, box.x, box.y, box.w, box.h, 12, fill);
            const int width = label_width(ds_colours[size_t(i)].name, 16);
            label_colour(r, ds_colours[size_t(i)].name, box.x + (box.w - width) / 2,
                         centred_label_y(locale.text(ds_colours[size_t(i)].name), 16, box.y + box.h / 2), 16,
                         tile_ink(fill), box.w - 12);
            if (profile && profile->colour == i) chosen_tick(r, box.x + box.w - 12, box.y + 12);
        }
    }
}

// The one in use: a white disc with the accent tick, on its tile's corner.
void App::chosen_tick(SDL_Renderer* r, int cx, int cy) {
    alpha_round(r, cx - 11, cy - 9, 22, 22, 11, shadow_colour(), 70);
    fill_round(r, cx - 11, cy - 11, 22, 22, 11, {255, 255, 255});
    ui_icon(r, SettingsIcon::Check, cx, cy, 14, "accent");
}

void App::profile_choose_top(SDL_Renderer* r, int y) {
    const auto* profile = edited_profile();
    if (profile_choice == ProfileChoice::Kind || !profile) {
        const int top = y + top_card_y + (top_card_h - 240) / 2;
        settings_card(r, 24, top, 592, 240);
        label(r, settings.focus == 1 ? "Kid" : "Adult", 48, top + 30, 28, "accent", 544);
        wrapped(r,
                settings.focus == 1 ? "A library that fits their age, and no way into the handheld's settings."
                                    : "A second grown-up or an older child, with everything open.",
                48, top + 84, 20, 544, 4);
        return;
    }
    Profile shown = *profile;
    if (profile_choice == ProfileChoice::Age) {
        const int top = y + top_card_y + (top_card_h - 240) / 2;
        const int age = settings.focus == 15   ? no_age_limit
                        : settings.focus == 14 ? only_picked_games
                                               : settings.focus + 3;
        settings_card(r, 24, top, 592, 240);
        raw_label(r, age_limit_text(age), 48, top + 30, 28, "accent", 544);
        wrapped(
            r,
            age == no_age_limit ? "Every game shows, whatever its rating."
            : age == only_picked_games
                ? "Nothing shows until you allow it, game by game or a shelf at a time."
                : "Games rated above this age are hidden. Games with no rating follow the setting on this profile's page.",
            48, top + 84, 20, 544, 4);
        return;
    }
    if (profile_choice == ProfileChoice::Picture) shown.picture = settings.focus;
    else shown.colour = settings.focus;
    profile_card_top(r, y, shown);
}

// ---- The worker that removes one ------------------------------------------------------------------------------------

void App::remove_profile() {
    if (worker > 0) {
        status = "One moment.";
        dirty = true;
        return;
    }
    std::ostringstream body;
    body << "{\"action\":\"profile-remove\",\"id\":\"" << profile_editing << "\"}";
    profile_removing = true;
    start_worker("device.library", body.str());
    settings.page = Page::Profiles;
    settings.focus = 0;
    dirty = true;
}

void App::erase_removed_profiles() {
    if (worker > 0) {
        status = "One moment.";
        dirty = true;
        return;
    }
    profile_erasing = true;
    start_worker("device.library", "{\"action\":\"profile-erase\"}");
    dirty = true;
}

void App::profile_removal_finished(bool ok) {
    profile_removing = false;
    profiles.load(o.data);
    status = ok ? "Profile removed. A backup is in userdata/exports." : "The profile could not be removed.";
    settings.focus = 0;
    dirty = true;
}

void App::profile_erasing_finished(bool ok) {
    profile_erasing = false;
    profiles.load(o.data);
    status = ok ? "Removed profiles erased." : "Removed profiles could not be erased.";
    if (settings.page == Page::ProfileEdit) profile_edit_focus(14);
    dirty = true;
}

// ---- Who sees what -------------------------------------------------------------------------------------------------

// Fills all_games with what this player may see, from catalog_games. Every list derives from all_games: Home's counts,
// the systems, search, filters, Recents and the switcher. With a rule that needs ratings nothing shows until the worker
// has them: the games must not flash up before they are filtered.
void App::apply_visibility() {
    if (o.game_menu) return;
    const auto& list = profiles.profiles();
    active_rules = list.empty() ? VisibilityRules{} : visibility_rules(list[size_t(profiles.active_index())]);
    visibility_reader.cancel();
    visibility_loading = false;
    all_games = catalog_games;
    listed_systems = catalog_listed;
    if (active_rules.restricted && !active_rules.allow_ports) port_entry.reset();
    if (active_rules.open()) return;
    if (active_rules.needs_ratings()) {
        all_games.clear();
        visibility_loading = true;
        visibility_reader.request(o.data, catalog_games);
    } else {
        all_games.erase(std::remove_if(all_games.begin(), all_games.end(),
                                       [&](const Game& game) {
                                           return !game_visible(active_rules, game_key(game.path), game.system, "");
                                       }),
                        all_games.end());
    }
    keep_visible_systems();
}

void App::keep_visible_systems() {
    std::vector<std::string> kept;
    for (const auto& id : catalog_listed) {
        const bool shown =
            id == "nds" || (id == "ports" && port_entry) ||
            (id == "homebrew" && (!active_rules.restricted || active_rules.allow_homebrew)) ||
            std::any_of(all_games.begin(), all_games.end(), [&](const Game& g) { return g.system == id; });
        if (shown) kept.push_back(id);
    }
    listed_systems = std::move(kept);
    if (std::find(listed_systems.begin(), listed_systems.end(), shown_system) == listed_systems.end())
        shown_system = "nds";
}

void App::poll_visibility() {
    if (auto entries = visibility_reader.poll()) {
        all_games.clear();
        for (auto& entry : *entries)
            if (game_visible(active_rules, entry.key, entry.game.system, entry.rating))
                all_games.push_back(std::move(entry.game));
        visibility_loading = false;
        keep_visible_systems();
        summary_ready = false;
        library_summary.request(o.data, all_games);
        if (settings.page == Page::Library && systems_menu) open_systems();
        else if (settings.page == Page::Library && !home_menu) library_view(preferences.view);
        dirty = true;
    }
    if (auto entries = games_reader.poll()) {
        games_entries = std::move(*entries);
        games_entries.erase(std::remove_if(games_entries.begin(), games_entries.end(),
                                           [](const CatalogReader::Entry& e) { return is_tool_entry(e.game.path); }),
                            games_entries.end());
        games_rows_build();
        dirty = true;
    }
    // A rule changed for whoever is playing: their lists follow once they are back at them, never under a page that is
    // still showing the game.
    if (visibility_changed && settings.page == Page::Library && !launching) {
        visibility_changed = false;
        apply_visibility();
        if (!visibility_loading) {
            summary_ready = false;
            library_summary.request(o.data, all_games);
            if (systems_menu) open_systems();
            else if (!home_menu) library_view(preferences.view);
        }
        dirty = true;
    }
}

// The last check, for any way to a game that a list did not catch.
bool App::game_allowed(const Game& game) {
    if (game.demo || active_rules.open()) return true;
    if (is_tool_entry(game.path)) return !active_rules.restricted || active_rules.allow_ports;
    return game_visible(active_rules, game_key(game.path), game.system,
                        fields(o.data / "library" / game_key(game.path) / "info.ini")["rating"]);
}

// With the Hidden games filter: what these rules keep from this player. Never on a Kid profile.
std::vector<Game> App::hidden_games() const {
    std::vector<Game> hidden;
    if (active_rules.restricted || active_rules.open()) return hidden;
    std::set<std::string> shown;
    for (const auto& game : all_games) shown.insert(game.path.string());
    for (const auto& game : catalog_games)
        if (!shown.count(game.path.string())) hidden.push_back(game);
    return hidden;
}

void App::set_game_override(const Profile& profile, const std::string& key, const std::string& state) {
    auto overrides = load_visibility(profile.root);
    if (state.empty()) overrides.erase(key);
    else overrides[key] = state;
    if (!profile_write_fields(profile.root / "visibility.ini", overrides))
        status = "Could not save all settings. Check storage.";
    if (profile.id == profiles.active()) visibility_changed = true;
}

// ---- A profile's games --------------------------------------------------------------------------------------------
// Every game with a box to tick: ticked is shown on this profile. Three buttons narrow the list (a search, one system,
// shown or hidden), and a shelf's own box ticks what is listed under it. A tick never moves a row: the list is only
// sorted again when a filter changes, so a game unticked under Shown stays there, unticked, until then.

void App::open_profile_games(int tab) {
    games_tab = tab;
    games_query.clear();
    games_system.clear();
    games_rows.clear();
    games_entries.clear();
    games_rules = VisibilityRules{};
    if (const auto* profile = edited_profile()) games_rules = visibility_rules(*profile);
    // Every system's folders: this lists what any profile could see, even with other systems switched off here.
    games_reader.request(o.data, ds_games, o.system_roots);
    settings.page = Page::ProfileGames;
    settings.focus = 0;
    list_scroll = 0;
    page_at = s.GetTicks();
    dirty = true;
}

bool App::games_match(const CatalogReader::Entry& entry, const std::string& query) const {
    if (!games_system.empty() && entry.game.system != games_system) return false;
    return query.empty() || lower(library_title(entry.game.title)).find(lower(query)) != std::string::npos;
}

bool App::games_entry_visible(const CatalogReader::Entry& entry) const {
    return game_visible(games_rules, entry.key, entry.game.system, entry.rating);
}

// Not to be ticked at all: a port on a Kid profile with Ports off.
bool App::games_entry_locked(const CatalogReader::Entry& entry) const {
    return games_rules.restricted && entry.game.system == "ports" && !games_rules.allow_ports;
}

// What a whole shelf may let through at once: never a game rated above the age. One at a time it can be.
bool App::games_entry_fits(const CatalogReader::Entry& entry) const {
    return !games_entry_locked(entry) &&
           (games_rules.age < 0 || entry.age < 0 || entry.age <= games_rules.age); // no limit, only picked, unrated
}

void App::games_rows_build() {
    games_rows.clear();
    std::vector<int> order(games_entries.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = int(i);
    const auto rank = [](const std::string& system) {
        int at = 0;
        for (const auto& info : systems()) {
            if (system == info.id) return at;
            at++;
        }
        return at;
    };
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        const auto& x = games_entries[size_t(a)].game;
        const auto& y = games_entries[size_t(b)].game;
        return rank(x.system) != rank(y.system) ? rank(x.system) < rank(y.system) : lower(x.title) < lower(y.title);
    });
    std::string shelf;
    for (int index : order) {
        const auto& entry = games_entries[size_t(index)];
        if (!games_match(entry, games_query)) continue;
        const bool visible = games_entry_visible(entry);
        const bool wanted = games_tab == 0 || (games_tab == 1 && visible) || (games_tab == 2 && !visible) ||
                            (games_tab == 3 && entry.age < 0);
        if (!wanted) continue;
        if (entry.game.system != shelf) {
            shelf = entry.game.system;
            games_rows.push_back({-1, shelf});
        }
        games_rows.push_back({index, shelf});
    }
    settings.focus = std::clamp(settings.focus, 0, std::max(0, int(games_rows.size()) - 1));
    dirty = true;
}

// A filter changed: the list starts again from the top.
void App::games_refilter() {
    settings.focus = 0;
    list_scroll = 0;
    games_rows_build();
}

// Shown and listed, of the games under one shelf's heading as the list stands.
std::pair<int, int> App::games_shelf_counts(const std::string& shelf) const {
    int shown = 0, listed = 0;
    for (const auto& row : games_rows)
        if (row.entry >= 0 && row.shelf == shelf) {
            listed++;
            shown += games_entry_visible(games_entries[size_t(row.entry)]);
        }
    return {shown, listed};
}

// One game shown or hidden: stored only where it differs from what the age and the no-rating rule decide.
void App::games_set(std::map<std::string, std::string>& overrides, const CatalogReader::Entry& entry, bool show) {
    VisibilityRules automatic = games_rules;
    automatic.overrides.clear();
    if (game_visible(automatic, entry.key, entry.game.system, entry.rating) == show) overrides.erase(entry.key);
    else overrides[entry.key] = show ? "show" : "hide";
}

void App::games_save(std::map<std::string, std::string> overrides) {
    const auto* profile = edited_profile();
    if (!profile) return;
    if (!profile_write_fields(profile->root / "visibility.ini", overrides))
        status = "Could not save all settings. Check storage.";
    if (profile->id == profiles.active()) visibility_changed = true;
    games_rules.overrides = std::move(overrides);
}

void App::games_toggle(int index) {
    if (index < 0 || index >= int(games_rows.size())) return;
    const auto row = games_rows[size_t(index)];
    if (row.entry < 0) {
        games_shelf = row.shelf;
        games_focus = index;
        // A shelf with every fitting game ticked is unticked at once; ticking a shelf asks first.
        bool all = true;
        for (const auto& other : games_rows)
            if (other.entry >= 0 && other.shelf == row.shelf) {
                const auto& entry = games_entries[size_t(other.entry)];
                if (games_entry_fits(entry) && !games_entry_visible(entry)) all = false;
            }
        if (all) set_shelf(false);
        else confirm("profile-allow-shelf", Page::ProfileGames);
        return;
    }
    const auto& entry = games_entries[size_t(row.entry)];
    if (games_entry_locked(entry)) {
        status = "Ports are off on this profile.";
        emit_sound(MenuCue::Error);
        return;
    }
    const bool show = !games_entry_visible(entry);
    auto overrides = games_rules.overrides;
    games_set(overrides, entry, show);
    games_save(std::move(overrides));
    emit_sound(show ? MenuCue::Enable : MenuCue::Disable);
}

// Every game listed under games_shelf, shown (those that fit the age) or hidden.
void App::set_shelf(bool show) {
    auto overrides = games_rules.overrides;
    for (const auto& row : games_rows)
        if (row.entry >= 0 && row.shelf == games_shelf) {
            const auto& entry = games_entries[size_t(row.entry)];
            if (!show || games_entry_fits(entry)) games_set(overrides, entry, show);
        }
    games_save(std::move(overrides));
    settings.focus = games_focus;
    emit_sound(show ? MenuCue::Enable : MenuCue::Disable);
    dirty = true;
}

void App::allow_shelf() { set_shelf(true); }

void App::games_pick_system() {
    std::map<std::string, int> counts;
    for (const auto& entry : games_entries)
        if (games_query.empty() || games_match(entry, games_query)) counts[entry.game.system]++;
    std::vector<std::string> names = {locale.text("All systems")}, ids = {""};
    int selected = 0;
    for (const auto& info : systems()) {
        const auto found = counts.find(info.id);
        if (found == counts.end()) continue;
        if (games_system == info.id) selected = int(ids.size());
        names.push_back(std::string(info.name) + " (" + std::to_string(found->second) + ")");
        ids.push_back(info.id);
    }
    show_picker("System", names, selected, [this, ids](int n) {
        if (n < 0 || n >= int(ids.size()) || ids[size_t(n)] == games_system) return;
        games_system = ids[size_t(n)];
        games_refilter();
    });
}

void App::games_pick_view() {
    show_picker("Showing", {locale.text("All"), locale.text("Shown"), locale.text("Hidden"), locale.text("No rating")},
                games_tab, [this](int n) {
                    if (n == games_tab) return;
                    games_tab = n;
                    games_refilter();
                });
}

int App::games_match_count(const std::string& query) const {
    const auto wanted = trimmed_query(query);
    return int(std::count_if(games_entries.begin(), games_entries.end(),
                             [&](const CatalogReader::Entry& entry) { return games_match(entry, wanted); }));
}

void App::profile_games_action(int code) {
    status.clear();
    if (code == 3) {
        if (!games_query.empty()) { // B first clears a search, as on the Cheats page
            games_query.clear();
            games_refilter();
            return;
        }
        const auto rows = profile_edit_rows();
        const auto at = std::find(rows.begin(), rows.end(), 8);
        games_reader.cancel();
        settings.page = Page::ProfileEdit;
        settings.focus = at == rows.end() ? 0 : int(at - rows.begin());
    } else if (code == 13 || code == 14) {
        const int next = std::clamp(games_tab + (code == 13 ? -1 : 1), 0, 3);
        if (next != games_tab) {
            games_tab = next;
            games_refilter();
        }
    } else if (code == 11) begin_edit(11);
    else if (code == 12) games_pick_system();
    else if ((code == 8 || code == 9) && !games_rows.empty()) {
        settings.focus = std::clamp(settings.focus + (code == 8 ? -1 : 1), 0, int(games_rows.size()) - 1);
    } else if ((code == 2 || code == 4) && settings.focus < int(games_rows.size())) games_toggle(settings.focus);
    dirty = true;
}

bool App::profile_games_header_hit(int x, int y) const { return y >= 24 && y < 66 && x >= games_pill_x[0]; }

void App::profile_games_header_tap(int x) {
    if (x >= games_pill_x[2]) games_pick_view();
    else if (x >= games_pill_x[1]) games_pick_system();
    else begin_edit(11);
    dirty = true;
}

void App::profile_games_header(SDL_Renderer* r, int y) {
    const bool pressing = pointer_down && profile_games_header_hit(contact_x, contact_y);
    const std::array<const char*, 4> views = {"All", "Shown", "Hidden", "No rating"};
    const std::array<std::string, 3> texts = {games_query.empty() ? locale.text("Search") : games_query,
                                              games_system.empty() ? locale.text("System")
                                                                   : std::string(system_info(games_system).badge),
                                              locale.text(views[size_t(std::clamp(games_tab, 0, 3))])};
    const std::array<bool, 3> narrowed = {!games_query.empty(), !games_system.empty(), games_tab != 0};
    const std::array<const char*, 3> keys = {"X", "Y", "R"};
    for (int i = 0; i < 3; i++) {
        const int left = games_pill_x[size_t(i)], right = i < 2 ? games_pill_x[size_t(i) + 1] - 8 : 616;
        hotkey_pill(r, left, y + 24, right - left, 42, texts[size_t(i)],
                    narrowed[size_t(i)] ? KeyLook::Accent : KeyLook::Panel,
                    pressing && contact_x >= left && contact_x < right, keys[size_t(i)], false);
    }
}

// A ticked box (accent, with the tick), an empty one, or a dash for a shelf that is partly ticked.
void App::tick_box(SDL_Renderer* r, int x, int y, int size, int state, bool enabled) {
    if (state == 0) {
        fill_round(r, x, y, size, size, size / 4, theme.at(enabled ? "ink" : "line"));
        fill_round(r, x + 3, y + 3, size - 6, size - 6, size / 4 - 2, theme.at("panel"));
        return;
    }
    const Color accent = theme.at("accent");
    glossy_pill(r, x, y, size, size, size / 4, enabled ? accent : theme.at("muted"));
    if (state == 2) ui_icon(r, SettingsIcon::Check, x + size / 2, y + size / 2, size * 5 / 8, "on_accent");
    else fill_round(r, x + size / 4, y + size / 2 - 3, size / 2, 6, 3, tile_ink(accent));
}

void App::profile_games_row(SDL_Renderer* r, int y, int i) {
    const auto& row = games_rows[size_t(i)];
    int top = y + list_top + i * list_stride - list_scroll - list_shift();
    const bool focused = i == list_selected();
    int dx = 24;
    move_element(uint32_t(i), dx, top, 566, 66, focused, list_top, list_top + list_height, 20, 595);
    dx -= 24;
    settings_card(r, 24 + dx, top, 566, 66, focused);
    if (row.entry < 0) {
        // The shelf: its box, its system's picture, its name and how many of its listed games are shown.
        const auto [shown, listed] = games_shelf_counts(row.shelf);
        tick_box(r, 40 + dx, top + 15, 36, shown == 0 ? 0 : shown == listed ? 2 : 1, true);
        const std::string id = row.shelf;
        if (!(preferences.rich_icons &&
              media.draw(r, asset_path("systems/" + id + ".bmp"), {92 + dx, top + 9, 48, 48})))
            draw_system_pixel(s, r, id, 116 + dx, top + 33, 40, theme.at("accent"));
        const std::string name = system_info(id).name;
        raw_label(r, name, 154 + dx, top + 10, 24, "ink", 400);
        raw_label(r, locale.text(std::to_string(shown) + " / " + std::to_string(listed) + " shown"), 154 + dx, top + 40,
                  14, "muted", 400);
        return;
    }
    const auto& entry = games_entries[size_t(row.entry)];
    const bool visible = games_entry_visible(entry), locked = games_entry_locked(entry);
    tick_box(r, 40 + dx, top + 15, 36, visible ? 2 : 0, !locked);
    request_icon(r, entry.game);
    game_icon(r, entry.game, 92 + dx, top + 9, 48);
    // Its rating on the right: the age it is for, in the accent when the age lets it in, muted when it does not.
    const bool fits = entry.age >= 0 && (games_rules.age < 0 || entry.age <= games_rules.age);
    const std::string badge = entry.age >= 0 ? std::to_string(entry.age) + "+" : locale.text("No rating");
    const int badge_w = label_width(badge, 16, false) + 24;
    round_rect(r, 574 - badge_w + dx, top + 18, badge_w, 30, 15, entry.age >= 0 && fits ? "accent" : "slot");
    raw_label(r, badge, 586 - badge_w + dx, centred_label_y(badge, 16, top + 33), 16,
              entry.age >= 0 && fits ? "on_accent" : "muted", badge_w);
    const int room = 574 - badge_w - 12 - 154;
    raw_label(r, library_title(entry.game.title), 154 + dx, top + 10, 22, visible ? "ink" : "muted", room);
    const auto found = games_rules.overrides.find(entry.key);
    const std::string why = found != games_rules.overrides.end()
                                ? locale.text(found->second == "show" ? "Picked by you" : "Hidden by you")
                                : "";
    std::string line = system_info(entry.game.system).name;
    if (!why.empty()) line += "  ·  " + why;
    raw_label(r, line, 154 + dx, top + 40, 14, "muted", room);
}

void App::profile_games_top(SDL_Renderer* r, int y) {
    const int top = y + top_card_y;
    settings_card(r, 24, top, 592, 266);
    const auto* profile = edited_profile();
    const std::string who = profile ? profile->name : "";
    if (games_reader.pending()) {
        busy_ring(r, 320, top + 133, 26, 4);
        return;
    }
    std::vector<KeyHint> keys = {{"X", "Search"}, {"Y", "System"}, {"L R", "Showing"}};
    if (settings.focus >= int(games_rows.size())) {
        wrapped(r, games_query.empty() ? "No games here." : "No games match. Try a shorter word.", 48, top + 110, 22,
                544, 2);
    } else if (const auto& row = games_rows[size_t(settings.focus)]; row.entry < 0) {
        const auto [shown, listed] = games_shelf_counts(row.shelf);
        raw_label(r, system_info(row.shelf).name, 48, top + 30, 30, "accent", 544);
        raw_label(r, locale.text(std::to_string(shown) + " / " + std::to_string(listed) + " shown"), 48, top + 80, 20,
                  "ink", 544);
        wrapped(r,
                shown == listed && listed > 0
                    ? "A hides every game listed under this shelf."
                    : "A shows every game listed under this shelf. Games rated above the age stay hidden; tick them "
                      "one by one if you want them.",
                48, top + 124, 18, 544, 4);
        keys.insert(keys.begin(), KeyHint{"A", shown == listed && listed > 0 ? "Hide all" : "Show all"});
    } else {
        const auto& entry = games_entries[size_t(row.entry)];
        const auto folder = folder_for_game(entry.game);
        // The cover, or failing that the screenshot or title screen, as the library shows them.
        bool drawn = false;
        for (const char* picture : {"box.bmp", "snap.bmp", "title.bmp"})
            if (media.warm(r, folder / picture)) {
                cover_art(r, folder / picture, {48, top + 28, 180, 180}, false);
                drawn = true;
                break;
            }
        if (!drawn) {
            warm_icon(r, entry.game);
            game_icon(r, entry.game, 66, top + 46, 144);
        }
        raw_wrapped(r, library_title(entry.game.title), 252, top + 28, 24, 340, 2);
        raw_label(r, system_info(entry.game.system).name, 252, top + 92, 16, "muted", 340);
        label(r, entry.age >= 0 ? entry.rating : "No age rating", 252, top + 116, 18, "accent", 340);
        const auto found = games_rules.overrides.find(entry.key);
        const bool visible = games_entry_visible(entry);
        const char* why = games_entry_locked(entry)              ? "Ports are off on this profile."
                          : found != games_rules.overrides.end() ? (visible ? "You picked it." : "You hid it.")
                          : entry.age >= 0 && !visible           ? "Above this profile's age."
                          : entry.age < 0 && !visible            ? "No rating: hidden until you tick it."
                                                                 : "Fits this profile's age.";
        raw_label(r, locale.text(visible ? "Shown" : "Hidden"), 252, top + 152, 22, visible ? "ink" : "muted", 340);
        wrapped(r, why, 252, top + 184, 18, 340, 3);
        keys.insert(keys.begin(), KeyHint{"A", visible ? "Hide" : "Show"});
    }
    if (!who.empty()) raw_label(r, who, 48, top + 236, 14, "muted", 544);
    key_hints(r, 320, y + top_card_y + 300, keys, 0);
}

// ---- One game: who sees it ---------------------------------------------------------------------------------------------

int App::game_visibility_row() const {
    if (active_rules.restricted || model.games.empty() || model.selected >= model.games.size() ||
        model.games[model.selected].demo)
        return -1;
    return game_options_base() + (game_removable() ? 1 : 0);
}

void App::open_game_visibility() {
    const auto& game = model.games[model.selected];
    const auto key = game_key(game.path);
    if (!profiles.enabled()) {
        // One person: only a choice to hide it. It stays in the Hidden games filter, to bring it back from.
        const auto& me = profiles.profiles()[size_t(profiles.active_index())];
        const bool hidden = load_visibility(me.root).count(key) > 0;
        set_game_override(me, key, hidden ? "" : "hide");
        emit_sound(hidden ? MenuCue::Enable : MenuCue::Disable);
        dirty = true;
        return;
    }
    settings.page = Page::ProfileGameRules;
    settings.focus = 0;
    list_scroll = 0;
    page_at = s.GetTicks();
    dirty = true;
}

void App::profile_game_rules_action(int code) {
    const auto& list = profiles.profiles();
    if (code == 3) {
        settings.page = Page::GameOptions;
        settings.focus = game_visibility_row();
    } else if (code == 8 || code == 9) {
        settings.focus = std::clamp(settings.focus + (code == 8 ? -1 : 1), 0, int(list.size()) - 1);
    } else if ((code == 2 || code == 4) && settings.focus < int(list.size())) {
        const auto& profile = list[size_t(settings.focus)];
        const auto key = game_key(model.games[model.selected].path);
        const auto overrides = load_visibility(profile.root);
        const auto found = overrides.find(key);
        const std::string next = found == overrides.end() ? "show" : found->second == "show" ? "hide" : "";
        set_game_override(profile, key, next);
        emit_sound(next == "show" ? MenuCue::Enable : next == "hide" ? MenuCue::Disable : MenuCue::Confirm);
    }
    dirty = true;
}

void App::profile_game_rules_row(SDL_Renderer* r, int y, int i) {
    const auto& profile = profiles.profiles()[size_t(i)];
    const auto& game = model.games[model.selected];
    const auto key = game_key(game.path);
    const auto rules = visibility_rules(profile);
    const auto found = rules.overrides.find(key);
    const bool visible = game_visible(rules, key, game.system, library_cache.metadata(game).info["rating"]);
    list_row(r, y, i, profile.name,
             found != rules.overrides.end() ? locale.text(found->second == "show" ? "Always show" : "Always hide")
                                            : locale.text(visible ? "Shown" : "Hidden"));
}

void App::profile_game_rules_top(SDL_Renderer* r, int y) {
    const int top = y + top_card_y + (top_card_h - 240) / 2;
    settings_card(r, 24, top, 592, 240);
    if (model.games.empty()) return;
    const auto& game = model.games[model.selected];
    cover_art(r, folder_for_game(game) / "box.bmp", {48, top + 32, 176, 176}, false);
    raw_wrapped(r, library_title(game.title), 248, top + 30, 24, 344, 3);
    const auto rating = library_cache.metadata(game).info["rating"];
    label(r, minimum_age(rating) >= 0 ? rating : "No age rating", 248, top + 126, 18, "accent", 344);
    wrapped(r, "A changes who sees this game: automatic, always show, always hide.", 248, top + 158, 18, 344, 3);
}

// ---- The lock ----------------------------------------------------------------------------------------------------------

const Profile& App::active_profile() const { return profiles.profiles()[size_t(profiles.active_index())]; }

const std::string& App::owner_pin() const { return profiles.profiles().front().pin; }

// What a Kid profile may open without the owner's PIN. A list of what is open, not of what is closed: a page nobody thought
// of here asks for the PIN, rather than letting anyone in.
bool App::page_open_to_kid(Page page) const {
    if (o.game_menu) return page != Page::Cheats || active_profile().allow_cheats;
    switch (page) {
    case Page::Library:
    case Page::Settings:
    case Page::Appearance:
    case Page::Colours:
    case Page::DisplayOptions:
    case Page::Display:
    case Page::Motion:
    case Page::Sound:
    case Page::MenuSounds:
    case Page::Haptics:
    case Page::Microphone:
    case Page::Statistics:
    case Page::Profiles:
    case Page::ProfilePicker:
    case Page::PinPad:
    case Page::GameOptions:
    case Page::GameInfo:
    case Page::ShaderPicker:
    case Page::ApplyShader:
    case Page::GameSwitcher:
    case Page::BootWarning: return true;
    case Page::Welcome: return welcome_active;
    case Page::Keyboard: return edit_field == 4 || edit_field == 5; // search, and a game's own DS name
    case Page::Confirm: return confirmation == "poweroff";
    default: return false;
    }
}

bool App::section_open_to_kid(int section) {
    static constexpr std::array<int, 6> open = {0, 3,  7,
                                                8, 10, 11}; // Theme, Sound, Statistics, Power off, Display, Profiles
    return std::find(open.begin(), open.end(), section) != open.end();
}

bool App::pictochat_hidden() const {
    return !profiles.profiles().empty() && active_profile().restricted && !active_profile().allow_pictochat;
}

bool App::kid_locked() const {
    return !profiles.profiles().empty() && active_profile().restricted && !settings_unlocked;
}

// The one checkpoint, before each frame is drawn: however a page was reached (a row, a shortcut, a restored state, a key
// nobody planned for), a page this player may not see is not drawn. It asks for the PIN, and goes on to that page.
void App::enforce_lock(Uint32 now) {
    if (settings.page == Page::PinPad) {
        if (pin_wait_left() > 0 && now - pin_ticked >= 1000) {
            pin_ticked = now;
            dirty = true;
        }
        return;
    }
    if (profiles.profiles().empty() || active_profile().owner()) return;
    const auto& who = active_profile();
    if (who.restricted && !o.game_menu && !o.preview && device_status["sharing"] == "1" && worker <= 0 &&
        now - sharing_stopped_at > 5000) {
        // Left on from before: the card should not be open on the network on a child's profile.
        sharing_stopped_at = now;
        start_worker("device.sharing-off");
    }
    if (settings.page == Page::Library && !chat_open) settings_unlocked = false;
    const Page page = settings.page;
    const bool managing = page == Page::ProfileEdit || page == Page::ProfileChoose || page == Page::ProfileGames ||
                          page == Page::ProfileGameRules;
    const bool closed = who.restricted ? !page_open_to_kid(page) : managing;
    if (!closed || settings_unlocked) {
        last_open_page = page;
        return;
    }
    if (o.game_menu) {
        // No PIN pad over a running game: what is off stays off.
        settings.page = Page::QuickMenu;
        settings.focus = 0;
        status = "Not on this profile";
        dirty = true;
        return;
    }
    if (owner_pin().empty()) return; // no PIN set: this profile is not locked, and its page says so
    pin_target_page = page;
    pin_target_focus = settings.focus;
    open_pin(PinPurpose::Unlock, owner_pin(), page == Page::Keyboard);
}

long App::pin_wait_left() const {
    auto state = profile_fields(o.data / "profiles/pin-state.ini");
    const long until = std::atol(state["until"].c_str()), left = until - long(std::time(nullptr));
    return left > 0 ? left : 0;
}

void App::pin_note_wrong() {
    auto state = profile_fields(o.data / "profiles/pin-state.ini");
    const int tries = std::atoi(state["tries"].c_str()) + 1;
    state["tries"] = std::to_string(tries);
    const long wait = pin_wait_after(tries);
    if (wait) state["until"] = std::to_string(long(std::time(nullptr)) + wait);
    // Kept on the card: a restart must not give more tries.
    profile_write_fields(o.data / "profiles/pin-state.ini", state);
}

void App::open_pin(PinPurpose purpose, const std::string& expected, bool stop_typing) {
    if (stop_typing) s.StopTextInput();
    pin_purpose = purpose;
    pin_expected = expected;
    pin_digits.clear();
    pin_message.clear();
    settings.page = Page::PinPad;
    settings.focus = 0;
    pin_ticked = s.GetTicks();
    page_at = pin_ticked;
    dirty = true;
}

void App::pin_cancel() {
    pin_digits.clear();
    if (pin_purpose == PinPurpose::Enter) {
        settings.page = Page::ProfilePicker;
        settings.focus = pin_profile;
    } else if (pin_purpose == PinPurpose::Unlock) {
        settings.page = last_open_page == Page::PinPad ? Page::Library : last_open_page;
        settings.focus = 0;
    } else if (new_profile_flow) {
        // Skipped: the profile is not locked until there is one, and its page says so.
        new_profile_flow = false;
        open_profile_games(3);
    } else {
        settings.page = Page::ProfileEdit;
        profile_edit_focus(14);
    }
    dirty = true;
}

void App::pin_submit() {
    if (pin_purpose == PinPurpose::Set) {
        pin_first = pin_digits;
        pin_digits.clear();
        pin_purpose = PinPurpose::Confirm;
        pin_message = "Enter it again";
        return;
    }
    if (pin_purpose == PinPurpose::Confirm) {
        if (pin_digits != pin_first) {
            pin_digits.clear();
            pin_first.clear();
            pin_purpose = PinPurpose::Set;
            pin_message = "Those did not match. Choose a PIN.";
            pin_shake();
            emit_sound(MenuCue::Error);
            return;
        }
        const auto stored = pin_store(pin_digits);
        pin_digits.clear();
        pin_first.clear();
        for (const auto& profile : profiles.profiles())
            if (profile.id == pin_owner_id) {
                Profile changed = profile;
                changed.pin = stored;
                if (!profiles.save(changed)) status = "Could not save all settings. Check storage.";
                break;
            }
        emit_sound(MenuCue::Confirm);
        if (new_profile_flow) {
            new_profile_flow = false;
            open_profile_games(3);
            return;
        }
        settings.page = Page::ProfileEdit;
        profile_edit_focus(14);
        return;
    }
    if (pin_wait_left() > 0) return;
    if (!pin_matches(pin_expected, pin_digits)) {
        pin_digits.clear();
        pin_note_wrong();
        const long wait = pin_wait_left();
        pin_message = wait ? "" : "That is not the PIN.";
        pin_shake();
        emit_sound(MenuCue::Error);
        return;
    }
    pin_digits.clear();
    std::error_code ec;
    fs::remove(o.data / "profiles/pin-state.ini", ec);
    emit_sound(MenuCue::Confirm);
    if (pin_purpose == PinPurpose::Enter) {
        commit_profile(pin_profile);
        return;
    }
    settings_unlocked = true;
    settings.page = pin_target_page;
    settings.focus = pin_target_focus;
    if (pin_target_page == Page::Keyboard) s.StartTextInput();
}

// A key lit by a press, by touch or by A: it fades back over a quarter of a second, as a phone's passcode keys do.
void App::pin_flash(int key) {
    pin_flash_key = key;
    pin_flash_at = s.GetTicks();
    micro_until = std::max(micro_until, pin_flash_at + pin_flash_ms + 20);
}

// Wrong: the dots shake, red, and then empty.
void App::pin_shake() {
    pin_shake_dots = 4;
    pin_shake_at = s.GetTicks();
    micro_until = std::max(micro_until, pin_shake_at + pin_shake_ms + 20);
}

void App::pin_press(int key) {
    pin_flash(key);
    if (key == 9) return pin_cancel();
    if (key == 11) {
        if (pin_digits.empty()) return;
        pin_digits.pop_back();
        emit_sound(MenuCue::Back);
        dirty = true;
        return;
    }
    const bool checking = pin_purpose == PinPurpose::Enter || pin_purpose == PinPurpose::Unlock;
    if (checking && pin_wait_left() > 0) {
        emit_sound(MenuCue::Error);
        return;
    }
    pin_digits += char('0' + (key == 10 ? 0 : key + 1));
    pin_message.clear();
    pin_pop_at = s.GetTicks();
    emit_sound(MenuCue::Navigate);
    if (pin_digits.size() == 4) pin_submit();
    dirty = true;
}

// 1 to 9 in three rows, then Cancel, 0 and Delete: round keys on a 128 x 80 grid, centred on the lower screen.
SDL_Rect App::pin_key_box(int key) const {
    const int cx = 192 + (key % 3) * 128, cy = 124 + (key / 3) * 80;
    return {cx - pin_key_radius - 4, cy - pin_key_radius - 4, 2 * pin_key_radius + 8, 2 * pin_key_radius + 8};
}

void App::pin_action(int code) {
    int& focus = settings.focus;
    if (code == 3) {
        if (pin_digits.empty()) pin_cancel();
        else pin_press(11);               // B steps back one digit first, then leaves
    } else if (code == 11) pin_press(11); // X: delete one
    else if (code == 2 || code == 4) pin_press(focus);
    else if (code == 0 && focus % 3 > 0) focus--;
    else if (code == 1 && focus % 3 < 2) focus++;
    else if (code == 8 && focus >= 3) focus -= 3;
    else if (code == 9 && focus < 9) focus += 3;
    dirty = true;
}

void App::pin_touch(int x, int y) {
    for (int key = 0; key < 12; key++)
        if (point_in(pin_key_box(key), x, y)) {
            settings.focus = key;
            pin_press(key);
            return;
        }
}

void App::pin_screen(SDL_Renderer* r, int y) {
    settings_header(r, y, "PIN");
    static const std::array<const char*, 12> letters = {"",     "ABC", "DEF",  "GHI", "JKL", "MNO",
                                                        "PQRS", "TUV", "WXYZ", "",    "",    ""};
    const Uint32 now = s.GetTicks();
    const Color accent = theme.at("accent"), panel = theme.at("panel");
    const auto blend = [](Color a, Color b, float t) {
        return Color{(unsigned char)(a.r + (b.r - a.r) * t), (unsigned char)(a.g + (b.g - a.g) * t),
                     (unsigned char)(a.b + (b.b - a.b) * t)};
    };
    for (int key = 0; key < 12; key++) {
        const auto box = pin_key_box(key);
        const int cx = box.x + box.w / 2, cy = y + box.y + box.h / 2, radius = pin_key_radius;
        const bool held = pointer_down && point_in(box, contact_x, contact_y) && point_in(box, down_x, down_y);
        // How lit the key is: fully while held, then fading out from the moment it was pressed.
        float lit = held ? 1.f : 0.f;
        if (!held && key == pin_flash_key && preferences.animations && now - pin_flash_at < pin_flash_ms) {
            const float t = float(now - pin_flash_at) / pin_flash_ms;
            lit = (1 - t) * (1 - t);
        }
        const bool focused = settings.focus == key;
        if (key == 9 || key == 11) {
            // Cancel and Delete: words and a glyph, no key face, lit as a soft disc when pressed.
            if (lit > 0) smooth_arc(r, cx, cy, radius / 2, radius, 0, 1, theme.at("focus"), int(255 * lit));
            if (focused) smooth_arc(r, cx, cy, radius + 5, 3, 0, 1, accent);
            const bool can = key == 9 || !pin_digits.empty();
            if (key == 9) {
                const std::string text = locale.text("Cancel");
                const int width = std::min(2 * radius + 24, label_width(text, 18, false));
                raw_label(r, text, cx - width / 2, centred_label_y(text, 18, cy), 18, "accent", 2 * radius + 24);
            } else {
                // The delete key: a tag pointing left with a cross in it.
                const Color ink = theme.at(can ? "ink" : "line");
                s.SetRenderDrawColor(r, ink.r, ink.g, ink.b, 255);
                for (int row = -11; row <= 11; row++) {
                    const int tip = cx - 20 + std::abs(row);
                    SDL_Rect band{tip, cy + row, cx + 16 - tip, 1};
                    s.RenderFillRect(r, &band);
                }
                const Color inside =
                    lit > 0 ? blend(theme.at("background"), theme.at("focus"), lit) : theme.at("background");
                s.SetRenderDrawColor(r, inside.r, inside.g, inside.b, 255);
                for (int row = -8; row <= 8; row++) {
                    const int tip = cx - 15 + std::abs(row);
                    SDL_Rect band{tip, cy + row, cx + 13 - tip, 1};
                    s.RenderFillRect(r, &band);
                }
                s.SetRenderDrawColor(r, ink.r, ink.g, ink.b, 255);
                for (int w = -1; w <= 1; w++) {
                    s.RenderDrawLine(r, cx - 4 + w, cy - 5, cx + 6 + w, cy + 5);
                    s.RenderDrawLine(r, cx + 6 + w, cy - 5, cx - 4 + w, cy + 5);
                }
            }
            continue;
        }
        if (focused) smooth_arc(r, cx, cy, radius + 5, 3, 0, 1, accent);
        smooth_arc(r, cx, cy + 2, radius / 2, radius, 0, 1, shadow_colour(), preferences.dark ? 70 : 34);
        smooth_arc(r, cx, cy, radius / 2, radius, 0, 1, blend(panel, accent, lit)); // a disc: a ring to its centre
        const Color ink = blend(theme.at("ink"), theme.at("on_accent"), lit),
                    soft = blend(theme.at("muted"), theme.at("on_accent"), lit);
        const std::string digit(1, char('0' + (key == 10 ? 0 : key + 1)));
        const std::string under = letters[size_t(key)];
        const int digit_y = under.empty() ? cy : cy - 7;
        raw_label_colour(r, digit, cx - label_width(digit, 28, false) / 2, centred_label_y(digit, 28, digit_y), 28, ink,
                         2 * radius);
        if (!under.empty())
            raw_label_colour(r, under, cx - label_width(under, 14, false) / 2, centred_label_y(under, 14, cy + 17), 14,
                             soft, 2 * radius);
    }
}

void App::pin_top(SDL_Renderer* r, int y) {
    const int top = y + top_card_y + (top_card_h - 240) / 2;
    settings_card(r, 24, top, 592, 240);
    const auto& list = profiles.profiles();
    const std::string name = pin_purpose == PinPurpose::Enter && pin_profile < int(list.size())
                                 ? list[size_t(pin_profile)].name
                             : list.empty() ? std::string()
                                            : list.front().name;
    std::string title;
    if (pin_purpose == PinPurpose::Enter || pin_purpose == PinPurpose::Unlock) title = locale.text("Enter the PIN");
    else title = locale.text(pin_purpose == PinPurpose::Set ? "Choose a PIN" : "Enter it again");
    const int title_width = std::min(544, label_width(title, 28, false));
    raw_label(r, title, 320 - title_width / 2, top + 28, 28, "accent", 544);
    const std::string about = pin_purpose == PinPurpose::Unlock ? locale.text("This needs a grown-up.") : name;
    const int about_width = std::min(544, label_width(about, 18, false));
    if (pin_purpose == PinPurpose::Enter || pin_purpose == PinPurpose::Unlock)
        raw_label(r, about, 320 - about_width / 2, top + 72, 18, "muted", 544);
    // Four dots. The newest one pops as it fills; a wrong PIN shakes them, red, before they empty.
    const Uint32 now = s.GetTicks();
    const bool shaking = preferences.animations && pin_shake_dots && now - pin_shake_at < pin_shake_ms;
    if (!shaking) pin_shake_dots = 0;
    const float shake_t = shaking ? float(now - pin_shake_at) / pin_shake_ms : 1.f;
    const int shift = shaking ? int(std::lround(std::sin(shake_t * 3.14159265 * 6) * 16 * (1 - shake_t))) : 0;
    const int filled = shaking ? pin_shake_dots : int(pin_digits.size());
    const Color wrong{214, 58, 58};
    for (int i = 0; i < 4; i++) {
        const int cx = 320 - 63 + i * 42 + shift, cy = top + 122;
        int radius = 9;
        if (!shaking && i == filled - 1 && preferences.animations && now - pin_pop_at < pin_pop_ms) {
            const float t = float(now - pin_pop_at) / pin_pop_ms;
            radius = int(std::lround(9 + 4 * std::sin(t * 3.14159265))); // out and back
            micro_until = std::max(micro_until, pin_pop_at + pin_pop_ms + 20);
        }
        if (i < filled)
            smooth_arc(r, cx, cy, radius / 2, radius + radius % 2, 0, 1, shaking ? wrong : theme.at("accent"));
        else smooth_arc(r, cx, cy, 8, 2, 0, 1, theme.at("muted"));
    }
    const long wait = pin_purpose == PinPurpose::Enter || pin_purpose == PinPurpose::Unlock ? pin_wait_left() : 0;
    const std::string line = wait                   ? locale.text("Too many tries. Wait " + std::to_string(wait) + " s")
                             : !pin_message.empty() ? locale.text(pin_message)
                             : pin_purpose == PinPurpose::Set && new_profile_flow
                                 ? locale.text("Without one this profile is not locked. Cancel skips it.")
                                 : std::string();
    const int width = std::min(544, label_width(line, 18, false));
    if (!line.empty())
        raw_label(r, line, 320 - width / 2, top + 160, 18, shaking || !pin_message.empty() ? "ink" : "muted", 544);
    key_hints(r, 320, top + 212, {{"A", "Type"}, {"X", "Delete"}, {"B", "Cancel"}}, 0);
}
