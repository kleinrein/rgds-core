// SPDX-License-Identifier: GPL-3.0-or-later
// RG DS Play: a libretro core (one .so, fceumm, snes9x, picodrive, ...) behind the play::Core interface, so every
// system it brings gets RG DS Play's screens, menu, frames and saves. The libretro API has no user pointer in its
// callbacks and a core keeps global state, so one core runs per process; shadow() (the companion's silent copy)
// is therefore not available for these systems.
#include "core.hpp"
#include "libretro/libretro.h"

#include <algorithm>
#include <chrono>
#include <array>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <map>
#include <mutex>
#include <type_traits>

namespace play {

class LibretroCore;

namespace {

LibretroCore* g_active = nullptr; // the one core the callbacks belong to
const GlHost* g_gl = nullptr;     // the player's GLES context for cores that draw with the GPU

// The few GLES 3 calls the adapter makes itself (its framebuffer and the read-back), loaded through the host so the
// player links no GL library of its own.
namespace gl {
using uint = unsigned;
constexpr uint FRAMEBUFFER = 0x8D40, READ_FRAMEBUFFER = 0x8CA8, RENDERBUFFER = 0x8D41, COLOR_ATTACHMENT0 = 0x8CE0,
               DEPTH_STENCIL_ATTACHMENT = 0x821A, DEPTH_ATTACHMENT = 0x8D00, RGBA8 = 0x8058, DEPTH24_STENCIL8 = 0x88F0,
               DEPTH_COMPONENT24 = 0x81A6, RGBA = 0x1908, UNSIGNED_BYTE = 0x1401, FRAMEBUFFER_COMPLETE = 0x8CD5,
               PACK_ALIGNMENT = 0x0D05;
struct Api {
    void (*GenFramebuffers)(int, uint*);
    void (*DeleteFramebuffers)(int, const uint*);
    void (*BindFramebuffer)(uint, uint);
    void (*GenRenderbuffers)(int, uint*);
    void (*DeleteRenderbuffers)(int, const uint*);
    void (*BindRenderbuffer)(uint, uint);
    void (*RenderbufferStorage)(uint, uint, int, int);
    void (*FramebufferRenderbuffer)(uint, uint, uint, uint);
    uint (*CheckFramebufferStatus)(uint);
    void (*PixelStorei)(uint, int);
    void (*ReadPixels)(int, int, int, int, uint, uint, void*);
    bool load() {
        bool ok = true;
        auto get = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(g_gl->proc(name));
            ok = ok && fn;
        };
        get(GenFramebuffers, "glGenFramebuffers");
        get(DeleteFramebuffers, "glDeleteFramebuffers");
        get(BindFramebuffer, "glBindFramebuffer");
        get(GenRenderbuffers, "glGenRenderbuffers");
        get(DeleteRenderbuffers, "glDeleteRenderbuffers");
        get(BindRenderbuffer, "glBindRenderbuffer");
        get(RenderbufferStorage, "glRenderbufferStorage");
        get(FramebufferRenderbuffer, "glFramebufferRenderbuffer");
        get(CheckFramebufferStatus, "glCheckFramebufferStatus");
        get(PixelStorei, "glPixelStorei");
        get(ReadPixels, "glReadPixels");
        return ok;
    }
};
} // namespace gl

// A name as a profile code: "Legend of Zelda, The (USA)" -> LEGEND_OF_ZELDA, "SUPER MARIOKART" -> SUPER_MARIOKART.
std::string title_code(std::string name) {
    for (char open : {'(', '['}) {
        const char close = open == '(' ? ')' : ']';
        for (size_t at; (at = name.find(open)) != std::string::npos;) {
            const size_t end = name.find(close, at);
            name.erase(at, end == std::string::npos ? std::string::npos : end - at + 1);
        }
    }
    std::string out;
    for (char ch : name) {
        auto c = static_cast<unsigned char>(ch);
        if (c == '\'') continue; // Simon's -> SIMONS
        if (std::isalnum(c)) out += char(std::toupper(c));
        else if (!out.empty() && out.back() != '_') out += '_';
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    if (out.size() > 4 && out.compare(out.size() - 4, 4, "_THE") == 0) out.erase(out.size() - 4);
    if (out.rfind("THE_", 0) == 0) out.erase(0, 4);
    return out;
}

uint32_t crc32(const uint8_t* data, size_t size) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

// A cartridge's own bytes, without the header a dumper or a format puts in front: iNES's 16 bytes, a SNES copier's 512.
std::pair<const uint8_t*, size_t> cartridge_bytes(const std::vector<uint8_t>& file, const std::string& system) {
    size_t skip = 0;
    if (system == "nes" && file.size() > 16 && std::memcmp(file.data(), "NES\x1A", 4) == 0) skip = 16;
    if (system == "snes" && file.size() % 1024 == 512) skip = 512;
    return {file.data() + skip, file.size() - skip};
}

// A SNES cartridge's internal name (21 bytes at 7FC0 for LoROM, FFC0 for HiROM): the header whose checksum and
// complement add up wins.
std::string snes_title(const uint8_t* rom, size_t size) {
    for (size_t base : {size_t(0x7FC0), size_t(0xFFC0), size_t(0x40FFC0)}) {
        if (base + 0x20 > size) continue;
        const unsigned complement = rom[base + 0x1C] | (rom[base + 0x1D] << 8);
        const unsigned checksum = rom[base + 0x1E] | (rom[base + 0x1F] << 8);
        if ((complement ^ checksum) != 0xFFFF) continue;
        return std::string(reinterpret_cast<const char*>(rom + base), 21);
    }
    return {};
}

} // namespace

class LibretroCore final : public Core {
public:
    ~LibretroCore() override { close(); }

    bool open(const std::string& library, const std::string& rom_path, const std::string& system,
              const std::string& system_dir, const std::string& save_dir, const std::vector<uint8_t>& battery,
              std::string& error) {
        if (g_active) {
            error = "A libretro core is already running in this process";
            return false;
        }
        lib_ = dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!lib_) {
            error = std::string("The core could not be loaded: ") + dlerror();
            return false;
        }
#define SYM(name)                                                                                                      \
    if (!(name##_ = reinterpret_cast<decltype(name##_)>(dlsym(lib_, #name)))) {                                        \
        error = "The core lacks " #name;                                                                               \
        close();                                                                                                       \
        return false;                                                                                                  \
    }
        SYM(retro_init)
        SYM(retro_deinit)
        SYM(retro_api_version)
        SYM(retro_get_system_info)
        SYM(retro_get_system_av_info)
        SYM(retro_set_environment)
        SYM(retro_set_video_refresh)
        SYM(retro_set_audio_sample)
        SYM(retro_set_audio_sample_batch)
        SYM(retro_set_input_poll)
        SYM(retro_set_input_state)
        SYM(retro_set_controller_port_device)
        SYM(retro_reset)
        SYM(retro_run)
        SYM(retro_serialize_size)
        SYM(retro_serialize)
        SYM(retro_unserialize)
        SYM(retro_load_game)
        SYM(retro_unload_game)
        SYM(retro_get_memory_data)
        SYM(retro_get_memory_size)
#undef SYM
        // Optional: snes9x's record of each scanline's Mode 7 state and its PPU
        // (the palette is 70 bytes in), exported by our build (libretro-snes9x
        // 0001-export-mode7-line-data.patch) and by the libretro buildbot's macOS one.
        mode7_lines_ = reinterpret_cast<const int16_t*>(dlsym(lib_, "LineMatrixData"));
        mode7_ppu_ = reinterpret_cast<const uint8_t*>(dlsym(lib_, "PPU"));
        // Optional: a core without cheats just lists none.
        retro_cheat_reset_ = reinterpret_cast<decltype(retro_cheat_reset_)>(dlsym(lib_, "retro_cheat_reset"));
        retro_cheat_set_ = reinterpret_cast<decltype(retro_cheat_set_)>(dlsym(lib_, "retro_cheat_set"));
        if (retro_api_version_() != RETRO_API_VERSION) {
            error = "The core speaks another libretro API version";
            close();
            return false;
        }
        system_ = system;
        system_dir_ = system_dir;
        save_dir_ = save_dir;
        rom_path_ = rom_path;
        if (system == "n64") {
            // GLideN64 at the N64's own 320 x 240 (a quarter of the drawing and the read-back of 640 x 480; the panel
            // shows it at 2x), on the dynarec and the HLE RSP. The threaded renderer: the emulation on a thread of its
            // own beside GLideN64's drawing took Mario Kart 64's race from ~13 to ~9 ms a frame on the A55s (frames
            // over budget from ~120 to ~15 in 300), for a frame of input lag. No framebuffer emulation: GLideN64 then draws straight into the player's
            // framebuffer, faster on the Mali-G52, and its copy of an emulated framebuffer to the screen made Mesa
            // 25.0's Panfrost crash in pan_preload_emit_pre_frame_dcd a minute into Mario Kart 64 (every run, the same
            // race moment).
            variables_ = {{"mupen64plus-rdp-plugin", "gliden64"},   {"mupen64plus-EnableFBEmulation", "False"},
                          {"mupen64plus-rsp-plugin", "hle"},        {"mupen64plus-cpucore", "dynamic_recompiler"},
                          {"mupen64plus-ThreadedRenderer", "True"}, {"mupen64plus-aspect", "4:3"},
                          {"mupen64plus-43screensize", "320x240"}};
        }
        // RGDS_PLAY_CORE_OPTIONS="key=value;key=value": options over the player's own, for trying a core's settings.
        if (const char* extra = std::getenv("RGDS_PLAY_CORE_OPTIONS")) {
            std::string list = extra;
            for (size_t at = 0; at < list.size();) {
                size_t end = list.find(';', at);
                if (end == std::string::npos) end = list.size();
                const std::string item = list.substr(at, end - at);
                const size_t eq = item.find('=');
                if (eq != std::string::npos) variables_[item.substr(0, eq)] = item.substr(eq + 1);
                at = end + 1;
            }
        }
        if (system == "nes") {
            // A known picture: eight rows of overscan off the top and the bottom (256 x 224), so profiles can name
            // rows; no crosshair over a touch-aimed Zapper.
            variables_ = {{"fceumm_overscan_v_top", "8"},
                          {"fceumm_overscan_v_bottom", "8"},
                          {"fceumm_overscan_h_left", "0"},
                          {"fceumm_overscan_h_right", "0"},
                          {"fceumm_show_crosshair", "disabled"}};
        }
        g_active = this;
        retro_set_environment_(environment_cb);
        retro_init_();
        inited_ = true;
        retro_set_video_refresh_(video_cb);
        retro_set_audio_sample_(audio_sample_cb);
        retro_set_audio_sample_batch_(audio_batch_cb);
        retro_set_input_poll_(input_poll_cb);
        retro_set_input_state_(input_state_cb);

        retro_system_info info{};
        retro_get_system_info_(&info);
        name_ = info.library_name ? info.library_name : "libretro";
        retro_game_info game{};
        game.path = rom_path_.c_str();
        if (!info.need_fullpath) {
            std::ifstream in(rom_path_, std::ios::binary);
            rom_.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            if (rom_.empty()) {
                error = "The ROM could not be read";
                close();
                return false;
            }
            game.data = rom_.data();
            game.size = rom_.size();
        }
        if (!retro_load_game_(&game)) {
            error = hw_refused_ ? "This system needs a GPU context the core can use" : "The core refused the game";
            close();
            return false;
        }
        loaded_ = true;
        retro_system_av_info av{};
        retro_get_system_av_info_(&av);
        apply_av(av);
        retro_set_controller_port_device_(0, RETRO_DEVICE_JOYPAD);
        identify();
        restore_battery(battery);
        battery_seen_ = battery_hash();
        return true;
    }

    const char* system() const override { return system_.c_str(); }
    // The cartridge's CRC-32 (without a dumper's header): "CRC_3FE272FB", the same for every name a file is given.
    std::string game_code() const override { return code_; }
    // The cartridge's own name where it has one (SNES), else the file's name: LEGEND_OF_ZELDA.
    std::string header_title() const override { return title_; }
    double frame_rate() const override { return fps_ > 1.0 ? fps_ : 60.0; }

    void run_frame(uint32_t buttons) override {
        buttons_ = buttons;
        if (mouse_) move_mouse();
        trigger_now_ = trigger_frames_ > 0 || aim_down_;
        if (trigger_frames_ > 0) --trigger_frames_;
        rumble_ = 0;
        if (hw_.context_type != RETRO_HW_CONTEXT_NONE) {
            // A GPU-drawing core runs only with its context current; its first frame makes the context and the
            // framebuffer and tells the core (context_reset), as RetroArch does after loading.
            if (!loaded_ || gpu_failed_ || !g_gl || !g_gl->begin(hw_major_, hw_minor_)) return;
            if (!fbo_ && !make_framebuffer()) {
                g_gl->end();
                gpu_failed_ = true; // stays black instead of retrying every frame
                return;
            }
            const auto run_start = profile_clock::now();
            retro_run_();
            profile_.add(0, run_start);
            // The auto-load: mupen64plus takes a state only once its emulation has started, inside the first
            // retro_run, so after it (a few tries, then given up).
            if (!pending_state_.empty()) {
                if (retro_unserialize_(pending_state_.data(), pending_state_.size())) {
                    std::fprintf(stderr, "play: state loaded at frame %llu\n", (unsigned long long)frames_ + 1);
                    pending_state_.clear();
                } else if (frames_ >= 10) {
                    std::fputs("play: the saved state could not be loaded\n", stderr);
                    pending_state_.clear();
                }
            }
            g_gl->end();
        } else {
            retro_run_();
        }
        if (++frames_ % 60 == 0) {
            const uint64_t h = battery_hash();
            if (h != battery_seen_) {
                battery_seen_ = h;
                ++battery_revision_;
            }
        }
    }
    void set_sticks(int16_t lx, int16_t ly, int16_t rx, int16_t ry) override { sticks_ = {lx, ly, rx, ry}; }
    Picture picture() const override { return {frame_.data(), fw_, fh_, fw_}; }

    bool mode7(Mode7& out) const override {
        if (system_ != "snes" || !mode7_lines_ || !mode7_ppu_ || memory_size(RETRO_MEMORY_VIDEO_RAM) < 0x10000)
            return false;
        out.lines = mode7_lines_;
        out.palette = reinterpret_cast<const uint16_t*>(mode7_ppu_ + 70);
        out.vram = static_cast<const uint8_t*>(memory(RETRO_MEMORY_VIDEO_RAM));
        // snes9x leaves the overscan out of a 224-line picture: its first row is scanline 8.
        out.first_line = fh_ == 224 ? 8 : 0;
        // OBJSEL's parts 2122, 2124 and 2126 bytes in (ppu.h), trusted only while
        // the core's decoded sprite table (OBJ[128], 12 bytes each from 582: HPos,
        // VPos) agrees with the table it decoded (OAMData, 2140 in).
        const uint8_t* oam = mode7_ppu_ + 2140;
        bool same = true;
        for (int i = 0; i < 128 && same; ++i) {
            int16_t h;
            uint16_t v;
            std::memcpy(&h, mode7_ppu_ + 582 + 12 * i, 2);
            std::memcpy(&v, mode7_ppu_ + 584 + 12 * i, 2);
            const int x = oam[4 * i] | (((oam[512 + i / 4] >> (2 * (i & 3))) & 1) << 8);
            same = (x & 0x100 ? x - 512 : x) == h && v == oam[4 * i + 1];
        }
        if (same) {
            uint16_t base, second;
            std::memcpy(&base, mode7_ppu_ + 2122, 2);
            std::memcpy(&second, mode7_ppu_ + 2124, 2);
            out.objsel = true;
            out.obj_base = base;
            out.obj_second = base + 0x2000 + second;
            out.obj_sizes = mode7_ppu_[2126] & 7;
        }
        return out.vram != nullptr;
    }
    float rumble() const override { return rumble_; }

    // A GPU-drawing core emulates on until the state is taken (mupen64plus does), so its context is current then too.
    bool save_state(std::vector<uint8_t>& out) override {
        return on_gpu([&] {
            const size_t size = retro_serialize_size_();
            if (!size) return false;
            out.resize(size);
            if (!retro_serialize_(out.data(), size)) return false;
            // snes9x's libretro side keeps the mouse's place as the sum of every move it was told, outside the
            // state, while the state holds where the SNES last saw it: kept after the state, so a load in a new
            // run can put the sum back (else the core walks the gap 127 counts a poll, the cursor still for seconds).
            if (mouse_) {
                const int16_t sum[2] = {sum_x_, sum_y_};
                out.insert(out.end(), mouse_tag, mouse_tag + 4);
                out.insert(out.end(), reinterpret_cast<const uint8_t*>(sum), reinterpret_cast<const uint8_t*>(sum) + 4);
            }
            return true;
        });
    }
    // retro_cheat_reset, then every code that is on at its own index (cores replace a code by index).
    bool apply_cheats(const std::vector<CheatCode>& codes) override {
        if (!loaded_ || !retro_cheat_reset_ || !retro_cheat_set_) return false;
        retro_cheat_reset_(); // storing codes needs no GL context, so this works before a GPU core's first frame
        for (size_t i = 0; i < codes.size(); ++i)
            if (codes[i].on) retro_cheat_set_(unsigned(i), true, codes[i].code.c_str());
        return true;
    }

    bool load_state(const std::vector<uint8_t>& given) override {
        if (given.empty()) return false;
        std::vector<uint8_t> in = given;
        if (in.size() > 8 && std::memcmp(in.data() + in.size() - 8, mouse_tag, 4) == 0) {
            int16_t sum[2];
            std::memcpy(sum, in.data() + in.size() - 4, 4);
            in.resize(in.size() - 8);
            // The next poll moves the sum to where it was when the state was kept (int16 wraps, as the core's).
            sync_x_ = int16_t(sum[0] - sum_x_), sync_y_ = int16_t(sum[1] - sum_y_), syncing_ = true;
        }
        if (loaded_ && hw_.context_type != RETRO_HW_CONTEXT_NONE && !fbo_ && !gpu_failed_) {
            pending_state_ = in; // the auto-load comes before the first frame makes the context
            return true;
        }
        const bool ok = on_gpu([&] { return retro_unserialize_(in.data(), in.size()); });
        // A state comes back with the core's own idea of what is plugged in: the gun goes back in.
        if (ok && lightgun_) retro_set_controller_port_device_(1, RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_MOUSE, 0));
        return ok;
    }

    // Addresses as the system's CPU sees them: an NES's 0000-07FF and its cartridge RAM at 6000, a SNES's work RAM
    // at 7E0000-7FFFFF, a Master System's at C000-DFFF. Little-endian, one byte at a time, so a value may span pages.
    // An N64's are its RDRAM by physical address (profiles write 0F69A4 for 800F69A4: their top bit marks a banked
    // address), big-endian like its CPU.
    uint32_t peek(uint32_t address, int size) override {
        if (size < 1 || size > 4) return 0;
        uint32_t value = 0;
        if (system_ == "n64") {
            for (int i = 0; i < size; ++i) {
                const uint8_t* p = byte_at(address + uint32_t(i));
                value = (value << 8) | (p ? *p : 0);
            }
            return value;
        }
        for (int i = size - 1; i >= 0; --i) {
            const uint8_t* p = byte_at(address + uint32_t(i));
            value = (value << 8) | (p ? *p : 0);
        }
        return value;
    }
    void poke8(uint32_t address, int bank, uint8_t value) override {
        (void)bank;
        if (const uint8_t* p = byte_at(address)) *const_cast<uint8_t*>(p) = value;
    }

    // Duck Hunt's Zapper: the second port is a light gun aimed at a pixel of the picture, its trigger held a few
    // frames so the game sees the shot and then looks for the light where it was aimed.
    bool set_lightgun(bool on) override {
        if (!loaded_ || system_ != "nes") return false;
        lightgun_ = on;
        // fceumm's Zapper: a subclass of the mouse on the second port.
        retro_set_controller_port_device_(1, on ? RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_MOUSE, 0) : RETRO_DEVICE_JOYPAD);
        return true;
    }
    // Mario Paint's SNES Mouse: the first port is the mouse, moved by touch (core.hpp).
    bool set_mouse(bool on) override {
        if (!loaded_ || system_ != "snes") return false;
        mouse_ = on;
        retro_set_controller_port_device_(0, on ? RETRO_DEVICE_MOUSE : RETRO_DEVICE_JOYPAD);
        return true;
    }
    void set_mouse_cursor(int x, int y) override { cursor_x_ = x, cursor_y_ = y; }
    // This frame's motion and buttons: toward the finger (or where the D-pad nudged it), the left button held only
    // once the cursor is there, so touching down doesn't draw a line from where the cursor was.
    void move_mouse() {
        const bool known = cursor_x_ >= 0 && cursor_y_ >= 0;
        const int cx = known ? cursor_x_ : guess_x_, cy = known ? cursor_y_ : guess_y_;
        const bool finger = aim_down_ && aim_x_ >= 0 && aim_y_ >= 0;
        if (finger) goal_x_ = aim_x_, goal_y_ = aim_y_;
        const int nudge_x = (buttons_ & Button::Right ? 2 : 0) - (buttons_ & Button::Left ? 2 : 0),
                  nudge_y = (buttons_ & Button::Down ? 2 : 0) - (buttons_ & Button::Up ? 2 : 0);
        if (!finger && (nudge_x || nudge_y)) goal_x_ = cx + nudge_x, goal_y_ = cy + nudge_y;
        if (goal_x_ < 0) goal_x_ = cx, goal_y_ = cy;
        // A known cursor is chased by half the way a frame (a game that moves it more than a pixel a count
        // still settles); an unknown one is counted as moving a pixel a count.
        auto step = [&](int to, int at) {
            const int d = to - at;
            return std::clamp(known && std::abs(d) > 1 ? d / 2 : d, -24, 24);
        };
        mouse_dx_ = int16_t(step(goal_x_, cx));
        mouse_dy_ = int16_t(step(goal_y_, cy));
        if (!known) guess_x_ = std::clamp(guess_x_ + mouse_dx_, 0, std::max(0, fw_ - 1)),
                    guess_y_ = std::clamp(guess_y_ + mouse_dy_, 0, std::max(0, fh_ - 1));
        const bool there = std::abs(goal_x_ - cx) <= 2 && std::abs(goal_y_ - cy) <= 2;
        if (!finger) settled_ = false;
        else if (there) settled_ = true;
        mouse_left_ = (finger && settled_) || (buttons_ & Button::A);
        mouse_right_ = (buttons_ & Button::B) != 0;
    }
    int16_t mouse(unsigned id) {
        switch (id) {
        case RETRO_DEVICE_ID_MOUSE_X: {
            const int16_t d = syncing_ ? sync_x_ : mouse_dx_;
            sum_x_ = int16_t(sum_x_ + d);
            return d;
        }
        case RETRO_DEVICE_ID_MOUSE_Y: {
            const int16_t d = syncing_ ? sync_y_ : mouse_dy_;
            sum_y_ = int16_t(sum_y_ + d);
            syncing_ = false; // the core reads X, then Y
            return d;
        }
        case RETRO_DEVICE_ID_MOUSE_LEFT: return mouse_left_ ? 1 : 0;
        case RETRO_DEVICE_ID_MOUSE_RIGHT: return mouse_right_ ? 1 : 0;
        default: return 0;
        }
    }
    void set_pointer(int x, int y, bool pressed) override {
        aim_x_ = x;
        aim_y_ = y;
        if (pressed && !aim_down_) trigger_frames_ = 4;
        aim_down_ = pressed;
    }

    size_t audio(int16_t* stereo, size_t max_frames) override {
        const size_t frames = std::min(max_frames, audio_.size() / 2);
        std::memcpy(stereo, audio_.data(), frames * 2 * sizeof(int16_t));
        audio_.erase(audio_.begin(), audio_.begin() + static_cast<std::ptrdiff_t>(frames * 2));
        return frames;
    }
    unsigned audio_rate() const override { return rate_ ? rate_ : 48000; }

    std::vector<uint8_t> battery() const override {
        const auto* sram = static_cast<const uint8_t*>(memory(RETRO_MEMORY_SAVE_RAM));
        const size_t size = memory_size(RETRO_MEMORY_SAVE_RAM);
        return sram && size ? std::vector<uint8_t>(sram, sram + size) : std::vector<uint8_t>{};
    }
    uint64_t battery_revision() const override { return battery_revision_; }
    void restore_battery(const std::vector<uint8_t>& bytes) override {
        auto* sram = static_cast<uint8_t*>(memory(RETRO_MEMORY_SAVE_RAM));
        const size_t size = memory_size(RETRO_MEMORY_SAVE_RAM);
        if (sram && size && !bytes.empty()) std::memcpy(sram, bytes.data(), std::min(size, bytes.size()));
    }

    void set_layers(unsigned) override {}
    std::unique_ptr<Core> shadow() const override { return nullptr; }

public:
    // The core torn down with its context current (close_gpu_core), the library kept until the destructor.
    void close_gpu() {
        if (hw_.context_type == RETRO_HW_CONTEXT_NONE || !loaded_) return;
        const bool current = g_gl && g_gl->begin(hw_major_, hw_minor_);
        if (current && fbo_ && hw_.context_destroy) hw_.context_destroy();
        retro_unload_game_();
        retro_deinit_();
        loaded_ = inited_ = false;
        if (current) {
            if (fbo_) gl_.DeleteFramebuffers(1, &fbo_);
            if (depth_) gl_.DeleteRenderbuffers(1, &depth_);
            if (colour_) gl_.DeleteRenderbuffers(1, &colour_);
            g_gl->end();
        }
        fbo_ = depth_ = colour_ = 0;
    }

private:
    void close() {
        if (hw_.context_type != RETRO_HW_CONTEXT_NONE) close_gpu();
        if (loaded_) retro_unload_game_();
        if (inited_) retro_deinit_();
        loaded_ = inited_ = false;
        if (g_active == this) g_active = nullptr;
        if (lib_) dlclose(lib_);
        lib_ = nullptr;
        mode7_lines_ = nullptr;
        mode7_ppu_ = nullptr;
    }
    template <class F> bool on_gpu(F&& call) {
        if (!loaded_) return false;
        if (hw_.context_type == RETRO_HW_CONTEXT_NONE) return call();
        if (!fbo_ || gpu_failed_ || !g_gl->begin(hw_major_, hw_minor_)) return false; // no frame drawn yet
        const bool ok = call();
        g_gl->end();
        return ok;
    }
    void* memory(unsigned id) const { return loaded_ ? retro_get_memory_data_(id) : nullptr; }

    // The byte at a CPU address: from the core's memory map (libretro's rules: `select` picks the bits that must
    // match `start`, `disconnect` the bits that are dropped), else from its work RAM where each system keeps it.
    const uint8_t* byte_at(uint32_t address) const {
        if (!loaded_) return nullptr;
        if (system_ == "n64") {
            // mupen64plus keeps RDRAM as the CPU's 32-bit words in the host's order: a byte is at its address ^ 3.
            const auto* ram = static_cast<const uint8_t*>(memory(RETRO_MEMORY_SYSTEM_RAM));
            const uint32_t offset = (address & 0x1FFFFFFF) ^ 3;
            return ram && offset < memory_size(RETRO_MEMORY_SYSTEM_RAM) ? ram + offset : nullptr;
        }
        for (const auto& d : maps_) {
            if (!d.ptr || !d.len || (d.flags & RETRO_MEMDESC_CONST) || (d.addrspace && *d.addrspace)) continue;
            const uint64_t select = d.select ? d.select : ~uint64_t(0);
            if ((address & select) != (d.start & select)) continue;
            if (!d.select && (address < d.start || address - d.start >= d.len)) continue;
            uint64_t offset = reduce(address - d.start, d.disconnect);
            if (offset >= d.len) offset %= d.len;
            return static_cast<const uint8_t*>(d.ptr) + d.offset + offset;
        }
        const auto* ram = static_cast<const uint8_t*>(memory(RETRO_MEMORY_SYSTEM_RAM));
        const size_t total = memory_size(RETRO_MEMORY_SYSTEM_RAM);
        if (!ram || !total) return nullptr;
        uint32_t offset = address;
        if (system_ == "nes") {
            if (address >= 0x6000 && address < 0x8000) {
                const auto* sram = static_cast<const uint8_t*>(memory(RETRO_MEMORY_SAVE_RAM));
                const size_t size = memory_size(RETRO_MEMORY_SAVE_RAM);
                return sram && address - 0x6000 < size ? sram + (address - 0x6000) : nullptr;
            }
            if (address >= 0x2000) return nullptr;
            offset = address & 0x7FF;
        } else if (system_ == "snes") {
            if (address >= 0x7E0000 && address < 0x800000) offset = address - 0x7E0000;
            else if ((address & 0xFFFF) < 0x2000 &&
                     ((address >> 16) < 0x40 || ((address >> 16) >= 0x80 && (address >> 16) < 0xC0)))
                offset = address & 0x1FFF; // the low 8 KiB mirrored into the system banks
        } else if (system_ == "sms" || system_ == "gg") {
            if (address >= 0xC000) offset = (address - 0xC000) & 0x1FFF;
        }
        return offset < total ? ram + offset : nullptr;
    }
    static uint64_t reduce(uint64_t address, uint64_t mask) {
        while (mask) {
            const uint64_t low = (mask & (0 - mask)) - 1; // the bits under the lowest set bit of the mask
            address = (address & low) | ((address >> 1) & ~low);
            mask = (mask & (mask - 1)) >> 1;
        }
        return address;
    }
    void identify() {
        std::vector<uint8_t> file = rom_;
        if (file.empty()) {
            std::ifstream in(rom_path_, std::ios::binary);
            file.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        if (!file.empty()) {
            auto [bytes, size] = cartridge_bytes(file, system_);
            char crc[16];
            std::snprintf(crc, sizeof crc, "CRC_%08X", crc32(bytes, size));
            code_ = crc;
            if (system_ == "snes") title_ = title_code(snes_title(bytes, size));
        }
        if (title_.empty()) {
            auto slash = rom_path_.rfind('/');
            std::string name = slash == std::string::npos ? rom_path_ : rom_path_.substr(slash + 1);
            if (auto dot = name.rfind('.'); dot != std::string::npos) name.erase(dot);
            title_ = title_code(name);
        }
        if (code_.empty()) code_ = title_;
    }
    int16_t lightgun(unsigned id) const {
        const bool aimed = aim_x_ >= 0 && aim_y_ >= 0 && aim_x_ < fw_ && aim_y_ < fh_;
        switch (id) {
        case RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X:
            return aimed ? int16_t(aim_x_ * 0xFFFE / std::max(1, fw_ - 1) - 0x7FFF) : -0x8000;
        case RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y:
            return aimed ? int16_t(aim_y_ * 0xFFFE / std::max(1, fh_ - 1) - 0x7FFF) : -0x8000;
        case RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN: return aimed ? 0 : 1;
        case RETRO_DEVICE_ID_LIGHTGUN_TRIGGER: return trigger_now_ && aimed ? 1 : 0;
        default: return 0;
        }
    }
    size_t memory_size(unsigned id) const { return loaded_ ? retro_get_memory_size_(id) : 0; }
    // The framebuffer the core draws into, at the largest size it said it draws; false when the GPU can't make it.
    bool make_framebuffer() {
        if (!gl_.load()) {
            std::fputs("play: the GPU context lacks framebuffer calls\n", stderr);
            return false;
        }
        const int w = std::max(1, max_w_), h = std::max(1, max_h_);
        gl_.GenFramebuffers(1, &fbo_);
        gl_.BindFramebuffer(gl::FRAMEBUFFER, fbo_);
        gl_.GenRenderbuffers(1, &colour_);
        gl_.BindRenderbuffer(gl::RENDERBUFFER, colour_);
        gl_.RenderbufferStorage(gl::RENDERBUFFER, gl::RGBA8, w, h);
        gl_.FramebufferRenderbuffer(gl::FRAMEBUFFER, gl::COLOR_ATTACHMENT0, gl::RENDERBUFFER, colour_);
        // Stencil only when asked (GLideN64 draws its 3D straight into this framebuffer, so it needs the depth).
        if (hw_.depth || hw_.stencil) {
            gl_.GenRenderbuffers(1, &depth_);
            gl_.BindRenderbuffer(gl::RENDERBUFFER, depth_);
            gl_.RenderbufferStorage(gl::RENDERBUFFER, hw_.stencil ? gl::DEPTH24_STENCIL8 : gl::DEPTH_COMPONENT24, w, h);
            gl_.FramebufferRenderbuffer(gl::FRAMEBUFFER,
                                        hw_.stencil ? gl::DEPTH_STENCIL_ATTACHMENT : gl::DEPTH_ATTACHMENT,
                                        gl::RENDERBUFFER, depth_);
        }
        if (gl_.CheckFramebufferStatus(gl::FRAMEBUFFER) != gl::FRAMEBUFFER_COMPLETE) {
            std::fprintf(stderr, "play: the core's %d x %d framebuffer is incomplete\n", w, h);
            return false;
        }
        std::fprintf(stderr, "play: GPU core framebuffer %d x %d\n", w, h);
        if (hw_.context_reset) hw_.context_reset();
        return true;
    }
    // The frame the core just drew, into frame_: rows bottom-up in GL, bytes R, G, B, A as Picture wants them.
    void read_back(unsigned w, unsigned h) {
        if (!fbo_ || !w || !h) return;
        readback_.resize(static_cast<size_t>(w) * h);
        gl_.BindFramebuffer(gl::READ_FRAMEBUFFER, fbo_);
        gl_.PixelStorei(gl::PACK_ALIGNMENT, 4);
        const auto read_start = profile_clock::now();
        gl_.ReadPixels(0, 0, static_cast<int>(w), static_cast<int>(h), gl::RGBA, gl::UNSIGNED_BYTE, readback_.data());
        profile_.add(1, read_start);
        copy_frame(readback_.data(), w, h);
    }
    void copy_frame(const uint32_t* px, unsigned w, unsigned h) {
        resize(static_cast<int>(w), static_cast<int>(h));
        const bool flip = hw_.bottom_left_origin;
        for (unsigned y = 0; y < h; ++y) {
            const uint32_t* src = px + static_cast<size_t>(flip ? h - 1 - y : y) * w;
            uint32_t* dst = frame_.data() + static_cast<size_t>(y) * fw_;
            for (unsigned x = 0; x < w; ++x) dst[x] = src[x] | 0xFF000000u;
        }
    }
    void apply_av(const retro_system_av_info& av) {
        fps_ = av.timing.fps;
        rate_ = static_cast<unsigned>(av.timing.sample_rate + 0.5);
        max_w_ = static_cast<int>(std::max(av.geometry.max_width, av.geometry.base_width));
        max_h_ = static_cast<int>(std::max(av.geometry.max_height, av.geometry.base_height));
        resize(static_cast<int>(av.geometry.base_width), static_cast<int>(av.geometry.base_height));
    }
    void resize(int w, int h) {
        if (w <= 0 || h <= 0) return;
        if (w != fw_ || h != fh_) {
            fw_ = w;
            fh_ = h;
            frame_.assign(static_cast<size_t>(w) * h, 0xFF000000u);
        }
    }
    uint64_t battery_hash() const {
        const auto* sram = static_cast<const uint8_t*>(memory(RETRO_MEMORY_SAVE_RAM));
        const size_t size = memory_size(RETRO_MEMORY_SAVE_RAM);
        uint64_t h = 1469598103934665603ull;
        for (size_t i = 0; sram && i < size; ++i) h = (h ^ sram[i]) * 1099511628211ull;
        return h;
    }

    // The libretro callbacks.
    static bool environment_cb(unsigned cmd, void* data) { return g_active && g_active->environment(cmd, data); }
    static void video_cb(const void* data, unsigned w, unsigned h, size_t pitch) {
        if (g_active) g_active->video(data, w, h, pitch);
    }
    static void audio_sample_cb(int16_t l, int16_t r) {
        if (!g_active) return;
        g_active->audio_.push_back(l);
        g_active->audio_.push_back(r);
    }
    static size_t audio_batch_cb(const int16_t* data, size_t frames) {
        if (g_active) g_active->audio_.insert(g_active->audio_.end(), data, data + frames * 2);
        return frames;
    }
    static void input_poll_cb() {}
    static int16_t input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id) {
        if (g_active && port == 1 && g_active->lightgun_ && (device & 0xFF) == RETRO_DEVICE_LIGHTGUN)
            return g_active->lightgun(id);
        if (g_active && port == 0 && g_active->mouse_ && (device & 0xFF) == RETRO_DEVICE_MOUSE)
            return g_active->mouse(id);
        if (!g_active || port != 0) return 0;
        if ((device & 0xFF) == RETRO_DEVICE_ANALOG) {
            const auto& s = g_active->sticks_;
            const bool right = index == RETRO_DEVICE_INDEX_ANALOG_RIGHT;
            if (index != RETRO_DEVICE_INDEX_ANALOG_LEFT && !right) return 0;
            return id == RETRO_DEVICE_ID_ANALOG_X ? s[right ? 2 : 0] : s[right ? 3 : 1];
        }
        if ((device & 0xFF) != RETRO_DEVICE_JOYPAD) return 0;
        uint32_t b = g_active->buttons_;
        // N64: the buttons by their labels (mupen64plus reads N64 A from RetroPad B and N64 B from RetroPad Y, an
        // Xbox pad's places; X and Y have no N64 meaning, the C buttons are the right stick), and Z (mupen64plus's
        // L2: items in Mario Kart, aiming in Zelda) on L, where a handheld's thumb rests; the rarely used N64 L
        // shoulder goes to L2.
        if (g_active->system_ == "n64")
            b = (b & ~(A | B | X | Y | L | L2)) | (b & A ? B : 0) | (b & B ? Y : 0) | (b & L ? L2 : 0) |
                (b & L2 ? L : 0);
        static const struct {
            unsigned id;
            uint32_t bit;
        } map[] = {{RETRO_DEVICE_ID_JOYPAD_B, Button::B},           {RETRO_DEVICE_ID_JOYPAD_Y, Button::Y},
                   {RETRO_DEVICE_ID_JOYPAD_SELECT, Button::Select}, {RETRO_DEVICE_ID_JOYPAD_START, Button::Start},
                   {RETRO_DEVICE_ID_JOYPAD_UP, Button::Up},         {RETRO_DEVICE_ID_JOYPAD_DOWN, Button::Down},
                   {RETRO_DEVICE_ID_JOYPAD_LEFT, Button::Left},     {RETRO_DEVICE_ID_JOYPAD_RIGHT, Button::Right},
                   {RETRO_DEVICE_ID_JOYPAD_A, Button::A},           {RETRO_DEVICE_ID_JOYPAD_X, Button::X},
                   {RETRO_DEVICE_ID_JOYPAD_L, Button::L},           {RETRO_DEVICE_ID_JOYPAD_R, Button::R},
                   {RETRO_DEVICE_ID_JOYPAD_L2, Button::L2},         {RETRO_DEVICE_ID_JOYPAD_R2, Button::R2}};
        if (id == RETRO_DEVICE_ID_JOYPAD_MASK) {
            int16_t mask = 0;
            for (const auto& m : map)
                if (b & m.bit) mask |= int16_t(1 << m.id);
            return mask;
        }
        for (const auto& m : map)
            if (m.id == id) return (b & m.bit) ? 1 : 0;
        return 0;
    }
    static void log_cb(enum retro_log_level level, const char* fmt, ...) {
        if (level < RETRO_LOG_INFO) return;
        va_list args;
        va_start(args, fmt);
        std::fputs("play: core: ", stderr);
        std::vfprintf(stderr, fmt, args);
        va_end(args);
    }
    static uintptr_t hw_framebuffer_cb() { return g_active ? g_active->fbo_ : 0; }
    static retro_proc_address_t hw_proc_cb(const char* name) {
        return g_gl ? reinterpret_cast<retro_proc_address_t>(g_gl->proc(name)) : nullptr;
    }
    static bool rumble_cb(unsigned port, enum retro_rumble_effect, uint16_t strength) {
        if (g_active && port == 0) g_active->rumble_ = std::max(g_active->rumble_, strength / 65535.0f);
        return true;
    }

    bool environment(unsigned cmd, void* data) {
        switch (cmd) {
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
            const auto format = *static_cast<const enum retro_pixel_format*>(data);
            if (format != RETRO_PIXEL_FORMAT_XRGB8888 && format != RETRO_PIXEL_FORMAT_RGB565 &&
                format != RETRO_PIXEL_FORMAT_0RGB1555)
                return false;
            pixel_format_ = format;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
            *static_cast<const char**>(data) = system_dir_.c_str();
            return true;
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: *static_cast<const char**>(data) = save_dir_.c_str(); return true;
        case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY:
            *static_cast<const char**>(data) = system_dir_.c_str();
            return true;
        case RETRO_ENVIRONMENT_GET_CAN_DUPE: *static_cast<bool*>(data) = true; return true;
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: static_cast<retro_log_callback*>(data)->log = log_cb; return true;
        case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE:
            static_cast<retro_rumble_interface*>(data)->set_rumble_state = rumble_cb;
            return true;
        case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS: return true;
        case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION: *static_cast<unsigned*>(data) = 2; return true;
        case RETRO_ENVIRONMENT_GET_LANGUAGE: *static_cast<unsigned*>(data) = RETRO_LANGUAGE_ENGLISH; return true;
        case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE: *static_cast<int*>(data) = 3; return true;
        case RETRO_ENVIRONMENT_GET_VARIABLE: {
            auto* v = static_cast<retro_variable*>(data);
            auto it = v->key ? variables_.find(v->key) : variables_.end();
            v->value = it == variables_.end() ? nullptr : it->second.c_str();
            return it != variables_.end();
        }
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: *static_cast<bool*>(data) = false; return true;
        // The core's options stay at the defaults it declares, as RetroArch answers them through GET_VARIABLE (a
        // core can't rely on an unset option: mupen64plus's texture cache size is then 0 and GLideN64 corrupts its
        // heap). What the player set itself (open()) wins.
        case RETRO_ENVIRONMENT_SET_VARIABLES:
            for (const auto* v = static_cast<const retro_variable*>(data); v && v->key; ++v) {
                // "Description; first|second|...": the first value is the default
                const char* values = v->value ? std::strchr(v->value, ';') : nullptr;
                if (!values) continue;
                std::string first(values + 1 + std::strspn(values + 1, " "));
                first = first.substr(0, first.find('|'));
                variables_.emplace(v->key, first);
            }
            return true;
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
            option_defaults(*static_cast<const retro_core_option_definition* const*>(data));
            return true;
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL:
            option_defaults(static_cast<const retro_core_options_intl*>(data)->us);
            return true;
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
            option_defaults(static_cast<const retro_core_options_v2*>(data)->definitions);
            return true;
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL:
            option_defaults(static_cast<const retro_core_options_v2_intl*>(data)->us->definitions);
            return true;
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
        case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
        case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO: return true;
        case RETRO_ENVIRONMENT_SET_MEMORY_MAPS: {
            // Where each part of the system's memory is, so peek() reads the addresses profiles write.
            const auto* map = static_cast<const retro_memory_map*>(data);
            maps_.assign(map->descriptors, map->descriptors + map->num_descriptors);
            return true;
        }
        case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO:
        case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
        case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
        case RETRO_ENVIRONMENT_SET_MESSAGE: return true;
        case RETRO_ENVIRONMENT_SET_GEOMETRY: {
            const auto* g = static_cast<const retro_game_geometry*>(data);
            resize(static_cast<int>(g->base_width), static_cast<int>(g->base_height));
            return true;
        }
        case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
            apply_av(*static_cast<const retro_system_av_info*>(data));
            return true;
        case RETRO_ENVIRONMENT_GET_FASTFORWARDING: *static_cast<bool*>(data) = false; return true;
        case 0x800003: // RetroArch's GET_CLEAR_ALL_THREAD_WAITS_CB: mupen64plus's threaded renderer calls it
            // unchecked when it stops or saves a state; the player has no thread waits to clear
            *static_cast<retro_environment_t*>(data) = [](unsigned, void*) { return true; };
            return true;
        case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:
            *static_cast<unsigned*>(data) = RETRO_HW_CONTEXT_OPENGLES3;
            return g_gl != nullptr;
        case RETRO_ENVIRONMENT_SET_HW_RENDER: {
            // GLES only (the device's Panfrost; desktop GL and Vulkan cores are not offered a context).
            auto* hw = static_cast<retro_hw_render_callback*>(data);
            int major = 2, minor = 0;
            if (hw->context_type == RETRO_HW_CONTEXT_OPENGLES3) major = 3;
            else if (hw->context_type == RETRO_HW_CONTEXT_OPENGLES_VERSION) {
                major = static_cast<int>(hw->version_major);
                minor = static_cast<int>(hw->version_minor);
            } else if (hw->context_type != RETRO_HW_CONTEXT_OPENGLES2) {
                hw_refused_ = true;
                return false;
            }
            if (!g_gl) {
                hw_refused_ = true;
                return false;
            }
            hw->get_current_framebuffer = hw_framebuffer_cb;
            hw->get_proc_address = hw_proc_cb;
            hw_ = *hw;
            hw_major_ = major;
            hw_minor_ = minor;
            return true;
        }
        default: return false; // Vulkan, perf counters, cameras: not here
        }
    }

    // Each option's declared default (its first value when it names none), unless already set.
    template <class Definition> void option_defaults(const Definition* d) {
        for (; d && d->key; ++d) {
            const char* value = d->default_value ? d->default_value : d->values[0].value;
            if (value) variables_.emplace(d->key, value);
        }
    }

    void video(const void* data, unsigned w, unsigned h, size_t pitch) {
        if (!data) return; // a duplicated frame: the last picture stands
        if (data == RETRO_HW_FRAME_BUFFER_VALID) return read_back(w, h);
        resize(static_cast<int>(w), static_cast<int>(h));
        const auto* src = static_cast<const uint8_t*>(data);
        for (unsigned y = 0; y < h; ++y) {
            uint32_t* dst = frame_.data() + static_cast<size_t>(y) * fw_;
            const uint8_t* row = src + y * pitch;
            if (pixel_format_ == RETRO_PIXEL_FORMAT_XRGB8888) {
                const auto* p = reinterpret_cast<const uint32_t*>(row);
                for (unsigned x = 0; x < w; ++x) dst[x] = rgb_to_px(p[x] & 0xFFFFFFu);
            } else if (pixel_format_ == RETRO_PIXEL_FORMAT_RGB565) {
                const auto* p = reinterpret_cast<const uint16_t*>(row);
                for (unsigned x = 0; x < w; ++x) {
                    const uint16_t v = p[x];
                    const uint32_t r = ((v >> 11) & 0x1F) * 255 / 31, g = ((v >> 5) & 0x3F) * 255 / 63,
                                   b = (v & 0x1F) * 255 / 31;
                    dst[x] = rgb_to_px((r << 16) | (g << 8) | b);
                }
            } else { // 0RGB1555
                const auto* p = reinterpret_cast<const uint16_t*>(row);
                for (unsigned x = 0; x < w; ++x) {
                    const uint16_t v = p[x];
                    const uint32_t r = ((v >> 10) & 0x1F) * 255 / 31, g = ((v >> 5) & 0x1F) * 255 / 31,
                                   b = (v & 0x1F) * 255 / 31;
                    dst[x] = rgb_to_px((r << 16) | (g << 8) | b);
                }
            }
        }
    }

    void* lib_ = nullptr;
    bool inited_ = false, loaded_ = false;
    void (*retro_init_)() = nullptr;
    void (*retro_deinit_)() = nullptr;
    unsigned (*retro_api_version_)() = nullptr;
    void (*retro_get_system_info_)(retro_system_info*) = nullptr;
    void (*retro_get_system_av_info_)(retro_system_av_info*) = nullptr;
    void (*retro_set_environment_)(retro_environment_t) = nullptr;
    void (*retro_set_video_refresh_)(retro_video_refresh_t) = nullptr;
    void (*retro_set_audio_sample_)(retro_audio_sample_t) = nullptr;
    void (*retro_set_audio_sample_batch_)(retro_audio_sample_batch_t) = nullptr;
    void (*retro_set_input_poll_)(retro_input_poll_t) = nullptr;
    void (*retro_set_input_state_)(retro_input_state_t) = nullptr;
    void (*retro_set_controller_port_device_)(unsigned, unsigned) = nullptr;
    void (*retro_reset_)() = nullptr;
    void (*retro_run_)() = nullptr;
    size_t (*retro_serialize_size_)() = nullptr;
    bool (*retro_serialize_)(void*, size_t) = nullptr;
    bool (*retro_unserialize_)(const void*, size_t) = nullptr;
    bool (*retro_load_game_)(const retro_game_info*) = nullptr;
    void (*retro_unload_game_)() = nullptr;
    void* (*retro_get_memory_data_)(unsigned) = nullptr;
    size_t (*retro_get_memory_size_)(unsigned) = nullptr;
    void (*retro_cheat_reset_)() = nullptr;
    void (*retro_cheat_set_)(unsigned, bool, const char*) = nullptr;

    std::string system_, name_, rom_path_, system_dir_, save_dir_;
    std::vector<uint8_t> rom_;
    std::string code_, title_; // identify()
    std::vector<retro_memory_descriptor> maps_;
    const int16_t* mode7_lines_ = nullptr;
    const uint8_t* mode7_ppu_ = nullptr;
    bool lightgun_ = false, aim_down_ = false, trigger_now_ = false;
    int aim_x_ = -1, aim_y_ = -1, trigger_frames_ = 0;
    bool mouse_ = false, mouse_left_ = false, mouse_right_ = false, settled_ = false;
    int cursor_x_ = -1, cursor_y_ = -1, guess_x_ = 128, guess_y_ = 112, goal_x_ = -1, goal_y_ = -1;
    int16_t mouse_dx_ = 0, mouse_dy_ = 0;
    int16_t sum_x_ = 0, sum_y_ = 0, sync_x_ = 0, sync_y_ = 0; // what the core has been told in all; a load's catch-up
    bool syncing_ = false;
    static constexpr uint8_t mouse_tag[4] = {'R', 'G', 'M', 'O'};
    std::map<std::string, std::string> variables_;
    enum retro_pixel_format pixel_format_ = RETRO_PIXEL_FORMAT_0RGB1555;
    std::vector<uint32_t> frame_;
    int fw_ = 0, fh_ = 0;
    double fps_ = 60.0;
    unsigned rate_ = 48000;
    std::vector<int16_t> audio_;
    uint32_t buttons_ = 0;
    std::array<int16_t, 4> sticks_{}; // left x, y, right x, y
    float rumble_ = 0;
    retro_hw_render_callback hw_{}; // context_type NONE: the core draws on the CPU
    int hw_major_ = 2, hw_minor_ = 0, max_w_ = 0, max_h_ = 0;
    bool hw_refused_ = false, gpu_failed_ = false;
    gl::Api gl_{};
    gl::uint fbo_ = 0, colour_ = 0, depth_ = 0;
    std::vector<uint32_t> readback_;
    std::vector<uint8_t> pending_state_;
    // RGDS_PLAY_PROFILE=1: every 300 frames, the core's frame (retro_run, the read-back included) and the read-back
    // alone, average and worst in ms, for measuring a GPU core's speed on the device.
    using profile_clock = std::chrono::steady_clock;
    struct Profile {
        bool on = std::getenv("RGDS_PLAY_PROFILE") != nullptr;
        double sum[2]{}, worst[2]{};
        unsigned count[2]{}, over = 0;
        void add(int i, profile_clock::time_point start) {
            if (!on) return;
            const double ms = std::chrono::duration<double, std::milli>(profile_clock::now() - start).count();
            sum[i] += ms;
            worst[i] = std::max(worst[i], ms);
            ++count[i];
            if (i == 0 && ms > 16.7) ++over;
            if (i == 0 && count[0] == 300) {
                std::fprintf(stderr, "play: profile run %.2f avg %.2f worst (%u over 16.7) read %.2f avg %.2f worst\n",
                             sum[0] / count[0], worst[0], over, count[1] ? sum[1] / count[1] : 0.0, worst[1]);
                *this = Profile{};
            }
        }
    } profile_;
    uint64_t frames_ = 0, battery_seen_ = 0, battery_revision_ = 0;
};

void set_gl_host(const GlHost* host) { g_gl = host; }

void close_gpu_core() {
    if (g_active) g_active->close_gpu();
}

OpenResult open_libretro(const std::string& library, const std::string& rom_path, const std::string& system,
                         const std::string& system_dir, const std::string& save_dir,
                         const std::vector<uint8_t>& battery) {
    OpenResult result;
    auto core = std::make_unique<LibretroCore>();
    if (core->open(library, rom_path, system, system_dir, save_dir, battery, result.error))
        result.core = std::move(core);
    return result;
}

} // namespace play
