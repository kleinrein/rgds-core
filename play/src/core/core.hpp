// SPDX-License-Identifier: GPL-3.0-or-later
// RG DS Play: one player for every system that is not the DS.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace play {

// The pad as the player holds it, whatever the system. The first ten bits are
// the GBA's own key bits, so the GBA core passes them straight through; other
// systems map these to theirs.
enum Button : uint32_t {
    A = 1u << 0,
    B = 1u << 1,
    Select = 1u << 2,
    Start = 1u << 3,
    Right = 1u << 4,
    Left = 1u << 5,
    Up = 1u << 6,
    Down = 1u << 7,
    R = 1u << 8,
    L = 1u << 9,
    X = 1u << 10,
    Y = 1u << 11,
    L2 = 1u << 12,
    R2 = 1u << 13,
};

// Pixels are 32-bit words whose bytes are R, G, B, X in memory (mGBA's own
// order, SDL_PIXELFORMAT_ABGR8888 on a little-endian machine).
struct Picture {
    const uint32_t* px = nullptr;
    int w = 0, h = 0, stride = 0; // stride in pixels
    uint32_t at(int x, int y) const { return px[static_cast<size_t>(y) * stride + x]; }
};

inline uint32_t rgb_to_px(uint32_t rgb) {
    return ((rgb >> 16) & 0xFF) | (rgb & 0xFF00) | ((rgb & 0xFF) << 16) | 0xFF000000u;
}
inline uint32_t px_to_rgb(uint32_t px) { return ((px & 0xFF) << 16) | (px & 0xFF00) | ((px >> 16) & 0xFF); }

// One running game. The companion screen asks for a shadow(): a silent copy of
// the same game that it can load the live game's state into and play on its
// own, without ever touching the game being played.
class Core {
public:
    virtual ~Core() = default;
    virtual const char* system() const = 0; // "gba", "gb", "gbc"
    virtual std::string game_code() const = 0;
    virtual std::string header_title() const = 0;
    virtual double frame_rate() const = 0;

    virtual void run_frame(uint32_t buttons) = 0;
    virtual Picture picture() const = 0;

    virtual bool save_state(std::vector<uint8_t>& out) = 0;
    virtual bool load_state(const std::vector<uint8_t>& in) = 0;

    // Side-effect-free reads of the bus (RAM, I/O as last written).
    virtual uint32_t peek(uint32_t address, int size) = 0;
    // The same from one bank of banked memory, whatever the game has mapped
    // now: a Game Boy cartridge's ROM bank at 4000-7FFF, a Game Boy Color's
    // work RAM bank at D000-DFFF. -1: as mapped.
    virtual uint32_t peek_bank(uint32_t address, int bank, int size) {
        (void)bank;
        return peek(address, size);
    }

    virtual size_t audio(int16_t* stereo, size_t max_frames) = 0;
    virtual unsigned audio_rate() const = 0;

    // Battery save as the game last flushed it. `revision` changes each time
    // the game finishes writing its save (mGBA's settle-then-sync point).
    virtual std::vector<uint8_t> battery() const = 0;
    virtual uint64_t battery_revision() const = 0;
    // A shadow's battery memory set to the live game's (some games read it as
    // they draw: the European Oracles keep their language there). Memory only:
    // a shadow never writes a save file.
    virtual void restore_battery(const std::vector<uint8_t>& bytes) { (void)bytes; }

    // Layers drawn: bits 0-3 BG0-3, bit 4 sprites (0x1F = everything).
    virtual void set_layers(unsigned mask) = 0;

    // HUD split: the BG layers in bg_mask (bit 0 = BG0) and the sprites of
    // priority sprite_priority (-1: none) are drawn into hud() instead of the
    // game picture, from the next frame. Display only: the game's state is
    // the same either way. False when the core can't split.
    virtual bool hud_split(bool on, unsigned bg_mask, int sprite_priority) {
        (void)on, (void)bg_mask, (void)sprite_priority;
        return false;
    }
    // Sprites that are HUD by their first tile (lo..hi) and priority (-1: any),
    // as well as sprite_priority's; set before hud_split(true).
    struct ObjRule {
        int lo = 0, hi = 1023, priority = -1;
    };
    virtual void hud_objects(const std::vector<ObjRule>& rules) { (void)rules; }
    // While split, no colour effect inside these windows (bit 0 WIN0, bit 1 WIN1).
    virtual void hud_window_effects_off(unsigned windows) { (void)windows; }
    // The HUD as last drawn; pixels equal to hud_key() (low 24 bits) are empty.
    virtual Picture hud() const { return {}; }
    virtual uint32_t hud_key() const { return 0; }

    // A silent copy for the companion: no audio, no battery file, its own
    // memory. Null when the core cannot make one.
    virtual std::unique_ptr<Core> shadow() const = 0;
};

// A banked address ("1C:590D": ROM bank 1C; "1:DCD7": work RAM bank 1), as
// profiles write them, in one number: bit 31 set, the bank in bits 16-23.
constexpr uint32_t banked_flag = 0x80000000u;
constexpr uint32_t banked(int bank, uint32_t address) {
    return banked_flag | (uint32_t(bank & 0xFF) << 16) | (address & 0xFFFF);
}
// Reads a plain or banked address.
inline uint32_t peek_address(Core& core, uint32_t address, int size) {
    if (!(address & banked_flag)) return core.peek(address, size);
    return core.peek_bank(address & 0xFFFF, int((address >> 16) & 0xFF), size);
}

// ROM bytes are shared read-only between the live core and its shadows.
using RomBytes = std::shared_ptr<const std::vector<uint8_t>>;

struct OpenResult {
    std::unique_ptr<Core> core;
    std::string error;
};

// Reads .gba/.gb/.gbc, or the first ROM inside a .zip or .7z.
RomBytes read_rom(const std::string& path, std::string& error, std::string* inner_name = nullptr);
bool is_rom_name(const std::string& name);
OpenResult open_mgba(RomBytes rom, const std::string& name, const std::vector<uint8_t>& battery);

} // namespace play
