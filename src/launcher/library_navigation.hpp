#pragma once
// Selecting and navigating games, the library view and collections.
// Members of App in main.cpp; included once, right after the class.

void App::select_game(size_t index, bool animate) {
    if (model.games.empty()) return;
    model.selected = std::min(index, model.games.size() - 1);
    selected_at = s.GetTicks();
    if (animate) cover_motion_at = selected_at;
    browser_list.select(model.selected, model.games.size(), selected_at, animate && preferences.animations);
    if (animate && preferences.animations) carousel.select(model.selected, selected_at);
    else carousel.reset(model.selected);
    dirty = true;
}

void App::navigate(int delta) {
    SoundAction sound_action(*this, delta < 0 ? 0 : 1);
    motion_input(delta < 0 ? 0 : 1);
    if (!navigation_count()) return;
    long count = static_cast<long>(navigation_count()), previous = static_cast<long>(navigation_selected());
    size_t next = static_cast<size_t>((previous + delta % count + count) % count);
    if (next != size_t(previous)) sound_intent(MenuCue::Navigate);
    select_navigation(next, std::abs(previous - static_cast<long>(next)) <= 3);
}

void App::hold_navigate(int direction, unsigned held) {
    const int stride = home_menu ? 1 : hold_stride(held, navigation_count());
    if (stride == 1) {
        navigate(direction);
        return;
    }
    const long count = long(navigation_count()), at = long(navigation_selected()),
               next = std::clamp(at + direction * stride, 0L, count - 1);
    if (next != at) navigate(int(next - at));
}

void App::library_view(int view) {
    cancel_navigation();
    std::string selected = model.games.empty()                ? ""
                           : model.games[model.selected].demo ? "demo:" + model.games[model.selected].title
                                                              : model.games[model.selected].path.string();
    preferences.view = view;
    collection_filter.cancel();
    collection_loading = false;
    if ((view != 0 && all_games.size() > 256) || !search_query.empty() || discovery_filter || discovery_facets.any()) {
        collection_selection = selected;
        collection_loading = true;
        collection_filter.request(o.data, view_source(view), view, search_query, discovery_filter, discovery_facets);
        model.games.clear();
        model.selected = 0;
        carousel.reset(0);
        browser_list.scroll = 0;
        dirty = true;
        return;
    }
    model.games = view_source(view);
    if (view == 1)
        model.games.erase(
            std::remove_if(model.games.begin(), model.games.end(),
                           [&](const Game& game) { return !library_cache.options(game, true).favourite; }),
            model.games.end());
    if (view == 2) {
        // Snapshot each date once. Sorting a large collection never performs
        // filesystem reads or churns the bounded detail cache in a comparator.
        std::vector<std::pair<Game, std::string>> recent;
        recent.reserve(model.games.size());
        for (const auto& game : model.games) {
            auto date = library_cache.stats(game, true).last_played;
            if (!date.empty()) recent.emplace_back(game, std::move(date));
        }
        std::stable_sort(recent.begin(), recent.end(),
                         [](const auto& a, const auto& b) { return a.second > b.second; });
        if (recent.size() > kRecentsLimit) recent.erase(recent.begin() + kRecentsLimit, recent.end());
        model.games.clear();
        model.games.reserve(recent.size());
        for (auto& item : recent) model.games.push_back(std::move(item.first));
    }
    model.selected = 0;
    if (!selected.empty())
        for (size_t i = 0; i < model.games.size(); i++)
            if ((model.games[i].demo ? "demo:" + model.games[i].title : model.games[i].path.string()) == selected)
                model.selected = i;
    carousel.reset(model.selected);
    browser_list.scroll = 0;
    browser_list.reveal(model.selected, model.games.size());
    dirty = true;
}

void App::poll_collection() {
    try {
        auto result = collection_filter.poll();
        if (!result || !collection_loading) return;
        model.games = std::move(result->games);
        collection_loading = false;
        model.selected = 0;
        for (size_t i = 0; i < model.games.size(); i++)
            if (model.games[i].path.string() == collection_selection) {
                model.selected = i;
                break;
            }
        carousel.reset(model.selected);
        browser_list.reveal(model.selected, model.games.size());
        dirty = true;
    } catch (const std::exception&) {
        collection_loading = false;
        status = "Could not read this collection. Try again.";
        dirty = true;
    }
}
