#pragma once
#include <array>

namespace rgds {
// Classic 5x7 text uses an uppercase base plus separate Vietnamese shape/tone
// marks. Shapes: circumflex, breve, horn, D stroke. Tones: grave, acute,
// hook above, tilde, dot below. Precomposed UTF-8 does not lose either mark.
struct VietnamesePixelGlyph {unsigned base=0;int shape=0,tone=0;};
inline VietnamesePixelGlyph vietnamese_pixel_glyph(unsigned cp){
    static constexpr std::array<VietnamesePixelGlyph,45> vowels={{
        {'A',0,5}, // Ạ ạ
        {'A',0,3}, // Ả ả
        {'A',1,2}, // Ấ ấ
        {'A',1,1}, // Ầ ầ
        {'A',1,3}, // Ẩ ẩ
        {'A',1,4}, // Ẫ ẫ
        {'A',1,5}, // Ậ ậ
        {'A',2,2}, // Ắ ắ
        {'A',2,1}, // Ằ ằ
        {'A',2,3}, // Ẳ ẳ
        {'A',2,4}, // Ẵ ẵ
        {'A',2,5}, // Ặ ặ
        {'E',0,5}, // Ẹ ẹ
        {'E',0,3}, // Ẻ ẻ
        {'E',0,4}, // Ẽ ẽ
        {'E',1,2}, // Ế ế
        {'E',1,1}, // Ề ề
        {'E',1,3}, // Ể ể
        {'E',1,4}, // Ễ ễ
        {'E',1,5}, // Ệ ệ
        {'I',0,3}, // Ỉ ỉ
        {'I',0,5}, // Ị ị
        {'O',0,5}, // Ọ ọ
        {'O',0,3}, // Ỏ ỏ
        {'O',1,2}, // Ố ố
        {'O',1,1}, // Ồ ồ
        {'O',1,3}, // Ổ ổ
        {'O',1,4}, // Ỗ ỗ
        {'O',1,5}, // Ộ ộ
        {'O',3,2}, // Ớ ớ
        {'O',3,1}, // Ờ ờ
        {'O',3,3}, // Ở ở
        {'O',3,4}, // Ỡ ỡ
        {'O',3,5}, // Ợ ợ
        {'U',0,5}, // Ụ ụ
        {'U',0,3}, // Ủ ủ
        {'U',3,2}, // Ứ ứ
        {'U',3,1}, // Ừ ừ
        {'U',3,3}, // Ử ử
        {'U',3,4}, // Ữ ữ
        {'U',3,5}, // Ự ự
        {'Y',0,1}, // Ỳ ỳ
        {'Y',0,5}, // Ỵ ỵ
        {'Y',0,3}, // Ỷ ỷ
        {'Y',0,4}, // Ỹ ỹ
    }};
    if(cp>=0x1ea0 && cp<=0x1ef9)return vowels[(cp-0x1ea0)/2];
    switch(cp){
    case 0x102:case 0x103:return {'A',2,0};
    case 0x110:case 0x111:return {'D',4,0};
    case 0x128:case 0x129:return {'I',0,4};
    case 0x168:case 0x169:return {'U',0,4};
    case 0x1a0:case 0x1a1:return {'O',3,0};
    case 0x1af:case 0x1b0:return {'U',3,0};
    default:return {};
    }
}
}
