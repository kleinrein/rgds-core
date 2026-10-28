#pragma once
// Network status, startup choice, Wi-Fi and the menu request handler.
// Members of App in main.cpp; included once, right after the class.

void App::refresh_network(Uint32 now) {
    connection_poll = now;
    auto probe = o.preview ? NetworkProbe{} : probe_network();
    bool details = settings.page == Page::Network || settings.page == Page::Wifi || settings.page == Page::Bluetooth ||
                   settings.page == Page::DeviceInfo || settings.page == Page::Maintenance;
    if (!device_loaded || o.preview || details || probe.signature != network_signature ||
        now - device_info_at >= 300000) {
        start_worker("device.info");
        device_loaded = true;
        device_info_at = now;
        network_signature = probe.signature;
        probed_signal = -1;
        return;
    }
    int level = probe.signal >= 67   ? 3
                : probe.signal >= 34 ? 2
                : probe.signal > 0   ? 1
                                     : 0,
        previous = probed_signal >= 67   ? 3
                   : probed_signal >= 34 ? 2
                   : probed_signal > 0   ? 1
                                         : 0;
    if (probed_signal < 0 || level != previous) dirty = true;
    probed_signal = probe.signal;
}

void App::confirm(const std::string& operation, Page parent) {
    confirmation = operation;
    confirm_return = parent;
    settings.page = Page::Confirm;
    settings.focus = 0;
    dirty = true;
}

void App::save_startup() {
    std::ostringstream payload;
    payload << "{\"mode\":\"" << startup_mode << "\",\"warning\":" << (startup_warning ? "true" : "false") << '}';
    start_worker("device.startup", payload.str());
}

void App::change_startup(int direction, bool toggle) {
    if (worker > 0) return;
    // Only the start screen is chosen here; the boot target stays as it is.
    startup_mode = o.independent || device_status["boot_mode"] == "rgds" ? "rgds" : "stock";
    startup_warning = toggle ? device_status["boot_warning"] != "1" : direction > 0;
    save_startup();
}

void App::wifi_choose() {
    if (worker > 0) return;
    auto prefix = "network_" + std::to_string(settings.focus) + "_";
    network_id = device_status[prefix + "id"];
    if (network_id.empty()) return;
    if (device_status[prefix + "connected"] == "1") {
        settings.page = Page::Network;
        service_notice = "Already connected.";
        dirty = true;
        return;
    }
    if (device_status[prefix + "saved"] == "1") {
        auto selected_network = network_id;
        show_picker("Saved network", {"Connect", "Enter password"}, 0, [this, selected_network](int selected) {
            network_id = selected_network;
            if (selected == 1) {
                begin_edit(2);
                return;
            }
            std::ostringstream payload;
            payload << "{\"network\":" << std::quoted(network_id) << ",\"use_saved\":true}";
            start_worker("device.connect", payload.str());
            settings.page = Page::Network;
            settings.focus = 1;
        });
        return;
    }
    if (device_status[prefix + "security"].find("802.1X") != std::string::npos) {
        service_notice = "This network requires an enterprise profile.";
        dirty = true;
        return;
    }
    if (device_status[prefix + "security"].empty()) {
        std::ostringstream payload;
        payload << "{\"network\":" << std::quoted(network_id) << ",\"password\":\"\"}";
        start_worker("device.connect", payload.str());
        settings.page = Page::Network;
    } else begin_edit(2);
    dirty = true;
}

// Homebrew shelves (runtime/homebrew_store.py): A on a system's pinned Homebrew opens its shelf; A on a game downloads it
// into that system's folder. The DS shelf fills the first games folder.
fs::path App::homebrew_folder(const std::string& system) const {
    if (system == "nds") return o.roots.empty() ? (o.preview ? o.data / "roms/nds" : fs::path()) : o.roots.front();
    for (const auto& [id, root] : o.system_roots)
        if (id == system) return root;
    return o.preview ? o.data / "roms" / system : fs::path();
}

std::vector<std::string> App::homebrew_shelves() const {
    std::vector<std::string> shelves;
    for (const char* id : {"nds", "gba", "nes"})
        if (!homebrew_folder(id).empty()) shelves.push_back(id);
    return shelves;
}

void App::homebrew_step(int delta) {
    const auto shelves = homebrew_shelves();
    if (shelves.size() < 2) return;
    auto at = std::find(shelves.begin(), shelves.end(), homebrew_system);
    const int i = at == shelves.end() ? 0 : int(at - shelves.begin());
    const int next = (i + delta + int(shelves.size())) % int(shelves.size());
    homebrew_open(shelves[size_t(next)]);
    view_route = delta < 0 ? ViewTravel::Back : ViewTravel::Forward;
}

void App::homebrew_open(const std::string& system) {
    homebrew_system = system;
    settings.page = Page::Homebrew;
    settings.focus = 0;
    list_scroll = 0;
    list_focus = -1;
    // the previous shelf's rows must not flash up under this one
    for (auto it = device_status.begin(); it != device_status.end();)
        it = it->first.rfind("hb_", 0) == 0 ? device_status.erase(it) : std::next(it);
    homebrew_queued = system;
    homebrew_kick();
    dirty = true;
}

// Sends a waiting shelf request once the worker is free; artwork indexing gives way to it (start_worker stops it).
void App::homebrew_kick() {
    if (!homebrew_queued) return;
    // another shelf's list (still fetching its covers) gives way; a download in progress finishes first
    if (worker > 0 && worker_action == "device.homebrew-browse") stop_worker();
    if (worker > 0 && worker_action != "index") return;
    const std::string system = *homebrew_queued;
    homebrew_queued.reset();
    std::ostringstream payload;
    payload << "{\"system\":" << std::quoted(system) << ",\"folder\":" << std::quoted(homebrew_folder(system).string())
            << "}";
    start_worker("device.homebrew-browse", payload.str());
    dirty = true;
}

void App::homebrew_choose() {
    auto prefix = "hb_" + std::to_string(settings.focus) + "_";
    const auto id = device_status[prefix + "id"];
    if (id.empty()) return;
    if (device_status[prefix + "installed"] == "1") {
        service_notice = "Already in your library.";
        dirty = true;
        return;
    }
    if (worker > 0 || homebrew_queued) {
        service_notice = worker_action == "device.homebrew-get" ? "One download at a time." : "One moment.";
        dirty = true;
        return;
    }
    std::ostringstream payload;
    payload << "{\"id\":" << std::quoted(id) << ",\"folder\":" << std::quoted(homebrew_folder(homebrew_system).string())
            << "}";
    start_worker("device.homebrew-get", payload.str());
    dirty = true;
}

// Bluetooth devices: A pairs (then trusts and connects) or connects a paired device; X forgets a paired one.
void App::bluetooth_choose() {
    if (worker > 0) return;
    auto prefix = "bt_" + std::to_string(settings.focus) + "_";
    bluetooth_address = device_status[prefix + "address"];
    if (bluetooth_address.empty()) return;
    if (device_status[prefix + "connected"] == "1") {
        service_notice = "Already connected.";
        dirty = true;
        return;
    }
    std::ostringstream payload;
    payload << "{\"address\":" << std::quoted(bluetooth_address) << "}";
    start_worker("device.bt-pair", payload.str());
    dirty = true;
}

void App::bluetooth_forget() {
    if (worker > 0 || bluetooth_address.empty()) return;
    std::ostringstream payload;
    payload << "{\"address\":" << std::quoted(bluetooth_address) << "}";
    start_worker("device.bt-forget", payload.str());
    dirty = true;
}

void App::note_menu_input(const SDL_Event& e) {
    if (e.type == SDL_JOYBUTTONDOWN) menu_input = "joystick button " + std::to_string(e.jbutton.button);
    else if (e.type == SDL_JOYHATMOTION && e.jhat.value) menu_input = "joystick hat " + std::to_string(e.jhat.value);
    else if (e.type == SDL_KEYDOWN) menu_input = "key " + std::to_string(e.key.keysym.sym);
    else if (e.type == SDL_FINGERDOWN || e.type == SDL_MOUSEBUTTONDOWN) menu_input = "SDL pointer";
}

void App::menu_request(const std::string& action) {
    if (menu_status["state"] == "busy" || !menu_pending_token.empty()) {
        dirty = true;
        return;
    }
    if (o.preview && action.rfind("switch:", 0) == 0) {
        std::cout << "Game switch (preview): " << action << std::endl;
        menu_status["message"] = "Saving before switching games…";
        dirty = true;
        return;
    }
    if (action == "resume" && !departure_dispatch && preferences.menu_sounds) {
        if (emit_sound(MenuCue::Resume)) {
            explicit_sound.reset();
            departure_action = action;
            departure_at = s.GetTicks() + 180;
            return;
        }
    }
    menu_sound_action = action;
    if (action == "save" || action == "load" || action.rfind("slot-", 0) == 0) sound_intent(MenuCue::Confirm);
    // The supervisor logs <pid>-<n> tokens; this names the input behind each one.
    if (o.game_menu)
        std::cout << "RGDS menu helper: " << getpid() << '-' << menu_sequence + 1 << ' ' << action << " after "
                  << menu_input << " at " << s.GetTicks() << " ms" << std::endl;
    if (o.preview) {
        if (action == "save" || action.rfind("slot-save:", 0) == 0) sound_intent(MenuCue::Save);
        else if (action == "load" || action.rfind("slot-load:", 0) == 0) sound_intent(MenuCue::Load);
        if (action == "threaded:0" || action == "threaded:1") menu_status["threaded"] = action.substr(9);
        if (action == "fast-forward") menu_status["fast_forward"] = menu_status["fast_forward"] == "1" ? "0" : "1";
        menu_status["message"] = action == "save"                  ? "Checkpoint saved (preview)"
                                 : action == "load"                ? "Checkpoint loaded (preview)"
                                 : action.rfind("shader:", 0) == 0 ? "Shader change requested (preview)"
                                                                   : "";
        if (action.rfind("shader:", 0) == 0) std::cout << "Shader request (preview): " << action << std::endl;
        if (action == "resume" || action == "quit" || action == "force-quit" || action == "restart") quit = true;
        dirty = true;
        return;
    }
    auto path = o.data / "session/game-menu.request", pending = fs::path(path.string() + ".pending");
    fs::create_directories(path.parent_path());
    std::ofstream file(pending);
    auto request_token = std::to_string(getpid()) + '-' + std::to_string(++menu_sequence);
    file << "token=" << request_token << "\naction=" << action << '\n';
    file.close();
    if (!file) {
        status = "Could not send game action";
        dirty = true;
        return;
    }
    fs::rename(pending, path);
    if (action == "resume" && o.port_menu) port_go_idle(); // beside a port the helper stays
    else if (action == "resume") quit = true;
    else {
        menu_pending_token = request_token;
        menu_request_at = s.GetTicks();
        menu_status["state"] = "busy";
        menu_status["message"] = action == "save"   ? "Saving checkpoint…"
                                 : action == "load" ? "Loading checkpoint…"
                                 : (action.rfind("graphics:", 0) == 0 || action.rfind("filter2d:", 0) == 0 ||
                                    action.rfind("adaptive:", 0) == 0)
                                     ? "Applying graphics…"
                                 : action.rfind("shader:", 0) == 0 ? "Applying shader…"
                                 : action == "restart"             ? "Saving before restarting…"
                                 : action == "quit"                ? "Returning home…"
                                                                   : "Working…";
    }
    dirty = true;
}
