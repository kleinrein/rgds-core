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
    home_selected = std::min(home_selected, home_tiles() - 1);
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
    for (char tone : {'#', '+'}) {
        s.SetRenderDrawColor(r, ink.r, ink.g, ink.b, tone == '#' ? 255 : 110);
        for (size_t i = 0; i < pixels.size(); i++)
            if (pixels[i] == tone) {
                SDL_Rect cell{left + int(i % 16) * scale, top + int(i / 16) * scale, scale, scale};
                s.RenderFillRect(r, &cell);
            }
    }
    s.SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
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
        raw_label(r, name, box.x + box.w / 2 - width / 2, y + box.y + box.h + 8, 20, "ink", 168);
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
        label(r, "Last played", 248, top + 134, 14, "muted", 344);
        raw_wrapped(r, profile.last_game, 248, top + 156, 22, 344, 2);
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

SDL_Rect App::profiles_row_box(int row) const { return {28, 84 + row * 41, 584, 38}; }

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
        if (row >= 0) {
            const auto& profile = profiles.profiles()[size_t(row)];
            draw_profile_tile(r, profile, box.x + 8, top + 5, 28);
            raw_label(r, profile.name, box.x + 52, centred_label_y(profile.name, 20, middle), 20, "ink", 300);
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
            ui_icon(r,
                    row == profile_row_switch ? SettingsIcon::Refresh
                    : row == profile_row_add  ? SettingsIcon::Spark
                                              : SettingsIcon::Power,
                    box.x + 24, middle, 22, "accent");
            label(r, title, box.x + 52, centred_label_y(locale.text(title), 20, middle), 20, "ink", 440);
            if (row == profile_row_ask) draw_toggle(r, box.x + box.w - 60, top + 5, title, profiles.ask_at_boot());
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
    // Who it's for 0, Name 1, DS name 2, Picture 3, Colour 4, Age limit 6, No rating 7, Games 8, Ports 9, Homebrew 10,
    // PictoChat 11, Cheats 12, Local wireless 13, PIN 14, Remove the PIN 15, Remove 5: the numbers stay, the order is what
    // reads best.
    const auto* profile = edited_profile();
    std::vector<int> rows;
    if (profile && profile->owner()) rows = {1, 2, 3, 4, 8, 14};
    else if (profile && !profile->restricted) rows = {0, 1, 2, 3, 4, 6, 7, 8, 14};
    else rows = {0, 1, 2, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14};
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
    return {colour_grid_x + (index % 4) * (colour_tile_w + colour_gap), 92 + (index / 4) * 106, colour_tile_w, 98};
}

void App::profile_set_colour(const Profile& profile, int colour) {
    if (profile.id == profiles.active()) {
        preferences.accent = colour;
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
        look.accent = colour;
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
        Profile changed = *profile;
        changed.picture = index;
        if (!profiles.save(changed)) status = "Could not save all settings. Check storage.";
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
    } else if (profile) profile_set_colour(*profile, index);
    settings.page = Page::ProfileEdit;
    profile_edit_focus(profile_choice == ProfileChoice::Picture ? 3 : profile_choice == ProfileChoice::Age ? 6 : 4);
    dirty = true;
}

void App::profile_choose_action(int code) {
    int& focus = settings.focus;
    const int count = profile_choice_count(), wide = profile_choice == ProfileChoice::Kind ? 2 : 4;
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
            raised_pill(r, box.x, box.y, box.w, box.h, 12, theme.at("panel"));
            draw_profile_tile(r, shown, box.x + (box.w - 88) / 2, box.y + 5, 88);
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
        }
    }
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

void App::open_profile_games(int tab) {
    games_tab = tab;
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
        const bool visible = game_visible(games_rules, entry.key, entry.game.system, entry.rating);
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

int App::games_shelf_waiting(const std::string& shelf) const {
    int waiting = 0;
    for (const auto& entry : games_entries)
        if (entry.game.system == shelf && entry.age < 0 && !games_rules.overrides.count(entry.key)) waiting++;
    return waiting;
}

void App::allow_shelf() {
    const auto* profile = edited_profile();
    if (!profile) return;
    auto overrides = load_visibility(profile->root);
    for (const auto& entry : games_entries)
        if (entry.game.system == games_shelf && entry.age < 0 && !overrides.count(entry.key))
            overrides[entry.key] = "show"; // only what has no rating, and never over a choice already made
    if (!profile_write_fields(profile->root / "visibility.ini", overrides))
        status = "Could not save all settings. Check storage.";
    if (profile->id == profiles.active()) visibility_changed = true;
    games_rules.overrides = std::move(overrides);
    settings.focus = games_focus;
    games_rows_build();
}

void App::profile_games_action(int code) {
    if (code == 3) {
        const auto rows = profile_edit_rows();
        const auto at = std::find(rows.begin(), rows.end(), 8);
        games_reader.cancel();
        settings.page = Page::ProfileEdit;
        settings.focus = at == rows.end() ? 0 : int(at - rows.begin());
    } else if (code == 13 || code == 14) {
        const int next = std::clamp(games_tab + (code == 13 ? -1 : 1), 0, 3);
        if (next != games_tab) {
            games_tab = next;
            settings.focus = 0;
            list_scroll = 0;
            games_rows_build();
        }
    } else if ((code == 8 || code == 9) && !games_rows.empty()) {
        settings.focus = std::clamp(settings.focus + (code == 8 ? -1 : 1), 0, int(games_rows.size()) - 1);
    } else if ((code == 2 || code == 4) && settings.focus < int(games_rows.size())) {
        const auto row = games_rows[size_t(settings.focus)];
        const auto* profile = edited_profile();
        if (!profile) return;
        if (row.entry < 0) {
            if (games_shelf_waiting(row.shelf) == 0) return;
            games_shelf = row.shelf;
            games_focus = settings.focus;
            confirm("profile-allow-shelf", Page::ProfileGames);
            return;
        }
        const auto& key = games_entries[size_t(row.entry)].key;
        const auto found = games_rules.overrides.find(key);
        // Automatic, then always show, then always hide, and round.
        const std::string next = found == games_rules.overrides.end() ? "show" : found->second == "show" ? "hide" : "";
        set_game_override(*profile, key, next);
        if (next.empty()) games_rules.overrides.erase(key);
        else games_rules.overrides[key] = next;
        emit_sound(next == "show" ? MenuCue::Enable : next == "hide" ? MenuCue::Disable : MenuCue::Confirm);
        games_rows_build();
    }
    dirty = true;
}

bool App::profile_games_header_hit(int x, int y) const { return y >= 24 && y < 78 && x >= profile_games_tabs_left; }

void App::profile_games_header_tap(int x) {
    const int to = std::clamp((x - profile_games_tabs_left) / profile_games_tab, 0, 3);
    if (to == games_tab) return;
    games_tab = to;
    settings.focus = 0;
    list_scroll = 0;
    games_rows_build();
}

void App::profile_games_row(SDL_Renderer* r, int y, int i) {
    const auto& row = games_rows[size_t(i)];
    if (row.entry < 0) {
        const int waiting = games_shelf_waiting(row.shelf);
        list_row(r, y, i, system_info(row.shelf).name, waiting ? locale.text("Allow all") : "", waiting > 0);
        return;
    }
    const auto& entry = games_entries[size_t(row.entry)];
    const auto found = games_rules.overrides.find(entry.key);
    const bool visible = game_visible(games_rules, entry.key, entry.game.system, entry.rating);
    std::string value = found != games_rules.overrides.end()
                            ? locale.text(found->second == "show" ? "Always show" : "Always hide")
                            : locale.text(visible ? "Shown" : "Hidden");
    if (entry.age >= 0) value = std::to_string(entry.age) + "+ · " + value;
    list_row(r, y, i, library_title(entry.game.title), value);
}

void App::profile_games_top(SDL_Renderer* r, int y) {
    const int top = y + top_card_y + (top_card_h - 240) / 2;
    settings_card(r, 24, top, 592, 240);
    const auto* profile = edited_profile();
    if (games_reader.pending()) {
        busy_ring(r, 320, top + 120, 26, 4);
        return;
    }
    if (settings.focus >= int(games_rows.size())) {
        wrapped(r, "No games here.", 48, top + 100, 20, 544, 2);
        return;
    }
    const auto& row = games_rows[size_t(settings.focus)];
    if (row.entry < 0) {
        const int waiting = games_shelf_waiting(row.shelf);
        raw_label(r, system_info(row.shelf).name, 48, top + 30, 28, "accent", 544);
        // this shelf's own numbers: the heading names one system
        int shown = 0, above = 0, unrated = 0;
        for (const auto& entry : games_entries) {
            if (entry.game.system != row.shelf) continue;
            const bool visible = game_visible(games_rules, entry.key, entry.game.system, entry.rating);
            shown += visible;
            unrated += entry.age < 0;
            above += entry.age >= 0 && !visible;
        }
        const std::string counts = locale.text(std::to_string(shown) + " shown") + "  ·  " +
                                   locale.text(std::to_string(above) + " above the age") + "  ·  " +
                                   locale.text(std::to_string(unrated) + " with no rating");
        raw_label(r, counts, 48, top + 76, 18, "muted", 544);
        wrapped(r,
                waiting ? "A shows every game with no rating on this shelf. Games with a rating still follow the age."
                        : "Nothing here is waiting for a decision.",
                48, top + 116, 20, 544, 4);
        return;
    }
    const auto& entry = games_entries[size_t(row.entry)];
    cover_art(r, folder_for_game(entry.game) / "box.bmp", {48, top + 32, 176, 176}, false);
    raw_wrapped(r, library_title(entry.game.title), 248, top + 30, 24, 344, 3);
    label(r, entry.age >= 0 ? entry.rating : "No age rating", 248, top + 126, 18, "accent", 344);
    const auto found = games_rules.overrides.find(entry.key);
    const bool visible = game_visible(games_rules, entry.key, entry.game.system, entry.rating);
    const char* why = found != games_rules.overrides.end()
                          ? (visible ? "You chose to show it." : "You chose to hide it.")
                      : entry.age >= 0 && !visible ? "Above this profile's age."
                      : entry.age < 0 && !visible  ? "Hidden until you allow it."
                                                   : "Fits this profile.";
    wrapped(r, why, 248, top + 158, 18, 344, 2);
    if (profile) raw_label(r, profile->name, 248, top + 200, 14, "muted", 344);
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

void App::pin_press(int key) {
    if (key == 11) return pin_cancel();
    if (key == 9) {
        if (!pin_digits.empty()) pin_digits.pop_back();
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
    emit_sound(MenuCue::Navigate);
    if (pin_digits.size() == 4) pin_submit();
    dirty = true;
}

// 1 to 9 in three rows, then Clear, 0, Cancel.
SDL_Rect App::pin_key_box(int key) const { return {132 + (key % 3) * 128, 84 + (key / 3) * 80, 120, 72}; }

void App::pin_action(int code) {
    int& focus = settings.focus;
    if (code == 3) {
        if (pin_digits.empty()) pin_cancel();
        else pin_digits.pop_back();
    } else if (code == 2 || code == 4) pin_press(focus);
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
    static const std::array<const char*, 12> names = {"1", "2", "3", "4",     "5", "6",
                                                      "7", "8", "9", "Clear", "0", "Cancel"};
    for (int key = 0; key < 12; key++) {
        auto box = pin_key_box(key);
        box.y += y;
        const bool pressed = pointer_down && point_in(pin_key_box(key), contact_x, contact_y) &&
                             point_in(pin_key_box(key), down_x, down_y);
        raised_pill(r, box.x, box.y, box.w, box.h, 12, theme.at(pressed ? "focus" : "panel"), pressed ? 1 : 0);
        const std::string text = locale.text(names[size_t(key)]);
        const int size = key == 9 || key == 11 ? 18 : 28, width = label_width(text, size, false);
        raw_label(r, text, box.x + (box.w - width) / 2, centred_label_y(text, size, box.y + box.h / 2), size, "ink",
                  box.w);
        if (settings.focus == key) chat_brackets(r, box.x - 2, box.y - 2, box.w + 4, box.h + 4, theme.at("accent"), 4);
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
    for (int i = 0; i < 4; i++) {
        const int cx = 320 - 66 + i * 44;
        fill_round(r, cx - 12, top + 108, 24, 24, 12, theme.at("line"));
        fill_round(r, cx - 10, top + 110, 20, 20, 10, theme.at(i < int(pin_digits.size()) ? "accent" : "slot"));
    }
    const long wait = pin_purpose == PinPurpose::Enter || pin_purpose == PinPurpose::Unlock ? pin_wait_left() : 0;
    const std::string line = wait                   ? locale.text("Too many tries. Wait " + std::to_string(wait) + " s")
                             : !pin_message.empty() ? locale.text(pin_message)
                             : pin_purpose == PinPurpose::Set && new_profile_flow
                                 ? locale.text("Without one this profile is not locked. Cancel skips it.")
                                 : std::string();
    const int width = std::min(544, label_width(line, 18, false));
    if (!line.empty()) raw_label(r, line, 320 - width / 2, top + 160, 18, "ink", 544);
}
