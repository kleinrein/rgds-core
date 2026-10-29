#pragma once
// Game menu > Cheats: loading the list, All/Groups/On tabs, search and toggling.
// Members of App in main.cpp; included once, right after the class.

int App::cheats_on() const {
    int n = 0;
    for (auto& code : cheat_codes)
        if (code.state == 1) ++n;
    return n;
}

int App::cheats_in(int group, bool only_on) const {
    int n = 0;
    for (auto& code : cheat_codes)
        if (code.group == group && (!only_on || code.state == 1)) ++n;
    return n;
}

bool App::cheat_togglable(int row) const {
    return row >= 0 && row < int(cheat_rows.size()) && !cheat_rows[size_t(row)].header &&
           cheat_codes[size_t(cheat_rows[size_t(row)].index)].state >= 0;
}

std::string App::cheats_note() const {
    if (!cheats_engine()) return "DSCore and Play only";
    if (cheat_codes.empty()) return "None for this game";
    const int on = cheats_on();
    return on ? std::to_string(on) + (on == 1 ? " cheat on" : " cheats on") : "All off";
}

std::string App::cheat_group_name(int group) const {
    return group < 0 || group >= int(cheat_groups.size()) ? locale.text("Other cheats")
                                                          : cheat_groups[size_t(group)].name;
}

std::string App::cheats_title() const {
    return cheat_view == CheatView::Group ? cheat_group_name(cheat_group) : locale.text("Cheats");
}

std::string App::cheats_context() const {
    return cheat_view == CheatView::Groups ? locale.text("Choose a group")
           : cheat_view == CheatView::On   ? locale.text("Cheats that are on")
           : cheat_view == CheatView::All
               ? (cheat_query.empty() ? locale.text("All cheats") : locale.text("Search") + ": " + cheat_query)
               : cheat_group_name(cheat_group);
}

int App::cheat_match_count(const std::string& text) const {
    const auto query = lowered(trimmed_query(text));
    int n = 0;
    for (auto& code : cheat_codes)
        if (query.empty() || cheat_matches(code, query)) ++n;
    return n;
}

void App::build_cheat_rows() {
    cheat_rows.clear();
    if (cheat_view == CheatView::Groups) {
        for (int group : cheat_group_order) cheat_rows.push_back({true, group});
    } else {
        const auto query = lowered(cheat_query);
        for (int i = 0; i < int(cheat_codes.size()); i++) {
            const auto& code = cheat_codes[size_t(i)];
            const bool keep = cheat_view == CheatView::Group ? code.group == cheat_group
                              : cheat_view == CheatView::On  ? code.state == 1
                                                             : query.empty() || cheat_matches(code, query);
            if (keep) cheat_rows.push_back({false, i});
        }
    }
    if (settings.page == Page::Cheats)
        settings.focus = std::clamp(settings.focus, 0, std::max(0, int(cheat_rows.size()) - 1));
}

void App::show_cheat_view(CheatView view, int group) {
    if (cheat_view == CheatView::Groups && settings.page == Page::Cheats) cheat_groups_focus = settings.focus;
    cheat_view = view;
    if (view == CheatView::Group) cheat_group = group;
    settings.focus = view == CheatView::Groups ? cheat_groups_focus : 0;
    build_cheat_rows();
    status.clear();
    dirty = true;
}

std::vector<App::CheatView> App::cheat_tabs() const {
    return cheat_overview() ? std::vector<CheatView>{CheatView::All, CheatView::Groups, CheatView::On}
                            : std::vector<CheatView>{CheatView::All, CheatView::On};
}

int App::cheat_tab() const {
    const auto tabs = cheat_tabs();
    const CheatView view = cheat_view == CheatView::Group ? CheatView::Groups : cheat_view;
    return int(std::find(tabs.begin(), tabs.end(), view) - tabs.begin());
}

void App::cheat_tab_select(int index) {
    const auto tabs = cheat_tabs();
    if (index < 0 || index >= int(tabs.size())) return;
    cheat_query.clear();
    show_cheat_view(tabs[size_t(index)]);
}

void App::cheat_tab_step(int direction) {
    const int index = cheat_tab() + direction;
    if (index >= 0 && index < int(cheat_tabs().size())) cheat_tab_select(index);
}

void App::load_cheats() {
    cheat_codes.clear();
    cheat_groups.clear();
    cheat_group_order.clear();
    auto path = o.data / "session/emulator.control.cheats";
    if (o.preview && !fs::exists(path) && cheats_engine()) {
        cheat_groups = {{"Coins", true}, {"Movement", false}};
        cheat_codes = {{"Infinite lives", "Lives never go down.", -1, 0},
                       {"Codes from usrcheat.dat", "", -1, -1},
                       {"99 coins", "", 0, 1},
                       {"Max coins", "", 0, 0},
                       {"Walk through walls", "Hold L while walking.", 1, 0},
                       {"Moon jump", "Press B again in the air.", 1, 0}};
    } else {
        auto value = fields(path);
        const int count = std::clamp(std::atoi(value["count"].c_str()), 0, 100000),
                  groups = std::clamp(std::atoi(value["groups"].c_str()), 0, 10000);
        for (int g = 0; g < groups; g++) {
            auto key = "group_" + std::to_string(g);
            cheat_groups.push_back({value[key], value[key + "_exclusive"] == "1"});
        }
        for (int i = 0; i < count; i++) {
            auto key = "cheat_" + std::to_string(i);
            const int group = std::atoi(value[key + "_group"].c_str()),
                      state = std::clamp(std::atoi(value[key + "_state"].c_str()), -1, 1);
            cheat_codes.push_back(
                {value[key], value[key + "_description"], group >= 0 && group < groups ? group : -1, state});
        }
    }
    for (auto& code : cheat_codes)
        if (std::find(cheat_group_order.begin(), cheat_group_order.end(), code.group) == cheat_group_order.end())
            cheat_group_order.push_back(code.group);
    // A toggle's answer reloads the list mid-page: keep the view, unless its group is gone.
    if (cheat_view == CheatView::Group &&
        std::find(cheat_group_order.begin(), cheat_group_order.end(), cheat_group) == cheat_group_order.end())
        cheat_view = cheat_overview() ? CheatView::Groups : CheatView::All;
    build_cheat_rows();
}

void App::open_cheats() {
    load_cheats();
    if (!cheats_available()) {
        show_detail(
            "Cheats",
            locale.text(
                !cheats_engine() ? "Cheats are available when a game runs with DSCore or RG DS Play."
                : !experimental_game()
                    ? "No cheats were found for this game. Put its cheat file (GAME.cht, as RetroArch uses) beside "
                      "the game or in userdata/play/cheats, then start the game again."
                    : "No cheats were found for this game. Put an Action Replay database (usrcheat.dat) beside "
                      "your games or in userdata/emulators/rgds-experimental/cheats, then start the game again."));
        return;
    }
    settings.page = Page::Cheats;
    settings.focus = 0;
    cheat_groups_focus = 0;
    cheat_view = CheatView::Groups;
    cheat_query.clear();
    cheat_home_view();
}

void App::cheat_move(int step) {
    if (cheat_rows.empty()) return;
    settings.focus = std::clamp(settings.focus + step, 0, int(cheat_rows.size()) - 1);
}

void App::cheat_side(int direction) {
    if (cheat_view == CheatView::Group) {
        const auto at = std::find(cheat_group_order.begin(), cheat_group_order.end(), cheat_group);
        const int index = int(at - cheat_group_order.begin()) + direction;
        if (index < 0 || index >= int(cheat_group_order.size())) return;
        show_cheat_view(CheatView::Group, cheat_group_order[size_t(index)]);
    } else if (cheat_view != CheatView::Groups) cheat_move(direction * 8);
}

void App::cheat_back() {
    if (cheat_view == CheatView::Group) {
        show_cheat_view(CheatView::Groups);
        return;
    }
    if (cheat_view == CheatView::All && !cheat_query.empty()) {
        cheat_query.clear();
        show_cheat_view(CheatView::All);
        return;
    }
    settings.page = Page::QuickMenu;
    settings.focus = 11;
}

void App::cheat_action(int code) {
    if (menu_status["state"] == "busy") return;
    if (code == 0 || code == 1) {
        cheat_side(code == 0 ? -1 : 1);
        return;
    }
    if (cheat_view == CheatView::Groups) {
        if (settings.focus >= 0 && settings.focus < int(cheat_rows.size()))
            show_cheat_view(CheatView::Group, cheat_rows[size_t(settings.focus)].index);
        return;
    }
    if (!cheat_togglable(settings.focus)) return;
    const int index = cheat_rows[size_t(settings.focus)].index;
    auto& cheat = cheat_codes[size_t(index)];
    const bool on = cheat.state != 1;
    if (o.preview) {
        if (on && cheat.group >= 0 && cheat_groups[size_t(cheat.group)].exclusive)
            for (auto& other : cheat_codes)
                if (&other != &cheat && other.group == cheat.group) other.state = other.state < 0 ? -1 : 0;
        cheat.state = on ? 1 : 0;
        menu_status["message"] = on ? "Cheat enabled (preview)" : "Cheat disabled (preview)";
        sound_intent(on ? MenuCue::Enable : MenuCue::Disable);
        build_cheat_rows();
        return;
    }
    menu_request("cheat:" + std::to_string(index) + ":" + (on ? "1" : "0"));
}

void App::tab_strip(SDL_Renderer* r, int x, int rule, int w, const std::vector<std::string>& names, int selected,
                    int pressed) {
    for (int i = 0; i < int(names.size()); i++) {
        const bool open = i == selected;
        const int left = x + i * w, top = rule - (open ? 50 : 42) + (pressed == i ? 2 : 0), radius = 12,
                  body = rule - top + radius;
        SDL_Rect clip{left, top - 2, w, rule - top + 2};
        s.RenderSetClipRect(r, &clip);
        if (open) {
            alpha_round(r, left, top + 3, w, body, radius, shadow_colour(), preferences.dark ? 70 : 28);
            glossy_pill(r, left, top, w, body, radius, theme.at("accent"));
        } else {
            fill_round(r, left, top, w, body, radius, theme.at("line"));
            fill_round(r, left + 1, top + 1, w - 2, body - 2, radius - 1, theme.at("panel"));
        }
        s.RenderSetClipRect(r, nullptr);
        if (open) rect(r, left, rule - 1, w, 2, "accent");
        const std::string value = locale.text(names[size_t(i)]);
        const int width = std::min(w - 12, label_width(value, 16, false));
        raw_label(r, value, left + (w - width) / 2, centred_label_y(value, 16, (top + rule) / 2), 16,
                  open ? "on_accent" : "ink", width + 4);
    }
}

void App::cheat_header(SDL_Renderer* r, int y) {
    std::vector<std::string> names;
    for (CheatView view : cheat_tabs()) names.push_back(cheat_tab_name(view));
    const int left = cheat_tabs_left();
    const bool pressing = pointer_down && cheat_header_hit(contact_x, contact_y);
    const int pressed = pressing && contact_x < cheat_search_x ? (contact_x - left) / cheat_tab_w : -1;
    tab_strip(r, left, y + 77, cheat_tab_w, names, cheat_tab(), pressed);
    const bool filtering = cheat_view == CheatView::All && !cheat_query.empty();
    hotkey_pill(r, cheat_search_x, y + 24, cheat_search_w, 42, filtering ? cheat_query : "Search",
                filtering ? KeyLook::Accent : KeyLook::Panel, pressing && contact_x >= cheat_search_x, "X", !filtering);
}

void App::cheat_header_tap(int x) {
    if (x >= cheat_search_x) begin_edit(6);
    else cheat_tab_select((x - cheat_tabs_left()) / cheat_tab_w);
    dirty = true;
}

void App::cheat_row(SDL_Renderer* r, int y, int i) {
    const auto& row = cheat_rows[size_t(i)];
    if (row.header) {
        const int count = cheats_in(row.index), on = cheats_in(row.index, true);
        list_row(r, y, i, cheat_group_name(row.index),
                 on ? std::to_string(on) + " on" : std::to_string(count) + (count == 1 ? " cheat" : " cheats"));
        return;
    }
    const auto& cheat = cheat_codes[size_t(row.index)];
    list_row(r, y, i, cheat.name,
             cheat.state < 0    ? ""
             : cheat.state == 1 ? "On"
                                : "Off",
             cheat.state >= 0 && menu_status["state"] != "busy");
}

void App::cheats_top(SDL_Renderer* r, int y) {
    // The card, the key bar under it, then the game and the engine's last answer; the words are centred in the card.
    settings_card(r, 24, y + top_card_y, 592, 266);
    y += 4;
    const int total = int(cheat_codes.size()), on = cheats_on();
    const bool overview = cheat_view == CheatView::Groups;
    std::string context = cheats_context();
    if (cheat_view == CheatView::Group && cheat_group >= 0 && cheat_groups[size_t(cheat_group)].exclusive)
        context += "  ·  " + locale.text("One at a time");
    raw_label(r, context, 46, y + 98, 18, "accent", 548);
    if (cheat_rows.empty()) {
        wrapped(r,
                cheat_view == CheatView::All  ? "No cheats match. Try a shorter word."
                : cheat_view == CheatView::On ? "No cheats are on."
                                              : "No cheats in this group.",
                46, y + 130, 22, 548, 3);
    } else {
        const auto& row = cheat_rows[size_t(std::clamp(settings.focus, 0, int(cheat_rows.size()) - 1))];
        if (row.header) {
            const int count = cheats_in(row.index), group_on = cheats_in(row.index, true);
            raw_wrapped(r, cheat_group_name(row.index), 46, y + 124, 26, 548, 2);
            raw_label(r,
                      locale.text(std::to_string(count) + (count == 1 ? " cheat" : " cheats")) +
                          (group_on ? "  ·  " + locale.text(std::to_string(group_on) + " on") : std::string()),
                      46, y + 186, 18, "ink", 548);
            std::string names;
            int shown = 0;
            for (auto& code : cheat_codes) {
                if (code.group != row.index) continue;
                if (shown++ >= 4) {
                    names += ", …";
                    break;
                }
                names += (names.empty() ? "" : ", ") + code.name;
            }
            raw_wrapped(r, names, 46, y + 214, 16, 548, 2);
        } else {
            const auto& code = cheat_codes[size_t(row.index)];
            raw_wrapped(r, code.name, 46, y + 124, 26, 548, 2);
            if (!code.description.empty()) raw_wrapped(r, code.description, 46, y + 186, 18, 548, 2);
            else
                label(r,
                      code.state < 0 ? "A note from the database; nothing to turn on."
                                     : "No description in the database.",
                      46, y + 186, 18, "muted", 548);
            if (cheat_view != CheatView::Group)
                raw_label(r, cheat_group_name(code.group), 46, y + 238, 16, "muted", 548);
        }
    }
    raw_label(r, locale.text("Cheats on") + ": " + std::to_string(on) + " / " + std::to_string(total), 46, y + 274, 16,
              "accent", 548);
    // The keys that move around, in one ruled bar; then the game and the engine's last answer.
    struct Key {
        const char* key;
        const char* text;
    };
    const bool filtering = cheat_view == CheatView::All && !cheat_query.empty();
    std::vector<Key> keys = overview ? std::vector<Key>{{"A", "Open"}, {"L R", "Tabs"}, {"X", "Search"}}
                            : cheat_view == CheatView::Group
                                ? std::vector<Key>{{"A", "Toggle"}, {"L R", "Tabs"}, {"B", "Groups"}}
                            : filtering ? std::vector<Key>{{"X", "Edit search"}, {"L R", "Tabs"}, {"B", "Clear"}}
                                        : std::vector<Key>{{"A", "Toggle"}, {"L R", "Tabs"}, {"X", "Search"}};
    // A only when it does something here: an empty view or a note has nothing to open or toggle.
    if (overview ? cheat_rows.empty() : !cheat_togglable(settings.focus))
        keys.erase(std::remove_if(keys.begin(), keys.end(), [](const Key& key) { return std::string(key.key) == "A"; }),
                   keys.end());
    key_bar(r, 24, y + 326, 592, 44, keys);
    if (!model.games.empty()) raw_label(r, game_title(0), 28, y + 388, 18, "muted", 584);
    if (!menu_status["message"].empty()) wrapped(r, menu_status["message"], 32, y + 426, 16, 576, 2);
}

void App::cheat_search_top(SDL_Renderer* r, int y) {
    label(r, "Search in", 28, y + 78, 16, "muted", 580);
    label(r, "Cheats", 28, y + 102, 30, "ink", 580);
    const auto query = lowered(trimmed_query(edit_buffer));
    std::vector<const CheatCode*> first;
    int n = 0;
    for (auto& code : cheat_codes)
        if (query.empty() || cheat_matches(code, query)) {
            if (first.size() < 4) first.push_back(&code);
            ++n;
        }
    label(r, locale.text(std::to_string(n) + (n == 1 ? " matching cheat" : " matching cheats")), 28, y + 185, 22,
          "accent", 580);
    if (!n) wrapped(r, "Try a shorter word.", 28, y + 236, 22, 570, 2);
    for (size_t i = 0; i < first.size(); i++) {
        round_rect(r, 24, y + 228 + int(i) * 49, 592, 42, 9, "panel");
        raw_label(r, first[i]->name, 42, y + 237 + int(i) * 49, 20, "ink", 554);
    }
    key_hints(r, 28, y + 451, {{"A", "Type"}, {"Start", "Search"}, {"B", "Cancel"}});
}
