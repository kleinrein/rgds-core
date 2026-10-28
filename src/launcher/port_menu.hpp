#pragma once
// PortMaster ports (docs/PORTMASTER_PLAN.md): rgds-launcher --port-menu, the menu
// helper that stays up beside a port. It maps its windows before the port starts,
// so the port is the newest window and keeps keyboard focus on the stock Weston,
// which never hands focus back once a window closes. The port covers the top
// window; the bottom one is the companion until runtime/port_runner.py writes
// session/port-menu.state = menu, and then it is the game menu.
// Members of App in main.cpp; included once, right after the class.

void App::port_poll(Uint32 now) {
    if (now - port_poll_at < 50) return;
    port_poll_at = now;
    auto state = fields(o.data / "session/port-menu.state");
    if (!state["token"].empty() && state["token"] != port_state_token) {
        port_state_token = state["token"];
        const bool idle = state["state"] != "menu";
        if (!idle && port_idle) {
            port_idle = false;
            if (port_touch_phase == 2) port_pointer.button(0, false); // a drag the menu cut short
            port_release_click();
            port_touch_phase = 0;
            settings.page = Page::QuickMenu;
            settings.focus = port_manager() ? 1 : 0; // Home in PortMaster most often means done
            status.clear();
            menu_pending_token.clear();
            // The Anbernic button (or the info screen's Game switcher) opens on the switcher.
            const auto runner = fields(o.data / "session/game-menu.status");
            if (runner.count("view") && runner.at("view") == "switcher" && !port_manager()) {
                load_switcher();
                settings.page = Page::GameSwitcher;
                settings.focus = 0;
            }
            menu_status["view_request"] = runner.count("view_request") ? runner.at("view_request") : "";
            menu_status["state"] = "paused";
            menu_status["message"].clear();
            emit_sound(MenuCue::Open);
            dirty = true;
        } else if (idle && !port_idle) port_go_idle();
    }
    if (!port_idle) return;
    auto progress = fields(o.data / "session/port-status.ini");
    if (progress != port_status) {
        port_status = std::move(progress);
        dirty = true;
    }
    auto volume = fields(o.data / "session/volume-hud.ini");
    if (!volume["token"].empty() && volume["token"] != port_volume_token) {
        port_volume_token = volume["token"];
        port_volume = std::atoi(volume["volume"].c_str());
        port_volume_at = now;
        dirty = true;
    }
    if (port_volume_at && now - port_volume_at > 1600) {
        port_volume_at = 0;
        dirty = true;
    }
}

void App::port_go_idle() {
    port_idle = true;
    port_held = PortItem::None;
    port_dragging = false;
    port_touched_at = s.GetTicks();
    settings.page = Page::QuickMenu;
    settings.focus = 0;
    menu_pending_token.clear();
    status.clear();
    dirty = true;
}

// The bottom screen while a port plays: the info screen, or for PortMaster
// itself what is running, the keys that matter, its install progress and the
// volume when it changes.
void App::port_companion(SDL_Renderer* r, int y) {
    if (port_info_shown()) {
        port_info(r, y);
        return;
    }
    const Game& game = model.games[0];
    settings_card(r, 28, y + 28, 584, 116);
    game_icon(r, game, 50, y + 46, 80);
    if (port_manager()) {
        raw_label(r, "PortMaster", 150, y + 52, 28, "ink", 440);
        label(r, "Get, update and remove ports", 150, y + 92, 16, "muted", 440);
    } else {
        label(r, "Ports", 150, y + 52, 16, "muted", 440);
        raw_label(r, game.title, 150, y + 76, 28, "ink", 440);
    }
    const std::string message = port_status.count("message") ? port_status.at("message") : "";
    if (!message.empty()) {
        // PortMaster setting itself up (runtime/portmaster.py): the step, and a bar while it downloads.
        settings_card(r, 28, y + 168, 584, 132);
        wrapped(r, message, 52, y + 188, 20, 536, 2);
        const int percent = port_status.count("percent") ? std::atoi(port_status.at("percent").c_str()) : -1;
        if (percent >= 0) {
            round_rect(r, 52, y + 256, 536, 16, 8, "slot");
            const int fill = std::clamp(percent, 0, 100) * 536 / 100;
            if (fill > 0) fill_round(r, 52, y + 256, std::max(fill, 16), 16, 8, theme.at("accent"));
        }
        if (!port_status["detail"].empty()) raw_wrapped(r, port_status["detail"], 52, y + 278, 14, 536, 1);
    } else if (port_volume_at) {
        settings_card(r, 188, y + 196, 264, 76);
        ui_icon(r, SettingsIcon::Volume, 228, y + 234, 28, "accent");
        label(r, "Volume", 256, y + 210, 16, "muted", 120);
        raw_label(r, std::to_string(std::clamp(port_volume, 0, 100)) + "%", 256, y + 230, 24, "ink", 160);
    }
    if (port_manager()) key_hints(r, 320, y + 430, {{"Home", "Close PortMaster"}}, 0);
    else key_hints(r, 320, y + 430, {{"Home", "Game menu"}, {"Select Start", "Quit"}}, 0);
}

// The info screen's places (640 x 480), as RG DS Play lays them out (Dock::box).
SDL_Rect App::port_box(PortItem item) {
    switch (item) {
    case PortItem::Switcher: return {24, 140, 290, 112};
    case PortItem::Home: return {326, 140, 290, 112};
    case PortItem::Bright: return {24, 264, 592, 88};
    case PortItem::Touch: return {24, 364, 290, 72};
    case PortItem::Off: return {326, 364, 290, 72};
    default: return {};
    }
}

// Left alone for a while, the screen fades down; a touch brings it back.
int App::port_shown_brightness(Uint32 now) const {
    const int chosen = std::clamp(port_bright, 10, 100), rest = std::max(10, chosen * 35 / 100);
    const Uint32 idle = now - port_touched_at;
    if (idle < 15000) return chosen;
    const double t = preferences.animations ? std::min(1.0, (idle - 15000) / 1500.0) : 1.0;
    return int(std::lround(chosen + (rest - chosen) * t));
}

// The runner's triggers beside the control file: ".menu" opens the game switcher, ".home" returns home.
void App::port_trigger(const char* name) {
    std::ofstream(o.data / ("session/emulator.control" + std::string(name))) << s.GetTicks() << '\n';
}

void App::port_info_tick(Uint32 now) {
    if (port_bright < 0) {
        auto kept = fields(o.data / "emulators/rgds-play/second-screen.ini");
        port_bright = kept["brightness"].empty() ? 100 : std::clamp(std::atoi(kept["brightness"].c_str()), 10, 100);
        port_began = port_touched_at = now;
        if (!model.games.empty() && fields(folder_for_game(model.games[0]) / "touch.ini")["touch"] == "1")
            port_set_touch_mode(true, false);
    }
    if (!port_info_shown()) return;
    if (port_touch_mode) {
        port_touch_tick(now);
        return;
    }
    if (port_held == PortItem::Home && !port_fired && now - port_held_at >= 1000) {
        port_fired = true;
        port_trigger(".home");
    }
    if (port_bright_at && now - port_bright_at > 800) {
        port_bright_at = 0;
        const auto path = o.data / "emulators/rgds-play/second-screen.ini";
        const auto pending = fs::path(path.string() + ".pending");
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        std::ofstream(pending) << "brightness=" << port_bright << '\n';
        fs::rename(pending, path, ec);
    }
    // Drawn again while something moves by itself, and when the clock, the minutes played or the charge turn.
    const Uint32 idle = now - port_touched_at;
    const bool moving = (port_held == PortItem::Home && !port_fired) || port_dragging ||
                        (!port_note.empty() && now - port_note_at < 1900) || (idle >= 15000 && idle < 16600);
    std::time_t wall = std::time(nullptr);
    const std::tm local = *std::localtime(&wall);
    const uint64_t key = uint64_t(local.tm_hour * 60 + local.tm_min) | uint64_t((now - port_began) / 60000) << 12 |
                         uint64_t(hardware.battery + 2) << 32 | uint64_t(port_off) << 40;
    if (moving || key != port_info_key) {
        port_info_key = key;
        dirty = true;
    }
}

void App::port_touch(const TouchPoint& point, Uint32 now) {
    auto inside = [&](PortItem item) {
        const SDL_Rect b = port_box(item);
        return point.x >= b.x && point.x < b.x + b.w && point.y >= b.y && point.y < b.y + b.h;
    };
    dirty = true;
    if (port_touch_mode) {
        port_touch_pointer(point, now);
        return;
    }
    if (port_off) {
        // A touch on the black screen brings it back, and does nothing else.
        if (point.kind == 0) {
            port_off = false;
            port_touched_at = now;
        }
        return;
    }
    const bool faded = now - port_touched_at >= 15000;
    if (point.kind == 0 || point.kind == 2) port_touched_at = now;
    if (point.kind == 3) {
        port_held = PortItem::None;
        port_dragging = false;
        return;
    }
    if (point.kind == 0) {
        if (faded) return; // the first touch on a dimmed screen only wakes it
        for (PortItem item : {PortItem::Switcher, PortItem::Home, PortItem::Touch, PortItem::Off})
            if (inside(item)) {
                port_held = item;
                port_held_at = now;
                port_fired = false;
                return;
            }
        port_dragging = inside(PortItem::Bright);
    }
    if (port_dragging && (point.kind == 0 || point.kind == 1)) {
        const SDL_Rect b = port_box(PortItem::Bright);
        const int left = b.x + 80, right = b.x + b.w - 92;
        const int level = 10 + int(std::lround(90.0 * std::clamp(double(point.x - left) / (right - left), 0.0, 1.0)));
        if (level != port_bright) {
            port_bright = level;
            port_bright_at = now | 1;
        }
        port_touched_at = now;
        return;
    }
    if (port_held != PortItem::None && point.kind == 1 && !inside(port_held)) port_held = PortItem::None; // slid off
    if (point.kind == 2) {
        if (port_held == PortItem::Switcher && inside(PortItem::Switcher)) port_trigger(".menu");
        else if (port_held == PortItem::Home && !port_fired) {
            port_note = "Hold to return home"; // a tap is not enough
            port_note_at = now;
        } else if (port_held == PortItem::Off && inside(PortItem::Off)) port_off = true;
        else if (port_held == PortItem::Touch && inside(PortItem::Touch)) port_set_touch_mode(true);
        port_held = PortItem::None;
        port_dragging = false;
    }
}

// Touch screen mode: the bottom panel becomes an absolute mouse over the top one. Kept per port.
void App::port_set_touch_mode(bool on, bool remember) {
    if (on && !port_pointer.open(640, 480) && !o.preview) { // the host preview draws the pad without uinput
        port_note = "Touch screen is not available";
        port_note_at = s.GetTicks();
        return;
    }
    if (!on && port_touch_phase == 2) port_pointer.button(0, false);
    if (!on) {
        port_release_click();
        // Removing the device takes the pointer away from the seat, so Weston stops drawing its cursor.
        port_pointer.close();
    }
    port_touch_mode = on;
    port_touch_phase = 0;
    port_exit_held = false;
    port_touch_since = s.GetTicks();
    port_mirror_at = 0;
    if (!on && port_mirror_texture) {
        s.DestroyTexture(port_mirror_texture);
        port_mirror_texture = nullptr;
        port_mirror_shown = false;
    }
    port_held = PortItem::None;
    if (!on) {
        port_note = "Touch screen off";
        port_note_at = s.GetTicks();
    }
    if (remember && !model.games.empty()) {
        const fs::path path = folder_for_game(model.games[0]) / "touch.ini";
        const fs::path pending = path.string() + ".pending";
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        std::ofstream(pending) << "touch=" << (on ? 1 : 0) << '\n';
        fs::rename(pending, path, ec);
    }
    dirty = true;
}

void App::port_touch_pointer(const TouchPoint& point, Uint32 now) {
    const SDL_Rect exit = port_exit_box();
    const bool in_exit =
        point.x >= exit.x && point.x < exit.x + exit.w && point.y >= exit.y && point.y < exit.y + exit.h;
    // The corner pill leaves touch mode when held; touches there never reach the port.
    if (point.kind == 0 && in_exit && port_touch_phase == 0) {
        port_exit_held = true;
        port_exit_at = now;
        return;
    }
    if (port_exit_held) {
        if (point.kind == 2 || point.kind == 3 || (point.kind == 1 && !in_exit)) {
            port_exit_held = false;
            port_note = "Hold to exit";
            port_note_at = now;
        }
        return;
    }
    port_touch_x = point.x;
    port_touch_y = point.y;
    if (point.kind == 0) {
        port_release_click(); // a quick second tap: finish the first click before moving
        port_pointer.move(point.x, point.y);
        port_touch_phase = 1;
        port_touch_x0 = point.x;
        port_touch_y0 = point.y;
        port_touch_down_at = now;
    } else if (point.kind == 1 && port_touch_phase != 0) {
        port_pointer.move(point.x, point.y);
        const int dx = point.x - port_touch_x0, dy = point.y - port_touch_y0;
        if (port_touch_phase == 1 && dx * dx + dy * dy > 100) {
            port_pointer.button(0, true);
            port_touch_phase = 2;
        }
    } else if (point.kind == 2 || point.kind == 3) {
        if (port_touch_phase == 1 && point.kind == 2) port_click(0, now);
        else if (port_touch_phase == 2) port_pointer.button(0, false);
        port_touch_phase = 0;
    }
}

void App::port_click(int button, Uint32 now) {
    port_release_click();
    port_pointer.button(button, true);
    port_release_button = button;
    port_release_at = (now + 50) | 1;
    dirty = true;
}

void App::port_release_click() {
    if (!port_release_at) return;
    port_pointer.button(port_release_button, false);
    port_release_at = 0;
}

void App::port_touch_tick(Uint32 now) {
    if (port_release_at && int32_t(now - port_release_at) >= 0) port_release_click();
    else if (port_release_at) dirty = true; // keeps the loop turning until the release is sent
    if (port_touch_phase == 1 && now - port_touch_down_at >= 600) {
        port_click(1, now);
        port_touch_phase = 3;
        dirty = true;
    }
    if (port_exit_held) {
        if (now - port_exit_at >= 1000) port_set_touch_mode(false);
        dirty = true;
    }
    if (!port_note.empty() && now - port_note_at < 1900) dirty = true;
    if (now - port_mirror_at >= 80 && !port_mirror.failed) dirty = true; // the next mirror frame
    if (now - port_touch_since < 4200) dirty = true;                     // the hint fades after a while
}

void App::port_touch_pad(SDL_Renderer* r, int y) {
    const Uint32 now = s.GetTicks();
    tint_rect(r, 0, y, 640, 480, {14, 16, 22});
    // The game itself, dimmed, so a finger lands where it means to. Read at about 12 frames a second.
    if (now - port_mirror_at >= 80) {
        port_mirror_at = now;
        port_mirror_shown = false;
        if (port_mirror.open() && port_mirror.grab()) {
            if (!port_mirror_texture || port_mirror_renderer != r || port_mirror_w != port_mirror.width ||
                port_mirror_h != port_mirror.height) {
                if (port_mirror_texture) s.DestroyTexture(port_mirror_texture);
                port_mirror_texture =
                    s.CreateTexture(r, port_mirror.bgr ? SDL_PIXELFORMAT_BGR888 : SDL_PIXELFORMAT_RGB888,
                                    SDL_TEXTUREACCESS_STREAMING, port_mirror.width, port_mirror.height);
                port_mirror_renderer = r;
                port_mirror_w = port_mirror.width;
                port_mirror_h = port_mirror.height;
            }
            if (port_mirror_texture)
                port_mirror_shown =
                    s.UpdateTexture(port_mirror_texture, nullptr, port_mirror.pixels.data(), port_mirror_w * 4) == 0;
        }
    } else port_mirror_shown = port_mirror_texture && !port_mirror.failed;
    if (port_mirror_shown) {
        SDL_Rect whole{0, y, 640, 480};
        s.SetTextureColorMod(port_mirror_texture, 150, 150, 150);
        s.RenderCopy(r, port_mirror_texture, nullptr, &whole);
    }
    // A faint grid, so a finger finds its place on the picture above.
    s.SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    s.SetRenderDrawColor(r, 255, 255, 255, 22);
    for (int x = 80; x < 640; x += 80) s.RenderDrawLine(r, x, y, x, y + 479);
    for (int row = 80; row < 480; row += 80) s.RenderDrawLine(r, 0, y + row, 639, y + row);
    if (port_touch_phase == 0 && (!port_mirror_shown || now - port_touch_since < 4000)) {
        for (int i = 0; i < 2; i++) {
            const std::string line = locale.text(i == 0 ? "Touch here to point and click on the top screen."
                                                        : "Hold still for a right click");
            const int size = i == 0 ? 18 : 16, w = label_width(line, size, false);
            raw_label(r, line, std::max(16, (640 - w) / 2), y + 204 + i * 34, size, i == 0 ? "on_accent" : "muted",
                      608);
        }
    } else {
        // The finger: an accent ring, filled while the left button is held.
        const int cx = port_touch_x, cy = y + port_touch_y;
        fill_round(r, cx - 18, cy - 18, 36, 36, 18, theme.at("accent"));
        if (port_touch_phase != 2) fill_round(r, cx - 13, cy - 13, 26, 26, 13, {14, 16, 22});
    }
    const SDL_Rect exit = port_exit_box();
    const int ex = exit.x, ey = y + exit.y;
    round_rect(r, ex, ey, exit.w, exit.h, exit.h / 2, port_exit_held ? "accent" : "panel");
    ui_icon(r, SettingsIcon::Back, ex + 24, ey + exit.h / 2, 20, port_exit_held ? "on_accent" : "accent");
    label(r, "Hold to exit", ex + 42, centred_label_y(locale.text("Hold to exit"), 16, ey + exit.h / 2), 16,
          port_exit_held ? "on_accent" : "ink", exit.w - 50);
    if (port_exit_held) {
        const int fill = int(std::min<Uint32>(1000, now - port_exit_at) * (exit.w - 32) / 1000);
        fill_round(r, ex + 16, ey + exit.h - 7, std::max(4, fill), 4, 2, theme.at("on_accent"));
    }
    if (!port_note.empty() && now - port_note_at < 1800) {
        const std::string note = locale.text(port_note);
        const int w = label_width(note, 20, false) + 40, h = 44, x = (640 - w) / 2, top = y + 24;
        round_rect(r, x, top, w, h, h / 2, "ink");
        raw_label(r, note, x + 20, centred_label_y(note, 20, top + h / 2), 20, "panel", w - 36);
    }
}

// RG DS Play's info screen (play/src/companion/dock.cpp) in the launcher's own drawing.
void App::port_info(SDL_Renderer* r, int y) {
    const Uint32 now = s.GetTicks();
    if (port_off) {
        tint_rect(r, 0, y, 640, 480, {0, 0, 0});
        return;
    }
    if (port_touch_mode) {
        port_touch_pad(r, y);
        return;
    }
    // The game: its picture, its name, Ports and this session's time; the clock and the charge.
    const Game& game = model.games[0];
    settings_card(r, 24, y + 24, 592, 104);
    game_icon(r, game, 40, y + 40, 72);
    const std::string time = clock();
    const int clock_w = label_width(time, 36, false), right = 598, tx = 128, text_w = right - clock_w - 20 - tx;
    raw_label(r, time, right - clock_w, y + 40, 36, "ink", clock_w + 4);
    raw_label(r, game.title, tx, y + 38, 28, "ink", text_w);
    const unsigned minutes = (now - port_began) / 60000;
    const std::string played = minutes < 60 ? std::to_string(minutes) + " " + locale.text("min")
                                            : std::to_string(minutes / 60) + " " + locale.text("h") + " " +
                                                  std::to_string(minutes % 60) + " " + locale.text("min");
    raw_label(r, locale.text("Ports") + "  ·  " + locale.text("Played") + " " + played, tx, y + 84, 16, "muted",
              text_w);
    if (hardware.battery >= 0) {
        // The status bar's battery, the charge beside it.
        const std::string charge = std::to_string(hardware.battery) + "%";
        const int bw = label_width(charge, 16, false);
        battery_icon(r, y + 70, right - 41);
        raw_label(r, charge, right - 41 - (hardware.charging ? 22 : 8) - bw, y + 88, 16, "muted", bw + 4);
    }
    auto badge = [&](int cx, int cy, SettingsIcon symbol, bool lit) {
        round_rect(r, cx - 22, cy - 22, 44, 44, 12, lit ? "accent" : "slot");
        ui_icon(r, symbol, cx, cy, 25, lit ? "on_accent" : "accent");
    };
    // The game switcher opens on a tap; Return home is held, and the hold fills a ring round its badge.
    auto button = [&](PortItem item, SettingsIcon symbol, const std::string& name) {
        const SDL_Rect b = port_box(item);
        const bool held = port_held == item && !port_fired;
        const int top = y + b.y + (held ? 2 : 0), cx = b.x + 38, cy = top + b.h / 2;
        settings_card(r, b.x, top, b.w, b.h, held);
        badge(cx, cy, symbol, held);
        if (held && item == PortItem::Home) {
            const double done = std::min(1.0, (now - port_held_at) / 1000.0);
            color(r, "accent");
            for (int i = 0; i < int(72 * done); i++) {
                const double a = -1.5707963 + i * 6.2831853 / 72, e = a + 6.2831853 / 72;
                for (int radius = 27; radius <= 30; radius++)
                    s.RenderDrawLine(
                        r, cx + int(std::lround(std::cos(a) * radius)), cy + int(std::lround(std::sin(a) * radius)),
                        cx + int(std::lround(std::cos(e) * radius)), cy + int(std::lround(std::sin(e) * radius)));
            }
        }
        const int lx = b.x + 76, lw = b.x + b.w - 12 - lx;
        label(r, name, lx, centred_label_y(locale.text(name), 24, cy), 24, "ink", lw);
    };
    button(PortItem::Switcher, SettingsIcon::Library, "Game switcher");
    button(PortItem::Home, SettingsIcon::Home, "Return home");
    // Brightness: a slider, dragged.
    const SDL_Rect br = port_box(PortItem::Bright);
    settings_card(r, br.x, y + br.y, br.w, br.h, port_dragging);
    const int bcy = y + br.y + br.h / 2, left = br.x + 80, track = br.x + br.w - 92 - left, ty = bcy + 8;
    badge(br.x + 38, bcy, SettingsIcon::Sun, port_dragging);
    label(r, "Brightness", left, y + br.y + 14, 16, "muted", 300);
    const int chosen = std::clamp(port_bright, 10, 100), knob = left + int(std::lround(track * (chosen - 10) / 90.0));
    round_rect(r, left, ty - 7, track, 14, 7, "slot");
    if (knob > left) fill_round(r, left, ty - 7, std::max(14, knob - left), 14, 7, theme.at("accent"));
    round_rect(r, knob - 14, ty - 12, 28, 28, 14, "slot");
    round_rect(r, knob - 14, ty - 14, 28, 28, 14, port_dragging ? "focus" : "panel");
    const std::string pct = std::to_string(chosen) + "%";
    const int pw = label_width(pct, 20, false);
    raw_label(r, pct, br.x + br.w - 18 - pw, centred_label_y(pct, 20, bcy), 20, "ink", pw + 4);
    // Touch screen and screen off, side by side.
    auto half = [&](PortItem item, SettingsIcon symbol, const std::string& name) {
        const SDL_Rect b = port_box(item);
        const bool held = port_held == item;
        settings_card(r, b.x, y + b.y, b.w, b.h, held);
        const int cy = y + b.y + b.h / 2, lx = b.x + 76, lw = b.x + b.w - 12 - lx;
        badge(b.x + 38, cy, symbol, held);
        label(r, name, lx, centred_label_y(locale.text(name), 20, cy), 20, "ink", lw);
    };
    half(PortItem::Touch, SettingsIcon::Touch, "Touch screen");
    half(PortItem::Off, SettingsIcon::Sleep, "Screen off");
    // A note near the bottom, over everything: the volume as it changes, or what a tap did.
    std::string note;
    if (port_volume_at) note = locale.text("Volume") + " " + std::to_string(std::clamp(port_volume, 0, 100)) + "%";
    else if (!port_note.empty() && now - port_note_at < 1800) note = locale.text(port_note);
    if (!note.empty()) {
        const int w = label_width(note, 20, false) + 40, h = 44, x = (640 - w) / 2, top = y + 480 - 24 - h - 2;
        round_rect(r, x, top, w, h, h / 2, "ink");
        raw_label(r, note, x + 20, centred_label_y(note, 20, top + h / 2), 20, "panel", w - 36);
    }
    if (const int level = port_shown_brightness(now); level < 100) {
        s.SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        s.SetRenderDrawColor(r, 0, 0, 0, Uint8(255 - level * 255 / 100));
        SDL_Rect all{0, y, 640, 480};
        s.RenderFillRect(r, &all);
    }
}

// Home in PortMaster: not the game menu, two plain choices. New ports are in Ports when it closes.
void App::port_manager_menu(SDL_Renderer* r, int y) {
    settings_header(r, y, "PortMaster", 475, false);
    const bool busy = menu_status["state"] == "busy";
    const std::array<std::string, 2> names = {"Back to PortMaster", "Close PortMaster"};
    const std::array<std::string, 2> notes = {"Keep getting and updating ports", "Your ports are waiting in Ports"};
    const std::array<SettingsIcon, 2> symbols = {SettingsIcon::Play, SettingsIcon::Home};
    for (int i = 0; i < 2; i++) {
        const int row = y + 132 + i * 104;
        settings_card(r, 28, row, 584, 84, settings.focus == i);
        round_rect(r, 44, row + 18, 48, 48, 14, "panel");
        ui_icon(r, symbols[i], 68, row + 42, 32, busy ? "muted" : "accent");
        label(r, names[i], 112, row + 16, 24, busy ? "muted" : "ink", 420);
        label(r, notes[i], 112, row + 50, 16, "muted", 420);
        const std::string key = i == 0 ? "B" : "A";
        if (settings.focus == i || i == 0) key_badge(r, 612 - 18 - key_badge_width(key), row + 42, key, !busy);
    }
    if (busy && !menu_status["message"].empty()) wrapped(r, menu_status["message"], 30, y + 360, 18, 580, 2);
}

void App::port_manager_action(int code) {
    if (menu_status["state"] == "busy") return;
    if (code == 3 || code == 7) menu_request("resume");
    else if (code == 8 || code == 9) settings.focus = code == 8 ? 0 : 1;
    else if (code == 2 || code == 4) menu_request(settings.focus == 0 ? "resume" : "quit");
    dirty = true;
}

void App::port_manager_touch(int x, int y) {
    if (x < 28 || x > 612) return;
    for (int i = 0; i < 2; i++)
        if (y >= 132 + i * 104 && y < 216 + i * 104) {
            settings.focus = i;
            port_manager_action(2);
        }
}
