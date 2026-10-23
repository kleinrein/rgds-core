// SPDX-License-Identifier: GPL-3.0-or-later
// One game being played: the live core, its battery save, and the companion
// screen. No windows or sound devices here, so the headless tool and the
// tests drive exactly what the handheld runs.
#pragma once
#include "app/storage.hpp"
#include "companion/companion.hpp"
#include "companion/profile.hpp"
#include "core/core.hpp"
#include "ui/canvas.hpp"

#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace play {

struct SessionOptions {
    std::string rom;
    std::string data_root;                 // userdata/play
    std::vector<std::string> profile_dirs; // shipped first, the player's own last
    std::vector<std::string> save_sources; // folders to import an existing save from
    std::string font_dir;                  // assets/fonts
    bool companion = true;
};

std::string system_display_name(const std::string& system);

class Session {
public:
    ~Session();
    bool open(const SessionOptions& options, std::string& error);
    void frame(uint32_t buttons);
    // The game picture for the top panel (with the HUD split, the boxes the
    // profile puts back on it; a Game Boy HUD band off it unless `whole`).
    Picture game(bool whole = false) {
        return companion_ ? companion_->compose_game(core_->picture(), whole) : core_->picture();
    }
    void render_companion(Canvas& out, const Look& look) const;
    size_t audio(int16_t* stereo, size_t max_frames) { return core_->audio(stereo, max_frames); }
    unsigned audio_rate() const { return core_->audio_rate(); }
    double frame_rate() const { return core_->frame_rate(); }
    // Writes any unsaved battery data and waits for it.
    void flush();
    // A save state (the whole machine) to a file, written whole and atomically,
    // and back. The shared in-game menu's checkpoints go through these.
    bool save_state_file(const std::string& path, std::string& error);
    bool load_state_file(const std::string& path, std::string& error);

    Core& core() { return *core_; }
    const std::string& title() const { return title_; }
    const SaveSlot& save() const { return save_; }
    const Font& font() const { return font_; }
    const Profile* profile() const { return profile_; }
    Companion* companion() { return companion_.get(); }
    const std::vector<std::string>& profile_errors() const { return profile_errors_; }
    uint64_t frames() const { return frames_; }

private:
    std::string view_path() const;
    void save_view();

    std::thread view_writer_;
    std::unique_ptr<Core> core_;
    ProfileLibrary profiles_;
    std::vector<std::string> profile_errors_;
    const Profile* profile_ = nullptr;
    std::unique_ptr<Companion> companion_;
    std::unique_ptr<SaveWriter> writer_;
    SaveSlot save_;
    Font font_;
    std::string title_;
    uint64_t frames_ = 0, saved_revision_ = 0;
};

} // namespace play
