#pragma once
// Opening, matching and reviewing game artwork.
// Members of App in main.cpp; included once, right after the class.

void App::open_game_artwork(Page back) {
    if (model.games.empty()) return;
    artwork_return = back;
    artwork_match = fields(item_folder(model.selected) / "artwork.ini")["match_title"];
    artwork_result = fields(item_folder(model.selected) / "artwork-status.ini");
    service_notice.clear();
    settings.page = Page::GameArtwork;
    settings.focus = 0;
    dirty = true;
}

bool App::save_artwork_match(const std::string& title) {
    try {
        auto path = item_folder(model.selected) / "artwork.ini";
        fs::create_directories(path.parent_path());
        auto pending = path.string() + ".pending";
        std::ofstream file(pending);
        std::string clean = title;
        std::replace(clean.begin(), clean.end(), '\n', ' ');
        std::replace(clean.begin(), clean.end(), '\r', ' ');
        file << "schema=1\nmatch_title=" << clean << '\n';
        file.close();
        if (!file) throw std::runtime_error("Could not save matching title.");
        fs::rename(pending, path);
        artwork_match = clean;
        return true;
    } catch (const std::exception&) {
        service_notice = "Could not save matching title.";
        return false;
    }
}

void App::review_artwork() {
    std::vector<std::string> names;
    std::vector<size_t> targets;
    for (size_t i = 0; i < model.games.size(); i++) {
        auto result = fields(item_folder(i) / "artwork-status.ini");
        std::error_code ec;
        if (!fs::exists(item_folder(i) / "box.bmp", ec) || result["state"] == "error" || result["state"] == "missing") {
            names.push_back(game_title(i));
            targets.push_back(i);
        }
    }
    if (names.empty()) {
        service_notice =
            model.games.empty() ? "No games in this collection." : "Every game in this collection has artwork.";
        dirty = true;
        return;
    }
    show_picker("Unmatched artwork", names, 0, [this, targets](int n) {
        select_game(targets.at(n), false);
        open_game_artwork(Page::Scraper);
    });
}

void App::game_artwork_action() {
    if (worker > 0 && worker_action == "scrape") {
        if (settings.focus == 1) {
            std::ofstream(o.data / "scraper/cancel").put('\n');
            service_notice = "Stopping after this request.";
        } else service_notice = "Stop fetching before editing the match.";
        dirty = true;
        return;
    }
    if (settings.focus == 0) {
        begin_edit(3);
        return;
    }
    if (settings.focus == 2) {
        if (save_artwork_match("")) service_notice = "Using the ROM's game code and filename.";
        dirty = true;
        return;
    }
    if (worker > 0 && (worker_action == "index" || worker_action == "device.info" || worker_action == "status"))
        stop_worker();
    if (model.games[model.selected].demo) {
        service_notice = "Add your DS games to fetch their artwork.";
        dirty = true;
        return;
    }
    start_worker("scrape");
}
