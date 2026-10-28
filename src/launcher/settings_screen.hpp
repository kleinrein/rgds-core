#pragma once
// Settings screens: the top panel (title, hint, network card) and the
// scrolling settings list on the bottom panel. Members of App in main.cpp;
// included once, right after the class definition.

void App::list_scrollbar(SDL_Renderer* r, int y) {
    if (list_max_scroll() <= 0) return;
    int thumb = std::max(54, list_height * list_height / (list_count() * list_stride - 16));
    draw_scrollbar(r, 596, y + list_top, list_height, thumb, list_scroll * (list_height - thumb) / list_max_scroll(),
                   scroll_drag, scroll_hover);
}

// Sound's first list row: the same card as its neighbours with the volume slider inside it.
void App::volume_row(SDL_Renderer* r, int y) {
    int row = y + list_top - list_scroll;
    const int value = hardware.volume_percent;
    const bool focused = settings.focus == 0;
    int dx = 24;
    move_element(0, dx, row, 566, 66, focused, list_top, list_top + list_height, 20, 595);
    dx -= 24;
    settings_card(r, 24 + dx, row, 566, 66, focused);
    ui_icon(r, SettingsIcon::Volume, 50 + dx, row + 19, 22, "accent");
    label(r, "Volume", 72 + dx, row + 7, 22, "ink", 300);
    {
        const std::string shown = hardware.volume ? std::to_string(value) + "%" : locale.text("Unavailable");
        raw_label(r, shown, 566 - label_width(shown, hardware.volume ? 22 : 18, false) + dx,
                  row + (hardware.volume ? 7 : 10), hardware.volume ? 22 : 18, "accent", 200);
    }
    const int track = row + 46, position = sound_track_x + value * sound_track_width / 100 + dx;
    sunken_track(r, sound_track_x - 4 + dx, track - 2, sound_track_width + 8, 14, 7);
    if (position - sound_track_x - dx > 8)
        glossy_pill(r, sound_track_x + dx, track, std::max(10, position - sound_track_x - dx), 10, 5,
                    theme.at("accent"));
    raised_pill(r, std::clamp(position - 14, 40 + dx, 548 + dx), track - 10, 28, 30, 10,
                theme.at(hardware.volume ? "accent" : "muted"), volume_drag ? .6 : 0);
    ui_icon(r, SettingsIcon::Sliders, std::clamp(position, sound_track_x + dx, sound_track_x + sound_track_width + dx),
            track + 5 + (volume_drag ? 1 : 0), 14, "on_accent");
}

void App::choice(SDL_Renderer* r, int x, int y, const std::string& title, bool selected, bool focused) {
    move_element(uint32_t(x + y), x, y, 280, 48, focused && selected);
    alpha_round(r, x, y + 3, 280, 48, 12, shadow_colour(), preferences.dark ? 70 : 26);
    round_rect(r, x, y, 280, 48, 12, focused ? "accent" : "line");
    if (selected) raised_pill(r, x + 2, y + 2, 276, 44, 10, theme.at("accent"), 0, false);
    else round_rect(r, x + 2, y + 2, 276, 44, 10, "panel");
    ui_icon(r,
            title == "Cancel"     ? SettingsIcon::Back
            : title == "Continue" ? SettingsIcon::Check
                                  : settings_icon_for(title),
            x + 28, y + 24, 21, selected ? "on_accent" : "muted");
    label(r, title, x + 50, y + 9, 20, selected ? "on_accent" : "ink", 182);
    if (selected) {
        color(r, "on_accent");
        s.RenderDrawLine(r, x + 248, y + 24, x + 253, y + 29);
        s.RenderDrawLine(r, x + 253, y + 29, x + 263, y + 18);
    }
}

void App::settings_top(SDL_Renderer* r, int y) {
    if (settings.page == Page::GameInfo) {
        game_info_top(r, y);
        return;
    }
    if (settings.page == Page::ShaderPicker) {
        render_shader_preview(r, y);
        return;
    }
    if (settings.page == Page::Homebrew) {
        homebrew_top(r, y);
        return;
    }
    // Navigation belongs on the lower panel. Keep this identity independent
    // of row focus and setting values so the upper panel stays quiet.
    std::string title = "Settings";
    SettingsIcon symbol = SettingsIcon::System;
    Page page = settings.page == Page::Confirm ? confirm_return : settings.page;
    switch (page) {
    case Page::MenuSounds:
        title = "Sounds & music";
        symbol = SettingsIcon::Volume;
        break;
    case Page::Haptics:
        title = "Vibration";
        symbol = SettingsIcon::Vibrate;
        break;
    case Page::Motion:
        title = "Animations";
        symbol = SettingsIcon::Spark;
        break;
    case Page::Colours: colours_top(r, y); return;
    case Page::Appearance:
        title = "Theme";
        symbol = SettingsIcon::Display;
        break;
    case Page::DisplayOptions:
        title = "Display";
        symbol = SettingsIcon::Sun;
        break;
    case Page::Display:
        title = "Screen brightness";
        symbol = SettingsIcon::Sun;
        break;
    case Page::Power:
        title = "Power";
        symbol = SettingsIcon::Power;
        break;
    case Page::LibraryOptions:
    case Page::Homebrew:
        title = "Library";
        symbol = SettingsIcon::Library;
        break;
    case Page::Scraper:
    case Page::GameArtwork:
        title = "Artwork";
        symbol = SettingsIcon::Art;
        break;
    case Page::Keyboard:
        title = edit_field == 5   ? "DS nickname"
                : edit_field == 2 ? "Connectivity"
                : edit_field == 3 ? "Artwork"
                : edit_field == 4 ? "Search"
                : edit_field == 6 ? "Cheats"
                                  : "ScreenScraper";
        symbol = edit_field == 2 ? SettingsIcon::Wifi : edit_field == 6 ? SettingsIcon::Spark : SettingsIcon::Art;
        break;
    case Page::Microphone:
        title = "Test microphone";
        symbol = SettingsIcon::Volume;
        break;
    case Page::Sound:
        title = "Sound";
        symbol = SettingsIcon::Volume;
        break;
    case Page::Network:
    case Page::Wifi:
    case Page::Bluetooth:
        title = "Connectivity";
        symbol = SettingsIcon::Wifi;
        break;
    case Page::Timezone:
        title = "Timezone";
        symbol = SettingsIcon::Clock;
        break;
    case Page::System:
    case Page::DeviceInfo:
    case Page::Maintenance: title = "System"; break;
    case Page::SaveGallery:
        title = gallery_title();
        symbol = gallery_icon();
        break;
    case Page::RomSetup:
        title = "Add your games";
        symbol = SettingsIcon::Cartridge;
        break;
    case Page::PortFiles:
        title = "Add game files";
        symbol = SettingsIcon::Load;
        break;
    case Page::DSControls:
        title = "DS controls";
        symbol = SettingsIcon::Controller;
        break;
    case Page::Statistics:
        title = "Statistics";
        symbol = SettingsIcon::Clock;
        break;
    case Page::Controls:
        title = "Controls";
        symbol = SettingsIcon::Controller;
        break;
    case Page::GameOptions:
        title = "Game options";
        symbol = SettingsIcon::Cartridge;
        break;
    case Page::Graphics:
    case Page::PlayGraphics:
        title = "Graphics";
        symbol = SettingsIcon::Display;
        break;
    case Page::EmulatorOptions:
        title = std::string(core_names[active_engine()]) + " settings";
        symbol = SettingsIcon::Cartridge;
        break;
    case Page::Cheats:
        title = "Cheats";
        symbol = SettingsIcon::Spark;
        break;
    case Page::QuickMenu:
    case Page::ApplyShader:
        title = "Game menu";
        symbol = SettingsIcon::Controller;
        break;
    default: break;
    }
    if (page == Page::Cheats) {
        cheats_top(r, y);
        return;
    }
    if (page == Page::GameSwitcher && !switch_games.empty()) {
        settings_card(r, 24, y + 82, 592, 222);
        const auto& game = switch_games[std::min(settings.focus, int(switch_games.size()) - 1)];
        if (!cover_art(r, folder_for_game(game) / "box.bmp", {46, y + 104, 174, 174}, true))
            game_icon(r, game, 89, y + 147, 88);
        label(r,
              home_switcher         ? std::string(system_info(game.system).name)
              : settings.focus == 0 ? "Resume"
                                    : "Switch game",
              246, y + 103, 18, "accent", 344);
        raw_wrapped(r, library_title(game.title), 246, y + 139, 26, 344, 2);
        if (!home_switcher) wrapped(r, "Your current game is saved before switching.", 246, y + 222, 16, 344, 2);
        if (!menu_status["message"].empty()) wrapped(r, menu_status["message"], 32, y + 408, 18, 576, 2);
        return;
    }
    if (page == Page::MenuSounds) {
        settings_card(r, 28, y + 106, 584, 264);
        ui_icon(r, settings.focus >= 2 && settings.focus <= 3 ? SettingsIcon::Spark : SettingsIcon::Volume, 320,
                y + 160, 48, "accent");
        centered(r, "A little handheld magic", y + 205, 2, "ink");
        std::string note =
            settings.focus >= 2 && settings.focus <= 3
                ? (preferences.menu_music
                       ? (sounds.error.empty() ? "A soft tune plays while you browse. It fades out when a game starts."
                                               : sounds.error)
                       : "Turn on menu music for a soft tune while you browse.")
            : preferences.menu_sounds
                ? (sounds.error.empty() ? "Playful sounds for browsing, choosing and starting games." : sounds.error)
                : "Turn on menu sounds to hear the previews.";
        wrapped(r, note, 52, y + 258, 20, 536, 3);
        return;
    }
    if (page == Page::Haptics) {
        settings_card(r, 28, y + 106, 584, 264);
        ui_icon(r, SettingsIcon::Vibrate, 320, y + 160, 48, "accent");
        const bool games = settings.focus == 2 || settings.focus == 3;
        centered(r, games ? "Feel the game" : "Feel every move", y + 205, 2, "ink");
        std::string note =
            !haptics.available() ? "No vibration motor was found on this device."
            : games              ? (preferences.game_rumble
                                        ? "Rumble Pak games on DS, rumble cartridges on Game Boy and Game Boy Advance."
                                        : "Turn on game rumble for games made for a rumble accessory.")
            : preferences.menu_haptics ? "A light tap for every move, a firmer one when you choose, open or save."
                                       : "Turn on menu haptics to feel the menus.";
        wrapped(r, note, 52, y + 258, 20, 536, 3);
        return;
    }
    if (page == Page::SaveGallery) {
        round_rect(r, 24, y + 80, 592, 326, 12, "panel");
        auto key = std::to_string(settings.focus);
        const bool occupied = slot_occupied(settings.focus);
        if (!media.draw(r, slots["image_" + key], {40, y + 104, 320, 240}, false))
            ui_icon(r, gallery_icon(), 200, y + 220, 70, "muted");
        label(r, locale.text("Slot") + " " + std::to_string(settings.focus + 1), 388, y + 122, 28, "ink", 208);
        raw_label(r, core_names[gallery_core], 388, y + 173, 20, "accent", 208);
        raw_wrapped(r, occupied ? slots["slot_" + key] : locale.text("Empty slot"), 388, y + 219, 20, 208, 2);
        wrapped(r, slot_hint(occupied), 388, y + 272, 16, 208, 5);
        wrapped(r, o.game_menu ? menu_status["message"] : device_status["message"], 36, y + 418, 16, 568, 2);
        return;
    }
    if (page == Page::Microphone) {
        round_rect(r, 24, y + 82, 592, 320, 12, "panel");
        ui_icon(r, SettingsIcon::Volume, 320, y + 145, 52, "accent");
        centered(r, "Test microphone", y + 202, 2, "ink");
        wrapped(r, "Blow or speak into the microphone. Watch the level rise, then fall when you stop.", 54, y + 255, 22,
                532, 4);
        return;
    }
    if (page == Page::DSControls) {
        round_rect(r, 24, y + 82, 592, 320, 12, "panel");
        ui_icon(r, SettingsIcon::Controller, 320, y + 126, 48, "accent");
        centered(r, "DS controls", y + 181, 2, "ink");
        wrapped(
            r,
            !ds_supported() ? "Use the engine's native controls for microphone and DS settings."
            : status.empty()
                ? "Real microphone is enabled by default. Blow and lid actions return to the game for two seconds. Language changes apply after restarting."
                : status,
            54, y + 242, 22, 532, 5);
        return;
    }
    if (page == Page::PortFiles) {
        port_files_top(r, y);
        return;
    }
    if (page == Page::RomSetup) {
        round_rect(r, 24, y + 82, 592, 320, 12, "panel");
        ui_icon(r, SettingsIcon::Cartridge, 320, y + 128, 48, "accent");
        centered(r,
                 !library_ready  ? "Looking for games…"
                 : setup_scanned ? "Scan complete"
                                 : "Your DS library starts here",
                 y + 177, 2, "ink");
        const size_t found = model.games.size();
        if (!library_ready)
            wrapped(r, "Reading Roms/NDS on your games card. This takes a moment.", 54, y + 242, 24, 532, 4);
        else if (setup_scanned)
            wrapped(
                r,
                found ? locale.text(locale.text(std::to_string(found) + (found == 1 ? " game" : " games")) +
                                    " found. Choose Done to go to your library.")
                      : locale.text(
                            "No games found. Copy .nds or DS ZIP files to Roms/NDS on your games card and scan again."),
                54, y + 242, 24, 532, 4);
        else
            wrapped(r, "Copy .nds or DS ZIP files to Roms/NDS on your games card, then choose Scan for games.", 54,
                    y + 242, 24, 532, 4);
        return;
    }
    if (page == Page::Statistics) {
        const auto& group = summary.groups[0];
        // A ring of your collection: the played share fills it, the hours sit inside.
        settings_card(r, 24, y + 66, 592, 344);
        const double share = group.games ? double(group.played) / double(group.games) : 0;
        const int rx = 150, ry = y + 190;
        feedback_arc(r, rx, ry, 78, 0, 1, theme.at("slot"), 255, 12);
        if (summary_ready && share > 0)
            feedback_arc(r, rx, ry, 78, 0, std::max(.02, share), theme.at("accent"), 255, 12);
        const std::string hours = summary_ready ? std::to_string(group.seconds / 3600) + "h" : "…",
                          minutes = summary_ready ? std::to_string((group.seconds / 60) % 60) + "m" : "";
        raw_label(r, hours, rx - label_width(hours, 36, false) / 2, ry - 34, 36, "ink", 160);
        raw_label(r, minutes, rx - label_width(minutes, 20, false) / 2, ry + 10, 20, "muted", 160);
        {
            const std::string caption = locale.text("Time played");
            raw_label(r, caption, rx - label_width(caption, 14, false) / 2, ry + 94, 14, "muted", 200);
        }
        // Three tiles on the right.
        const std::array<std::string, 3> values = {
            summary_ready ? std::to_string(group.games) : "…",
            summary_ready ? std::to_string(group.played) +
                                (group.games ? "  ·  " + std::to_string(int(std::lround(share * 100))) + "%" : "")
                          : "…",
            summary_ready ? std::to_string(group.sessions) : "…"};
        const std::array<const char*, 3> titles = {"Games", "Games played", "Times played"};
        for (int i = 0; i < 3; i++) {
            const int row = y + 92 + i * 72;
            round_rect(r, 276, row, 320, 60, 12, "slot");
            round_rect(r, 277, row + 1, 318, 58, 11, "background");
            label(r, titles[i], 296, row + 8, 14, "muted", 280);
            raw_label(r, values[size_t(i)], 296, row + 27, 24, "ink", 280);
        }
        // The favourite: the most played game, with its icon.
        round_rect(r, 276, y + 310, 320, 76, 12, "focus");
        if (summary_ready && !summary.most_played.empty()) {
            const auto& top = summary.most_played[0];
            game_icon(r, top.game, 292, y + 324, 48, 0);
            label(r, "Most played", 352, y + 320, 14, "muted", 232);
            raw_label(r, top.title, 352, y + 341, 20, "ink", 232);
        } else {
            ui_icon(r, SettingsIcon::Heart, 316, y + 348, 28, "muted");
            label(r, summary_ready ? "Nothing played yet" : "Preparing…", 352, y + 338, 16, "muted", 232);
        }
        return;
    }
    if (page == Page::Maintenance) {
        if (update_stage() != UpdateStage::None) {
            update_card(r, y);
            return;
        }
        ui_icon(r, SettingsIcon::System, 320, y + 112, 40, "accent");
        centered(r, "Updates & recovery", y + 157, 2, "ink");
        round_rect(r, 32, y + 211, 576, 177, 12, "panel");
        label(r, "Installed version", 52, y + 226, 16, "muted", 530);
        raw_label(r, o.app.empty() ? "Development" : o.app.filename().string(), 52, y + 251, 20, "ink", 530);
        std::string message =
            update_status["message"].empty() ? "Check for updates when you are ready." : update_status["message"];
        if (!service_notice.empty()) message = service_notice;
        else if (worker_action.rfind("device.update-", 0) != 0 && !device_status["message"].empty())
            message = device_status["message"];
        wrapped(r, message, 52, y + 288, 20, 530, 3);
        int progress = std::clamp(std::atoi(update_status["progress"].c_str()), 0, 100);
        bool busy = worker > 0 && worker_action.rfind("device.update-", 0) == 0;
        if (busy) {
            round_rect(r, 52, y + 367, 494, 6, 3, "slot");
            round_rect(r, 52, y + 367, std::max(3, 494 * progress / 100), 6, 3, "accent");
            raw_label(r, std::to_string(progress) + "%", 552, y + 354, 16, "muted", 48);
        }
        if (!update_status["version"].empty()) raw_label(r, update_status["version"], 52, y + 408, 20, "accent", 530);
        return;
    }
    // The DSi's top screen: a tinted note about where you are, with the
    // shoulder buttons as tabs in the lower corners when they do something.
    const bool zone = page == Page::Timezone && !timezones.empty();
    const std::string focused_zone =
        zone ? timezones[size_t(std::clamp(settings.focus, 0, int(timezones.size()) - 1))] : "";
    // Pages that draw a card or prompt inside the note keep their words near its top.
    const bool crowded = page == Page::Network || page == Page::Wifi || page == Page::Bluetooth ||
                         page == Page::Homebrew || page == Page::GameArtwork ||
                         (page == Page::Keyboard && edit_field == 3) ||
                         (settings.page == Page::Controls && control_capture);
    const int note_y = crowded ? y : y + 64;
    dsi_note(r, note_y, y, symbol,
             zone                   ? timezone_place(focused_zone)
             : page == Page::System ? locale.text(system_row_name(settings.focus))
                                    : locale.text(title));
    if (zone) {
        const auto [previous, next] = timezone_neighbours();
        if (!previous.empty()) shoulder_tab(r, y, false, "L", previous);
        if (!next.empty()) shoulder_tab(r, y, true, "R", next);
    }
    if ((page == Page::GameArtwork || (page == Page::Keyboard && edit_field == 3)) && !model.games.empty()) {
        auto name = game_title(model.selected);
        int name_width = std::min(576, label_width(name, 20));
        label(r, name, 320 - name_width / 2, y + 318, 20, "ink", 576);
    }
    if (page == Page::Network || page == Page::Wifi) network_status_card(r, y);
    if (settings.page == Page::Controls && control_capture) {
        const std::string prompt = "Press a button for " + (capture_logical == 12 ? std::string("fast-forward")
                                                                                  : control_title(capture_logical));
        label(r, prompt, 320 - label_width(prompt, 20) / 2, y + 320, 20, "ink", 584);
        key_hints(r, 320, y + 363, {{"Anbernic", "Cancel"}}, 0);
    }
    // Operational feedback remains visible, without permanent advice cards.
    std::string message;
    if (o.game_menu && (menu_status["state"] == "busy" || menu_status["state"] == "error"))
        message = menu_status["message"];
    else if (picker_open && picker_core_offset >= 0) message = emulator_choice_hint();
    else if (picker_open && picker_title == "Standard screen order")
        message = "Standard DraStic only. Applies on the next launch; game saves are kept.";
    else if (picker_open && picker_title == "DraStic backend")
        message = "SuperDrastic uses its own pause menu and display path. Experimental; RG DS testing is pending.";
    else if (status.rfind("Could not save", 0) == 0) message = status;
    else if (settings.page == Page::GameOptions && settings.focus == 2 && superdrastic_game())
        message = "SuperDrastic currently uses native output. Your standard shader choice is kept.";
    else if (settings.page == Page::GameOptions && settings.focus == 7 && superdrastic_game())
        message =
            "Gengis is experimental. 3x smooths 3D into a 2x picture and costs more CPU; some effects may differ.";
    else if (settings.page == Page::Graphics) {
        static const char* hints[] = {
            "True 3D resolution. 3x balances quality and speed; 5x is experimental.",
            "Smooth combines 3D samples at the panel resolution. 2D artwork stays crisp.",
            "Adaptive renders heavy 3D scenes at native for a few seconds to hold 60 fps. Applies during play.",
            "Resamples 2D art, text and menus. Sharp, Smooth and Crisp: the 3D screen at 2x and above. HD and HD smooth rebuild both screens, sharp at any resolution and under any screen filter; HD smooth also smooths shading.",
            "Screen filters change the final picture, including 2D artwork.",
            "Keep off for smooth motion. Skipping display frames can help demanding games.",
            "The renderer currently running. Graphics changes apply during play.",
            "Save a checkpoint and restart with your chosen graphics settings."};
        message = status.empty() ? hints[std::clamp(settings.focus, 0, 7)] : status;
    } else if (settings.page == Page::EmulatorOptions) {
        int engine = active_engine(), next = game_preferences.core < 0 ? int(preferences.core) : game_preferences.core;
        if (settings.focus == list_count() - 1)
            message = "Restart from the title screen. In-game saves and save slots are kept.";
        else if (settings.focus == list_count() - 2) message = "Microphone, simulated blowing, lid and DS language.";
        else if (settings.focus == 0)
            message = engine != 0 ? "Counts emulated game frames. Changes immediately."
                                  : "Counts display updates, not emulation speed. Changes immediately.";
        else if (settings.focus == 1) message = "Changes immediately. Map a button in Controls for quick access.";
        else if (settings.focus == 2) message = "Applies immediately. Higher clocks use more battery.";
        else if (settings.focus == 3)
            message = engine == 1   ? "3D rendering automatically uses worker threads."
                      : engine == 2 ? "This engine does not expose a threading switch."
                      : engine == 3 ? "Applies immediately. Your game keeps its progress."
                                    : "Save & restart applies Threaded 3D.";
        else if (settings.focus == 4 && experimental_game())
            message = "Higher resolution costs GPU time. Changes apply during play; Native uses software rendering.";
        else if (settings.focus == 4)
            message = emulator_option_enabled(4)
                          ? "Native is faster; 2x uses more GPU time. Save & restart applies it."
                          : "This emulator uses native resolution. No resolution switch is available.";
        else if (settings.focus == 5)
            message = emulator_option_enabled(5)
                          ? "Off gives smooth motion. Skipping frames can help speed. Restart to apply."
                          : "This emulator does not expose a frameskip setting.";
        else if (settings.focus == 9)
            message = "Sets Standard DraStic screen order for this game. Save & restart applies it.";
        else if (settings.focus == 8)
            message = "Choose the display backend for the next DraStic launch. DSperate stays your default.";
        else if (settings.focus == 6 || next != engine)
            message =
                "Restart uses " + std::string(core_names[next]) + ". Saves and checkpoints stay separate per emulator.";
        else message = "Save & restart keeps your checkpoint and applies pending settings.";
    } else if (settings.page == Page::Controls && controls_notice.rfind("Could not", 0) == 0) message = controls_notice;
    else if (settings.page == Page::Controls && settings.focus == 0 && model.games.empty())
        message = "Per-game controls appear once a game is in your library.";
    else if (settings.page == Page::LibraryOptions) {
        if (backup_requested && worker_action == "device.library") {
            if (worker > 0) message = "Backing up your saves…";
            else if (device_status["state"] == "error")
                message = device_status["message"].empty() ? "The backup did not finish. Please try again."
                                                           : device_status["message"];
            else if (!device_status["message"].empty()) message = device_status["message"];
        }
        if (message.empty()) {
            const int row = library_row(settings.focus);
            if (row == 10) message = "Looks through your games card again. It also runs by itself when games change.";
            else if (row == 9) message = "Adds Game Boy and Game Boy Advance games: Games then opens a Systems page.";
            else if (row == 8) message = "Copies every save and checkpoint into one zip in userdata/exports.";
            else if (row == 7) message = "Where games go on the card, and a fresh scan.";
            else if (row == 1) message = "Where covers and titles come from.";
        }
    } else if (page == Page::System || page == Page::Network || page == Page::Wifi || page == Page::Bluetooth ||
               page == Page::Maintenance) {
        if (device_status["state"] == "error") message = device_status["message"];
    } else if (page == Page::Scraper || page == Page::GameArtwork) {
        if (service["state"] == "scraping") message = service["completed"] + " / " + service["total"] + " games";
        else if (!service_notice.empty()) message = service_notice;
        else message = page == Page::GameArtwork ? artwork_result["message"] : service["message"];
    }
    if (message.empty())
        message =
            zone ? offset_text(zone_offset(focused_zone)) + "  ·  " + zone_time(focused_zone) : settings_tip(page);
    if (!message.empty()) centered_wrapped(r, locale.text(message), note_y + 196, 20, 528, 3, "ink");
}

void App::network_status_card(SDL_Renderer* r, int y) {
    const bool connected = device_status["wifi_connected"] == "1", radio = device_status["wifi"] == "1",
               busy = network_busy(), failed = !busy && device_status["state"] == "error";
    // wpa_supplicant joining or looking for a saved network by itself (at boot, after Wi-Fi on): runtime/wpa_wifi.py
    const bool joining = !busy && !connected && radio && !device_status["wifi_joining"].empty(),
               looking = !busy && !connected && !joining && radio && device_status["wifi_looking"] == "1";
    const int left = 40, top = y + 296, width = 560, height = 116;
    const Color green = preferences.dark ? Color{36, 128, 76} : Color{44, 160, 92}, white{255, 255, 255};
    const Color tone = busy || joining ? theme.at("accent") : green;
    const bool lit = busy || connected || joining;
    alpha_round(r, left, top + 4, width, height, 16, shadow_colour(), preferences.dark ? 60 : 22);
    if (lit) glossy_pill(r, left, top, width, height, 16, tone);
    else {
        round_rect(r, left, top, width, height, 16, "line");
        round_rect(r, left + 1, top + 1, width - 2, height - 2, 15, failed ? "focus" : "panel");
    }
    const Color ink = lit ? white : theme.at("ink"), soft = lit ? blend(white, tone, 22) : theme.at("muted");
    // The badge on the left: a spinner, signal bars, or the radio glyph.
    const int bx = left + 54, by = top + height / 2;
    fill_round(r, bx - 30, by - 30, 60, 60, 30, lit ? blend(tone, white, 18) : theme.at("slot"));
    if (busy || joining || looking)
        feedback_arc(r, bx, by, 19, preferences.animations ? (s.GetTicks() % 1400) / 1400.0 : 0, .7,
                     lit ? white : theme.at("accent"), 255, 3);
    else if (connected) {
        int signal = device_status["wifi_signal"].empty() ? -1 : std::atoi(device_status["wifi_signal"].c_str());
        if (probed_signal >= 0) signal = probed_signal;
        const int level = signal >= 67 ? 3 : signal >= 34 ? 2 : signal > 0 ? 1 : 0;
        for (int i = 0; i < 4; i++) {
            const int h = 8 + i * 6;
            fill_round(r, bx - 16 + i * 9, by + 12 - h, 6, h, 2, i <= level ? white : blend(green, white, 45));
        }
    } else {
        draw_settings_icon(s, r, SettingsIcon::Wifi, bx, by, 30, theme.at(radio ? "accent" : "muted"));
        if (!radio) {
            color(r, "muted");
            for (int w = 0; w < 3; w++) s.RenderDrawLine(r, bx - 16 + w, by - 16, bx + 14 + w, by + 16);
        }
    }
    const int tx = left + 104;
    std::string headline, detail, extra;
    bool translate_headline = true;
    if (busy) {
        headline = worker_action == "device.scan"                                             ? "Looking for networks…"
                   : worker_action == "device.wifi-on"                                        ? "Turning Wi-Fi on…"
                   : worker_action == "device.bt-on"                                          ? "Turning Bluetooth on…"
                   : worker_action == "device.bt-off"                                         ? "Turning Bluetooth off…"
                   : worker_action == "device.bt-scan"                                        ? "Looking for devices…"
                   : worker_action == "device.bt-pair" || worker_action == "device.bt-forget" ? "Working…"
                   : worker_action == "device.homebrew-get"                                   ? "Downloading…"
                   : worker_action == "device.wifi-off"
                       ? "Turning Wi-Fi off…"
                       : locale.text("Connecting to") + " " +
                             (network_id.empty() ? device_status["connection"] : network_id) + "…";
        translate_headline = worker_action != "device.connect";
        detail = locale.text("This can take a few seconds");
    } else if (connected) {
        headline = device_status["connection"];
        translate_headline = false;
        detail = device_status["ip"].empty() ? locale.text("Waiting for an address…")
                                             : locale.text("IP address") + "  " + device_status["ip"];
        int signal = device_status["wifi_signal"].empty() ? -1 : std::atoi(device_status["wifi_signal"].c_str());
        if (probed_signal >= 0) signal = probed_signal;
        if (signal >= 0) extra = locale.text("Signal") + "  " + std::to_string(signal) + "%";
    } else if (joining) {
        headline = locale.text("Connecting to") + " " + device_status["wifi_joining"] + "…";
        translate_headline = false;
        detail = locale.text("This can take a few seconds");
    } else if (failed) {
        headline = "Could not connect";
        detail = device_status["message"];
    } else if (!radio) {
        headline = "Wi-Fi is off";
        detail = device_status["wifi_supported"] == "0" ? device_status["wifi_reason"]
                                                        : locale.text("Turn it on to find networks");
    } else if (looking) {
        headline = "Looking for networks…";
        detail = locale.text("Choose a network from the list");
    } else {
        headline = "Not connected";
        detail = locale.text("Choose a network from the list");
    }
    raw_label_colour(r, translate_headline ? locale.text(headline) : headline, tx, top + 22, 26, ink, width - 120);
    raw_label_colour(r, detail, tx, top + 60, 18, soft, width - 120);
    if (!extra.empty()) raw_label_colour(r, extra, tx, top + 86, 16, soft, width - 120);
}

void App::settings_bottom(SDL_Renderer* r, int y) {
    rect(r, 0, y, 640, 480, "background");
    pattern(r, y, 480, true);
    if (o.game_menu && menu_status["state"] == "busy") {
        feedback_arc(r, 320, y + 195, 30, preferences.animations ? (s.GetTicks() % 1600) / 1600.0 : 0, .72,
                     theme.at("accent"), 255, 3);
        ui_icon(r, SettingsIcon::Save, 320, y + 195, 25, "accent");
        wrapped(r, menu_status["message"].empty() ? "Saving your progress…" : menu_status["message"], 72, y + 257, 26,
                496, 3);
        centered(r, "Please keep your handheld on", y + 371, 1, "muted");
        return;
    }
    if (settings.page == Page::GameInfo) {
        game_info_bottom(r, y);
        return;
    }
    if (settings.page == Page::Statistics) {
        settings_header(r, y, "Most played");
        if (!summary_ready) wrapped(r, "Preparing your statistics…", 32, y + 126, 24, 560, 2);
        else if (summary.most_played.empty())
            wrapped(r, "Play a game to start your statistics.", 32, y + 126, 24, 560, 3);
        else
            for (size_t i = 0; i < summary.most_played.size(); i++) {
                auto& entry = summary.most_played[i];
                int row = y + 100 + int(i) * 64;
                alpha_round(r, 28, row + 3, 584, 60, 10, shadow_colour(), preferences.dark ? 50 : 16);
                round_rect(r, 28, row, 584, 60, 10, "panel");
                // Gold, silver and bronze for the top three; a plain number after that.
                static const std::array<Color, 3> medals = {Color{236, 178, 48}, Color{168, 180, 192},
                                                            Color{196, 128, 78}};
                if (i < 3) {
                    fill_round(r, 40, row + 16, 28, 28, 14, medals[i]);
                    alpha_round(r, 45, row + 19, 18, 8, 4, Color{255, 255, 255}, 90);
                    const std::string n = std::to_string(i + 1);
                    raw_label_colour(r, n, 54 - label_width(n, 16, false) / 2, centred_label_y(n, 16, row + 30), 16,
                                     Color{255, 255, 255}, 24);
                } else raw_label(r, std::to_string(i + 1), 42, row + 19, 20, "muted", 28);
                game_icon(r, entry.game, 76, row + 6, 48, i);
                raw_label(r, entry.title, 140, row + 9, 20, "ink", 286);
                auto time =
                    std::to_string(entry.seconds / 3600) + "h " + std::to_string((entry.seconds / 60) % 60) + "m";
                raw_label(r, time, 450, row + 10, 18, "accent", 146);
                round_rect(r, 140, row + 43, 286, 4, 2, "slot");
                round_rect(
                    r, 140, row + 43,
                    std::max(3, int(286 * entry.seconds / std::max<uint64_t>(1, summary.most_played[0].seconds))), 4, 2,
                    "accent");
                raw_label(r, std::to_string(entry.sessions) + " ×", 450, row + 34, 14, "muted", 145);
            }
        return;
    }
    if (settings.page == Page::GameSwitcher) {
        sync_list();
        settings_header(r, y, "Game switcher");
        SDL_Rect clip{20, y + list_top, 575, list_height};
        s.RenderSetClipRect(r, &clip);
        for (int i = 0; i < list_count(); i++) {
            int row = y + list_top + i * list_stride - list_scroll;
            if (row + 66 < y + list_top || row > y + list_top + list_height) continue;
            if (i == int(switch_games.size())) {
                list_row(r, y, i, "Game menu");
                continue;
            }
            int x = 24;
            bool selected = settings.focus == i;
            move_element(uint32_t(i), x, row, 566, 66, selected, list_top, list_top + list_height, 24, 590);
            round_rect(r, x, row, 566, 66, 10, selected ? "focus" : "panel");
            if (selected) round_rect(r, x, row + 16, 4, 34, 2, "accent");
            game_icon(r, switch_games[i], x + 14, row + 9, 48);
            raw_label(r, library_title(switch_games[i].title), x + 76, row + 6, 22, "ink", 464);
            // In a game the first is the one playing; from the launcher every row is a recent game, by system.
            label(r,
                  home_switcher ? std::string(system_info(switch_games[i].system).name)
                  : i == 0      ? "Resume"
                                : "Switch game",
                  x + 76, row + 38, 14, selected ? "accent" : "muted", 464);
        }
        s.RenderSetClipRect(r, nullptr);
        list_scrollbar(r, y);
        return;
    }
    if (settings.page == Page::ShaderPicker) {
        render_shader_carousel(r, y);
        return;
    }
    sync_list();
    int count = list_count();
    // Pages that keep their header and controls while their list is empty.
    if (count > 0 || settings.page == Page::Wifi || settings.page == Page::Bluetooth ||
        settings.page == Page::Homebrew || settings.page == Page::Cheats) {
        std::string title = settings.page == Page::Graphics || settings.page == Page::PlayGraphics ? "Graphics"
                            : settings.page == Page::Cheats                                        ? cheats_title()
                            : settings.page == Page::Sound                                         ? "Sound"
                            : settings.page == Page::MenuSounds                                    ? "Sounds & music"
                            : settings.page == Page::Haptics                                       ? "Vibration"
                            : settings.page == Page::SaveGallery                                   ? gallery_title()
                            : settings.page == Page::RomSetup                                      ? "Add your games"
                            : settings.page == Page::PortFiles                                     ? "Add game files"
                            : settings.page == Page::DSControls                                    ? "DS controls"
                            : settings.page == Page::Settings                                      ? "Settings"
                            : settings.page == Page::Appearance                                    ? "Theme"
                            : settings.page == Page::DisplayOptions                                ? "Display"
                            : settings.page == Page::Power                                         ? "Power"
                            : settings.page == Page::LibraryOptions                                ? "Library"
                            : settings.page == Page::GameOptions                                   ? "Game options"
                            : settings.page == Page::System                                        ? "System"
                            : settings.page == Page::Network                                       ? "Connectivity"
                            : settings.page == Page::Wifi            ? "Available networks"
                            : settings.page == Page::Bluetooth       ? "Bluetooth devices"
                            : settings.page == Page::Homebrew        ? "Homebrew"
                            : settings.page == Page::Timezone        ? "Timezone"
                            : settings.page == Page::ShaderPicker    ? "Shaders"
                            : settings.page == Page::EmulatorOptions ? "Emulator settings"
                            : (settings.page == Page::Scraper || settings.page == Page::GameArtwork) ? "Artwork"
                            : settings.page == Page::DeviceInfo ? "Device information"
                            : settings.page == Page::Controls   ? "Controls"
                                                                : "Updates & recovery";
        if (settings.page == Page::Timezone) title = timezone_title();
        settings_header(r, y, title,
                        settings.page == Page::Cheats     ? cheat_tabs_left() - 88 - 16
                        : settings.page == Page::Homebrew ? 616 - int(homebrew_shelves().size()) * 72 - 88 - 16
                        : settings.page == Page::Timezone ? 600 - 88
                                                          : 475,
                        settings.page != Page::Cheats); // group names are data
        if (settings.page == Page::Cheats) cheat_header(r, y);
        if (settings.page == Page::Homebrew) {
            // the shelves as tabs, L and R between them (the Cheats page's strip)
            std::vector<std::string> names;
            int selected = 0;
            for (const auto& id : homebrew_shelves()) {
                if (id == homebrew_system) selected = int(names.size());
                names.push_back(id == "nds" ? "DS" : id == "gba" ? "GBA" : "NES");
            }
            tab_strip(r, 616 - int(names.size()) * 72, y + 77, 72, names, selected);
        }
        if (settings.page == Page::Wifi || settings.page == Page::Bluetooth) {
            secondary_button(r, 476, y + 24, 150, 42, 12,
                             pointer_down && contact_x >= 476 && contact_y >= 24 && contact_y <= 66);
            ui_icon(r, SettingsIcon::Refresh, 498, y + 45, 18, "accent");
            label(r, "Scan", 518, y + 34, 20, "accent", 91);
        }
        if (settings.page == Page::ShaderPicker) {
            secondary_button(r, 368, y + 24, 120, 42, 12,
                             pointer_down && contact_x >= 368 && contact_x <= 488 && contact_y >= 24 &&
                                 contact_y <= 66);
            ui_icon(r, SettingsIcon::Art, 386, y + 45, 18, "accent");
            label(r, "Scene", 403, y + 35, 17, "ink", 74);
            secondary_button(r, 496, y + 24, 128, 42, 12,
                             pointer_down && contact_x >= 496 && contact_y >= 24 && contact_y <= 66);
            ui_icon(r, SettingsIcon::Shader, 514, y + 45, 18, "accent");
            label(r, "Compare", 530, y + 35, 16, "ink", 85);
        }
        SDL_Rect clip{20, y + list_top, 575, list_height};
        s.RenderSetClipRect(r, &clip);
        for (int i = 0; i < count; i++) {
            if (i * list_stride - list_scroll + 66 < 0 || i * list_stride - list_scroll > list_height) continue;
            if (settings.page == Page::Settings)
                list_row(r, y, i, sections[size_t(section_order[size_t(i)])].title,
                         section_order[size_t(i)] == 9 ? update_badge() : "");
            else if (settings.page == Page::Appearance || settings.page == Page::DisplayOptions)
                appearance_row(r, y, i);
            else if (settings.page == Page::ShaderPicker) {
                int value = shader_choices[i];
                list_row(r, y, i, shader_name(value),
                         (shader_per_game ? game_preferences.filter : preferences.filter) == value ? "Selected" : "");
            } else if (settings.page == Page::Power) {
                // indexed by control id; power_rows gives the order on the page
                const std::array<std::string, 9> names = {
                    "Power mode", "Menu idle sleep", "Sleep on lid close", "Battery percentage",  "Sleep now",
                    "Power off",  "Restart",         "Dim when idle",      "Sleep timer in games"};
                const std::array<std::string, 9> values = {power_names[system_preferences.power],
                                                           idle_name(system_preferences.idle),
                                                           system_preferences.lid ? "On" : "Off",
                                                           system_preferences.battery_percent ? "On" : "Off",
                                                           "Sleep",
                                                           "Power off",
                                                           "Restart",
                                                           dim_name(system_preferences.dim),
                                                           game_sleep_name(system_preferences.game_sleep)};
                const std::array<bool, 9> enabled = {hardware.power,        hardware.sleep,      hardware.lid,
                                                     hardware.battery >= 0, hardware.sleep,      hardware.poweroff,
                                                     hardware.poweroff,     hardware.brightness, hardware.sleep};
                const int id = power_rows[size_t(i)];
                list_row(r, y, i, names[id], enabled[id] ? values[id] : "Unavailable", enabled[id]);
            } else if (settings.page == Page::LibraryOptions) {
                const std::array<std::string, 11> names = {"Default emulator",  "Artwork sources", "Auto-load state",
                                                           "Auto-save on exit", "Default shader",  "Browser layout",
                                                           "DraStic backend",   "Add your games",  "Back up all saves",
                                                           "Systems",           "Scan for games"};
                const std::array<std::string, 11> values = {
                    core_names[preferences.core],
                    "",
                    preferences.auto_load ? "On" : "Off",
                    preferences.auto_save ? "On" : "Off",
                    shader_name(preferences.filter),
                    preferences.layout == 0   ? "Cartridges"
                    : preferences.layout == 1 ? "Covers"
                                              : "List",
                    drastic_backend_name(preferences.drastic_backend),
                    "",
                    "",
                    preferences.show_systems ? "All systems" : "DS only",
                    !library_ready
                        ? locale.text("Scanning…")
                        : locale.text(std::to_string(all_games.size()) + (all_games.size() == 1 ? " game" : " games"))};
                list_row(r, y, i, names[size_t(library_row(i))], values[size_t(library_row(i))]);
            } else if (settings.page == Page::PlayGraphics) {
                const bool palette = play_colour_kind() == 1;
                const std::array<std::string, 5> names = {"Game screen", "Second screen", "Frame", "Shader",
                                                          "Screen colours"};
                const std::array<std::string, 5> values = {
                    play_fit_name(play_fit), play_second_name(play_second), play_frame_label(play_frame),
                    // Default is this handheld's own screen: name it.
                    shader_name(game_preferences.filter < 0 ? default_shader_for(picker_game_system().c_str(), -1)
                                                            : game_preferences.filter),
                    palette ? play::gb_palettes[play_palette].name : play::gb_corrections[play_correction].name};
                // A palette's row wears its second-lightest colour, so the choice shows at a glance.
                const uint32_t tone = play::gb_palettes[play_palette].shade[1];
                const Color dot{uint8_t(tone >> 16), uint8_t(tone >> 8), uint8_t(tone)};
                list_row(r, y, i, names[size_t(i)], values[size_t(i)], true, palette && i == 4 ? &dot : nullptr);
            } else if (settings.page == Page::GameOptions && i == game_options_base())
                list_row(r, y, i, "Remove game", ""); // only listed when game_removable()
            else if (settings.page == Page::GameOptions && other_system_selected() &&
                     (i == 1 || i == game_options_base() - 1))
                list_row(r, y, i, i == 1 ? "Emulator" : "DS controls", i == 1 ? core_names[play_core] : "Unavailable",
                         false);
            else if (settings.page == Page::GameOptions && i == game_options_base() - 3) {
                const bool have = !model.games.empty() &&
                                  !library_cache.metadata(model.games[model.selected]).info["info_version"].empty();
                list_row(r, y, i, "Game info", have ? "Available" : "Not downloaded");
            } else if (settings.page == Page::GameOptions && i >= game_options_base() - 2)
                list_row(r, y, i, i == game_options_base() - 2 ? "Save gallery" : "DS controls");
            else if (settings.page == Page::GameOptions && i == 5 && experimental_game())
                list_row(r, y, i, "3D resolution", experimental_resolution_name());
            else if (settings.page == Page::GameOptions) {
                const std::array<std::string, 8> names = {"Favourite",    "Emulator",     "Shader",
                                                          "Controls",     "Artwork",      "DraStic backend",
                                                          "Screen order", "3D resolution"},
                                                 values = {
                                                     game_preferences.favourite ? "On" : "Off",
                                                     game_preferences.core < 0 ? "Default"
                                                                               : core_names[game_preferences.core],
                                                     superdrastic_game() ? "Native output"
                                                                         : shader_name(game_preferences.filter),
                                                     "",
                                                     "",
                                                     drastic_backend_name(game_preferences.drastic_backend),
                                                     game_preferences.drastic_screen_swap == 1 ? "Swapped" : "Normal",
                                                     superdrastic_resolution_name()};
                list_row(r, y, i, names[i], values[i], (i != 2 && i != 6) || !superdrastic_game());
            } else if (settings.page == Page::Controls) list_row(r, y, i, controls_row_name(i), controls_row_value(i));
            else if (settings.page == Page::Sound) {
                if (i == 0) volume_row(r, y);
                else {
                    const bool on = i == 4 ? preferences.menu_haptics || preferences.game_rumble
                                           : preferences.menu_sounds || preferences.menu_music;
                    list_row(r, y, i,
                             i == 4   ? "Vibration"
                             : i == 3 ? "Sounds & music"
                             : i == 2 ? "Test microphone"
                                      : (hardware.volume_percent == 0 ? "Unmute" : "Mute"),
                             i >= 3 ? (on ? "On" : "Off") : "", true, nullptr, false);
                }
            } else if (settings.page == Page::MenuSounds) {
                const std::array<std::string, 5> names = {"Menu effects", "Effects volume", "Menu music",
                                                          "Music volume", "Preview sound"};
                list_row(r, y, i, names[i],
                         i == 0   ? (preferences.menu_sounds ? "On" : "Off")
                         : i == 1 ? std::to_string(preferences.sound_volume) + "%"
                         : i == 2 ? (preferences.menu_music ? "On" : "Off")
                         : i == 3 ? std::to_string(preferences.music_volume) + "%"
                                  : "");
            } else if (settings.page == Page::Haptics) {
                const std::array<std::string, 5> names = {"Menu haptics", "Haptic strength", "Game rumble",
                                                          "Rumble strength", "Try a pattern"};
                list_row(r, y, i, names[size_t(i)],
                         i == 0   ? (preferences.menu_haptics ? "On" : "Off")
                         : i == 1 ? std::to_string(preferences.haptic_strength) + "%"
                         : i == 2 ? (preferences.game_rumble ? "On" : "Off")
                         : i == 3 ? std::to_string(preferences.rumble_strength) + "%"
                                  : "",
                         (i != 1 || preferences.menu_haptics) && (i != 3 || preferences.game_rumble) &&
                             (i != 4 || preferences.menu_haptics));
            } else if (settings.page == Page::System) {
                list_row(r, y, i, system_row_name(i),
                         i == 0   ? locale.name()
                         : i == 1 ? timezone_summary()
                         : i == 2 ? dst_names.at(system_preferences.dst)
                         : i == 3 ? date_format_names.at(system_preferences.date_format)
                         : i == 4 ? time_format_names.at(system_preferences.time_format)
                         : i == (o.rgds_os ? 7 : 8) ? (device_status["boot_warning"] == "1" ? "On" : "Off")
                                  : "");
            } else if (settings.page == Page::SaveGallery)
                list_row(r, y, i, locale.text("Slot") + " " + std::to_string(i + 1),
                         slot_occupied(i) ? slots["slot_" + std::to_string(i)] : "Empty slot", slot_primary_enabled(i));
            else if (settings.page == Page::RomSetup) {
                const std::array<std::string, 3> names = {"Where to put games", "Scan for games", "Done"};
                const size_t found = model.games.size();
                list_row(r, y, i, names[i],
                         i == 1
                             ? (!library_ready  ? locale.text("Scanning…")
                                : setup_scanned ? locale.text(std::to_string(found) + (found == 1 ? " game" : " games"))
                                                : std::string())
                             : std::string());
            } else if (settings.page == Page::PortFiles) port_files_row(r, y, i);
            else if (settings.page == Page::Cheats) cheat_row(r, y, i);
            else if (settings.page == Page::DSControls) ds_controls_row(r, y, i);
            else if (settings.page == Page::Network) {
                const std::array<std::string, 7> names = {"Wi-Fi",
                                                          "Available networks",
                                                          "Bluetooth",
                                                          "Bluetooth devices",
                                                          "Refresh connection",
                                                          "Network file sharing",
                                                          "Sharing details"};
                const bool bt_working = worker > 0 && worker_action.rfind("device.bt-", 0) == 0;
                std::string value = i == 0   ? (device_status["wifi_supported"] == "0" ? "Unavailable"
                                                : device_status["wifi"] == "1"         ? "On"
                                                                                       : "Off")
                                    : i == 1 ? (network_busy()                           ? "Working…"
                                                : device_status["wifi_connected"] == "1" ? device_status["connection"]
                                                : !device_status["wifi_joining"].empty() ? "Connecting…"
                                                                                         : "")
                                    : i == 2 ? (device_status["bt_supported"] == "0" ? "Unavailable"
                                                : bt_working                         ? "Working…"
                                                : device_status["bt"] == "1"         ? "On"
                                                                                     : "Off")
                                    : i == 3 ? (device_status["bt_supported"] == "0" ? "" : device_status["bt_count"])
                                    : i == 5 ? (device_status["sharing"] != "1"           ? "Off"
                                                : device_status["sharing_mode"] == "open" ? "Open"
                                                                                          : "Password")
                                    : i == 6 ? "SMB"
                                             : "";
                list_row(r, y, i, names[i], value,
                         i == 2 || i == 3 ? device_status["bt_supported"] != "0"
                                          : i != 6 || device_status["sharing"] == "1");
            } else if (settings.page == Page::Wifi) {
                auto prefix = "network_" + std::to_string(i) + "_";
                const bool joining =
                    worker > 0 && worker_action == "device.connect" && device_status[prefix + "id"] == network_id;
                list_row(r, y, i, device_status[prefix + "ssid"],
                         joining ? "Connecting…"
                         : device_status[prefix + "connected"] == "1"
                             ? "Connected"
                             : (device_status[prefix + "saved"] == "1" ? "Saved · " : "") +
                                   device_status[prefix + "signal"] + "%");
            } else if (settings.page == Page::Homebrew) {
                list_row(r, y, i, device_status["hb_" + std::to_string(i) + "_title"], homebrew_row_value(i));
            } else if (settings.page == Page::Bluetooth) {
                auto prefix = "bt_" + std::to_string(i) + "_";
                const bool working = worker > 0 &&
                                     (worker_action == "device.bt-pair" || worker_action == "device.bt-forget") &&
                                     device_status[prefix + "address"] == bluetooth_address;
                list_row(r, y, i, device_status[prefix + "name"],
                         working                                      ? "Working…"
                         : device_status[prefix + "connected"] == "1" ? "Connected"
                         : device_status[prefix + "paired"] == "1"    ? "Paired"
                                                                      : "");
            } else if (settings.page == Page::Timezone) {
                const std::string zone = timezones[size_t(i)];
                const auto offset = offset_text(zone_offset(zone));
                list_row(r, y, i, timezone_place(zone),
                         system_preferences.timezone == zone ? locale.text("Selected") + " · " + offset
                                                             : offset + " · " + zone_time(zone));
            } else if (settings.page == Page::Maintenance) {
                bool busy = worker > 0 && worker_action.rfind("device.update-", 0) == 0;
                const std::array<std::string, 7> names = {"Check for updates",
                                                          update_status["prepared"] == "1"        ? "Restart & install"
                                                          : update_stage() == UpdateStage::Paused ? "Resume download"
                                                                                                  : "Download update",
                                                          busy ? "Pause download" : "Release notes",
                                                          "Use previous build",
                                                          "Clear temporary cache",
                                                          update_status["prepared"] == "1" ? "Discard update"
                                                                                           : "Save device report",
                                                          "Automatic updates"};
                bool enabled = !busy || (i == 2 && worker_action == "device.update-download");
                if (i == 1)
                    enabled = enabled && (update_status["state"] == "available" || update_status["prepared"] == "1" ||
                                          update_status["state"] == "cancelled" || update_status["state"] == "error");
                list_row(r, y, i, names[i],
                         i == 0   ? (update_status["channel"] == "testing" ? "Testing" : "Stable")
                         : i == 1 ? update_status["version"]
                         : i == 6 ? experience["auto_update"] == "0"   ? "Off"
                                    : experience["auto_update"] == "2" ? "Download automatically"
                                                                       : "Check automatically"
                                  : "",
                         enabled);
            } else if (settings.page == Page::GameArtwork) {
                const std::array<std::string, 3> names = {
                    "Matching title", worker > 0 && worker_action == "scrape" ? "Stop fetching" : "Fetch artwork",
                    "Use ROM identity"};
                list_row(r, y, i, names[i], i == 0 ? (artwork_match.empty() ? "Automatic" : artwork_match) : "");
            } else if (settings.page == Page::Scraper) {
                const std::array<std::string, 3> providers = {"Automatic", "GameTDB", "Libretro"};
                const std::array<std::string, 4> pictures = {"Off", "Screenshots", "Title screens", "Both"};
                const std::array<std::string, 5> names = {
                    "Artwork source",
                    worker > 0 && worker_action == "scrape" ? "Stop scraping" : "Fetch missing artwork",
                    "Review unmatched", "Game info", "Extra pictures"};
                list_row(r, y, i, names[i],
                         i == 0   ? providers.at(preferences.artwork_provider)
                         : i == 3 ? (preferences.game_info ? "On" : "Off")
                         : i == 4 ? pictures.at(size_t(std::clamp(preferences.extra_pictures, 0, 3)))
                                  : "");
            } else if (settings.page == Page::DeviceInfo) list_row(r, y, i, device_info_name(i), device_info_value(i));
            else if (settings.page == Page::Graphics) {
                const std::array<std::string, 8> names = {"3D resolution", "3D edge smoothing", "Heavy 3D scenes",
                                                          "2D scaling",    "Screen filter",     "Frameskip",
                                                          "Running now",   "Save & restart"};
                const std::array<std::string, 8> values = {experimental_resolution_name(),
                                                           game_preferences.rgds_smooth ? "Smooth" : "Sharp",
                                                           game_preferences.rgds_adaptive ? "Adaptive" : "Always sharp",
                                                           filter2d_name(game_preferences.rgds_2d),
                                                           shader_name(game_preferences.filter),
                                                           game_preferences.frameskip > 0
                                                               ? std::to_string(game_preferences.frameskip) + " frames"
                                                               : "Off (smooth)",
                                                           active_graphics_name(),
                                                           ""};
                list_row(r, y, i, names[i], values[i], i != 6);
            } else if (settings.page == Page::EmulatorOptions && i >= count - 2)
                list_row(r, y, i, i == count - 2 ? "DS controls" : "Reset state");
            else if (settings.page == Page::EmulatorOptions) {
                int engine = active_engine();
                const std::array<std::string, 10> names = {engine != 0 ? "Game FPS" : "Display FPS",
                                                           "Fast-forward",
                                                           "Power profile",
                                                           "Threaded 3D",
                                                           "3D resolution",
                                                           "Frameskip",
                                                           "Next emulator",
                                                           "Save & restart",
                                                           "DraStic backend",
                                                           "Screen order"};
                bool enabled = emulator_option_enabled(i);
                const std::array<std::string, 10> values = {
                    menu_status["fps"] == "1" ? "On" : "Off",
                    menu_status["fast_forward"] == "1" ? "On" : "Off",
                    power_names[system_preferences.power],
                    (engine == 1 || engine == 4)     ? "Automatic"
                    : engine == 2                    ? "Engine managed"
                    : engine == 3                    ? (menu_status["threaded"] == "0" ? "Off" : "On")
                    : game_preferences.threaded == 0 ? "Off"
                                                     : "On",
                    experimental_game()           ? experimental_resolution_name()
                    : superdrastic_game()         ? superdrastic_resolution_name()
                    : game_preferences.hires == 1 ? "2x"
                                                  : "Native",
                    game_preferences.frameskip > 0 ? std::to_string(game_preferences.frameskip) + " frames"
                                                   : "Off (smooth)",
                    game_preferences.core < 0 ? "Default" : core_names[game_preferences.core],
                    "",
                    drastic_backend_name(game_preferences.drastic_backend),
                    game_preferences.drastic_screen_swap == 1 ? "Swapped" : "Normal"};
                list_row(r, y, i, names[i], enabled || i == 3 ? values[i] : "Unavailable", enabled);
            }
        }
        s.RenderSetClipRect(r, nullptr);
        list_scrollbar(r, y);
        if (settings.page == Page::Wifi && !count)
            wrapped(r,
                    worker > 0                              ? "Looking for networks…"
                    : !device_status["message"].empty()     ? device_status["message"]
                    : !device_status["wifi_reason"].empty() ? device_status["wifi_reason"]
                                                            : "No networks found",
                    30, y + 145, 24, 560, 4);
        if (settings.page == Page::Homebrew && !count) {
            if (homebrew_loading()) busy_ring(r, 320, y + 250, 26, 4);
            else
                wrapped(r, !device_status["message"].empty() ? device_status["message"] : "Nothing on this shelf yet.",
                        30, y + 145, 24, 560, 4);
        }
        if (settings.page == Page::Bluetooth && !count)
            wrapped(r,
                    worker > 0                             ? "Looking for devices…"
                    : device_status["bt_supported"] == "0" ? "Bluetooth is not available on this handheld."
                    : device_status["bt"] != "1"           ? "Turn Bluetooth on to find devices."
                    : !device_status["message"].empty() ? device_status["message"]
                                                        : "No devices found. Put the device in pairing mode and scan.",
                    30, y + 145, 24, 560, 4);
        if (settings.page == Page::Cheats && !count)
            wrapped(r,
                    cheat_view == CheatView::On    ? "No cheats are on. Switch one on under All or Groups."
                    : cheat_view == CheatView::All ? "No cheats match. Try a shorter word."
                                                   : "No cheats in this group.",
                    30, y + 145, 24, 560, 4);
        return;
    }
    if (settings.page == Page::QuickMenu) {
        settings_header(r, y, "Game menu");
        bool busy = menu_status["state"] == "busy";
        const std::array<std::string, 12> names = {
            "Resume",          "Save checkpoint",
            "Load checkpoint", experimental_game() || active_engine() == play_core ? "Graphics" : "Shaders",
            "Emulator",        "Sound",
            "Brightness",      "Save & restart",
            "Return home",     "Controls",
            "Game switcher",   "Cheats"};
        static const std::string port_restart = "Restart port"; // ports have no checkpoint to save first
        const std::array<SettingsIcon, 12> symbols = {
            SettingsIcon::Play,    SettingsIcon::Save,       SettingsIcon::Load,    SettingsIcon::Shader,
            SettingsIcon::Sliders, SettingsIcon::Volume,     SettingsIcon::Sun,     SettingsIcon::Restart,
            SettingsIcon::Home,    SettingsIcon::Controller, SettingsIcon::Library, SettingsIcon::Spark};
        // One rule for the page: wide tiles on the top and bottom rows, a 3x3 grid between them,
        // 16 px between tiles. Cheats stays muted outside DSCore or without a database entry.
        for (int i = 0; i < 12; i++) {
            const bool wide = i == 0 || i >= 10;
            int x = i == 0    ? 28
                    : i >= 10 ? (i == 10 ? 28 : 328)
                              : 28 + ((i - 1) % 3) * 200,
                row = y + (i == 0    ? quick_top
                           : i >= 10 ? quick_bottom_top
                                     : quick_grid_top + ((i - 1) / 3) * quick_grid_pitch);
            const int width = i == 0 ? 584 : wide ? 284 : 184, height = wide ? quick_wide_height : quick_tile_height;
            bool enabled = !busy && (i != 11 || cheats_available()) && (!port_game() || port_tile(i)),
                 focused = settings.focus == i;
            move_element(uint32_t(i), x, row, width, height, focused);
            settings_card(r, x, row, width, height, focused);
            // Tiles with their own button wear it in the corner: B resumes, X opens the emulator.
            const std::string hotkey = i == 0 ? "B" : i == 4 ? "X" : "";
            if (!hotkey.empty())
                key_badge(r, x + width - 10 - key_badge_width(hotkey), wide ? row + 26 : row + 17, hotkey, enabled);
            if (wide) {
                round_rect(r, x + 14, row + 8, 36, 36, 11, "panel");
                ui_icon(r, symbols[i], x + 32, row + 26, 28, enabled ? "accent" : "muted");
                const std::string note = i == 11 ? cheats_note() : std::string();
                const int note_width = note.empty() ? 0 : std::min(124, label_width(note, 15)) + 16;
                label(r, names[i], x + 62, row + 14, 20, enabled ? "ink" : "muted", width - 76 - note_width);
                if (!note.empty())
                    label(r, note, x + width - note_width, row + 17, 15, enabled ? "accent" : "muted", note_width - 16);
            } else {
                ui_icon(r, symbols[i], x + width / 2, row + 19, 28, enabled ? "accent" : "muted");
                int measured = std::min(width - 24, label_width(names[i], 16));
                const std::string& name = port_game() && i == 7 ? port_restart : names[i];
                measured = std::min(width - 24, label_width(name, 16));
                label(r, name, x + (width - measured) / 2, row + 37, 16, enabled ? "ink" : "muted", width - 24);
            }
        }
        return;
    }
    if (settings.page == Page::ApplyShader) {
        settings_header(r, y, "Shader saved");
        label(r, shader_name(game_preferences.filter), 30, y + 106, 24, "accent", 560);
        const std::array<std::string, 2> names = {"Save & restart", "Keep playing"};
        for (int i = 0; i < 2; i++) {
            int row = y + 174 + i * 86;
            settings_card(r, 28, row, 584, 66, settings.focus == i);
            ui_icon(r, i == 0 ? SettingsIcon::Refresh : SettingsIcon::Play, 62, row + 33, 25, "accent");
            label(r, names[i], 94, row + 21, 24, "ink", 490);
        }
        return;
    }
    if (settings.page == Page::LeaveGame) {
        settings_header(r, y, "Return home");
        wrapped(r, "The checkpoint did not finish saving.", 30, y + 105, 28, 580, 2);
        const std::array<const char*, 3> names = {"Try saving again", "Resume game", "Return without saving"};
        const std::array<SettingsIcon, 3> symbols = {SettingsIcon::Save, SettingsIcon::Play, SettingsIcon::Home};
        for (int i = 0; i < 3; i++) {
            int row = y + 210 + i * 70;
            settings_card(r, 28, row, 584, 58, settings.focus == i);
            ui_icon(r, symbols[i], 60, row + 29, 24, "accent");
            label(r, names[i], 94, row + 17, 20, "ink", 490);
        }
        return;
    }
    if (settings.page == Page::Keyboard) {
        keyboard_screen(r, y);
        return;
    }
    if (settings.page == Page::Colours) {
        colours_screen(r, y);
        return;
    }
    if (settings.page == Page::Motion) {
        settings_header(r, y, "Animations");
        const std::array<std::string, 2> names = {"Interface animations", "View transitions"};
        const std::array<std::string, 2> hints = {"Small touches, scrolling and startup motion",
                                                  "Animate pages with the selected style"};
        for (int i = 0; i < 2; i++) {
            int row = y + motion_row(i), dx = 28;
            move_element(uint32_t(i), dx, row, 584, 72, settings.focus == i);
            dx -= 28;
            settings_card(r, 28 + dx, row, 584, 72, settings.focus == i);
            ui_icon(r, SettingsIcon::Spark, 56 + dx, row + 22, 22, "accent");
            label(r, names[i], 83 + dx, row + 10, 22, "ink", 410);
            label(r, hints[i], 48 + dx, row + 44, 16, "muted", 480);
            draw_toggle(r, 552 + dx, row + 8, names[i], i == 0 ? preferences.animations : preferences.view_animations);
        }
        // Three-way choices: transition style and speed.
        auto segments = [&](int index, const char* title, const std::array<const char*, 3>& options, int selected,
                            const char* hint) {
            int row = y + motion_row(index), dx = 28;
            move_element(uint32_t(index), dx, row, 584, 72, settings.focus == index);
            dx -= 28;
            settings_card(r, 28 + dx, row, 584, 72, settings.focus == index);
            label(r, title, 48 + dx, row + 12, 22, "ink", 220);
            sunken_track(r, motion_segment_x(0) + dx - 4, row + 4, 336, 42, 13);
            const float slide = segment_position(index, selected);
            raised_pill(r, motion_segment_x(0) + dx + int(std::lround(slide * 112)), row + 8, 104, 34, 10,
                        theme.at("accent"), 0, false);
            for (int i = 0; i < 3; i++) {
                int x = motion_segment_x(i) + dx;
                label(r, options[i], x + 52 - label_width(options[i], 17) / 2,
                      centred_label_y(locale.text(options[i]), 17, row + 25), 17,
                      std::abs(slide - i) < .5f ? "on_accent" : "ink", 96);
            }
            label(r, hint, 48 + dx, row + 47, 15, "muted", 540);
        };
        const int style = preferences.view_style == ViewStyle::Push   ? 1
                          : preferences.view_style == ViewStyle::Zoom ? 2
                                                                      : 0;
        segments(2, "Transition style", {"Spring", "Push", "Zoom"}, style,
                 style == 1   ? "Slide into pages; slide back to return"
                 : style == 2 ? "Pages zoom and fade in, like the DSi menu"
                              : "Springy menu changes with a colourful wobble");
        segments(3, "Speed", {"Snappy", "Normal", "Relaxed"}, preferences.view_speed,
                 preferences.view_speed == 0   ? "Quick, crisp transitions"
                 : preferences.view_speed == 2 ? "Slow, unhurried, very smooth transitions"
                                               : "Balanced for everyday use");
        return;
    }
    if (settings.page == Page::Microphone) {
        microphone_screen(r, y);
        return;
    }
    if (settings.page == Page::Display) {
        const bool available = hardware.brightness;
        const int value = system_preferences.brightness;
        settings_header(r, y, "Screen brightness");
        const auto band = slider_band();
        const int card_top = band.first, card_height = band.second - band.first, track = card_top + 52;
        // Title and value inside the card, the slider below them.
        settings_card(r, 28, y + card_top, 584, card_height, false);
        ui_icon(r, SettingsIcon::Sun, 58, y + card_top + 22, 22, "accent");
        label(r, "Both screens", 82, y + card_top + 8, 22, "ink", 300);
        {
            const std::string shown = available ? std::to_string(value) + "%" : locale.text("Unavailable");
            raw_label(r, shown, 584 - label_width(shown, available ? 26 : 18, false),
                      y + card_top + (available ? 5 : 10), available ? 26 : 18, "accent", 200);
        }
        sunken_track(r, 54, y + track - 2, 532, 14, 7);
        int position = 56 + (value - 5) * 528 / 95;
        if (position - 56 > 8) glossy_pill(r, 56, y + track, std::max(10, position - 56), 10, 5, theme.at("accent"));
        raised_pill(r, std::clamp(position - 17, 39, 567), y + track - 13, 34, 36, 12,
                    theme.at(available ? "accent" : "muted"), brightness_drag ? .6 : 0);
        ui_icon(r, SettingsIcon::Sliders, std::clamp(position, 56, 584), y + track + 5 + (brightness_drag ? 1 : 0), 16,
                "on_accent");
        label(r, "5%", 56, y + track + 22, 14, "muted", 100);
        label(r, "100%", 545, y + track + 22, 14, "muted", 60);
        return;
    }
    if (settings.page == Page::Confirm) {
        settings_header(r, y, "Confirm");
        std::string prompt = confirmation == "startup-rgds" ? "Start RG DS automatically on future boots?"
                             : confirmation == "reboot"     ? "Restart the device?"
                             : confirmation == "return-stock"
                                 ? (o.independent ? "Restart the RGDS menu?" : "Return to the stock menu?")
                             : confirmation == "wifi-off"     ? "Turn Wi-Fi off?"
                             : confirmation == "bt-off"       ? "Turn Bluetooth off?"
                             : confirmation == "bt-forget"    ? "Forget this device?"
                             : confirmation == "remove-game"  ? "Remove this game from the card? Its saves stay."
                             : confirmation == "poweroff"     ? "Power off your handheld?"
                             : confirmation == "update-apply" ? "Restart and install the update?"
                             : confirmation == "rollback"     ? "Use the previous RGDS build?"
                                                              : "Clear temporary game files?";
        settings_card(r, 28, y + 105, 584, 209);
        ui_icon(r, SettingsIcon::Info, 58, y + 136, 24, "accent");
        label(r, "REVIEW YOUR CHOICE", 88, y + 125, 13, "muted", 495);
        wrapped(r, prompt, 52, y + 170, 28, 536, 3);
        choice(r, 28, y + 365, "Cancel", settings.focus == 0, true);
        choice(r, 332, y + 365, "Continue", settings.focus == 1, true);
        return;
    }
    settings_header(r, y,
                    settings.page == Page::MenuSounds ? "Sounds & music"
                    : settings.page == Page::Haptics  ? "Vibration"
                                                      : sections[settings.section].title);
}

std::string App::settings_tip(Page page) {
    if (page == Page::System) {
        static const std::array<const char*, 9> tips = {
            "The language for menus and messages.",
            "Set the clock to where you are.",
            "Summer time follows your timezone. Force it on or off if the clock is an hour out.",
            "How dates appear at the top of the screen.",
            "A 24-hour or 12-hour clock.",
            "Versions, free space and the network address of your handheld.",
            "Check for updates, go back to the previous build or clear temporary files.",
            "Leave RG DS Core and go back to the stock menu.",
            "Show a start screen when the handheld turns on. Tap it or press A to continue."};
        if (settings.focus == 7 && o.rgds_os) return tips[8];
        if (settings.focus == 7 && o.independent) return "Restart the RGDS menu.";
        return tips[size_t(std::clamp(settings.focus, 0, 8))];
    }
    return "";
}
