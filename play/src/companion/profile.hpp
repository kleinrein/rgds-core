// SPDX-License-Identifier: GPL-3.0-or-later
// What the companion screen knows about one game, read from a profile file
// (profiles/<system>/<name>.ini). Profiles are data: a new game, or a fix to
// an old one, needs no rebuild.
#pragma once
#include "core/core.hpp"

#include <map>
#include <string>
#include <vector>

namespace play {

struct Step {
    uint32_t buttons = 0;
    int frames = 1;
};

// A value read from the game: mem8[03000BDE], mem16[...], mem32[...], with an
// optional "& mask".
struct Ref {
    uint32_t address = 0;
    int size = 1;
    uint32_t mask = 0xFFFFFFFFu;
    uint32_t read(Core& core) const { return peek_address(core, address, size) & mask; }
};

// ref == v | w | ...,  ref != v,  ref in lo..hi; joined with "and".
struct Condition {
    struct Term {
        Ref ref;
        enum Op { Eq, Ne, In } op = Eq;
        std::vector<uint32_t> values; // Eq/Ne: any of; In: lo, hi
    };
    std::vector<Term> terms;
    bool empty() const { return terms.empty(); }
    bool match(Core& core) const;
    // What each term reads now ("mem16[04000008]=0700 ..."), for the log.
    std::string describe(Core& core) const;
};

struct PixelProbe {
    int x = 0, y = 0;
    uint32_t rgb = 0;
};

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    bool empty() const { return w <= 0 || h <= 0; }
};

// How to bring up the game's own map screen in the shadow copy and know that
// it is really showing.
struct MapSpec {
    int release = 2;        // frames with nothing held first: a button the player
                            // holds in the snapshot would not read as a fresh press
    std::vector<Step> open; // buttons that open it, from gameplay
    int timeout = 180;      // frames to wait for it after `open`
    int stable = 3;         // frames in a row that barely change
    int max_diff = 160;     // pixels allowed to change between them (a blinking marker)
    Rect crop;              // part of the screen kept (empty: all)
    Condition ready;        // memory says the map is up
    std::vector<PixelProbe> pixels;
    int pixels_min = 0; // how many must match (0: all)
    int tolerance = 8;  // per channel
    bool has_background = false;
    uint32_t background = 0; // the map's empty colour
    int min_foreground = 0;  // pixels that must differ from it (not a blank map)
    int anim_frames = 1;     // frames recorded so the player marker still blinks
    int anim_step = 4;       // emulated frames between them
    unsigned layers = 0x1F;  // layers drawn in the capture
};

// A HUD that moves to the second screen (Mario Kart: Super Circuit): while
// `on` matches, its layers are drawn there instead of on the game.
struct HudMove {
    Rect from;        // a box of the HUD (HUD pixels)
    int x = 0, y = 0; // put back onto the game picture with its top left here
};
// A box of the HUD laid out on the second screen: drawn at the largest whole
// scale that fits `to` (in a 640x480 panel's coordinates, scaled to the real
// panel), centred in it. With `trim`, the box is first cut down to the part
// the HUD has drawn in since it came on (a course map's shape differs per course).
struct HudPanel {
    Rect from, to;
    bool trim = false;
};
struct HudSpec {
    Condition on;                            // the HUD moves (racing)
    Condition keep;                          // the last HUD stays up (paused)
    unsigned bg_mask = 1;                    // BG layers that are HUD (bit 0 = BG0)
    int sprite_priority = -1;                // sprites of this priority are HUD (-1: none)
    std::vector<Core::ObjRule> sprite_tiles; // and sprites by tile range (and priority)
    int bg_priority_below = 0;               // >0: the HUD is the BG layers of lower priority (any BG; bg ignored)
    uint32_t map_cnt = 0;                    // >0: on only while some BGxCNT equals this (the map's layer)
    unsigned window_effects_off = 0;         // no shade inside WIN0 (1) / WIN1 (2) on the game screen
    bool backdrop_game = true;               // behind the HUD: the game, dimmed; else a colour
    uint32_t backdrop = 0x0C111A;
    int backdrop_rows = 0;        // only the game's top rows behind the HUD (the sky), 0: all
    unsigned backdrop_layers = 0; // drawn by the shadow copy with only these layers (bit 4:
                                  // sprites), so no kart or Lakitu shows twice; 0: the live picture
    std::vector<HudPanel> panels; // the second screen's layout while moves_when holds
    Condition moves_when;         // the boxes below go back to the game only then
    std::vector<HudMove> to_game; // parts the player must see where they look (item box, place)
    // Game Boy: the HUD is a band of whole rows (the window layer at the
    // bottom, a status bar at the top). While `on` holds the band leaves the
    // game picture and is shown large under the second screen's own picture.
    Rect strip;
};

// [party]: the team on the second screen (companion/party.hpp).
struct PartySpec {
    std::string format;              // "pokemon1", "pokemon2", "pokemon3", "goldensun"
    uint32_t count = 0, slots = 0;   // the member count (a byte) and the first member
    uint32_t icons = 0;              // ROM: one pointer per species to its icon
    uint32_t icon_palette_index = 0; // ROM: one byte per species, which palette
    uint32_t icon_palettes = 0;      // ROM: the palette table ({pointer, tag} pairs)
    uint32_t names = 0;              // pokemon1/2: the nicknames, 11 bytes each
    uint32_t dex_order = 0;          // pokemon1: internal species number -> Pokédex number
    uint32_t icon_kinds = 0;         // pokemon1/2: which icon each species uses
    Condition on;                    // shown only while this holds (empty: whenever there is a team)
};

// [view]: a second screen the player draws itself (companion/view.hpp). The
// format reads its own keys (addresses mostly) from `values`.
struct ViewSpec {
    std::string format;
    std::map<std::string, std::string> values;
    Condition on; // shown only while this holds (empty: always)
};

struct Profile {
    std::string file;
    std::string system = "gba";
    std::string title;
    std::vector<std::string> codes;
    Condition live;        // the player is in gameplay (captures make sense)
    Condition menu;        // back on the title or file screens: forget the map
    std::vector<Ref> room; // values that change on entering a new room
    int interval = 90;     // frames between captures while nothing happens
    bool dim_when_unavailable = false;
    bool has_map = false;
    MapSpec map;
    bool has_hud = false;
    HudSpec hud;
    bool has_party = false;
    PartySpec party;
    bool has_view = false;
    ViewSpec view;
};

// Parses one profile. On failure returns false with "file:line: why".
bool parse_profile(const std::string& text, const std::string& file, Profile& out, std::string& error);
bool parse_buttons(const std::string& text, uint32_t& out);
bool parse_condition(const std::string& text, Condition& out, std::string& error);
// An address in hex, or banked as BANK:ADDRESS (core.hpp's banked()).
bool parse_address(const std::string& text, uint32_t& out);

class ProfileLibrary {
public:
    // Later directories override earlier ones for the same game code, so a
    // user's own profile wins over the shipped one.
    void load_directory(const std::string& directory, std::vector<std::string>& errors);
    const Profile* find(const std::string& system, const std::string& code) const;
    size_t size() const { return profiles_.size(); }

private:
    std::map<std::string, Profile> profiles_; // "system/code"
};

} // namespace play
