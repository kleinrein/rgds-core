// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/session.hpp"

#include <cstdio>

namespace play {
namespace {

// "Metroid Fusion (USA)" -> "Metroid Fusion", the way the launcher shows titles.
std::string clean_title(std::string name) {
    for (size_t at; (at = name.find('(')) != std::string::npos;) {
        auto end = name.find(')', at);
        if (end == std::string::npos) break;
        name.erase(at, end - at + 1);
    }
    for (size_t at; (at = name.find('[')) != std::string::npos;) {
        auto end = name.find(']', at);
        if (end == std::string::npos) break;
        name.erase(at, end - at + 1);
    }
    while (!name.empty() && name.back() == ' ') name.pop_back();
    while (!name.empty() && name.front() == ' ') name.erase(0, 1);
    if (name.size() > 5 && name.compare(name.size() - 5, 5, ", The") == 0)
        name = "The " + name.substr(0, name.size() - 5);
    return name;
}

} // namespace

std::string system_display_name(const std::string& system) {
    if (system == "gba") return "GAME BOY ADVANCE";
    if (system == "gbc") return "GAME BOY COLOR";
    if (system == "gb") return "GAME BOY";
    return system;
}

Session::~Session() { flush(); }

bool Session::open(const SessionOptions& o, std::string& error) {
    std::string inner;
    RomBytes rom = read_rom(o.rom, error, &inner);
    if (!rom) return false;
    // The save is chosen before the core starts, so the game sees it at boot.
    // Its folder needs the system, which the core knows only once loaded:
    // open once to learn it (cheap), then for real with the save attached.
    {
        auto probe = open_mgba(rom, inner, {});
        if (!probe.core) {
            error = probe.error;
            return false;
        }
        if (!prepare_save(o.data_root, probe.core->system(), o.rom, o.save_sources, save_, error)) return false;
    }
    auto opened = open_mgba(rom, inner, save_.bytes);
    if (!opened.core) {
        error = opened.error;
        return false;
    }
    core_ = std::move(opened.core);
    writer_ = std::make_unique<SaveWriter>(save_.path);
    if (!save_.imported_from.empty())
        std::fprintf(stderr, "play: copied the save from %s (left in place)\n", save_.imported_from.c_str());

    for (const auto& dir : o.profile_dirs) profiles_.load_directory(dir, profile_errors_);
    for (const auto& e : profile_errors_) std::fprintf(stderr, "play: profile skipped: %s\n", e.c_str());
    profile_ = profiles_.find(core_->system(), core_->game_code());
    title_ = profile_ ? profile_->title : clean_title(file_stem(o.rom));
    if (!o.font_dir.empty()) font_.load(o.font_dir);
    if (o.companion)
        companion_ = std::make_unique<Companion>(*core_, profile_, title_, system_display_name(core_->system()));
    return true;
}

void Session::frame(uint32_t buttons) {
    core_->run_frame(buttons);
    ++frames_;
    if (companion_) companion_->after_frame(*core_);
    uint64_t revision = core_->battery_revision();
    if (revision != saved_revision_) {
        saved_revision_ = revision;
        auto bytes = core_->battery();
        if (!bytes.empty()) writer_->submit(std::move(bytes));
    }
}

void Session::render_companion(Canvas& out, const Look& look) const {
    if (companion_) companion_->render(out, look, font_);
    else out.fill(look.background);
}

bool Session::save_state_file(const std::string& path, std::string& error) {
    std::vector<uint8_t> state;
    if (!core_->save_state(state)) {
        error = "the emulator could not save its state";
        return false;
    }
    return write_file_atomic(path, state, error);
}

bool Session::load_state_file(const std::string& path, std::string& error) {
    std::vector<uint8_t> state;
    if (!read_file(path, state) || state.empty()) {
        error = "no state at " + path;
        return false;
    }
    if (!core_->load_state(state)) {
        error = "the state in " + path + " is not for this game or this version";
        return false;
    }
    return true;
}

void Session::flush() {
    if (!core_ || !writer_) return;
    uint64_t revision = core_->battery_revision();
    if (revision != saved_revision_) {
        saved_revision_ = revision;
        auto bytes = core_->battery();
        if (!bytes.empty()) writer_->submit(std::move(bytes));
    }
    writer_->finish();
}

} // namespace play
