#pragma once
// Loading the library, the background scan, and the boot session with its input snapshots.
// Members of App in main.cpp; included once, right after the class.

void App::load_library(std::vector<Game> games) {
    model.games = std::move(games);
    if (o.preview && model.games.empty())
        for (const char* name :
             {"A little adventure", "Puzzle collection", "Racing club", "Garden stories", "Pocket exploration"})
            model.games.push_back({name, {}, true});
    ds_games = model.games;
    refresh_other_systems();
    library_ready = true;
    if (!o.game_menu) {
        summary_ready = false;
        library_summary.request(o.data, all_games);
    }
    device_status = fields(o.data / "device/status.ini");
    if (!o.game_menu && !o.boot_startup) library_view(preferences.view);
    if (o.game_menu) {
        if (std::find(std::begin(core_ids), std::end(core_ids), o.game_core) == std::end(core_ids))
            throw std::runtime_error("Unknown game engine");
        if (!o.game_rom.empty()) o.game_rom = fs::weakly_canonical(o.game_rom);
        model.games = {
            {o.game_rom.empty() ? "Preview game" : o.game_rom.stem().string(), o.game_rom, o.game_rom.empty()}};
        // Its system, for its icon and the Graphics it offers (a zipped game in RG DS Play is a GBA one).
        model.games[0].system = system_for_path(o.game_rom);
        if (model.games[0].system == "nds" && o.game_core == "rgds-play") model.games[0].system = "gba";
        model.selected = 0;
        home_menu = false;
        settings.page = Page::QuickMenu;
        menu_status = {{"state", "paused"}, {"checkpoint", o.preview ? "1" : "0"}};
        load_cheats();
    }
    if (o.game_switcher) {
        load_switcher();
        settings.page = Page::GameSwitcher;
    }
    restore_state();
    if (o.library || o.game_menu) home_menu = false;
    else if (o.home || o.boot_startup) {
        home_menu = true;
        settings.page = Page::Library;
    }
    home_selected = std::min(home_selected, home_tiles() - 1);
    carousel.reset(navigation_selected());
    browser_list.reveal(model.selected, model.games.size());
    theme_source = read_theme_source();
    if (o.settings) {
        settings.open();
        if (!o.section.empty()) {
            if (o.section == "scraper") open_scraper();
            else if (o.section == "updates") open_updates(Page::Settings);
            else {
                static const std::array<std::string, 8> ids = {"theme",        "power",  "library",  "sound",
                                                               "connectivity", "system", "controls", "statistics"};
                const bool display = o.section == "display" || o.section == "brightness" || o.section == "motion";
                auto found = std::find(ids.begin(), ids.end(), display ? "theme" : o.section == "vibration" ? "sound" : o.section);
                if (found == ids.end()) throw std::runtime_error("Unknown settings section");
                settings.section = display ? 10 : int(found - ids.begin());
                settings.enter();
                if (o.section == "brightness") settings.page = Page::Display;
                else if (o.section == "motion") settings.page = Page::Motion;
                else if (o.section == "vibration") settings.page = Page::Haptics;
                else if (o.section == "controls") open_controls(false);
            }
        }
    }
}

void App::stop_library_scan() {
    if (library_child > 0) {
        kill(library_child, SIGTERM);
        for (int i = 0; i < 30; i++) {
            if (waitpid(library_child, nullptr, WNOHANG) != 0) {
                library_child = -1;
                break;
            }
            usleep(10000);
        }
        if (library_child > 0) {
            kill(library_child, SIGKILL);
            waitpid(library_child, nullptr, 0);
        }
    }
    library_child = -1;
    if (library_pipe >= 0) ::close(library_pipe);
    library_pipe = -1;
}

// The games folders' modification times, one level down: enough to notice a copy of new games.
std::string App::games_signature() const {
    std::string result;
    auto stamp = [&](const fs::path& path) {
        std::error_code error;
        auto time = fs::last_write_time(path, error);
        if (!error) result += path.string() + ':' + std::to_string(static_cast<long long>(time.time_since_epoch().count())) + ';';
    };
    auto walk = [&](const fs::path& root) {
        stamp(root);
        std::error_code error;
        for (fs::directory_iterator it(root, error), end; !error && it != end; it.increment(error))
            if (it->is_directory(error)) stamp(it->path());
    };
    for (const auto& root : o.roots) walk(root);
    for (const auto& entry : o.system_roots) walk(entry.second);
    return result;
}

// Games put on the card while the menu is open turn up by themselves: from Home (or the Library
// settings) the folders are looked at every few seconds, and a change starts the same scan as the button.
void App::watch_games(Uint32 now) {
    if (o.preview || o.game_menu || launching || !library_ready || !session_initialized || worker > 0 ||
        now - games_checked_at < 4000 || picker_open)
        return;
    games_checked_at = now;
    if (!((home_menu && settings.page == Page::Library) || settings.page == Page::LibraryOptions)) return;
    if (games_signature() == games_signature_seen) return;
    if (!model.games.empty() && model.selected < model.games.size()) games_keep_selected = model.games[model.selected].path;
    rescan_games(true);
}

void App::begin_library_scan() {
    library_scan_started = true;
    games_signature_seen = games_signature();
    if (o.roots.empty()) {
        load_library({});
        return;
    }
    int channel[2];
    if (pipe(channel) != 0) {
        status = "Could not start library scan";
        library_ready = true;
        return;
    }
    if (fcntl(channel[0], F_SETFL, O_NONBLOCK) < 0 || fcntl(channel[0], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(channel[1], F_SETFD, FD_CLOEXEC) < 0) {
        ::close(channel[0]);
        ::close(channel[1]);
        status = "Could not start library scan";
        library_ready = true;
        return;
    }
    std::vector<std::string> arguments = {o.executable.string(), "--scan-library"};
    for (const auto& root : o.roots) {
        arguments.push_back("--rom-dir");
        arguments.push_back(root.string());
    }
    std::vector<char*> argv;
    for (auto& argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    library_child = fork();
    if (library_child == 0) {
        ::close(channel[0]);
        if (dup2(channel[1], STDOUT_FILENO) < 0) _exit(127);
        ::close(channel[1]);
        execv(argv[0], argv.data());
        _exit(127);
    }
    ::close(channel[1]);
    if (library_child < 0) {
        ::close(channel[0]);
        status = "Could not start library scan";
        library_ready = true;
        return;
    }
    library_pipe = channel[0];
}

void App::poll_library_scan() {
    if (library_pipe < 0) return;
    char bytes[4096];
    ssize_t count;
    while ((count = read(library_pipe, bytes, sizeof(bytes))) > 0) {
        library_bytes.append(bytes, size_t(count));
        if (library_bytes.size() > 64 * 1024 * 1024) {
            stop_library_scan();
            status = "Library scan exceeded its limit";
            library_ready = true;
            return;
        }
    }
    if (count == 0) library_eof = true;
    else if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        stop_library_scan();
        status = "Could not read library scan";
        library_ready = true;
        return;
    }
    if (library_child > 0) {
        int result = 0;
        auto done = waitpid(library_child, &result, WNOHANG);
        if (done == 0) return;
        if (done < 0 || !WIFEXITED(result) || WEXITSTATUS(result) != 0) {
            stop_library_scan();
            status = "Library scan failed";
            library_ready = true;
            return;
        }
        library_child = 0;
    }
    if (!library_eof) return;
    ::close(library_pipe);
    library_pipe = -1;
    library_child = -1;
    std::vector<Game> games;
    size_t at = 0;
    while (at < library_bytes.size()) {
        auto path_end = library_bytes.find('\0', at);
        if (path_end == std::string::npos) break;
        auto title_end = library_bytes.find('\0', path_end + 1);
        if (title_end == std::string::npos) break;
        games.push_back({library_bytes.substr(path_end + 1, title_end - path_end - 1),
                         fs::path(library_bytes.substr(at, path_end - at)), false});
        at = title_end + 1;
    }
    library_bytes.clear();
    auto previous = settings;
    bool previous_home = home_menu;
    auto previous_category = home_selected;
    load_library(std::move(games));
    if (!games_keep_selected.empty()) { // a scan that began by itself leaves the selection where it was
        auto found = std::find_if(model.games.begin(), model.games.end(),
                                  [&](const Game& g) { return g.path == games_keep_selected; });
        if (found != model.games.end()) model.selected = size_t(found - model.games.begin());
        games_keep_selected.clear();
        carousel.reset(navigation_selected());
        browser_list.reveal(model.selected, model.games.size());
    }
    if (o.boot_startup) {
        settings = previous;
        home_menu = previous_home;
        home_selected = previous_category;
        if (!home_menu) library_view(preferences.view);
        carousel.reset(navigation_selected());
    } else if (scan_stay) {
        if (!previous_home) library_view(preferences.view);
        settings = previous;
        home_menu = previous_home;
        home_selected = previous_category;
        carousel.reset(navigation_selected());
    }
    scan_stay = false;
    if (!o.roots.empty()) start_worker("index");
    dirty = true;
}

void App::start_boot_session() {
    if (session_initialized) return;
    load_system_configuration();
    if (!preferences.pixel) check_fonts();
    shaders =
        load_shaders(o.app.empty() ? fs::path("assets/shaders/catalog.tsv") : o.app / "assets/shaders/catalog.tsv");
    cancel_navigation();
    boot_touch_candidate = false;
    session_initialized = true;
    settings.page = Page::Library;
    home_menu = !o.library;
    selected_at = page_at = s.GetTicks();
    if (o.preview) {
        refresh_system();
        boot_hardware_ready = true;
    } else system.request_status();
    begin_library_scan();
    dirty = true;
}

void App::continue_boot() {
    if (settings.page == Page::BootWarning && boot_displayed) start_boot_session();
}

void App::snapshot_boot_input() {
    if (!o.boot_startup || session_initialized) return;
    auto pump = reinterpret_cast<decltype(&SDL_PumpEvents)>(dlsym(s.handle, "SDL_PumpEvents"));
    if (pump) pump();
    auto keys = reinterpret_cast<decltype(&SDL_GetKeyboardState)>(dlsym(s.handle, "SDL_GetKeyboardState"));
    if (keys) {
        int length = 0;
        auto state = keys(&length);
        for (auto item : {std::make_pair(SDLK_a, SDL_SCANCODE_A), std::make_pair(SDLK_RETURN, SDL_SCANCODE_RETURN)})
            if (state && item.second < length && state[item.second]) boot_blocked_keys.insert(item.first);
    }
    auto mouse = reinterpret_cast<decltype(&SDL_GetMouseState)>(dlsym(s.handle, "SDL_GetMouseState"));
    if (mouse && (mouse(nullptr, nullptr) & SDL_BUTTON_LMASK)) boot_touch_wait_release = true;
    auto touch_count = reinterpret_cast<decltype(&SDL_GetNumTouchDevices)>(dlsym(s.handle, "SDL_GetNumTouchDevices"));
    auto touch_id = reinterpret_cast<decltype(&SDL_GetTouchDevice)>(dlsym(s.handle, "SDL_GetTouchDevice"));
    auto fingers = reinterpret_cast<decltype(&SDL_GetNumTouchFingers)>(dlsym(s.handle, "SDL_GetNumTouchFingers"));
    if (touch_count && touch_id && fingers)
        for (int i = 0; i < touch_count(); i++)
            if (fingers(touch_id(i)) > 0) boot_touch_wait_release = true;
    boot_touch_wait_release = boot_touch_wait_release || touchscreen.initially_pressed();
    auto update = reinterpret_cast<decltype(&SDL_JoystickUpdate)>(dlsym(s.handle, "SDL_JoystickUpdate"));
    auto button = reinterpret_cast<decltype(&SDL_JoystickGetButton)>(dlsym(s.handle, "SDL_JoystickGetButton"));
    auto instance = reinterpret_cast<decltype(&SDL_JoystickInstanceID)>(dlsym(s.handle, "SDL_JoystickInstanceID"));
    if (update) update();
    const char* a = std::getenv("RGDS_BUTTON_A");
    int accept = a ? std::atoi(a) : 0;
    if (button && instance)
        for (auto* joystick : joysticks)
            for (int index : {accept, 7}) {
                bool mainline = o.mainline && mainline_pads.count(instance(joystick));
                int raw = mainline ? (index == 7 ? 9 : 1) : index;
                if (button(joystick, raw)) boot_blocked_buttons.insert({instance(joystick), Uint8(index)});
            }
}

void App::snapshot_menu_input() {
    if (!o.game_menu) return;
    std::string held;
    if (touchscreen.initially_pressed()) {
        touchscreen.hold_initial_contact();
        held = " touch";
    }
    auto update = reinterpret_cast<decltype(&SDL_JoystickUpdate)>(dlsym(s.handle, "SDL_JoystickUpdate"));
    auto count = reinterpret_cast<decltype(&SDL_JoystickNumButtons)>(dlsym(s.handle, "SDL_JoystickNumButtons"));
    auto button = reinterpret_cast<decltype(&SDL_JoystickGetButton)>(dlsym(s.handle, "SDL_JoystickGetButton"));
    auto hat = reinterpret_cast<decltype(&SDL_JoystickGetHat)>(dlsym(s.handle, "SDL_JoystickGetHat"));
    auto instance = reinterpret_cast<decltype(&SDL_JoystickInstanceID)>(dlsym(s.handle, "SDL_JoystickInstanceID"));
    if (update && count && button && hat && instance) {
        update();
        for (auto* joystick : joysticks) {
            auto id = instance(joystick);
            bool mainline = o.mainline && mainline_pads.count(id);
            for (int raw = 0; raw < count(joystick); raw++)
                if (button(joystick, raw)) {
                    int index = !mainline ? raw
                                : MainlineControls::button(mainline_controls.joypads.count(id)
                                                               ? MainlineControls::from_joypad(raw)
                                                               : raw);
                    if (index >= 0) {
                        boot_blocked_buttons.insert({id, Uint8(index)});
                        held += " button " + std::to_string(index);
                    }
                }
            if (hat(joystick, 0) != SDL_HAT_CENTERED) {
                menu_held_hats.insert(id);
                held += " hat";
            }
        }
    }
    if (!held.empty())
        std::cout << "RGDS menu helper: held at open:" << held << "; ignored until released" << std::endl;
}
