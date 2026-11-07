#pragma once
// The pictures a profile can have: 16 x 16. The first twelve are '#' ink and '+' half-tone, as the systems' pixel icons;
// the rest are characters in their own colours, one palette letter a pixel, '.' clear.
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace rgds {
inline constexpr size_t profile_picture_count = 24, profile_plain_pictures = 12;
inline std::string_view profile_picture_name(int index) {
    static constexpr std::array<const char*, profile_picture_count> names = {
        "Star", "Heart", "Cat",      "Dog",     "Robot", "Rocket",    "Flower",  "Ghost",
        "Fish", "Crown", "Mushroom", "Rainbow", "Fox",   "Panda",     "Frog",    "Penguin",
        "Owl",  "Bear",  "Unicorn",  "Dino",    "Alien", "Astronaut", "Octopus", "Bunny"};
    return names[size_t(index) % profile_picture_count];
}
inline std::string_view profile_picture(int index) {
    static constexpr std::array<const char*, profile_picture_count> pictures = {{
        // Star
        "................"
        ".......##......."
        ".......##......."
        "......####......"
        "......####......"
        ".##############."
        "..############.."
        "...##########..."
        "....########...."
        "....########...."
        "...##########..."
        "...####..####..."
        "..###......###.."
        "..##........##.."
        "................"
        "................",
        // Heart
        "................"
        "................"
        "...###....###..."
        "..#####..#####.."
        ".##############."
        ".##############."
        ".##############."
        "..############.."
        "...##########..."
        "....########...."
        ".....######....."
        "......####......"
        ".......##......."
        "................"
        "................"
        "................",
        // Cat
        "................"
        "..##........##.."
        "..###......###.."
        "..############.."
        "..############.."
        "..###..##..###.."
        "..###..##..###.."
        "..############.."
        "..######.#####.."
        "..#####...####.."
        "..############.."
        "...##########..."
        "....########...."
        "................"
        "................"
        "................",
        // Dog
        "................"
        ".####......####."
        ".##############."
        ".##############."
        ".##.########.##."
        ".##.##.##.##.##."
        ".##.##.##.##.##."
        "....########...."
        "....###..###...."
        "....###..###...."
        "....########...."
        ".....##..##....."
        ".....######....."
        "......####......"
        "................"
        "................",
        // Robot
        "................"
        ".......##......."
        ".......##......."
        "..############.."
        "..############.."
        "..###..##..###.."
        "..###..##..###.."
        "..############.."
        "..############.."
        "..##.#.##.#.##.."
        "..############.."
        "................"
        ".....######....."
        "...##########..."
        "...##########..."
        "................",
        // Rocket
        ".......##......."
        "......####......"
        ".....######....."
        ".....##..##....."
        ".....##..##....."
        ".....######....."
        ".....######....."
        ".....######....."
        "....########...."
        "...##########..."
        "...##.####.##..."
        "...#..####..#..."
        ".......##......."
        "......+##+......"
        ".......++......."
        "................",
        // Flower
        "................"
        "......####......"
        ".....######....."
        "..##..####..##.."
        ".####.####.####."
        ".#####+..+#####."
        ".#####....#####."
        ".#####....#####."
        ".#####+..+#####."
        ".####.####.####."
        "..##..####..##.."
        ".....######....."
        "......####......"
        "................"
        "................"
        "................",
        // Ghost
        "................"
        ".....######....."
        "....########...."
        "...##########..."
        "...##..##..###.."
        "...##..##..###.."
        "...##..##..###.."
        "...###########.."
        "...###########.."
        "...###########.."
        "...###########.."
        "...###########.."
        "...###.###.###.."
        "...##...#...##.."
        "................"
        "................",
        // Fish
        "................"
        "................"
        "................"
        ".....######....#"
        "...##########.##"
        "..##.#########.#"
        ".#############.#"
        ".##############."
        ".#############.#"
        "..############.#"
        "...##########.##"
        ".....######....#"
        "................"
        "................"
        "................"
        "................",
        // Crown
        "................"
        "................"
        ".##....##....##."
        ".##...####...##."
        ".###..####..###."
        ".####.####.####."
        ".##############."
        ".##############."
        ".##..##..##..##."
        ".##############."
        ".##############."
        "................"
        ".##############."
        "................"
        "................"
        "................",
        // Mushroom
        "................"
        ".....######....."
        "...##########..."
        "..###..####..##."
        ".####..####..###"
        ".###############"
        ".###############"
        "..+###.###.###+."
        ".....######....."
        ".....######....."
        ".....######....."
        ".....######....."
        "......####......"
        "................"
        "................"
        "................",
        // Rainbow
        "................"
        "................"
        ".....######....."
        "...##########..."
        "..####....####.."
        ".###..++++..###."
        ".##.++++++++.##."
        "##.++......++.##"
        "##.+...##...+.##"
        "##.+..#..#..+.##"
        "##.+..#..#..+.##"
        "................"
        "................"
        "................"
        "................"
        "................",
        // Fox
        "................"
        ".kk..........kk."
        ".kOk........kOk."
        ".kOok......koOk."
        ".kooOkkkkkkOook."
        ".kooooooooooook."
        "kooooooooooooook"
        "koowkkoooookkwok"
        "kowwkwkoookwkwwk"
        "kowwwwwoowwwwwok"
        ".kwwwwwkkwwwwwk."
        "..kwwwwkkwwwwk.."
        "...kkwwwwwwkk..."
        ".....kkwwkk....."
        ".......kk......."
        "................",
        // Panda
        "................"
        "..kkk......kkk.."
        ".kkkkkkkkkkkkkk."
        ".kkwwwwwwwwwwkk."
        "..kwwwwwwwwwwk.."
        ".kwwwwwwwwwwwwk."
        ".kwkkkwwwwkkkwk."
        ".kkkwkkwwkkwkkk."
        ".kwkkkwwwwkkkwk."
        ".kwwwwwkkwwwwwk."
        ".kwwwwwkkwwwwwk."
        ".kwwwwkwwkwwwwk."
        "..kwwwwkkwwwwk.."
        "...kkwwwwwwkk..."
        ".....kkkkkk....."
        "................",
        // Frog
        "................"
        "..kkkk....kkkk.."
        ".kwwwwk..kwwwwk."
        ".kwkkwk..kwkkwk."
        ".kwkkwkkkkwkkwk."
        ".kgwwggggggwwgk."
        "kggggggggggggggk"
        "kggggggggggggggk"
        "kgpgggggggggggpk"
        "kggkggggggggkggk"
        "kgggkkkkkkkkgggk"
        ".kgggggggggggggk"
        "..kGGggggggGGk.."
        "...kkkkkkkkkk..."
        "................"
        "................",
        // Penguin
        "................"
        ".....kkkkkk....."
        "....kkkkkkkk...."
        "...kkkkkkkkkk..."
        "..kkkwwkkwwkkk.."
        "..kkwwwwwwwwkk.."
        "..kkwkkwwkkwkk.."
        ".kkkwkwwwwkwkkk."
        ".kkkwwwyywwwkkk."
        "kkkwwwwYYwwwwkkk"
        "kk.kwwwwwwwwk.kk"
        "k..kwwwwwwwwk..k"
        "...kwwwwwwwwk..."
        "....kwwwwwwk...."
        "...yyykkkkyyy..."
        "................",
        // Owl
        "................"
        "..kk........kk.."
        "..knk......knk.."
        "..knnkkkkkknnk.."
        "..knnnnnnnnnnk.."
        ".knwwwnnnnwwwnk."
        ".kwwkkwnnwkkwwk."
        ".kwkkkwyywkkkwk."
        ".kwwkkwYYwkkwwk."
        ".knwwwnyynwwwnk."
        ".knllnnnnnnllnk."
        ".knlNlnnnnlNlnk."
        ".knllllllllllnk."
        "..knllNllNllnk.."
        "...kkyykkyykk..."
        "................",
        // Bear
        "................"
        "..kkk......kkk.."
        ".knnnk....knnnk."
        ".knpnkkkkkknpnk."
        ".knnnnnnnnnnnnk."
        "..knnnnnnnnnnk.."
        ".knnknnnnnnknnk."
        ".knnknnnnnnknnk."
        ".knnnnllllnnnnk."
        ".knnnllllllnnnk."
        ".knnnllkkllnnnk."
        ".knnnllllllnnnk."
        "..knnnnllnnnnk.."
        "...kknnnnnnkk..."
        ".....kkkkkk....."
        "................",
        // Unicorn
        ".......kk......."
        "......kyyk......"
        "..k...kYyk...k.."
        ".kwk..kyYk..kwk."
        ".kwwkkkkkkkkwwk."
        ".kvwwwwwwwwwwvk."
        "kvvwwwwwwwwwwvvk"
        "kvwwkkwwwwkkwwvk"
        "kvwwkkwwwwkkwwpk"
        "kpwwwwwwwwwwwwpk"
        "kppwpwwwwwwpwppk"
        ".kpwwwwwwwwwwpk."
        "..kwwwkwwkwwwk.."
        "...kwwwwwwwwk..."
        "....kkkkkkkk...."
        "................",
        // Dino
        "................"
        ".....kkkkkkk...."
        "....kgggggggk..."
        "...kggwkgggggk.."
        "...kggkkgggggk.."
        "...kgggggggggk.."
        "...kggggggggkk.."
        "..kggggggwkkwk.."
        "..kgggggggwwwk.."
        ".kggGgggkkkkk..."
        "kgggGggggk......"
        "kggGgggggk..kk.."
        "kgggggggggkkgk.."
        ".kggGggggggggk.."
        "..kkkkkkkkkkk..."
        "................",
        // Alien
        "................"
        ".....kkkkkk....."
        "...kkggggggkk..."
        "..kggggggggggk.."
        ".kggggggggggggk."
        ".kgggggggggggggk"
        "kkkkkggggggkkkkk"
        "kkwkkkggggkkkwkk"
        "kkkkkkggggkkkkkk"
        ".kkkkggggggkkkk."
        "..kggggggggggk.."
        "...kggGGGGggk..."
        "....kggggggk...."
        ".....kkkkkk....."
        "................"
        "................",
        // Astronaut
        "................"
        ".....kkkkkk....."
        "...kkeeeeeekk..."
        "..keeeeeeeeeek.."
        ".keekkkkkkkkeek."
        ".kekbbbbbbbbkek."
        ".kekbcbbbbbbkek."
        ".kekbbcbbbbbkek."
        ".kekbbbbbbbbkek."
        ".keekkkkkkkkeek."
        "..keeeeeeeeeek.."
        "..kkeerrrreekk.."
        ".keekeeeeeekeek."
        ".keekeeeeeekeek."
        "..kkkkkkkkkkkk.."
        "................",
        // Octopus
        "................"
        ".....kkkkkk....."
        "...kkvvvvvvkk..."
        "..kvvvvvvvvvvk.."
        ".kvvvwvvvvvvvvk."
        ".kvvwvvvvvvvvvk."
        ".kvvvkkvvkkvvvk."
        ".kvvkwkvvkwkvvk."
        ".kvvvkkvvkkvvvk."
        ".kvpvvvvvvvvpvk."
        "..kvvvvkkvvvvk.."
        ".kvkvvkvvkvvkvk."
        "kvkkvkkvvkkvkkvk"
        "kk.kvk.kk.kvk.kk"
        "...kk......kk..."
        "................",
        // Bunny
        "...kkk...kkk...."
        "..kwwwk.kwwwk..."
        "..kwpwk.kwpwk..."
        "..kwpwk.kwpwk..."
        "..kwpwk.kwpwk..."
        "..kwwwkkkwwwk..."
        ".kwwwwwwwwwwwk.."
        "kwwwwwwwwwwwwwk."
        "kwwkkwwwwwkkwwk."
        "kwwkkwwwwwkkwwk."
        "kwpwwwwkwwwwpwk."
        "kwwwwwkpkwwwwwk."
        ".kwwwwwkwwwwwk.."
        "..kkwwwwwwwkk..."
        "....kkkkkkk....."
        "................",
    }};
    return pictures[size_t(index) % profile_picture_count];
}
inline bool profile_picture_rich(int index) { return size_t(index) % profile_picture_count >= profile_plain_pictures; }
// The smooth drawing of a character (tools/build_profile_avatars.py): assets/profiles/<this>.bmp. The pixel version below
// stands in when the file is missing.
inline std::string_view profile_picture_file(int index) {
    static constexpr std::array<const char*, profile_picture_count - profile_plain_pictures> files = {
        "fox", "panda", "frog", "penguin", "owl", "bear", "unicorn", "dino", "alien", "astronaut", "octopus", "bunny"};
    return profile_picture_rich(index) ? files[size_t(index) % profile_picture_count - profile_plain_pictures] : "";
}
// The characters' colours, as 0xRRGGBB: bright enough on any of the 16 DS colours, with a dark outline.
inline std::optional<uint32_t> profile_palette(char letter) {
    switch (letter) {
    case 'k': return 0x2B2B33;
    case 'w': return 0xFFFFFF;
    case 'o': return 0xF28C28;
    case 'O': return 0xC8641A;
    case 'y': return 0xFFD23F;
    case 'Y': return 0xE0A800;
    case 'p': return 0xFFA8BF;
    case 'P': return 0xE8507A;
    case 'r': return 0xE53935;
    case 'g': return 0x6CC24A;
    case 'G': return 0x2E8B3A;
    case 'b': return 0x5AAAF5;
    case 'B': return 0x2457B8;
    case 'c': return 0x9BE7F2;
    case 'v': return 0xA77BE8;
    case 'V': return 0x6A3FB5;
    case 'n': return 0x9A6440;
    case 'N': return 0x5E3B26;
    case 'e': return 0xC9CDD6;
    case 'E': return 0x7A7F8C;
    case 'l': return 0xFFEBD0;
    case 's': return 0xF6C9A0;
    default: return std::nullopt;
    }
}
}
