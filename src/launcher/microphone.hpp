#pragma once
// The microphone test screen.
// Members of App in main.cpp; included once, right after the class.

void App::stop_microphone() {
    if (microphone_worker > 0) {
        kill(-microphone_worker, SIGTERM);
        kill(microphone_worker, SIGTERM);
        for (int i = 0; i < 30; i++) {
            if (waitpid(microphone_worker, nullptr, WNOHANG) != 0) {
                microphone_worker = -1;
                break;
            }
            usleep(10000);
        }
        if (microphone_worker > 0) {
            kill(microphone_worker, SIGKILL);
            waitpid(microphone_worker, nullptr, 0);
            microphone_worker = -1;
        }
    }
    if (microphone_pipe >= 0) {
        ::close(microphone_pipe);
        microphone_pipe = -1;
    }
    microphone_bytes.clear();
}

void App::open_microphone() {
    if (o.game_menu) {
        show_detail("Test microphone", locale.text("Return home before testing the physical microphone."));
        return;
    }
    stop_microphone();
    microphone_level = microphone_peak = 0;
    microphone_db = -60;
    microphone_state = o.preview ? "Test the microphone on your handheld." : "Listening…";
    settings.page = Page::Microphone;
    settings.focus = 0;
    configure_sounds();
    dirty = true;
    if (o.preview) return;
    int channel[2];
    if (pipe(channel) != 0) {
        microphone_state = "Microphone unavailable";
        return;
    }
    auto helper = (o.app / "runtime/microphone_test.py").string(), data = o.data.string();
    auto child = fork();
    if (child == 0) {
        setpgid(0, 0);
        ::close(channel[0]);
        dup2(channel[1], STDOUT_FILENO);
        ::close(channel[1]);
        execlp("python3", "python3", "-B", helper.c_str(), "--data", data.c_str(), nullptr);
        _exit(127);
    }
    ::close(channel[1]);
    if (child < 0) {
        ::close(channel[0]);
        microphone_state = "Microphone unavailable";
        return;
    }
    setpgid(child, child);
    microphone_worker = child;
    microphone_pipe = channel[0];
    fcntl(microphone_pipe, F_SETFL, fcntl(microphone_pipe, F_GETFL) | O_NONBLOCK);
    fcntl(microphone_pipe, F_SETFD, FD_CLOEXEC);
    microphone_update = s.GetTicks();
}

void App::poll_microphone(Uint32 now) {
    if (settings.page != Page::Microphone) {
        if (microphone_worker > 0 || microphone_pipe >= 0) stop_microphone();
        return;
    }
    if (microphone_pipe < 0) return;
    char buffer[512];
    ssize_t size;
    while ((size = read(microphone_pipe, buffer, sizeof(buffer))) > 0) microphone_bytes.append(buffer, size);
    size_t newline;
    while ((newline = microphone_bytes.find('\n')) != std::string::npos) {
        auto line = microphone_bytes.substr(0, newline);
        microphone_bytes.erase(0, newline + 1);
        std::istringstream values(line);
        int level, peak, db;
        if (values >> level >> peak >> db) {
            microphone_level = std::clamp(level, 0, 100);
            microphone_peak = std::clamp(peak, 0, 100);
            microphone_db = std::clamp(db, -60, 0);
            microphone_state = "Listening…";
            microphone_update = now;
        } else {
            microphone_state =
                line == "busy" ? "Return home before testing the physical microphone." : "Microphone unavailable";
            microphone_level = microphone_peak = 0;
        }
        dirty = true;
    }
    if (microphone_bytes.size() > 4096) microphone_bytes.clear();
    if (microphone_worker > 0 && waitpid(microphone_worker, nullptr, WNOHANG) != 0) {
        microphone_worker = -1;
        ::close(microphone_pipe);
        microphone_pipe = -1;
        if (microphone_state == "Listening…") microphone_state = "Microphone unavailable";
        microphone_level = microphone_peak = 0;
        dirty = true;
    }
    if (now - microphone_update > 3000 && microphone_state == "Listening…") {
        microphone_state = "Microphone unavailable";
        microphone_level = microphone_peak = 0;
        dirty = true;
    }
}

void App::microphone_screen(SDL_Renderer* r, int y) {
    settings_header(r, y, "Test microphone");
    round_rect(r, 28, y + 105, 584, 250, 12, "panel");
    ui_icon(r, SettingsIcon::Volume, 70, y + 145, 30, "accent");
    label(r, "Live input", 100, y + 133, 24, "ink", 280);
    raw_label(r, std::to_string(microphone_db) + " dBFS", 424, y + 136, 22, "accent", 160);
    for (int i = 0; i < 24; i++)
        round_rect(r, 52 + i * 22, y + 207, 17, 52, 4, i * 100 / 24 < microphone_level ? "accent" : "slot");
    int peak = 52 + std::clamp(microphone_peak * 523 / 100, 0, 523);
    rect(r, peak, y + 198, 3, 70, "ink");
    label(r, "Quiet", 52, y + 275, 16, "muted", 200);
    label(r, "Loud", 526, y + 275, 16, "muted", 70);
    wrapped(r, microphone_state, 52, y + 312, 18, 536, 2);
    label(r, "Audio is not recorded or saved.", 40, y + 381, 18, "muted", 560);
}
