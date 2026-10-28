#pragma once
// Homebrew shelves: the top screen (the focused game's cover, maker, license and a line about it; while it downloads,
// the update screen's glitter bar) and the rows' values. Data comes from runtime/homebrew_store.py's hb_ fields.
// Members of App in main.cpp; included once, right after the class.

static std::string homebrew_size(double bytes) {
    char text[32];
    if (bytes >= 1048576) std::snprintf(text, sizeof text, "%.1f MB", bytes / 1048576.0);
    else std::snprintf(text, sizeof text, "%d KB", std::max(1, int(bytes / 1024)));
    return text;
}

std::string App::homebrew_row_value(int i) {
    const auto prefix = "hb_" + std::to_string(i) + "_";
    if (device_status[prefix + "installed"] == "1") return locale.text("In library");
    const double size = std::atof(device_status[prefix + "size"].c_str());
    if (worker > 0 && worker_action == "device.homebrew-get" &&
        device_status["hb_get"] == device_status[prefix + "id"]) {
        const double got = std::atof(device_status["hb_received"].c_str());
        return std::to_string(size > 0 ? int(std::clamp(got / size, 0.0, 1.0) * 100) : 0) + "%";
    }
    return homebrew_size(size);
}

void App::homebrew_top(SDL_Renderer* r, int y) {
    settings_card(r, 24, y + 56, 592, 368);
    // L and R as in a game library: the shoulder tabs name the shelves either side
    const auto shelves = homebrew_shelves();
    if (shelves.size() > 1) {
        const auto at = std::find(shelves.begin(), shelves.end(), homebrew_system);
        const size_t i = at == shelves.end() ? 0 : size_t(at - shelves.begin()), n = shelves.size();
        auto name = [](const std::string& id) { return id == "gba" ? "GBA" : id == "nes" ? "NES" : "DS"; };
        shoulder_tab(r, y, false, "L", name(shelves[(i + n - 1) % n]));
        shoulder_tab(r, y, true, "R", name(shelves[(i + 1) % n]));
    }
    const int count = list_count();
    const std::string shelf = homebrew_system == "gba"   ? "Game Boy Advance"
                              : homebrew_system == "nes" ? "NES"
                                                         : "Nintendo DS";
    if (homebrew_loading()) {
        busy_ring(r, 320, y + 240, 34, 5);
        return;
    }
    if (!count) {
        dsi_note(r, y, y, SettingsIcon::Library, locale.text("Homebrew") + " · " + shelf);
        return;
    }
    const int i = std::clamp(settings.focus, 0, count - 1);
    const auto prefix = "hb_" + std::to_string(i) + "_";
    const std::string title = device_status[prefix + "title"], cover = device_status[prefix + "cover"];

    // The cover on the left (the system's cartridge until it arrives), as on the Game info page.
    const SDL_Rect frame{44, y + 76, 236, 300};
    if (!cover.empty() && fs::exists(cover)) {
        if (!media.ready(r, cover)) media.request(r, cover);
        SDL_Rect box = media.fitted_box(r, cover, frame);
        alpha_round(r, box.x - 2, box.y + 4, box.w + 4, box.h + 4, 12, shadow_colour(), preferences.dark ? 70 : 28);
        round_rect(r, box.x - 4, box.y - 4, box.w + 8, box.h + 8, 12, "line");
        round_rect(r, box.x - 3, box.y - 3, box.w + 6, box.h + 6, 11, "panel");
        media.draw(r, cover, box, false, 255, false, 8);
    } else {
        round_rect(r, frame.x, frame.y, frame.w, frame.h, 14, "slot");
        const auto at = std::find(listed_systems.begin(), listed_systems.end(), homebrew_system);
        system_icon(r, size_t(at - listed_systems.begin()), frame.x + frame.w / 2 - 56, frame.y + frame.h / 2 - 56,
                    112);
    }

    // The right column: title, maker, then the license and size as chips, then a line about it.
    const int left = 304, width = 296;
    int at = y + 82;
    raw_wrapped(r, title, left, at, 26, width, 2);
    at += (label_width(title, 26, false) > width ? 2 : 1) * 33 + 6;
    const std::string developer = device_status[prefix + "developer"];
    if (!developer.empty()) {
        raw_label(r, developer, left, at, 18, "ink", width);
        at += 27;
    }
    at += 8;
    int x = left;
    auto chip = [&](const std::string& text, bool accent) {
        if (text.empty()) return;
        const int w = std::min(width, label_width(text, 14, false) + 26);
        if (x + w > left + width) {
            x = left;
            at += 38;
        }
        if (accent) fill_round(r, x, at, w, 30, 15, theme.at("accent"));
        else {
            round_rect(r, x, at, w, 30, 15, "line");
            round_rect(r, x + 1, at + 1, w - 2, 28, 14, "slot");
        }
        raw_label_colour(r, text, x + 13, centred_label_y(text, 14, at + 15), 14,
                         accent ? theme.at("on_accent") : theme.at("ink"), w - 20);
        x += w + 8;
    };
    std::string license = device_status[prefix + "license"];
    if (license.rfind("GBA Jam 2026", 0) == 0) license = "GBA Jam 2026";
    chip(shelf, true);
    chip(license, false);
    chip(homebrew_size(std::atof(device_status[prefix + "size"].c_str())), false);
    at += 46;

    const bool downloading =
        worker > 0 && worker_action == "device.homebrew-get" && device_status["hb_get"] == device_status[prefix + "id"];
    if (downloading) {
        // the bar where the description goes: received of the file's size
        const double size = std::atof(device_status["hb_size"].c_str()),
                     got = std::atof(device_status["hb_received"].c_str());
        const double fraction = size > 0 ? std::clamp(got / size, 0.0, 1.0) : 0;
        glitter_bar(r, left, at, width, 26, fraction, s.GetTicks(), true);
        const std::string progress = homebrew_size(got) + " / " + homebrew_size(size);
        raw_label(r, progress, left, at + 38, 15, "muted", width);
    } else if (device_status[prefix + "installed"] == "1") {
        fill_round(r, left, at, 160, 34, 17, theme.at("accent"));
        ui_icon(r, SettingsIcon::Library, left + 22, at + 17, 16, "on_accent");
        label(r, "In library", left + 38, centred_label_y(locale.text("In library"), 16, at + 17), 16, "on_accent",
              116);
    } else {
        // a failed download says why, in place of the description, until the next request
        const bool failed = worker <= 0 && worker_action == "device.homebrew-get" && device_status["state"] == "error";
        const std::string about = failed ? device_status["message"] : device_status[prefix + "about"];
        if (!about.empty()) raw_wrapped(r, about, left, at, 16, width, 5);
    }
}
