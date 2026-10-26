#pragma once
// Add game files: a port that needs its owner's files (Steam folder, GOG installer, data.win)
// opens this page instead of starting. While it is open, runtime/port_files.py serves a web
// page on the handheld; the top screen shows its QR code for a phone, and the address and
// code for a computer's browser. Files land where the port expects them.
// Members of App in main.cpp; included once, right after the class.

// A port whose port.json says it is not ready to run, until files arrive or the owner plays it anyway.
bool App::port_needs_files(const Game& game) {
    if (o.port_menu || game.system != "ports" || is_port_entry(game.path)) return false;
    const fs::path folder = folder_for_game(game);
    if (fields(folder / "port.ini")["needs_files"] != "1") return false;
    auto added = fields(folder / "port-files.ini");
    return std::atoi(added["files"].c_str()) <= 0 && added["skipped"] != "1";
}

void App::port_files_open(const Game& game) {
    port_files_game = game;
    port_files_details = fields(folder_for_game(game) / "port.ini");
    port_files_status.clear();
    port_files_qr.clear();
    port_files_token.clear();
    settings.page = Page::PortFiles;
    settings.focus = 0;
    list_scroll = 0;
    port_files_start();
    dirty = true;
}

void App::port_files_start() {
    if (port_files_child > 0 || device_status["wifi_connected"] != "1") return;
    const fs::path app = o.app.empty() ? fs::current_path() : o.app;
    std::error_code ec;
    fs::create_directories(o.data / "logs", ec);
    std::vector<std::string> args = {"python3",
                                     "-B",
                                     (app / "runtime/port_files.py").string(),
                                     "serve",
                                     "--data",
                                     o.data.string(),
                                     "--app",
                                     app.string(),
                                     "--script",
                                     port_files_game.path.string(),
                                     "--ip",
                                     device_status["ip"]};
    const pid_t child = fork();
    if (child == 0) {
        int log = ::open((o.data / "logs/port-files.log").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
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
    if (child > 0) port_files_child = child;
}

void App::port_files_close() {
    if (port_files_child > 0) {
        if (waitpid(port_files_child, nullptr, WNOHANG) == 0) {
            kill(port_files_child, SIGTERM);
            for (int i = 0; i < 100 && waitpid(port_files_child, nullptr, WNOHANG) == 0; i++) usleep(10000);
            if (waitpid(port_files_child, nullptr, WNOHANG) == 0) {
                kill(port_files_child, SIGKILL);
                waitpid(port_files_child, nullptr, 0);
            }
        }
        port_files_child = -1;
    }
    port_files_status.clear();
    port_files_qr.clear();
}

void App::port_files_poll(Uint32 now) {
    if (settings.page != Page::PortFiles) {
        if (port_files_child > 0) port_files_close();
        return;
    }
    if (now - port_files_poll_at < 200) return;
    port_files_poll_at = now;
    if (port_files_child > 0 && waitpid(port_files_child, nullptr, WNOHANG) != 0) port_files_child = -1;
    if (port_files_child <= 0) {
        port_files_start(); // Wi-Fi came on while the page was open
        if (port_files_child <= 0 && !port_files_status.empty()) {
            port_files_status.clear();
            port_files_qr.clear();
            dirty = true;
        }
    }
    auto status = fields(o.data / "session/port-files.ini");
    if (status["token"] == port_files_token) return;
    port_files_token = status["token"];
    port_files_status = std::move(status);
    port_files_qr.clear();
    std::ifstream rows(o.data / "session/port-files.qr");
    for (std::string row; std::getline(rows, row);)
        if (!row.empty()) port_files_qr.push_back(row);
    dirty = true;
}

int App::port_files_added() {
    return std::atoi(fields(folder_for_game(port_files_game) / "port-files.ini")["files"].c_str());
}

// The top screen: the QR code for a phone, the address and code for a computer, and what is arriving.
void App::port_files_top(SDL_Renderer* r, int y) {
    settings_card(r, 24, y + 82, 592, 320);
    const auto at = [&](const char* key) {
        auto found = port_files_status.find(key);
        return found == port_files_status.end() ? std::string() : found->second;
    };
    const int box = 232, bx = 40, by = y + 98;
    const int size = int(port_files_qr.size());
    if (device_status["wifi_connected"] != "1") {
        round_rect(r, bx, by, box, box, 16, "slot");
        ui_icon(r, SettingsIcon::Wifi, bx + box / 2, by + box / 2, 64, "muted");
    } else if (size >= 21 && size <= 57) {
        // Dark modules on white with a four-module quiet zone, whatever the theme.
        s.SetRenderDrawColor(r, 255, 255, 255, 255);
        SDL_Rect paper{bx, by, box, box};
        s.RenderFillRect(r, &paper);
        const int scale = box / (size + 8), offset = (box - scale * size) / 2;
        s.SetRenderDrawColor(r, 0, 0, 0, 255);
        for (int row = 0; row < size; row++)
            for (int col = 0; col < size && col < int(port_files_qr[row].size()); col++)
                if (port_files_qr[row][col] == '1') {
                    SDL_Rect module{bx + offset + col * scale, by + offset + row * scale, scale, scale};
                    s.RenderFillRect(r, &module);
                }
    } else {
        round_rect(r, bx, by, box, box, 16, "slot");
        const std::string wait = locale.text("Starting…");
        const int ww = label_width(wait, 18, false);
        raw_label(r, wait, bx + (box - ww) / 2, centred_label_y(wait, 18, by + box / 2), 18, "muted", ww + 4);
    }
    const int tx = 296, tw = 600 - tx;
    raw_label(r, library_title(port_files_game.title), tx, y + 96, 24, "ink", tw);
    const std::string state = at("state");
    if (device_status["wifi_connected"] != "1") {
        wrapped(r, "Connect to Wi-Fi to send files from a phone or computer.", tx, y + 140, 18, tw, 4);
    } else if (!at("error").empty() && state == "error") {
        raw_wrapped(r, at("error"), tx, y + 140, 18, tw, 4);
    } else {
        label(r, "Scan with your phone", tx, y + 138, 16, "muted", tw);
        label(r, "Or open on a computer", tx, y + 172, 16, "muted", tw);
        const std::string named = at("named"), address = at("address");
        auto bare = [](std::string link) { return link.rfind("http://", 0) == 0 ? link.substr(7) : link; };
        raw_label(r, bare(named.empty() ? address : named), tx, y + 194, 20, "ink", tw);
        if (!named.empty() && !address.empty()) raw_label(r, bare(address), tx, y + 222, 16, "muted", tw);
        label(r, "Code", tx, y + 254, 16, "muted", tw);
        raw_label(r, at("code"), tx, y + 274, 36, "accent", tw);
    }
    // What is arriving, under the code: a bar while files come in, a note when they are all here.
    const long long done = std::atoll(at("done").c_str()), total = std::atoll(at("total").c_str());
    if (state == "receiving" || state == "unpacking") {
        const int bar = y + 352;
        label(r, state == "unpacking" ? "Unpacking files" : "Receiving files", 40, bar - 26, 16, "muted", 300);
        const std::string count = at("files_done") + " / " + at("files_total");
        const int cw = label_width(count, 16, false);
        raw_label(r, count, 600 - cw, bar - 26, 16, "muted", cw + 4);
        round_rect(r, 40, bar, 560, 14, 7, "slot");
        const int fill = total > 0 ? int(std::clamp(done * 560 / total, 0LL, 560LL)) : 0;
        if (fill > 0) fill_round(r, 40, bar, std::max(fill, 14), 14, 7, theme.at("accent"));
        raw_label(r, at("file"), 40, bar + 18, 14, "muted", 560);
    } else if (state == "received" || port_files_added() > 0) {
        ui_icon(r, SettingsIcon::Check, 52, y + 362, 22, "accent");
        label(r, "Files arrived. Choose Play to start.", 72, y + 350, 18, "ink", 528);
    } else if (state == "connected") {
        label(r, "Phone or computer connected", 40, y + 350, 18, "muted", 560);
    } else {
        const std::string target = port_files_details["target"];
        const std::string where = "ports/" + port_files_details["id"] + (target.empty() ? "" : "/" + target);
        raw_label(r, locale.text("Goes into") + "  " + where, 40, y + 352, 16, "muted", 560);
    }
}

void App::port_files_row(SDL_Renderer* r, int y, int i) {
    const int added = port_files_added();
    if (i == 0)
        list_row(r, y, i, "Play",
                 added > 0 ? locale.text("Files added") + " · " + std::to_string(added)
                           : locale.text("Needs your files"));
    else if (i == 1) list_row(r, y, i, "How to get the files", "");
    else list_row(r, y, i, "Back", "");
}

void App::port_files_action(int code) {
    if (code == 3) {
        port_files_close();
        back_to(Page::Library);
    } else if (code == 8 || code == 9) settings.focus = std::clamp(settings.focus + (code == 8 ? -1 : 1), 0, 2);
    else if (code == 2 || code == 4) {
        if (settings.focus == 0) {
            // Played without files once: the port starts straight away from now on, as before.
            const fs::path record = folder_for_game(port_files_game) / "port-files.ini";
            if (port_files_added() <= 0) {
                auto kept = fields(record);
                kept["skipped"] = "1";
                std::error_code ec;
                fs::create_directories(record.parent_path(), ec);
                const fs::path pending = record.string() + ".pending";
                {
                    std::ofstream out(pending);
                    for (const auto& [key, value] : kept) out << key << '=' << value << '\n';
                }
                fs::rename(pending, record, ec);
            }
            port_files_close();
            back_to(Page::Library);
            launch();
        } else if (settings.focus == 1) {
            const std::string text = port_files_details["instructions"];
            show_detail("How to get the files", text.empty() ? locale.text("This port lists no instructions.") : text);
        } else {
            port_files_close();
            back_to(Page::Library);
        }
    }
    dirty = true;
}
