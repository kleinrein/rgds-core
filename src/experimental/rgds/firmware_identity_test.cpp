// SPDX-License-Identifier: GPL-3.0-or-later
// Firmware identity encoding and field-boundary regression.
#include "core/bios/freebios.h"
#include <cassert>
#include <cstring>
#include <cstdio>
int main() {
    using namespace ds::bios;
    UserSettings u;
    assert(u.nickname == "RG DS Core");
    u.nickname = "Åse🎮";
    auto fw = generate_firmware(u);
    auto p = fw.data() + fw.size() - 0x200;
    const unsigned char expected[] = {0xC5, 0, 0x73, 0, 0x65, 0, 0x3C, 0xD8, 0xAE, 0xDF};
    assert(!std::memcmp(p + 6, expected, sizeof expected));
    assert(p[0x1A] == 5);
    assert(user_text(p + 6, 5) == u.nickname);
    assert(user_text("123456789🎮", 10) == u"123456789");
    assert(user_text("12345678🎮x", 10) == u"12345678🎮");
    assert(user_text(std::string("\xFF"), 10).empty());
    assert(crc16(p, 0x70, 0xFFFF) == (p[0x72] | (p[0x73] << 8)));
    puts("DS nickname: UTF-8/UTF-16, default, boundary and firmware checksum passed");
}
