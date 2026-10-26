#pragma once
// The main loop.
// Members of App in main.cpp; included once, right after the class.

int App::run() {
    open();
    configure_sounds();
    view_motion.seed = ViewMotion::hash(uint32_t(getpid()) ^ uint32_t(std::time(nullptr)));
    for (auto& panel : surfaces) {
        for (const char* name : {"brand/mark-48.bmp", "brand/mark-120.bmp"})
            media.warm(panel.renderer, asset_path(name));
        media.warm(panel.renderer, asset_path(preferences.dark ? "brand/status-dark.bmp" : "brand/status-light.bmp"));
        media.warm(panel.renderer,
                   asset_path(preferences.dark ? "brand/wordmark-dark.bmp" : "brand/wordmark-light.bmp"));
    }
    const auto began = s.GetTicks();
    boot_scene_at = began;
    last_activity = began;
    int frames = 0;
    std::set<int> visited_pages;
    if (!o.boot_startup && !o.game_menu && !o.roots.empty()) start_worker("index");
    if (!o.input_script.empty()) {
        std::ifstream file(o.input_script);
        if (!file) throw std::runtime_error("Cannot read input script");
        std::string line;
        while (std::getline(file, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream record(line);
            Input input{};
            std::string type, key;
            if (!(record >> input.time >> type)) throw std::runtime_error("Invalid preview input script");
            if (type == "key" || type == "key-up") {
                if (!(record >> key)) throw std::runtime_error("Input key missing");
                input.event.type = type == "key" ? SDL_KEYDOWN : SDL_KEYUP;
                input.event.key.keysym.sym = s.GetKeyFromName(key.c_str());
                if (!input.event.key.keysym.sym) throw std::runtime_error("Unknown input key");
            } else if (type == "text") {
                input.event.type = SDL_TEXTINPUT;
                std::string value;
                std::getline(record >> std::ws, value);
                if (value.size() > 31) throw std::runtime_error("Input text too long");
                std::copy(value.begin(), value.end(), input.event.text.text);
            } else if (type == "tap" || type == "click") {
                input.event.type = SDL_MOUSEBUTTONDOWN;
                input.event.button.button = SDL_BUTTON_LEFT;
                if (!(record >> input.event.button.x >> input.event.button.y))
                    throw std::runtime_error("Input coordinates missing");
                input.event.button.y += 480;
                input.event.button.windowID = surfaces[0].id;
            } else if (type == "drag") {
                input.event.type = SDL_MOUSEMOTION;
                input.event.motion.state = SDL_BUTTON_LMASK;
                if (!(record >> input.event.motion.x >> input.event.motion.y))
                    throw std::runtime_error("Input coordinates missing");
                input.event.motion.y += 480;
                input.event.motion.windowID = surfaces[0].id;
            } else if (type == "release") {
                input.event.type = SDL_MOUSEBUTTONUP;
                input.event.button.button = SDL_BUTTON_LEFT;
            } else if (type == "finger-down" || type == "finger-move" || type == "finger-up") {
                input.event.type = type == "finger-down"   ? SDL_FINGERDOWN
                                   : type == "finger-move" ? SDL_FINGERMOTION
                                                           : SDL_FINGERUP;
                input.event.tfinger.touchId = 1;
                input.event.tfinger.fingerId = 1;
                input.event.tfinger.windowID = surfaces[0].id;
                if (!(record >> input.event.tfinger.x >> input.event.tfinger.y))
                    throw std::runtime_error("Finger coordinates missing");
                record >> input.event.tfinger.fingerId;
            } else if (type == "joy-down" || type == "joy-up") {
                input.event.type = type == "joy-down" ? SDL_JOYBUTTONDOWN : SDL_JOYBUTTONUP;
                int button = 0;
                if (!(record >> button) || button < 0 || button > 255)
                    throw std::runtime_error("Joystick button missing");
                input.event.jbutton.button = Uint8(button);
                input.event.jbutton.which = 0;
                input.event.jbutton.state = type == "joy-down" ? SDL_PRESSED : SDL_RELEASED;
            } else if (type == "joy-hat") {
                int value = 0;
                if (!(record >> value) || value < 0 || value > 15)
                    throw std::runtime_error("Joystick direction missing");
                input.event.type = SDL_JOYHATMOTION;
                input.event.jhat.which = 0;
                input.event.jhat.hat = 0;
                input.event.jhat.value = Uint8(value);
            } else if (type == "wheel") {
                input.event.type = SDL_MOUSEWHEEL;
                if (!(record >> input.event.wheel.x >> input.event.wheel.y))
                    throw std::runtime_error("Wheel delta missing");
            } else if (type == "focus-lost") {
                input.event.type = SDL_WINDOWEVENT;
                input.event.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
                input.event.window.windowID = surfaces[0].id;
            } else throw std::runtime_error("Unknown preview input type");
            inputs.push_back(input);
            if (type == "click") {
                input.time++;
                input.event.type = SDL_MOUSEBUTTONUP;
                inputs.push_back(input);
            }
        }
        std::stable_sort(inputs.begin(), inputs.end(), [](const Input& a, const Input& b) { return a.time < b.time; });
        // A key the script never releases is a tap, as a real keyboard would send; a key-up later on holds it (and repeats) until then.
        std::vector<Input> taps;
        for (size_t i = 0; i < inputs.size(); i++)
            if (inputs[i].event.type == SDL_KEYDOWN) {
                bool released = false;
                for (size_t j = i + 1; j < inputs.size() && !released; j++)
                    released = inputs[j].event.type == SDL_KEYUP &&
                               inputs[j].event.key.keysym.sym == inputs[i].event.key.keysym.sym;
                if (!released) {
                    Input up = inputs[i];
                    up.time++;
                    up.event.type = SDL_KEYUP;
                    taps.push_back(up);
                }
            }
        inputs.insert(inputs.end(), taps.begin(), taps.end());
        std::stable_sort(inputs.begin(), inputs.end(), [](const Input& a, const Input& b) { return a.time < b.time; });
    }
    while (!quit && !stopping) {
        update_health();
        if (o.game_menu && !o.preview && !sleep_audio_parked && fs::exists(o.data / "session/game-menu.audio-sleep")) {
            sounds.close();
            sleep_audio_parked = true;
            std::ofstream(o.data / "session/game-menu.audio-sleep.ready").put('1');
        }
        if (sleep_audio_parked && !fs::exists(o.data / "session/game-menu.audio-sleep")) {
            sleep_audio_parked = false;
            restore_sleep_audio();
        }
        if (o.game_menu && !o.preview && fs::exists(o.data / "session/game-menu.wake")) {
            fs::remove(o.data / "session/game-menu.wake");
            reset_after_sleep();
            sleep_audio_parked = false;
            restore_sleep_audio();
        }
        // The emulator owns the displays during play; keep only its lifecycle alive.
        if (launch_phase == LaunchPhase::Playing) {
            haptics.close(); // the game's own rumble needs the pad's effect slots
            ui_trace.mark(UiTrace::Playing, rendered_frames, uint32_t(settings.page));
            poll_launch(0);
            if (launch_phase == LaunchPhase::Playing) wait_child_exit(launch_child, 1000);
            continue;
        }
        while (next_input < inputs.size() && s.GetTicks() - began >= inputs[next_input].time)
            s.PushEvent(&inputs[next_input++].event);
        // Apply releases/back actions before held-button repeat or drawing.
        // A slow screenshot/frame must not repeat a key already released.
        ui_trace.mark(UiTrace::Events, rendered_frames, uint32_t(settings.page));
        SDL_Event queued{};
        while (s.PollEvent(&queued)) event(queued);
        ui_trace.mark(UiTrace::Loop, rendered_frames, uint32_t(settings.page));
        if (!o.cache_report.empty()) visited_pages.insert(int(settings.page));
        if (quit || stopping) break;
        auto now = s.GetTicks();
        const bool muted = hardware.volume && hardware.volume_percent == 0;
        sounds.mute_music(muted);
        sounds.set_master(hardware.volume ? hardware.volume_percent : 100);
        if (muted && sounds.active()) sounds.stop();
        sounds.poll(now);
        haptics.poll();
        // A long press on a game is felt the moment it counts, finger still down.
        if (options_hold() && !hold_felt && now - pointer_at >= 600) {
            hold_felt = true;
            haptics.play(Haptic::Hold);
        }
        // Plugging in a charger, as on a phone.
        if (charge_seen && hardware.charging && !was_charging && !probe) haptics.play(Haptic::Notify);
        charge_seen = true;
        was_charging = hardware.charging;
        if (!departure_action.empty() && int32_t(now - departure_at) >= 0) {
            auto request = std::move(departure_action);
            departure_action.clear();
            departure_dispatch = true;
            menu_request(request);
            departure_dispatch = false;
            if (quit) break;
        }
        if (settings.page == Page::Controls && control_capture) {
            if (now - capture_started >= 10000) {
                control_capture = false;
                capture_logical = -1;
                controls_notice = "No button detected. Try again when you're ready.";
                dirty = true;
            } else if (now - capture_clock >= 1000) {
                capture_clock = now;
                dirty = true;
            }
        }
        if (!o.probe && (session_initialized || o.card_repair)) check_card(now);
        if (card_repair) {
            if (o.seconds && now - began >= Uint32(o.seconds) * 1000) break;
            if (card_repair == 1 && preferences.animations && now - feedback_frame_at >= 50) {
                feedback_frame_at = now;
                dirty = true;
            }
            if (dirty || o.frames) {
                draw(!o.screenshot.empty());
                frames++;
            }
            if (o.frames && frames >= o.frames) break;
            SDL_Event wait{};
            if (wait_event(&wait, 50)) {
                event(wait);
                while (s.PollEvent(&wait)) event(wait);
            }
            continue;
        }
        int volume_delta = volume_keys.poll();
        if (session_initialized && !launching && volume_delta) change_volume(hardware.volume_percent + volume_delta);
        if (volume_toast && now >= volume_toast) {
            volume_toast = 0;
            dirty = true;
        }
        if (o.port_menu) port_poll(now);
        else port_files_poll(now);
        for (auto point : touchscreen.poll(now - began))
            if (!(o.port_menu && port_idle)) raw_touch(point, now);
            else if (port_info_shown()) port_touch(point, now);
        if (o.port_menu && port_idle) port_info_tick(now);
        if (!session_initialized) {
            if (o.seconds && now - began >= Uint32(o.seconds) * 1000) break;
            const bool motion = boot_animating(now);
            if (motion && now - feedback_frame_at >= 33) {
                feedback_frame_at = now;
                dirty = true;
            }
            if (dirty || o.frames) {
                draw(!o.screenshot.empty());
                frames++;
            }
            if (o.frames && frames >= o.frames) break;
            if (o.boot_startup && !o.boot_warning) {
                start_boot_session();
                continue;
            }
            SDL_Event wait{};
            if (wait_event(&wait, motion ? 16 : input_wake.available() ? 250 : touchscreen.available() ? 8 : 50)) {
                event(wait);
                while (s.PollEvent(&wait)) event(wait);
            }
            continue;
        }
        poll_library_scan();
        poll_collection();
        poll_search_preview(now);
        if (facet_index.poll()) dirty = true;
        if (auto result = library_summary.poll()) {
            summary = std::move(*result);
            summary_ready = true;
            dirty = true;
        }
        if (!o.game_menu && library_ready && !setup_checked) {
            setup_checked = true;
            if (!o.preview && !fs::exists(o.data / "setup-complete") && fs::exists(o.data / "setup-required")) {
                settings.page = Page::RomSetup;
                settings.focus = 0;
                setup_first_run = true;
                dirty = true;
            }
        }
        if (!o.game_menu && o.boot_startup && library_ready && boot_hardware_ready && !return_writes_pending() &&
            !system.volume_pending() && !system.brightness_pending() && !resume_checked &&
            settings.page != Page::BootWarning) {
            resume_checked = true;
            auto ticket = o.data / "session/resume-launch.ini";
            auto saved = fields(ticket);
            std::error_code ec;
            fs::remove(ticket, ec);
            if (!saved["rom"].empty()) {
                search_query.clear();
                discovery_filter = 0;
                discovery_facets = {};
                library_view(0);
                for (size_t i = 0; i < model.games.size(); i++)
                    if (model.games[i].path.string() == saved["rom"]) {
                        select_game(i, false);
                        home_menu = false;
                        for (int core = 0; core < core_count; core++)
                            if (saved["core"] == core_ids[core]) resume_core = core;
                        resume_launch = resume_core >= 0;
                        launch();
                        break;
                    }
            }
        }
        poll_return_controls();
        if (!launching) {
            int direction = hold.poll(now);
            if (direction == -2 || direction == 2) {
                action(direction < 0 ? 8 : 9);
                last_activity = now;
            } else if (direction && !picker_open && horizontal_hold_page()) {
                if (settings.page == Page::Library) hold_navigate(direction, hold.held_for(now));
                else action(direction < 0 ? 0 : 1);
                last_activity = now;
            }
        }
        poll_brightness();
        poll_volume();
        if (auto refreshed = system.poll_status())
            if (refreshed->success) {
                const auto previous = hardware;
                auto brightness = hardware.brightness_percent, volume = hardware.volume_percent;
                hardware = refreshed->value;
                if (brightness_unsaved || brightness_drag || system.brightness_pending())
                    hardware.brightness_percent = brightness;
                if (o.preview || return_writes_pending() || volume_unsaved || volume_drag || system.volume_pending() ||
                    volume_toast > now)
                    hardware.volume_percent = volume;
                if (!volume_unsaved && !system.volume_pending()) {
                    volume_applied = hardware.volume_percent;
                    if (volume_applied > 0) unmuted_volume = volume_applied;
                }
                if (o.boot_startup && !boot_hardware_ready) {
                    boot_hardware_ready = true;
                    if (hardware.volume) {
                        system.request_volume(hardware.volume_percent);
                        volume_unsaved = true;
                    }
                    if (hardware.brightness) {
                        system.request_brightness(system_preferences.brightness);
                        brightness_unsaved = true;
                        brightness_changed = now;
                        hardware.brightness_percent = system_preferences.brightness;
                    }
                    if (hardware.power && system_preferences.power > 0) system.set("power", system_preferences.power);
                    if (hardware.lid && system_preferences.lid) system.set("lid", 1);
                }
                if (!same_hardware(previous, hardware)) dirty = true;
            }
        if (!launching || launch_preview) poll_service(now);
        home_bar_settle();
        if (!o.preview && !o.game_menu && !launching && worker <= 0 && now - automatic_update_at > 60000) {
            automatic_update_at = now;
            start_worker("device.update-auto");
        }
        poll_microphone(now);
        watch_menu(now);
        if (settings.page == Page::Scraper && !service_loaded && worker <= 0) {
            start_worker("status");
            service_loaded = true;
        }
        if (!o.game_menu && !launching && worker <= 0 && (!device_loaded || now - connection_poll >= 15000))
            refresh_network(now);
        poll_launch(now);
        poll_chat();
        if (media.adopt()) dirty = true;
        if (launch_phase == LaunchPhase::Playing) continue;
        if (touch_tail && now - release_at >= 140) {
            touch_tail = false;
            dirty = true;
        }
        bool launch_transition =
            launching && preferences.animations && (now - launch_at < 110 || launch_phase == LaunchPhase::Exiting);
        if (launching && preferences.animations) {
            int head = int((now - launch_at) / 75) % 8;
            if (head != launch_drawn_head) {
                launch_drawn_head = head;
                dirty = true;
            }
        }
        sync_view_motion(now);
        bool view_active = view_motion.active();
        if (view_motion.update(now) && !view_motion.active()) dirty = true;
        bool rail_animating = update_rail_feedback(now);
        bool feedback_active = view_active || rail_animating || (preferences.animations && shutting_down()) ||
                               (o.game_menu && menu_status["state"] == "busy" && preferences.animations) ||
                               (preferences.animations && network_busy() &&
                                (settings.page == Page::Network || settings.page == Page::Wifi)) ||
                               boot_animating(now) || launch_transition || touch_animating(now) ||
                               (preferences.animations &&
                                (now < micro_until || (picker_open && now - picker_at < 170) || picker_closing()));
        // Stop refreshing a stationary held contact after its cue completes.
        if (feedback_active && now - feedback_frame_at >= 16) {
            feedback_frame_at = now;
            dirty = true;
        }
        bool ready = options_hold() && now - pointer_at >= 600;
        if (ready != options_ready) {
            options_ready = ready;
            dirty = true;
        }
        ease_update(now);
        const bool glitter = glitter_active();
        if (glitter && now - glitter_frame_at >= 33) {
            glitter_frame_at = now;
            dirty = true;
        }
        if (banner_shown() && banner_frame(now) != drawn_banner_frame) dirty = true;
        bool list_animated = browser_list.update(now);
        const bool shader_moving = settings.page == Page::ShaderPicker && shader_carousel.update(now);
        bool animated =
            (preferences.animations && (carousel.update(now) || shader_moving || list_animated ||
                                        now - selected_at < 190 || (!view_active && now - page_at < 150))) ||
            (settings.page == Page::ShaderPicker && shader_thumbs_pending);
        if (animated) dirty = true;
        if (hardware.charging && !drawn_status_stamp.empty() && int((now / 420) % 4) != drawn_charge_phase)
            dirty = true;
        if (artwork_busy() && !drawn_status_stamp.empty() && loader_phase(now) != drawn_loader_phase) dirty = true;
        watch_theme(now);
        if (!launching) watch_system(now);
        if (!launching && !animated && !carousel.dragging) watch_library_assets(now);
        if (o.seconds && s.GetTicks() - began >= static_cast<Uint32>(o.seconds) * 1000) break;
        if (now - clock_poll >= 250) {
            clock_poll = now;
            if (!drawn_status_stamp.empty() && status_stamp() != drawn_status_stamp) dirty = true;
            if (home_menu && preferences.home_layout == 1 && settings.page == Page::Library && !chat_open &&
                !launching && std::time(nullptr) != drawn_second) {
                drawn_second = std::time(nullptr);
                dirty = true;
            }
        }
        if (dirty || o.frames) {
            ui_trace.mark(UiTrace::Assets, rendered_frames, uint32_t(settings.page));
            if (!rendered_frames) launch_profile.mark("assets-start");
            prepare_render_assets();
            if (!rendered_frames) launch_profile.mark("assets-ready");
            draw(!o.screenshot.empty());
            frames++;
            if (frames == 1 && o.game_menu && !o.preview) {
                system.request_status();
                system_poll = now;
            }
        }
        if (!o.preview && fs::exists(o.data / "session/capture-request")) {
            auto previous = o.screenshot;
            o.screenshot = o.data / "session/ui-snapshot";
            prepare_render_assets();
            draw(true);
            o.screenshot = previous;
            // Acknowledge only after both captures have been saved.
            fs::remove(o.data / "session/capture-request");
        }
        if (o.frames && frames >= o.frames) break;
        prepare_next_cover();
        SDL_Event e{};
        int wait_ms = animated || dirty                     ? 1
                      : feedback_active                     ? 16
                      : launching && preferences.animations ? int(std::min(50u, 75u - (now - launch_at) % 75))
                                                            : idle_wait_ms(now);
        if (o.game_menu && (!menu_pending_token.empty() || menu_status["state"] == "busy"))
            wait_ms = std::min(wait_ms, 20);
        if (chat_open) wait_ms = std::min(wait_ms, 40);
        if (options_hold() && !hold_felt) wait_ms = std::min(wait_ms, std::max(1, int(600 - (now - pointer_at))));
        if (glitter) wait_ms = std::min(wait_ms, 33);
        if (banner_shown() && preferences.animations) wait_ms = std::min(wait_ms, int(banner_wait(now)));
        if (wait_event(&e, wait_ms)) {
            event(e);
            while (s.PollEvent(&e)) event(e);
        }
    }
    flush_brightness();
    flush_volume();
    save_state();
    if (!o.cache_report.empty()) {
        std::ofstream report(o.cache_report);
        if (!report) throw std::runtime_error("Cannot write cache report");
        report << "{\"picker_detail\":" << picker_detail << ",\"detail_lines\":" << detail_lines.size()
               << ",\"detail_scroll\":" << detail_scroll << ",\"picker_open\":" << picker_open
               << ",\"picker_focus\":" << picker_focus << ",\"picker_scroll\":" << picker_scroll
               << ",\"list_scroll\":" << list_scroll << ",\"status_stamp\":" << std::quoted(drawn_status_stamp) << ',';
        report << "\"menu_music\":" << preferences.menu_music << ",\"music_volume\":" << preferences.music_volume
               << ",\"music_reads\":" << sounds.music_reads << ",\"music_playing\":" << sounds.music_playing()
               << ",\"music_samples\":" << sounds.music_samples() << ",\"menu_sounds\":" << preferences.menu_sounds
               << ",\"sound_volume\":" << preferences.sound_volume << ",\"sound_master\":" << sounds.master()
               << ",\"home_bar_focus\":" << home_bar_focus << ",\"sound_plays\":" << sounds.plays
               << ",\"sound_opens\":" << sounds.opens << ",\"sound_releases\":" << sounds.releases
               << ",\"sound_open_now\":" << sounds.open_now() << ",\"sound_reads\":" << sounds.reads
               << ",\"sound_bytes\":" << sounds.bytes() << ",\"sound_active\":" << sounds.active()
               << ",\"sound_counts\":[";
        for (size_t i = 0; i < sounds.counts.size(); i++) {
            if (i) report << ',';
            report << sounds.counts[i];
        }
        report << "],";
        report << "\"menu_haptics\":" << preferences.menu_haptics
               << ",\"haptic_strength\":" << preferences.haptic_strength
               << ",\"game_rumble\":" << preferences.game_rumble
               << ",\"rumble_strength\":" << preferences.rumble_strength << ",\"haptic_plays\":" << haptics.plays
               << ",\"haptic_rumbles\":" << haptics.rumbles << ",\"haptic_counts\":[";
        for (size_t i = 0; i < haptics.counts.size(); i++) report << (i ? "," : "") << haptics.counts[i];
        report << "],";
        report << "\"element_moves\":" << element_moves << ",\"back_transitions\":" << back_transitions
               << ",\"view_energy\":" << view_motion.energy << ",\"view_variant\":" << view_motion.variant << ',';
        report << "\"view_transition_style\":" << std::quoted(view_style_name(preferences.view_style))
               << ",\"push_animation_frames\":" << push_animation_frames << ',';
        report << "\"view_animations\":" << preferences.view_animations << ",\"view_transitions\":" << view_transitions
               << ",\"view_animation_frames\":" << view_animation_frames
               << ",\"view_animation_active\":" << view_motion.active()
               << ",\"view_canvas_failed\":" << view_canvas_failed << ',';
        report << "\"font_atlases\":" << rounded.loaded_atlases() << ",\"font_bytes\":" << rounded.loaded_bytes()
               << ",\"shader_catalog_loaded\":" << !shaders.empty() << ',';
        report << "\"shader_preview_scene\":" << shader_preview_scene
               << ",\"shader_preview_mode\":" << int(shader_preview_mode)
               << ",\"shader_preview_split\":" << shader_preview_split
               << ",\"shader_preview_entries\":" << shader_preview.count()
               << ",\"shader_preview_bytes\":" << shader_preview.bytes()
               << ",\"shader_preview_reads\":" << shader_preview.counters.file_reads
               << ",\"shader_preview_stamp_checks\":" << shader_preview.counters.stamp_checks << ',';
        report << "\"controls_game\":" << controls_per_game << ",\"controls_inherited\":" << controls_inherit
               << ",\"controls_capturing\":" << control_capture
               << ",\"controls_preset\":" << std::quoted(edited_controls.preset) << ',';
        report << "\"system_configuration_loaded\":" << system_configuration_loaded
               << ",\"timezone\":" << std::quoted(system_preferences.timezone) << ',';
        report << "\"drastic_backend\":" << preferences.drastic_backend
               << ",\"game_drastic_backend\":" << game_preferences.drastic_backend << ",";
        report << "\"systems_menu\":" << systems_menu << ",\"systems_selected\":" << systems_selected << ',';
        report << "\"show_systems\":" << preferences.show_systems << ",\"shown_system\":" << std::quoted(shown_system)
               << ",\"listed_systems\":" << listed_systems.size() << ",\"launching_core\":" << launching_core
               << ",\"library_count\":" << model.games.size() << ",\"play_fit\":" << play_fit
               << ",\"play_second\":" << play_second << ",\"play_frame\":" << std::quoted(play_frame) << ',';
        report << "\"shader_choices\":[";
        for (size_t i = 0; i < shader_choices.size(); ++i) report << (i ? "," : "") << shader_choices[i];
        report << "],";
        report << "\"default_core\":" << preferences.core << ",\"game_core_override\":" << game_preferences.core
               << ",\"game_threaded\":" << game_preferences.threaded << ",\"game_hires\":" << game_preferences.hires
               << ",\"game_frameskip\":" << game_preferences.frameskip
               << ",\"game_scale\":" << game_preferences.rgds_scale << ",\"game_filter2d\":" << game_preferences.rgds_2d
               << ",\"menu_fast_forward\":" << (menu_status["fast_forward"] == "1")
               << ",\"cheat_rows\":" << cheat_rows.size() << ",\"cheats_on\":" << cheats_on()
               << ",\"cheat_view\":" << int(cheat_view) << ",\"cheat_group\":" << cheat_group
               << ",\"cheats_available\":" << cheats_available() << ",\"settings_notice\":" << std::quoted(status)
               << ",\"picker_title\":" << std::quoted(picker_title) << ',';
        report << "\"visited_pages\":[";
        bool first_page = true;
        for (int page : visited_pages) {
            if (!first_page) report << ',';
            report << page;
            first_page = false;
        }
        report << "],";
        report << "\"filter_sheet\":" << filter_sheet_active() << ",\"filter_focus\":" << filter_focus
               << ",\"filter_scroll\":" << filter_scroll << ",\"library_view\":" << preferences.view
               << ",\"library_layout\":" << preferences.layout << ",\"discovery_active\":" << discovery_active()
               << ",\"discovery_description\":" << std::quoted(discovery_description())
               << ",\"search_preview_games\":" << search_matches.size()
               << ",\"search_preview_loading\":" << search_preview_loading << ",\"keyboard_row\":" << key_row
               << ",\"keyboard_col\":" << key_col << ",\"keyboard_layer\":" << key_layer
               << ",\"keyboard_caps\":" << key_caps << ",\"edit_cursor\":" << (edit_field == 4 ? edit_cursor : 0)
               << ',';
        report << "\"home_menu\":" << home_menu << ",\"filtered_games\":" << model.games.size()
               << ",\"search_query\":" << std::quoted(search_query) << ",\"discovery_filter\":" << discovery_filter
               << ',';
        report << "\"page\":" << int(settings.page) << ",\"boot_pending\":" << (settings.page == Page::BootWarning)
               << ",\"library_scan_started\":" << library_scan_started << ",\"library_ready\":" << library_ready
               << ",\"frames\":" << rendered_frames << ",\"metadata_reads\":" << library_cache.counters.file_reads
               << ",\"metadata_lookups\":" << library_cache.counters.lookups
               << ",\"metadata_hits\":" << library_cache.counters.hits
               << ",\"metadata_stamp_checks\":" << library_cache.counters.stamp_checks
               << ",\"image_reads\":" << media.image_reads << ",\"async_loads\":" << media.async_loads
               << ",\"top_skips\":" << top_skips << ",\"banner_reads\":" << media.banner_reads
               << ",\"media_lookups\":" << media.counters.lookups << ",\"media_hits\":" << media.counters.hits
               << ",\"media_stamp_checks\":" << media.counters.stamp_checks
               << ",\"render_file_reads\":" << render_file_reads << ",\"render_stamp_checks\":" << render_stamp_checks
               << ",\"metadata_entries\":" << library_cache.size() << ",\"image_entries\":" << media.image_count()
               << ",\"banner_entries\":" << media.banner_count() << ",\"texture_bytes\":" << media.bytes()
               << ",\"evictions\":" << library_cache.counters.evictions + media.counters.evictions << "}\n";
    }
    // The managed session dispatches a terminal request only with this
    // acknowledgement; ordinary exit or failed cleanup cannot trigger it.
    return system_exit_ack ? 76 : 0;
}
