#pragma once
// Search and filters > More filters: narrow the library by what the game information knows (genre, publisher,
// developer, players, rating, decade, online play, whether a game has information at all). Each pick applies at
// once behind the page; "Show games" goes to the list. Members of App in main.cpp, included after game_info_screen.hpp.

void App::open_more_filters() {
    // Counts are read again each time, so a fetch that just finished shows up.
    if (library_ready) facet_index.request(o.data, all_games);
    settings.page = Page::MoreFilters;
    settings.focus = 0;
    dirty = true;
}

void App::facet_picker(int row) {
    const auto& spec = facet_rows().at(size_t(row));
    if (!facet_index.ready()) {
        show_detail(spec.title, locale.text("Still reading your library. Try again in a moment."));
        return;
    }
    const auto values = facet_index.values(spec.name, spec.by_count);
    if (values.empty()) {
        show_detail(spec.title, locale.text("No game has this information yet. Fetch artwork with Game info turned on."));
        return;
    }
    std::vector<std::string> names = {"Any"}, choices = {""};
    int selected = 0;
    for (const auto& [value, count] : values) {
        if (value == discovery_facets.*spec.field) selected = int(names.size());
        names.push_back(value + " (" + std::to_string(count) + ")");
        choices.push_back(value);
    }
    show_picker(spec.title, names, selected, [this, row, choices](int n) {
        discovery_facets.*facet_rows().at(size_t(row)).field = choices.at(size_t(n));
        library_view(preferences.view);
    });
}

void App::reset_facets() {
    if (!discovery_facets.any()) return;
    discovery_facets = {};
    library_view(preferences.view);
    dirty = true;
}
