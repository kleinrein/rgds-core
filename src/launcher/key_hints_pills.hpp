#pragma once
// Key badges and hints, shoulder tabs, touch feedback, loading overlay and pill buttons.
// Members of App in main.cpp; included once, right after the class.

void App::centered_wrapped(SDL_Renderer* r, const std::string& value, int y, int pixels, int width, int rows,
                           const std::string& key) {
    std::istringstream words(value);
    std::string word, line;
    std::vector<std::string> lines;
    while (words >> word) {
        std::string next = line.empty() ? word : line + " " + word;
        if (!line.empty() && label_width(next, pixels, false) > width) {
            lines.push_back(line);
            line = word;
        } else line = next;
    }
    if (!line.empty()) lines.push_back(line);
    for (int i = 0; i < std::min<int>(rows, int(lines.size())); i++) {
        int w = std::min(width, label_width(lines[size_t(i)], pixels, false));
        raw_label(r, lines[size_t(i)], 320 - w / 2, y + i * (pixels + 8), pixels, key, width);
    }
}

void App::alpha_round(SDL_Renderer* r, int x, int y, int w, int h, int radius, Color fill, int alpha) {
    radius = std::clamp(radius, 0, std::min(w, h) / 2);
    s.SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    s.SetRenderDrawColor(r, fill.r, fill.g, fill.b, Uint8(std::clamp(alpha, 0, 255)));
    for (int dy = 0; dy < radius; dy++) {
        int inset = radius - int(std::sqrt(radius * radius - (radius - dy - 0.5) * (radius - dy - 0.5)));
        SDL_Rect a{x + inset, y + dy, w - inset * 2, 1}, b{x + inset, y + h - 1 - dy, w - inset * 2, 1};
        s.RenderFillRect(r, &a);
        s.RenderFillRect(r, &b);
    }
    SDL_Rect middle{x, y + radius, w, h - radius * 2};
    s.RenderFillRect(r, &middle);
}

void App::shoulder_fill(SDL_Renderer* r, int x, int y, int w, int h, bool right, Color fill, int alpha) {
    const double rx = std::min(w * .45, h * .7), ry = h * .8;
    const int small = std::min(4, h / 2);
    const auto corner = [](double radius, int row) {
        return row >= radius ? 0 : int(radius - std::sqrt(radius * radius - (radius - row - .5) * (radius - row - .5)));
    };
    s.SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    s.SetRenderDrawColor(r, fill.r, fill.g, fill.b, Uint8(std::clamp(alpha, 0, 255)));
    for (int dy = 0; dy < h; dy++) {
        const double t = std::max(0.0, (ry - dy - .5) / ry);
        const int sweep = dy < ry ? int(std::lround(rx - rx * std::sqrt(std::max(0.0, 1 - t * t)))) : 0,
                  flat = std::max(corner(small, dy), corner(small, h - 1 - dy)),
                  outer = std::max(sweep, corner(small, h - 1 - dy)), inner = flat;
        SDL_Rect line{x + (right ? inner : outer), y + dy, w - inner - outer, 1};
        s.RenderFillRect(r, &line);
    }
}

int App::key_badge_width(const std::string& key) {
    return shoulder_key(key) ? std::max(34, label_width(key, 14, false) + 20)
                             : std::max(key_badge_size, label_width(key, 14, false) + 14);
}

int App::key_badge(SDL_Renderer* r, int x, int cy, const std::string& key, bool enabled, bool inverse) {
    const int w = key_badge_width(key), y = cy - key_badge_size / 2;
    const Color fill = theme.at(inverse   ? "on_accent"
                                : enabled ? "accent"
                                          : "slot"),
                ink = theme.at(inverse   ? "accent"
                               : enabled ? "on_accent"
                                         : "muted");
    if (shoulder_key(key)) {
        const bool right = key[0] == 'R';
        shoulder_fill(r, x, y, w, key_badge_size, right, fill);
        if (enabled && !inverse)
            shoulder_fill(r, x + (right ? 3 : 8), y + 2, w - 11, 6, right, Color{255, 255, 255}, 56);
        // The letter leans away from the deep curve, onto the flat part of the tab.
        const int text_x = x + (w - label_width(key, 14, false)) / 2 + (right ? -3 : 3);
        raw_label_colour(r, key, text_x, centred_label_y(key, 14, cy), 14, ink, w);
        return w;
    }
    fill_round(r, x, y, w, key_badge_size, key_badge_size / 2, fill);
    if (enabled && !inverse) alpha_round(r, x + 4, y + 2, w - 8, 7, 3, Color{255, 255, 255}, 56);
    raw_label_colour(r, key, x + (w - label_width(key, 14, false)) / 2, centred_label_y(key, 14, cy), 14, ink, w);
    return w;
}

int App::key_group(SDL_Renderer* r, int x, int cy, const std::string& keys, bool enabled, bool inverse) {
    int w = 0;
    std::istringstream words(keys);
    for (std::string key; words >> key;) {
        if (w) w += 4;
        w += r ? key_badge(r, x + w, cy, key, enabled, inverse) : key_badge_width(key);
    }
    return w;
}

int App::key_hints_width(const std::vector<KeyHint>& hints, int size) {
    int w = 0;
    for (auto& hint : hints)
        w += (w ? 18 : 0) + key_group(nullptr, 0, 0, hint.key) +
             (hint.text.empty() ? 0 : 8 + label_width(hint.text, size));
    return w;
}

int App::key_hints(SDL_Renderer* r, int x, int cy, const std::vector<KeyHint>& hints, int align, int size, int limit,
                   const std::string& ink, bool inverse) {
    while (size > 12 && key_hints_width(hints, size) > limit) size -= 2;
    const int width = key_hints_width(hints, size);
    int at = align < 0 ? x : align == 0 ? x - width / 2 : x - width;
    for (auto& hint : hints) {
        at += key_group(r, at, cy, hint.key, true, inverse);
        if (!hint.text.empty()) {
            const auto value = locale.text(hint.text);
            const int w = label_width(value, size, false);
            raw_label(r, value, at + 8, centred_label_y(value, size, cy), size, ink, w + 4);
            at += 8 + w;
        }
        at += 18;
    }
    return width;
}

void App::dsi_note(SDL_Renderer* r, int y, int screen, SettingsIcon symbol, const std::string& title) {
    const int x = 24, top = screen + 64, w = 592, h = 356;
    alpha_round(r, x, top + 3, w, h, 18, shadow_colour(), preferences.dark ? 60 : 20);
    fill_round(r, x, top, w, h, 18, theme.at("accent"));
    fill_round(r, x + 2, top + 2, w - 4, h - 4, 16, theme.at("panel"));
    ui_icon(r, symbol, 320, y + 118, 40, "accent");
    centered_wrapped(r, title, y + 148, 24, 540, 1, "ink");
}

void App::shoulder_tab(SDL_Renderer* r, int y, bool right, const std::string& key, const std::string& text) {
    const int h = 36, top = y + 430, cy = top + h / 2, size = 16;
    const int w = std::min(300, 14 + key_badge_width(key) + 8 + label_width(text, size, false) + 16),
              left = right ? 640 - w : 0;
    alpha_round(r, right ? left : left - 18, top + 3, w + 18, h, 14, shadow_colour(), preferences.dark ? 60 : 20);
    fill_round(r, right ? left : left - 18, top, w + 18, h, 14, theme.at("panel"));
    const int badge = right ? 640 - 14 - key_badge_width(key) : 14;
    key_badge(r, badge, cy, key);
    const int text_x = right ? left + 16 : badge + key_badge_width(key) + 8;
    raw_label(r, text, text_x, centred_label_y(text, size, cy), size, "ink", w - key_badge_width(key) - 38);
}

std::string App::system_row_name(int row) const {
    static const std::array<const char*, 9> names = {"Language",           "Timezone",        "Daylight saving",
                                                     "Date format",        "Time format",     "Device information",
                                                     "Updates & recovery", "Return to stock", "Startup"};
    if (row == 7 && o.rgds_os) return "Startup";
    if (row == 7 && o.independent) return "Restart menu";
    return names[size_t(std::clamp(row, 0, 8))];
}

void App::feedback_arc(SDL_Renderer* r, int x, int y, int radius, double start, double amount, Color ink, int alpha,
                       int weight) {
    static const auto points = []() {
        std::array<std::pair<double, double>, 64> result{};
        for (size_t i = 0; i < result.size(); i++) {
            double a = (double(i) / 64 - .25) * 6.283185307;
            result[i] = {std::cos(a), std::sin(a)};
        }
        return result;
    }();
    s.SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    s.SetRenderDrawColor(r, ink.r, ink.g, ink.b, Uint8(std::clamp(alpha, 0, 255)));
    int first = int(start * 64), count = std::clamp(int(std::ceil(amount * 64)), 0, 64);
    for (int i = 0; i < count; i++) {
        auto a = points[(first + i) % 64], b = points[(first + i + 1) % 64];
        for (int w = 0; w < weight; w++)
            s.RenderDrawLine(r, x + int(std::round(a.first * (radius + w))),
                             y + int(std::round(a.second * (radius + w))), x + int(std::round(b.first * (radius + w))),
                             y + int(std::round(b.second * (radius + w))));
    }
}

bool App::options_hold() const {
    return pointer_down && !probe && !home_menu && settings.page == Page::Library && carousel.dragging &&
           !carousel.moved && !carousel.range && !model.games.empty() && pressed_game == model.selected &&
           carousel.tapped(down_x, model.games.size()) == pressed_game &&
           std::abs(carousel.position - pressed_game) < .08;
}

bool App::touch_animating(Uint32 now) const {
    return preferences.touch_indicator && preferences.animations &&
           ((pointer_down && (now - pointer_at < 100 || (options_hold() && now - pointer_at < 600))) ||
            (touch_tail && now - release_at < 140));
}

void App::touch_feedback(SDL_Renderer* r, int y) {
    gesture_cues(r, y);
    if (!preferences.touch_indicator || probe || launching || chat_open || (!pointer_down && !touch_tail)) return;
    auto now = s.GetTicks();
    double release = pointer_down ? 0 : std::min(1.0, (now - release_at) / 140.0);
    if (release >= 1) return;
    int radius = preferences.animations ? (pointer_down ? 18 + int(4 * std::max(0.0, 1.0 - (now - pointer_at) / 100.0))
                                                        : 18 + int(release * 6))
                                        : 18;
    int alpha = int(220 * (1 - release));
    SDL_Rect clip{0, y, 640, 480};
    s.RenderSetClipRect(r, &clip);
    feedback_arc(r, contact_x, y + contact_y, radius + 2, 0, 1, theme.at("panel"), alpha, 2);
    feedback_arc(r, contact_x, y + contact_y, radius, 0, 1, theme.at("accent"), alpha, 2);
    if (options_hold()) {
        double progress = std::min(1.0, (now - pointer_at) / 600.0);
        feedback_arc(r, contact_x, y + contact_y, radius + 5, 0,
                     preferences.animations ? progress
                     : progress >= 1        ? 1
                                            : 0,
                     theme.at("accent"), 255, 3);
        if (progress >= 1) centered(r, "Release for options", y + 361, 1, "accent");
    }
    s.RenderSetClipRect(r, nullptr);
}

double App::launch_opacity(Uint32 now) const {
    if (!preferences.animations) return launch_phase == LaunchPhase::Exiting ? 0 : 1;
    if (launch_phase == LaunchPhase::Exiting) {
        double t = std::min(1.0, (now - launch_exit_at) / 80.0);
        return launch_exit_opacity * (1 - t * t);
    }
    double t = std::min(1.0, (now - launch_at) / 110.0);
    return 1 - std::pow(1 - t, 3);
}

void App::loading_overlay(SDL_Renderer* r, int y, bool lower) {
    if (!launching) return;
    auto now = s.GetTicks();
    double opacity = launch_opacity(now);
    SDL_Rect panel{0, y, 640, 480};
    s.SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    auto background = theme.at("background");
    double wash = launch_phase == LaunchPhase::Exiting && launch_handoff ? launch_exit_opacity : opacity;
    s.SetRenderDrawColor(r, background.r, background.g, background.b, Uint8((lower ? 224 : 172) * wash));
    s.RenderFillRect(r, &panel);
    if (lower && opacity > 0) {
        // Eight small DS-style blocks: no card, labels, textures or artwork.
        static const std::array<std::pair<int, int>, 8> offsets = {
            {{0, -23}, {16, -16}, {23, 0}, {16, 16}, {0, 23}, {-16, 16}, {-23, 0}, {-16, -16}}};
        int head = preferences.animations ? int((now - launch_at) / 75) % 8 : 0;
        auto accent = theme.at("accent");
        for (int i = 0; i < 8; i++) {
            int age = (head - i + 8) % 8, alpha = preferences.animations ? 255 - age * 27 : 180;
            s.SetRenderDrawColor(r, accent.r, accent.g, accent.b, Uint8(alpha * opacity));
            SDL_Rect block{317 + offsets[i].first, y + 237 + offsets[i].second, 6, 6};
            s.RenderFillRect(r, &block);
        }
    }
    if (launch_phase == LaunchPhase::Exiting && launch_handoff) {
        // Clear the library into the theme's neutral colour for handoff.
        double t = preferences.animations ? std::min(1.0, (now - launch_exit_at) / 80.0) : 1;
        s.SetRenderDrawColor(r, background.r, background.g, background.b, Uint8(255 * t * t));
        s.RenderFillRect(r, &panel);
    }
}

void App::glossy_pill(SDL_Renderer* r, int x, int y, int w, int h, int radius, Color base, double press,
                     bool sheen) {
    radius = std::clamp(radius, 0, std::min(w, h) / 2);
    const Color white{255, 255, 255}, black{0, 0, 0};
    const int lum = (base.r * 3 + base.g * 6 + base.b) / 10;
    // Pale bodies (white panels) need a visible rim; saturated ones a deeper one.
    fill_round(r, x, y, w, h, radius, blend(base, black, lum > 200 ? 16 : 26));
    if (w <= 2 || h <= 2) return;
    fill_round(r, x + 1, y + 1, w - 2, h - 2, std::max(0, radius - 1), base);
    auto inset_at = [&](int row) {
        double dy = row < radius ? radius - row - .5 : row >= h - radius ? row - (h - radius) + .5 : -1;
        return dy < 0 ? 0 : radius - int(std::sqrt(std::max(0.0, radius * radius - dy * dy)));
    };
    const double lift = lum > 200 ? 0.0 : lum < 90 ? 10.0 : 30.0, deepen = lum > 200 ? 9.0 : 14.0;
    for (int row = 1; row < h - 1; row++) {
        int inset = inset_at(row) + 1;
        if (w - 2 * inset <= 0) continue;
        double t = double(row - 1) / std::max(1, h - 3);
        if (press > 0) t = t + (1 - 2 * t) * press * .8;
        const Color c = t < .5 ? blend(base, white, int(std::lround(lift * (1 - 2 * t))))
                               : blend(base, black, int(std::lround(deepen * (2 * t - 1))));
        tint_rect(r, x + inset, y + row, w - 2 * inset, 1, c);
    }
    int inset = inset_at(1) + 2;
    if (sheen && w - 2 * inset > 0 && h > 6)
        tint_rect(r, x + inset, y + 1, w - 2 * inset, 1, blend(base, white, int(55 * (1 - .7 * press))));
}

void App::raised_pill(SDL_Renderer* r, int x, int y, int w, int h, int radius, Color fill, double press, bool shadow,
                       bool sheen) {
    const int sink = int(std::lround(2 * press));
    if (shadow)
        alpha_round(r, x, y + 3, w, h, radius, shadow_colour(), int((preferences.dark ? 70 : 28) * (1 - .5 * press)));
    glossy_pill(r, x, y + sink, w, h, radius, fill, press, sheen);
}

void App::footer_button(SDL_Renderer* r, int x, int y, int w, const std::string& text, const std::string& badge,
                        bool primary, bool pressed, Color* tint) {
    const Color fill = primary ? (tint ? *tint : theme.at("accent")) : theme.at(pressed ? "focus" : "panel"),
                ink = primary ? (tint ? tile_ink(*tint) : theme.at("on_accent")) : theme.at("ink");
    raised_pill(r, x, y, w, 40, 12, fill, pressed ? 1 : 0);
    const int sink = pressed ? 2 : 0;
    int tx = x + 18;
    if (!badge.empty()) {
        if (primary) {
            fill_round(r, x + w - 36, y + 9 + sink, 22, 22, 11, ink);
            raw_label_colour(r, badge, x + w - 36 + (22 - label_width(badge, 14, false)) / 2,
                             centred_label_y(badge, 14, y + 20 + sink), 14, fill, 22);
        } else {
            round_rect(r, x + 13, y + 9 + sink, 22, 22, 11, "slot");
            raw_label(r, badge, x + 13 + (22 - label_width(badge, 14, false)) / 2,
                      centred_label_y(badge, 14, y + 20 + sink), 14, "ink", 22);
            tx = x + 48;
        }
    }
    const std::string value = locale.text(text);
    raw_label_colour(r, value, tx, centred_label_y(value, 20, y + 20 + sink), 20, ink,
                     w - (badge.empty() ? 36
                          : primary     ? 58
                                        : 60));
}

void App::blend_rect(SDL_Renderer* r, int x, int y, int w, int h, Color c, int alpha) {
    if (w <= 0 || h <= 0 || alpha <= 0) return;
    s.SetRenderDrawColor(r, c.r, c.g, c.b, Uint8(std::min(alpha, 255)));
    SDL_Rect b{x, y, w, h};
    s.RenderFillRect(r, &b);
}

void App::hotkey_pill(SDL_Renderer* r, int x, int y, int w, int h, const std::string& text, KeyLook look, bool pressed,
                      const std::string& key, bool translate) {
    const Color accent = theme.at("accent"),
                fill = look == KeyLook::White   ? Color{255, 255, 255}
                       : look == KeyLook::Panel ? theme.at(pressed ? "focus" : "panel")
                                                : accent,
                ink = look == KeyLook::Accent ? tile_ink(accent) : accent;
    raised_pill(r, x, y, w, h, h / 2, fill, pressed ? 1 : 0);
    const int sink = pressed ? 2 : 0, badge = h - 14;
    fill_round(r, x + 7, y + 7 + sink, badge, badge, badge / 2, ink);
    raw_label_colour(r, key, x + 7 + (badge - label_width(key, 16, false)) / 2,
                     centred_label_y(key, 16, y + h / 2 + sink), 16, fill, badge);
    const std::string value = translate ? locale.text(text) : text;
    const int room = w - badge - 30;
    int size = h >= 44 ? 20 : 18;
    while (size > 14 && label_width(value, size, false) > room)
        size -= 2; // long translations step down before they clip
    const int tx = x + badge + 14 + std::max(0, (room - label_width(value, size, false)) / 2);
    raw_label_colour(r, value, tx, centred_label_y(value, size, y + h / 2 + sink), size, ink, room);
}
