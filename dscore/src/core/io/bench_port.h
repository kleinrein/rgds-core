// SPDX-License-Identifier: GPL-3.0-or-later
// RG DS: the port the Impeccable benchmark ROM (bench/ds) talks to the frontend through. A DS program only sees
// emulated time, so it cannot tell a smooth run from a slow one; the frontend's clock and page flips answer here.
// Off (reads as open bus) unless the frontend sets a handler, which it does for that one cart.
//
// ARM9, 32-bit accesses at 0x04FFFB00:
//   +00  R  magic 'RGBP'   W  run command, the result lands in +14
//   +04..+10  RW  arguments
//   +14  R  result
//   +18  RW index into the buffer
//   +1C  RW buffer word at the index, which then advances
#pragma once
#include "core/types.h"

#include <array>
#include <functional>

namespace ds::io {

struct BenchPort {
  static constexpr u32 BASE = 0x04FFFB00, END = BASE + 0x20, MAGIC = 0x50424752;
  static constexpr u32 BUFFER_WORDS = 1024;

  std::function<u32(u32 command, const u32* args, std::array<u32, BUFFER_WORDS>& buffer)> handler;
  std::array<u32, 4> args{};
  std::array<u32, BUFFER_WORDS> buffer{};
  u32 result = 0, index = 0;

  static bool owns(u32 addr) { return addr >= BASE && addr < END; }

  u32 read(u32 addr) {
    if (!handler) return 0;
    switch (addr - BASE) {
    case 0x00: return MAGIC;
    case 0x04:
    case 0x08:
    case 0x0C:
    case 0x10: return args[(addr - BASE - 4) / 4];
    case 0x14: return result;
    case 0x18: return index;
    case 0x1C: {
      const u32 value = buffer[index % BUFFER_WORDS];
      index = (index + 1) % BUFFER_WORDS;
      return value;
    }
    default: return 0;
    }
  }

  void write(u32 addr, u32 value) {
    if (!handler) return;
    switch (addr - BASE) {
    case 0x00: result = handler(value, args.data(), buffer); return;
    case 0x04:
    case 0x08:
    case 0x0C:
    case 0x10: args[(addr - BASE - 4) / 4] = value; return;
    case 0x18: index = value % BUFFER_WORDS; return;
    case 0x1C:
      buffer[index % BUFFER_WORDS] = value;
      index = (index + 1) % BUFFER_WORDS;
      return;
    default: return;
    }
  }
};

} // namespace ds::io
