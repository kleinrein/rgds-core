#pragma once
// Launching a game: the exit handoff, the runner thread and polling its result.
// Members of App in main.cpp; included once, right after the class.

void App::launch(bool from_home) {
    if (launching) return;
    if (worker > 0 && (worker_action == "device.reboot" || worker_action == "device.poweroff" ||
                       worker_action == "device.update-apply")) {
        status = "Finishing the system request";
        dirty = true;
        return;
    }
    if (probe) return;
    if (home_menu && !from_home) {
        open_category();
        return;
    }
    if (systems_menu) {
        choose_system(systems_selected);
        return;
    }
    if (return_writes_pending()) {
        status = "Restoring settings";
        dirty = true;
        return;
    }
    // A previous failure must not latch every core off. The runner checks
    // writable storage and reapplies saved volume before opening any audio.
    return_audio_failed = false;
    if (o.boot_startup && (!library_ready || (!o.preview && (!boot_hardware_ready || system.volume_pending())))) {
        status = "Preparing the launcher";
        dirty = true;
        return;
    }
    if (model.games.empty()) return;
    // A port that still needs its owner's files shows where to send them first (port_files_screen.hpp).
    if (!resume_launch && port_needs_files(model.games[model.selected])) {
        port_files_open(model.games[model.selected]);
        return;
    }
    launch_preview = o.preview || model.games[model.selected].demo || !o.allow_launch;
    if (!launch_preview && (o.app.empty() || o.data.empty())) {
        status = "ERROR: Launch needs app and data paths";
        dirty = true;
        return;
    }
    auto local = library_cache.options(model.games[model.selected]);
    launching_core = unsigned(offered_core(local.core < 0 ? int(preferences.core) : local.core));
    if (resume_launch) launching_core = unsigned(resume_core);
    const std::string& system = model.games[model.selected].system;
    if (system == "ports") launching_core = unsigned(portmaster_core); // a PortMaster port
    else if (system != "nds") launching_core = unsigned(play_core);    // RG DS Play
    sound_intent(MenuCue::Launch);
    launching = true;
    launch_at = s.GetTicks();
    feedback_frame_at = launch_at;
    launch_phase = LaunchPhase::Waiting;
    sounds.fade_music();
    launch_cancelled = launch_handoff = false;
    launch_result = 0;
    launch_drawn_head = -1;
    cancel_navigation();
    carousel.reset(model.selected);
    dirty = true;
    if (!launch_preview && worker > 0) {
        kill(worker, SIGTERM);
        worker_action.clear();
    }
}

void App::begin_launch_exit(bool handoff, Uint32 now) {
    launch_handoff = handoff;
    if (launch_phase == LaunchPhase::Exiting) return;
    launch_exit_opacity = launch_opacity(now);
    launch_exit_at = now;
    launch_phase = LaunchPhase::Exiting;
    dirty = true;
}

void App::cancel_launch() {
    if (!launching || launch_phase == LaunchPhase::Playing || launch_cancelled) return;
    sounds.stop();
    sound_intent(MenuCue::Back);
    launch_cancelled = true;
    launch_cancel_at = s.GetTicks();
    if (launch_child > 0) kill(launch_child, SIGTERM);
    else begin_launch_exit(false, s.GetTicks());
    launch_handoff = false;
}

void App::finish_launch(bool reopen) {
    if (launch_channel >= 0) ::close(launch_channel);
    launch_channel = -1;
    launching = false;
    launch_phase = LaunchPhase::None;
    if (!reopen) configure_sounds(); // a cancelled or preview launch: the music comes back
    if (reopen) {
        if (launching_core == unsigned(portmaster_core)) refresh_other_systems(); // ports added or removed
        library_cache.clear();
        library_summary.request(o.data, all_games);
        open();
        configure_sounds();
        last_activity = s.GetTicks();
        library_view(preferences.view);
        if (home_menu) carousel.reset(home_selected); // back to Home, which the game was started from
        try {
            if (fs::exists(o.system_preferences)) system_preferences = load_system_preferences(o.system_preferences);
        } catch (const std::exception& e) {
            std::cerr << "Could not reload system settings: " << e.what() << std::endl;
        }
        if (hardware.volume) {
            hardware.volume_percent = confirmed_volume();
            return_volume = hardware.volume_percent;
            volume_unsaved = true;
        }
        if (hardware.brightness) {
            return_brightness = system_preferences.brightness;
            hardware.brightness_percent = system_preferences.brightness;
            brightness_unsaved = true;
            brightness_changed = last_activity;
        }
        if (hardware.power && system_preferences.power > 0) return_power = system_preferences.power;
        return_status_needed = true;
        begin_return_controls();
    }
    last_activity = s.GetTicks();
    input_guard_until = last_activity + (reopen ? 250 : 100);
    status = launch_cancelled      ? "Launch cancelled"
             : launch_result == 78 ? "The memory card is read-only"
             : launch_result       ? "LAUNCH ERROR " + std::to_string(launch_result) + " - SEE LOG"
             : launch_preview      ? "PREVIEW: " + std::string(core_names[launching_core]) + " SELECTED"
                                   : "BACK HOME";
    if (launch_result == 78) check_card(s.GetTicks(), true);
    if (launch_result && !launch_cancelled) emit_sound(MenuCue::Error);
    else if (reopen) emit_sound(MenuCue::Back);
    dirty = true;
}

void App::start_launch_runner(Uint32 now) {
    try {
        save_switcher_library();
    } catch (const std::exception& error) {
        std::cerr << "Game switcher snapshot: " << error.what() << std::endl;
        std::error_code ec;
        fs::remove(o.data / "session/switcher-library.ini", ec);
    }
    int channel[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, channel) < 0) {
        launch_result = 127;
        begin_launch_exit(false, now);
        return;
    }
    if (fcntl(channel[0], F_SETFD, FD_CLOEXEC) < 0 || fcntl(channel[1], F_SETFD, FD_CLOEXEC) < 0) {
        ::close(channel[0]);
        ::close(channel[1]);
        launch_result = 127;
        begin_launch_exit(false, now);
        return;
    }
#ifdef SO_NOSIGPIPE
    const int suppress = 1;
    setsockopt(channel[0], SOL_SOCKET, SO_NOSIGPIPE, &suppress, sizeof(suppress));
#endif
    std::vector<std::string> args = {"python3",
                                     (o.app / "runtime/emulators.py").string(),
                                     "launch",
                                     "--app",
                                     o.app.string(),
                                     "--data",
                                     o.data.string(),
                                     "--core",
                                     core_ids[launching_core],
                                     "--rom",
                                     model.games[model.selected].path.string(),
                                     "--display-handoff-fd",
                                     std::to_string(channel[1])};
    save_state();
    launch_child = fork();
    if (launch_child == 0) {
        if (resume_launch) setenv("RGDS_FORCE_AUTOLOAD", "1", 1);
        ::close(channel[0]);
        if (fcntl(channel[1], F_SETFD, 0) < 0) _exit(127);
        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(a.data());
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    resume_launch = false;
    ::close(channel[1]);
    launch_channel = channel[0];
    launch_phase = LaunchPhase::Preparing;
    if (launch_child < 0) {
        launch_result = 127;
        begin_launch_exit(false, now);
    }
}

void App::poll_launch(Uint32 now) {
    if (!launching) return;
    if (launch_child > 0) {
        int result = 0;
        auto finished = waitpid(launch_child, &result, WNOHANG);
        if (finished == launch_child || (finished < 0 && errno != EINTR)) {
            launch_child = -1;
            launch_result = finished < 0        ? 127
                            : WIFEXITED(result) ? WEXITSTATUS(result)
                                                : 128 + (WIFSIGNALED(result) ? WTERMSIG(result) : 0);
            if (launch_phase == LaunchPhase::Playing) {
                finish_launch(true);
                return;
            }
            begin_launch_exit(false, now);
        } else if (launch_cancelled && now - launch_cancel_at >= 5000) kill(launch_child, SIGKILL);
    }
    if (launch_phase == LaunchPhase::Playing) return;
    if (launch_preview) {
        if (launch_phase != LaunchPhase::Exiting && now - launch_at >= 500) begin_launch_exit(false, now);
    } else if (launch_phase == LaunchPhase::Waiting) {
        // Reap helpers and drain pending controls via the ordinary event loop.
        // No sleeps, synchronous worker shutdown or control flush on START.
        if (worker > 0) {
            auto done = waitpid(worker, nullptr, WNOHANG);
            if (done > 0 || (done < 0 && errno != EINTR)) worker = -1;
            else if (now - launch_at >= 1000) kill(worker, SIGKILL);
        }
        if (worker <= 0 && !system.volume_pending() && !system.brightness_pending() && !volume_unsaved &&
            !brightness_unsaved)
            start_launch_runner(now);
    } else if (launch_phase == LaunchPhase::Preparing && !launch_cancelled) {
        char ready = 0;
        auto received = recv(launch_channel, &ready, 1, MSG_DONTWAIT);
        if (received == 1 && ready == 'R') begin_launch_exit(true, now);
    }
    if (launch_phase == LaunchPhase::Exiting && (!preferences.animations || now - launch_exit_at >= 80)) {
        if (launch_handoff && launch_child > 0) {
            // Let the finite launch sparkle finish while input/cancel remains live.
            if (sounds.active() && now - launch_at < 1200) return;
            // Present the final fade frame, release SDL, then acknowledge ownership.
            draw(!o.screenshot.empty());
            close();
            launch_phase = LaunchPhase::Playing;
            int flags = 0;
#ifdef MSG_NOSIGNAL
            flags = MSG_NOSIGNAL;
#endif
            const char released = 'C';
            if (send(launch_channel, &released, 1, flags) != 1) {
                kill(launch_child, SIGTERM);
            }
            ::close(launch_channel);
            launch_channel = -1;
        } else if (launch_child <= 0) finish_launch(false);
    }
}

void App::stop_launch() {
    if (launch_channel >= 0) ::close(launch_channel);
    launch_channel = -1;
    if (launch_child > 0) {
        kill(launch_child, SIGTERM);
        for (int i = 0; i < 600; i++) {
            auto done = waitpid(launch_child, nullptr, WNOHANG);
            if (done == launch_child || (done < 0 && errno != EINTR)) {
                launch_child = -1;
                break;
            }
            usleep(10000);
        }
        if (launch_child > 0) {
            kill(launch_child, SIGKILL);
            while (waitpid(launch_child, nullptr, 0) < 0 && errno == EINTR) {}
        }
    }
    launch_child = -1;
}
