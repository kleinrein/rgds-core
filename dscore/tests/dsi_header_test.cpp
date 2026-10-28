// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// DSiWare by header (io/dsi_title_install): a DSi-only title (unit code 3) is DSiWare and does not run on a DS;
// BlocksDS homebrew carries DSiWare's title ID high with unit code 2 and still runs on a DS, which the frontend uses
// to start it in DS mode when there is no DSi BIOS pair.
#include "check.h"
#include "core/io/dsi_title_install.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

static std::string rom(const std::string& name, uint8_t unit, uint32_t title_high) {
  std::vector<char> head(0x1000, 0);
  head[0x12] = static_cast<char>(unit);
  for (int i = 0; i < 4; i++) head[0x234 + i] = static_cast<char>((title_high >> (8 * i)) & 0xff);
  const auto path = std::filesystem::temp_directory_path() / name;
  std::ofstream(path, std::ios::binary).write(head.data(), static_cast<std::streamsize>(head.size()));
  return path.string();
}

int main() {
  const std::string blocksds = rom("dsi_header_blocksds.nds", 2, 0x00030004);
  const std::string dsiware = rom("dsi_header_dsiware.nds", 3, 0x00030004);
  const std::string enhanced = rom("dsi_header_enhanced.nds", 2, 0x00030000);
  const std::string plain = rom("dsi_header_plain.nds", 0, 0);
  CHECK(ds::io::file_is_dsiware(blocksds) && ds::io::file_runs_on_ds(blocksds));
  CHECK(ds::io::file_is_dsiware(dsiware) && !ds::io::file_runs_on_ds(dsiware));
  CHECK(!ds::io::file_is_dsiware(enhanced) && ds::io::file_runs_on_ds(enhanced));
  CHECK(!ds::io::file_is_dsiware(plain) && ds::io::file_runs_on_ds(plain));
  CHECK(!ds::io::file_runs_on_ds("missing.gba"));
  for (const auto& path : {blocksds, dsiware, enhanced, plain}) std::filesystem::remove(path);
  std::puts("dsi_header: ok");
  return 0;
}
