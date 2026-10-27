#pragma once
// Boot, memory-card repair and shutdown screens.
// Members of App in main.cpp; included once, right after the class.

void App::shutdown_overlay(SDL_Renderer* r, int y, bool upper) {
    if (!shutting_down()) return;
    const Uint32 now = s.GetTicks();
    const double t = preferences.animations ? std::min(1.0, (now - shutdown_at) / 420.0) : 1.0,
                 ease = 1 - (1 - t) * (1 - t);
    const Color bg = theme.at("background");
    s.SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    s.SetRenderDrawColor(r, bg.r, bg.g, bg.b, Uint8(std::lround(255 * ease)));
    SDL_Rect veil{0, y, 640, 480};
    s.RenderFillRect(r, &veil);
    s.SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    if (ease < .6) return;
    const int alpha = int(std::lround(255 * std::min(1.0, (ease - .6) / .4)));
    if (upper) {
        const int cx = 320, cy = y + 206;
        alpha_round(r, cx - 70, cy - 66, 140, 140, 34, shadow_colour(),
                    int((preferences.dark ? 60 : 18) * alpha / 255));
        round_rect(r, cx - 70, cy - 70, 140, 140, 34, "panel");
        brand(r, cx - 48, cy - 48, 96);
        const double spin = preferences.animations ? (now % 1400) / 1400.0 : 0;
        feedback_arc(r, cx, cy, 86, 0, 1, theme.at("slot"), alpha, 5);
        feedback_arc(r, cx, cy, 86, spin, .28, theme.at("accent"), alpha, 5);
        const std::string text = locale.text(shutdown_kind == "reboot" ? "Restarting…" : "Powering off…");
        raw_label(r, text, 320 - label_width(text, 26, false) / 2, y + 318, 26, "ink", 400);
        const std::string note = locale.text("Your saves are safe. See you soon!");
        raw_label(r, note, 320 - label_width(note, 16, false) / 2, y + 358, 16, "muted", 500);
    } else {
        const double spin = preferences.animations ? (now % 1400) / 1400.0 : 0;
        feedback_arc(r, 320, y + 240, 26, spin, .3, theme.at("accent"), alpha, 4);
    }
}

void App::brand(SDL_Renderer* r, int x, int y, int size) {
    if (!preferences.rich_icons &&
        draw_pixel_icon(s, r, SettingsIcon::Display, x + size / 2, y + size / 2, size, theme.at("accent")))
        return;
    if (!media.draw(r, asset_path(mark_asset(size)), {x, y, size, size}, false))
        ui_icon(r, SettingsIcon::Display, x + size / 2, y + size / 2, size, "accent");
}

bool App::boot_animating(Uint32) const {
    return o.boot_startup && preferences.animations && (settings.page == Page::BootWarning || !library_ready);
}

void App::boot_caption(SDL_Renderer* r, const std::string& value, int y, int pixels, const std::string& key) {
    const int width = std::min(576, label_width(value, pixels));
    label(r, value, 320 - width / 2, y, pixels, key, 576);
}

void App::boot_emblem(SDL_Renderer* r, int y) {
    // The identity tile settles in once (a short rise and fade), then
    // rests; a soft floor shadow breathes slowly while the library loads.
    const double t = preferences.animations ? std::min(1.0, (s.GetTicks() - boot_scene_at) / 420.0) : 1.0;
    const double ease = 1 - std::pow(1 - t, 3);
    const int lift = int(std::lround(6 * (1 - ease)));
    const double breath = preferences.animations ? (1 + std::cos(boot_phase() * 6.283185307)) * .5 : .5;
    // A soft floor shadow (always darker than the page, in both modes).
    const Color shade{0, 0, 0};
    for (int i = 0; i < 6; i++) {
        int half = 44 - i * 6 + int(4 * breath);
        tint_rect(r, 320 - half, y + 128 + i, half * 2, 1,
                  blend(theme.at("background"), shade, (preferences.dark ? 5 : 7) - i));
    }
    // Visible from frame one (the identity never pops in from nothing).
    if (!media.draw(r, asset_path("brand/mark-120.bmp"), {260, y + lift, 120, 120}, false,
                    static_cast<unsigned char>(std::lround(255 * (.55 + .45 * ease)))))
        ui_icon(r, SettingsIcon::Display, 320, y + 60, 96, "accent");
}

void App::boot_wordmark(SDL_Renderer* r, int y, int) {
    // Drawn at its rendered size (289 x 48): no scaling, crisp edges.
    if (!media.draw(r, asset_path(preferences.dark ? "brand/wordmark-dark.bmp" : "brand/wordmark-light.bmp"),
                    {176, y, 289, 48}, false))
        boot_caption(r, "RG DS Core", y + 8, 28, "ink");
}

void App::boot_scene(SDL_Renderer* r, int y) {
    rect(r, 0, y, 640, 480, "background");
    boot_emblem(r, y + 112);
    boot_wordmark(r, y + 268, 356);
    // Five soft round dots, a slow wave of light passing through them.
    for (int i = 0; i < 5; i++) {
        double wave = preferences.animations ? (1 + std::cos(boot_phase() * 6.283185307 - i * .65)) * .5 : .4;
        const Color c = blend(theme.at("background"), theme.at("accent"), int(22 + 68 * wave));
        const int cx = 288 + i * 16, cy = y + 368;
        for (int dy = -4; dy <= 4; dy++) {
            int half = int(std::sqrt(std::max(0.0, 16.5 - dy * dy)));
            tint_rect(r, cx - half, cy + dy, half * 2 + 1, 1, c);
        }
    }
    boot_caption(r, "Preparing your library…", y + 403, 16, "muted");
}

void App::boot_top(SDL_Renderer* r, int y) {
    rect(r, 0, y, 640, 480, "background");
    // Small, deliberate safety mark and an open text layout, like the
    // original handheld welcome screen rather than a settings dialog.
    Color amber{239, 178, 65};
    s.SetRenderDrawColor(r, amber.r, amber.g, amber.b, 255);
    for (int row = 0; row < 30; row++) {
        int half = row * 18 / 30;
        s.RenderDrawLine(r, 320 - half, y + 88 + row, 320 + half, y + 88 + row);
    }
    rect(r, 319, y + 98, 3, 10, "background");
    rect(r, 319, y + 111, 3, 3, "background");
    boot_caption(r, "Before you play", y + 150, 28, "ink");
    rect(r, 288, y + 207, 64, 1, "line");
    boot_caption(r, "Play at a comfortable volume.", y + 241, 20, "ink");
    boot_caption(r, "Give your eyes and hands a break.", y + 277, 20, "ink");
    boot_caption(r, "Pause when you need to.", y + 313, 20, "ink");
    boot_caption(r, "RG DS Core", y + 415, 16, "muted");
}

void App::boot_stylus(SDL_Renderer* r, int x, int y) {
    double phase = boot_phase();
    int lift = preferences.animations ? int(3 * (1 - std::cos(phase * 6.283185307))) : 0;
    if (preferences.animations) {
        feedback_arc(r, x, y + 10, 9 + int(phase * 11), 0, 1, theme.at("accent"), int(100 * (1 - phase)), 1);
    } else feedback_arc(r, x, y + 10, 12, 0, 1, theme.at("line"), 255, 1);
    // Closed pen silhouette, a pointed nib and an inset highlight.
    color(r, "ink");
    for (int i = 0; i < 7; i++) s.RenderDrawLine(r, x + 4 + i, y + 3 - lift, x + 29 + i, y - 22 - lift);
    for (int i = 0; i < 6; i++) s.RenderDrawLine(r, x + i, y + 9 - i - lift, x + 4 + i, y + 3 - lift);
    color(r, "background");
    s.RenderDrawLine(r, x + 9, y - lift, x + 31, y - 22 - lift);
}

void App::boot_bottom(SDL_Renderer* r, int y) {
    rect(r, 0, y, 640, 480, "background");
    boot_emblem(r, y + 70);
    boot_wordmark(r, y + 222, 332);
    boot_stylus(r, 309, y + 309);
    boot_caption(r, "Touch to continue", y + 344, 28, "ink");
    key_hints(r, 320, y + 428, {{"A Start", ""}}, 0);
}
