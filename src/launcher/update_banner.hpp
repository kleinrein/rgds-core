#pragma once
// The Home update banner and card, the Y hotkey, and the glitter bar and sparkle
// effects that decorate them.
// Members of App in main.cpp; included once, right after the class.

App::UpdateStage App::update_stage() {
    if (o.game_menu) return UpdateStage::None;
    const std::string state = update_status["state"];
    if (update_status["prepared"] == "1") return state == "restarting" ? UpdateStage::None : UpdateStage::Ready;
    const bool transfer = state == "downloading" || state == "verifying" || state == "staging";
    if (transfer && worker > 0 && worker_action.rfind("device.update-", 0) == 0) return UpdateStage::Running;
    if (state == "available") return UpdateStage::Offered;
    if (update_status["offer"].empty()) return UpdateStage::None;
    if (transfer || state == "cancelled" || state == "error" || state == "checking")
        return std::atoll(update_status["received"].c_str()) > 0 ? UpdateStage::Paused : UpdateStage::Offered;
    return UpdateStage::None;
}

void App::sparkle(SDL_Renderer* r, int cx, int cy, int size, Color ink, int alpha) {
    if (size < 1 || alpha <= 0) return;
    for (int d = 1; d <= size; d++) {
        const int a = alpha * (size + 1 - d) / (size + 1);
        blend_rect(r, cx - d, cy, 1, 1, ink, a);
        blend_rect(r, cx + d, cy, 1, 1, ink, a);
        blend_rect(r, cx, cy - d, 1, 1, ink, a);
        blend_rect(r, cx, cy + d, 1, 1, ink, a);
        if (d <= size / 2) {
            const int side = a / 3;
            blend_rect(r, cx - d, cy - 1, 1, 1, ink, side);
            blend_rect(r, cx - d, cy + 1, 1, 1, ink, side);
            blend_rect(r, cx + d, cy - 1, 1, 1, ink, side);
            blend_rect(r, cx + d, cy + 1, 1, 1, ink, side);
            blend_rect(r, cx - 1, cy - d, 1, 1, ink, side);
            blend_rect(r, cx + 1, cy - d, 1, 1, ink, side);
            blend_rect(r, cx - 1, cy + d, 1, 1, ink, side);
            blend_rect(r, cx + 1, cy + d, 1, 1, ink, side);
        }
    }
    for (int d = 1; d <= size / 3; d++) {
        const int a = alpha * (size / 3 + 1 - d) / (size / 3 + 2);
        blend_rect(r, cx - d, cy - d, 1, 1, ink, a);
        blend_rect(r, cx + d, cy - d, 1, 1, ink, a);
        blend_rect(r, cx - d, cy + d, 1, 1, ink, a);
        blend_rect(r, cx + d, cy + d, 1, 1, ink, a);
    }
    blend_rect(r, cx - 1, cy - 1, 3, 3, ink, alpha * 3 / 4);
    blend_rect(r, cx, cy, 1, 1, Color{255, 255, 255}, alpha);
}

void App::glints(SDL_Renderer* r, SDL_Rect box, int count, uint32_t seed, Uint32 now, int largest, bool moving) {
    const Color warm{255, 246, 214};
    for (int i = 0; i < count; i++) {
        const uint32_t period = 700 + (uint32_t(i) * 173) % 480, clock = now + uint32_t(i) * 389;
        const double life = moving ? double(clock % period) / period : (i % 2 ? 0 : .5);
        if (life <= 0) continue;
        const uint32_t h = glint_hash(seed * 977u + uint32_t(i) * 7919u + (moving ? clock / period : 0u) * 104729u);
        const double grow = std::sin(life * 3.14159265);
        sparkle(r, box.x + int(h % uint32_t(std::max(1, box.w))), box.y + int((h >> 12) % uint32_t(std::max(1, box.h))),
                1 + int(std::lround(largest * grow)), warm, int(255 * grow));
    }
}

void App::light_sweep(SDL_Renderer* r, int x, int y, int h, int radius, int right_edge, double t, int strength) {
    const int centre = x - 80 + int(t * (right_edge - x + 160));
    for (int row = 0; row < h; row++) {
        const int in = round_inset(row, h, radius), left = x + in, right = right_edge - in,
                  cx = centre - (row - h / 2) / 2;
        for (auto [half, alpha] :
             {std::pair{34, strength * 2 / 5}, std::pair{18, strength * 3 / 5}, std::pair{6, strength}}) {
            const int a = std::max(left, cx - half), b = std::min(right, cx + half);
            blend_rect(r, a, y + row, b - a, 1, Color{255, 255, 255}, alpha);
        }
    }
}

void App::glitter_bar(SDL_Renderer* r, int x, int y, int w, int h, double fraction, Uint32 now, bool moving) {
    const bool dark = preferences.dark;
    const Color accent = theme.at("accent"), white{255, 255, 255}, black{0, 0, 0},
                floor = blend(theme.at("background"), black, dark ? 18 : 5);
    alpha_round(r, x - 1, y + 1, w + 2, h + 2, (h + 2) / 2, white,
                dark ? 14 : 200); // light on the groove's lower lip
    round_rect(r, x, y, w, h, h / 2, "line");
    fill_round(r, x + 1, y + 1, w - 2, h - 2, (h - 2) / 2, floor);
    alpha_round(r, x + 5, y + 2, w - 10, 4, 2, shadow_colour(), dark ? 70 : 24);
    for (int i = 1; i < 10; i++)
        fill_round(r, x + w * i / 10 - 1, y + h / 2 - 2, 3, 5, 1, blend(floor, theme.at("line"), dark ? 60 : 85));
    const int pad = 3, fx = x + pad, fy = y + pad, fh = h - 2 * pad, full = w - 2 * pad, radius = fh / 2;
    if (fraction > 0) {
        const int fw = std::clamp(int(std::lround(full * std::min(1.0, fraction))), fh, full);
        for (int row = 0; row < fh; row++) {
            const int in = round_inset(row, fh, radius);
            if (fw - 2 * in <= 0) continue;
            tint_rect(r, fx + in, fy + row, fw - 2 * in, 1,
                      row < fh / 2 ? blend(accent, white, std::max(0, 34 - row * 56 / fh))
                                   : blend(accent, black, (row - fh / 2) * 30 / fh));
        }
        s.SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        const int period = 22, stripe = 10, phase = moving ? int((now / 30) % period) : 0;
        for (int row = 0; row < fh; row++) {
            const int in = round_inset(row, fh, radius), left = fx + in, right = fx + fw - in;
            for (int sx = left - ((left + row - phase) % period + period) % period; sx < right; sx += period) {
                const int a = std::max(sx, left), b = std::min(sx + stripe, right);
                blend_rect(r, a, fy + row, b - a, 1, white, dark ? 20 : 28);
            }
        }
        const int band = std::max(3, fh * 9 / 20);
        for (int row = 1; row < band; row++) {
            const int in = round_inset(row, fh, radius) + 2;
            blend_rect(r, fx + in, fy + row, fw - 2 * in, 1, white, int(105 * (1.0 - double(row) / band)));
        }
        {
            const int in = round_inset(1, fh, radius) + 3;
            blend_rect(r, fx + in, fy + 1, fw - 2 * in, 1, white, 170);
        }
        {
            const int in = round_inset(fh - 2, fh, radius) + 3;
            blend_rect(r, fx + in, fy + fh - 3, fw - 2 * in, 1, white, 45);
            blend_rect(r, fx + in, fy + fh - 1, fw - 2 * in, 1, black, 40);
        }
        if (moving) light_sweep(r, fx, fy, fh, radius, fx + fw, double(now % 1600) / 1600, 60);
        if (fw < full) {
            alpha_round(r, fx + fw - 12, fy + 2, 10, fh - 4, std::min(5, (fh - 4) / 2), white, 90);
            s.SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
            glints(r, {fx + fw - 30, fy - 6, 38, fh + 12}, 5, 1, now, 6, moving);
            glints(r, {fx + 8, fy + 4, std::max(1, fw - 40), fh - 8}, 3, 2, now, 3, moving);
        } else glints(r, {fx + 6, fy - 5, fw - 12, fh + 10}, 9, 3, now, 6, moving);
    }
    s.SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
}

void App::update_glyph(SDL_Renderer* r, int cx, int cy, int size, Color ink, bool ready) {
    const double f = size / 42.0;
    auto v = [&](double n) { return int(std::lround(n * f)); };
    if (ready) {
        const int weight = std::max(1, v(1.6));
        tint_colour(r, ink);
        for (int t = -weight; t <= weight; t++) {
            s.RenderDrawLine(r, cx - v(10), cy + t, cx - v(3), cy + v(7) + t);
            s.RenderDrawLine(r, cx - v(3), cy + v(7) + t, cx + v(10), cy - v(7) + t);
        }
        return;
    }
    const int shaft = std::max(2, v(6)), head = std::max(2, v(8));
    tint_rect(r, cx - shaft / 2, cy - v(12), shaft, v(11), ink);
    for (int i = 0; i < head; i++) {
        const int half = head - i;
        tint_rect(r, cx - half, cy - v(2) + i, 2 * half + 1, 1, ink);
    }
    const int tray = std::max(1, v(3));
    tint_rect(r, cx - v(12), cy + v(10) - tray, v(24), tray, ink);
    tint_rect(r, cx - v(12), cy + v(5), tray, v(5), ink);
    tint_rect(r, cx + v(12) - tray, cy + v(5), tray, v(5), ink);
}

void App::update_disc(SDL_Renderer* r, int cx, int cy, int size, UpdateStage stage) {
    const Color accent = theme.at("accent"), white{255, 255, 255};
    alpha_round(r, cx - size / 2, cy - size / 2 + 3, size, size, size / 2, Color{0, 0, 0}, preferences.dark ? 70 : 36);
    glossy_pill(r, cx - size / 2, cy - size / 2, size, size, size / 2, white);
    if (stage == UpdateStage::Running || stage == UpdateStage::Paused) {
        feedback_arc(r, cx, cy, size / 2 + 2, 0, 1, blend(accent, Color{0, 0, 0}, 30), 255, 3);
        feedback_arc(r, cx, cy, size / 2 + 2, 0, std::max(.02, update_shown), white, 255, 3);
    }
    update_glyph(r, cx, cy, size, accent, stage == UpdateStage::Ready);
    s.SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
}

std::string App::update_action_name(UpdateStage stage) {
    return stage == UpdateStage::Ready     ? "Restart & install"
           : stage == UpdateStage::Paused  ? "Resume download"
           : stage == UpdateStage::Running ? "Open"
                                           : "Update now";
}

void App::update_banner(SDL_Renderer* r, int y) {
    const auto stage = update_stage();
    if (stage == UpdateStage::None) return;
    const Uint32 now = s.GetTicks();
    const int frame = banner_frame(now);
    drawn_banner_frame = frame;
    const int x = 24, w = 592, h = 62, radius = 18;
    const Color accent = theme.at("accent"), ink = tile_ink(accent), black{0, 0, 0};
    alpha_round(r, x, y + 4, w, h, radius, shadow_colour(), preferences.dark ? 80 : 42);
    fill_round(r, x - 1, y - 1, w + 2, h + 2, radius + 1, blend(accent, black, 28));
    glossy_pill(r, x, y, w, h, radius, accent);
    if (frame >= 0) {
        s.SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        light_sweep(r, x + 2, y + 2, h - 4, radius - 2, x + w - 2, double(now % banner_cycle) / banner_sweep, 46);
    }
    update_disc(r, x + 36, y + h / 2, 42, stage);
    s.SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    glints(r, {x + 8, y + 2, 22, 12}, 2, 7, now, 4, frame >= 0);
    glints(r, {x + w - 210, y + 6, 40, 12}, 2, 9, now, 4, frame >= 0);
    s.SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    // The key says one short verb; it grows to fit a longer translation.
    const std::string verb = stage == UpdateStage::Ready     ? "Install"
                             : stage == UpdateStage::Paused  ? "Resume"
                             : stage == UpdateStage::Running ? "Open"
                                                             : "Update";
    const int key = std::clamp(label_width(verb, 18) + 66, 136, 210), text_width = w - 74 - key - 24;
    label(r, "System update", x + 70, y + 9, 14, "on_accent", text_width);
    const std::string version = update_version();
    const std::string line =
        stage == UpdateStage::Ready ? locale.text("Version " + version + " is ready to install")
        : stage == UpdateStage::Running
            ? (update_status["state"] == "downloading"
                   ? locale.text("Downloading " + version)
                   : locale.text(update_status["state"] == "verifying" ? "Verifying update…" : "Preparing update…")) +
                  "  ·  " + std::to_string(int(update_shown * 100)) + "%"
        : stage == UpdateStage::Paused ? locale.text("Paused at " + std::to_string(int(update_shown * 100)) + "%")
                                       : locale.text("Version " + version + " is ready");
    raw_label_colour(r, line, x + 70, y + 29, 20, ink, text_width);
    hotkey_pill(r, x + w - key - 12, y + 11, key, 40, verb, KeyLook::White);
}

void App::update_card(SDL_Renderer* r, int y) {
    const auto stage = update_stage();
    const Uint32 now = s.GetTicks();
    const bool moving = preferences.animations;
    const Color accent = theme.at("accent"), ink = tile_ink(accent), black{0, 0, 0};
    const int x = 24, w = 592, top = y + 60, height = 398, header = 86;
    settings_card(r, x, top, w, height);
    glossy_pill(r, x, top, w, header + 16, 14, accent);
    rect(r, x, top + header, w, 17, "panel");
    tint_rect(r, x, top + header, w, 1, blend(accent, black, 30));
    tint_rect(r, x, top + header + 1, w, 1, blend(theme.at("panel"), black, 6));
    update_disc(r, x + 48, top + header / 2, 52, stage);
    s.SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    glints(r, {x + 300, top + 8, 130, 68}, 5, 3, now, 5, moving);
    glints(r, {x + 12, top + 6, 22, 18}, 2, 4, now, 4, moving);
    s.SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    label(r, "System update", x + 92, top + 16, 14, "on_accent", 300);
    raw_label_colour(r, "RG DS Core " + update_version(), x + 92, top + 38, 26, ink, 330);
    const double size = std::atof(update_status["size"].c_str());
    if (size > 0) {
        const std::string chip = megabytes(size) + " MB";
        const int cw = label_width(chip, 16, false) + 28, cx = x + w - 24 - cw;
        alpha_round(r, cx, top + header / 2 - 15, cw, 30, 15, Color{255, 255, 255}, preferences.dark ? 40 : 60);
        raw_label_colour(r, chip, cx + 14, centred_label_y(chip, 16, top + header / 2), 16, ink, cw);
    }
    const int body = top + header;
    if (stage == UpdateStage::Offered) {
        std::string message = update_status["message"];
        if (update_status["state"] != "error") message = "A new version is ready to download.";
        if (!service_notice.empty()) message = service_notice;
        wrapped(r, message, x + 28, body + 18, 20, w - 56, 2);
        label(r, "What's new", x + 28, body + 80, 14, "muted", w - 56);
        sunken_track(r, x + 20, body + 102, w - 40, 126, 12);
        const std::string notes = update_status["notes"];
        if (notes.empty()) label(r, "No release notes yet.", x + 38, body + 116, 16, "muted", w - 76);
        else raw_wrapped(r, notes, x + 38, body + 114, 16, w - 76, 4);
        hotkey_pill(r, x + (w - 300) / 2, body + 246, 300, 48, update_action_name(stage), KeyLook::Accent);
        return;
    }
    // The four steps, joined by the DSi's dotted track.
    const std::string state = update_status["state"];
    const int current = stage == UpdateStage::Ready ? 3 : state == "staging" ? 2 : state == "verifying" ? 1 : 0;
    const std::array<const char*, 4> steps = {"Download", "Verify", "Prepare", "Install"};
    const int first = x + 86, span = (w - 172) / 3, dot = 24, row = body + 30;
    for (int i = 0; i < 3; i++)
        for (int dx = first + i * span + dot / 2 + 6; dx < first + (i + 1) * span - dot / 2 - 6; dx += 8)
            fill_round(r, dx, row - 2, 4, 4, 2, blend(theme.at("line"), accent, i < current ? 100 : 0));
    for (int i = 0; i < 4; i++) {
        const int cx = first + i * span;
        const bool done = i < current, lit = i == current;
        if (lit && moving && stage == UpdateStage::Running) {
            const double pulse = .5 + .5 * std::sin(now / 260.0);
            alpha_round(r, cx - dot / 2 - 5, row - dot / 2 - 5, dot + 10, dot + 10, dot / 2 + 5, accent,
                        int(40 + 50 * pulse));
        }
        if (done || lit) {
            alpha_round(r, cx - dot / 2, row - dot / 2 + 2, dot, dot, dot / 2, shadow_colour(),
                        preferences.dark ? 60 : 30);
            glossy_pill(r, cx - dot / 2, row - dot / 2, dot, dot, dot / 2, accent);
        } else {
            round_rect(r, cx - dot / 2, row - dot / 2, dot, dot, dot / 2, "line");
            round_rect(r, cx - dot / 2 + 1, row - dot / 2 + 1, dot - 2, dot - 2, dot / 2 - 1, "slot");
        }
        if (done) update_glyph(r, cx, row, dot, ink, true);
        else if (lit) fill_round(r, cx - 4, row - 4, 8, 8, 4, ink);
        const std::string name = locale.text(steps[size_t(i)]);
        raw_label(r, name, cx - label_width(name, 14, false) / 2, row + 18, 14, lit ? "ink" : "muted", span);
    }
    s.SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    const std::string percent = std::to_string(int(update_shown * 100)) + "%";
    raw_label(r, percent, x + 28, body + 74, 40, "ink", 200);
    const double received = std::atof(update_status["received"].c_str());
    const std::string amount = current > 0 ? locale.text("Step " + std::to_string(current + 1) + " of 4")
                               : size > 0  ? megabytes(std::min(received, size)) + " / " + megabytes(size) + " MB"
                                           : "";
    if (!amount.empty()) raw_label(r, amount, x + w - 28 - label_width(amount, 18, false), body + 92, 18, "muted", 300);
    glitter_bar(r, x + 24, body + 128, w - 48, 34, update_shown, now, moving && stage != UpdateStage::Paused);
    std::string message = update_status["message"];
    if (stage == UpdateStage::Paused && state != "error") message = "Download paused. Retry to continue.";
    if (!service_notice.empty() && stage != UpdateStage::Running) message = service_notice;
    const std::string speed = update_speed();
    const int speed_width = speed.empty() ? 0 : label_width(speed, 16, false);
    wrapped(r, message, x + 28, body + 176, 18, w - 56 - (speed_width ? speed_width + 16 : 0), 2);
    if (!speed.empty()) raw_label(r, speed, x + w - 28 - speed_width, body + 178, 16, "muted", speed_width + 2);
    if (stage == UpdateStage::Running) {
        // Reassurance where the key would be: what happens if the link drops, or that saves are safe.
        const std::string tip =
            locale.text(state == "downloading" ? "Downloads resume where they stopped if Wi-Fi drops."
                                               : "Your games and saves are not touched.");
        const int tw = std::min(w - 96, label_width(tip, 16, false)), tx = x + (w - tw - 30) / 2;
        ui_icon(r, SettingsIcon::Info, tx + 10, body + 270, 20, "accent");
        raw_label(r, tip, tx + 30, centred_label_y(tip, 16, body + 270), 16, "muted", tw);
    } else hotkey_pill(r, x + (w - 300) / 2, body + 246, 300, 48, update_action_name(stage), KeyLook::Accent);
}

bool App::update_now() {
    const auto stage = update_stage();
    if (stage == UpdateStage::None) return false;
    if (settings.page != Page::Maintenance) {
        updates_return = settings.page == Page::Library ? Page::Library : Page::Settings;
        view_route = ViewTravel::Forward;
        settings.section = 9;
        settings.page = Page::Maintenance;
        settings.focus = 1;
        sound_intent(MenuCue::Open);
    }
    if (stage == UpdateStage::Ready) confirm("update-apply", Page::Maintenance);
    else if (stage == UpdateStage::Offered || stage == UpdateStage::Paused) {
        start_worker("device.update-download");
        // Another task (an artwork scrape, a Wi-Fi scan) holds the worker: say so rather than wait silently.
        if (worker_action != "device.update-download") {
            service_notice = "Finishing another task. Press Y again in a moment.";
            sound_intent(MenuCue::Error);
        } else sound_intent(MenuCue::Confirm);
    }
    dirty = true;
    return true;
}

void App::open_updates(Page from) {
    updates_return = from;
    if (from != Page::System) settings.section = 9;
    settings.page = Page::Maintenance;
    // Land on the row that acts on an update when there is one.
    settings.focus = update_stage() == UpdateStage::None ? 0 : 1;
    start_worker("device.update-info");
}

void App::update_status_chip(SDL_Renderer* r, int y, int limit) {
    const auto stage = update_stage();
    if (stage == UpdateStage::None) return;
    const int x = artwork_busy() ? 248 : 226, cy = y + 24;
    if (limit - x < 24) return;
    const Color accent = theme.at("accent");
    alpha_round(r, x, cy - 9, 22, 22, 11, shadow_colour(), preferences.dark ? 60 : 26);
    glossy_pill(r, x, cy - 11, 22, 22, 11, accent);
    update_glyph(r, x + 11, cy, 22, tile_ink(accent), stage == UpdateStage::Ready);
    s.SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    if (stage != UpdateStage::Running && stage != UpdateStage::Paused) return;
    const std::string percent = std::to_string(int(update_shown * 100)) + "%";
    const int pw = label_width(percent, 16, false), tx = x + 30, track = std::min(64, limit - tx - pw - 8);
    if (track < 24) return;
    sunken_track(r, tx, cy - 6, track, 12, 6);
    const int fill = std::clamp(int(std::lround((track - 4) * update_shown)), 0, track - 4);
    if (fill >= 8) glossy_pill(r, tx + 2, cy - 4, fill, 8, 4, accent);
    raw_label(r, percent, tx + track + 6, status_label_y(percent, 16, y), 16, "muted", pw + 2);
}

std::string App::update_badge() {
    const auto stage = update_stage();
    if (stage == UpdateStage::Running) return std::to_string(int(update_shown * 100)) + "%";
    return stage == UpdateStage::None ? std::string() : update_version();
}
