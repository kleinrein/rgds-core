#pragma once
// System settings: volume, brightness, date, time, timezone and controls.
// Members of App in main.cpp; included once, right after the class.

void App::refresh_system() {
    hardware = system.status();
    dirty = true;
    if (o.preview) {
        std::ifstream file(o.data / "volume.ini");
        std::string line;
        while (std::getline(file, line))
            if (line.rfind("volume=", 0) == 0)
                hardware.volume_percent = std::clamp(std::atoi(line.c_str() + 7), 0, 100);
    }
    if (hardware.volume_percent > 0) unmuted_volume = hardware.volume_percent;
    volume_applied = hardware.volume_percent;
}

void App::save_preview_volume(int value) {
    if (!o.preview) return;
    fs::create_directories(o.data);
    auto path = o.data / "volume.ini", pending = fs::path(path.string() + ".tmp");
    std::ofstream file(pending);
    file << "schema=1\nvolume=" << value << '\n';
    file.close();
    if (!file) throw std::runtime_error("Could not save volume");
    fs::rename(pending, path);
}

int App::confirmed_volume() {
    if (!o.preview) {
        auto fields = rgds::fields(o.data / "volume.ini");
        if (fields.count("volume")) {
            try {
                size_t end = 0;
                int value = std::stoi(fields["volume"], &end);
                if (end == fields["volume"].size() && value >= 0 && value <= 100) return value;
            } catch (...) {}
        }
    }
    return system.cached().volume_percent;
}

void App::poll_volume() {
    if (auto update = system.poll_volume()) {
        if (update->success) {
            volume_applied = update->value;
            return_audio_failed = false;
        }
        if (update->settled && update->value == hardware.volume_percent) {
            if (!update->success) {
                hardware.volume_percent = volume_applied = confirmed_volume();
                status = "Could not set volume - see log";
            }
            try {
                save_preview_volume(volume_applied);
            } catch (const std::exception& error) {
                status = error.what();
            }
            volume_unsaved = false;
            dirty = true;
        }
    }
}

void App::flush_volume() {
    if (!volume_unsaved && !system.volume_pending()) return;
    bool ok = system.flush_volume();
    volume_applied = ok ? system.cached().volume_percent : confirmed_volume();
    if (!ok) {
        hardware.volume_percent = volume_applied;
        status = "Could not set volume - see log";
    }
    try {
        save_preview_volume(volume_applied);
    } catch (const std::exception& error) {
        status = error.what();
    }
    volume_unsaved = false;
    poll_volume();
}

void App::toggle_mute() {
    hold.clear();
    volume_drag = false;
    change_volume(hardware.volume_percent == 0 ? unmuted_volume : 0);
}

void App::change_volume(int value) {
    if (!hardware.volume) {
        status = "Volume control unavailable";
        dirty = true;
        return;
    }
    value = std::clamp(value, 0, 100);
    if (value == hardware.volume_percent && !return_audio_failed) {
        volume_toast = s.GetTicks() + 1800;
        last_activity = s.GetTicks();
        dirty = true;
        return;
    }
    if (return_writes_pending()) return_volume = value;
    else system.request_volume(value);
    volume_unsaved = true;
    hardware.volume_percent = value;
    if (value > 0) unmuted_volume = value;
    volume_toast = s.GetTicks() + 1800;
    last_activity = s.GetTicks();
    dirty = true;
}

// The TZ value for a zone. Daylight saving "Automatic" is the zone name itself (tzdata knows the rules); forcing it
// on or off pins the zone's standard offset (+1 h for on) as a fixed rule, since tzdata has no such switch.
std::string App::timezone_value(const std::string& zone, int dst) {
    if (dst == 0 || zone == "UTC") return zone;
    const auto now = std::time(nullptr);
    std::tm local{};
    setenv("TZ", zone.c_str(), 1);
    tzset();
    const bool known = localtime_r(&now, &local) != nullptr;
    long standard = known ? local.tm_gmtoff - (local.tm_isdst > 0 ? 3600 : 0) : 0;
    return fixed_timezone(standard + (dst == 1 ? 3600 : 0));
}

void App::apply_timezone() {
    zone_offsets.clear();
    setenv("TZ", timezone_value(system_preferences.timezone, system_preferences.dst).c_str(), 1);
    tzset();
    dirty = true;
}

long App::zone_offset(const std::string& zone) {
    auto now = std::time(nullptr);
    if (now / 900 != zone_offsets_bucket) {
        zone_offsets.clear();
        zone_offsets_bucket = now / 900;
    }
    auto found = zone_offsets.find(zone);
    if (found != zone_offsets.end()) return found->second;
    std::tm local{};
    long offset = 0;
    if (zone != "UTC") {
        const bool chosen = zone == system_preferences.timezone;
        setenv("TZ", timezone_value(zone, chosen ? system_preferences.dst : 0).c_str(), 1);
        tzset();
        if (localtime_r(&now, &local)) offset = local.tm_gmtoff;
        setenv("TZ", timezone_value(system_preferences.timezone, system_preferences.dst).c_str(), 1);
        tzset();
    }
    return zone_offsets[zone] = offset;
}

std::string App::zone_time(const std::string& zone) {
    auto shifted = std::time(nullptr) + zone_offset(zone);
    std::tm t{};
    gmtime_r(&shifted, &t);
    return time_text(t);
}

std::string App::timezone_summary() {
    const auto& zone = system_preferences.timezone;
    return zone == "UTC" ? zone : timezone_place(zone) + " · " + offset_text(zone_offset(zone));
}

std::string App::timezone_title() {
    auto region = timezone_region(timezones.at(size_t(std::clamp(settings.focus, 0, int(timezones.size()) - 1))));
    return region == "UTC" ? locale.text("Timezone")
                           : locale.text("Timezone") + " · " + locale.text(timezone_region_name(region));
}

std::pair<std::string, std::string> App::timezone_neighbours() {
    const int count = int(timezones.size()), focus = std::clamp(settings.focus, 0, count - 1);
    auto region = [&](int i) { return timezone_region(timezones[size_t(i)]); };
    int start = focus;
    while (start > 0 && region(start - 1) == region(focus)) start--;
    const int next = timezone_region_jump(focus, 1);
    return {start > 0 ? locale.text(timezone_region_name(region(start - 1))) : std::string(),
            region(next) != region(focus) ? locale.text(timezone_region_name(region(next))) : std::string()};
}

void App::load_system_configuration() {
    if (system_configuration_loaded) return;
    system_configuration_loaded = true;
    try {
        if (fs::exists(o.system_preferences)) {
            system_preferences = load_system_preferences(o.system_preferences);
            if (!probe && !o.game_menu) {
                if (hardware.power && system_preferences.power > 0 && !system.set("power", system_preferences.power))
                    status = "Could not apply saved power mode";
                if (hardware.brightness) {
                    if (system.set("brightness", system_preferences.brightness)) {
                        brightness_applied = system_preferences.brightness;
                        hardware.brightness_percent = brightness_applied;
                    } else {
                        system_preferences.brightness = brightness_applied;
                        status = "Could not apply saved brightness";
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "System preferences fallback: " << e.what() << std::endl;
    }
    if (!probe && !o.game_menu && hardware.lid && system_preferences.lid && !system.set("lid", 1))
        status = "Could not enable lid wake";
    apply_timezone();
}

void App::save_date_format(int value) {
    auto previous = system_preferences.date_format;
    system_preferences.date_format = value;
    try {
        save_system_preferences(o.system_preferences, system_preferences);
        dirty = true;
    } catch (const std::exception& e) {
        system_preferences.date_format = previous;
        status = "Could not save date format";
        std::cerr << e.what() << std::endl;
        dirty = true;
    }
}

void App::save_time_format(int value) {
    auto previous = system_preferences.time_format;
    system_preferences.time_format = value;
    try {
        save_system_preferences(o.system_preferences, system_preferences);
        dirty = true;
    } catch (const std::exception& e) {
        system_preferences.time_format = previous;
        status = "Could not save time format";
        std::cerr << e.what() << std::endl;
        dirty = true;
    }
}

void App::save_dst(int value) {
    auto previous = system_preferences.dst;
    system_preferences.dst = value;
    try {
        save_system_preferences(o.system_preferences, system_preferences);
        apply_timezone();
    } catch (const std::exception& e) {
        system_preferences.dst = previous;
        status = "Could not save daylight saving";
        std::cerr << e.what() << std::endl;
        dirty = true;
    }
}

void App::save_timezone() {
    auto previous = system_preferences.timezone;
    system_preferences.timezone = timezones.at(settings.focus);
    try {
        save_system_preferences(o.system_preferences, system_preferences);
        apply_timezone();
    } catch (const std::exception& e) {
        system_preferences.timezone = previous;
        status = "Could not save timezone";
        std::cerr << e.what() << std::endl;
        dirty = true;
    }
}

bool App::apply_control(int focus, const SystemPreferences& value) {
    if (return_writes_pending()) {
        if (focus == 0) return_power = value.power;
        else if (focus == 2) return_lid = value.lid;
        else if (focus == 4) return_brightness = value.brightness;
        return true;
    }
    if (focus == 0) return system.set("power", value.power);
    if (focus == 2) return system.set("lid", value.lid);
    if (focus == 4) return system.set("brightness", value.brightness);
    return true;
}

void App::poll_brightness() {
    if (auto result = system.poll_brightness()) {
        if (result->success) {
            brightness_applied = result->value;
            hardware.brightness_percent = result->value;
        } else if (result->settled && result->value == system_preferences.brightness) {
            system_preferences.brightness = brightness_applied;
            status = "Could not apply brightness";
            dirty = true;
        }
    }
    if (brightness_unsaved && !return_writes_pending() && !system.brightness_pending() && !brightness_drag &&
        s.GetTicks() - brightness_changed >= 180) {
        try {
            save_system_preferences(o.system_preferences, system_preferences);
            brightness_unsaved = false;
        } catch (const std::exception& e) {
            status = "Could not save brightness";
            brightness_unsaved = false;
            std::cerr << e.what() << std::endl;
            dirty = true;
        }
    }
}

void App::flush_brightness() {
    bool success = system.flush_brightness();
    if (auto result = system.poll_brightness()) {
        if (result->success) brightness_applied = result->value;
    }
    if (!success) {
        system_preferences.brightness = brightness_applied;
        status = "Could not apply brightness";
    }
    if (brightness_unsaved) {
        try {
            save_system_preferences(o.system_preferences, system_preferences);
            brightness_unsaved = false;
        } catch (const std::exception& e) {
            std::cerr << e.what() << std::endl;
        }
    }
}

void App::change_system(int focus, int delta, bool cycle) {
    auto previous = system_preferences, next = previous;
    bool available = focus == 0   ? hardware.power
                     : focus == 1 ? hardware.sleep
                     : focus == 2 ? hardware.lid
                     : focus == 3 ? hardware.battery >= 0
                                  : hardware.brightness;
    if (!available) {
        status = "Control unavailable on this device";
        dirty = true;
        return;
    }
    if (focus == 0) next.power = cycle ? (next.power + 1) % 4 : std::clamp(next.power + delta, 0, 3);
    else if (focus == 1) next.idle = cycle ? (next.idle + 1) % 5 : std::clamp(next.idle + delta, 0, 4);
    else if (focus == 2) next.lid = cycle ? !next.lid : delta > 0;
    else if (focus == 3) next.battery_percent = cycle ? !next.battery_percent : delta > 0;
    else if (focus == 4)
        next.brightness = cycle ? (next.brightness >= 100 ? 5 : std::min(100, (next.brightness / 5 + 1) * 5))
                                : std::clamp((next.brightness / 5 + delta) * 5, 5, 100);
    if (next.power == previous.power && next.idle == previous.idle && next.lid == previous.lid &&
        next.brightness == previous.brightness && next.battery_percent == previous.battery_percent)
        return;
    if (focus == 4) {
        system_preferences.brightness = next.brightness;
        if (return_writes_pending()) return_brightness = next.brightness;
        else system.request_brightness(next.brightness);
        brightness_changed = s.GetTicks();
        brightness_unsaved = true;
        dirty = true;
        return;
    }
    if (!apply_control(focus, next)) {
        status = "Could not apply setting - see log";
        dirty = true;
        return;
    }
    try {
        save_system_preferences(o.system_preferences, next);
        system_preferences = next;
        status = o.preview ? "Saved - desktop simulation" : "Settings saved";
    } catch (const std::exception& e) {
        if (!apply_control(focus, previous)) std::cerr << "Could not restore previous hardware setting" << std::endl;
        status = "Could not save settings";
        std::cerr << e.what() << std::endl;
    }
    dirty = true;
}

void App::reset_after_sleep() {
    top_seen.clear();
    // Discard pre-suspend SDL/raw presses, including a held browser gesture.
    SDL_Event pending{};
    while (s.PollEvent(&pending)) {
        if (pending.type == SDL_QUIT) quit = true;
    }
    touchscreen.discard_until_release();
    cancel_navigation();
    brightness_drag = volume_drag = false;
    carousel.reset(navigation_selected());
    last_activity = s.GetTicks();
    dirty = true;
}

void App::sleep_now(bool lid) {
    if (!hardware.sleep) {
        status = "Sleep unavailable on this device";
        dirty = true;
        return;
    }
    flush_return_controls();
    flush_volume();
    flush_brightness();
    save_state();
    status = o.preview ? "Sleep simulated - press any control" : "Sleeping - use Power to wake";
    dirty = true;
    draw();
    sounds.stop();
    // The private shared PCM can survive suspend just like an engine's
    // PCM. Keep it warm: reopening the codec can swallow a short cue and
    // delays the wake sound. Exclusive/custom configurations still release it.
    const char* shared_audio = std::getenv("RGDS_SHARED_AUDIO");
    bool warm_audio = !o.preview && shared_audio && std::string(shared_audio) == "1" && sounds.available();
    bool close_played = false;
    if (warm_audio || o.preview) close_played = emit_sound(MenuCue::LidClose);
    if (o.preview && close_played) {
        auto began = s.GetTicks();
        while (sounds.active() && s.GetTicks() - began < 400) s.Delay(5);
    }
    if (!warm_audio) sounds.close();
    bool ok = system.sleep(lid, close_played);
    status = ok ? (o.preview ? "Sleep simulated - settings retained" : "Welcome back") : "Could not sleep - see log";
    reset_after_sleep();
    // Stock resume hooks may reset the mixer even with the sink retained.
    if (warm_audio && !system.sync_volume()) sounds.close();
    restore_sleep_audio();
    if (ok) emit_sound(MenuCue::Unlock);
}

void App::watch_system(Uint32 now) {
    if (!return_writes_pending() && now - system_poll >= 10000 && now - system_retry >= 500) {
        system_retry = now;
        if (system.request_status()) system_poll = now;
    }
    // The game runner owns lid sleep while playing, including its overlay.
    if (o.preview || probe || o.game_menu) return;
    if (now - lid_poll >= 50) {
        lid_poll = now;
        auto closed = system.lid_closed();
        if (lid_sleep.poll(closed, system_preferences.lid && hardware.lid, system.lid_generation())) {
            sleep_now(true);
            return;
        }
    }
    if (!pointer_down && system_preferences.idle > 0 && hardware.sleep &&
        now - last_activity >= static_cast<Uint32>(idle_minutes[system_preferences.idle]) * 60000)
        sleep_now();
}

void App::watch_theme(Uint32 now) {
    if (!o.watch_assets || o.theme.empty() || now - theme_poll < 100) return;
    theme_poll = now;
    auto source = read_theme_source();
    if (source != theme_source) {
        theme_source = std::move(source);
        theme_changed = now;
        theme_pending = true;
    }
    // Editors may truncate then write, or replace the file via rename.
    if (theme_pending && now - theme_changed >= 200) {
        theme_pending = false;
        action(6);
    }
}
