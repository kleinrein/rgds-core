// SPDX-License-Identifier: GPL-3.0-or-later
// One game being played: the live core, its battery save, and the companion
// screen. No windows or sound devices here, so the headless tool and the
// tests drive exactly what the handheld runs.
#pragma once
#include "app/achievements.hpp"
#include "app/cheats.hpp"
#include "app/storage.hpp"
#include "app/timeline.hpp"
#include "companion/companion.hpp"
#include "companion/expr.hpp"
#include "companion/profile.hpp"
#include "companion/rewind.hpp"
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
    bool import_saves = true;              // off (--no-save-import): another player's save may lie beside the game
    std::string shared_cheats;             // --shared-cheats: the owner's cheats folder, for another profile to read
    std::string font_dir;                  // assets/fonts
    std::string preferences;               // userdata/preferences.ini (and ui.ini beside it): the launcher's look
    std::string core_library;              // --core: a libretro core's .so (else mGBA for GBA, GB and GBC)
    std::string system;                    // --system: the system id of a libretro game (nes, snes, md, ...)
    std::string system_dir;                // --system-dir: BIOS files for libretro cores
    std::string art_dir;                   // --art: the game's library folder (box.bmp, its box art)
    bool companion = true;
    bool rewind = true; // the timeline of moments behind the Rewind screen (RGDS_PLAY_REWIND=0: off)
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
    // Changes whenever render_companion() would draw something different.
    uint64_t visual_key() const;
    // A finger on the second screen: the Rewind screen's while it is open, else the companion's.
    bool touch(const Touch& t);

    // Rewind (docs/TIME_TRAVEL_PLAN.md): the game holds while the screen is open, and the top screen shows the
    // moment chosen. False with `why` (for the dock's note) when this game can't be rewound.
    bool open_rewind(std::string& why);
    bool rewinding() const { return rewind_.is_open(); }
    // Every loop while rewinding: the pad and the clock; loads the moment chosen, and plays from it or goes back
    // to now when asked. True when the second screen must be drawn again.
    bool rewind_input(uint32_t buttons, uint64_t ms, bool animations);
    Timeline& timeline() { return timeline_; }
    // The game menu's Rewind screen (the launcher draws it, the bridge calls these through rgds_rewind_*): a moment
    // now (0: none), a moment loaded for a look with its picture (one silent frame run; px null for a GPU game), and
    // playing from a moment or going back to now. While a look is in effect, saving puts back now first.
    uint32_t menu_rewind_open();
    bool menu_rewind_show(uint32_t id, Picture& shown);
    bool menu_rewind_jump(uint32_t id, bool play);
    RewindScreen& rewind_screen() { return rewind_; }
    size_t audio(int16_t* stereo, size_t max_frames) { return core_->audio(stereo, max_frames); }
    unsigned audio_rate() const { return core_->audio_rate(); }
    double frame_rate() const { return core_->frame_rate(); }
    // Writes any unsaved battery data and waits for it.
    void flush();
    // A save state (the whole machine) to a file, written whole and atomically,
    // and back. The shared in-game menu's checkpoints go through these.
    bool save_state_file(const std::string& path, std::string& error);
    bool load_state_file(const std::string& path, std::string& error);

    // The game's cheats (a .cht file for it, if there is one), already applied to the core.
    const CheatBook& cheats() const { return cheats_; }

    // RetroAchievements for this game (app/achievements.hpp), once the launcher's sign-in is given: false with
    // `error` when the system has none or it cannot start. Evaluated after every frame from then on.
    bool start_achievements(const AchievementOptions& options, std::string& error);
    Achievements& achievements() { return achievements_; }
    bool set_cheat(size_t index, bool on);

    Core& core() { return *core_; }
    const std::string& title() const { return title_; }
    const SaveSlot& save() const { return save_; }
    const Font& font() const { return font_; }
    Font& font() { return font_; }
    // Theme & display as the launcher has it (the default look without preferences).
    const Look& look() const { return look_; }
    Look& look() { return look_; }
    const Words& words() const { return words_; }
    const Profile* profile() const { return profile_; }
    Companion* companion() { return companion_.get(); }
    const std::vector<std::string>& profile_errors() const { return profile_errors_; }
    uint64_t frames() const { return frames_; }

private:
    std::string view_path() const;
    void save_view();

    std::thread view_writer_;
    std::unique_ptr<Core> core_;
    RomBytes rom_;         // the cartridge as read (mGBA's systems), for RetroAchievements to identify it by
    std::string rom_path_; // --rom
    Achievements achievements_;
    ProfileLibrary profiles_;
    std::vector<std::string> profile_errors_;
    const Profile* profile_ = nullptr;
    Profile tall_profile_; // profile_ for a vertical arcade game with none of its own
    std::unique_ptr<Companion> companion_;
    std::unique_ptr<SaveWriter> writer_;
    SaveSlot save_;
    CheatBook cheats_;
    Font font_;
    Look look_;
    Words words_;
    std::string title_;
    std::string data_root_;
    uint64_t frames_ = 0, saved_revision_ = 0;
    Expr cursor_x_, cursor_y_; // where a mouse game draws its cursor (the profile's cursor_x, cursor_y)
    // Rewind: the moments, the screen, the moment the core holds now (shown on the top screen), and the buttons
    // that closed the screen, kept from the game until let go.
    void show_moment(uint32_t id);
    void menu_rewind_restore(); // a look in effect: back to the moment the menu opened at
    uint32_t menu_now_ = 0;
    bool menu_previewed_ = false;
    // Before playing on from an earlier moment: the battery save as it is, kept beside it (the first of the session
    // as .before-rewind, never replaced; the latest as .before-rewind.last). False when it could not be kept.
    bool keep_before_rewind(bool fresh);
    bool rewind_backed_up_ = false;
    std::vector<uint32_t> menu_shown_;
    void close_rewind(uint32_t id);
    Timeline timeline_;
    bool rewind_on_ = true;
    RewindScreen rewind_;
    uint32_t shown_moment_ = 0, swallow_ = 0, pad_now_ = 0;
};

} // namespace play
