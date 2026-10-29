// SPDX-License-Identifier: GPL-3.0-or-later
// The frames the player draws itself, per system, in the menu's order. One
// list for the player (ui/frame.cpp) and the launcher's Frame picker, so a
// frame is only ever offered for the machine it looks like; the slugs are
// what play.ini keeps and runtime/play_options.py repeats (a test compares).
//
// Each is drawn from shapes and the launcher's font (no pictures of real
// consoles, no logos): a handheld's own face around its screen; a home
// console's colours with its name on a strip under the screen; the kind of
// television, monitor or cabinet its games were played on. Systems are the
// launcher's stock system ids (src/launcher/system_icons.hpp), comma
// separated; "" is every system (off and the black bar). The DS and 3DS have
// their own engines and two screens, so nothing here is for them.
#pragma once
#include <cstdint>
#include <string_view>

namespace play {

enum class FrameKind {
    Off,
    Dmg,     // the original Game Boy: grey bezel, rounded low-right corner, the two lines over the screen
    Pocket,  // the Game Boy Pocket: a slim dark bezel, the name under it
    Bezel,   // a black bezel with the name in it under the screen (Game Boy Color, Neo Geo Pocket Color)
    Agb,     // the Game Boy Advance: a wide squat body, the lens in the upper half, the logo and speaker below
    Nes,     // the Nintendo Entertainment System: the red stripe with a blue line under it, the front badge
    Wsc,     // the WonderSwan Color: a tall portrait handheld, the screen in its upper half
    Ngpc,    // the Neo Geo Pocket Color: a portrait handheld, the screen in its upper half
    Psp,     // the PlayStation Portable: a wide lens with the button row and the speaker under it
    Lens,    // a wide dark lens, the name under the screen, a speaker (WonderSwan, PSP)
    Sp,      // the Game Boy Advance SP: a slim lens on the clamshell's lid
    Console, // a home console's colours, its name on a strip under the screen
    Tv,      // a grey television, its controls under the screen
    TvWood,  // a wood-grain television, its dials beside the screen
    Monitor, // a beige computer monitor
    Arcade,  // an arcade cabinet: a lit marquee over the screen, buttons under it
    Vectrex, // the Vectrex: its own upright black screen
    Bar,     // the game across the top, the system's name on a black bar below
    // Only the screen's own bezel, edge to edge, so the game is as large as the panel allows (3x on a Game Boy)
    DmgInner,    // the Game Boy's dark bezel: the two lines and "DOT MATRIX WITH STEREO SOUND" over the screen
    PocketInner, // the Pocket's dark bezel
    BezelInner,  // the Color's black bezel with its name under the screen
    SpInner,     // the SP's lid close up: its window and logo, the game at 2x, the hinge below the panel
};

struct FrameStyleInfo {
    const char* slug;
    const char* system; // stock system ids, comma separated; "" for every system
    const char* name;   // what the Frame picker says
    FrameKind kind;
    uint32_t top, bottom; // the body, lit from above (0xRRGGBB)
    int speckle;          // plastic grain; a clear shell shows its insides
    uint32_t bezel, rim;  // the bezel, lens or picture tube, and the light catching its edge
    uint32_t ink;         // the name printed on the body
    const char* print;    // that name ("" none)
    uint32_t band, line;  // a console's strip and its fine lines; a set's controls; a cabinet's marquee and side art
    int wood;             // wood grain: 1 on the strip, 2 on the body
};

inline constexpr FrameStyleInfo frame_styles[] = {
    {"off", "", "Off", FrameKind::Off, 0, 0, 0, 0, 0, 0, "", 0, 0, 0},
    // Game Boy
    {"dmg", "gb", "Game Boy", FrameKind::Dmg, 0xCBC9C2, 0xB6B4AD, 4, 0x5E6071, 0x7C7E8E, 0x2B2F78, "GAME BOY", 0, 0, 0},
    {"dmg-red", "gb", "Game Boy · Red", FrameKind::Dmg, 0xD2403A, 0xAE2B27, 3, 0x5E6071, 0x7C7E8E, 0xE4E5EC, "GAME BOY",
     0, 0, 0},
    {"dmg-yellow", "gb", "Game Boy · Yellow", FrameKind::Dmg, 0xF3CB2E, 0xD8AC14, 3, 0x5E6071, 0x7C7E8E, 0x2B2F78,
     "GAME BOY", 0, 0, 0},
    {"dmg-green", "gb", "Game Boy · Green", FrameKind::Dmg, 0x3FAE63, 0x2A8A49, 3, 0x5E6071, 0x7C7E8E, 0xE4E5EC,
     "GAME BOY", 0, 0, 0},
    {"dmg-blue", "gb", "Game Boy · Blue", FrameKind::Dmg, 0x3478CC, 0x2358A4, 3, 0x5E6071, 0x7C7E8E, 0xE4E5EC,
     "GAME BOY", 0, 0, 0},
    {"dmg-black", "gb", "Game Boy · Black", FrameKind::Dmg, 0x3A3A3F, 0x26262A, 4, 0x5E6071, 0x7C7E8E, 0xD6D7DF,
     "GAME BOY", 0, 0, 0},
    {"dmg-screen", "gb", "Game Boy · Screen only", FrameKind::DmgInner, 0x666879, 0x555768, 3, 0x5E6071, 0, 0, "", 0, 0,
     0},
    {"pocket-silver", "gb", "Game Boy Pocket · Silver", FrameKind::Pocket, 0xD9DBE0, 0xB2B5BC, 3, 0x383940, 0x5E606B,
     0x3A3C48, "GAME BOY pocket", 0, 0, 0},
    {"pocket-black", "gb", "Game Boy Pocket · Black", FrameKind::Pocket, 0x45464C, 0x2C2D31, 4, 0x17181B, 0x4E5058,
     0xC4C6D0, "GAME BOY pocket", 0, 0, 0},
    {"pocket-yellow", "gb", "Game Boy Pocket · Yellow", FrameKind::Pocket, 0xF4D23A, 0xD9B418, 3, 0x383940, 0x5E606B,
     0x3A3C48, "GAME BOY pocket", 0, 0, 0},
    {"pocket-red", "gb", "Game Boy Pocket · Red", FrameKind::Pocket, 0xD83C36, 0xB22A26, 3, 0x383940, 0x5E606B,
     0xEDEEF3, "GAME BOY pocket", 0, 0, 0},
    {"pocket-green", "gb", "Game Boy Pocket · Green", FrameKind::Pocket, 0x4DB566, 0x338F4B, 3, 0x383940, 0x5E606B,
     0xEDEEF3, "GAME BOY pocket", 0, 0, 0},
    {"pocket-blue", "gb", "Game Boy Pocket · Blue", FrameKind::Pocket, 0x3F7ED0, 0x2A5EA8, 3, 0x383940, 0x5E606B,
     0xEDEEF3, "GAME BOY pocket", 0, 0, 0},
    {"pocket-pink", "gb", "Game Boy Pocket · Pink", FrameKind::Pocket, 0xEF9DB8, 0xD27898, 3, 0x383940, 0x5E606B,
     0x3A3C48, "GAME BOY pocket", 0, 0, 0},
    {"pocket-screen", "gb", "Game Boy Pocket · Screen only", FrameKind::PocketInner, 0x3C3D44, 0x2E2F34, 3, 0x383940, 0,
     0, "", 0, 0, 0},
    // Game Boy Color
    {"gbc-berry", "gbc", "Game Boy Color · Berry", FrameKind::Bezel, 0xC4285A, 0x98163F, 5, 0x1C1C21, 0x4A4A55,
     0xC9CBD3, "GAME BOY COLOR", 0, 0, 0},
    {"gbc-grape", "gbc", "Game Boy Color · Grape", FrameKind::Bezel, 0x6A47A8, 0x46297A, 5, 0x1C1C21, 0x4A4A55,
     0xC9CBD3, "GAME BOY COLOR", 0, 0, 0},
    {"gbc-kiwi", "gbc", "Game Boy Color · Kiwi", FrameKind::Bezel, 0xA6C93C, 0x7C9F1E, 5, 0x1C1C21, 0x4A4A55, 0xC9CBD3,
     "GAME BOY COLOR", 0, 0, 0},
    {"gbc-dandelion", "gbc", "Game Boy Color · Dandelion", FrameKind::Bezel, 0xF4CC2C, 0xD4A40E, 5, 0x1C1C21, 0x4A4A55,
     0xC9CBD3, "GAME BOY COLOR", 0, 0, 0},
    {"gbc-teal", "gbc", "Game Boy Color · Teal", FrameKind::Bezel, 0x27A3AE, 0x137780, 5, 0x1C1C21, 0x4A4A55, 0xC9CBD3,
     "GAME BOY COLOR", 0, 0, 0},
    {"gbc-atomic-purple", "gbc", "Game Boy Color · Atomic Purple", FrameKind::Bezel, 0x7B68A8, 0x4E3F78, 18, 0x1C1C21,
     0x4A4A55, 0xC9CBD3, "GAME BOY COLOR", 0, 0, 0},
    {"gbc-screen", "gbc", "Game Boy Color · Screen only", FrameKind::BezelInner, 0x25252B, 0x1A1A1F, 3, 0x1C1C21, 0,
     0xC9CBD3, "GAME BOY COLOR", 0, 0, 0},
    // Game Boy Advance
    {"indigo", "gba", "Game Boy Advance · Indigo", FrameKind::Agb, 0x5B4FAF, 0x3B3283, 7, 0x22222C, 0x55556A, 0xB9BBC8,
     "GAME BOY ADVANCE", 0, 0, 0},
    {"glacier", "gba", "Game Boy Advance · Glacier", FrameKind::Agb, 0xB5D6EE, 0x86AFD3, 16, 0x252A34, 0x5C6678,
     0xB9BBC8, "GAME BOY ADVANCE", 0, 0, 0},
    {"black", "gba", "Game Boy Advance · Black", FrameKind::Agb, 0x34343A, 0x1E1E22, 5, 0x141417, 0x4A4A52, 0xB9BBC8,
     "GAME BOY ADVANCE", 0, 0, 0},
    {"platinum", "gba", "Game Boy Advance · Platinum", FrameKind::Agb, 0xD9D9DE, 0xAEAEB6, 5, 0x2A2A31, 0x60606C,
     0xB9BBC8, "GAME BOY ADVANCE", 0, 0, 0},
    {"sp-cobalt", "gba", "Game Boy Advance SP · Cobalt", FrameKind::Sp, 0x3A5BB8, 0x253E86, 5, 0x33353B, 0x6B6E78,
     0xEEEFF3, "GAME BOY ADVANCE SP", 0, 0, 0},
    {"sp-onyx", "gba", "Game Boy Advance SP · Onyx", FrameKind::Sp, 0x36363D, 0x1E1E23, 4, 0x33353B, 0x6B6E78, 0xEEEFF3,
     "GAME BOY ADVANCE SP", 0, 0, 0},
    {"sp-platinum", "gba", "Game Boy Advance SP · Platinum", FrameKind::Sp, 0xDCDEE4, 0xA9ACB6, 3, 0x33353B, 0x6B6E78,
     0xEEEFF3, "GAME BOY ADVANCE SP", 0, 0, 0},
    {"sp-cobalt-screen", "gba", "Game Boy Advance SP · Cobalt · Screen only", FrameKind::SpInner, 0x3A5BB8, 0x253E86, 5,
     0x33353B, 0x6B6E78, 0xEEEFF3, "GAME BOY ADVANCE SP", 0, 0, 0},
    {"sp-onyx-screen", "gba", "Game Boy Advance SP · Onyx · Screen only", FrameKind::SpInner, 0x36363D, 0x1E1E23, 4,
     0x33353B, 0x6B6E78, 0xEEEFF3, "GAME BOY ADVANCE SP", 0, 0, 0},
    {"sp-platinum-screen", "gba", "Game Boy Advance SP · Platinum · Screen only", FrameKind::SpInner, 0xDCDEE4,
     0xA9ACB6, 3, 0x33353B, 0x6B6E78, 0xEEEFF3, "GAME BOY ADVANCE SP", 0, 0, 0},
    // Other handhelds
    {"ngpc-silver", "ngp", "Neo Geo Pocket Color · Silver", FrameKind::Ngpc, 0xCDD0D6, 0x9EA2AA, 3, 0x1C1C21, 0x4A4A55,
     0xC9CBD3, "NEOGEO POCKET COLOR", 0, 0, 0},
    {"ngpc-black", "ngp", "Neo Geo Pocket Color · Black", FrameKind::Ngpc, 0x2E2F34, 0x18191C, 4, 0x111114, 0x3E3E48,
     0xC9CBD3, "NEOGEO POCKET COLOR", 0, 0, 0},
    {"wsc-white", "wonderswan", "WonderSwan Color · White", FrameKind::Wsc, 0xEEEEEA, 0xCFCFCA, 3, 0x1E1F24, 0x55565F,
     0xB9BBC8, "WONDERSWAN COLOR", 0, 0, 0},
    {"wsc-blue", "wonderswan", "WonderSwan Color · Blue", FrameKind::Wsc, 0x3D6CCB, 0x24479A, 5, 0x1A1C24, 0x50607E,
     0xC7CEE0, "WONDERSWAN COLOR", 0, 0, 0},
    {"psp-black", "psp", "PSP · Black", FrameKind::Psp, 0x1C1C1F, 0x09090A, 2, 0x0B0B0C, 0x3A3A40, 0x8A8A92, "", 0, 0,
     0},
    {"psp-white", "psp", "PSP · White", FrameKind::Psp, 0xF2F2F0, 0xD5D5D2, 2, 0x111113, 0x45454C, 0x8A8A92, "", 0, 0,
     0},
    // Home consoles, in their own colours
    {"nes", "nes", "Nintendo Entertainment System", FrameKind::Nes, 0x6A6A6E, 0x444448, 3, 0x1C1C1E, 0x55555A, 0xD22630,
     "NINTENDO ENTERTAINMENT SYSTEM", 0x2F2F31, 0x8A8A8E, 0},
    {"snes", "snes", "Super Nintendo", FrameKind::Console, 0xD6D4DC, 0xB4B2BC, 3, 0x1C1C20, 0x5A5868, 0x3E3958,
     "SUPER NINTENDO", 0x3A3A44, 0x6A5BA8, 0},
    {"md", "md", "Mega Drive", FrameKind::Console, 0x242427, 0x111113, 4, 0x0A0A0B, 0x3C3C40, 0xEDEDED,
     "MEGA DRIVE 16-BIT", 0x121214, 0xC8202A, 0},
    {"sms", "sms", "Master System", FrameKind::Console, 0x1E1E20, 0x101012, 4, 0x0A0A0B, 0x3C3C40, 0xF0F0F0,
     "MASTER SYSTEM", 0x0E0E10, 0xD0202A, 0},
    {"pce", "pce", "PC Engine", FrameKind::Console, 0xF4F3EE, 0xDAD8D0, 2, 0x1E1E20, 0x55555A, 0xF2F1EC, "PC ENGINE",
     0xE4E2DA, 0xE06A1A, 0},
    {"neo", "neo", "Neo Geo", FrameKind::Console, 0x1A1A1C, 0x0D0D0E, 4, 0x070708, 0x3A3A3E, 0xD8B45A, "NEO GEO",
     0x101012, 0xC9A34E, 0},
    {"psx", "psx", "PlayStation", FrameKind::Console, 0x3A3A3E, 0x242427, 3, 0x1C1C1E, 0x55555A, 0x3A3A3E,
     "PlayStation", 0x2E2E31, 0x2A5FBF, 0},
    {"n64", "n64", "Nintendo 64", FrameKind::Console, 0x4A5A52, 0x2A3630, 4, 0x0B0B0D, 0x3E3E46, 0xDADADF,
     "NINTENDO 64", 0x36443E, 0x4A4A52, 0},
    {"saturn", "saturn", "Saturn", FrameKind::Console, 0x2B2B30, 0x15151A, 4, 0x09090B, 0x3C3C44, 0xD7DBE6, "SATURN",
     0x0E0E11, 0x3A6FD8, 0},
    {"dc", "dc", "Dreamcast", FrameKind::Console, 0xF2F2F0, 0xDADAD6, 2, 0x1C1C1E, 0x55555A, 0x2B2B2E, "DREAMCAST",
     0xE6E6E3, 0xE86A18, 0},
    {"atari", "atari", "Atari 2600", FrameKind::Console, 0x1E1C1A, 0x0E0D0C, 3, 0x0A0A0A, 0x3A3836, 0xEDE5D2,
     "ATARI 2600", 0x6B4424, 0xC8B89A, 1},
    {"coleco", "coleco", "ColecoVision", FrameKind::Console, 0x1E1E20, 0x0E0E10, 3, 0x0A0A0B, 0x3C3C40, 0xE6E6E6,
     "COLECOVISION", 0x55585F, 0xB8BAC2, 0},
    {"intellivision", "intellivision", "Intellivision", FrameKind::Console, 0x6E4526, 0x4A2C15, 0, 0x0E0D0C, 0x3A3836,
     0x2A1A0C, "INTELLIVISION", 0xC9A34E, 0x8C6A2A, 2},
    {"msx", "msx", "MSX", FrameKind::Console, 0x222224, 0x111113, 3, 0x0A0A0B, 0x3C3C40, 0xE6E6E6, "MSX", 0x2C2C30,
     0x8C8C92, 0},
    {"pico8", "pico8", "PICO-8", FrameKind::Console, 0x1D2B53, 0x111A35, 0, 0x000000, 0x5F574F, 0xFFF1E8, "PICO-8",
     0x7E2553, 0xFF004D, 0},
    // What the games were played on
    {"arcade", "arcade", "Arcade cabinet", FrameKind::Arcade, 0x141416, 0x08080A, 3, 0x050506, 0x2A2A30, 0xFFF4D6,
     "ARCADE", 0xE0402A, 0x2A7BE0, 0},
    {"mvs", "neo", "Arcade cabinet", FrameKind::Arcade, 0x141416, 0x08080A, 3, 0x050506, 0x2A2A30, 0xFFF4D6, "NEO GEO",
     0xD02A2A, 0x2A5BD0, 0},
    {"vectrex", "vectrex", "Vectrex", FrameKind::Vectrex, 0x1C1C1E, 0x0B0B0C, 3, 0x050506, 0x3C3C42, 0xEDEDED,
     "VECTREX", 0x26262A, 0x8C8C92, 0},
    {"tv-grey", "nes,snes,md,sms,pce,neo,psx,n64,saturn,dc,msx", "Grey TV", FrameKind::Tv, 0x9C9C9E, 0x7C7C80, 3,
     0x0E1012, 0x3A3D42, 0x4A4A4E, "", 0x5A5A5E, 0xE0302A, 0},
    {"tv-wood", "nes,sms,atari,coleco,intellivision", "Wood-grain TV", FrameKind::TvWood, 0x7A4E2A, 0x4E2F17, 0,
     0x0E1012, 0x3A3D42, 0x2A2A2C, "", 0xB9B4A8, 0xE0302A, 2},
    {"monitor", "dos,msx", "PC monitor", FrameKind::Monitor, 0xDCD3BC, 0xC4BAA0, 3, 0x0E1012, 0x3A3D42, 0x6F6858, "",
     0xCDC4AA, 0x4CD964, 0},
    {"bar", "", "Black bar", FrameKind::Bar, 0x060607, 0x060607, 0, 0, 0x1E1E24, 0xC4C4CC, "", 0, 0, 0},
};
inline constexpr int frame_style_count = int(sizeof(frame_styles) / sizeof(frame_styles[0]));

// Whether a style is offered for a system's games ("off" and the bar always are).
constexpr bool frame_fits(const FrameStyleInfo& style, std::string_view system) {
    std::string_view list = style.system;
    if (list.empty()) return true;
    while (!list.empty()) {
        const size_t comma = list.find(',');
        if (list.substr(0, comma) == system) return true;
        if (comma == std::string_view::npos) break;
        list.remove_prefix(comma + 1);
    }
    return false;
}

// The mark a console carries on its front, so a frame reads as that machine and not as a strip of text. Each is
// drawn from shapes, in the console's own colours.
enum class Badge {
    None,
    Snes,          // the Super Nintendo: the purple mark on a dark panel
    Md,            // the Mega Drive: SEGA above the red 16-BIT band
    Sms,           // the Master System: the red 16-BIT band
    Pce,           // the PC Engine: the orange HuCARD band
    Neo,           // the Neo Geo: the gold band
    Psx,           // the PlayStation: the blue PS line above the wordmark
    N64,           // the Nintendo 64: the wordmark with the N in colour
    Saturn,        // the Saturn: the wordmark with the planet ring
    Dc,            // the Dreamcast: the orange spiral
    Atari,         // the Atari 2600: the gold Atari mark
    Coleco,        // the ColecoVision: the white and gold mark
    Msx,           // the MSX: the white mark
    Pico8,         // the PICO-8: its own palette, a cart of its own
    Intellivision, // the Intellivision: the tan badge
};

// The badge a style carries, from its slug. The marks are the hardware's own, so each console frame shows the
// logo its console wears.
constexpr Badge badge_of(std::string_view slug) {
    if (slug == "snes") return Badge::Snes;
    if (slug == "md") return Badge::Md;
    if (slug == "sms") return Badge::Sms;
    if (slug == "pce") return Badge::Pce;
    if (slug == "neo") return Badge::Neo;
    if (slug == "psx") return Badge::Psx;
    if (slug == "n64") return Badge::N64;
    if (slug == "saturn") return Badge::Saturn;
    if (slug == "dc") return Badge::Dc;
    if (slug == "atari") return Badge::Atari;
    if (slug == "coleco") return Badge::Coleco;
    if (slug == "msx") return Badge::Msx;
    if (slug == "pico8") return Badge::Pico8;
    if (slug == "intellivision") return Badge::Intellivision;
    return Badge::None;
}

} // namespace play
