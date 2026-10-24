#pragma once
// The status bar: battery, Wi-Fi and clock icons, plus the sheen helpers and artwork loader icon.
// Members of App in main.cpp; included once, right after the class.

void App::sheen(SDL_Renderer* r, int x, int y, int w, int h, const std::string& key, int strength) {
    auto base = theme.at(key);
    for (int i = 0; i < 12; i++) {
        int top = y + h * i / 12, bottom = y + h * (i + 1) / 12;
        auto shade = blend(base, i < 6 ? theme.at("panel") : theme.at("background"), strength * std::abs(6 - i) / 6);
        tint_rect(r, x, top, w, bottom - top, shade);
    }
}

void App::horizontal_sheen(SDL_Renderer* r, int x, int y, int w, int h, const std::string& key, int strength) {
    auto base = theme.at(key), highlight = theme.at("panel");
    int span = std::max(1, w - 1);
    for (int column = 0; column < w; column++)
        tint_rect(r, x + column, y, 1, h, blend(base, highlight, strength * (w - 1 - column) / span));
}

void App::triangle(SDL_Renderer* r, int x, int y, int half, int height, const std::string& key, bool down) {
    color(r, key);
    for (int i = 0; i < height; i++) {
        int width = half * (height - i) / height;
        int row = down ? y + i : y + height - 1 - i;
        s.RenderDrawLine(r, x - width, row, x + width, row);
    }
}

void App::battery_icon(SDL_Renderer* r, int y) {
    const int x = 576, top = y + 14;
    round_rect(r, x, top, 38, 20, 3, "muted");
    round_rect(r, x + 2, top + 2, 34, 16, 2, "slot");
    rect(r, x + 38, top + 6, 3, 8, "muted");
    int bars = hardware.battery < 0    ? 0
               : hardware.battery > 75 ? 4
               : hardware.battery > 45 ? 3
               : hardware.battery > 15 ? 2
               : hardware.battery > 0  ? 1
                                       : 0;
    drawn_charge_phase = hardware.charging ? int((s.GetTicks() / 420) % 4) : -1;
    if (hardware.charging) bars = 1 + drawn_charge_phase;
    auto level = hardware.battery >= 0 && hardware.battery <= 15 && !hardware.charging ? Color{239, 129, 75}
                                                                                       : theme.at("accent");
    for (int i = 0; i < 4; i++)
        tint_rect(r, x + 5 + i * 7, top + 5, 5, 10, i < bars ? level : blend(theme.at("slot"), theme.at("muted"), 16));
    if (hardware.charging) {
        color(r, "accent");
        s.RenderDrawLine(r, x - 10, top + 1, x - 16, top + 10);
        s.RenderDrawLine(r, x - 16, top + 10, x - 9, top + 10);
        s.RenderDrawLine(r, x - 9, top + 10, x - 15, top + 19);
    } else if (hardware.external_power) {
        color(r, "accent");
        s.RenderDrawLine(r, x - 15, top + 3, x - 15, top + 8);
        s.RenderDrawLine(r, x - 10, top + 3, x - 10, top + 8);
        s.RenderDrawLine(r, x - 17, top + 8, x - 8, top + 8);
        s.RenderDrawLine(r, x - 17, top + 8, x - 17, top + 12);
        s.RenderDrawLine(r, x - 8, top + 8, x - 8, top + 12);
        s.RenderDrawLine(r, x - 17, top + 12, x - 8, top + 12);
        s.RenderDrawLine(r, x - 13, top + 12, x - 13, top + 18);
    }
}

fs::path App::status_asset(const std::string& name) {
    return asset_path(std::string("status/") + (preferences.dark ? "dark-" : "light-") + name + ".bmp");
}

std::string App::wifi_asset() {
    if (device_status["wifi"] != "1") return "wifi-off";
    int signal = -1;
    if (!device_status["wifi_signal"].empty()) signal = std::atoi(device_status["wifi_signal"].c_str());
    if (probed_signal >= 0 && device_status["wifi_connected"] == "1") signal = probed_signal;
    int level = signal >= 67 ? 3 : signal >= 34 ? 2 : signal > 0 ? 1 : 0;
    return "wifi-" + std::to_string(level);
}

bool App::artwork_busy() const {
    auto found = service.find("state");
    return found != service.end() && found->second == "scraping";
}

void App::artwork_loader(SDL_Renderer* r, int y) {
    static const int dx[8] = {0, 5, 7, 5, 0, -5, -7, -5}, dy[8] = {-7, -5, 0, 5, 7, 5, 0, -5};
    const int head = drawn_loader_phase = loader_phase(s.GetTicks()), cx = 232, cy = y + 24;
    const Color ink = theme.at("accent");
    s.SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    for (int i = 0; i < 8; i++) {
        static const Uint8 tail[8] = {255, 200, 150, 105, 70, 50, 40, 40};
        s.SetRenderDrawColor(r, ink.r, ink.g, ink.b, tail[(head - i + 8) % 8]);
        SDL_Rect dot{cx + dx[i] - 2, cy + dy[i] - 2, 4, 4};
        s.RenderFillRect(r, &dot);
    }
    s.SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    auto count = [&](const char* key) {
        auto found = service.find(key);
        return found == service.end() ? std::string("0") : found->second;
    };
    const std::string progress = count("completed") + "/" + count("total");
    label(r, progress, 246, status_label_y(progress, 16, y), 16, "muted", 110, false);
}

int App::label_width(const std::string& source, int size, bool translate) {
    auto value = translate ? locale.text(source) : source;
    return preferences.pixel ? pixel_cells(value) * pixel_scale(size) * 6 : int(std::ceil(rounded.width(value, size)));
}

std::string App::status_stamp() {
    auto now = std::time(nullptr);
    char stamp[32];
    const auto& local = *std::localtime(&now);
    std::strftime(stamp, sizeof(stamp), date_format_patterns.at(system_preferences.date_format), &local);
    return std::string(stamp) + "  " + time_text(local);
}

int App::centred_label_y(const std::string& value, int size, int cy) {
    auto bounds = preferences.pixel ? std::pair<int, int>{pixel_scale(size) * 3, pixel_scale(size) * 10}
                                    : rounded.ink_bounds(value, size);
    return cy - (bounds.first + bounds.second + 1) / 2;
}

void App::status_bar(SDL_Renderer* r, int y) {
    sheen(r, 0, y, 640, 48, "background", 24);
    if (!preferences.rich_icons) {
        // The pixel style's own status symbols, at the same 24 px as its icons.
        draw_pixel_glyph(s, r, pixel_status_glyph(sound_asset()), 24, y + 24, 24, theme.at("ink"));
        draw_pixel_glyph(s, r, pixel_status_glyph(wifi_asset()), 58, y + 24, 24, theme.at("ink"));
    } else {
        media.draw(r, status_asset(sound_asset()), {10, y + 10, 28, 28}, false);
        media.draw(r, status_asset(wifi_asset()), {44, y + 10, 28, 28}, false);
    }
    // Rendered at its drawn size; its capitals match the clock's (14 px),
    // centred on the bar's text line (y + 24) like the labels.
    if (!preferences.rich_icons) {
        // The simple pixel style sets the name in its own dots, not from the rendered wordmark.
        const int dot = pixel_scale(20);
        color(r, "ink");
        pixel_dots(
            "RG DS CORE", 84, status_label_y("RG DS CORE", 20, y), dot, 12,
            [&](const SDL_Rect& p) { s.RenderFillRect(r, &p); }, [](unsigned, int, int) {});
    } else
        media.draw(r, asset_path(preferences.dark ? "brand/status-dark.bmp" : "brand/status-light.bmp"),
                   {84, y + 13, 129, 22}, false);
    if (artwork_busy()) artwork_loader(r, y);
    drawn_status_stamp = status_stamp();
    const auto& stamp = drawn_status_stamp;
    int right = hardware.battery >= 0 && system_preferences.battery_percent ? 494 : 550;
    label(r, stamp, right - label_width(stamp, 20), status_label_y(stamp, 20, y), 20, "ink", right - 180);
    update_status_chip(r, y, right - label_width(stamp, 20) - 14);
    battery_icon(r, y);
    if (system_preferences.battery_percent && hardware.battery >= 0) {
        auto percent = std::to_string(hardware.battery) + "%";
        label(r, percent, 554 - label_width(percent, 20), status_label_y(percent, 20, y), 20, "muted", 65);
    }
    rect(r, 0, y + 47, 640, 1, "line");
}
