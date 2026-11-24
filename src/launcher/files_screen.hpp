#pragma once
// Files (Settings > Files, docs/NETWORK_PLAN.md section 3): the card's folders on the lower screen, what the
// selected thing is on the upper one. The launcher lists folders itself; every change to the card goes through
// runtime/files.py, one command at a time, which also owns the "are you sure" rules for saves and the system folder
// (it stops with state=confirm and the page asks, then runs the same command again with --confirmed).
// Members of App in main.cpp; included once, right after the class.

// L and R step through these. RGDS_FILES_ROOTS="Name=/path;Name=/path" replaces them (host previews and tests).
void App::files_setup_roots() {
    files_roots.clear();
    if (const char* custom = std::getenv("RGDS_FILES_ROOTS")) {
        std::stringstream list(custom);
        for (std::string item; std::getline(list, item, ';');) {
            const auto at = item.find('=');
            if (at != std::string::npos && at > 0) files_roots.push_back({item.substr(0, at), item.substr(at + 1)});
        }
    }
    if (files_roots.empty()) {
        const fs::path card = o.rgds_os ? fs::path("/data") : o.data;
        files_roots.push_back({"Games", o.rgds_os ? card / "roms" : (o.roots.empty() ? card : o.roots.front())});
        files_roots.push_back({"Inbox", card / "inbox"});
        files_roots.push_back({"Screenshots", album_root()});
        if (!o.music_dirs.empty()) files_roots.push_back({"Music", o.music_dirs.front()});
        files_roots.push_back({"Card", card});
    }
    // Network places (network/sources.ini, written by runtime/net_sources.py), or one Network root to add the first.
    bool any = false;
    std::ifstream ini(o.data / "network/sources.ini");
    FilesRoot place;
    auto keep = [&]() {
        if (!place.source.empty() && (place.kind == "smb" || place.kind == "sftp" || place.kind == "local")) {
            if (place.name.empty()) place.name = place.source;
            files_roots.push_back(place);
            any = true;
        }
        place = FilesRoot{};
    };
    for (std::string line; std::getline(ini, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() > 2 && line.front() == '[' && line.back() == ']') {
            keep();
            place.source = line.substr(1, line.size() - 2);
            continue;
        }
        const auto at = line.find('=');
        if (at == std::string::npos) continue;
        auto key = line.substr(0, at), value = line.substr(at + 1);
        while (!key.empty() && key.back() == ' ') key.pop_back();
        while (!value.empty() && value.front() == ' ') value.erase(value.begin());
        if (key == "type") place.kind = value;
        else if (key == "name") place.name = value;
        else if (key == "host" && place.name.empty()) place.name = value;
    }
    keep();
    if (!any) files_roots.push_back({"Network", {}, "+", "network"});
    files_roots.push_back({"Trash", o.data / "trash"});
}

bool App::files_remote_root() const { return !files_roots.empty() && !files_roots[size_t(files_root)].source.empty(); }

fs::path App::files_root_named(const std::string& name) const {
    for (const auto& root : files_roots)
        if (root.name == name && root.source.empty()) return root.path;
    return {};
}

// Step L/R to another root: a folder on the card is listed now, a network place is asked for its top folder.
void App::files_open_root(int index) {
    files_root = std::clamp(index, 0, int(files_roots.size()) - 1);
    settings.focus = 0;
    files_marked.clear();
    files_list_error.clear();
    if (files_remote_root()) files_remote_enter("");
    else files_enter(files_roots[size_t(files_root)].path);
}

void App::open_files() {
    settings.page = Page::Files; // a Settings root row since 2026-11-24: Back returns there
    settings.focus = 0;
    files_setup_roots();
    files_open_root(files_root);
}

bool App::files_at_root() const {
    if (files_roots.empty()) return true;
    if (files_remote_root()) return files_remote.empty();
    std::error_code ec;
    return fs::equivalent(files_dir, files_roots[size_t(files_root)].path, ec) ||
           files_dir == files_roots[size_t(files_root)].path;
}

void App::files_enter(const fs::path& dir, const std::string& select) {
    if (const auto* row = files_focused()) files_memory[files_dir.string()] = row->name;
    files_dir = dir;
    files_marked.clear();
    std::string want = select;
    if (const auto remembered = files_memory.find(dir.string()); want.empty() && remembered != files_memory.end())
        want = remembered->second; // by name, so a different sort since still finds it
    settings.focus = 0;
    files_load(want);
    settings.focus = files_rows.empty() ? 0 : std::clamp(settings.focus, 0, int(files_rows.size()) - 1);
    files_peek_request();
}

// Dot files, macOS clutter and half-written .part files hide; files_sort_rows puts the rest in order.
void App::files_load(const std::string& select) {
    std::vector<FileRow> rows;
    std::error_code ec;
    for (fs::directory_iterator it(files_dir, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end && rows.size() < 5000; it.increment(ec)) {
        const auto name = it->path().filename().string();
        if (name.empty() || name[0] == '.' || (name.size() > 5 && name.compare(name.size() - 5, 5, ".part") == 0))
            continue;
        std::error_code entry_ec;
        FileRow row{name, it->is_directory(entry_ec), 0, 0};
        if (!row.dir) row.size = it->file_size(entry_ec);
        const auto written = it->last_write_time(entry_ec);
        if (!entry_ec)
            row.mtime =
                std::chrono::system_clock::to_time_t(std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    written - fs::file_time_type::clock::now() + std::chrono::system_clock::now()));
        rows.push_back(std::move(row));
    }
    files_rows = std::move(rows);
    files_sort_rows();
    files_tapped = -1;
    const auto space = fs::space(files_dir, ec);
    files_free = ec ? 0 : space.available;
    for (auto it = files_marked.begin(); it != files_marked.end();)
        it = std::any_of(files_rows.begin(), files_rows.end(), [&](const FileRow& row) { return row.name == *it; })
                 ? std::next(it)
                 : files_marked.erase(it);
    if (!select.empty())
        for (size_t i = 0; i < files_rows.size(); i++)
            if (files_rows[i].name == select) settings.focus = int(i);
    if (settings.page == Page::Files)
        settings.focus = files_rows.empty() ? 0 : std::clamp(settings.focus, 0, int(files_rows.size()) - 1);
    dirty = true;
}

// A network place's folder: net_sources.py ls answers in session/files-ls.tsv; the rows arrive in files_net_done.
void App::files_remote_enter(const std::string& relative, const std::string& select) {
    if (const auto* row = files_focused())
        files_memory["net:" + files_roots[size_t(files_root)].source + ":" + files_remote] = row->name;
    files_remote = relative;
    files_rows.clear();
    files_marked.clear();
    files_list_error.clear();
    files_select_after = select;
    settings.focus = 0;
    const auto& root = files_roots[size_t(files_root)];
    if (root.source == "+") return; // nothing added yet: the page explains and Y adds a place
    files_loading = true;
    if (files_net_child > 0 && files_net_task == "ls") {
        kill(files_net_child, SIGTERM); // the folder it was reading is no longer the one on screen
        waitpid(files_net_child, nullptr, 0);
        files_net_child = -1;
    }
    if (files_net_child > 0) files_net_waiting = true; // a setup step is running: list when it is done
    else files_net_run({"ls", root.source, relative}, "ls");
    dirty = true;
}

std::vector<std::vector<std::string>> App::files_answer(const fs::path& path, std::string& error) {
    std::vector<std::vector<std::string>> rows;
    std::ifstream file(path);
    std::string line;
    error.clear();
    if (!std::getline(file, line)) {
        error = locale.text("That did not work.");
        return rows;
    }
    if (line.rfind("error\t", 0) == 0) {
        error = line.substr(6);
        return rows;
    }
    while (std::getline(file, line) && rows.size() < 5000) {
        std::vector<std::string> cells;
        std::stringstream split(line);
        for (std::string cell; std::getline(split, cell, '\t');) cells.push_back(cell);
        rows.push_back(std::move(cells));
    }
    return rows;
}

// One net_sources.py command at a time (listing, finding servers, adding a place); a password goes on stdin.
void App::files_net_run(std::vector<std::string> command, const std::string& task, const std::string& input) {
    if (files_net_child > 0) return;
    const fs::path app = o.app.empty() ? fs::current_path() : o.app;
    std::error_code ec;
    fs::create_directories(o.data / "session", ec);
    fs::create_directories(o.data / "logs", ec);
    const auto out = o.data / ("session/files-" + task + ".tsv");
    fs::remove(out, ec);
    std::vector<std::string> args = {
        "python3", "-B", (app / "runtime/net_sources.py").string(), "--data", o.data.string(), "--out", out.string()};
    args.insert(args.end(), command.begin(), command.end());
    int feed[2] = {-1, -1};
    if (!input.empty() && pipe(feed) != 0) return;
    const pid_t child = fork();
    if (child == 0) {
        if (feed[0] >= 0) {
            dup2(feed[0], STDIN_FILENO);
            ::close(feed[0]);
            ::close(feed[1]);
        }
        int log = ::open((o.data / "logs/files.log").c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
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
    if (feed[0] >= 0) {
        ::close(feed[0]);
        if (child > 0) {
            const std::string line = input + "\n";
            (void)!::write(feed[1], line.data(), line.size());
        }
        ::close(feed[1]);
    }
    if (child < 0) return;
    files_net_child = child;
    files_net_task = task;
    files_net_out = out;
    dirty = true;
}

const App::FileRow* App::files_focused() const {
    if (files_rows.empty()) return nullptr;
    return &files_rows[size_t(std::clamp(settings.focus, 0, int(files_rows.size()) - 1))];
}

// What a command acts on: the marked rows, or the selected one.
std::vector<fs::path> App::files_selection() const {
    std::vector<fs::path> chosen;
    for (const auto& row : files_rows)
        if (files_marked.count(row.name)) chosen.push_back(files_dir / row.name);
    if (chosen.empty())
        if (const auto* row = files_focused()) chosen.push_back(files_dir / row->name);
    return chosen;
}

std::vector<std::string> App::files_remote_selection() const {
    std::vector<std::string> chosen;
    const auto inside = [&](const std::string& name) {
        return files_remote.empty() ? name : files_remote + "/" + name;
    };
    for (const auto& row : files_rows)
        if (files_marked.count(row.name)) chosen.push_back(inside(row.name));
    if (chosen.empty())
        if (const auto* row = files_focused()) chosen.push_back(inside(row->name));
    return chosen;
}

// Copies from a network place go through the transfer queue (runtime/transfers.py): it resumes after a dropped
// Wi-Fi or sleep, checks sizes, and puts games in their system's folder. One process adds and then runs the queue.
void App::files_queue(const std::string& source, const std::vector<std::string>& paths, const std::string& to) {
    if (paths.empty()) return;
    if (files_queue_child > 0 && waitpid(files_queue_child, nullptr, WNOHANG) == 0) {
        // The running queue picks new jobs up itself, but this adding process must wait for the last one's answer.
        status = locale.text("Wait a moment, then try again.");
        emit_sound(MenuCue::Error);
        return;
    }
    const fs::path app = o.app.empty() ? fs::current_path() : o.app;
    std::error_code ec;
    fs::create_directories(o.data / "session", ec);
    fs::remove(o.data / "session/files-queue.tsv", ec);
    std::vector<std::string> args = {"python3",       "-B",       (app / "runtime/transfers.py").string(),    "--data",
                                     o.data.string(), "--status", (o.data / "session/transfers.ini").string()};
    const auto games = files_root_named("Games"), inbox = files_root_named("Inbox");
    if (!games.empty()) args.insert(args.end(), {"--roms", games.string()});
    if (!inbox.empty()) args.insert(args.end(), {"--inbox", inbox.string()});
    args.insert(args.end(), {"add", source});
    args.insert(args.end(), paths.begin(), paths.end());
    args.insert(args.end(), {"--to", to, "--run", "--answer", (o.data / "session/files-queue.tsv").string()});
    const pid_t child = fork();
    if (child == 0) {
        setsid(); // the copy carries on if the menu restarts
        int log = ::open((o.data / "logs/transfers.log").c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
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
    if (child > 0) {
        files_queue_child = child;
        files_queue_answered = false;
        status = locale.text(paths.size() == 1 ? "Getting it…" : "Getting them…");
        emit_sound(MenuCue::Save);
    }
    dirty = true;
}

const Game* App::files_game(const fs::path& path) const {
    for (const auto& game : all_games)
        if (game.path == path) return &game;
    return nullptr;
}

std::string App::files_size(uintmax_t bytes) {
    static const char* units[] = {"KB", "MB", "GB", "TB"};
    if (bytes < 1024) return std::to_string(bytes) + " B";
    double value = double(bytes);
    int unit = -1;
    while (value >= 1024 && unit < 3) {
        value /= 1024;
        unit++;
    }
    std::ostringstream out;
    out << std::fixed << std::setprecision(value < 10 ? 1 : 0) << value << ' ' << units[unit];
    return out.str();
}

void App::files_action(int code) {
    if (files_setup.step == FilesSetup::Key && code == 3) {
        files_setup = FilesSetup{}; // B leaves the key screen: the place is not added
        dirty = true;
        return;
    }
    if (code == 3) {
        if (!files_marked.empty()) files_marked.clear();
        else files_go_up(true);
    } else if (code == 0) files_go_up(false); // Left: the folder above, as in a desktop file manager
    else if (code == 1) {                     // Right: into the selected folder
        if (const auto* row = files_focused(); row && row->dir) files_action(2);
    } else if (code == 13 || code == 14) {
        if (files_roots.size() > 1) {
            const int n = int(files_roots.size());
            files_open_root((files_root + (code == 13 ? n - 1 : 1)) % n);
            emit_sound(MenuCue::Navigate);
        }
    } else if (code == 12) files_more();
    else if (code == 8 || code == 9) {
        if (!files_rows.empty()) {
            const int next = std::clamp(settings.focus + (code == 8 ? -1 : 1), 0, int(files_rows.size()) - 1);
            if (next != settings.focus) {
                settings.focus = next;
                files_peek_request();
            }
        }
    } else if (code == 11) {
        if (const auto* row = files_focused()) {
            if (!files_marked.erase(row->name)) files_marked.insert(row->name);
            emit_sound(files_marked.count(row->name) ? MenuCue::Enable : MenuCue::Disable);
            if (settings.focus + 1 < int(files_rows.size())) {
                settings.focus++;
                files_peek_request();
            }
        }
    } else if (code == 2 || code == 4) {
        if (files_setup.step == FilesSetup::Key) files_setup_next();
        else if (const auto* row = files_focused()) {
            if (row->dir && files_remote_root())
                files_remote_enter(files_remote.empty() ? row->name : files_remote + "/" + row->name);
            else if (row->dir) files_enter(files_dir / row->name);
            else files_more();
        } else files_more();
    }
    dirty = true;
}

// Y (and A on a file): what can be done here, the selection's actions first.
void App::files_more() {
    if (files_child > 0) {
        status = "Wait for the current task to finish.";
        emit_sound(MenuCue::Error);
        return;
    }
    if (files_remote_root()) {
        files_remote_more();
        return;
    }
    const auto chosen = files_selection();
    const auto* row = files_focused();
    const bool trash = files_roots[size_t(files_root)].name == "Trash";
    std::vector<std::string> actions;
    if (!files_clip.empty()) actions.push_back(files_clip_move ? "Move here" : "Paste here");
    if (!chosen.empty()) {
        actions.push_back("Copy");
        actions.push_back("Move");
        if (chosen.size() == 1) actions.push_back("Rename");
        actions.push_back(trash ? "Delete for good" : "Delete");
    }
    if (chosen.size() == 1 && row && !row->dir) {
        auto ext = fs::path(row->name).extension().string();
        for (auto& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (ext == ".zip") actions.push_back("Extract here");
    }
    if (!trash) actions.push_back("New folder");
    if (files_rows.size() > 1) actions.push_back("Sort by");
    if (!files_marked.empty()) actions.push_back("Clear marks");
    if (!files_clip.empty()) actions.push_back("Cancel paste");
    if (trash && !files_rows.empty()) actions.push_back("Empty trash");
    const std::string title = files_marked.size() > 1 ? locale.text(std::to_string(files_marked.size()) + " items")
                              : chosen.size() == 1    ? chosen.front().filename().string()
                                                      : files_roots[size_t(files_root)].name;
    show_picker(title, actions, 0, [this, actions, chosen](int choice) {
        const auto& action = actions.at(size_t(choice));
        if (action == "Copy" || action == "Move") {
            files_clip_remote.clear();
            files_clip_source.clear();
            files_clip = chosen;
            files_clip_move = action == "Move";
            files_marked.clear();
            status = locale.text(action == "Move" ? "Open the folder to move to, then press Y."
                                                  : "Open the folder to copy to, then press Y.");
        } else if ((action == "Paste here") && !files_clip_source.empty()) {
            files_queue(files_clip_source, files_clip_remote, files_dir.string());
            files_clip.clear();
            files_clip_remote.clear();
            files_clip_source.clear();
        } else if (action == "Paste here" || action == "Move here") {
            std::vector<std::string> command = {files_clip_move ? "move" : "copy"};
            for (const auto& path : files_clip) command.push_back(path.string());
            command.insert(command.end(), {"--to", files_dir.string()});
            files_run(command, files_clip_move ? "move" : "copy");
        } else if (action == "Cancel paste") {
            files_clip.clear();
            files_clip_remote.clear();
            files_clip_source.clear();
        } else if (action == "Clear marks") files_marked.clear();
        else if (action == "Sort by") files_set_sort(-1);
        else if (action == "Rename") {
            files_edit_target = chosen.front();
            files_edit_seed = chosen.front().filename().string();
            begin_edit(14);
        } else if (action == "New folder") {
            files_edit_target = files_dir;
            files_edit_seed.clear();
            begin_edit(15);
        } else if (action == "Delete" || action == "Delete for good") {
            const std::string question =
                chosen.size() == 1
                    ? locale.text(action == "Delete" ? "Move this to the trash?" : "Delete this for good?")
                    : locale.text(action == "Delete" ? "Move these to the trash?" : "Delete these for good?");
            show_picker(question, {"Cancel", action}, 0, [this, chosen](int yes) {
                if (!yes) return;
                std::vector<std::string> command = {"delete"};
                for (const auto& path : chosen) command.push_back(path.string());
                files_run(command, "delete");
            });
        } else if (action == "Extract here") files_run({"extract", chosen.front().string()}, "extract");
        else if (action == "Empty trash")
            show_picker("Empty the trash for good?", {"Cancel", "Empty trash"}, 0, [this](int yes) {
                if (yes) files_run({"empty-trash"}, "empty-trash");
            });
        dirty = true;
    });
}

// Y on a network place: get the selection (into the library, the inbox, or a folder chosen later), or manage places.
void App::files_remote_more() {
    const auto& root = files_roots[size_t(files_root)];
    const auto chosen = files_remote_selection();
    std::vector<std::string> actions;
    if (root.source != "+" && !chosen.empty() && !files_loading) {
        actions.insert(actions.end(), {"Add to library", "Download to Inbox", "Copy"});
        if (!files_marked.empty()) actions.push_back("Clear marks");
    }
    if (root.source != "+" && !files_loading) actions.push_back("Refresh");
    actions.push_back("Add network place");
    if (root.source != "+") actions.push_back("Remove this place");
    if (root.source != "+" && files_rows.size() > 1) actions.push_back("Sort by");
    const std::string title = files_marked.size() > 1 ? locale.text(std::to_string(files_marked.size()) + " items")
                              : chosen.size() == 1 && root.source != "+"
                                  ? chosen.front().substr(chosen.front().rfind('/') + 1)
                                  : root.name;
    const auto source = root.source;
    show_picker(title, actions, 0, [this, actions, chosen, source](int choice) {
        const auto& action = actions.at(size_t(choice));
        if (action == "Add to library") files_queue(source, chosen, "library");
        else if (action == "Download to Inbox") {
            const auto inbox = files_root_named("Inbox");
            files_queue(source, chosen, inbox.empty() ? "library" : inbox.string());
        } else if (action == "Copy") {
            files_clip.clear();
            for (const auto& path : chosen) files_clip.push_back(path);
            files_clip_remote = chosen;
            files_clip_source = source;
            files_clip_move = false;
            files_marked.clear();
            status = locale.text("Open the folder to copy to, then press Y.");
        } else if (action == "Clear marks") files_marked.clear();
        else if (action == "Refresh") files_remote_enter(files_remote);
        else if (action == "Sort by") files_set_sort(-1);
        else if (action == "Add network place") files_setup_begin();
        else if (action == "Remove this place")
            show_picker("Remove this place?", {"Cancel", "Remove"}, 0, [this, source](int yes) {
                if (yes) files_net_run({"remove", source}, "remove");
            });
        dirty = true;
    });
}

// Adding a place: what kind, which server (found on the network or typed), who signs in, then the share or folder.
void App::files_setup_begin() {
    files_setup = FilesSetup{};
    show_picker("Add network place", {"Windows, Mac or NAS (SMB)", "Linux or Raspberry Pi (SFTP)"}, 0,
                [this](int choice) {
                    files_setup.kind = choice == 0 ? "smb" : "sftp";
                    files_setup.step = FilesSetup::Finding;
                    files_net_run({"discover"}, "discover");
                });
}

void App::files_setup_next() {
    auto& setup = files_setup;
    switch (setup.step) {
    case FilesSetup::Host: begin_edit(17); return; // who signs in
    case FilesSetup::User:
        if (setup.kind == "smb") {
            if (setup.user.empty()) {
                setup.step = FilesSetup::Shares; // a guest: straight to the shares
                files_net_run({"shares", "--host", setup.host}, "shares");
            } else begin_edit(18);
        } else
            show_picker("Sign in with", {"Password", "This handheld's key"}, 0, [this](int choice) {
                if (choice == 0) begin_edit(18);
                else {
                    files_setup.step = FilesSetup::Finding;
                    files_net_run({"key"}, "key");
                }
            });
        return;
    case FilesSetup::Password:
        if (setup.kind == "smb") {
            setup.step = FilesSetup::Shares;
            files_net_run({"shares", "--host", setup.host, "--user", setup.user, "--password-stdin"}, "shares",
                          setup.password);
        } else begin_edit(19);
        return;
    case FilesSetup::Key:
        setup.step = FilesSetup::Password;
        begin_edit(19);
        return;
    case FilesSetup::Share:
    case FilesSetup::Path: {
        setup.step = FilesSetup::Adding;
        std::vector<std::string> command = {
            "add",    setup.kind, "--name", setup.name.empty() ? setup.host : setup.name,
            "--host", setup.host, "--user", setup.user,
            "--port", setup.port, "--path", setup.path};
        if (setup.kind == "smb") command.insert(command.end(), {"--share", setup.share});
        if (!setup.password.empty()) command.push_back("--password-stdin");
        files_net_run(command, "add", setup.password);
        std::fill(setup.password.begin(), setup.password.end(), '\0');
        setup.password.clear();
        return;
    }
    default: return;
    }
}

// A net_sources.py command ended: rows for a listing, the next setup step, or a message.
void App::files_net_done() {
    std::string error;
    const auto rows = files_answer(files_net_out, error);
    const auto task = files_net_task;
    files_net_task.clear();
    auto& setup = files_setup;
    if (task == "ls") {
        files_loading = false;
        files_list_error = error;
        files_rows.clear();
        for (const auto& cells : rows) {
            if (cells.size() < 4 || cells[0].empty()) continue;
            files_rows.push_back({cells[0], cells[1] == "1", uintmax_t(std::strtoull(cells[2].c_str(), nullptr, 10)),
                                  std::time_t(std::strtoll(cells[3].c_str(), nullptr, 10))});
        }
        files_sort_rows();
        files_free = 0;
        const auto remembered = files_memory.find("net:" + files_roots[size_t(files_root)].source + ":" + files_remote);
        if (files_select_after.empty() && remembered != files_memory.end()) files_select_after = remembered->second;
        settings.focus = 0;
        files_tapped = -1;
        for (size_t i = 0; i < files_rows.size(); i++)
            if (files_rows[i].name == files_select_after) settings.focus = int(i);
        files_select_after.clear();
        settings.focus = files_rows.empty() ? 0 : std::clamp(settings.focus, 0, int(files_rows.size()) - 1);
        if (!error.empty()) emit_sound(MenuCue::Error);
    } else if (task == "discover") {
        files_found.clear();
        std::vector<std::string> options;
        for (const auto& cells : rows)
            if (cells.size() >= 5 && cells[0] == setup.kind) {
                files_found.push_back(cells);
                options.push_back(cells[1] + (cells[3].empty() ? "" : " · " + cells[3]));
            }
        options.push_back("Type an address");
        setup.step = FilesSetup::Choosing;
        show_picker(files_found.empty() ? "No servers found" : "Servers on this network", options, 0,
                    [this](int choice) {
                        if (choice < int(files_found.size())) {
                            const auto& found = files_found[size_t(choice)];
                            files_setup.name = found[1];
                            files_setup.host = found[3].empty() ? found[2] : found[3];
                            files_setup.port = found[4] == "22" || found[4] == "445" ? "" : found[4];
                            files_setup.step = FilesSetup::Host;
                            files_setup_next();
                        } else begin_edit(16);
                    });
    } else if (task == "key") {
        if (error.empty() && !rows.empty() && !rows[0].empty()) {
            setup.key = rows[0][0];
            setup.step = FilesSetup::Key; // the upper screen shows the key; A goes on when it is on the server
        } else {
            setup = FilesSetup{};
            status = error;
            emit_sound(MenuCue::Error);
        }
    } else if (task == "shares") {
        if (!error.empty() || rows.empty()) {
            setup = FilesSetup{};
            status = error.empty() ? locale.text("That server shares no folders.") : error;
            emit_sound(MenuCue::Error);
        } else {
            std::vector<std::string> shares;
            for (const auto& cells : rows)
                if (!cells.empty()) shares.push_back(cells[0]);
            setup.step = FilesSetup::Choosing;
            show_picker("Choose a share", shares, 0, [this, shares](int choice) {
                files_setup.share = shares.at(size_t(choice));
                if (files_setup.name.empty()) files_setup.name = files_setup.host;
                files_setup.name += " · " + files_setup.share;
                files_setup.step = FilesSetup::Share;
                files_setup_next();
            });
        }
    } else if (task == "add") {
        if (!error.empty() || rows.empty() || rows[0].empty()) {
            setup = FilesSetup{};
            status = error.empty() ? locale.text("That did not work.") : error;
            emit_sound(MenuCue::Error);
        } else {
            setup.added = rows[0][0];
            setup.step = FilesSetup::Testing;
            files_net_run({"test", setup.added}, "test");
        }
    } else if (task == "test") {
        const auto added = setup.added;
        setup = FilesSetup{};
        if (error.empty()) {
            status = locale.text("Added.");
            emit_sound(MenuCue::Save);
            files_show_place(added);
        } else {
            emit_sound(MenuCue::Error);
            show_picker(error, {"Keep it anyway", "Remove it"}, 0, [this, added](int choice) {
                if (choice == 1) files_net_run({"remove", added}, "remove");
                else files_show_place(added);
            });
        }
    } else if (task == "remove") {
        status = locale.text("Removed.");
        files_setup_roots();
        files_open_root(std::min(files_root, int(files_roots.size()) - 1));
    }
    if (files_net_waiting && files_net_child <= 0) {
        files_net_waiting = false;
        if (files_remote_root()) files_remote_enter(files_remote, files_select_after);
    }
    dirty = true;
}

void App::files_show_place(const std::string& source) {
    files_setup_roots();
    for (size_t i = 0; i < files_roots.size(); i++)
        if (files_roots[i].source == source) {
            files_open_root(int(i));
            return;
        }
    files_open_root(files_root);
}

// The Keyboard page's Done (or B) for Rename (14), New folder (15) and a new place's address, user, password and
// folder (16-19).
void App::files_edited(bool commit) {
    const int field = edit_field;
    const auto name = edit_buffer;
    s.StopTextInput();
    settings.page = Page::Files;
    if (field >= 16) {
        std::fill(edit_buffer.begin(), edit_buffer.end(), '\0');
        edit_buffer.clear();
        dirty = true;
        if (!commit || (field == 16 && name.empty())) {
            files_setup = FilesSetup{}; // B anywhere in the setup leaves it
            return;
        }
        auto& setup = files_setup;
        if (field == 16) {
            setup.host = name;
            const auto colon = name.rfind(':');
            if (colon != std::string::npos && name.find(':') == colon) { // host:port, not an IPv6 address
                setup.host = name.substr(0, colon);
                setup.port = name.substr(colon + 1);
            }
            setup.step = FilesSetup::Host;
        } else if (field == 17) {
            setup.user = name;
            setup.step = FilesSetup::User;
        } else if (field == 18) {
            setup.password = name;
            setup.step = FilesSetup::Password;
        } else {
            setup.path = name;
            setup.step = FilesSetup::Path;
        }
        files_setup_next();
        return;
    }
    if (commit && !name.empty() && !(field == 14 && name == files_edit_seed)) {
        if (field == 14) files_run({"rename", files_edit_target.string(), name}, "rename");
        else files_run({"mkdir", files_edit_target.string(), name}, "mkdir");
    }
    edit_buffer.clear();
    dirty = true;
}

void App::files_run(std::vector<std::string> command, const std::string& task, bool confirmed) {
    if (files_child > 0) return;
    const fs::path app = o.app.empty() ? fs::current_path() : o.app;
    const auto status_file = o.data / "session/files.ini";
    std::error_code ec;
    fs::create_directories(o.data / "logs", ec);
    fs::create_directories(o.data / "session", ec);
    fs::remove(status_file, ec);
    std::vector<std::string> args = {
        "python3",           "-B", (app / "runtime/files.py").string(), "--data", o.data.string(), "--status",
        status_file.string()};
    if (confirmed) args.push_back("--confirmed");
    args.insert(args.end(), command.begin(), command.end());
    const pid_t child = fork();
    if (child == 0) {
        int log = ::open((o.data / "logs/files.log").c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
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
    if (child < 0) return;
    files_child = child;
    files_task = task;
    files_command = std::move(command);
    files_progress.clear();
    dirty = true;
}

void App::files_poll(Uint32 now) {
    if (files_peek_child > 0 && waitpid(files_peek_child, nullptr, WNOHANG) == files_peek_child) {
        files_peek_child = -1;
        files_peek_read();
        if (!files_peek_wanted.empty() && files_peek_wanted != files_peeked) files_peek_request();
    }
    if (files_net_child > 0 && waitpid(files_net_child, nullptr, WNOHANG) == files_net_child) {
        files_net_child = -1;
        files_net_done();
    }
    if (files_queue_child > 0 && !files_queue_answered) {
        std::string error;
        const auto answer = o.data / "session/files-queue.tsv";
        if (fs::exists(answer)) {
            files_answer(answer, error);
            files_queue_answered = true;
            if (!error.empty()) {
                status = error;
                emit_sound(MenuCue::Error);
            }
            dirty = true;
        }
    }
    if (files_queue_child > 0 && waitpid(files_queue_child, nullptr, WNOHANG) == files_queue_child)
        files_queue_child = -1;
    // The queue's progress, for the strip on the upper screen; a placed game makes the library look again.
    if ((settings.page == Page::Files || files_queue_child > 0) && now - files_transfer_at >= 500) {
        files_transfer_at = now;
        auto transfer = fields(o.data / "session/transfers.ini");
        if (transfer != files_transfer) {
            if (!files_transfer_serial.empty() && transfer["serial"] != files_transfer_serial) {
                files_library_changed = true;
                if (settings.page == Page::Files && !files_remote_root()) files_load();
            }
            files_transfer_serial = transfer["serial"];
            files_transfer = std::move(transfer);
            dirty = true;
        }
    }
    if (files_child <= 0) return;
    int code = 0;
    const bool ended = waitpid(files_child, &code, WNOHANG) == files_child;
    if (!ended) {
        if (now - files_poll_at >= 200) {
            files_poll_at = now;
            files_progress = fields(o.data / "session/files.ini");
            dirty = true;
        }
        return;
    }
    files_child = -1;
    files_progress = fields(o.data / "session/files.ini");
    const auto state = files_progress["state"], message = files_progress["message"];
    const auto task = files_task;
    const auto command = files_command;
    files_task.clear();
    if (state == "confirm" && settings.page == Page::Files) {
        show_picker(message, {"Cancel", "Go ahead"}, 0, [this, command, task](int yes) {
            if (yes) files_run(command, task, true);
        });
    } else if (state == "done") {
        const auto result = fs::path(files_progress["result"]);
        if (task == "copy" || task == "move") {
            if (task == "move") files_clip.clear();
            status = locale.text(task == "move" ? "Moved." : "Copied.");
        } else if (task == "delete") status = locale.text("Moved to the trash.");
        else if (task == "empty-trash") status = locale.text("The trash is empty.");
        else status.clear();
        emit_sound(task == "delete" || task == "empty-trash" ? MenuCue::Unfavourite : MenuCue::Save);
        files_marked.clear();
        files_load(result.parent_path() == files_dir ? result.filename().string() : "");
        files_peek_request();
        // A game that arrived, left or changed its name: the library looks again.
        if (task != "empty-trash") files_library_changed = true;
    } else {
        status = message.empty() ? locale.text("That did not work.") : message;
        emit_sound(MenuCue::Error);
        files_load();
    }
    files_progress.clear();
    dirty = true;
}

// The upper screen's preview of a zip, a text or a picture, made by files.py off the frame loop.
void App::files_peek_request() {
    const auto* row = files_focused();
    files_peek_wanted.clear();
    if (!row || row->dir || files_remote_root()) return;
    auto ext = fs::path(row->name).extension().string();
    for (auto& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
    static const std::set<std::string> peekable = {".zip", ".png", ".bmp", ".txt", ".md",  ".ini", ".cfg", ".json",
                                                   ".xml", ".cue", ".m3u", ".gdi", ".nfo", ".log", ".csv", ".sh"};
    if (!peekable.count(ext)) return;
    files_peek_wanted = files_dir / row->name;
    if (files_peek_wanted == files_peeked || files_peek_child > 0) return;
    const fs::path app = o.app.empty() ? fs::current_path() : o.app;
    std::vector<std::string> args = {"python3",
                                     "-B",
                                     (app / "runtime/files.py").string(),
                                     "--data",
                                     o.data.string(),
                                     "peek",
                                     files_peek_wanted.string(),
                                     "--out",
                                     (o.data / "session/files-peek").string()};
    const pid_t child = fork();
    if (child == 0) {
        int none = ::open("/dev/null", O_WRONLY);
        if (none >= 0) {
            dup2(none, STDOUT_FILENO);
            dup2(none, STDERR_FILENO);
            ::close(none);
        }
        std::vector<char*> argv;
        for (auto& arg : args) argv.push_back(arg.data());
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    if (child > 0) {
        files_peek_child = child;
        files_peeked = files_peek_wanted;
    }
}

void App::files_peek_read() {
    files_peek_lines.clear();
    std::ifstream text(o.data / "session/files-peek.txt");
    std::string first;
    if (!std::getline(text, first) || first != files_peeked.string()) return;
    for (std::string line; std::getline(text, line) && files_peek_lines.size() < 12;) files_peek_lines.push_back(line);
    files_peek_picture = fs::exists(o.data / "session/files-peek.bmp");
    media.refresh();
    dirty = true;
}

std::string App::files_place() const {
    if (files_roots.empty()) return "";
    const auto& root = files_roots[size_t(files_root)];
    if (!root.source.empty()) return files_remote.empty() ? root.name : root.name + " / " + files_remote;
    std::error_code ec;
    const auto inside = fs::relative(files_dir, root.path, ec);
    const auto name = locale.text(root.name);
    return ec || inside.empty() || inside == "." ? name : name + " / " + inside.generic_string();
}

void App::files_top(SDL_Renderer* r, int screen) {
    settings_card(r, 24, screen + top_card_y, 592, top_card_h);
    const int n = int(files_roots.size());
    if (n > 1) {
        shoulder_tab(r, screen, false, "L", files_roots[size_t((files_root + n - 1) % n)].name);
        shoulder_tab(r, screen, true, "R", files_roots[size_t((files_root + 1) % n)].name);
    }
    const int y = screen + top_card_y;
    raw_label(r, files_place(), 44, y + 20, 18, "muted", 552);
    // Along the bottom, a desktop window's status bar: how many things, how many marked, the room left; a download
    // under way takes its place.
    const bool listing = !files_remote_root() ||
                         (files_list_error.empty() && files_roots[size_t(files_root)].source != "+");
    if (!files_transfer_strip(r, y + top_card_h - 62) && files_setup.step == FilesSetup::None && files_child <= 0 &&
        !files_loading && listing) {
        const int n = int(files_rows.size());
        std::string line = locale.text(std::to_string(n) + (n == 1 ? " item" : " items"));
        if (!files_marked.empty()) line += " · " + locale.text(std::to_string(files_marked.size()) + " marked");
        if (files_free) line += " · " + locale.text(files_size(files_free) + " free");
        rect(r, 44, y + top_card_h - 72, 552, 1, "line");
        raw_label(r, line, 320 - std::min(400, label_width(line, 15, false)) / 2, y + top_card_h - 62, 15, "muted",
                  400);
    }
    if (files_setup.step == FilesSetup::Key) {
        label(r, "Put this key on the server", 44, y + 60, 24, "ink", 552);
        wrapped(r,
                "Add the line below to ~/.ssh/authorized_keys for that user, then press A. It is also on the card in "
                "rgds/private/ssh/id_ed25519.pub.",
                44, y + 100, 16, 552, 3);
        round_rect(r, 40, y + 170, 560, 150, 12, "slot");
        // The key is one long word: broken by hand into lines that fit.
        int line_y = y + 184;
        for (size_t at = 0; at < files_setup.key.size() && line_y < y + 304; at += 52, line_y += 22)
            raw_label(r, files_setup.key.substr(at, 52), 56, line_y, 15, "ink", 528);
        return;
    }
    if (files_net_child > 0 && files_net_task != "ls") {
        const std::string doing = files_net_task == "discover" ? "Looking for servers…"
                                  : files_net_task == "shares" ? "Asking for its shares…"
                                  : files_net_task == "key"    ? "Making this handheld's key…"
                                  : files_net_task == "remove" ? "Removing…"
                                                               : "Checking the connection…";
        busy_ring(r, 320, y + 150, 34, 5);
        centered(r, doing, y + 210, 2, "ink");
        return;
    }
    if (files_remote_root() && (files_loading || !files_list_error.empty() || files_rows.empty())) {
        const auto& root = files_roots[size_t(files_root)];
        if (files_loading) {
            busy_ring(r, 320, y + 150, 34, 5);
            centered(r, "Opening…", y + 210, 2, "ink");
        } else if (root.source == "+") {
            ui_icon(r, SettingsIcon::Wifi, 320, y + 140, 64, "muted");
            centered(r, "No network places yet", y + 200, 2, "ink");
            wrapped(r,
                    "Press Y to add a computer, NAS or Raspberry Pi. Games you copy from it go straight into your "
                    "library.",
                    64, y + 240, 18, 512, 3);
        } else if (!files_list_error.empty()) {
            ui_icon(r, SettingsIcon::Wifi, 320, y + 140, 64, "muted");
            raw_wrapped(r, files_list_error, 64, y + 200, 20, 512, 3);
            wrapped(r, "Y > Refresh tries again.", 64, y + 290, 16, 512, 1);
        } else {
            ui_icon(r, SettingsIcon::Folder, 320, y + 150, 64, "muted");
            centered(r, "This folder is empty", y + 206, 2, "ink");
        }
        return;
    }
    if (files_child > 0) {
        // A copy, move or delete under way: what and how far.
        const double total = std::atof(files_progress["total"].c_str()),
                     done = std::atof(files_progress["done"].c_str());
        const std::string doing = files_task == "move"      ? "Moving…"
                                  : files_task == "copy"    ? "Copying…"
                                  : files_task == "delete"  ? "Moving to the trash…"
                                  : files_task == "extract" ? "Extracting…"
                                                            : "Working…";
        busy_ring(r, 320, y + 150, 34, 5);
        centered(r, doing, y + 210, 2, "ink");
        raw_label(r, files_progress["current"], 64, y + 250, 18, "muted", 512);
        if (total > 0) {
            sunken_track(r, 64, y + 290, 512, 12, 6);
            const int fill = int(512 * std::clamp(done / total, 0., 1.));
            if (fill > 0) raised_pill(r, 64, y + 290, std::max(12, fill), 12, 6, theme.at("accent"), 0, false);
            const bool bytes = files_task != "delete";
            raw_label(r,
                      bytes ? files_size(uintmax_t(done)) + " / " + files_size(uintmax_t(total))
                            : std::to_string(int(done)) + " / " + std::to_string(int(total)),
                      64, y + 314, 16, "muted", 512);
        }
        return;
    }
    const auto* row = files_focused();
    if (!row) {
        ui_icon(r, files_roots[size_t(files_root)].name == "Trash" ? SettingsIcon::Trash : SettingsIcon::Folder, 320,
                y + 150, 64, "muted");
        centered(r, files_roots[size_t(files_root)].name == "Trash" ? "The trash is empty" : "Nothing here yet",
                 y + 206, 2, "ink");
        if (!files_clip.empty())
            wrapped(r, files_clip_move ? "Press Y to move here." : "Press Y to paste here.", 64, y + 250, 18, 512, 2);
        else if (!status.empty() && status != "READY") raw_wrapped(r, status, 64, y + 250, 18, 512, 3);
        return;
    }
    const auto path = files_dir / row->name;
    const int tx = 268, tw = 600 - tx - 8;
    const Game* game = row->dir || files_remote_root() ? nullptr : files_game(path);
    // The picture side: the game's own art, the picture itself, or the kind of thing it is.
    SDL_Rect box{44, y + 56, 200, 200};
    if (game) game_icon(r, *game, box.x, box.y, box.w);
    else if (files_peek_picture && files_peeked == path && media.draw(r, o.data / "session/files-peek.bmp", box)) {
    } else {
        round_rect(r, box.x, box.y, box.w, box.h, 16, "slot");
        ui_icon(r, row->dir ? SettingsIcon::Folder : files_icon(row->name), box.x + box.w / 2, box.y + box.h / 2, 72,
                row->dir ? "accent" : "muted");
    }
    raw_wrapped(r, game ? game->title : row->name, tx, y + 56, 22, tw, 2);
    // Get Info: what it is, how big, when it changed, where it lives. Labels on the left, values beside them.
    std::string size;
    if (row->dir && !files_remote_root()) {
        std::error_code ec;
        int count = 0;
        for (fs::directory_iterator it(path, ec), end; !ec && it != end && count < 10000; it.increment(ec))
            if (it->path().filename().string().rfind('.', 0) != 0) count++;
        size = locale.text(std::to_string(count) + (count == 1 ? " item" : " items"));
    } else if (!row->dir) size = files_size(row->size);
    const std::vector<std::pair<std::string, std::string>> facts = {
        {"Kind", files_kind(*row, game)},
        {"Size", size},
        {"Modified", row->mtime ? achievement_date(row->mtime) : ""},
        {"Where", files_place()}};
    int line = y + 124;
    for (const auto& [name, value] : facts) {
        if (value.empty()) continue;
        label(r, name, tx, line + 2, 15, "muted", 84);
        raw_label(r, value, tx + 90, line, 17, "ink", tw - 90);
        line += 30;
    }
    if (files_marked.count(row->name)) {
        round_rect(r, tx, line + 6, 96, 28, 14, "focus");
        ui_icon(r, SettingsIcon::Check, tx + 18, line + 20, 14, "accent");
        label(r, "Marked", tx + 32, centred_label_y(locale.text("Marked"), 15, line + 20), 15, "accent", 60);
    }
    // Under the picture: a zip's contents or a text's first lines.
    int text_y = y + 270;
    if (files_peeked == path && !files_peek_lines.empty() && !game)
        for (size_t i = 0; i < files_peek_lines.size() && text_y < y + top_card_h - 90; i++, text_y += 20)
            raw_label(r, files_peek_lines[i], 44, text_y, 15, i == 0 ? "ink" : "muted", 552);
    else if (!files_clip.empty())
        raw_wrapped(r,
                    std::to_string(files_clip.size()) + " " +
                        locale.text(files_clip_move ? "to move. Open a folder and press Y."
                                                    : "to paste. Open a folder and press Y."),
                    44, text_y, 16, 552, 2);
    else if (!status.empty() && status != "READY") raw_wrapped(r, status, 44, text_y, 16, 552, 2);
}

// A download under way (or one that stopped): its name, how far, and why it is waiting.
bool App::files_transfer_strip(SDL_Renderer* r, int y) {
    const auto at = [&](const char* key) {
        const auto found = files_transfer.find(key);
        return found == files_transfer.end() ? std::string() : found->second;
    };
    const auto state = at("state");
    const int failed = std::atoi(at("failed").c_str());
    if (state != "running" && state != "waiting" && !failed) return false;
    if (state == "running" || state == "waiting") {
        const double size = std::atof(at("size").c_str()), received = std::atof(at("received").c_str());
        const int queued = std::atoi(at("queued").c_str());
        std::string text =
            locale.text((state == "waiting" ? "Waiting to retry " : "Getting ") + at("name"));
        if (queued > 1) text += " · " + locale.text(std::to_string(queued - 1) + " more");
        raw_label(r, text, 44, y, 15, state == "waiting" ? "muted" : "ink", 360);
        if (size > 0) {
            sunken_track(r, 412, y + 4, 184, 10, 5);
            const int fill = int(184 * std::clamp(received / size, 0., 1.));
            if (fill > 0) raised_pill(r, 412, y + 4, std::max(10, fill), 10, 5, theme.at("accent"), 0, false);
        }
    } else
        raw_label(r, std::to_string(failed) + " " + locale.text(failed == 1 ? "download stopped" : "downloads stopped"),
                  44, y, 15, "muted", 552);
    return true;
}

SettingsIcon App::files_icon(const std::string& name) const {
    auto ext = fs::path(name).extension().string();
    for (auto& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (ext == ".png" || ext == ".bmp" || ext == ".jpg" || ext == ".jpeg") return SettingsIcon::Art;
    if (ext == ".mp3" || ext == ".ogg" || ext == ".flac" || ext == ".wav" || ext == ".opus") return SettingsIcon::Music;
    if (ext == ".zip" || ext == ".7z") return SettingsIcon::Save;
    if (ext == ".txt" || ext == ".md" || ext == ".ini" || ext == ".cfg" || ext == ".json" || ext == ".log")
        return SettingsIcon::Type;
    if (ext == ".sav" || ext == ".srm" || ext == ".dss" || ext == ".state") return SettingsIcon::Heart;
    return SettingsIcon::Cartridge;
}

SettingsIcon App::files_row_icon(int index) const {
    if (index < 0 || index >= int(files_rows.size())) return SettingsIcon::Cartridge;
    const auto& row = files_rows[size_t(index)];
    return row.dir ? SettingsIcon::Folder : files_icon(row.name);
}

// ---- The lower screen, laid out like a desktop file manager ----
// Back and the path bar along the top, the places (the roots L and R step through) in a sidebar, and the folder as a
// list with Name, Size and Modified columns whose titles sort it. The rows are the shared list's (row_top, row_at,
// scrolling and flicks) at files_row_height instead of the settings cards' 66.
namespace files_layout {
constexpr int bar_x = 84, bar_y = 20, bar_h = 44; // the path bar, beside Back
constexpr int side_x = 16, side_w = 136;          // places
constexpr int pane_x = 160, pane_r = 590;         // the listing, up to the scroll rail
constexpr int head_y = 66;                        // column titles over the rows at list_top
constexpr int place_h = 36, place_gap = 10;
constexpr int size_w = 74, date_w = 96; // right-hand columns, right-aligned size and left-aligned date
inline int date_x() { return pane_r - 8 - date_w; }
inline int size_right() { return date_x() - 14; }
inline int name_w() { return size_right() - size_w - (pane_x + 44); }
} // namespace files_layout

SettingsIcon App::files_place_icon(const FilesRoot& root) const {
    if (!root.source.empty()) return SettingsIcon::Wifi;
    if (root.name == "Games") return SettingsIcon::Cartridge;
    if (root.name == "Inbox") return SettingsIcon::Load;
    if (root.name == "Screenshots") return SettingsIcon::Art;
    if (root.name == "Music") return SettingsIcon::Music;
    if (root.name == "Trash") return SettingsIcon::Trash;
    if (root.name == "Card") return SettingsIcon::Save;
    return SettingsIcon::Folder;
}

// A place's top inside the sidebar: the card's folders, then a gap before the network places, another before Trash.
int App::files_place_top(int index) const {
    using namespace files_layout;
    int top = 8;
    for (int i = 0; i < index && i < int(files_roots.size()); i++) {
        top += place_h;
        const bool next_remote = i + 1 < int(files_roots.size()) && !files_roots[size_t(i + 1)].source.empty();
        const bool next_trash = i + 1 == int(files_roots.size()) - 1 && files_roots.back().name == "Trash";
        if ((next_remote && files_roots[size_t(i)].source.empty()) || next_trash) top += place_gap;
    }
    return top;
}

void App::files_sidebar(SDL_Renderer* r, int y) {
    using namespace files_layout;
    const int top = y + head_y, height = list_top + list_height - head_y;
    alpha_round(r, side_x, top, side_w, height, card_radius, theme.at("panel"), 150);
    // Keep the open place in view when there are more than fit.
    const int n = int(files_roots.size()), last = n ? files_place_top(n - 1) + place_h + 8 : 0;
    const int scroll =
        std::clamp(files_place_top(files_root) + place_h + 8 - height, 0, std::max(0, last - height));
    SDL_Rect clip{side_x, top, side_w, height};
    s.RenderSetClipRect(r, &clip);
    for (int i = 0; i < n; i++) {
        const auto& root = files_roots[size_t(i)];
        const int iy = top + files_place_top(i) - scroll;
        const bool open = i == files_root;
        const bool pressed = pointer_down && contact_x >= side_x && contact_x < side_x + side_w &&
                             contact_y >= iy - y && contact_y < iy - y + place_h;
        if (open) round_rect(r, side_x + 6, iy, side_w - 12, place_h - 2, 10, "accent");
        else if (pressed) alpha_round(r, side_x + 6, iy, side_w - 12, place_h - 2, 10, theme.at("accent"), 40);
        ui_icon(r, files_place_icon(root), side_x + 24, iy + place_h / 2 - 1, 18, open ? "on_accent" : "accent");
        const std::string name = root.source.empty() || root.source == "+" ? locale.text(root.name) : root.name;
        raw_label(r, name, side_x + 40, centred_label_y(name, 16, iy + place_h / 2 - 1), 16, open ? "on_accent" : "ink",
                  side_w - 50);
    }
    s.RenderSetClipRect(r, nullptr);
}

// The path as parts to tap, like a desktop window's path bar: the place, then each folder inside it. When it does not
// fit, the parts nearest the place give way to "…".
void App::files_path_bar(SDL_Renderer* r, int y) {
    using namespace files_layout;
    files_crumbs.clear();
    const int w = 616 - bar_x;
    sunken_track(r, bar_x, y + bar_y, w, bar_h, 12);
    if (files_roots.empty()) return;
    const auto& root = files_roots[size_t(files_root)];
    std::vector<std::string> parts = {root.source.empty() || root.source == "+" ? locale.text(root.name) : root.name};
    if (!root.source.empty()) {
        std::stringstream split(files_remote);
        for (std::string part; std::getline(split, part, '/');)
            if (!part.empty()) parts.push_back(part);
    } else {
        std::error_code ec;
        const auto inside = fs::relative(files_dir, root.path, ec);
        if (!ec && inside != ".")
            for (const auto& part : inside)
                if (!part.empty() && part != ".") parts.push_back(part.string());
    }
    constexpr int size = 17, gap = 22, icon = 26;
    const int room = w - 24 - icon;
    // Widest first from the end: the folder you are in always shows.
    std::vector<int> widths;
    for (size_t i = 0; i < parts.size(); i++)
        widths.push_back(std::min(label_width(parts[i], size, false), i + 1 == parts.size() ? room : 150));
    size_t first = parts.size() - 1;
    int used = widths.back();
    const int ellipsis = label_width("…", size, false) + gap;
    while (first > 0 && used + gap + widths[first - 1] + (first - 1 > 0 ? ellipsis : 0) <= room) {
        first--;
        used += gap + widths[first];
    }
    int x = bar_x + 12;
    const int cy = y + bar_y + bar_h / 2;
    ui_icon(r, files_place_icon(root), x + 10, cy, 18, "accent");
    x += icon;
    if (first > 0) {
        raw_label(r, "…", x, centred_label_y("…", size, cy), size, "muted", 30);
        x += ellipsis - gap;
        ui_icon(r, SettingsIcon::Chevron, x + gap / 2, cy, 10, "muted");
        x += gap;
    }
    for (size_t i = first; i < parts.size(); i++) {
        const bool here = i + 1 == parts.size();
        const bool pressed = !here && pointer_down && contact_x >= x - 4 && contact_x < x + widths[i] + 4 &&
                             contact_y >= bar_y && contact_y < bar_y + bar_h;
        if (pressed) alpha_round(r, x - 6, y + bar_y + 6, widths[i] + 12, bar_h - 12, 8, theme.at("accent"), 40);
        raw_label(r, parts[i], x, centred_label_y(parts[i], size, cy), size, here ? "ink" : "accent", widths[i]);
        if (!here) files_crumbs.push_back({SDL_Rect{x - 6, bar_y, widths[i] + 12, bar_h}, int(i)});
        x += widths[i];
        if (!here) {
            ui_icon(r, SettingsIcon::Chevron, x + gap / 2, cy, 10, "muted");
            x += gap;
        }
    }
}

void App::files_row(SDL_Renderer* r, int y, int i) {
    using namespace files_layout;
    const auto& row = files_rows[size_t(i)];
    const int h = files_row_height, top = y + list_top + row_top(i) - list_scroll - list_shift();
    const bool focused = i == settings.focus, marked = files_marked.count(row.name) > 0;
    const int x = pane_x + 6, w = pane_r - pane_x - 12, cy = top + h / 2;
    // Striped like a desktop list; the selection is a solid bar, marked rows a tint of it.
    if (focused) round_rect(r, x, top + 1, w, h - 2, 8, "accent");
    else if (marked) alpha_round(r, x, top + 1, w, h - 2, 8, theme.at("accent"), 46);
    else if (i % 2) alpha_round(r, x, top + 1, w, h - 2, 8, theme.at("slot"), 120);
    const std::string ink = focused ? "on_accent" : "ink", soft = focused ? "on_accent" : "muted";
    if (marked) ui_icon(r, SettingsIcon::Check, x + 18, cy, 16, focused ? "on_accent" : "accent");
    else
        ui_icon(r, row.dir ? SettingsIcon::Folder : files_icon(row.name), x + 18, cy, 20,
                focused ? "on_accent" : row.dir ? "accent" : "muted");
    raw_label(r, row.name, pane_x + 44, centred_label_y(row.name, 17, cy), 17, ink, name_w());
    const std::string size = row.dir ? "--" : files_size(row.size);
    const int size_px = label_width(size, 15, false);
    raw_label(r, size, size_right() - size_px, centred_label_y(size, 15, cy), 15, soft, size_w);
    if (row.mtime) {
        const auto date = achievement_date(row.mtime);
        raw_label(r, date, date_x(), centred_label_y(date, 15, cy), 15, soft, date_w);
    }
}

void App::files_bottom(SDL_Renderer* r, int y) {
    using namespace files_layout;
    sync_list();
    back_button(r, 28, y + 20, pointer_down && contact_x < 80 && contact_y < 78);
    files_path_bar(r, y);
    files_sidebar(r, y);
    // The listing: a card with the column titles at its head.
    const int card_top = y + head_y, card_h = list_top + list_height - head_y;
    round_rect(r, pane_x, card_top + 3, pane_r - pane_x, card_h, card_radius, "slot");
    round_rect(r, pane_x, card_top, pane_r - pane_x, card_h, card_radius, "panel");
    const std::array<std::pair<const char*, int>, 3> columns = {
        {{"Name", pane_x + 44}, {"Size", size_right() - size_w}, {"Modified", date_x()}}};
    for (int c = 0; c < 3; c++) {
        const bool sorted = c == files_sort;
        const int cx = columns[size_t(c)].second, cy = card_top + (list_top - head_y) / 2 + 1;
        // Size is right-aligned under its title's right edge, like its numbers.
        const int width = label_width(columns[size_t(c)].first, 14);
        const int lx = c == 1 ? size_right() - width - (sorted ? 14 : 0) : cx;
        label(r, columns[size_t(c)].first, lx, centred_label_y(locale.text(columns[size_t(c)].first), 14, cy), 14,
              sorted ? "accent" : "muted", 120);
        if (sorted) {
            // A small arrow after the sorted column's title: up for A to Z, oldest or smallest first.
            const Color ink = theme.at("accent");
            s.SetRenderDrawColor(r, ink.r, ink.g, ink.b, 255);
            const int ax = lx + width + 9;
            for (int row = 0; row < 5; row++) {
                const int half = files_sort_down ? 4 - row : row;
                s.RenderDrawLine(r, ax - half, cy - 2 + row, ax + half, cy - 2 + row);
            }
        }
    }
    rect(r, pane_x + 10, y + list_top - 1, pane_r - pane_x - 20, 1, "line");
    SDL_Rect clip{pane_x, y + list_top, pane_r - pane_x, list_height - 4};
    s.RenderSetClipRect(r, &clip);
    for (int i = 0; i < int(files_rows.size()); i++) {
        const int top = row_top(i) - list_scroll - list_shift();
        if (top + files_row_height < 0 || top > list_height) continue;
        files_row(r, y, i);
    }
    s.RenderSetClipRect(r, nullptr);
    if (files_rows.empty()) {
        const int mid = (pane_x + pane_r) / 2, cy = y + list_top + 110;
        if (files_loading) busy_ring(r, mid, cy, 22, 4);
        else {
            const bool trash = !files_roots.empty() && files_roots[size_t(files_root)].name == "Trash";
            const std::string empty = !files_list_error.empty()                                  ? "Could not open it"
                                      : !files_roots.empty() && files_roots[size_t(files_root)].source == "+"
                                          ? "No network places yet"
                                      : trash ? "The trash is empty"
                                              : "This folder is empty";
            ui_icon(r, trash ? SettingsIcon::Trash : SettingsIcon::Folder, mid, cy - 14, 40, "line");
            const int width = label_width(empty, 17);
            label(r, empty, mid - std::min(width, 400) / 2, cy + 22, 17, "muted", 400);
        }
    }
    list_scrollbar(r, y);
}

// Taps: a part of the path opens that folder, a place opens it, a column title sorts by it, and a row is chosen by the
// first tap and opened by a tap on the chosen row (a desktop's click, then double-click).
bool App::files_touch(int x, int y) {
    using namespace files_layout;
    if (files_setup.step == FilesSetup::Key) return false;
    for (const auto& [box, depth] : files_crumbs)
        if (x >= box.x && x < box.x + box.w && y >= box.y && y < box.y + box.h) {
            files_jump(depth);
            emit_sound(MenuCue::Navigate);
            return true;
        }
    if (x >= bar_x && y >= bar_y && y < bar_y + bar_h) return true;
    const int bottom = list_top + list_height;
    if (x >= side_x && x < side_x + side_w && y >= head_y && y < bottom) {
        const int height = bottom - head_y, n = int(files_roots.size());
        const int last = n ? files_place_top(n - 1) + place_h + 8 : 0;
        const int scroll =
            std::clamp(files_place_top(files_root) + place_h + 8 - height, 0, std::max(0, last - height));
        const int local = y - head_y + scroll;
        for (int i = 0; i < n; i++)
            if (local >= files_place_top(i) && local < files_place_top(i) + place_h) {
                if (i != files_root) {
                    files_open_root(i);
                    emit_sound(MenuCue::Navigate);
                }
                break;
            }
        return true;
    }
    if (x >= pane_x && x < pane_r && y >= head_y && y < list_top) {
        files_set_sort(x >= date_x() ? 2 : x >= size_right() - size_w ? 1 : 0);
        return true;
    }
    if (x >= pane_x && x < pane_r && y >= list_top && y < bottom) {
        const int index = row_at(y - list_top + list_scroll);
        if (index < 0) return true;
        // Only a row an earlier tap chose opens, so the row already selected on arrival needs two taps like the rest.
        if (index == settings.focus && index == files_tapped) {
            files_tapped = -1;
            action(2);
        } else {
            if (index != settings.focus) {
                settings.focus = index;
                files_peek_request();
                emit_sound(MenuCue::Navigate);
            }
            files_tapped = index;
        }
        return true;
    }
    return x < pane_x && y >= head_y && y < bottom; // the gap beside the sidebar does nothing
}

// Up one folder (B, Left, Back); B at a place's top leaves the page, Left stays.
void App::files_go_up(bool leave) {
    if (files_remote_root() && !files_remote.empty()) {
        const auto cut = files_remote.rfind('/');
        files_remote_enter(cut == std::string::npos ? "" : files_remote.substr(0, cut),
                           cut == std::string::npos ? files_remote : files_remote.substr(cut + 1));
    } else if (!files_remote_root() && !files_at_root())
        files_enter(files_dir.parent_path(), files_dir.filename().string());
    else if (leave) {
        settings.back();
        if (files_library_changed) rescan_games(true);
        files_library_changed = false;
    }
}

// A part of the path bar: depth 0 is the place itself, 1 the first folder in it, and so on.
void App::files_jump(int depth) {
    if (files_roots.empty()) return;
    const auto& root = files_roots[size_t(files_root)];
    if (!root.source.empty()) {
        std::string path, next;
        std::stringstream split(files_remote);
        int at = 0;
        for (std::string part; std::getline(split, part, '/');) {
            if (part.empty()) continue;
            if (at < depth) path += (path.empty() ? "" : "/") + part;
            else if (at == depth) next = part;
            at++;
        }
        files_remote_enter(path, next);
        return;
    }
    std::error_code ec;
    const auto inside = fs::relative(files_dir, root.path, ec);
    fs::path target = root.path;
    std::string next;
    int at = 0;
    if (!ec)
        for (const auto& part : inside) {
            if (part.empty() || part == ".") continue;
            if (at < depth) target /= part;
            else if (at == depth) next = part.string();
            at++;
        }
    files_enter(target, next);
}

// Folders stay on top; then by name (A to Z), size (biggest first) or date (newest first), a second choice of the same
// column turning it round.
void App::files_sort_rows() {
    auto lowered = [](std::string text) {
        for (auto& c : text) c = char(std::tolower(static_cast<unsigned char>(c)));
        return text;
    };
    std::stable_sort(files_rows.begin(), files_rows.end(), [&](const FileRow& a, const FileRow& b) {
        if (a.dir != b.dir) return a.dir;
        const auto an = lowered(a.name), bn = lowered(b.name);
        if (files_sort == 1 && !a.dir && a.size != b.size) return files_sort_down ? a.size > b.size : a.size < b.size;
        if (files_sort == 2 && a.mtime != b.mtime) return files_sort_down ? a.mtime > b.mtime : a.mtime < b.mtime;
        if (files_sort == 0 && files_sort_down) return an > bn;
        return an < bn;
    });
}

// A column title (or Y > Sort by, column -1, which asks). A second tap on the same title turns the order round; the
// menu only picks the column, so choosing the current one changes nothing.
void App::files_set_sort(int column, bool title) {
    if (column < 0) {
        show_picker("Sort by", {"Name", "Size", "Modified"}, files_sort,
                    [this](int choice) { files_set_sort(choice, false); });
        return;
    }
    if (column == files_sort) {
        if (!title) return;
        files_sort_down = !files_sort_down;
    } else {
        files_sort = column;
        files_sort_down = column != 0; // sizes and dates read best biggest and newest first
    }
    const std::string keep = files_focused() ? files_focused()->name : ""; // the selection stays on its row
    files_sort_rows();
    files_tapped = -1;
    for (size_t i = 0; i < files_rows.size(); i++)
        if (files_rows[i].name == keep) settings.focus = int(i);
    emit_sound(MenuCue::Navigate);
    dirty = true;
}

std::string App::files_kind(const FileRow& row, const Game* game) const {
    if (row.dir) return locale.text("Folder");
    if (game) return system_info(game->system).name;
    auto ext = fs::path(row.name).extension().string();
    for (auto& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (ext == ".png" || ext == ".bmp" || ext == ".jpg" || ext == ".jpeg" || ext == ".gif") return locale.text("Picture");
    if (ext == ".mp3" || ext == ".ogg" || ext == ".flac" || ext == ".wav" || ext == ".opus") return locale.text("Music");
    if (ext == ".zip" || ext == ".7z" || ext == ".rar") return locale.text("Archive");
    if (ext == ".txt" || ext == ".md" || ext == ".nfo" || ext == ".log") return locale.text("Text");
    if (ext == ".sav" || ext == ".srm" || ext == ".dss" || ext == ".state") return locale.text("Save data");
    if (ext.size() < 2) return locale.text("File");
    auto upper = ext.substr(1);
    for (auto& c : upper) c = char(std::toupper(static_cast<unsigned char>(c)));
    return locale.text(upper + " file");
}

void App::files_warm() {
    if (settings.page != Page::Files) return;
    for (auto& panel : surfaces) {
        if (files_peek_picture) media.warm(panel.renderer, o.data / "session/files-peek.bmp");
    }
}
