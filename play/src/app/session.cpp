// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/session.hpp"
#include "reader/manual.hpp"

#include "launcher/system_icons.hpp"
#include "ui/png.hpp"

#include <zlib.h>

#include <filesystem>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace play {
namespace {

bool unpack_view(std::vector<uint8_t>& bytes); // below, with pack_view

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
    // The launcher's own names for the rest (its Systems page), in the same capitals.
    for (const auto& known : rgds::stock_systems())
        if (system == known.id) {
            std::string name = known.name;
            for (char& c : name) c = char(std::toupper(static_cast<unsigned char>(c)));
            return name;
        }
    return system;
}

Session::~Session() { flush(); }

bool Session::set_cheat(size_t index, bool on) {
    if (!cheats_.set(index, on)) return false;
    return core_->apply_cheats(cheats_.codes());
}

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
        if (!prepare_save(o.data_root, probe.core->system(), o.rom, o.save_sources, save_, error, o.import_saves))
            return false;
    } else {
        // A libretro core: the launcher names the system, and the core reads the file itself.
        if (o.system.empty()) {
            error = "--core needs --system";
            return false;
        }
        rom = std::make_shared<const std::vector<uint8_t>>();
        if (!prepare_save(o.data_root, o.system, o.rom, o.save_sources, save_, error, o.import_saves)) return false;
    }
    auto opened = o.core_library.empty()
                      ? open_mgba(rom, inner, save_.bytes)
                      : open_libretro(o.core_library, o.rom, o.system, o.system_dir, o.data_root, save_.bytes);
    if (!opened.core) {
        error = opened.error;
        return false;
    }
    core_ = std::move(opened.core);
    rom_ = rom;
    rom_path_ = o.rom;
    writer_ = std::make_unique<SaveWriter>(save_.path);
    data_root_ = o.data_root;
    if (cheats_.load(o.data_root, core_->system(), o.rom, o.shared_cheats)) {
        core_->apply_cheats(cheats_.codes());
        std::fprintf(stderr, "play: %zu cheats from %s\n", cheats_.codes().size(), cheats_.path().c_str());
    }
    if (!save_.imported_from.empty())
        std::fprintf(stderr, "play: copied the save from %s (left in place)\n", save_.imported_from.c_str());

    for (const auto& dir : o.profile_dirs) profiles_.load_directory(dir, profile_errors_);
    for (const auto& e : profile_errors_) std::fprintf(stderr, "play: profile skipped: %s\n", e.c_str());
    profile_ = profiles_.find(core_->system(), core_->game_code());
    if (!profile_ && std::string(core_->system()) != "gba" && !rom->empty())
        profile_ = profiles_.find(core_->system(), header_title_code(*rom));
    // A libretro game by its cartridge's own name (SNES) or its file's.
    if (!profile_ && rom->empty()) profile_ = profiles_.find(core_->system(), core_->header_title());
    if (!profile_)
        for (const auto& code : core_->other_codes())
            if ((profile_ = profiles_.find(core_->system(), code))) break;
    if (profile_ && profile_->lightgun && !core_->set_lightgun(true))
        std::fprintf(stderr, "play: this core has no light gun for %s\n", profile_->title.c_str());
    if (profile_ && profile_->mouse) {
        if (!core_->set_mouse(true))
            std::fprintf(stderr, "play: this core has no mouse for %s\n", profile_->title.c_str());
        std::string why;
        if (!profile_->cursor_x.empty() &&
            (!cursor_x_.parse(profile_->cursor_x, why) || !cursor_y_.parse(profile_->cursor_y, why)))
            std::fprintf(stderr, "play: the cursor is not read: %s\n", why.c_str()), cursor_x_ = cursor_y_ = Expr{};
    }
    title_ = profile_ ? profile_->title : clean_title(file_stem(o.rom));
    // A ScummVM game is its folder, whatever file names it; a profile there covers a whole family of games.
    if (std::string(core_->system()) == "scummvm")
        title_ = clean_title(std::filesystem::path(o.rom).parent_path().filename().string());
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
        // The system's picture beside the fonts in the release (assets/systems), the box art in the library.
        Dock::Image picture, cover;
        std::string unread;
        if (!o.font_dir.empty())
            read_bmp(o.font_dir + "/../systems/" + system + ".bmp", picture.px, picture.w, picture.h, unread);
        if (!o.art_dir.empty()) read_bmp(o.art_dir + "/box.bmp", cover.px, cover.w, cover.h, unread);
        companion_->dock().set_art(system, std::move(picture), std::move(cover));
        if (!o.art_dir.empty()) companion_->remember_switch_in(o.art_dir + "/second-screen.txt");
        // A scanned manual beside the ROM or in the library folder: one more screen behind the switch.
        if (const auto manuals = find_manuals(o.rom, o.art_dir); !manuals.empty())
            companion_->set_manual(
                make_manual(manuals.front(), o.art_dir.empty() ? "" : o.art_dir + "/manual-place.txt"));
        // What a view has learnt (the explored map) lives beside the save.
        std::vector<uint8_t> bytes;
        if (companion_->view() && read_file(view_path(), bytes) && unpack_view(bytes))
            companion_->view()->load(std::string(bytes.begin(), bytes.end()));
    }
    rewind_on_ = o.rewind;
    timeline_.configure(core_->frame_rate(), Timeline::Options{});
    return true;
}

// A view's file: as the view wrote it, or "RGVZ", its size (32 bits, little-endian) and it deflated. A map made
// from the game's picture is megabytes of mostly sky.
namespace {
std::vector<uint8_t> pack_view(const std::string& bytes) {
    uLongf size = compressBound(uLong(bytes.size()));
    std::vector<uint8_t> out(8 + size);
    if (bytes.size() < 4096 || bytes.size() > 0x7FFFFFFF ||
        compress2(out.data() + 8, &size, reinterpret_cast<const Bytef*>(bytes.data()), uLong(bytes.size()), 1) != Z_OK)
        return std::vector<uint8_t>(bytes.begin(), bytes.end());
    std::memcpy(out.data(), "RGVZ", 4);
    for (int i = 0; i < 4; ++i) out[size_t(4 + i)] = uint8_t(bytes.size() >> (8 * i));
    out.resize(8 + size);
    return out;
}

bool unpack_view(std::vector<uint8_t>& bytes) {
    if (bytes.size() < 8 || std::memcmp(bytes.data(), "RGVZ", 4) != 0) return true;
    uLongf size = uLongf(bytes[4]) | (uLongf(bytes[5]) << 8) | (uLongf(bytes[6]) << 16) | (uLongf(bytes[7]) << 24);
    if (size > 0x7FFFFFFF) return false;
    std::vector<uint8_t> out(size);
    if (uncompress(out.data(), &size, bytes.data() + 8, uLong(bytes.size() - 8)) != Z_OK || size != out.size())
        return false;
    bytes.swap(out);
    return true;
}
} // namespace

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
        if (!write_file_atomic(path, pack_view(bytes), error))
            std::fprintf(stderr, "play: the companion's map was not kept: %s\n", error.c_str());
    });
}

void Session::frame(uint32_t buttons) {
    // The game menu closed while it showed an old moment (not through Back or Play from here): the game goes on
    // from now, never from a moment only looked at.
    if (menu_now_) {
        menu_rewind_restore();
        timeline_.protect(0);
        menu_now_ = 0;
    }
    if (!cursor_x_.empty()) core_->set_mouse_cursor(int(cursor_x_.eval(*core_)), int(cursor_y_.eval(*core_)));
    // What closed the Rewind screen (A, B) is not the game's until let go.
    swallow_ &= buttons;
    buttons &= ~swallow_;
    core_->run_frame(companion_ ? companion_->machine_buttons(buttons) : buttons);
    ++frames_;
    achievements_.frame();
    if (companion_) companion_->after_frame(*core_);
    if (rewind_on_) timeline_.after_frame(*core_, core_->picture());
    if (frames_ % 1800 == 0) save_view(); // every half minute, when it has changed
    uint64_t revision = core_->battery_revision();
    if (revision != saved_revision_) {
        saved_revision_ = revision;
        auto bytes = core_->battery();
        if (!bytes.empty()) writer_->submit(std::move(bytes));
    }
}

void Session::render_companion(Canvas& out, const Look& look) const {
    if (rewind_.is_open()) rewind_.draw(out, look, font_, words_);
    else if (companion_) companion_->render(out, look, font_, words_);
    else out.fill(look.background);
}

uint64_t Session::visual_key() const {
    if (rewind_.is_open()) return rewind_.key() ^ 0x5EEDull << 48;
    return companion_ ? companion_->visual_key() : 0;
}

bool Session::touch(const Touch& t) {
    if (rewind_.is_open()) return rewind_.touch(t);
    return companion_ && companion_->touch(t);
}

bool Session::open_rewind(std::string& why) {
    if (!rewind_on_) {
        why = "Rewind is off";
        return false;
    }
    if (!timeline_.available()) {
        why = timeline_.unavailable_reason();
        return false;
    }
    const uint32_t now = timeline_.mark(*core_, core_->picture());
    if (!now) {
        why = timeline_.available() ? "Not now: the game is busy" : timeline_.unavailable_reason();
        return false;
    }
    timeline_.protect(now);
    shown_moment_ = now;
    rewind_.open(timeline_, now);
    std::fprintf(stderr, "play: rewind opened at moment %u, %zu kept, %zu KiB\n", now, timeline_.size(),
                 timeline_.bytes() / 1024);
    return true;
}

void Session::show_moment(uint32_t id) {
    if (id == shown_moment_) return;
    std::string error;
    if (!timeline_.jump(*core_, id, error)) {
        std::fprintf(stderr, "play: rewind: %s\n", error.c_str());
        shown_moment_ = id; // not again for this one
        return;
    }
    shown_moment_ = id;
    // One silent frame, so the top screen shows the moment: no achievements, no frame count, no sound.
    core_->run_frame(0);
    int16_t discard[2048 * 2];
    while (core_->audio(discard, 2048) == 2048) {}
}

void Session::close_rewind(uint32_t id) {
    std::string error;
    // Exactly as the moment was taken: the preview's frame is not kept.
    if (!timeline_.jump(*core_, id, error)) {
        std::fprintf(stderr, "play: rewind: %s, back to now\n", error.c_str());
        id = rewind_.now();
        if (!timeline_.jump(*core_, id, error)) std::fprintf(stderr, "play: rewind: now: %s\n", error.c_str());
    }
    achievements_.state_loaded();
    timeline_.protect(0);
    rewind_.close();
    shown_moment_ = 0;
    swallow_ = pad_now_;
    std::fprintf(stderr, "play: rewind closed at moment %u%s\n", id, id == rewind_.now() ? " (now)" : "");
}

bool Session::rewind_input(uint32_t buttons, uint64_t ms, bool animations) {
    if (!rewind_.is_open()) return false;
    pad_now_ = buttons;
    rewind_.set_time(ms, animations);
    bool changed = rewind_.pad(buttons) || rewind_.moving();
    uint32_t target = 0;
    switch (rewind_.take(target)) {
    case RewindScreen::Result::Jump: close_rewind(target); return true;
    case RewindScreen::Result::Back: close_rewind(rewind_.now()); return true;
    case RewindScreen::Result::None: break;
    }
    // The top screen shows the moment once the strip rests on it: loading each one it glides past would stall.
    if (rewind_.settled() && rewind_.chosen() != shown_moment_) show_moment(rewind_.chosen());
    return changed;
}

uint32_t Session::menu_rewind_open() {
    if (!rewind_on_ || !timeline_.available()) return 0;
    if (rewind_.is_open()) close_rewind(rewind_.now()); // the dock's own screen gives way to the menu's
    menu_rewind_restore();
    menu_now_ = timeline_.mark(*core_, core_->picture());
    timeline_.protect(menu_now_);
    std::fprintf(stderr, "play: menu rewind opened at moment %u, %zu kept\n", menu_now_, timeline_.size());
    return menu_now_;
}

bool Session::menu_rewind_show(uint32_t id, Picture& shown) {
    if (!menu_now_) return false;
    std::string error;
    if (!timeline_.jump(*core_, id, error)) {
        std::fprintf(stderr, "play: menu rewind: %s\n", error.c_str());
        return false;
    }
    menu_previewed_ = id != menu_now_;
    core_->run_frame(0);
    int16_t discard[2048 * 2];
    while (core_->audio(discard, 2048) == 2048) {}
    const Picture p = core_->picture();
    if (!p.px || p.w <= 0 || p.h <= 0) {
        shown = {};
        return true; // loaded; nothing to read back (a GPU game): the menu keeps the small picture
    }
    menu_shown_.resize(size_t(p.w) * p.h);
    for (int y = 0; y < p.h; ++y)
        std::copy(p.px + size_t(y) * p.stride, p.px + size_t(y) * p.stride + p.w, &menu_shown_[size_t(y) * p.w]);
    shown = {menu_shown_.data(), p.w, p.h, p.w};
    return true;
}

bool Session::menu_rewind_jump(uint32_t id, bool play) {
    if (!menu_now_) return false;
    std::string error;
    if (!timeline_.jump(*core_, id, error)) {
        std::fprintf(stderr, "play: menu rewind: %s\n", error.c_str());
        return false;
    }
    achievements_.state_loaded();
    timeline_.protect(0);
    std::fprintf(stderr, "play: menu rewind: %s moment %u\n", play ? "playing from" : "back to", id);
    menu_now_ = 0;
    menu_previewed_ = false;
    return true;
}

void Session::menu_rewind_restore() {
    if (!menu_previewed_ || !menu_now_) return;
    menu_previewed_ = false;
    std::string error;
    if (!timeline_.jump(*core_, menu_now_, error))
        std::fprintf(stderr, "play: rewind: back to now: %s\n", error.c_str());
    else std::fprintf(stderr, "play: rewind: back to now (a look was still in effect)\n");
}

bool Session::save_state_file(const std::string& path, std::string& error) {
    // Never a checkpoint of a moment only looked at, nor of the Rewind screen's preview.
    menu_rewind_restore();
    if (rewind_.is_open()) close_rewind(rewind_.now());
    if (core_->still_loading()) {
        error = "the game is still loading";
        return false;
    }
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
    achievements_.state_loaded();
    return true;
}

bool Session::start_achievements(const AchievementOptions& options, std::string& error) {
    // The file the core opened (a packed game once unpacked), or the cartridge mGBA holds in memory.
    const std::string opened = core_->content_path();
    const std::string file = opened.empty() ? rom_path_ : opened;
    const uint32_t console = achievement_console(core_->system(), file);
    const std::vector<uint8_t>* data = rom_ && !rom_->empty() ? rom_.get() : nullptr;
    Core* core = core_.get();
    return achievements_.start(
        options, console, file, data,
        [core, console](uint32_t address, uint8_t* buffer, uint32_t size) {
            return core->achievement_memory(console, address, buffer, size);
        },
        nullptr, error);
}

void Session::flush() {
    if (!core_ || !writer_) return;
    // The battery as the game is now, never as a rewind preview left it.
    menu_rewind_restore();
    if (rewind_.is_open()) close_rewind(rewind_.now());
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
