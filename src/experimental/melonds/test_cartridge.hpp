#pragma once
#include <cstring>
#include <vector>
#include "types.h"
using namespace melonDS;
static void word(std::vector<u8>& bytes, size_t offset, u32 value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}
static std::vector<u8> cartridge() {
    std::vector<u8> rom(0x2000);
    std::memcpy(rom.data(), "RGDS PROBE  ####00", 18);
    // ARM9 repeatedly increments RAM and samples the emulated key register.
    const u32 arm9[] = {0xe59f0018, 0xe3a01000, 0xe2811001, 0xe5801000, 0xe59f200c,
                        0xe1d230b0, 0xe5803004, 0xeafffff9, 0x02001000, 0x04000130};
    std::memcpy(rom.data() + 0x200, arm9, sizeof(arm9));
    word(rom, 0x20, 0x200);
    word(rom, 0x24, 0x02000000);
    word(rom, 0x28, 0x02000000);
    word(rom, 0x2c, sizeof(arm9));
    word(rom, 0x30, 0x1000);
    word(rom, 0x34, 0x03800000);
    word(rom, 0x38, 0x03800000);
    word(rom, 0x3c, 4);
    word(rom, 0x1000, 0xeafffffe); // ARM7: idle branch.
    word(rom, 0x80, rom.size());
    word(rom, 0x84, 0x200);
    return rom;
}
