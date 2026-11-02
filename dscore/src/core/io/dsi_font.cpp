// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// DSperate's own DSi system fonts, linked in whole.
#include "core/io/dsi_nand_synth.h"

#include <cstring>

// The directory comes from CMake (DSPERATE_DSI_FONT_DIR), absolute.
#if defined(__APPLE__)
// Mach-O (the macOS host build of the headless frontend): C symbols carry a leading underscore, read-only data
// lives in __TEXT,__const, and there is no .previous.
#define DSPERATE_FONT_PREFIX "_"
#define DSPERATE_FONT_SECTION "\t.section __TEXT,__const\n"
#define DSPERATE_FONT_SECTION_END ""
#else
#define DSPERATE_FONT_PREFIX ""
#define DSPERATE_FONT_SECTION "\t.section .rodata.dsperate_dsi_font,\"a\",%progbits\n"
#define DSPERATE_FONT_SECTION_END "\t.previous\n"
#endif
#define DSPERATE_FONT_BLOB(sym, file)                                                                                  \
  "\t.balign 4\n"                                                                                                      \
  "\t.globl " DSPERATE_FONT_PREFIX sym "_begin\n" DSPERATE_FONT_PREFIX sym "_begin:\n"                                 \
  "\t.incbin \"" DSPERATE_DSI_FONT_DIR "/" file "\"\n"                                                                 \
  "\t.globl " DSPERATE_FONT_PREFIX sym "_end\n" DSPERATE_FONT_PREFIX sym "_end:\n"
__asm__(DSPERATE_FONT_SECTION DSPERATE_FONT_BLOB("dsperate_dsi_font", "TWLFontTable.dat")
            DSPERATE_FONT_BLOB("dsperate_dsi_font_cn", "TWLFontTable-cn.dat")
                DSPERATE_FONT_BLOB("dsperate_dsi_font_kr", "TWLFontTable-kr.dat") DSPERATE_FONT_SECTION_END);
extern "C" const ds::u8 dsperate_dsi_font_begin[], dsperate_dsi_font_end[];
extern "C" const ds::u8 dsperate_dsi_font_cn_begin[], dsperate_dsi_font_cn_end[];
extern "C" const ds::u8 dsperate_dsi_font_kr_begin[], dsperate_dsi_font_kr_end[];

namespace ds::io {

std::vector<u8> builtin_dsi_font(u8 region) {
  if (region == 4) return std::vector<u8>(dsperate_dsi_font_cn_begin, dsperate_dsi_font_cn_end);
  if (region == 5) return std::vector<u8>(dsperate_dsi_font_kr_begin, dsperate_dsi_font_kr_end);
  return std::vector<u8>(dsperate_dsi_font_begin, dsperate_dsi_font_end);
}

bool is_builtin_font_signature(const u8 sig[0x80]) {
  // The three tables carry the same marker.
  return dsperate_dsi_font_end - dsperate_dsi_font_begin >= 0xA0 &&
         std::memcmp(sig, dsperate_dsi_font_begin, 0x80) == 0;
}

int font_table_region(const std::vector<u8>& table) {
  if (table.size() < 0xA0) return -1;
  if (table[0x84] == 3 && table[0x86] == 0) return 0;
  if (table[0x84] == 9 && (table[0x86] == 4 || table[0x86] == 5)) return table[0x86];
  return -1;
}

} // namespace ds::io
