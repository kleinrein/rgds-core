#pragma once
// Menu sound cues, the DAC level and the sound actions on settings rows.
// Members of App in main.cpp; included once, right after the class.

App::SoundState App::sound_state() const {
    return {settings.page,
            home_menu,
            picker_open,
            preferences.view,
            preferences.layout,
            settings.page == Page::Settings ? settings.section : settings.focus,
            picker_focus,
            key_row,
            key_col,
            hardware.volume_percent,
            system_preferences.brightness,
            navigation_selected(),
            edit_buffer.size(),
            sound_toggles(),
            game_preferences.favourite,
            launching,
            home_bar_focus};
}

void App::configure_haptics() {
    haptics.set_strength(preferences.menu_haptics && !probe ? preferences.haptic_strength : 0);
}

void App::configure_sounds() {
    configure_haptics();
    sounds.set_master(hardware.volume ? hardware.volume_percent : 100);
    if (!sounds.opened) sounds.opened = [this] { restore_dac_level(); };
    sounds.configure(preferences.menu_sounds && !probe && !sleep_audio_parked && settings.page != Page::Microphone,
                     preferences.sound_volume, asset_path("sounds"));
    // Music belongs to the home, library and settings; not to the in-game
    // menu, the microphone test, sleep or a launch in progress.
    sounds.configure_music(preferences.menu_music && !probe && !o.game_menu && !sleep_audio_parked &&
                               settings.page != Page::Microphone && !launching,
                           preferences.music_volume, asset_path("music") / "menu.wav");
}

void App::restore_dac_level() {
    if (o.preview) return;
    int level = hardware.volume ? hardware.volume_percent : -1;
    if (level < 0) {
        auto saved = fields(o.data / "volume.ini");
        try {
            if (saved["schema"] == "1") level = std::clamp(std::stoi(saved["volume"]), 0, 100);
        } catch (const std::exception&) {
            level = -1;
        }
    }
    if (level >= 0 && write_mixer_percent(dac_percent_for_level(level))) return;
    if (!system.sync_volume()) {
        sounds.close();
        sounds.error = "Menu audio is unavailable on this device.";
    }
}

bool App::emit_sound(MenuCue cue, int direction) {
    // The motor answers first and also while muted, as a phone does on silent.
    if (!probe) haptics.play(static_cast<Haptic>(cue));
    if (probe || settings.page == Page::Microphone || (hardware.volume && hardware.volume_percent == 0)) return false;
    return sounds.play(cue, s.GetTicks(), direction, motion_touch ? (contact_x / 640.f - .5f) * .5f : direction * .12f);
}

void App::sound_intent(MenuCue cue) {
    if (sound_depth) explicit_sound = cue;
    else emit_sound(cue);
}

void App::finish_sound_action() {
    if (--sound_depth) return;
    if (!departure_action.empty()) {
        explicit_sound.reset();
        return;
    }
    const auto after = sound_state();
    auto before = sound_before;
    int direction = sound_code == 0 || sound_code == 8 ? -1 : 1;
    if (explicit_sound) {
        emit_sound(*explicit_sound);
        explicit_sound.reset();
        return;
    }
    if (before.launch || after.launch) return;
    if (before.page == Page::GameOptions && after.page == Page::GameOptions && before.favourite != after.favourite) {
        emit_sound(after.favourite ? MenuCue::Favourite : MenuCue::Unfavourite);
        return;
    }
    if (before.toggles != after.toggles) {
        emit_sound((after.toggles & ~before.toggles) ? MenuCue::Enable : MenuCue::Disable);
        return;
    }
    if (before.page != after.page || before.home != after.home || before.picker != after.picker) {
        bool back = sound_code == 3 || view_route == ViewTravel::Back;
        emit_sound(back ? MenuCue::Back : before.picker && !after.picker ? MenuCue::Confirm : MenuCue::Open);
        return;
    }
    if (before.view != after.view || before.layout != after.layout) {
        emit_sound(MenuCue::Open, direction);
        return;
    }
    if (before.volume != after.volume || before.brightness != after.brightness) {
        emit_sound(MenuCue::Adjust, direction);
        return;
    }
    if (before.text != after.text) {
        emit_sound(MenuCue::Type);
        return;
    }
    if (before.game != after.game || before.focus != after.focus || before.pick != after.pick ||
        before.row != after.row || before.col != after.col || before.bar != after.bar)
        emit_sound(MenuCue::Navigate, direction);
    else if (!chat_open && !filter_sheet_active() &&
             (sound_code == 8 || sound_code == 9 ||
              ((sound_code == 0 || sound_code == 1) && settings.page == Page::Library && !picker_open)) &&
             (picker_open || settings.page == Page::Library || list_count() > 0)) {
        // A move that went nowhere: the end of a list. One bump, not one per
        // repeat while the button stays held there.
        const Uint32 now = s.GetTicks();
        if ((!edge_felt_at || now - edge_felt_at > 350) && !probe) haptics.play(Haptic::Edge);
        edge_felt_at = now;
    }
}

void App::haptics_action(int code) {
    if (code == 3) {
        settings.back();
        dirty = true;
        return;
    }
    if (code == 8 || code == 9) {
        settings.focus = std::clamp(settings.focus + (code == 8 ? -1 : 1), 0, 4);
        dirty = true;
        return;
    }
    if (code != 0 && code != 1 && code != 2 && code != 4) return;
    // A strength change is felt at once: a Select tap for the menus, a short
    // burst at the new level for games.
    auto feel = [this](bool rumble) {
        configure_haptics();
        if (probe) return;
        if (rumble) {
            haptics.set_rumble_strength(preferences.rumble_strength);
            haptics.rumble(1, 260);
        } else haptics.play(Haptic::Select, true);
    };
    const int row = settings.focus;
    if (row == 0 || row == 2) {
        bool& value = row == 0 ? preferences.menu_haptics : preferences.game_rumble;
        value = (code == 2 || code == 4) ? !value : code == 1;
        remember_appearance();
        configure_haptics(); // turning menu haptics on is felt through the Switch on cue itself
        if (value && row == 2) feel(true);
        sound_intent(value ? MenuCue::Enable : MenuCue::Disable);
    } else if (row == 1 || row == 3) {
        int& value = row == 1 ? preferences.haptic_strength : preferences.rumble_strength;
        if ((row == 1 && !preferences.menu_haptics) || (row == 3 && !preferences.game_rumble)) return;
        if (code == 0 || code == 1) {
            value = std::clamp(value + (code == 0 ? -10 : 10), 10, 100);
            remember_appearance();
            feel(row == 3);
            sound_intent(MenuCue::Adjust);
        } else
            show_picker(row == 1 ? "Haptic strength" : "Rumble strength", {"20%", "40%", "60%", "80%", "100%"},
                        std::clamp((value + 10) / 20 - 1, 0, 4), [this, row](int n) {
                            (row == 1 ? preferences.haptic_strength : preferences.rumble_strength) = (n + 1) * 20;
                            remember_appearance();
                            configure_haptics();
                            if (row == 3) {
                                haptics.set_rumble_strength(preferences.rumble_strength);
                                haptics.rumble(1, 260);
                            } else haptics.play(Haptic::Select, true);
                        });
    } else if (preferences.menu_haptics) {
        std::vector<std::string> names(haptic_labels.begin(), haptic_labels.end());
        show_picker("Try a pattern", names, 0, [this](int n) { haptics.play(static_cast<Haptic>(n), true); });
    }
    dirty = true;
}

void App::menu_sounds_action(int code) {
    if (code == 3) {
        settings.back();
        dirty = true;
        return;
    }
    if (code == 8 || code == 9) {
        settings.focus = std::clamp(settings.focus + (code == 8 ? -1 : 1), 0, 4);
        dirty = true;
        return;
    }
    if (code != 0 && code != 1 && code != 2 && code != 4) return;
    if (settings.focus == 0) {
        preferences.menu_sounds = (code == 2 || code == 4) ? !preferences.menu_sounds : code == 1;
        remember_appearance();
    } else if (settings.focus == 1) {
        if (code == 0 || code == 1) {
            preferences.sound_volume = std::clamp(preferences.sound_volume + (code == 0 ? -5 : 5), 0, 100);
            remember_appearance();
            sound_intent(MenuCue::Adjust);
        } else
            show_picker("Effects volume", {"Mute", "25%", "50%", "75%", "100%"}, (preferences.sound_volume + 12) / 25,
                        [this](int n) {
                            preferences.sound_volume = n * 25;
                            remember_appearance();
                            sound_intent(MenuCue::Adjust);
                        });
    } else if (settings.focus == 2) {
        preferences.menu_music = (code == 2 || code == 4) ? !preferences.menu_music : code == 1;
        remember_appearance();
        sound_intent(preferences.menu_music ? MenuCue::Enable : MenuCue::Disable);
    } else if (settings.focus == 3) {
        if (code == 0 || code == 1) {
            preferences.music_volume = std::clamp(preferences.music_volume + (code == 0 ? -5 : 5), 0, 100);
            remember_appearance();
        } else
            show_picker("Music volume", {"Mute", "20%", "40%", "60%", "80%", "100%"},
                        std::clamp((preferences.music_volume + 10) / 20, 0, 5), [this](int n) {
                            preferences.music_volume = n * 20;
                            remember_appearance();
                        });
    } else {
        std::vector<std::string> names(menu_cue_labels.begin(), menu_cue_labels.end());
        show_picker("Preview sound", names, 0, [this](int n) { sound_intent(static_cast<MenuCue>(n)); });
    }
    dirty = true;
}
