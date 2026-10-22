#pragma once
#include <array>

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
}
