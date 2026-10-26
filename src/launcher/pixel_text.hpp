#pragma once
#include <array>
#include <map>
#include <string>
#include <vector>

namespace rgds {
// Classic 5x7 text uses an uppercase base plus separate Vietnamese shape/tone
// marks. Shapes: circumflex, breve, horn, D stroke. Tones: grave, acute,
// hook above, tilde, dot below. Precomposed UTF-8 does not lose either mark.
struct VietnamesePixelGlyph {
    unsigned base = 0;
    int shape = 0, tone = 0;
};
inline VietnamesePixelGlyph vietnamese_pixel_glyph(unsigned cp) {
    static constexpr std::array<VietnamesePixelGlyph, 45> vowels = {{
        {'A', 0, 5}, // Ạ ạ
        {'A', 0, 3}, // Ả ả
        {'A', 1, 2}, // Ấ ấ
        {'A', 1, 1}, // Ầ ầ
        {'A', 1, 3}, // Ẩ ẩ
        {'A', 1, 4}, // Ẫ ẫ
        {'A', 1, 5}, // Ậ ậ
        {'A', 2, 2}, // Ắ ắ
        {'A', 2, 1}, // Ằ ằ
        {'A', 2, 3}, // Ẳ ẳ
        {'A', 2, 4}, // Ẵ ẵ
        {'A', 2, 5}, // Ặ ặ
        {'E', 0, 5}, // Ẹ ẹ
        {'E', 0, 3}, // Ẻ ẻ
        {'E', 0, 4}, // Ẽ ẽ
        {'E', 1, 2}, // Ế ế
        {'E', 1, 1}, // Ề ề
        {'E', 1, 3}, // Ể ể
        {'E', 1, 4}, // Ễ ễ
        {'E', 1, 5}, // Ệ ệ
        {'I', 0, 3}, // Ỉ ỉ
        {'I', 0, 5}, // Ị ị
        {'O', 0, 5}, // Ọ ọ
        {'O', 0, 3}, // Ỏ ỏ
        {'O', 1, 2}, // Ố ố
        {'O', 1, 1}, // Ồ ồ
        {'O', 1, 3}, // Ổ ổ
        {'O', 1, 4}, // Ỗ ỗ
        {'O', 1, 5}, // Ộ ộ
        {'O', 3, 2}, // Ớ ớ
        {'O', 3, 1}, // Ờ ờ
        {'O', 3, 3}, // Ở ở
        {'O', 3, 4}, // Ỡ ỡ
        {'O', 3, 5}, // Ợ ợ
        {'U', 0, 5}, // Ụ ụ
        {'U', 0, 3}, // Ủ ủ
        {'U', 3, 2}, // Ứ ứ
        {'U', 3, 1}, // Ừ ừ
        {'U', 3, 3}, // Ử ử
        {'U', 3, 4}, // Ữ ữ
        {'U', 3, 5}, // Ự ự
        {'Y', 0, 1}, // Ỳ ỳ
        {'Y', 0, 5}, // Ỵ ỵ
        {'Y', 0, 3}, // Ỷ ỷ
        {'Y', 0, 4}, // Ỹ ỹ
    }};
    if (cp >= 0x1ea0 && cp <= 0x1ef9) return vowels[(cp - 0x1ea0) / 2];
    switch (cp) {
    case 0x102:
    case 0x103: return {'A', 2, 0};
    case 0x110:
    case 0x111: return {'D', 4, 0};
    case 0x128:
    case 0x129: return {'I', 0, 4};
    case 0x168:
    case 0x169: return {'U', 0, 4};
    case 0x1a0:
    case 0x1a1: return {'O', 3, 0};
    case 0x1af:
    case 0x1b0: return {'U', 3, 0};
    default: return {};
    }
}

// Polish, Turkish and Cyrillic letters drawn as a base plus the classic text's
// accent number: 2 acute, 5 diaeresis, 6 cedilla, 7 ogonek, 8 dot above,
// 9 breve, 10 stroke. Cyrillic bases without a Latin twin stay Cyrillic
// capitals and draw from cyrillic_pixel_bitmap.
struct ExtendedPixelGlyph {
    unsigned base = 0;
    int accent = 0;
};
inline ExtendedPixelGlyph extended_pixel_glyph(unsigned cp) {
    if (cp >= 0x430 && cp <= 0x44f) cp -= 0x20;
    else if (cp >= 0x450 && cp <= 0x45f) cp -= 0x50;
    else if (cp == 0x491) cp = 0x490;
    switch (cp) {
    case 0x104:
    case 0x105: return {'A', 7};
    case 0x106:
    case 0x107: return {'C', 2};
    case 0x118:
    case 0x119: return {'E', 7};
    case 0x11e:
    case 0x11f: return {'G', 9};
    case 0x130: return {'I', 8};
    case 0x131: return {'I', 0};
    case 0x141:
    case 0x142: return {'L', 10};
    case 0x143:
    case 0x144: return {'N', 2};
    case 0x15a:
    case 0x15b: return {'S', 2};
    case 0x15e:
    case 0x15f: return {'S', 6};
    case 0x179:
    case 0x17a: return {'Z', 2};
    case 0x17b:
    case 0x17c: return {'Z', 8};
    case 0x401: return {'E', 5}; // Ё
    case 0x406: return {'I', 0}; // І
    case 0x407: return {'I', 5}; // Ї
    case 0x410: return {'A', 0};
    case 0x412: return {'B', 0};
    case 0x415: return {'E', 0};
    case 0x419: return {0x418, 9}; // Й
    case 0x41a: return {'K', 0};
    case 0x41c: return {'M', 0};
    case 0x41d: return {'H', 0};
    case 0x41e: return {'O', 0};
    case 0x420: return {'P', 0};
    case 0x421: return {'C', 0};
    case 0x422: return {'T', 0};
    case 0x425: return {'X', 0};
    default: return (cp >= 0x400 && cp <= 0x42f) || cp == 0x490 ? ExtendedPixelGlyph{cp, 0} : ExtendedPixelGlyph{};
    }
}
// Kana, kanji and other CJK characters have no 5×7 form; the classic text draws
// the rounded glyph in a two-cell box instead. Halfwidth katakana stay one cell.
inline bool pixel_wide(unsigned cp) { return cp >= 0x2e80 && !(cp >= 0xff61 && cp <= 0xffdc); }
inline std::string utf8_encode(unsigned cp) {
    std::string out;
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) {
        out += static_cast<char>(0xc0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xe0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    } else {
        out += static_cast<char>(0xf0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    }
    return out;
}
inline const std::array<unsigned char, 7>* cyrillic_pixel_bitmap(unsigned cp) {
    struct Entry {
        unsigned cp;
        std::array<unsigned char, 7> rows;
    };
    static constexpr std::array<Entry, 22> letters = {{
        {0x404, {14, 17, 16, 28, 16, 17, 14}}, // Є
        {0x411, {31, 16, 16, 30, 17, 17, 30}}, // Б
        {0x413, {31, 16, 16, 16, 16, 16, 16}}, // Г
        {0x414, {6, 10, 10, 10, 10, 31, 17}},  // Д
        {0x416, {21, 21, 21, 14, 21, 21, 21}}, // Ж
        {0x417, {30, 1, 1, 14, 1, 1, 30}},     // З
        {0x418, {17, 17, 19, 21, 25, 17, 17}}, // И
        {0x41b, {15, 9, 9, 9, 9, 9, 25}},      // Л
        {0x41f, {31, 17, 17, 17, 17, 17, 17}}, // П
        {0x423, {17, 17, 17, 15, 1, 1, 14}},   // У
        {0x424, {4, 14, 21, 21, 21, 14, 4}},   // Ф
        {0x426, {18, 18, 18, 18, 18, 31, 1}},  // Ц
        {0x427, {17, 17, 17, 15, 1, 1, 1}},    // Ч
        {0x428, {21, 21, 21, 21, 21, 21, 31}}, // Ш
        {0x429, {21, 21, 21, 21, 21, 31, 1}},  // Щ
        {0x42a, {24, 8, 8, 14, 9, 9, 14}},     // Ъ
        {0x42b, {17, 17, 17, 25, 21, 21, 25}}, // Ы
        {0x42c, {16, 16, 16, 30, 17, 17, 30}}, // Ь
        {0x42d, {14, 17, 1, 7, 1, 17, 14}},    // Э
        {0x42e, {18, 21, 21, 29, 21, 21, 18}}, // Ю
        {0x42f, {15, 17, 17, 15, 5, 9, 17}},   // Я
        {0x490, {1, 31, 16, 16, 16, 16, 16}},  // Ґ
    }};
    for (const auto& letter : letters)
        if (letter.cp == cp) return &letter.rows;
    return nullptr;
}
// The classic 5x7 text: one row of five dots a byte (bit 4 the left dot).
// Shared with RG DS Play's companion screen, so Pixel classic reads the same there.
inline const std::map<char, std::array<unsigned char, 7>>& pixel_glyphs() {
    static const std::map<char, std::array<unsigned char, 7>> glyphs = {{'A', {14, 17, 17, 31, 17, 17, 17}},
                                                                    {'B', {30, 17, 17, 30, 17, 17, 30}},
                                                                    {'C', {14, 17, 16, 16, 16, 17, 14}},
                                                                    {'D', {30, 17, 17, 17, 17, 17, 30}},
                                                                    {'E', {31, 16, 16, 30, 16, 16, 31}},
                                                                    {'F', {31, 16, 16, 30, 16, 16, 16}},
                                                                    {'G', {14, 17, 16, 23, 17, 17, 15}},
                                                                    {'H', {17, 17, 17, 31, 17, 17, 17}},
                                                                    {'I', {14, 4, 4, 4, 4, 4, 14}},
                                                                    {'J', {7, 2, 2, 2, 18, 18, 12}},
                                                                    {'K', {17, 18, 20, 24, 20, 18, 17}},
                                                                    {'L', {16, 16, 16, 16, 16, 16, 31}},
                                                                    {'M', {17, 27, 21, 21, 17, 17, 17}},
                                                                    {'N', {17, 25, 21, 19, 17, 17, 17}},
                                                                    {'O', {14, 17, 17, 17, 17, 17, 14}},
                                                                    {'P', {30, 17, 17, 30, 16, 16, 16}},
                                                                    {'Q', {14, 17, 17, 17, 21, 18, 13}},
                                                                    {'R', {30, 17, 17, 30, 20, 18, 17}},
                                                                    {'S', {15, 16, 16, 14, 1, 1, 30}},
                                                                    {'T', {31, 4, 4, 4, 4, 4, 4}},
                                                                    {'U', {17, 17, 17, 17, 17, 17, 14}},
                                                                    {'V', {17, 17, 17, 17, 17, 10, 4}},
                                                                    {'W', {17, 17, 17, 21, 21, 21, 10}},
                                                                    {'X', {17, 17, 10, 4, 10, 17, 17}},
                                                                    {'Y', {17, 17, 10, 4, 4, 4, 4}},
                                                                    {'Z', {31, 1, 2, 4, 8, 16, 31}},
                                                                    {'0', {14, 17, 19, 21, 25, 17, 14}},
                                                                    {'1', {4, 12, 4, 4, 4, 4, 14}},
                                                                    {'2', {14, 17, 1, 2, 4, 8, 31}},
                                                                    {'3', {30, 1, 1, 14, 1, 1, 30}},
                                                                    {'4', {2, 6, 10, 18, 31, 2, 2}},
                                                                    {'5', {31, 16, 16, 30, 1, 1, 30}},
                                                                    {'6', {14, 16, 16, 30, 17, 17, 14}},
                                                                    {'7', {31, 1, 2, 4, 8, 8, 8}},
                                                                    {'8', {14, 17, 17, 14, 17, 17, 14}},
                                                                    {'9', {14, 17, 17, 15, 1, 1, 14}},
                                                                    {'.', {0, 0, 0, 0, 0, 6, 6}},
                                                                    {':', {0, 6, 6, 0, 6, 6, 0}},
                                                                    {'-', {0, 0, 0, 31, 0, 0, 0}},
                                                                    {'/', {1, 2, 2, 4, 8, 8, 16}},
                                                                    {'+', {0, 4, 4, 31, 4, 4, 0}},
                                                                    {'?', {14, 17, 1, 2, 4, 0, 4}},
                                                                    {'!', {4, 4, 4, 4, 4, 0, 4}},
                                                                    {'=', {0, 31, 0, 31, 0, 0, 0}},
                                                                    {'%', {25, 25, 2, 4, 8, 19, 19}},
                                                                    {'[', {14, 8, 8, 8, 8, 8, 14}},
                                                                    {']', {14, 2, 2, 2, 2, 2, 14}},
                                                                    {'(', {2, 4, 8, 8, 8, 4, 2}},
                                                                    {')', {8, 4, 2, 2, 2, 4, 8}},
                                                                    {'<', {1, 2, 4, 8, 4, 2, 1}},
                                                                    {'>', {16, 8, 4, 2, 4, 8, 16}},
                                                                    {'_', {0, 0, 0, 0, 0, 0, 31}},
                                                                    {' ', {0, 0, 0, 0, 0, 0, 0}},
                                                                    {char(0xdf), {14, 17, 17, 22, 17, 17, 22}},
                                                                    {char(0xc6), {15, 20, 20, 30, 20, 20, 23}},
                                                                    {char(0xd8), {15, 19, 21, 21, 25, 17, 30}},
                                                                    {char(0xc5), {4, 10, 4, 14, 17, 31, 17}},
                                                                    {'&', {12, 18, 20, 8, 21, 18, 13}},
                                                                    {'\'', {4, 4, 8, 0, 0, 0, 0}},
                                                                    {char(0xb7), {0, 0, 0, 6, 6, 0, 0}},
                                                                    {',', {0, 0, 0, 0, 0, 4, 8}}};
    return glyphs;
}

// UTF-8 to codepoints, '?' for a broken sequence (as the rounded font reads text).
inline std::vector<unsigned> pixel_codepoints(const std::string& value) {
    std::vector<unsigned> out;
    for (size_t i = 0; i < value.size();) {
        unsigned cp = static_cast<unsigned char>(value[i++]);
        int extra = cp < 128 ? 0 : ((cp & 0xe0) == 0xc0 ? 1 : ((cp & 0xf0) == 0xe0 ? 2 : ((cp & 0xf8) == 0xf0 ? 3 : -1)));
        if (extra < 0) {
            out.push_back('?');
            continue;
        }
        if (extra) cp &= (1u << (6 - extra)) - 1;
        for (int n = 0; n < extra; n++) {
            if (i >= value.size() || (static_cast<unsigned char>(value[i]) & 0xc0) != 0x80) {
                cp = '?';
                break;
            }
            cp = (cp << 6) | (static_cast<unsigned char>(value[i++]) & 63);
        }
        out.push_back(cp);
    }
    return out;
}
// Classic 5x7 text as filled squares: `fill` takes each dot's Rect{x, y, w, h}
// and `wide` each kana or kanji cell (the caller draws those from the rounded
// font). `y` is the line box's top, as for the rounded font.
template <class Rect, class Fill, class Wide>
void pixel_text_dots(const std::string& value, int x, int y, int size, int max_chars, Fill&& fill, Wide&& wide) {
    const auto& glyphs = pixel_glyphs();
    y += size * 3; // Match the rounded font's visible top, rather than its line-box top.
    int n = 0;
    for (unsigned c : pixel_codepoints(value)) {
        if (++n > max_chars) break;
        if (pixel_wide(c)) {
            wide(c, x, y);
            x += size * 12;
            continue;
        }
        unsigned accent = 0;
        auto vietnamese = vietnamese_pixel_glyph(c);
        if (vietnamese.base) c = vietnamese.base;
        if (c >= 0xe0 && c <= 0xfe && c != 0xf7) c -= 32;
        auto extended = extended_pixel_glyph(c);
        if (extended.base) {
            c = extended.base;
            accent = static_cast<unsigned>(extended.accent);
        }
        switch (c) {
        case 0xc0:
        case 0xc1:
        case 0xc2:
        case 0xc3:
        case 0xc4:
            accent = c - 0xc0 + 1;
            c = 'A';
            break;
        case 0xc8:
        case 0xc9:
        case 0xca:
        case 0xcb:
            accent = c == 0xcb ? 5 : c - 0xc8 + 1;
            c = 'E';
            break;
        case 0xcc:
        case 0xcd:
        case 0xce:
        case 0xcf:
            accent = c == 0xcf ? 5 : c - 0xcc + 1;
            c = 'I';
            break;
        case 0xd2:
        case 0xd3:
        case 0xd4:
        case 0xd5:
        case 0xd6:
            accent = c - 0xd2 + 1;
            c = 'O';
            break;
        case 0xd9:
        case 0xda:
        case 0xdb:
        case 0xdc:
            accent = c == 0xdc ? 5 : c - 0xd9 + 1;
            c = 'U';
            break;
        case 0xd1:
            accent = 4;
            c = 'N';
            break;
        case 0xc7:
            accent = 6;
            c = 'C';
            break;
        case 0xdd:
            accent = 2;
            c = 'Y';
            break;
        case 0xa1: c = '!'; break;
        case 0xbf: c = '?'; break;
        default: break;
        }
        if (accent) {
            auto dot = [&](int col, int row) { fill(Rect{x + col * size, y + row * size, size, size}); };
            if (accent == 1) {
                dot(1, -2);
                dot(2, -1);
            } else if (accent == 2) {
                dot(3, -2);
                dot(2, -1);
            } else if (accent == 3) {
                dot(2, -2);
                dot(1, -1);
                dot(3, -1);
            } else if (accent == 4) {
                dot(1, -2);
                dot(2, -2);
                dot(2, -1);
                dot(3, -1);
            } else if (accent == 5) {
                dot(1, -2);
                dot(3, -2);
            } else if (accent == 6) {
                dot(2, 7);
                dot(1, 8);
            } else if (accent == 7) {
                dot(3, 7);
                dot(4, 8);
            } else if (accent == 8) {
                dot(2, -2);
            } else if (accent == 9) {
                dot(1, -2);
                dot(2, -1);
                dot(3, -2);
            } else if (accent == 10) {
                dot(1, 3);
                dot(2, 2);
            }
        }
        if (vietnamese.base) {
            auto dot = [&](int col, int row) { fill(Rect{x + col * size, y + row * size, size, size}); };
            if (vietnamese.shape == 1) {
                dot(2, -2);
                dot(1, -1);
                dot(3, -1);
            } else if (vietnamese.shape == 2) {
                dot(1, -2);
                dot(2, -1);
                dot(3, -2);
            } else if (vietnamese.shape == 3) {
                dot(4, -1);
                dot(5, -2);
            } else if (vietnamese.shape == 4) {
                for (int col = 0; col < 4; col++) dot(col, 3);
            }
            int lift = vietnamese.shape && vietnamese.shape != 4 ? -2 : 0;
            if (vietnamese.tone == 1) {
                dot(1, -2 + lift);
                dot(2, -1 + lift);
            } else if (vietnamese.tone == 2) {
                dot(3, -2 + lift);
                dot(2, -1 + lift);
            } else if (vietnamese.tone == 3) {
                dot(2, -3 + lift);
                dot(3, -2 + lift);
                dot(2, -1 + lift);
            } else if (vietnamese.tone == 4) {
                dot(1, -2 + lift);
                dot(2, -2 + lift);
                dot(2, -1 + lift);
                dot(3, -1 + lift);
            } else if (vietnamese.tone == 5) dot(2, 8);
        }
        if (c == 0xe6) c = 0xc6;
        else if (c == 0xf8) c = 0xd8;
        else if (c == 0xe5) c = 0xc5;
        else if (c == 0xe9) c = 'E';
        else if (c == 0x2019) c = '\'';
        else if (c == 0x2026) c = '.';
        else if (c == 0x2013 || c == 0x2014) c = '-';
        else if (c == '"' || c == 0xab || c == 0xbb || (c >= 0x201c && c <= 0x201e)) c = '\'';
        const auto* bitmap = cyrillic_pixel_bitmap(c);
        if (!bitmap) {
            char ch = c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : static_cast<char>(c);
            auto it = glyphs.find(ch);
            if (it == glyphs.end()) it = glyphs.find('?');
            bitmap = &it->second;
        }
        for (int row = 0; row < 7; row++)
            for (int col = 0; col < 5; col++) {
                if ((*bitmap)[row] & (1 << (4 - col))) {
                    fill(Rect{x + col * size, y + row * size, size, size});
                }
            }
        x += size * 6;
    }
}
}
