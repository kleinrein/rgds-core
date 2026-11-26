// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The analog controls database, linked in whole. The data is shocoman's CPAD
// codes, not under this file's licence: see analog_db/README.md.
#include "core/cheat/database.h"

// The directory comes from CMake (DSPERATE_ANALOG_DB_DIR), absolute. Mach-O (Mac host builds) has no
// .section/.previous pair and prefixes C symbols with an underscore.
#ifdef __APPLE__
__asm__("\t.const_data\n"
        "\t.balign 4\n"
        "\t.globl _dsperate_analog_db_begin\n_dsperate_analog_db_begin:\n"
        "\t.incbin \"" DSPERATE_ANALOG_DB_DIR "/usrcheat-analog.dat\"\n"
        "\t.globl _dsperate_analog_db_end\n_dsperate_analog_db_end:\n"
        "\t.text\n");
#else
__asm__("\t.section .rodata.dsperate_analog_db,\"a\",%progbits\n"
        "\t.balign 4\n"
        "\t.globl dsperate_analog_db_begin\ndsperate_analog_db_begin:\n"
        "\t.incbin \"" DSPERATE_ANALOG_DB_DIR "/usrcheat-analog.dat\"\n"
        "\t.globl dsperate_analog_db_end\ndsperate_analog_db_end:\n"
        "\t.previous\n");
#endif
extern "C" const ds::u8 dsperate_analog_db_begin[], dsperate_analog_db_end[];

namespace ds::cheat {

std::vector<u8> builtin_analog_database() { return std::vector<u8>(dsperate_analog_db_begin, dsperate_analog_db_end); }

} // namespace ds::cheat
