#pragma once
// RG DS Media (play/src/media, docs/MEDIA_PLAN.md): Home's Media tile. It is started the way a game is: the
// launcher's exit fade, then runtime/media.py takes both panels over the display-handoff socket, and the launcher
// comes back when it leaves. Its Music tab leaves with 42, and finish_launch opens the music player.
// Members of App in main.cpp; included once, right after the class.

bool App::media_available() const {
    if (o.game_menu || probe) return false;
    if (media_found < 0) {
        std::error_code ec;
        const char* override_path = std::getenv("RGDS_MEDIA_BINARY"); // a host build, for previews and tests
        media_found = (override_path && *override_path && fs::exists(override_path, ec)) ||
                              (!o.app.empty() && fs::exists(o.app / "emulators/rgds-media/rgds-media", ec)) ||
                              fs::exists("/usr/bin/rgds-media", ec)
                          ? 1
                          : 0;
    }
    return media_found == 1;
}

void App::launch_media() {
    if (launching || probe) return;
    if (worker > 0 && (worker_action == "device.reboot" || worker_action == "device.poweroff" ||
                       worker_action == "device.update-apply")) {
        status = "Finishing the system request";
        dirty = true;
        return;
    }
    if (return_writes_pending()) {
        status = "Restoring settings";
        dirty = true;
        return;
    }
    launch_preview = o.preview || !o.allow_launch;
    if (!launch_preview && (o.app.empty() || o.data.empty())) {
        status = "ERROR: Launch needs app and data paths";
        dirty = true;
        return;
    }
    return_audio_failed = false;
    media_launch = true;
    launching_core = unsigned(play_core); // only named by a preview's status line
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
    dirty = true;
    if (!launch_preview && worker > 0) {
        kill(worker, SIGTERM);
        worker_action.clear();
    }
}

void App::start_media_runner(Uint32 now) {
    int channel[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, channel) < 0 || fcntl(channel[0], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(channel[1], F_SETFD, FD_CLOEXEC) < 0) {
        launch_result = 127;
        begin_launch_exit(false, now);
        return;
    }
#ifdef SO_NOSIGPIPE
    const int suppress = 1;
    setsockopt(channel[0], SOL_SOCKET, SO_NOSIGPIPE, &suppress, sizeof(suppress));
#endif
    std::vector<std::string> args = {"python3",
                                     (o.app / "runtime/media.py").string(),
                                     "launch",
                                     "--app",
                                     o.app.string(),
                                     "--data",
                                     o.data.string(),
                                     "--display-handoff-fd",
                                     std::to_string(channel[1])};
    // The Music tab only where the music player is on.
    const bool music = music_available();
    save_state();
    launch_child = fork();
    if (launch_child == 0) {
        if (music) setenv("RGDS_MEDIA_MUSIC_TAB", "1", 1);
        // Where each film and book was left is the profile's; the look is the one the launcher draws.
        setenv("RGDS_MEDIA_STATE", (o.player / "media").c_str(), 1);
        setenv("RGDS_MEDIA_PREFERENCES", o.preferences.c_str(), 1);
        ::close(channel[0]);
        if (fcntl(channel[1], F_SETFD, 0) < 0) _exit(127);
        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(a.data());
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    ::close(channel[1]);
    launch_channel = channel[0];
    launch_phase = LaunchPhase::Preparing;
    if (launch_child < 0) {
        launch_result = 127;
        begin_launch_exit(false, now);
    }
}

// The upper screen while the Media tile is picked on Home.
void App::media_home_top(SDL_Renderer* r, int y) {
    category_icon(r, music_tile(), 272, y + 120, 96);
    centered(r, "Media", y + 236, 2, "ink");
    const std::string note = locale.text(music_available() ? "Films, books and music" : "Films and books");
    raw_label(r, note, 320 - std::min(500, label_width(note, 18, false)) / 2, y + 282, 18, "muted", 500);
    key_hints(r, 320, y + 432, {{"A", "Open"}}, 0);
}
