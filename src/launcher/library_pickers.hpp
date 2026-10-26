#pragma once
// The library view sheet's entry and the jump-to-letter strip.
// Members of App in main.cpp; included once, right after the class.

void App::choose_library_view() { open_filter_sheet(); }

void App::library_choice_icon(SDL_Renderer* r, int index, int x, int y, int size, const std::string& ink) {
    if (index == 5) {
        for (int i = -1; i <= 1; i++) {
            round_rect(r, x - size / 2, y + i * size / 3 - 1, 3, 3, 1, ink);
            round_rect(r, x - size / 2 + 7, y + i * size / 3 - 1, size - 7, 3, 1, ink);
        }
        return;
    }
    if (index == 7) {
        search_icon(r, x, y, size, ink);
        return;
    }
    static constexpr std::array<SettingsIcon, 9> icons = {
        SettingsIcon::Cartridge, SettingsIcon::Heart, SettingsIcon::Clock,   SettingsIcon::Cartridge, SettingsIcon::Art,
        SettingsIcon::Library,   SettingsIcon::Type,  SettingsIcon::Library, SettingsIcon::Refresh};
    ui_icon(r, icons.at(index), x, y, size, ink);
}

std::string App::jump_title(size_t index) {
    const auto& game = model.games[index];
    if (game.demo) return game.title;
    if (const auto* entry = library_cache.peek(game); entry && entry->title_ready) return entry->title;
    return library_title(game.title);
}

void App::jump_to_letter() {
    std::map<std::string, size_t> first;
    for (size_t i = 0; i < model.games.size(); i++) first.emplace(initial_letter(jump_title(i)), i);
    std::vector<std::string> letters;
    std::vector<size_t> targets;
    int selected = 0;
    auto current = model.games.empty() ? "" : initial_letter(jump_title(model.selected));
    for (const auto& entry : first) {
        if (entry.first == current) selected = int(letters.size());
        letters.push_back(entry.first);
        targets.push_back(entry.second);
    }
    show_picker("Jump to letter", letters, selected, [this, targets](int n) {
        if (n >= 0 && n < int(targets.size())) select_game(targets[n], false);
    });
}
