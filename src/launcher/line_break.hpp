#pragma once
#include <algorithm>
#include <string>
#include <vector>

namespace rgds {
// The pieces a wrapped label may break between. Spaces separate words as
// before; Japanese and Chinese, written without spaces, may also break between
// two characters, except before closing punctuation, small kana and the
// long-vowel mark, or after an opening bracket.
struct LinePiece {
    std::string text;
    bool space_before = false;
};
inline bool breaks_without_space(unsigned cp) {
    return (cp >= 0x3000 && cp <= 0x30ff) || (cp >= 0x31f0 && cp <= 0x31ff) || (cp >= 0x3400 && cp <= 0x4dbf) ||
           (cp >= 0x4e00 && cp <= 0x9fff) || (cp >= 0xf900 && cp <= 0xfaff) || (cp >= 0xff00 && cp <= 0xffef);
}
inline bool never_starts_line(unsigned cp) {
    static const std::u32string closing = U")]},.!?:;%…’”、。，．・：；？！ー」』）】〕〉》〗〙〜ゝゞヽヾ"
                                          U"ぁぃぅぇぉっゃゅょゎゕゖァィゥェォッャュョヮヵヶ）］｝";
    return closing.find(static_cast<char32_t>(cp)) != std::u32string::npos;
}
inline bool never_ends_line(unsigned cp) {
    static const std::u32string opening = U"([{‘“「『（【〔〈《〖〘［｛";
    return opening.find(static_cast<char32_t>(cp)) != std::u32string::npos;
}
inline std::vector<LinePiece> line_pieces(const std::string& value) {
    std::vector<LinePiece> pieces;
    bool space = false, fresh = true;
    unsigned previous = 0;
    for (size_t i = 0; i < value.size();) {
        auto lead = static_cast<unsigned char>(value[i]);
        size_t length = lead < 0x80 ? 1 : (lead & 0xe0) == 0xc0 ? 2 : (lead & 0xf0) == 0xe0 ? 3 : (lead & 0xf8) == 0xf0 ? 4 : 1;
        length = std::min(length, value.size() - i);
        unsigned cp = length == 1 ? lead : lead & (0x7f >> length);
        for (size_t n = 1; n < length; n++) cp = (cp << 6) | (static_cast<unsigned char>(value[i + n]) & 0x3f);
        auto bytes = value.substr(i, length);
        i += length;
        if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r') {
            space = !pieces.empty();
            fresh = true;
            continue;
        }
        bool split = fresh || ((breaks_without_space(previous) || breaks_without_space(cp)) &&
                               !never_starts_line(cp) && !never_ends_line(previous));
        if (split) pieces.push_back({bytes, space});
        else pieces.back().text += bytes;
        space = fresh = false;
        previous = cp;
    }
    return pieces;
}
}
