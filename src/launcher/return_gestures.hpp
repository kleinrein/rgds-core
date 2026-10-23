#pragma once
// Returning home, the controls handover and the edge/gesture cues.
// Members of App in main.cpp; included once, right after the class.

void App::return_home() {
    view_route = ViewTravel::Back;
    cancel_navigation();
    home_menu = true;
    home_selected = size_t(preferences.view) + 1;
    carousel.reset(home_selected);
    selected_at = s.GetTicks();
    dirty = true;
}

bool App::return_writes_pending() const {
    return return_control_child > 0 || return_volume || return_brightness || return_power || return_lid;
}

void App::begin_return_controls() {
    if (return_control_child > 0 || system.volume_pending() || system.brightness_pending()) return;
    if (!return_volume && !return_brightness && !return_power && !return_lid) return;
    std::vector<std::string> arguments = {o.executable.string(),
                                          "--restore-controls",
                                          o.app.string(),
                                          o.data.string(),
                                          std::to_string(return_volume.value_or(-1)),
                                          std::to_string(return_brightness.value_or(-1)),
                                          std::to_string(return_power.value_or(-1)),
                                          std::to_string(return_lid.value_or(-1))};
    std::vector<char*> argv;
    for (auto& argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    active_return_volume = return_volume;
    active_return_brightness = return_brightness;
    return_volume.reset();
    return_brightness.reset();
    return_power.reset();
    return_lid.reset();
    system.discard_status();
    return_control_child = fork();
    if (return_control_child == 0) {
        execv(argv[0], argv.data());
        _exit(127);
    }
    if (return_control_child < 0) {
        return_audio_failed = active_return_volume.has_value();
        status = "ERROR: Could not restore settings";
    }
}

void App::poll_return_controls(bool wait) {
    if (return_control_child > 0) {
        int result = 0;
        pid_t completed;
        do {
            completed = waitpid(return_control_child, &result, wait ? 0 : WNOHANG);
        } while (completed < 0 && errno == EINTR);
        if (completed == 0) return;
        return_control_child = -1;
        int failure = completed > 0 && WIFEXITED(result) ? WEXITSTATUS(result) : 127;
        if (active_return_volume) {
            return_audio_failed = (failure & 1) != 0;
            if (!return_audio_failed) volume_applied = *active_return_volume;
        }
        if (active_return_brightness && !(failure & 2)) brightness_applied = *active_return_brightness;
        if (failure) status = "ERROR: Could not restore settings - see log";
        if (!return_volume && active_return_volume) {
            if (return_audio_failed) hardware.volume_percent = confirmed_volume();
            volume_unsaved = false;
        }
        if (!return_brightness && active_return_brightness) {
            if (failure & 2) {
                system_preferences.brightness = brightness_applied;
                hardware.brightness_percent = brightness_applied;
            }
            if (brightness_unsaved) {
                try {
                    save_system_preferences(o.system_preferences, system_preferences);
                } catch (const std::exception& error) {
                    status = error.what();
                }
            }
            brightness_unsaved = false;
        }
        active_return_volume.reset();
        active_return_brightness.reset();
        dirty = true;
    }
    begin_return_controls();
    if (!wait && !return_writes_pending() && return_status_needed && system.request_status()) {
        return_status_needed = false;
        system_poll = s.GetTicks();
    }
}

void App::flush_return_controls() {
    while (return_writes_pending()) {
        poll_return_controls(true);
        if (return_control_child <= 0 && (system.volume_pending() || system.brightness_pending())) {
            system.flush_volume();
            system.flush_brightness();
        }
    }
}

void App::check_fonts(bool first_frame) {
    if (!font_ready || font_checked) return;
    try {
        if (first_frame) {
            if (o.boot_warning) rounded.check({16, 20, 28, 36});
            else rounded.check({16, 20, 28});
        } else {
            rounded.check();
            font_checked = true;
        }
    } catch (const std::exception& e) {
        font_ready = false;
        preferences.pixel = true;
        std::cerr << "Font fallback: " << e.what() << std::endl;
    }
}

void App::gesture_intro() {
    remember_appearance();
    show_detail(
        "Touch gestures",
        locale.text("Swipe up from the bottom edge to go Home from anywhere.") + "\n" +
            locale.text("Swipe in from the left edge to go Back.") + "\n" +
            locale.text(
                "A small handle at the bottom of the lower screen shows gestures are on; it rises with your finger, and a ring marks the spot when a gesture fires."));
}

void App::gesture_cues(SDL_Renderer* r, int y) {
    if (!gestures_available()) return;
    const Uint32 now = s.GetTicks();
    const bool bottom = pointer_down && edge_gesture == EdgeGesture::Bottom,
               left = pointer_down && edge_gesture == EdgeGesture::Left;
    const double pb = bottom ? std::clamp((down_y - contact_y) / 72.0, 0.0, 1.0) : 0,
                 pl = left ? std::clamp((contact_x - down_x) / 72.0, 0.0, 1.0) : 0;
    {
        const int w = 72 + int(48 * pb), lift = int(26 * pb);
        alpha_round(r, 320 - w / 2, y + 471 - lift, w, 5, 2, theme.at(pb > 0 ? "accent" : "ink"),
                    int((preferences.dark ? 90 : 110) + 140 * pb));
        if (pb > .2) {
            const std::string text = locale.text("Home");
            const int pw = label_width(text, 16, false) + 52;
            alpha_round(r, 320 - pw / 2, y + 428 - lift, pw, 34, 17, theme.at("accent"),
                        int(250 * std::min(1.0, (pb - .2) * 2)));
            if (pb > .5) {
                ui_icon(r, SettingsIcon::Home, 320 - pw / 2 + 20, y + 445 - lift, 18, "on_accent");
                raw_label_colour(r, text, 320 - pw / 2 + 36, centred_label_y(text, 16, y + 445 - lift), 16,
                                 theme.at("on_accent"), pw);
            }
        }
    }
    if (pl > 0) {
        const int cx = -44 + int(80 * pl);
        alpha_round(r, cx - 40, y + 200, 80, 80, 40, theme.at("accent"), int(250 * pl));
        if (pl > .4) ui_icon(r, SettingsIcon::Back, cx + 14, y + 240, 24, "on_accent");
    }
    if (preferences.animations && gesture_flash_at && now - gesture_flash_at < 260) {
        const double t = (now - gesture_flash_at) / 260.0;
        feedback_arc(r, gesture_flash_kind ? 24 : 320, y + (gesture_flash_kind ? 240 : 468), int(18 + 56 * t), 0, 1,
                     theme.at("accent"), int(220 * (1 - t)), 4);
    }
}

bool App::gestures_available() const {
    return preferences.gestures && !probe && !(chat_open && chat.view == pictochat::View::Room) && !launching &&
           settings.page != Page::BootWarning && settings.page != Page::Controls && !o.game_menu;
}

bool App::edge_gesture_move(int x, int y) {
    if (edge_gesture == EdgeGesture::None) return false;
    const bool home = edge_gesture == EdgeGesture::Bottom && down_y - y >= 72 && std::abs(x - down_x) < 120,
               back = edge_gesture == EdgeGesture::Left && x - down_x >= 72 && std::abs(y - down_y) < 120;
    if (!home && !back) return false;
    edge_gesture = EdgeGesture::None;
    cancel_navigation();
    brightness_drag = volume_drag = false;
    gesture_flash_at = s.GetTicks();
    gesture_flash_kind = home ? 0 : 1;
    micro_until = gesture_flash_at + 270;
    if (home) {
        const bool chatting = chat_open;
        close_pictochat();
        picker_open = false;
        if (settings.page != Page::Library) {
            settings.page = Page::Library;
            settings.focus = 0;
        }
        if (!home_menu) return_home();
        if (!chatting) sound_intent(MenuCue::Back);
    } else action(3);
    dirty = true;
    return true;
}
