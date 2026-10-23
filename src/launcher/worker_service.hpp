#pragma once
// Menu watching, the background worker and the service poll.
// Members of App in main.cpp; included once, right after the class.

void App::watch_menu(Uint32 now) {
    // Pending actions should acknowledge promptly; idle menus keep their
    // slower poll. The first frame starts with the supervisor's real state.
    if (!o.game_menu || o.preview ||
        (rendered_frames &&
         now - menu_poll < (menu_pending_token.empty() && menu_status["state"] != "busy" ? 100u : 20u)))
        return;
    menu_poll = now;
    auto value = fields(o.data / "session/game-menu.status");
    if (value["view_request"] != menu_status["view_request"] && std::atoi(value["view_request"].c_str()) > 0) {
        picker_open = false;
        cancel_navigation();
        if (value["view"] == "switcher") {
            load_switcher();
            settings.page = Page::GameSwitcher;
        } else settings.page = Page::QuickMenu;
        settings.focus = 0;
        load_cheats();
        dirty = true;
    }
    // The old status can still be on disk after a tap. Keep the local
    // busy state until the supervisor acknowledges this exact request.
    if (menu_pending_token.empty() || value["request_token"] == menu_pending_token) {
        if (!menu_pending_token.empty() && value["state"] != "busy") {
            if (value["state"] == "error") emit_sound(MenuCue::Error);
            // The direct present has no GL pass: the filter is saved and needs the renderer, so offer the restart.
            else if (menu_sound_action.rfind("shader:", 0) == 0 && value["message"].rfind("Shader saved", 0) == 0) {
                settings.page = Page::ApplyShader;
                settings.focus = 0;
            } else if (menu_sound_action == "save" || menu_sound_action.rfind("slot-save:", 0) == 0)
                emit_sound(MenuCue::Save);
            else if (menu_sound_action == "load" || menu_sound_action.rfind("slot-load:", 0) == 0)
                emit_sound(MenuCue::Load);
            // The supervisor saves live graphics choices after the engine accepts them; show what it stored.
            if ((menu_sound_action.rfind("graphics:", 0) == 0 || menu_sound_action.rfind("filter2d:", 0) == 0 ||
                 menu_sound_action.rfind("adaptive:", 0) == 0) &&
                value["state"] != "error")
                game_preferences = library_cache.reload_options(model.games[0]);
            // The engine rewrote its cheat list before answering; show that, whichever way it went.
            if (menu_sound_action.rfind("cheat:", 0) == 0) load_cheats();
            // RG DS Play's view: what the runtime stored once the player applied it.
            if (menu_sound_action.rfind("play-graphics:", 0) == 0) load_play_view();
            menu_sound_action.clear();
        }
        if (value["state"] != "busy") menu_pending_token.clear();
        if (!same_fields(value, menu_status)) {
            menu_status = std::move(value);
            dirty = true;
        }
    } else if (now - menu_request_at > (menu_sound_action.rfind("graphics:", 0) == 0 ? 50000u : 15000u)) {
        menu_pending_token.clear();
        emit_sound(MenuCue::Error);
        menu_status["state"] = "error";
        menu_status["message"] = "The game menu did not respond. Resume or return without saving.";
        dirty = true;
    }
    auto volume = fields(o.data / "volume.ini");
    if (volume.count("volume")) {
        int level = std::clamp(std::atoi(volume["volume"].c_str()), 0, 100);
        volume_applied = level;
        if (!volume_unsaved && !system.volume_pending() && level != hardware.volume_percent) {
            hardware.volume_percent = level;
            if (level > 0) unmuted_volume = level;
            volume_toast = now + 1800;
            dirty = true;
        }
    }
    if (menu_status["state"] == "home") quit = true;
}

void App::start_worker(const std::string& operation, const std::string& payload) {
    if (worker > 0 && (worker_action == "device.info" || worker_action == "device.update-auto") &&
        operation != "device.info")
        stop_worker();
    if (worker > 0 && worker_action == "index" && operation.rfind("device.", 0) == 0) stop_worker();
    if (worker > 0) return;
    bool busy_visible = operation != "device.info" || settings.page == Page::Startup || settings.page == Page::Wifi ||
                        !service_notice.empty();
    service_notice.clear();
    bool device = operation.rfind("device.", 0) == 0;
    auto folder = o.data / (device ? "device" : "scraper");
    fs::create_directories(folder);
    fs::path script = fs::path("runtime") / (device ? "device_services.py" : "scraper.py");
    std::vector<std::string> args = {"python3",
                                     "-B",
                                     (o.app.empty() ? script : o.app / script).string(),
                                     device ? operation.substr(7) : operation,
                                     "--data",
                                     o.data.string()};
    if (device) {
        args.push_back("--app");
        args.push_back((o.app.empty() ? fs::current_path() : o.app).string());
        if (o.preview) args.push_back("--preview");
    } else {
        static const std::array<const char*, 3> providers = {"auto", "gametdb", "libretro"};
        args.push_back("--provider");
        args.push_back(operation == "scrape" && settings.page == Page::GameArtwork
                           ? "auto"
                           : providers.at(preferences.artwork_provider));
        if (operation == "scrape" && settings.page == Page::GameArtwork && !model.games.empty()) {
            args.push_back("--rom");
            args.push_back(model.games[model.selected].path.string());
            args.push_back("--force");
        }
        // Game info and extra pictures ride along with the covers when they are switched on.
        std::string extras = preferences.game_info ? "info" : "";
        if (preferences.extra_pictures & 1) extras += std::string(extras.empty() ? "" : ",") + "snap";
        if (preferences.extra_pictures & 2) extras += std::string(extras.empty() ? "" : ",") + "title";
        if (operation == "scrape" && !extras.empty()) {
            args.push_back("--extras");
            args.push_back(extras);
        }
        for (auto root : o.roots) {
            args.push_back("--rom-dir");
            args.push_back(root.string());
        }
        if (preferences.show_systems)
            for (const auto& [system, root] : o.system_roots) {
                args.push_back("--system-dir");
                args.push_back(system + "=" + root.string());
            }
    }
    int channel[2];
    if (pipe(channel) != 0) {
        status = "Could not start artwork service";
        return;
    }
    auto child = fork();
    if (child == 0) {
        ::close(channel[1]);
        dup2(channel[0], STDIN_FILENO);
        ::close(channel[0]);
        int log = ::open((folder / "worker.log").c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (log >= 0) {
            dup2(log, STDOUT_FILENO);
            dup2(log, STDERR_FILENO);
            ::close(log);
        }
        std::vector<char*> argv;
        for (auto& arg : args) argv.push_back(arg.data());
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    ::close(channel[0]);
    if (!payload.empty() && child > 0) {
        auto sent = write(channel[1], payload.data(), payload.size());
        (void)sent;
    }
    ::close(channel[1]);
    if (child < 0) {
        status = "Could not start artwork service";
        return;
    }
    worker = child;
    worker_action = operation;
    worker_ok = false;
    if (busy_visible) dirty = true;
}

void App::stop_worker() {
    if (worker <= 0) return;
    if (waitpid(worker, nullptr, WNOHANG) == 0) {
        kill(worker, SIGTERM);
        for (int i = 0; i < 100; i++) {
            if (waitpid(worker, nullptr, WNOHANG) != 0) {
                worker = -1;
                return;
            }
            usleep(10000);
        }
        kill(worker, SIGKILL);
        waitpid(worker, nullptr, 0);
    }
    worker = -1;
}

void App::poll_service(Uint32 now) {
    bool artwork_finished = false;
    if (worker > 0) {
        int result = 0;
        auto finished = waitpid(worker, &result, WNOHANG);
        if (finished > 0) {
            worker_ok = WIFEXITED(result) && WEXITSTATUS(result) == 0;
            worker = -1;
            worker_finished_at = now;
            service_poll = 0;
            artwork_finished = worker_action == "index" || worker_action == "scrape";
            if (worker_action != "device.info" || settings.page == Page::Startup || settings.page == Page::Wifi)
                dirty = true;
        }
    }
    if (artwork_finished) {
        if (settings.page == Page::GameArtwork && !model.games.empty())
            artwork_result = fields(item_folder(model.selected) / "artwork-status.ini");
        library_cache.invalidate_metadata();
        if (media.refresh()) library_cache.invalidate_titles();
    }
    // Status files change only through workers; poll quickly while one runs.
    bool quick = worker > 0 || now - worker_finished_at < 1000 || settings.page == Page::SaveGallery ||
                 !menu_pending_token.empty();
    if (now - service_poll < (quick ? 250u : 2000u)) return;
    service_poll = now;
    auto next = fields(o.data / "scraper/status.ini");
    if (!same_fields(next, service)) {
        service = std::move(next);
        library_cache.invalidate_metadata();
        if (media.refresh()) library_cache.invalidate_titles();
        dirty = true;
    }
    auto device = fields(o.data / "device/status.ini");
    if (!same_fields(device, device_status)) {
        device_status = std::move(device);
        dirty = true;
    }
    experience = fields(o.data / "experience.ini");
    if (!experience.count("auto_update")) experience["auto_update"] = "1";
    if (settings.page == Page::SaveGallery && !model.games.empty()) {
        auto next = fields(gallery_root() / "index.ini");
        if (next != slots) {
            slots = std::move(next);
            media.refresh();
            dirty = true;
        }
    }
    auto update = fields(o.data / "update/status.ini");
    if (!same_fields(update, update_status)) {
        update_status = std::move(update);
        dirty = true;
    }
    if (worker <= 0 && worker_ok && worker_action == "device.update-apply" && update_status["state"] == "restarting")
        quit = true;
    if (shutting_down() && worker <= 0 && !worker_ok) {
        shutdown_at = 0;
        shutdown_kind.clear();
        dirty = true;
    }
    if (worker <= 0 && !worker_ok && worker_action.rfind("device.", 0) == 0 && device_status["state"] != "error") {
        device_status["state"] = "error";
        device_status["message"] = "This request did not finish. Please try again.";
        dirty = true;
    }
    if (worker <= 0 && worker_ok && (worker_action == "device.poweroff" || worker_action == "device.reboot") &&
        device_status["state"] == "ready") {
        system_exit_ack = !o.preview;
        quit = true;
    }
}
