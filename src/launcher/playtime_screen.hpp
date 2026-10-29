#pragma once
// The Playtime page (Page::Statistics): time by system around a ring, the last
// seven days as bars and four numbers above; the last four weeks and the most
// played games below. Members of App in main.cpp; included once, after the class.

namespace {
// Fixed hues for the systems in the ring and the legend, readable on both themes.
Color playtime_system_colour(const std::string& id) {
    static const std::array<std::pair<const char*, Color>, 11> hues = {{
        {"nds", Color{64, 132, 246}},
        {"gba", Color{132, 94, 236}},
        {"gbc", Color{22, 176, 160}},
        {"gb", Color{128, 166, 52}},
        {"nes", Color{226, 80, 64}},
        {"snes", Color{190, 98, 196}},
        {"md", Color{40, 160, 220}},
        {"sms", Color{236, 150, 40}},
        {"psx", Color{110, 120, 140}},
        {"n64", Color{46, 170, 90}},
        {"ports", Color{214, 120, 80}},
    }};
    for (const auto& [system, colour] : hues)
        if (id == system) return colour;
    return Color{150, 150, 160};
}

std::string playtime_span(uint64_t seconds) {
    const uint64_t hours = seconds / 3600, minutes = (seconds / 60) % 60;
    if (hours >= 100) return std::to_string(hours) + "h";
    if (hours) return std::to_string(hours) + "h" + (minutes ? " " + std::to_string(minutes) + "m" : "");
    if (seconds && seconds < 60) return "<1m";
    return std::to_string(minutes) + "m";
}
} // namespace

// An anti-aliased ring piece, baked once as white coverage and tinted per draw:
// thin lines at neighbouring radii leave pinholes, a signed distance does not.
void App::smooth_arc(SDL_Renderer* r, int cx, int cy, int radius, int thickness, double start, double amount, Color ink,
                     int alpha, bool round_caps) {
    if (amount <= 0 || thickness <= 0) return;
    amount = std::min(1.0, amount);
    const int turn = 1440; // quarter-degree steps keep the cache small
    const int from = int(std::lround(start * turn)) % turn, span = std::max(1, int(std::lround(amount * turn)));
    const auto key = std::make_tuple(r, radius, thickness, from, span, round_caps);
    auto found = arc_textures.find(key);
    SDL_Texture* texture = found == arc_textures.end() ? nullptr : found->second;
    const int side = 2 * (radius + thickness / 2 + 2);
    if (found == arc_textures.end()) {
        if (arc_textures.size() >= 48) {
            for (auto& entry : arc_textures)
                if (entry.second) s.DestroyTexture(entry.second);
            arc_textures.clear();
        }
        const double pi = 3.14159265358979, half = thickness / 2.0, mid = radius, a0 = double(from) / turn * 2 * pi,
                     length = double(span) / turn * 2 * pi;
        const bool full = span >= turn;
        // The two cap centres on the middle of the band.
        const double c0x = std::sin(a0) * mid, c0y = -std::cos(a0) * mid, c1x = std::sin(a0 + length) * mid,
                     c1y = -std::cos(a0 + length) * mid;
        std::vector<uint32_t> pixels(size_t(side) * size_t(side), 0);
        for (int py = 0; py < side; py++)
            for (int px = 0; px < side; px++) {
                const double x = px + .5 - side / 2.0, y = py + .5 - side / 2.0, d = std::hypot(x, y);
                const double band = half - std::abs(d - mid);
                if (band < -1) continue;
                double sd = band;
                if (!full) {
                    // Clockwise from the top, as the rest of the launcher's arcs.
                    double angle = std::atan2(x, -y) - a0;
                    while (angle < 0) angle += 2 * pi;
                    while (angle >= 2 * pi) angle -= 2 * pi;
                    const bool inside = angle <= length;
                    if (round_caps) {
                        if (!inside) sd = half - std::min(std::hypot(x - c0x, y - c0y), std::hypot(x - c1x, y - c1y));
                    } else {
                        const double edge =
                            inside ? std::min(angle, length - angle) : -std::min(angle - length, 2 * pi - angle);
                        sd = std::min(band, d * std::sin(std::clamp(edge, -pi / 2, pi / 2)));
                    }
                }
                const double coverage = std::clamp(sd + .5, 0.0, 1.0);
                if (coverage > 0)
                    pixels[size_t(py) * size_t(side) + size_t(px)] = uint32_t(coverage * 255 + .5) << 24 | 0xffffff;
            }
        texture = s.CreateTexture(r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, side, side);
        if (texture && s.UpdateTexture(texture, nullptr, pixels.data(), side * 4) != 0) {
            s.DestroyTexture(texture);
            texture = nullptr;
        }
        if (texture) s.SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        arc_textures[key] = texture;
    }
    if (!texture) return;
    s.SetTextureColorMod(texture, ink.r, ink.g, ink.b);
    s.SetTextureAlphaMod(texture, Uint8(std::clamp(alpha, 0, 255)));
    SDL_Rect to{cx - side / 2, cy - side / 2, side, side};
    s.RenderCopy(r, texture, nullptr, &to);
}

std::string App::playtime_days(int days) {
    return days == 1 ? locale.text("1 day") : locale.text(std::to_string(days) + " days");
}

void App::playtime_top(SDL_Renderer* r, int y) {
    const auto& group = summary.groups[0];
    const auto& activity = summary.activity;
    settings_card(r, 24, y + top_card_y, 592, top_card_h);
    y += 10; // the content, centred in the card
    const Color panel = theme.at("panel"), accent = theme.at("accent");

    // The ring: all time played, split by system.
    const int rx = 152, ry = y + 176, radius = 84, band = 18;
    smooth_arc(r, rx, ry, radius, band, 0, 1, theme.at("slot"));
    if (summary_ready && group.seconds) {
        // Round caps reach half the band past each end; the gap keeps 3 px between pieces.
        const double gap = summary.systems.size() > 1 ? (band + 3.0) / (2 * 3.14159265358979 * radius) : 0;
        double at = 0;
        for (const auto& [system, seconds] : summary.systems) {
            const double share = double(seconds) / double(group.seconds);
            const double length = std::max(share - gap, .002);
            smooth_arc(r, rx, ry, radius, band, at + gap / 2, length, playtime_system_colour(system));
            at += share;
        }
    }
    const std::string hours = summary_ready ? std::to_string(group.seconds / 3600) + "h" : "…",
                      minutes = summary_ready ? std::to_string((group.seconds / 60) % 60) + "m" : "";
    raw_label(r, hours, rx - label_width(hours, 36, false) / 2, ry - 38, 36, "ink", 140);
    raw_label(r, minutes, rx - label_width(minutes, 20, false) / 2, ry + 4, 20, "muted", 140);
    {
        const std::string caption = locale.text("Time played");
        raw_label(r, caption, rx - label_width(caption, 14, false) / 2, ry + 32, 14, "muted", 132);
    }
    // The legend: the three systems played most, the rest together.
    if (summary_ready && group.seconds && !summary.systems.empty()) {
        std::vector<std::pair<std::string, Color>> chips;
        uint64_t rest = group.seconds;
        for (size_t i = 0; i < summary.systems.size() && i < 3; i++) {
            const auto& [system, seconds] = summary.systems[i];
            const bool last = i == 2 && summary.systems.size() > 3;
            const uint64_t part = last ? rest : seconds;
            rest -= std::min(rest, seconds);
            const int percent = int(std::lround(100.0 * double(part) / double(group.seconds)));
            chips.push_back({(last ? locale.text("Other") : std::string(system_info(system).badge)) + " " +
                                 std::to_string(percent) + "%",
                             last ? Color{150, 150, 160} : playtime_system_colour(system)});
        }
        int width = 0;
        for (const auto& chip : chips) width += 14 + label_width(chip.first, 14, false) + 12;
        // Centred under the ring, but never past the card's padding.
        int x = std::max(40, rx - (width - 12) / 2);
        for (const auto& [text, colour] : chips) {
            fill_round(r, x, y + 288, 8, 8, 4, colour);
            raw_label(r, text, x + 14, centred_label_y(text, 14, y + 292), 14, "muted", 120);
            x += 14 + label_width(text, 14, false) + 12;
        }
    }

    // The last seven days, today on the right.
    const int left = 272, right = 592, chart_top = y + 146, chart_bottom = y + 262;
    label(r, "Last 7 days", left, y + 78, 14, "muted", 200);
    const uint64_t week = activity.week(0), before = activity.week(1);
    const std::string total = summary_ready ? playtime_span(week) : "…";
    raw_label(r, total, left, y + 96, 28, "ink", 200);
    if (summary_ready && before && week != before) {
        // Against the seven days before: green up, the muted ink down.
        const int change = int(std::lround(100.0 * (double(week) - double(before)) / double(before)));
        const std::string text = (change > 0 ? "+" : "") + std::to_string(std::clamp(change, -999, 999)) + "%";
        const Color tone = change > 0 ? Color{40, 168, 96} : theme.at("muted");
        const int px = left + label_width(total, 28, false) + 12, pw = label_width(text, 14, false) + 30;
        alpha_round(r, px, y + 104, pw, 24, 12, tone, preferences.dark ? 60 : 36);
        theme["swatch"] = tone;
        triangle(r, px + 11, y + 112, 5, 7, "swatch", change < 0);
        raw_label_colour(r, text, px + 20, centred_label_y(text, 14, y + 116), 14, tone, pw);
    }
    uint64_t peak = 0;
    for (int i = 0; i < 7; i++) peak = std::max(peak, activity.seconds[size_t(LibrarySummary::Activity::days - 7 + i)]);
    // The scale rounds up to whole half hours, so the guide reads as a time.
    const uint64_t scale = std::max<uint64_t>(1800, (peak + 1799) / 1800 * 1800);
    const int height = chart_bottom - chart_top, column = (right - left) / 7, bar = 26;
    for (int g = 0; g <= 2; g++) {
        const int gy = chart_top + height * g / 2;
        for (int x = left; x < right; x += 8) tint_rect(r, x, gy, 4, 1, theme.at("line"));
    }
    {
        const std::string top_mark = playtime_span(scale);
        raw_label(r, top_mark, right - label_width(top_mark, 14, false), chart_top - 18, 14, "muted", 80);
    }
    static const std::array<const char*, 7> weekdays = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};
    for (int i = 0; i < 7; i++) {
        const uint64_t seconds = activity.seconds[size_t(LibrarySummary::Activity::days - 7 + i)];
        const bool today = i == 6;
        const int bx = left + i * column + (column - bar) / 2;
        const int h = seconds ? std::max(10, int(std::lround(double(height) * double(seconds) / double(scale)))) : 4;
        // Rounded at the top only, standing on the baseline.
        const Color fill = !seconds ? theme.at("slot") : today ? accent : blend(accent, panel, 45);
        const int corner = std::min(8, h / 2);
        fill_round(r, bx, chart_bottom - h, bar, h, corner, fill);
        tint_rect(r, bx, chart_bottom - corner, bar, corner, fill);
        const std::string day = locale.text(weekdays[size_t((activity.weekday + 1 + i) % 7)]);
        const int dx = left + i * column + column / 2 - label_width(day, 14, false) / 2;
        if (today) raw_label_colour(r, day, dx, chart_bottom + 8, 14, accent, column);
        else raw_label(r, day, dx, chart_bottom + 8, 14, "muted", column);
    }
    if (summary_ready && !activity.logged) {
        const int w = right - left - 16, box_y = chart_top + height / 2 - 26;
        fill_round(r, left + 8, box_y, w, 48, 12, panel);
        const std::string note = "History starts with your next game";
        if (label_width(note, 14) <= w - 24) {
            const int nw = label_width(note, 14);
            label(r, note, left + 8 + (w - nw) / 2, centred_label_y(locale.text(note), 14, box_y + 24), 14, "muted",
                  nw + 2);
        } else wrapped(r, note, left + 20, box_y + 4, 14, w - 24, 2);
    }

    // Four numbers along the bottom of the card.
    const uint64_t average = group.sessions ? group.seconds / group.sessions : 0;
    const std::array<std::string, 4> values = {
        summary_ready ? playtime_days(activity.streak) : "…", summary_ready ? std::to_string(group.sessions) : "…",
        summary_ready ? playtime_span(average) : "…",
        summary_ready ? std::to_string(group.played) + " / " + std::to_string(group.games) : "…"};
    static const std::array<const char*, 4> captions = {"Day streak", "Times played", "Average session",
                                                        "Games played"};
    for (int i = 0; i < 4; i++) {
        const int tx = 44 + i * 140, ty = y + 332, bx = tx + 28, by = ty + 26;
        round_rect(r, tx, ty, 132, 96, 12, "slot");
        round_rect(r, tx + 1, ty + 1, 130, 94, 11, "background");
        // One badge style for the four: a soft accent disc, the glyph in accent.
        smooth_arc(r, bx, by, 8, 16, 0, 1, accent, preferences.dark ? 60 : 40);
        if (i == 0) ui_icon(r, SettingsIcon::Spark, bx, by, 18, "accent");
        else if (i == 1) ui_icon(r, SettingsIcon::Play, bx + 1, by, 16, "accent");
        else if (i == 2) {
            smooth_arc(r, bx, by, 7, 2, 0, 1, accent);
            s.SetRenderDrawColor(r, accent.r, accent.g, accent.b, 255);
            for (int w = 0; w < 2; w++) {
                s.RenderDrawLine(r, bx + w, by - 5, bx + w, by);
                s.RenderDrawLine(r, bx, by + w, bx + 4, by + w);
            }
        } else check_mark(r, bx, by, accent);
        raw_label(r, values[size_t(i)], tx + 14, ty + 40, 24, "ink", 110);
        // Long captions step down a size before they would be cut.
        const int size = label_width(captions[size_t(i)], 14) <= 112 ? 14 : 12;
        label(r, captions[size_t(i)], tx + 14, ty + (size == 14 ? 68 : 70), size, "muted", 112);
    }
}

void App::playtime_bottom(SDL_Renderer* r, int y) {
    settings_header(r, y, "Most played");
    const auto& activity = summary.activity;
    const Color panel = theme.at("panel"), accent = theme.at("accent");

    // The last four weeks, one cell a day, brighter for longer days.
    label(r, "Last 4 weeks", 28, y + 94, 14, "muted", 300);
    if (summary_ready && activity.longest) {
        const std::string longest = locale.text("Longest session") + "  " + playtime_span(activity.longest);
        raw_label(r, longest, 612 - label_width(longest, 14, false), y + 94, 14, "muted", 300);
    }
    uint64_t peak = 0;
    for (auto seconds : activity.seconds) peak = std::max(peak, seconds);
    auto cell_x = [](int i) { return 28 + i * 587 / LibrarySummary::Activity::days; };
    for (int i = 0; i < LibrarySummary::Activity::days; i++) {
        const uint64_t seconds = activity.seconds[size_t(i)];
        const int x = cell_x(i), w = cell_x(i + 1) - x - 3;
        const int level =
            !seconds ? 0 : 1 + std::min(3, int(4 * double(seconds) / double(std::max<uint64_t>(1, peak + 1))));
        static const std::array<int, 4> mix = {0, 28, 52, 76};
        const Color fill = level == 0   ? theme.at("slot")
                           : level == 4 ? accent
                                        : blend(panel, accent, mix[size_t(level)]);
        fill_round(r, x, y + 118, w, 28, 5, fill);
        if (i == LibrarySummary::Activity::days - 1) fill_round(r, x + w / 2 - 3, y + 152, 6, 6, 3, accent);
    }
    // Week marks under the strip: each column of seven ends on today's weekday.
    {
        const std::string first = locale.text("4 weeks ago"), last = locale.text("Today");
        raw_label(r, first, 28, y + 150, 14, "muted", 200);
        raw_label(r, last, cell_x(LibrarySummary::Activity::days - 1) - label_width(last, 14, false) - 4, y + 150, 14,
                  "muted", 140);
    }

    if (!summary_ready) {
        wrapped(r, "Preparing your statistics…", 32, y + 196, 24, 560, 2);
        return;
    }
    if (summary.most_played.empty()) {
        wrapped(r, "Play a game to start your statistics.", 32, y + 196, 24, 560, 3);
        return;
    }
    const uint64_t total = std::max<uint64_t>(1, summary.groups[0].seconds),
                   top = std::max<uint64_t>(1, summary.most_played[0].seconds);
    for (size_t i = 0; i < summary.most_played.size(); i++) {
        const auto& entry = summary.most_played[i];
        const int row = y + 172 + int(i) * 49;
        alpha_round(r, 28, row + 2, 584, 45, 10, shadow_colour(), preferences.dark ? 50 : 14);
        round_rect(r, 28, row, 584, 45, 10, "panel");
        // Gold, silver and bronze for the top three; a plain number after that.
        static const std::array<Color, 3> medals = {Color{236, 178, 48}, Color{168, 180, 192}, Color{196, 128, 78}};
        const std::string n = std::to_string(i + 1);
        if (i < 3) {
            fill_round(r, 38, row + 11, 24, 24, 12, medals[i]);
            alpha_round(r, 42, row + 13, 16, 7, 4, Color{255, 255, 255}, 90);
            raw_label_colour(r, n, 50 - label_width(n, 14, false) / 2, centred_label_y(n, 14, row + 23), 14,
                             Color{255, 255, 255}, 24);
        } else raw_label(r, n, 50 - label_width(n, 16, false) / 2, centred_label_y(n, 16, row + 23), 16, "muted", 24);
        game_icon(r, entry.game, 70, row + 5, 36, i);
        raw_label(r, entry.title, 116, row + 1, 20, "ink", 300);
        // The bar is this game against the most played; the number its share of all time.
        const Color hue = playtime_system_colour(entry.game.system);
        const int track = 236, filled = std::max(6, int(track * entry.seconds / top));
        fill_round(r, 116, row + 31, track, 6, 3, theme.at("slot"));
        fill_round(r, 116, row + 31, filled, 6, 3, hue);
        const std::string share = std::to_string(int(std::lround(100.0 * double(entry.seconds) / double(total)))) + "%";
        raw_label(r, share, 116 + track + 8, centred_label_y(share, 14, row + 34), 14, "muted", 48);
        const std::string time = playtime_span(entry.seconds);
        raw_label(r, time, 600 - label_width(time, 20, false), row + 1, 20, "accent", 140);
        std::string when = entry.days_ago < 0    ? ""
                           : entry.days_ago == 0 ? locale.text("Today")
                           : entry.days_ago == 1 ? locale.text("Yesterday")
                                                 : locale.text(std::to_string(entry.days_ago) + " days ago");
        const std::string detail = std::to_string(entry.sessions) + "×" + (when.empty() ? "" : "  ·  " + when);
        raw_label(r, detail, 600 - label_width(detail, 14, false), centred_label_y(detail, 14, row + 34), 14, "muted",
                  180);
    }
}
