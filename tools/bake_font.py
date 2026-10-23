#!/usr/bin/env python3
"""Bake bundled fonts into portable alpha atlases. Only regeneration needs Pillow 13+ and fontTools."""
from pathlib import Path
import struct
from fontTools.ttLib import TTFont
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
DEST = ROOT / "assets/fonts"
SIZES = (16, 20, 24, 28, 36)
# Vietnamese uses precomposed vowels with both shape and tone marks.
VIETNAMESE = [0x102, 0x103, 0x110, 0x111, 0x128, 0x129, 0x168, 0x169, 0x1A0, 0x1A1, 0x1AF, 0x1B0
              ] + list(range(0x1EA0, 0x1EFA))
# Polish and Turkish letters outside Latin-1: Ą Ć Ę Ł Ń Ś Ź Ż, Ğ İ ı Ş.
LATIN_EXTENDED = [0x104, 0x105, 0x106, 0x107, 0x118, 0x119, 0x11E, 0x11F, 0x130, 0x131, 0x141, 0x142, 0x143, 0x144,
                  0x15A, 0x15B, 0x15E, 0x15F, 0x179, 0x17A, 0x17B, 0x17C]
# Russian and Ukrainian: the basic Cyrillic block plus Ґ ґ.
CYRILLIC = list(range(0x400, 0x460)) + [0x490, 0x491]
CODEPOINTS = (list(range(32, 127)) + list(range(160, 256)) + [0x2013, 0x2014, 0x2019, 0x201C, 0x201D, 0x201E, 0x2026]
              + VIETNAMESE + LATIN_EXTENDED + CYRILLIC)


def national(encoding, first_row, last_row):
    """Every character of a 94×94 national set's rows first_row..last_row, via its EUC encoding."""
    found = []
    for row in range(first_row, last_row + 1):
        for cell in range(1, 95):
            try:
                found.append(ord(bytes([0xA0 + row, 0xA0 + cell]).decode(encoding)))
            except UnicodeDecodeError:
                pass
    return found


# The launcher looks in a CJK family only for codepoints from U+2E80 up that the
# Latin atlas lacks, so a family holds nothing below that. Each family has its
# script's symbols and full-width forms, the national set's everyday characters
# and every character its catalog uses. Japanese: kana, halfwidth katakana and the
# 2,965 JIS X 0208 level-1 kanji. Simplified Chinese: the 3,755 GB 2312 level-1
# hanzi. Korean: the 2,350 KS X 1001 hangul syllables and compatibility jamo.
CJK_SYMBOLS = list(range(0x3000, 0x3040)) + list(range(0xFF01, 0xFF5F))
JAPANESE = CJK_SYMBOLS + list(range(0x3041, 0x3100)) + list(range(0xFF5F, 0xFFA0)) + national(
    "euc_jp", 1, 8) + national("euc_jp", 16, 47)
CHINESE = CJK_SYMBOLS + national("gb2312", 1, 9) + national("gb2312", 16, 55)
KOREAN = CJK_SYMBOLS + list(range(0x3131, 0x318F)) + national("euc_kr", 16, 40)
# Source fonts live in tools/fonts, outside the shipped assets; the Noto files
# are weight-500 instances subset to their national sets (see PROVENANCE.txt).
FAMILIES = {
    "ja": ("MPLUSRounded1c-Medium.ttf", JAPANESE, "ja-JP"),
    "zh": ("NotoSansSC-Medium-subset.ttf", CHINESE, "zh-CN"),
    "ko": ("NotoSansKR-Medium-subset.ttf", KOREAN, "ko-KR"),
}
SOURCES = ROOT / "tools/fonts"


def bake(size, source, codepoints, output, width, weight=None):
    font = ImageFont.truetype(str(source), size)
    if weight:
        font.set_variation_by_axes([weight])
    ascent, _ = font.getmetrics()
    records, x, y, row = [], 2, 2, 0
    for cp in codepoints:
        left, top, right, bottom = font.getbbox(chr(cp), anchor="ls")
        glyph_width, glyph_height = right - left, bottom - top
        if x + glyph_width + 2 > width:
            x, y, row = 2, y + row + 2, 0
        records.append((cp, x, y, glyph_width, glyph_height, left, top, round(font.getlength(chr(cp)) * 64)))
        x += glyph_width + 2
        row = max(row, glyph_height)
    height = y + row + 2
    image = Image.new("L", (width, height))
    draw = ImageDraw.Draw(image)
    for cp, x, y, w, h, left, top, advance in records:
        draw.text((x - left, y - top), chr(cp), font=font, fill=255, anchor="ls")
    output = DEST / output
    with output.open("wb") as stream:
        stream.write(struct.pack("<4s4I", b"RGF1", width, height, ascent, len(records)))
        for record in records:
            stream.write(struct.pack("<I7i", *record))
        stream.write(image.tobytes())
    print(output.relative_to(ROOT), len(records), output.stat().st_size)


def family_codepoints(source, base, language):
    covered = TTFont(str(source)).getBestCmap()
    catalog = ROOT / "assets/locales" / (language + ".tsv")
    wanted = set(base) | ({ord(c) for c in catalog.read_text(encoding="utf-8")} if catalog.exists() else set())
    return sorted(cp for cp in wanted if cp >= 0x2E80 and cp in covered)


if __name__ == "__main__":
    for size in SIZES:
        bake(size, DEST / "source/Nunito.ttf", CODEPOINTS, "rounded-%d.rgf" % size, 512, 600)
    for family, (source, base, language) in FAMILIES.items():
        codepoints = family_codepoints(SOURCES / source, base, language)
        for size in SIZES:
            bake(size, SOURCES / source, codepoints, "rounded-%s-%d.rgf" % (family, size), 1024)
