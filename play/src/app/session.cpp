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

// A Game Boy cartridge's header title as a profile code: "POKEMON BLUE" ->
// POKEMON_BLUE. Older carts carry no game code (or a garbled one, where the
// title runs into it), so their profiles name the title instead.
std::string header_title_code(const std::vector<uint8_t>& rom) {
    if (rom.size() < 0x150) return {};
    std::string out;
    for (size_t i = 0x134; i < 0x144; ++i) {
        uint8_t c = rom[i];
        if (c == 0 || (i == 0x143 && (c & 0x80))) break;
        bool word = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (c >= 'a' && c <= 'z') c = uint8_t(c - 32), word = true;
        if (word) out += char(c);
        else if (!out.empty() && out.back() != '_') out += '_';
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out;
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
    RomBytes rom;
    if (o.core_library.empty()) {
        rom = read_rom(o.rom, error, &inner);
        if (!rom) return false;
        // The save is chosen before the core starts, so the game sees it at boot.
        // Its folder needs the system, which the core knows only once loaded:
        // open once to learn it (cheap), then for real with the save attached.
        auto probe = open_mgba(rom, inner, {});
        if (!probe.core) {
            error = probe.error;
            return false;
        }
        if (!prepare_save(o.data_root, probe.core->system(), o.rom, o.save_sources, save_, error)) return false;
    } else {
        // A libretro core: the launcher names the system, and the core reads the file itself.
        if (o.system.empty()) {
            error = "--core needs --system";
            return false;
        }
        rom = std::make_shared<const std::vector<uint8_t>>();
        if (!prepare_save(o.data_root, o.system, o.rom, o.save_sources, save_, error)) return false;
    }
    auto opened = o.core_library.empty()
                      ? open_mgba(rom, inner, save_.bytes)
                      : open_libretro(o.core_library, o.rom, o.system, o.system_dir, o.data_root, save_.bytes);
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
    if (!profile_ && std::string(core_->system()) != "gba" && !rom->empty())
        profile_ = profiles_.find(core_->system(), header_title_code(*rom));
    // A libretro game by its cartridge's own name (SNES) or its file's.
    if (!profile_ && rom->empty()) profile_ = profiles_.find(core_->system(), core_->header_title());
    if (profile_ && profile_->lightgun && !core_->set_lightgun(true))
        std::fprintf(stderr, "play: this core has no light gun for %s\n", profile_->title.c_str());
    title_ = profile_ ? profile_->title : clean_title(file_stem(o.rom));
    // The launcher's appearance and language, so the second screen is part of RG DS.
    if (!o.preferences.empty()) look_ = load_look(o.preferences);
    else look_ = make_look(false, -1, false); // the launcher's defaults: light, its own blue
    if (!o.font_dir.empty()) {
        font_.load(o.font_dir, look_.language);
        words_.load(o.font_dir + "/../locales", look_.language);
    }
    font_.set_pixel(look_.pixel_text);
    if (o.companion) {
        // A colour game that also plays on the first Game Boy (header flag 80)
        // runs in colour, but mGBA names it a Game Boy game (its saves stay
        // under "gb"); the card says what it is.
        std::string system = core_->system();
        if (system == "gb" && rom->size() > 0x143 && ((*rom)[0x143] & 0x80)) system = "gbc";
        companion_ = std::make_unique<Companion>(*core_, profile_, title_, system_display_name(system));
        // What a view has learnt (the explored map) lives beside the save.
        std::vector<uint8_t> bytes;
        if (companion_->view() && read_file(view_path(), bytes))
            companion_->view()->load(std::string(bytes.begin(), bytes.end()));
    }
    return true;
}

std::string Session::view_path() const {
    std::string path = save_.path;
    auto dot = path.rfind('.');
    if (dot != std::string::npos && path.find('/', dot) == std::string::npos) path.erase(dot);
    return path + ".view";
}

void Session::save_view() {
    View* view = companion_ ? companion_->view() : nullptr;
    std::string bytes;
    if (!view || !view->dirty() || !view->save(bytes)) return;
    // On its own thread, like the battery save: an SD card fsync can stall a frame.
    if (view_writer_.joinable()) view_writer_.join();
    view_writer_ = std::thread([path = view_path(), bytes = std::move(bytes)] {
        std::string error;
        if (!write_file_atomic(path, std::vector<uint8_t>(bytes.begin(), bytes.end()), error))
            std::fprintf(stderr, "play: the companion's map was not kept: %s\n", error.c_str());
    });
}

void Session::frame(uint32_t buttons) {
    core_->run_frame(buttons);
    ++frames_;
    if (companion_) companion_->after_frame(*core_);
    if (frames_ % 1800 == 0) save_view(); // every half minute, when it has changed
    uint64_t revision = core_->battery_revision();
    if (revision != saved_revision_) {
        saved_revision_ = revision;
        auto bytes = core_->battery();
        if (!bytes.empty()) writer_->submit(std::move(bytes));
    }
}

void Session::render_companion(Canvas& out, const Look& look) const {
    if (companion_) companion_->render(out, look, font_, words_);
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
    save_view();
    if (view_writer_.joinable()) view_writer_.join();
    uint64_t revision = core_->battery_revision();
    if (revision != saved_revision_) {
        saved_revision_ = revision;
        auto bytes = core_->battery();
        if (!bytes.empty()) writer_->submit(std::move(bytes));
    }
    writer_->finish();
}

} // namespace play
