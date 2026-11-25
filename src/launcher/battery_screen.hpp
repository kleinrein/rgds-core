#pragma once
// Settings > Power > Battery health (Page::BatteryHealth), as a phone shows its battery: the charge now around a
// ring with the time left and the power drawn, the last 24 hours as a level chart (charging, in use, asleep), and
// four numbers; below, the maximum capacity against new, what the battery did since it was unplugged, and what used
// it over the last 7 days. The data is battery_health.hpp's. Members of App in main.cpp; included once, after the
// class.

namespace {
const Color battery_green{40, 168, 96}, battery_orange{239, 129, 75}, battery_red{226, 72, 60};

Color battery_level_colour(int level, bool plugged) {
    return plugged || level > 20 ? battery_green : level > 10 ? battery_orange : battery_red;
}

std::string battery_watts(double watts) {
    char text[16];
    std::snprintf(text, sizeof text, "%.1f W", std::abs(watts));
    return text;
}
} // namespace

void App::open_battery_health() {
    settings.remember(Page::Power, settings.focus);
    settings.page = Page::BatteryHealth;
    settings.focus = 0;
    battery_refresh();
}

void App::battery_refresh() {
    battery_read_at = s.GetTicks();
    const auto gauge = read_battery_gauge(battery_supply_root());
    std::ifstream file(o.data / "logs/battery-history.tsv");
    const auto rows = read_battery_history(file);
    std::vector<BatterySitting> sittings;
    if (summary_ready)
        for (const auto& entry : summary.journal.entries)
            sittings.push_back({entry.began, entry.seconds, shelf_of(entry.game.system)});
    battery = battery_report(gauge, rows, std::move(sittings), (long long)std::time(nullptr));
    dirty = true;
}

std::string App::battery_status_line() {
    const auto& g = battery.gauge;
    if (!g.present) return locale.text("No battery found");
    if (g.plugged && (g.status == 'F' || g.level >= 100)) return locale.text("Fully charged");
    if (g.plugged && g.status == 'C')
        return battery.minutes > 0 ? locale.text("Full in " + playtime_span(uint64_t(battery.minutes) * 60))
                                   : locale.text("Charging");
    if (g.plugged) return locale.text("On the charger, not charging");
    if (battery.minutes < 0) return locale.text("On battery");
    return battery.minutes >= 120
               ? locale.text("About " + std::to_string((battery.minutes + 30) / 60) + " h left")
               : locale.text("About " + std::to_string(std::max(1LL, battery.minutes)) + " min left");
}

void App::battery_top(SDL_Renderer* r, int y) {
    const auto& g = battery.gauge;
    settings_card(r, 24, y + top_card_y, 592, top_card_h);
    y += 10;
    const Color panel = theme.at("panel"), accent = theme.at("accent");

    // The ring: the charge now, green on the charger or above 20 %.
    const int rx = 152, ry = y + 176, radius = 84, band = 18;
    smooth_arc(r, rx, ry, radius, band, 0, 1, theme.at("slot"));
    if (g.present && g.level > 0)
        smooth_arc(r, rx, ry, radius, band, 0, g.level / 100.0, battery_level_colour(g.level, g.plugged));
    {
        const std::string level = g.present && g.level >= 0 ? std::to_string(g.level) + "%" : "–";
        raw_label(r, level, rx - label_width(level, 36, false) / 2, ry - 30, 36, "ink", 140);
        const std::string state = !g.present                     ? ""
                                  : g.plugged && g.status == 'C' ? locale.text("Charging")
                                  : g.plugged                    ? locale.text("Plugged in")
                                                                 : locale.text("On battery");
        if (g.plugged && g.status == 'C')
            raw_label_colour(r, state, rx - label_width(state, 14, false) / 2, ry + 12, 14, battery_green, 132);
        else raw_label(r, state, rx - label_width(state, 14, false) / 2, ry + 12, 14, "muted", 132);
    }
    {
        const std::string line = battery_status_line();
        raw_label(r, line, std::max(40, rx - label_width(line, 16, false) / 2), y + 276, 16, "ink", 220);
        if (g.present && std::abs(battery.watts) >= .05) {
            const std::string power =
                (battery.watts > 0 ? locale.text("Charging at") + " " : locale.text("Using") + " ") +
                battery_watts(battery.watts);
            raw_label(r, power, std::max(40, rx - label_width(power, 14, false) / 2), y + 300, 14, "muted", 220);
        }
    }

    // The last 24 hours: the level at each half hour, coloured by what the handheld was doing.
    const int left = 272, right = 592, chart_top = y + 114, chart_bottom = y + 250;
    label(r, "Last 24 hours", left, y + 78, 14, "muted", 220);
    const int height = chart_bottom - chart_top;
    for (int g2 = 0; g2 <= 2; g2++) {
        const int gy = chart_top + height * g2 / 2;
        for (int x = left; x < right - 40; x += 8) tint_rect(r, x, gy, 4, 1, theme.at("line"));
        const std::string mark = std::to_string(100 - 50 * g2) + "%";
        raw_label(r, mark, right - label_width(mark, 12, false), centred_label_y(mark, 12, gy), 12, "muted", 40);
    }
    const int plot = right - 44 - left, column = plot * 1000 / BatteryReport::slots,
              bar = std::max(3, column / 1000 - 2);
    bool any = false;
    for (int i = 0; i < BatteryReport::slots; i++) {
        const int level = battery.level[size_t(i)];
        const char kind = battery.kind[size_t(i)];
        const int bx = left + i * column / 1000;
        if (level < 0 || !kind) {
            fill_round(r, bx, chart_bottom - 3, bar, 3, 1, theme.at("slot"));
            continue;
        }
        any = true;
        const int h = std::max(3, height * level / 100);
        const Color fill = kind == 'c'   ? battery_green
                           : kind == 's' ? blend(panel, theme.at("muted"), 45)
                           : level <= 20 ? battery_orange
                                         : accent;
        const int corner = std::min(2, bar / 2);
        fill_round(r, bx, chart_bottom - h, bar, h, corner, fill);
        tint_rect(r, bx, chart_bottom - corner, bar, corner, fill);
    }
    {
        const std::string first = locale.text("24 h ago"), middle = locale.text("12 h ago"), last = locale.text("Now");
        raw_label(r, first, left, chart_bottom + 8, 14, "muted", 110);
        raw_label(r, middle, left + plot / 2 - label_width(middle, 14, false) / 2, chart_bottom + 8, 14, "muted", 110);
        raw_label(r, last, left + plot - label_width(last, 14, false), chart_bottom + 8, 14, "muted", 80);
    }
    // the legend
    {
        const std::array<std::pair<const char*, Color>, 3> chips = {
            {{"Charging", battery_green}, {"In use", accent}, {"Asleep", blend(panel, theme.at("muted"), 45)}}};
        int x = left;
        for (const auto& [name, colour] : chips) {
            const std::string text = locale.text(name);
            fill_round(r, x, y + 288, 8, 8, 4, colour);
            raw_label(r, text, x + 14, centred_label_y(text, 14, y + 292), 14, "muted", 110);
            x += 14 + label_width(text, 14, false) + 16;
        }
    }
    if (!any || battery.rows < 2) {
        const int w = plot - 16, box_y = chart_top + height / 2 - 26;
        fill_round(r, left + 8, box_y, w, 48, 12, panel);
        wrapped(r, "The chart fills in as you play", left + 20, box_y + 4, 14, w - 24, 2);
    }

    // Four numbers along the bottom of the card, as Playtime's.
    char sleep[24] = "–";
    if (battery.sleep_ma >= 0 && battery.gauge.full > 0)
        std::snprintf(sleep, sizeof sleep, "%.1f%%/h", 100.0 * battery.sleep_ma * 1000 / double(battery.gauge.full));
    char cycles[16] = "–";
    if (battery.cycles >= 0)
        std::snprintf(cycles, sizeof cycles, battery.cycles < 10 ? "%.1f" : "%.0f", battery.cycles);
    const std::array<std::string, 4> values = {
        battery.health >= 0 ? std::to_string(battery.health) + "%" : "–", cycles,
        g.plugged || !battery.unplugged ? "–" : playtime_span(uint64_t(battery.awake + battery.asleep)), sleep};
    static const std::array<const char*, 4> captions = {"Maximum capacity", "Charge cycles", "Since unplugged",
                                                        "Asleep"};
    for (int i = 0; i < 4; i++) {
        const int tx = 44 + i * 140, ty = y + 332, bx = tx + 28, by = ty + 26;
        round_rect(r, tx, ty, 132, 96, 12, "slot");
        round_rect(r, tx + 1, ty + 1, 130, 94, 11, "background");
        smooth_arc(r, bx, by, 8, 16, 0, 1, accent, preferences.dark ? 60 : 40);
        if (i == 0) ui_icon(r, SettingsIcon::Battery, bx, by, 18, "accent");
        else if (i == 1) ui_icon(r, SettingsIcon::Refresh, bx, by, 16, "accent");
        else if (i == 2) ui_icon(r, SettingsIcon::Clock, bx, by, 16, "accent");
        else ui_icon(r, SettingsIcon::Moon, bx, by, 16, "accent");
        raw_label(r, values[size_t(i)], tx + 14, ty + 40, 24, "ink", 110);
        const int size = label_width(captions[size_t(i)], 14) <= 112 ? 14 : 12;
        label(r, captions[size_t(i)], tx + 14, ty + (size == 14 ? 68 : 70), size, "muted", 112);
    }
}

void App::battery_bottom(SDL_Renderer* r, int y) {
    settings_header(r, y, "Battery health");
    const auto& g = battery.gauge;
    const Color accent = theme.at("accent");
    auto card = [&](int top, int h) {
        alpha_round(r, 28, top + 2, 584, h, 12, shadow_colour(), preferences.dark ? 50 : 14);
        round_rect(r, 28, top, 584, h, 12, "panel");
    };
    if (!g.present) {
        wrapped(r, "This handheld does not report its battery.", 32, y + 110, 24, 560, 3);
        return;
    }

    // Maximum capacity: the gauge's full charge against the cell's design, and what that means.
    int top = y + 90;
    card(top, 96);
    label(r, "Maximum capacity", 44, top + 10, 20, "ink", 360);
    const std::string health = battery.health >= 0 ? std::to_string(battery.health) + "%" : "–";
    raw_label(r, health, 596 - label_width(health, 28, false), top + 4, 28, "accent", 120);
    {
        const int track = 552, filled = battery.health > 0 ? std::max(6, track * battery.health / 100) : 0;
        fill_round(r, 44, top + 46, track, 8, 4, theme.at("slot"));
        if (filled) fill_round(r, 44, top + 46, filled, 8, 4, battery.health >= 80 ? battery_green : battery_orange);
    }
    {
        const std::string verdict = battery.health < 0     ? "The battery's capacity is not known yet."
                                    : battery.health >= 80 ? "Your battery is in good health."
                                                           : "Your battery holds clearly less than when new.";
        label(r, verdict, 44, top + 64, 14, "muted", 380);
        if (g.full > 0 && g.design > 0) {
            const std::string mah = std::to_string(g.full / 1000) + " / " + std::to_string(g.design / 1000) + " mAh";
            raw_label(r, mah, 596 - label_width(mah, 14, false), top + 64, 14, "muted", 170);
        }
    }

    // Since unplugged: in use against asleep, as one bar of what each took.
    top = y + 198;
    card(top, 76);
    if (g.plugged) {
        label(r, "On the charger", 44, top + 10, 20, "ink", 360);
        std::string detail = locale.text("Voltage");
        char volts[24];
        std::snprintf(volts, sizeof volts, " %.2f V", double(g.voltage) / 1e6);
        detail += volts;
        raw_label(r, detail, 44, top + 44, 14, "muted", 360);
    } else {
        label(r, "Since unplugged", 44, top + 10, 20, "ink", 300);
        if (battery.charged_to >= 0 && battery.unplugged) {
            const std::string from = locale.text("Charged to " + std::to_string(battery.charged_to) + "%");
            raw_label(r, from, 596 - label_width(from, 14, false), top + 14, 14, "muted", 240);
        }
        const double used = battery.awake_used + battery.asleep_used;
        const int track = 552;
        fill_round(r, 44, top + 40, track, 8, 4, theme.at("slot"));
        if (used > 0) {
            const int awake = int(track * std::min(100.0, battery.awake_used) / 100),
                      asleep = int(track * std::min(100.0, battery.asleep_used) / 100);
            if (awake > 0) fill_round(r, 44, top + 40, std::max(6, awake), 8, 4, accent);
            if (asleep > 0)
                fill_round(r, 44 + awake, top + 40, std::max(6, asleep), 8, 4,
                           blend(theme.at("panel"), theme.at("muted"), 45));
        }
        auto part = [&](const char* name, long long seconds, double percent) {
            return locale.text(name) + " " + playtime_span(uint64_t(std::max(0LL, seconds))) + " · " +
                   std::to_string(int(std::lround(percent))) + "%";
        };
        const std::string awake = part("In use", battery.awake, battery.awake_used),
                          asleep = part("Asleep", battery.asleep, battery.asleep_used);
        raw_label(r, awake, 44, top + 52, 14, "muted", 270);
        raw_label(r, asleep, 596 - label_width(asleep, 14, false), top + 52, 14, "muted", 270);
    }

    // The last 7 days on battery: what used it.
    top = y + 286;
    label(r, "Last 7 days on battery", 32, top, 14, "muted", 300);
    if (battery.game_watts > 0 || battery.menu_watts > 0) {
        std::string average;
        if (battery.game_watts > 0) average += locale.text("Games") + " " + battery_watts(battery.game_watts);
        if (battery.menu_watts > 0)
            average +=
                (average.empty() ? "" : "  ·  ") + locale.text("Menus") + " " + battery_watts(battery.menu_watts);
        raw_label(r, average, 612 - label_width(average, 14, false), top, 14, "muted", 300);
    }
    if (battery.uses.empty()) {
        wrapped(r, "What uses your battery shows here after some time unplugged.", 32, top + 28, 16, 576, 2);
        return;
    }
    const double total = std::max(1.0, battery.used_mah), most = std::max(1.0, battery.uses[0].second);
    for (size_t i = 0; i < battery.uses.size() && i < 4; i++) {
        const auto& [what, mah] = battery.uses[i];
        const int row = top + 24 + int(i) * 30;
        const Color hue = what == "sleep" ? blend(theme.at("panel"), theme.at("muted"), 45)
                          : what.empty()  ? accent
                                          : playtime_system_colour(what);
        fill_round(r, 34, row + 8, 10, 10, 5, hue);
        const std::string name = what == "sleep" ? locale.text("Asleep")
                                 : what.empty()  ? locale.text("Menus & Home")
                                                 : std::string(system_info(what).name);
        raw_label(r, name, 54, row, 16, "ink", 230);
        const int track = 240, filled = std::max(6, int(track * mah / most));
        fill_round(r, 292, row + 9, track, 6, 3, theme.at("slot"));
        fill_round(r, 292, row + 9, filled, 6, 3, hue);
        const std::string share = std::to_string(int(std::lround(100.0 * mah / total))) + "%";
        raw_label(r, share, 608 - label_width(share, 16, false), row, 16, "accent", 60);
    }
}
