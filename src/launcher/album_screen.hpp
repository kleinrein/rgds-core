#pragma once
// Screenshots: Menu + R in the launcher, and the Album (Settings > Library > Album). The pictures themselves are
// runtime/screenshots.py's: it writes the PNGs, the thumbnails and album.tsv, and the launcher runs it for every
// change (save, delete, cover, view) so there is one album format. In a game the runner takes the picture
// (game_menu.py); this page only shows what is there.
// Members of App in main.cpp; included once, right after the class.

fs::path App::album_root() const { return player_root(o.data) / "screenshots"; }

// album.tsv, newest first: path, game key, title, system, YYYYMMDD. Shown grouped by game (a game's group sits where
// its newest picture is), newest first inside each group.
void App::load_album() {
    std::vector<AlbumShot> shots;
    std::ifstream file(album_root() / "album.tsv");
    for (std::string line; std::getline(file, line) && shots.size() < 5000;) {
        std::array<std::string, 5> cell;
        size_t at = 0;
        for (size_t i = 0; i < cell.size(); i++) {
            const size_t end = i + 1 < cell.size() ? line.find('\t', at) : std::string::npos;
            cell[i] = line.substr(at, end == std::string::npos ? std::string::npos : end - at);
            if (end == std::string::npos) break;
            at = end + 1;
        }
        if (cell[0].empty() || cell[1].empty()) continue;
        AlbumShot shot{cell[0], cell[1], cell[2], cell[3], cell[4]};
        if (shot.key == "launcher") shot.title = "Menus";
        else if (shot.title.empty()) shot.title = fs::path(shot.path).parent_path().filename().string();
        shots.push_back(std::move(shot));
    }
    std::vector<std::string> order;
    for (const auto& shot : shots)
        if (std::find(order.begin(), order.end(), shot.key) == order.end()) order.push_back(shot.key);
    std::stable_sort(shots.begin(), shots.end(), [&](const AlbumShot& a, const AlbumShot& b) {
        return std::find(order.begin(), order.end(), a.key) < std::find(order.begin(), order.end(), b.key);
    });
    album = std::move(shots);
    if (settings.page == Page::Album)
        settings.focus = album.empty() ? 0 : std::clamp(settings.focus, 0, int(album.size()) - 1);
    dirty = true;
}

void App::open_album() {
    settings.section = 2;
    settings.library_rows = library_row_count();
    settings.remember(Page::LibraryOptions, library_row_count() - 2);
    settings.page = Page::Album;
    settings.focus = 0;
    album_viewing = false;
    load_album();
    album_helper({"index"}, "index"); // pictures copied in or removed by hand since the last save
}

// One screenshots.py command at a time, off the frame loop; album_poll sees it end.
bool App::album_helper(const std::vector<std::string>& command, const std::string& tag) {
    if (album_child > 0) return false;
    const fs::path app = o.app.empty() ? fs::current_path() : o.app;
    std::vector<std::string> args = {"python3", "-B", (app / "runtime/screenshots.py").string()};
    args.insert(args.end(), command.begin(), command.end());
    args.push_back("--data");
    args.push_back(o.data.string());
    std::error_code ec;
    fs::create_directories(o.data / "logs", ec);
    fs::create_directories(o.data / "session", ec);
    const pid_t child = fork();
    if (child == 0) {
        int log = ::open((o.data / "logs/screenshots.log").c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
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
    if (child < 0) return false;
    album_child = child;
    album_task = tag;
    return true;
}

void App::album_poll() {
    if (album_child <= 0) return;
    int status_code = 0;
    if (waitpid(album_child, &status_code, WNOHANG) != album_child) return;
    album_child = -1;
    const bool ok = WIFEXITED(status_code) && WEXITSTATUS(status_code) == 0;
    const std::string task = std::move(album_task);
    album_task.clear();
    media.refresh(); // a new cover (box.bmp) or a new view replaces what is cached
    if (task == "view") {
        album_viewing = ok && settings.page == Page::Album;
        if (!ok) emit_sound(MenuCue::Error);
        if (ok && album_view_then_photo && settings.page == Page::Album) photo_begin();
        else if (ok && album_view_then_chat && settings.page == Page::Album) album_to_pictochat();
        album_view_then_photo = album_view_then_chat = false;
    } else if (task == "photo-save") {
        status = ok ? "Saved to the Album." : "The photo could not be saved.";
        if (!ok) emit_sound(MenuCue::Error);
    } else if (task == "cover") {
        status = ok ? "This picture is the game's cover now." : "The cover could not be changed.";
        emit_sound(ok ? MenuCue::Save : MenuCue::Error);
    } else if (task == "shot") {
        if (!ok) status = "The screenshot could not be saved.";
    }
    if (task == "delete" && !ok) emit_sound(MenuCue::Error);
    if (task != "view") load_album();
    if (task == "photo-save" && ok && settings.page == Page::Album) {
        // The new photo is its game's newest picture: the first of that game in the list.
        for (size_t i = 0; i < album.size(); i++)
            if (album[i].key == photo_saved_key) {
                settings.focus = int(i);
                break;
            }
    }
    dirty = true;
}

// Menu + R outside a game: the next frame of both panels, saved without the flash that follows it.
void App::take_launcher_screenshot() {
    if (shot_requested || album_child > 0) return;
    shot_requested = true;
    dirty = true;
}

void App::service_launcher_screenshot() {
    if (!shot_requested) return;
    shot_requested = false;
    const auto stem = o.data / "session/launcher-shot";
    std::error_code ec;
    for (const char* suffix : {"", "-top.bmp", "-bottom.bmp"}) fs::remove(stem.string() + suffix, ec);
    auto previous = o.screenshot;
    o.screenshot = stem;
    prepare_render_assets();
    draw(true);
    o.screenshot = previous;
    std::vector<std::string> command = {"save", "--key", "launcher", "--title", "Menus"};
    if (fs::exists(stem.string() + "-top.bmp")) {
        command.insert(command.end(), {"--top", stem.string() + "-top.bmp"});
        if (fs::exists(stem.string() + "-bottom.bmp"))
            command.insert(command.end(), {"--bottom", stem.string() + "-bottom.bmp"});
    } else if (fs::exists(stem))
        command.insert(command.end(), {"--combined", stem.string()}); // the preview's one window
    else return;
    if (!album_helper(command, "shot")) return;
    shutter_at = s.GetTicks();
    if (!shutter_at) shutter_at = 1;
    emit_sound(MenuCue::Save); // the click and the buzz; the flash follows in the next frames
    dirty = true;
}

// The shutter: both panels flash white and fade over 240 ms.
bool App::shutter_active() const { return shutter_at && s.GetTicks() - shutter_at < 240; }

void App::shutter_overlay(SDL_Renderer* r) {
    if (!shutter_active()) return;
    const int alpha = 210 - int((s.GetTicks() - shutter_at) * 210 / 240);
    if (alpha <= 0) return;
    s.SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    s.SetRenderDrawColor(r, 255, 255, 255, Uint8(alpha));
    SDL_Rect all{0, 0, 640, 480};
    s.RenderFillRect(r, &all);
}

// The launcher's Menu button is held for a moment to see whether R joins it: the switcher opens on release.
bool App::album_chord(const SDL_Event& e, bool menu_button) {
    if (o.game_menu || !session_initialized || settings.page == Page::BootWarning) return false;
    const Uint32 now = s.GetTicks();
    if (launcher_menu_held && now - launcher_menu_at > 5000) launcher_menu_held = launcher_menu_chorded = false;
    if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_PRINTSCREEN && !e.key.repeat) {
        take_launcher_screenshot();
        return true;
    }
    if (e.type == SDL_JOYBUTTONDOWN && e.jbutton.button == 5) {
        launcher_r_held = true;
        if (launcher_menu_held) {
            launcher_menu_chorded = true;
            take_launcher_screenshot();
            return true;
        }
        return false;
    }
    if (e.type == SDL_JOYBUTTONUP && e.jbutton.button == 5) launcher_r_held = false;
    if (menu_button && e.type == SDL_JOYBUTTONDOWN) {
        if (launcher_r_held) {
            take_launcher_screenshot();
            launcher_menu_held = true;
            launcher_menu_chorded = true;
        } else {
            launcher_menu_held = true;
            launcher_menu_chorded = false;
            launcher_menu_at = now;
        }
        return true;
    }
    if (e.type == SDL_JOYBUTTONUP && joystick_source(e.jbutton.button) == control_menu && launcher_menu_held) {
        launcher_menu_held = false;
        if (!launcher_menu_chorded) launcher_switcher_shortcut();
        launcher_menu_chorded = false;
        return true;
    }
    return false;
}

std::string App::album_time(const AlbumShot& shot) const {
    // <YYYYMMDD-HHMMSS>[-n].png
    const auto name = fs::path(shot.path).filename().string();
    if (name.size() < 15 || name[8] != '-') return name;
    return name.substr(9, 2) + ":" + name.substr(11, 2) + ":" + name.substr(13, 2);
}

// The day as the player writes dates (System > Date format), with the year.
std::string App::album_date(const AlbumShot& shot) const {
    if (shot.date.size() != 8) return shot.date;
    std::tm day{};
    day.tm_year = std::atoi(shot.date.substr(0, 4).c_str()) - 1900;
    day.tm_mon = std::clamp(std::atoi(shot.date.substr(4, 2).c_str()), 1, 12) - 1;
    day.tm_mday = std::clamp(std::atoi(shot.date.substr(6, 2).c_str()), 1, 31);
    day.tm_hour = 12;
    const auto when = std::mktime(&day);
    return when == -1 ? shot.date : achievement_date(when);
}

fs::path App::album_thumb(const AlbumShot& shot) const {
    auto path = fs::path(shot.path);
    return path.parent_path() / (path.stem().string() + ".thumb.bmp");
}

// A caption over the first picture of each game, and over the first of each day inside it.
const char* App::album_group(int row) const {
    if (row < 0 || row >= int(album.size())) return nullptr;
    if (row == 0 || album[size_t(row)].key != album[size_t(row - 1)].key) return album[size_t(row)].title.c_str();
    return album[size_t(row)].date != album[size_t(row - 1)].date ? "" : nullptr;
}

void App::album_action(int code) {
    if (album_sharing) {
        if (code == 3 || code == 2) album_share_stop(); // B (or A) closes the sheet; the server stops with it
        else if (code == 8 || code == 9)
            settings.focus = std::clamp(settings.focus + (code == 8 ? -1 : 1), 0, std::max(0, int(album.size()) - 1));
        dirty = true;
        return;
    }
    if (album_viewing) {
        if (code == 2 || code == 12) open_photo(false);
        else if (code == 11) open_photo(true); // X: the lower screen
        else if (code == 3 || code == 4) album_viewing = false;
        else if ((code == 8 || code == 9 || code == 0 || code == 1) && !album.empty()) {
            const int next = std::clamp(settings.focus + (code == 8 || code == 0 ? -1 : 1), 0, int(album.size()) - 1);
            if (next != settings.focus) {
                settings.focus = next;
                album_view();
            }
        }
        dirty = true;
        return;
    }
    if (code == 3) {
        back_to(Page::LibraryOptions);
        settings.focus = library_row_count() - 2;
    } else if (code == 12) album_more();
    else if (album.empty()) return;
    else if (code == 8 || code == 9)
        settings.focus = std::clamp(settings.focus + (code == 8 ? -1 : 1), 0, int(album.size()) - 1);
    else if (code == 2 || code == 4) album_view();
    else if (code == 11) album_delete();
    dirty = true;
}

void App::album_view() {
    if (album.empty()) return;
    const auto session = o.data / "session";
    album_helper({"view", "--path", album[size_t(settings.focus)].path, "--top",
                  (session / "album-view-top.bmp").string(), "--bottom", (session / "album-view-bottom.bmp").string()},
                 "view");
}

void App::album_delete() {
    if (album.empty()) return;
    const auto path = album[size_t(settings.focus)].path;
    show_picker("Delete this screenshot?", {"Cancel", "Delete"}, 0, [this, path](int yes) {
        if (yes && album_helper({"delete", "--path", path}, "delete")) emit_sound(MenuCue::Unfavourite);
    });
}

void App::album_more() {
    if (album.empty()) {
        album_screens();
        return;
    }
    const auto shot = album[size_t(settings.focus)];
    std::vector<std::string> actions = {
        "Photo mode", "View", "Use as the game's cover", "Send in PictoChat", "Send to phone", "Delete", "Screenshot"};
    if (shot.key == "launcher") actions.erase(actions.begin() + 2);
    show_picker(shot.title, actions, 0, [this, actions, shot](int choice) {
        const auto& action = actions.at(size_t(choice));
        if (action == "Photo mode") open_photo(false);
        else if (action == "View") album_view();
        else if (action == "Delete") album_delete();
        else if (action == "Screenshot") album_screens();
        else if (action == "Send to phone") album_share_start();
        else if (action == "Send in PictoChat") {
            album_view_then_chat = true;
            album_view();
        } else album_helper({"cover", "--path", shot.path}, "cover");
    });
}

// Which screens a new picture keeps (screenshots/settings.ini, read by the game runner and the launcher alike).
void App::album_screens() {
    static const std::array<const char*, 3> values = {"both", "top", "bottom"};
    const auto current = fields(album_root() / "settings.ini")["screens"];
    const int selected = current == "top" ? 1 : current == "bottom" ? 2 : 0;
    show_picker("Screenshot", {"Both screens", "Top display", "Bottom display"}, selected, [this](int choice) {
        album_helper({"screens", "--value", values.at(size_t(std::clamp(choice, 0, 2)))}, "screens");
    });
}

void App::album_top(SDL_Renderer* r, int y) {
    if (album_sharing) {
        album_share_top(r, y);
        return;
    }
    if (album_viewing) {
        if (!media.draw(r, o.data / "session/album-view-top.bmp", {0, y, 640, 480}))
            centered(r, "Loading…", y + 230, 1, "muted");
        return;
    }
    settings_card(r, 24, y + top_card_y, 592, top_card_h);
    if (album.empty()) {
        ui_icon(r, SettingsIcon::Art, 320, y + 180, 64, "muted");
        centered(r, "No screenshots yet", y + 236, 2, "ink");
        wrapped(r, "In a game press Menu + R, or choose Screenshot in the Game menu. Menu + R works in the menus too.",
                64, y + 280, 18, 512, 3);
        return;
    }
    const auto& shot = album[size_t(std::clamp(settings.focus, 0, int(album.size()) - 1))];
    if (!media.draw(r, album_thumb(shot), {40, y + 104, 300, 288}))
        ui_icon(r, SettingsIcon::Art, 190, y + 248, 64, "muted");
    raw_wrapped(r, locale.text(shot.title), 364, y + 116, 26, 232, 2);
    label(r, album_date(shot), 364, y + 196, 20, "accent", 232);
    raw_label(r, album_time(shot), 364, y + 228, 20, "muted", 232);
    raw_label(r, std::to_string(settings.focus + 1) + " / " + std::to_string(album.size()), 364, y + 260, 16, "muted",
              232);
    wrapped(r, status == "READY" ? "" : status, 364, y + 300, 16, 232, 4);
}

void App::album_row(SDL_Renderer* r, int y, int i) {
    const auto& shot = album[size_t(i)];
    list_row(r, y, i, album_date(shot), album_time(shot), true, nullptr, false);
}

void App::album_warm() {
    if (settings.page != Page::Album) return;
    for (auto& panel : surfaces) {
        if (album_viewing) {
            media.warm(panel.renderer, o.data / "session/album-view-top.bmp");
            media.warm(panel.renderer, o.data / "session/album-view-bottom.bmp");
        } else if (!album.empty())
            media.warm(panel.renderer,
                       album_thumb(album[size_t(std::clamp(settings.focus, 0, int(album.size()) - 1))]));
    }
}

// ---- Send to phone ----------------------------------------------------------------------------------------------------
// album_share.py serves the album read-only while the sheet is open; the upper screen shows its QR code, address and
// code. B closes the sheet, leaving the Album closes it too, and the server stops itself when the launcher goes.
void App::album_share_start() {
    album_sharing = true;
    album_share_status.clear();
    album_share_qr.clear();
    album_share_token.clear();
    dirty = true;
    if (album_share_child > 0 || device_status["wifi_connected"] != "1") return;
    const fs::path app = o.app.empty() ? fs::current_path() : o.app;
    std::vector<std::string> args = {
        "python3",          "-B", (app / "runtime/album_share.py").string(), "serve", "--data", o.data.string(), "--ip",
        device_status["ip"]};
    std::error_code ec;
    fs::create_directories(o.data / "logs", ec);
    const pid_t child = fork();
    if (child == 0) {
        int log = ::open((o.data / "logs/album-share.log").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (log >= 0) {
            dup2(log, STDOUT_FILENO);
            dup2(log, STDERR_FILENO);
            ::close(log);
        }
        int none = ::open("/dev/null", O_RDONLY);
        if (none >= 0) {
            dup2(none, STDIN_FILENO);
            ::close(none);
        }
        std::vector<char*> argv;
        for (auto& arg : args) argv.push_back(arg.data());
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    if (child > 0) album_share_child = child;
}

void App::album_share_stop() {
    if (album_share_child > 0) {
        if (waitpid(album_share_child, nullptr, WNOHANG) == 0) {
            kill(album_share_child, SIGTERM);
            for (int i = 0; i < 100 && waitpid(album_share_child, nullptr, WNOHANG) == 0; i++) usleep(10000);
            if (waitpid(album_share_child, nullptr, WNOHANG) == 0) {
                kill(album_share_child, SIGKILL);
                waitpid(album_share_child, nullptr, 0);
            }
        }
        album_share_child = -1;
    }
    album_sharing = false;
    album_share_status.clear();
    album_share_qr.clear();
    dirty = true;
}

void App::album_share_poll(Uint32 now) {
    if (!album_sharing) return;
    if (settings.page != Page::Album) {
        album_share_stop();
        return;
    }
    if (now - album_share_poll_at < 200) return;
    album_share_poll_at = now;
    if (album_share_child > 0 && waitpid(album_share_child, nullptr, WNOHANG) != 0) album_share_child = -1;
    if (album_share_child <= 0 && device_status["wifi_connected"] == "1" && album_share_status.empty())
        album_share_start(); // Wi-Fi came on while the sheet was open
    auto status = fields(o.data / "session/album-share.ini");
    if (status["token"] == album_share_token) return;
    album_share_token = status["token"];
    album_share_status = std::move(status);
    album_share_qr.clear();
    std::ifstream rows(o.data / "session/album-share.qr");
    for (std::string row; std::getline(rows, row);)
        if (!row.empty()) album_share_qr.push_back(row);
    dirty = true;
}

void App::album_share_top(SDL_Renderer* r, int y) {
    settings_card(r, 24, y + top_card_y, 592, top_card_h);
    y += 22;
    const auto at = [&](const char* key) {
        auto found = album_share_status.find(key);
        return found == album_share_status.end() ? std::string() : found->second;
    };
    const int box = 232, bx = 40, by = y + 98, size = int(album_share_qr.size());
    const bool online = device_status["wifi_connected"] == "1";
    if (!online) {
        round_rect(r, bx, by, box, box, 16, "slot");
        ui_icon(r, SettingsIcon::Wifi, bx + box / 2, by + box / 2, 64, "muted");
    } else if (size >= 21 && size <= 57) draw_qr(r, album_share_qr, bx, by, box);
    else {
        round_rect(r, bx, by, box, box, 16, "slot");
        const std::string wait = locale.text("Starting…");
        const int ww = label_width(wait, 18, false);
        raw_label(r, wait, bx + (box - ww) / 2, centred_label_y(wait, 18, by + box / 2), 18, "muted", ww + 4);
    }
    const int tx = 296, tw = 600 - tx;
    label(r, "Send to phone", tx, y + 96, 24, "ink", tw);
    if (!online) wrapped(r, "Connect to Wi-Fi to send pictures to a phone or computer.", tx, y + 140, 18, tw, 4);
    else if (at("state") == "error") raw_wrapped(r, at("error"), tx, y + 140, 18, tw, 4);
    else {
        label(r, "Scan with your phone", tx, y + 138, 16, "muted", tw);
        label(r, "Or open on a computer", tx, y + 172, 16, "muted", tw);
        const std::string named = at("named"), address = at("address");
        auto bare = [](std::string link) { return link.rfind("http://", 0) == 0 ? link.substr(7) : link; };
        raw_label(r, bare(named.empty() ? address : named), tx, y + 194, 20, "ink", tw);
        if (!named.empty() && !address.empty()) raw_label(r, bare(address), tx, y + 222, 16, "muted", tw);
        label(r, "Code", tx, y + 254, 16, "muted", tw);
        raw_label(r, at("code"), tx, y + 274, 36, "accent", tw);
        if (at("state") == "connected") label(r, "Phone or computer connected", 40, y + 350, 18, "muted", 560);
    }
}
