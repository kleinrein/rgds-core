// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// RG DS Core SDL frontend. Emulation stays in the core; RGDS owns the
// library, pause/settings UI, save gallery, loading screens and status HUD.
#include "core/cpu/timing_mode.h"
#include "core/nds.h"
#include "core/cart/rom_source.h"
#include "core/handoff_stats.h"
#include "core/host_cores.h"
#include "core/cart/zip.h"
#include "core/cart/zip_cache.h"
#include "core/profile.h"
#include "core/frame_report.h"
#include "core/input/input_log.h"
#include "core/state/state.h"
#include "core/io/dsi_nand_persist.h"
#include "core/io/dsi_title_install.h"
#include "core/io/dsi_nand_launch.h"
#include "core/cheat/database.h"
#include "core/cart/miniz/miniz_tdef.h"
#if DSPERATE_JIT
#include "core/cpu/jit/jit.h"
#endif
#include "adaptive_res.h"

// RG DS: -1 not yet read (DS_ADAPTIVE / video.adaptive at the first frame), else the live choice.
static std::atomic<int> g_rgds_adaptive{-1};
#include "audio.h"
#include "config.h"
#include "display.h"
#include "input.h"
#include "lid.h"
#include "rgds_frontend_ui.hpp"
#include "cpu_gov.h"
#include "pacer.h"
#include "phase_lock.h"
#include "kms_flips.h"
#include "display_drm.h"
#include "present_thread.h"

#include <dirent.h>
#include <algorithm>
#include "mic_alsa.h"

#include <SDL2/SDL.h>
#include <sched.h>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <deque>
#include <unistd.h>
#include <sys/stat.h>
#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <thread>
#include <atomic>
#include <ctime>
#include <string>
#include <random>
#include <vector>

#include <cstdint>
static std::atomic<uint32_t> rgds_completed_frames{0};
extern "C" uint32_t rgds_emulated_frames() { return rgds_completed_frames.load(std::memory_order_relaxed); }

// Nearest tag and short commit hash.
extern const char* const kDsperateVersion;
extern const char* const kDsperateCommit;

namespace {

// Gates startup chatter and hotkey echoes; errors, refusals and disk-touching
// confirmations are never gated. DS_VERBOSE=1 enables.
bool verbose() {
  static const bool v = std::getenv("DS_VERBOSE") != nullptr;
  return v;
}
#define VLOG(...)                                                                                                      \
  do {                                                                                                                 \
    if (verbose()) std::fprintf(stderr, __VA_ARGS__);                                                                  \
  } while (0)

using namespace ds;

const char* kUsage =
    "usage: rgds-experimental [rom.nds|rom.zip] [--bios9 F --bios7 F --firmware F] [options]\n"
    "  With no ROM (or a file named BootMenu.nds) the console boots its own\n"
    "  firmware: the DS menu, with the clock set from this machine and PictoChat.\n"
    "  A file named BootMenuDSi.nds is --dsi-mode: the DSi Menu off the NAND.\n"
    "  Neither file has to exist -- the name is the whole instruction.\n"
    "  Without dumps a built-in replacement BIOS and a generated firmware run games\n"
    "  (direct boot only; the DS menu and exact timing need the real files).\n"
    "  --config F      settings file (default ~/.config/dsperate/dsperate.ini; every\n"
    "                  option below has a key there; games/<rom name>.ini and games/<CODE>.ini\n"
    "                  override it per game, the filename one winning)\n"
    "  --version       print the release tag and the commit this was built from, and exit\n"
    "  --write-config F  write the default settings file (all keys commented) to F and exit\n"
    "  --dsi-mode      boot a DSi from its NAND (boot2, then the DSi Launcher) instead of the DS menu; no\n"
    "                  ROM. Needs --bios9i F --bios7i F (the DSi BIOS pair) and --dsi-nand F (a nand.bin\n"
    "                  with its nocash footer; paths.dsi_nand), with --bios9/--bios7 as usual and the DSi's\n"
    "                  firmware (--firmware, or paths.dsi_firmware for every DSi session).\n"
    "                  EXPERIMENTAL. The NAND is opened\n"
    "                  read-only; what a session changes is kept as files instead: title saves as\n"
    "                  <GAMECODE>.pub/.prv/.bnr (paths.saves, else beside the dump), system settings in\n"
    "                  <nand>.ovr and photos under <nand>.photos/, put back in at the next boot.\n"
    "                  A DSiWare .nds/.cia given with it is installed into the session (not the dump) unless\n"
    "                  the NAND has it; it needs its signed DSi TMD: embedded in the CIA, cached beside the\n"
    "                  saves, downloaded from Nintendo's update CDN, or --dsi-tmd F (default <game>.tmd).\n"
    "                  --dsi-offline never downloads; --dsi-hide-installed (emu.dsi_hide_installed) hides\n"
    "                  the dump's own DSiWare.\n"
    "                  The title starts straight away; --dsi-menu boots to the DSi Menu with it instead\n"
    "                  Without --dsi-nand, a DSiWare .nds/.cia given with it runs with only the DSi BIOS\n"
    "                  pair: the launcher's hand-off is emulated, the NAND and the DSi settings are made\n"
    "                  up from [user], and saves go to paths.saves (else beside the game). The DSi system\n"
    "                  font is RG DS Core's own (Noto Sans, WenQuanYi Micro Hei); --dsi-font F\n"
    "                  (paths.dsi_font) uses a console's /sys/TWLFontTable.dat instead.\n"
    "                  --dsi-sd DIR (paths.dsi_sd) puts a host folder in the DSi's SD card slot; what the\n"
    "                  DSi changes on the card is written back to the folder (new, changed and removed files)\n"
    "                  A DSiWare .nds/.dsi/.cia named without --dsi-mode runs this way too; the DSi BIOS pair\n"
    "                  can be set as\n"
    "                  paths.bios9i/paths.bios7i. When the title leaves (a soft reset), the session ends.\n"
    "  --scale N       window scale (default 2)\n"
    "  --fullscreen    start fullscreen\n"
    "  --layout L      vertical (default) | horizontal | single | pip | dominant_v | dominant_h\n"
    "  --screen S      top (default) or bottom: the screen shown alone, large or dominant\n"
    "  --screen-gap N  pixels between the two screens in the stacked, side-by-side and dominant layouts\n"
    "                  (video.screen_gap; any whole number, a negative one overlaps them; auto pushes\n"
    "                  the screens to the edges when the panel leaves room along the pair)\n"
    "  --pip-alpha X   opacity of the PiP inset at rest, 0..1 (default 1; it comes up to opaque\n"
    "                  while the bottom screen is touched)\n"
    "  --dominant-ratio R  the dominant layouts' secondary, relative to the dominant screen: auto\n"
    "                  (default: the dominant screen takes the largest whole scale that leaves the\n"
    "                  secondary at least --dominant-threshold of it, and the secondary the room left)\n"
    "                  or a ratio such as 0.5\n"
    "  --dominant-threshold T  the smallest secondary auto accepts, 0.1..0.99 (default 0.25)\n"
    "  --integer-scale [M]  whole panel pixels per DS pixel: under (default when bare; the largest\n"
    "                  that fits, letterboxed) | over (the smallest that covers, cropped, keeping the\n"
    "                  edge between the screens) | off\n"
    "  --dual-window   one window per video display, one DS screen each (dual-panel\n"
    "                  handhelds; also what direct scanout needs on them)\n"
    "  --linear        bilinear scaling instead of nearest (takes precedence over the grid, seams\n"
    "                  and chunky)\n"
    "  --lcd-grid S    LCD pixel grid strength, 0 (off, default) .. 1 (software scaling, or the\n"
    "                  display engine's overlay layer on the A30)\n"
    "  --seam S        dark (default): the LCD grid, dimmed by --lcd-grid | blend: box-filter seams\n"
    "                  (sharp-shimmerless): the one panel pixel/row that straddles two DS pixels is their\n"
    "                  area-weighted blend, all others crisp | blend_linear: the same in linear light\n"
    "  --chunky [M]    draw each 2x2 block of DS pixels as one cell (with the grid, one seam per\n"
    "                  block); lower resolution, for panels at fractional scales. M: mean (default;\n"
    "                  the area-weighted box) | extreme (the mean, unless the darkest or brightest\n"
    "                  pixel stands more than --chunky-threshold N (0..255, default 180) from it) |\n"
    "                  mode (dominant colour, mean when all differ) | tl (top-left pixel) | min | max\n"
    "  --chunky-cell C panel pixels per chunky cell: auto (default; the smallest of 4..16 that divides the\n"
    "                  screen, 4 on a 640x480 panel = 160x120 cells) | pair (2x2 DS pixels) | N (the\n"
    "                  nearest size at or below N that divides the screen)\n"
    "  --sink S        where frames go: auto (the default: the best this device has) | disp (the\n"
    "                  display engine's scaler layer, Miyoo A30 class) | fbdev (/dev/fb0, the H700\n"
    "                  handhelds' mali-fbdev SDL2) | kms (page flips) | dmabuf (Wayland zero-copy) |\n"
    "                  surface (SDL's window surface) | renderer (SDL_Renderer). One that won't open\n"
    "                  falls back to the renderer. video.sink. --disp / --fbdev are --sink disp / fbdev\n"
    "  --gpu-present / --no-gpu-present  scale and lay out the picture off the CPU, into the\n"
    "                  scanout buffer: the Rockchip RGA where the device has one, else a Vulkan\n"
    "                  compute pass (video.gpu_present = true | rga | vulkan | false; on by default)\n"
    "  --preload-rom / --no-preload-rom  read the whole ROM into memory at load, or page it in as\n"
    "                  the game reads it (cart.preload = auto | true | false; auto preloads a ROM on a\n"
    "                  network share that fits in memory, where a page read mid-frame is a round trip)\n"
    "  --no-audio      run without sound\n"
    "  --volume N      0..100\n"
    "  --audio-buffer X  how much sound is held ahead: auto (the default) or milliseconds;\n"
    "                  audio.buffer_size. Lower is less delay and less slack before a\n"
    "                  late frame is heard as a gap\n"
    "  --no-mic        do not open the microphone (M still fakes one)\n"
    "  --no-vsync      present without waiting for the display refresh\n"
    "  --interp        interpreter instead of the recompiler\n"
    "  --timing M      CPU cycle model: fast (default: constant memory and jump costs, much cheaper)\n"
    "                  | exact (melonDS's per-access model); emu.timing\n"
    "  --aa / --no-aa  3D anti-aliasing (video.aa, off by default): the hardware's edge blend on the\n"
    "                  CPU raster, 4x MSAA on the GPU raster\n"
    "  --gpu3d / --no-gpu3d  the 3D layer drawn on the GPU (video.gpu3d, off by default); the CPU\n"
    "                  raster where there is no Vulkan\n"
    "  --frameskip N   skip drawing up to N frames in N+1 (0 = off); emu.frameskip. Skipping runs\n"
    "                  in whole display periods, so on a game that drives its screens on\n"
    "                  alternate frames the limit counts pairs (DS_DEBUG_SKIP=1 shows the period)\n"
    "  --frameskip-mode M  adaptive (default; skip only while the emulator is behind, up to N)\n"
    "                  | fixed (always skip N of every N+1)\n"
    "  --no-frameskip-capture  do not skip frames that display-capture. Exact, but a game that\n"
    "                  captures every frame -- Pokemon B/W, Golden Sun -- then skips nothing,\n"
    "                  so frameskip does nothing at all on it. On by default (the captured\n"
    "                  VRAM holds the last drawn frame); emu.frameskip_capture\n"
    "  --limiter HZ    the rate the emulator is held to: auto (the console's own 59.8261 Hz, the\n"
    "                  default and the speed a game was written for), 30, 60 (what a player means\n"
    "                  by 60 fps: 0.29 % fast, and in step with a 60 Hz panel), 120, 144, 240, or\n"
    "                  off. emu.limiter\n"
    "  --speed N       run at N percent of that rate (25..400); emu.speed\n"
    "  --pacing M      how the wait for the next frame is spent: auto (default) | sleep | busy.\n"
    "                  A frequency governor that decides by polling how busy the last few\n"
    "                  milliseconds looked reads that sleep as an idle machine and clocks down\n"
    "                  under the emulator (ondemand on ROCKNIX: 4-6x the missed frames and ~20x\n"
    "                  the audio gaps). busy holds the core to the deadline instead, at the price\n"
    "                  of a core that never idles; auto does that only where such a governor is in\n"
    "                  charge. Nothing here changes a system setting. emu.pacing\n"
    "  --frames N      quit after N frames (for repeatable measurements)\n"
    "  --stats-from N  leave the first N frames out of the frame statistics (DS_FRAME_STATS)\n"
    "  --record F      write the played inputs to F (one record per frame)\n"
    "  --replay F      play the inputs in F instead of the controls; quits at its end\n"
    "  --rtc-host      run the clock from this machine even under --replay (INEXACT: a game\n"
    "                  that reads the date no longer replays the same, but the firmware's own\n"
    "                  menu needs a real clock to appear at all)\n"
    "  --netplay       local wireless with whoever is on the LAN: join a session heard within\n"
    "                  2.5 s, else host one (melonDS's LAN protocol; a melonDS can be the other end).\n"
    "                  The menu's NETWORK FEATURES row and net.mode do the same without a flag.\n"
    "  --lan-host NAME host a local-wireless session as NAME; --lan-join ADDR joins the one at ADDR;\n"
    "                  --lan-name NAME is our player name (default: the console's nickname)\n"
    "  --internet      the game reaches the real network through the emulated access point, over a\n"
    "                  user-mode TCP/IP stack (no privileges, works on wlan0). Not with local wireless\n"
    "  --dns WHERE     where the game looks up its servers: wiimmfi (the default: Nintendo's own\n"
    "                  servers were switched off in 2014), host (this machine's resolver), or an address\n"
    "  --load-state F  start from a save state instead of booting the game\n"
    "  --autosave-png F  with emu.autosave, write a PNG of both screens to F beside the auto state\n"
    "  --autoload      start from the auto slot (emu.autosave's state) when there is one\n"
    "  --no-autoload   ignore it for this run, whatever emu.autoload says\n"
    "  --save F        battery save to start from, instead of <rom>.sav\n"
    "                  (a --replay never writes the save back, so a scene repeats)\n"
    "  --clear-cache   delete every unpacked zipped game (the .dsperate directories beside the\n"
    "                  games and paths.cache) except the one being launched, then run as usual\n";

std::string rom_dir_of(const std::string& rom) {
  const size_t slash = rom.find_last_of('/');
  return slash == std::string::npos ? "." : rom.substr(0, slash);
}

std::string rom_stem(const std::string& rom) {
  const size_t dot = rom.find_last_of('.');
  const size_t slash = rom.find_last_of('/');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return rom;
  return rom.substr(0, dot);
}
std::string base_name(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}
// Lower-cased extension, "" if none.
std::string rom_ext(const std::string& rom) {
  const std::string stem = rom_stem(rom);
  std::string ext = rom.substr(stem.size());
  for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return ext;
}

// A CIA, loose or inside a zip; read_dsiware unwraps it to the SRL the DSi slot takes.
bool is_cia_container(const std::string& rom) {
  const std::string ext = rom_ext(rom);
  if (ext == ".cia") return true;
  return ext == ".zip" && ds::io::zip_holds_cia(rom);
}

// Four-letter game code from a plain .nds header (offset 0x0C), read before
// cfg setup. Zips aren't peeked; they fall back to filename config, merged by code later.
bool peek_game_code(const std::string& rom, char out[4]) {
  if (rom_ext(rom) != ".nds") return false;
  FILE* f = std::fopen(rom.c_str(), "rb");
  if (!f) return false;
  char code[4] = {};
  const bool ok = std::fseek(f, 0x0C, SEEK_SET) == 0 && std::fread(code, 1, 4, f) == 4;
  std::fclose(f);
  // Must be four printable characters.
  for (int i = 0; ok && i < 4; ++i)
    if (code[i] < 0x20 || code[i] >= 0x7f) return false;
  if (!ok) return false;
  std::memcpy(out, code, 4);
  return true;
}

std::string save_path(const std::string& rom, const std::string& dir) {
  return dir.empty() ? rom_stem(rom) + ".sav" : dir + "/" + base_name(rom_stem(rom)) + ".sav";
}

// video.aa: true / false. The older mode words still read: accurate / enhanced / smooth as on, off as off.
bool aa_on(const std::string& v) {
  return v == "true" || v == "1" || v == "yes" || v == "on" || v == "accurate" || v == "enhanced" || v == "smooth";
}
void apply_aa(NDS& nds, bool on) { nds.gpu3d.renderer().set_aa(on); }

void load_save(NDS& nds, const std::string& path) {
  if (!nds.cart) return;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    if (!nds.cart->save_type_listed())
      VLOG("save: game code not in the save list; the chip is detected on first use\n");
    return;
  }
  std::vector<u8> data;
  u8 buf[65536];
  for (size_t got; (got = std::fread(buf, 1, sizeof buf, f)) > 0;) data.insert(data.end(), buf, buf + got);
  std::fclose(f);
  const ds::cart::Cart::SaveLoad r = nds.cart->load_save(data.data(), data.size());
  if (r.fitted) {
    VLOG("save: loaded %zu bytes from %s\n", data.size(), path.c_str());
    return;
  }
  // First write resizes to the chip's size, which would cut off/pad the
  // player's original: keep it as .bak.
  const std::string bak = path + ".bak";
  if (r.chip_bytes)
    std::fprintf(stderr, "save: %s is %zu bytes but the game's save chip holds %u; the original is kept as %s\n",
                 path.c_str(), data.size(), r.chip_bytes, bak.c_str());
  else
    std::fprintf(stderr, "save: %s is %zu bytes, a size no save chip has; ignored, the original is kept as %s\n",
                 path.c_str(), data.size(), bak.c_str());
  if (FILE* e = std::fopen(bak.c_str(), "rb")) {
    std::fclose(e);
    return;
  } // earlier original wins
  if (FILE* o = std::fopen(bak.c_str(), "wb")) {
    if (std::fwrite(data.data(), 1, data.size(), o) != data.size())
      std::fprintf(stderr, "save: cannot write %s\n", bak.c_str());
    std::fclose(o);
  }
}

void write_save(NDS& nds, const std::string& path) {
  if (!nds.cart || nds.cart->sram().empty()) return;
  // Write-then-rename: a power cut mid-write leaves the previous file.
  const std::string tmp = path + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) {
    std::fprintf(stderr, "save: cannot write %s\n", tmp.c_str());
    return;
  }
  const bool ok = std::fwrite(nds.cart->sram().data(), 1, nds.cart->sram().size(), f) == nds.cart->sram().size();
  std::fclose(f);
  if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) {
    std::fprintf(stderr, "save: cannot write %s\n", path.c_str());
    return;
  }
  nds.cart->clear_sram_dirty();
}

// Both screens into one PNG at the layout's natural size. Reads the
// framebuffers directly, so grid/chunky/seams/scaler layer are excluded.
bool write_png(NDS& nds, const std::string& path, const ds::sdl::Display::Layout& shown) {
  using Disp = ds::sdl::Display;
  int w = 0, h = 0;
  Disp::Layout layout = shown;
  layout.gap = 0;
  layout.gap_auto = false; // screen_gap is a panel distance, not part of the game's picture
  Disp::natural_size(layout, 1.0, w, h);
  Disp::View views[Disp::SCREENS];
  Disp::place(layout, w, h, views);
  std::vector<u8> rgba(static_cast<size_t>(w) * h * 4, 0);
  for (int i = 0; i < Disp::SCREENS; ++i) {
    const Disp::View& v = views[i];
    if (!v.shown || v.rect.w <= 0 || v.rect.h <= 0) continue;
    const u32* fb = nds.gpu.framebuffer(v.screen);
    // Inset at its resting opacity (the touch ramp is live-only).
    const u32 alpha = v.direct ? 255 : static_cast<u32>(std::clamp(layout.pip_alpha, 0.0, 1.0) * 255.0 + 0.5);
    for (int y = 0; y < v.rect.h; ++y) {
      const int dy = v.rect.y + y;
      if (dy < 0 || dy >= h) continue;
      const int sy = static_cast<int>(static_cast<long>(y) * ds::SCREEN_H / v.rect.h);
      u8* dst = rgba.data() + (static_cast<size_t>(dy) * w + v.rect.x) * 4;
      for (int x = 0; x < v.rect.w; ++x) {
        const int dx = v.rect.x + x;
        if (dx < 0 || dx >= w) continue;
        const int sx = static_cast<int>(static_cast<long>(x) * ds::SCREEN_W / v.rect.w);
        u32 c = fb[sy * ds::SCREEN_W + sx]; // ARGB8888
        if (alpha < 255) {
          u32 under =
              (static_cast<u32>(dst[x * 4 + 0]) << 16) | (static_cast<u32>(dst[x * 4 + 1]) << 8) | dst[x * 4 + 2];
          Disp::blend_row(&under, &c, 1, alpha);
          c = under;
        }
        dst[x * 4 + 0] = (c >> 16) & 0xFF;
        dst[x * 4 + 1] = (c >> 8) & 0xFF;
        dst[x * 4 + 2] = c & 0xFF;
        dst[x * 4 + 3] = 0xFF;
      }
    }
  }
  size_t len = 0;
  void* png =
      tdefl_write_image_to_png_file_in_memory_ex(rgba.data(), w, h, 4, &len, 6 /* zlib default level */, MZ_FALSE);
  if (!png) {
    std::fprintf(stderr, "png: cannot encode\n");
    return false;
  }
  const std::string tmp = path + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "wb");
  const bool ok = f && std::fwrite(png, 1, len, f) == len;
  if (f) std::fclose(f);
  std::free(png); // miniz allocates with plain malloc (MZ_MALLOC is not overridden)
  if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) {
    std::fprintf(stderr, "png: cannot write %s\n", path.c_str());
    std::remove(tmp.c_str());
    return false;
  }
  return true;
}

// <GAMECODE>-<timestamp>.png in paths.screenshots (states dir unless set).
void screenshot(NDS& nds, const std::string& dir, const ds::sdl::Display::Layout& layout) {
  ::mkdir(dir.c_str(), 0755);
  char stamp[32];
  const std::time_t now = std::time(nullptr);
  std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", std::localtime(&now));
  std::string code(nds.cart ? nds.cart->header().game_code : "NONE", 4);
  const std::string path = dir + "/" + code + "-" + stamp + ".png";
  if (write_png(nds, path, layout)) std::fprintf(stderr, "screenshot: %s\n", path.c_str());
}

std::string state_path(NDS& nds, const std::string& dir, int slot) {
  const std::string code(nds.cart ? nds.cart->header().game_code : "NONE", 4);
  return dir + "/" + code + "." + std::to_string(slot) + ".dss";
}

// ".auto" rather than a number so it can't collide with a player slot and
// refresh_slots() (walks 0..9) leaves it off the menu; reached only via --load-state.
std::string auto_state_path(NDS& nds, const std::string& dir) {
  const std::string code(nds.cart ? nds.cart->header().game_code : "NONE", 4);
  return dir + "/" + code + ".auto.dss";
}

// Auto slot as the starting point when emu.autoload is set and it exists.
// Left in place: a SIGKILL can't overwrite it, a clean exit does anyway.
std::string autoload_path(NDS& nds, const std::string& dir, bool enabled) {
  // Never for cart-less boot: auto_state_path() keys on "NONE".
  if (!enabled || !nds.cart) return {};
  const std::string path = auto_state_path(nds, dir);
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return {};
  std::fclose(f);
  return path;
}

// Screen layout rides after the machine's chunks. Frontend-only chunk:
// headless never writes it, an older file just ends before it.
void write_layout_chunk(ds::state::Writer& w, const ds::sdl::Display::Layout& l) {
  w.begin("VIEW");
  w.put(static_cast<u8>(l.mode));
  w.put(static_cast<u8>(l.primary));
  w.put(static_cast<u8>(l.corner));
  w.put(l.pip);
  w.put(l.dominant);
  w.end();
}

bool read_layout_chunk(ds::state::Reader& r, ds::sdl::Display::Layout& l) {
  using Disp = ds::sdl::Display;
  if (r.at_end() || !r.begin("VIEW")) return false;
  u8 mode = 0, primary = 0, corner = 0;
  double pip = l.pip, dominant = l.dominant;
  r.fields(mode, primary, corner);
  if (r.more()) r.fields(pip, dominant);
  r.end();
  if (!r.ok() || mode >= static_cast<u8>(Disp::Mode::Count) || primary > 1 ||
      corner >= static_cast<u8>(Disp::Corner::Count))
    return false;
  l.mode = static_cast<Disp::Mode>(mode);
  l.primary = primary;
  l.corner = static_cast<Disp::Corner>(corner);
  l.pip = std::clamp(pip, 0.1, 0.9);
  l.dominant = std::clamp(dominant, 0.1, 0.99);
  return true;
}

// Achievement progress, riding after the layout. Needed since Casual mode
// allows save states, so cheevos runtime state must travel with them.
// File-scope hook (not a parameter) so no save/load call site can skip it.
struct CheevosStateHook {
  virtual ~CheevosStateHook() = default;
  // False: nothing to carry, chunk not written.
  virtual bool save(u32& game_id, std::vector<u8>& blob) = 0;
};
CheevosStateHook* g_cheevos_state = nullptr;

// A state loaded before cheevos sign-in/fetch completes is held here and
// applied once the set arrives; also covers an in-session load mid-fetch.
struct PendingCheevosState {
  bool waiting = false; // state loaded, not yet accounted for
  u32 game_id = 0;
  std::vector<u8> blob; // empty: state carried no progress, so reset
} g_cheevos_pending;

void write_cheevos_chunk(ds::state::Writer& w) {
  if (!g_cheevos_state) return;
  u32 game_id = 0;
  std::vector<u8> blob;
  if (!g_cheevos_state->save(game_id, blob) || blob.empty()) return;
  w.begin("CHVO");
  w.put(game_id);
  w.vec(blob);
  w.end();
}

void read_cheevos_chunk(ds::state::Reader& r) {
  // No hook check: the session doesn't exist yet at startup read time.
  // No chunk found: still reset the runtime, or it keeps counting stale progress.
  if (r.at_end() || !r.begin("CHVO")) {
    g_cheevos_pending = {true, 0, {}};
    return;
  }
  u32 game_id = 0;
  std::vector<u8> blob;
  r.fields(game_id);
  r.vec(blob);
  r.end();
  if (!r.ok() || blob.empty()) {
    g_cheevos_pending = {true, 0, {}};
    return;
  }
  g_cheevos_pending = {true, game_id, std::move(blob)};
}

bool save_state_file(NDS& nds, const std::string& path, const ds::sdl::Display::Layout& layout) {
  ds::state::Writer w;
  std::string err;
  if (!nds.save_state(w, err)) {
    std::fprintf(stderr, "state: cannot save: %s\n", err.c_str());
    return false;
  }
  write_layout_chunk(w, layout);
  write_cheevos_chunk(w);
  const std::string tmp = path + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) {
    std::fprintf(stderr, "state: cannot write %s\n", tmp.c_str());
    return false;
  }
  const bool ok = std::fwrite(w.data().data(), 1, w.data().size(), f) == w.data().size();
  std::fclose(f);
  if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) {
    std::fprintf(stderr, "state: cannot write %s\n", path.c_str());
    return false;
  }
  std::fprintf(stderr, "state: saved %s (%zu KB)\n", path.c_str(), w.data().size() >> 10);
  return true;
}

// Set when a real state was refused (BIOS mismatch, another ROM, older
// format); not for an empty/unreadable slot. Shown on the slot row.
std::string g_state_refused;

// False if unusable, machine untouched; caller must reset if this fails
// after the load began. `layout` is set only if the state carries one.
bool load_state_file(NDS& nds, const std::string& path, ds::sdl::Display::Layout& layout, bool& layout_loaded) {
  layout_loaded = false;
  std::vector<u8> bytes;
  if (FILE* f = std::fopen(path.c_str(), "rb")) {
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n > 0) {
      bytes.resize(static_cast<size_t>(n));
      if (std::fread(bytes.data(), 1, bytes.size(), f) != bytes.size()) bytes.clear();
    }
    std::fclose(f);
  }
  if (bytes.empty()) {
    std::fprintf(stderr, "state: no state in slot (%s)\n", path.c_str());
    return false;
  }
  g_state_refused.clear();
  ds::state::Reader r(bytes.data(), bytes.size());
  std::string err;
  if (!nds.load_state(r, err)) {
    std::fprintf(stderr, "state: cannot load %s: %s\n", path.c_str(), err.c_str());
    g_state_refused = "REJECTED";
    return false;
  }
  layout_loaded = read_layout_chunk(r, layout);
  read_cheevos_chunk(r);
  std::fprintf(stderr, "state: loaded %s (frame %llu%s)\n", path.c_str(),
               static_cast<unsigned long long>(nds.frame_count),
               layout_loaded ? (std::string(", layout ") + ds::sdl::Display::mode_name(layout.mode)).c_str() : "");
  return true;
}

// paths.dsi_games; paths.dsi_shortcuts is its old name.
std::string dsi_games_dir(const ds::sdl::Config& cfg) {
  const std::string d = cfg.str("paths.dsi_games");
  return d.empty() ? cfg.str("paths.dsi_shortcuts") : d;
}

// NAND title shortcuts: paths.dsi_games, or the games folder if unset.
std::string shortcuts_dir(const ds::sdl::Config& cfg) {
  const std::string d = dsi_games_dir(cfg);
  return d.empty() ? cfg.str("paths.games") : d;
}

// Everything keyed to the ROM in the slot: save/states/screenshots/cheats
// paths and the per-game .ini a hotkey writes to. Boot fills this once; the
// loader cart resets the machine and re-opens it without redoing config
// layering (a second per-game .ini merge would accumulate, not replace) --
// only game_ini is re-keyed, so a post-launch hotkey targets the right game.
struct Session {
  std::string rom_path;         // what is in the slot
  std::string rom_dir;          // its directory: the default home for everything below
  std::string game_ini;         // per-game settings; empty when there is no cart
  std::string states_dir;       // save states (and the autosave's PNG)
  std::string shots_dir;        // manual screenshots (F9); paths.screenshots, else states_dir
  std::string sav;              // battery save
  std::string cheats_on_path;   // which cheats are on, one name per line
  ds::cheat::GameCheats cheats; // the database entry for this ROM; the menu points at its groups

  // save_arg is --save: pins the battery save regardless of ROM.
  void open(NDS& nds, const ds::sdl::Config& cfg, const std::string& rom, const char* save_arg);
  void load_enabled(NDS& nds) const;
  void save_enabled(NDS& nds) const;
};

void Session::open(NDS& nds, const ds::sdl::Config& cfg, const std::string& rom, const char* save_arg) {
  rom_path = rom;
  const size_t slash = rom_path.find_last_of('/');
  rom_dir = slash == std::string::npos ? "." : rom_path.substr(0, slash);
  states_dir = cfg.str("paths.states", rom_dir);
  shots_dir = cfg.str("paths.screenshots", states_dir);
  sav = save_arg ? std::string(save_arg) : save_path(rom_path, cfg.str("paths.saves"));
  game_ini.clear();
  if (nds.cart) {
    game_ini = ds::sdl::Config::game_path_rom(rom_path);
    if (game_ini.empty()) game_ini = ds::sdl::Config::game_path_code(nds.cart->header().game_code);
  }

  // usrcheat.dat from paths.cheats, beside the ROM, or the config dir.
  cheats = ds::cheat::GameCheats{};
  nds.cheats.codes.clear();
  cheats_on_path.clear();
  std::string db = cfg.str("paths.cheats");
  // Path may name the file itself or its directory; avoid a double usrcheat.dat.
  if (!db.empty()) {
    auto ends_with_db = [](const std::string& s) {
      static const std::string tail = "usrcheat.dat";
      return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
    };
    while (db.size() > 1 && (db.back() == '/' || db.back() == '\\')) db.pop_back();
    if (!ends_with_db(db)) {
      std::string joined = db + "/usrcheat.dat";
      if (FILE* f = std::fopen(joined.c_str(), "rb")) {
        std::fclose(f);
        db = joined;
      }
    }
  }
  if (db.empty()) {
    for (const std::string& candidate : {rom_dir + "/usrcheat.dat", ds::sdl::Config::dir() + "/usrcheat.dat"}) {
      if (FILE* f = std::fopen(candidate.c_str(), "rb")) {
        std::fclose(f);
        db = candidate;
        break;
      }
    }
  }
  if (!db.empty()) {
    std::string err;
    // Header from the loaded cart, not the file at rom_path: a zipped game's
    // file starts with the archive header, so every lookup would miss.
    u8 header[512] = {};
    if (nds.cart) nds.cart->rom_read(0, header, sizeof header);
    if (nds.cart && ds::cheat::load_for_header(db, header, cheats, err)) {
      VLOG("cheats: %s -- %zu codes in %zu groups\n", cheats.name.c_str(), cheats.codes.size(), cheats.groups.size());
      nds.cheats.codes = cheats.codes;
    } else if (!err.empty()) {
      std::fprintf(stderr, "cheats: %s\n", err.c_str());
    }
  }
  // Names, not indices: a database update renumbers; a stale name is dropped.
  if (!cheats.codes.empty())
    cheats_on_path = states_dir + "/" + std::string(nds.cart ? nds.cart->header().game_code : "NONE", 4) + ".cheats";
}

void Session::load_enabled(NDS& nds) const {
  if (cheats_on_path.empty()) return;
  FILE* f = std::fopen(cheats_on_path.c_str(), "rb");
  if (!f) return;
  char line[512];
  size_t on = 0;
  while (std::fgets(line, sizeof line, f)) {
    std::string want(line);
    while (!want.empty() && (want.back() == '\n' || want.back() == '\r')) want.pop_back();
    if (want.empty()) continue;
    for (ds::cheat::Code& c : nds.cheats.codes)
      if (!c.is_note() && c.name == want) {
        c.enabled = true;
        ++on;
      }
  }
  std::fclose(f);
  if (on) VLOG("cheats: %zu enabled from %s\n", on, cheats_on_path.c_str());
}

void Session::save_enabled(NDS& nds) const {
  if (cheats_on_path.empty()) return;
  FILE* f = std::fopen(cheats_on_path.c_str(), "wb");
  if (!f) {
    std::fprintf(stderr, "cheats: cannot write %s\n", cheats_on_path.c_str());
    return;
  }
  for (const ds::cheat::Code& c : nds.cheats.codes)
    if (c.enabled && !c.is_note()) std::fprintf(f, "%s\n", c.name.c_str());
  std::fclose(f);
}

// The stick-driven pen: an outlined crosshair with a red centre, drawn over
// the bottom screen in DS pixel space and mapped onto the destination.
struct CursorDst {
  u32* px;
  u32 pitch;
  u32 h;
  const u16* xrun;
};
void draw_cursor(const CursorDst& d, int cx, int cy, int size) {
  auto fill = [&](int x, int y, u32 colour) {
    if (x < 0 || x > 255 || y < 0 || y > 191) return;
    const u32 w = d.xrun ? d.xrun[ds::SCREEN_W] : ds::SCREEN_W;
    for (u32 yy = u32(y) * d.h / 192; yy < u32(y + 1) * d.h / 192; ++yy)
      for (u32 xx = u32(x) * w / 256; xx < u32(x + 1) * w / 256; ++xx) d.px[yy * d.pitch + xx] = colour;
  };
  // `size` scales the shape: arms `size` wide, 3*size long, size x size centre.
  auto box = [&](int x0, int y0, int w, int h, u32 colour) {
    for (int y = y0; y < y0 + h; ++y)
      for (int x = x0; x < x0 + w; ++x) fill(x, y, colour);
  };
  const u32 outline = 0xFF14263B, line = 0xFF4EBBFF, centre = 0xFFE9F4FF;
  if (size < 1) size = 1;
  const int arm = 3 * size, half = size / 2; // the centre box spans [cx-half, cx-half+size)
  const int c0 = -half, c1 = -half + size;   // centre extent relative to cx/cy
  box(cx + c0 - arm - 1, cy + c0 - 1, 2 * arm + size + 2, size + 2, outline); // horizontal bar outline
  box(cx + c0 - 1, cy + c0 - arm - 1, size + 2, 2 * arm + size + 2, outline); // vertical bar outline
  box(cx + c0 - arm, cy + c0, 2 * arm + size, size, line);
  box(cx + c0, cy + c0 - arm, size, 2 * arm + size, line);
  box(cx + c0, cy + c0, c1 - c0, c1 - c0, centre);
}

// A launcher's SIGTERM (or Ctrl-C) must still flush the battery save.
volatile std::sig_atomic_t g_signalled = 0;
void on_signal(int) { g_signalled = 1; }

// [video] settings, re-read/re-applied from the pause menu, not just at boot.
// parse_video() is pure (no SDL); open_displays() does the set_* calls that
// must precede Display::open. The sink plan and dual-window order are
// boot-only: they can force SDL's video driver, fixed before SDL_Init.
struct VideoSetup {
  int scale = 2;
  bool fullscreen = false, linear = false, vsync = true, dual_window = false;
  ds::sdl::Display::IntScale int_scale = ds::sdl::Display::IntScale::Off;
  double grid_s = 0.0; // 0..1 as configured
  u32 grid = 256;      // brightness kept on a seam, 0..256 (256 = off)
  u8 seam_blend = 0;   // 0 dark, 1 blend, 2 blend in linear light
  u8 chunky = 0;
  int chunky_cell = -1;
  u32 chunky_thresh = 180 * 256;
  ds::sdl::Display::Layout layout;
  std::vector<ds::sdl::Display::Mode> layout_cycle;
  // Boot-only, see above.
  ds::frontend::Sink panel_sink =
      ds::frontend::Sink::Count; // Disp/Fbdev when one owns the panel (frontend/video/select.h)
  ds::frontend::Sink want_sink = ds::frontend::Sink::Count; // explicit video.sink among the window sinks
  bool gpu_present = false;
  ds::sdl::Display::GpuPresentMode gpu_mode =
      ds::sdl::Display::GpuPresentMode::Auto; // video.gpu_present: true (auto: RGA, else Vulkan) | rga | vulkan
  int upper_display = 0, lower_display = 1;   // dual-window: SDL display per physical panel
};

constexpr const char* kDefaultLayoutCycle = "vertical,horizontal,single,pip,dominant_v,dominant_h";

// "vertical, pip" -> modes in file order, duplicates kept. Unknown name
// fails the parse. `out` may come back empty; caller decides what that means.
bool parse_layout_cycle(const std::string& cyc, std::vector<ds::sdl::Display::Mode>& out) {
  using Disp = ds::sdl::Display;
  out.clear();
  for (size_t at = 0; at <= cyc.size();) {
    size_t end = cyc.find(',', at);
    if (end == std::string::npos) end = cyc.size();
    std::string name = cyc.substr(at, end - at);
    name.erase(0, name.find_first_not_of(' '));
    name.erase(name.find_last_not_of(' ') + 1);
    Disp::Mode m;
    if (!name.empty() && !Disp::parse_mode(name, m)) {
      std::fprintf(stderr, "unknown layout %s in layout_cycle\n", name.c_str());
      return false;
    }
    if (!name.empty()) out.push_back(m);
    at = end + 1;
  }
  return true;
}

bool parse_video(const ds::sdl::Config& cfg, VideoSetup& vs) {
  using Disp = ds::sdl::Display;
  vs.scale = cfg.num("video.scale", 2);
  if (vs.scale < 1) vs.scale = 1;
  vs.fullscreen = cfg.flag("video.fullscreen", false);
  vs.linear = cfg.flag("video.linear", false);
  vs.vsync = cfg.flag("video.vsync", true);
  vs.dual_window = cfg.flag("video.dual_window", false);
  if (!Disp::parse_int_scale(cfg.str("video.integer_scale", "off"), vs.int_scale)) {
    std::fprintf(stderr, "integer_scale must be off, under or over\n");
    return false;
  }
  vs.grid_s = std::min(1.0, std::max(0.0, cfg.real("video.lcd_grid", 0.0)));
  vs.grid = static_cast<u32>(std::lround((1.0 - vs.grid_s) * 256.0));
  vs.seam_blend = 0;
  {
    const std::string sm = cfg.str("video.seam", "dark");
    if (sm == "blend") vs.seam_blend = 1;
    else if (sm == "blend_linear") vs.seam_blend = 2;
    else if (sm != "dark") {
      std::fprintf(stderr, "unknown seam %s (dark | blend | blend_linear)\n", sm.c_str());
      return false;
    }
    if (vs.linear && (vs.grid_s > 0.0 || vs.seam_blend || cfg.str("video.chunky", "false") != "false"))
      std::fprintf(stderr, "video.linear takes precedence over lcd_grid, seam and chunky\n");
  }
  {
    const std::string c = cfg.str("video.chunky_cell", "auto");
    if (c == "auto") vs.chunky_cell = -1;
    else if (c == "pair" || c == "2x") vs.chunky_cell = 0;
    else {
      vs.chunky_cell = std::atoi(c.c_str());
      if (vs.chunky_cell < 2 || vs.chunky_cell > 64) {
        std::fprintf(stderr, "chunky_cell %s: auto | pair | 2..64\n", c.c_str());
        return false;
      }
    }
  }
  vs.chunky_thresh = static_cast<u32>(std::min(255, std::max(0, cfg.num("video.chunky_threshold", 180)))) * 256;
  vs.chunky = 0;
  {
    const std::string c = cfg.str("video.chunky", "false");
    if (c == "tl") vs.chunky = 1;
    else if (c == "mean" || c == "true" || c == "1" || c == "yes" || c == "on") vs.chunky = 2;
    else if (c == "min") vs.chunky = 4;
    else if (c == "max") vs.chunky = 5;
    else if (c == "mode") vs.chunky = 3;
    else if (c == "extreme") vs.chunky = 6;
    else if (!(c == "false" || c == "0" || c == "no" || c == "off" || c.empty())) {
      std::fprintf(stderr, "unknown chunky %s (mean | extreme | mode | tl | min | max | false)\n", c.c_str());
      return false;
    }
  }
  vs.layout = Disp::Layout{};
  vs.layout_cycle.clear();
  {
    const std::string l = cfg.str("video.layout", "vertical"), sc = cfg.str("video.screen", "top"),
                      co = cfg.str("video.pip_corner", "br");
    if (!Disp::parse_mode(l, vs.layout.mode)) {
      std::fprintf(stderr, "unknown layout %s\n", l.c_str());
      return false;
    }
    if (sc == "top") vs.layout.primary = 0;
    else if (sc == "bottom") vs.layout.primary = 1;
    else {
      std::fprintf(stderr, "unknown screen %s (top | bottom)\n", sc.c_str());
      return false;
    }
    if (!Disp::parse_corner(co, vs.layout.corner)) {
      std::fprintf(stderr, "unknown pip_corner %s (tl | tr | bl | br)\n", co.c_str());
      return false;
    }
    if (!parse_layout_cycle(cfg.str("video.layout_cycle", kDefaultLayoutCycle), vs.layout_cycle)) return false;
    if (vs.layout_cycle.empty()) vs.layout_cycle.push_back(vs.layout.mode);
    vs.layout.pip = std::clamp(cfg.real("video.pip_scale", 1.0 / 3.0), 0.1, 0.9);
    vs.layout.pip_alpha = std::clamp(cfg.real("video.pip_alpha", 1.0), 0.0, 1.0);
    vs.layout.gap_auto = cfg.str("video.screen_gap", "0") == "auto"; // the pair pushed to the edges
    vs.layout.gap = vs.layout.gap_auto
                        ? 0
                        : cfg.num("video.screen_gap", 0); // menu bounds it; file/CLI take anything, overlap included
    const std::string dr = cfg.str("video.dominant_ratio", "auto");
    vs.layout.dominant_auto = dr == "auto";
    if (!vs.layout.dominant_auto) {
      char* end = nullptr;
      const double v = std::strtod(dr.c_str(), &end);
      if (end == dr.c_str() || *end) {
        std::fprintf(stderr, "dominant_ratio must be a number or auto\n");
        return false;
      }
      vs.layout.dominant = std::clamp(v, 0.1, 0.99);
    }
    vs.layout.dominant_min = std::clamp(cfg.real("video.dominant_threshold", 0.25), 0.1, 0.99);
  }
  return true;
}

// Dual-window: both windows present the same way (the frame loop hands the
// scaled targets to both or draws both). One that could not get or keep GPU
// present, e.g. a dmabuf allocation failing on a CMA pool full of page cache,
// takes the other down to scanline scaling with it. False if still mixed.
bool sync_dual_modes(ds::sdl::Display& a, ds::sdl::Display& b) {
  if (a.scaling() == b.scaling()) return true;
  (a.scaling() ? b : a).drop_gpu_present("dropped to match the other window");
  return a.scaling() == b.scaling();
}

// Split from parse_video so a settings change can close the windows and
// come back through here with new values.
bool open_displays(const VideoSetup& vs, ds::sdl::Display& display, ds::sdl::Display& display2) {
  display.set_gpu_present(vs.gpu_present, vs.gpu_mode);
  display2.set_gpu_present(vs.gpu_present, vs.gpu_mode);
  if (vs.dual_window) {
    display.set_sinks(ds::frontend::Sink::Count, vs.want_sink);
    display2.set_sinks(ds::frontend::Sink::Count, vs.want_sink);
    display.set_chunky(vs.chunky != 0, vs.chunky_cell);
    display2.set_chunky(vs.chunky != 0, vs.chunky_cell);
    display.set_grid_strength(vs.linear ? 0.0 : vs.grid_s);
    display2.set_grid_strength(vs.linear ? 0.0 : vs.grid_s);
    display.set_integer_scale(vs.int_scale);
    display2.set_integer_scale(vs.int_scale);
    if (!display.open("RG DS Core", vs.scale, vs.fullscreen, vs.linear, vs.vsync, vs.layout, 0, vs.upper_display) ||
        !display2.open("RG DS Core (Bottom)", vs.scale, vs.fullscreen, vs.linear, vs.vsync, vs.layout, 1,
                       vs.lower_display))
      return false;
    if (!sync_dual_modes(display, display2)) {
      std::fprintf(stderr, "dual-window: mixed display modes\n");
      return false;
    }
    // MAIN SCREEN picks the DS screen on the upper panel.
    display.set_only_screen(vs.layout.primary);
    display2.set_only_screen(1 - vs.layout.primary);
    return true;
  }
  display.set_chunky(vs.chunky != 0, vs.chunky_cell);
  display.set_sinks(vs.panel_sink, vs.want_sink);
  display.set_integer_scale(vs.int_scale);
  display.set_grid_strength(vs.linear ? 0.0 : vs.grid_s);
  if (!vs.linear) display.set_disp_grid(static_cast<u8>(((256 - vs.grid) * 255) / 256));
  return display.open("RG DS Core", vs.scale, vs.fullscreen, vs.linear, vs.vsync, vs.layout);
}

// Controls page rows that are neither a DS button nor a hotkey.
} // namespace

// DS_STARTUP_TRACE=1 (or DS_HIRES_DEBUG): monotonic stamps of the startup stages.
static void startup_stage(const char* what) {
  static const bool on = std::getenv("DS_STARTUP_TRACE") || std::getenv("DS_HIRES_DEBUG");
  if (!on) return;
  timespec t{};
  clock_gettime(CLOCK_MONOTONIC, &t);
  std::fprintf(stderr, "startup: %s at %.1f ms\n", what, t.tv_sec * 1e3 + t.tv_nsec / 1e6);
}
static int run(int argc, char** argv) {
  startup_stage("run");
  const char* rom = nullptr;
  const char* config_arg = nullptr;
  long frame_limit = 0;
  const char *record = nullptr, *replay = nullptr, *save_arg = nullptr, *load_state = nullptr;
  bool clear_cache = false;
  bool rtc_host = false; // --rtc-host: real clock even under replay
  const char* lan_host = nullptr;
  const char* lan_join = nullptr;
  const char* lan_name = "RG DS Core";
  bool netplay = false;
  bool lan_guest = false;    // net.mode = guest: join only, never host
  bool lan_name_set = false; // --lan-name given, so nickname must not override it
  bool internet = false;     // --internet / net.mode = internet: emulated AP reaches the real network
  long stats_from = 0;       // frames run but left out of timing statistics
  bool dsi_mode = false;
  const char* bios9i = nullptr;
  const char* bios7i = nullptr;
  const char* dsi_nand = nullptr;
  const char* dsi_title = nullptr;
  const char* dsi_tmd = nullptr;
  bool dsi_hide_installed = false, dsi_menu = false;
  bool dsi_hle = false; // --dsi-mode with title and no NAND: launcher hand-off, BIOS pair only
  const char* dsi_font = nullptr;
  const char* dsi_sd = nullptr;
  u32 dsi_title_lo = 0;               // title --dsi-mode was given, once on the NAND: what autoload starts
  std::string dsi_title_cheevos_hash; // RetroAchievements identity; runs from NAND, no cart to hash later

  // Found before options are read, so an optional-value flag like --chunky
  // [M] doesn't swallow `--chunky game.nds` as its mode.
  const char* rom_word = nullptr;
  auto looks_like_rom = [](const char* w) {
    if (w[0] == '-') return false;
    const std::string ext = rom_ext(w);
    if (ext == ".nds" || ext == ".zip") return true;
    struct stat st;
    return ::stat(w, &st) == 0 && S_ISREG(st.st_mode);
  };
  for (int i = 1; i < argc && !rom_word; ++i)
    if (looks_like_rom(argv[i])) rom_word = argv[i];
  // Command line is one more settings layer, applied after the files.
  ds::sdl::Config cli;
  for (int i = 1; i < argc; ++i) {
    auto arg = [&](const char* name) { return !std::strcmp(argv[i], name) && i + 1 < argc; };
    auto flag = [&](const char* name) { return !std::strcmp(argv[i], name); };
    // Next word unless it's another option or the game.
    auto optional = [&](const char* fallback) {
      return i + 1 < argc && argv[i + 1][0] != '-' && argv[i + 1] != rom_word ? argv[++i] : fallback;
    };
    if (arg("--bios9")) cli.set("paths.bios9", argv[++i]);
    else if (arg("--bios7")) cli.set("paths.bios7", argv[++i]);
    else if (arg("--firmware")) cli.set("paths.firmware", argv[++i]);
    else if (flag("--dsi-mode")) dsi_mode = true;
    else if (arg("--bios9i")) bios9i = argv[++i];
    else if (arg("--bios7i")) bios7i = argv[++i];
    else if (arg("--dsi-nand")) dsi_nand = argv[++i];
    else if (arg("--dsi-tmd")) dsi_tmd = argv[++i];
    else if (arg("--dsi-font")) dsi_font = argv[++i];
    else if (arg("--dsi-sd")) dsi_sd = argv[++i];
    else if (flag("--dsi-offline")) { /* This build is already offline. */ } else if (flag("--dsi-menu"))
      dsi_menu = true;
    else if (flag("--dsi-hide-installed")) dsi_hide_installed = true;
    else if (arg("--config")) config_arg = argv[++i];
    else if (arg("--write-config")) {
      ds::sdl::Config::write_default(argv[++i], true);
      return 0;
    } else if (arg("--scale")) cli.set("video.scale", argv[++i]);
    else if (flag("--dual-window")) cli.set("video.dual_window", "true");
    else if (arg("--pacing")) cli.set("emu.pacing", argv[++i]);
    else if (arg("--layout")) cli.set("video.layout", argv[++i]);
    else if (arg("--screen")) cli.set("video.screen", argv[++i]);
    else if (arg("--pip-alpha")) cli.set("video.pip_alpha", argv[++i]);
    else if (arg("--screen-gap")) cli.set("video.screen_gap", argv[++i]);
    else if (arg("--dominant-ratio")) cli.set("video.dominant_ratio", argv[++i]);
    else if (arg("--dominant-threshold")) cli.set("video.dominant_threshold", argv[++i]);
    else if (flag("--integer-scale")) cli.set("video.integer_scale", optional("under"));
    else if (arg("--frames")) frame_limit = std::atol(argv[++i]);
    else if (flag("--rtc-host")) rtc_host = true;
    else if (arg("--lan-host")) lan_host = argv[++i];
    else if (arg("--lan-join")) lan_join = argv[++i];
    else if (arg("--lan-name")) {
      lan_name = argv[++i];
      lan_name_set = true;
    } else if (flag("--netplay")) netplay = true; // join a session heard on the LAN, else host one
    else if (flag("--internet")) internet = true; // exclusive with local wireless
    else if (arg("--dns")) {
      ++i; /* No host network transport in RGDS. */
    } else if (flag("--clear-cache")) clear_cache = true;
    else if (arg("--record")) record = argv[++i];
    else if (arg("--replay")) replay = argv[++i];
    else if (arg("--save")) save_arg = argv[++i];
    else if (arg("--load-state")) load_state = argv[++i];
    else if (arg("--autosave-png")) cli.set("emu.autosave_png", argv[++i]);
    else if (flag("--autoload")) cli.set("emu.autoload", "true");
    else if (flag("--no-autoload")) cli.set("emu.autoload", "false");
    else if (arg("--stats-from")) stats_from = std::atol(argv[++i]);
    else if (flag("--fullscreen")) cli.set("video.fullscreen", "true");
    else if (flag("--linear")) cli.set("video.linear", "true");
    else if (arg("--lcd-grid")) cli.set("video.lcd_grid", argv[++i]);
    else if (flag("--chunky")) cli.set("video.chunky", optional("mean"));
    else if (arg("--chunky-threshold")) cli.set("video.chunky_threshold", argv[++i]);
    else if (arg("--chunky-cell")) cli.set("video.chunky_cell", argv[++i]);
    else if (arg("--seam")) cli.set("video.seam", argv[++i]);
    else if (arg("--sink")) cli.set("video.sink", argv[++i]);
    else if (flag("--disp")) cli.set("video.sink", "disp");
    else if (flag("--no-disp")) cli.set("video.disp", "false");
    else if (flag("--fbdev")) cli.set("video.sink", "fbdev");
    else if (flag("--no-fbdev")) cli.set("video.fbdev", "false");
    else if (flag("--gpu-present")) cli.set("video.gpu_present", "true");
    else if (flag("--no-gpu-present")) cli.set("video.gpu_present", "false");
    else if (flag("--no-audio")) cli.set("audio.enabled", "false");
    else if (arg("--volume")) cli.set("audio.volume", argv[++i]);
    else if (arg("--audio-buffer")) cli.set("audio.buffer_size", argv[++i]);
    else if (arg("--speed")) cli.set("emu.speed", argv[++i]);
    else if (arg("--limiter")) cli.set("emu.limiter", argv[++i]);
    else if (flag("--no-mic")) cli.set("audio.mic", "false");
    else if (flag("--no-vsync")) cli.set("video.vsync", "false");
    else if (flag("--interp")) cli.set("emu.jit", "false");
    else if (arg("--timing")) cli.set("emu.timing", argv[++i]);
    else if (arg("--frameskip")) cli.set("emu.frameskip", argv[++i]);
    else if (arg("--frameskip-mode")) cli.set("emu.frameskip_mode", argv[++i]);
    else if (flag("--frameskip-capture")) cli.set("emu.frameskip_capture", "true");
    else if (flag("--no-frameskip-capture")) cli.set("emu.frameskip_capture", "false");
    else if (flag("--aa")) cli.set("video.aa", "true");
    else if (flag("--no-aa")) cli.set("video.aa", "false");
    else if (flag("--gpu3d")) cli.set("video.gpu3d", "true");
    else if (flag("--preload-rom")) cli.set("cart.preload", "true");
    else if (flag("--no-preload-rom")) cli.set("cart.preload", "false");
    else if (flag("--no-gpu3d")) cli.set("video.gpu3d", "false");
    else if (flag("--version")) {
      std::printf("RG DS Core %s (%s)\n", kDsperateVersion, kDsperateCommit);
      return 0;
    } else if (flag("--help")) {
      std::fputs(kUsage, stderr);
      return 0;
    } else if (argv[i][0] == '-' && argv[i][1] == '-') {
      std::fprintf(stderr, "unknown option %s\n", argv[i]);
      std::fputs(kUsage, stderr);
      return 2;
    } else rom = argv[i];
  }
  // BootMenuDSi.nds is --dsi-mode, the way BootMenu.nds is a firmware boot;
  // neither file has to exist. ROM is dropped, becoming the cartless DSi boot.
  const char* dsi_mode_by = "--dsi-mode";
  if (rom && rom_stem(base_name(rom)) == "BootMenuDSi") {
    dsi_mode = true;
    dsi_mode_by = "BootMenuDSi.nds";
    rom = nullptr;
  }
  // DSiWare named without --dsi-mode runs the DSi machine too: as a DS it
  // would not start at all.
  const bool dsi_mode_asked = dsi_mode; // --dsi-mode itself, not implied by a DSiWare title
  const bool rom_shortcut = rom && ds::io::is_shortcut_name(rom);
  if (!dsi_mode && rom && !dsi_nand && (rom_shortcut || ds::io::file_is_dsiware(rom))) dsi_mode = true;
  ds::sdl::Config cfg;
  const std::string global_ini = config_arg ? std::string(config_arg) : ds::sdl::Config::global_path();
  if (!config_arg) ds::sdl::Config::write_default(global_ini);
  if (!cfg.load(global_ini) && config_arg) {
    std::fprintf(stderr, "cannot read %s\n", config_arg);
    return 2;
  }
  // paths.dsi_nand stands in for --dsi-nand only under --dsi-mode.
  const std::string dsi_nand_cfg = dsi_mode_asked && !dsi_nand ? cfg.str("paths.dsi_nand") : std::string();
  if (!dsi_nand_cfg.empty()) dsi_nand = dsi_nand_cfg.c_str();
  if (!dsi_hide_installed) dsi_hide_installed = cfg.flag("emu.dsi_hide_installed", false);
  const std::string shortcut_nand = dsi_nand ? std::string(dsi_nand) : cfg.str("paths.dsi_nand");
  if (dsi_mode) {
    // A game named with a NAND is DSiWare, installed into the session's NAND.
    dsi_hle = !dsi_nand && rom;
    if (!dsi_nand && !rom) {
      std::fprintf(stderr, "%s needs --dsi-nand (or paths.dsi_nand in %s) or a DSiWare title\n", dsi_mode_by,
                   global_ini.c_str());
      return 2;
    }
    if (dsi_hle && dsi_menu) {
      std::fprintf(stderr, "--dsi-menu needs --dsi-nand: without one there is no DSi Menu to boot\n");
      return 2;
    }
    // Without a NAND the title goes in the slot (NDS::prepare_dsi_hle).
    if (!dsi_hle) {
      dsi_title = rom;
      rom = nullptr;
    }
  }
  auto apply_cli = [&] {
    for (const char* k : {"paths.bios9",
                          "paths.bios7",
                          "paths.firmware",
                          "video.scale",
                          "video.dual_window",
                          "video.layout",
                          "video.screen",
                          "video.pip_alpha",
                          "video.screen_gap",
                          "video.dominant_ratio",
                          "video.dominant_threshold",
                          "video.integer_scale",
                          "video.fullscreen",
                          "video.linear",
                          "video.lcd_grid",
                          "video.chunky",
                          "video.chunky_threshold",
                          "video.chunky_cell",
                          "video.seam",
                          "video.sink",
                          "video.disp",
                          "video.fbdev",
                          "video.gpu_present",
                          "video.vsync",
                          "audio.enabled",
                          "audio.volume",
                          "audio.mic",
                          "emu.jit",
                          "emu.timing",
                          "emu.speed",
                          "emu.limiter",
                          "emu.pacing",
                          "audio.buffer_size",
                          "audio.latency_frames",
                          "emu.frameskip",
                          "emu.frameskip_mode",
                          "emu.frameskip_capture",
                          "video.aa",
                          "video.gpu3d",
                          "cart.preload",
                          "emu.autosave_png",
                          "emu.autoload",
                          "cheevos.enabled",
                          "cheevos.token_file",
                          "cheevos.username"})
      if (cli.has(k)) cfg.set(k, cli.str(k));
  };
  apply_cli();
  const std::string bios9 = cfg.str("paths.bios9"), bios7 = cfg.str("paths.bios7");
  const std::string dsi_fw = cfg.str("paths.dsi_firmware");
  const bool have_dsi_fw = !dsi_fw.empty() && std::filesystem::exists(dsi_fw);
  const std::string fw = dsi_mode && have_dsi_fw && !cli.has("paths.firmware") ? dsi_fw : cfg.str("paths.firmware");
  const std::string dsi_bios9i = bios9i ? std::string(bios9i) : cfg.str("paths.bios9i");
  const std::string dsi_bios7i = bios7i ? std::string(bios7i) : cfg.str("paths.bios7i");
  auto have_dsi_bios = [&] {
    return !dsi_bios9i.empty() && !dsi_bios7i.empty() && std::filesystem::exists(dsi_bios9i) &&
           std::filesystem::exists(dsi_bios7i);
  };
  if (dsi_mode && !have_dsi_bios()) {
    std::fprintf(stderr, "%s needs the DSi BIOS pair: --bios9i/--bios7i, or paths.bios9i/paths.bios7i in %s\n",
                 dsi_hle ? (std::string(rom) + " is DSiWare and").c_str() : dsi_mode_by, global_ini.c_str());
    return 2;
  }

  // No ROM (or BootMenu.nds, needn't exist) boots the firmware's own menu.
  // Every per-game path below is derived from this string.
  const bool boot_firmware = !rom || rom_stem(base_name(rom)) == "BootMenu";
  const std::string rom_path = rom ? std::string(rom) : ds::sdl::Config::dir() + "/BootMenu.nds";
  if (boot_firmware) VLOG("no game: booting the firmware\n");

  // Per-game files on top of the global one, CLI on top of both: title ID
  // first, then filename-keyed, so the filename wins. Rerun after the cart
  // loads to cover a zip (game code unreadable until unpacked).
  auto merge_game_config = [&](const char* code) {
    std::vector<std::string> paths;
    if (code) paths.push_back(ds::sdl::Config::game_path_code(code));
    paths.push_back(ds::sdl::Config::game_path_rom(rom_path));
    for (const std::string& p : paths)
      if (!p.empty() && cfg.load(p)) VLOG("config: %s\n", p.c_str());
    apply_cli();
  };
  if (!boot_firmware) {
    char code[4];
    merge_game_config(peek_game_code(rom_path, code) ? code : nullptr);
  }

  // Set before any thread exists so every later thread inherits it. Needs
  // privilege (root, or an rtprio limit); refused quietly otherwise.
  // emu.realtime = off | rr | fifo, emu.rt_priority.
  {
    const std::string rt = cfg.str("emu.realtime", "rr");
    const int prio = cfg.num("emu.rt_priority", 5);
    if (rt == "rr" || rt == "fifo") {
      sched_param sp{};
      sp.sched_priority = prio;
      if (sched_setscheduler(0, rt == "rr" ? SCHED_RR : SCHED_FIFO, &sp) != 0)
        VLOG("realtime scheduling (%s %d) not permitted: %s\n", rt.c_str(), prio, std::strerror(errno));
      else {
        VLOG("realtime scheduling: %s %d\n", rt.c_str(), prio);
      }
    }
    ds::latch_worker_sched(); // what the video workers run at, whichever thread starts them
  }
  // CPU timing model (core/cpu/timing_mode.h), fixed before anything runs or
  // the recompiler emits its stubs. DS_TIMING wins.
  if (cfg.has("emu.timing") && !std::getenv("DS_TIMING")) ds::g_fast_timing = cfg.str("emu.timing") != "exact";
  VLOG("cpu timing: %s\n", ds::g_fast_timing ? "fast" : "exact");
  if (cfg.num("emu.host_cores", 0) > 0)
    ds::set_host_cores(static_cast<ds::u32>(cfg.num("emu.host_cores", 0))); // DS_HOST_CORES still wins
  VLOG("host: %u cores\n", ds::host_cores());
  // Thread layout (host_cores.h): before any worker starts, so each places itself.
  {
    const std::string tl = cfg.str("emu.thread_layout", "auto");
    const bool on = (tl == "on" || tl == "auto") && !std::getenv("DS_NO_THREAD_LAYOUT");
    ds::set_thread_layout(on);
    VLOG("thread layout: %s\n", on ? "pinned" : "free");
  }

  NDS nds;
  if (cfg.has("emu.idle_skip") && !std::getenv("DS_IDLE_SKIP"))
    nds.sched.set_idle_skip(cfg.str("emu.idle_skip").c_str()); // DS_IDLE_SKIP wins
  ds::bios::UserSettings user;
  user.nickname = cfg.str("user.nickname", user.nickname);
  user.message = cfg.str("user.message", user.message);
  user.birthday_month = static_cast<ds::u8>(cfg.num("user.birthday_month", user.birthday_month));
  user.birthday_day = static_cast<ds::u8>(cfg.num("user.birthday_day", user.birthday_day));
  user.favourite_colour = static_cast<ds::u8>(cfg.num("user.colour", user.favourite_colour));
  user.language = static_cast<ds::u8>(cfg.num("user.language", user.language));
  // net.mode: config only speaks when no --netplay/--lan-* flag already decided.
  std::string lan_name_str = lan_name;
  if (!lan_name_set && !user.nickname.empty()) lan_name_str = user.nickname;
  lan_name = lan_name_str.c_str();
  if (!lan_host && !lan_join && !netplay && !internet) {
    const std::string mode = cfg.str("net.mode", "off");
    if (mode == "auto") netplay = true;
    else if (mode == "host") lan_host = lan_name;
    else if (mode == "guest") lan_guest = true;
    else if (mode == "internet") internet = true;
    else if (mode != "off")
      std::fprintf(stderr, "net.mode: \"%s\" is not off, auto, host, guest or internet\n", mode.c_str());
  }
  if (internet && (lan_host || lan_join || netplay || lan_guest)) {
    std::fprintf(stderr, "net: --internet is not local wireless; pick one\n");
    return 1;
  }
  {
    std::string err;
    if (!nds.load_bios(bios9, bios7, fw, user, &err)) {
      std::fprintf(stderr, "bios: %s\n", err.c_str());
      return 1;
    }
    if (!nds.firmware_synthetic) VLOG("firmware: %s\n", fw.c_str());
    // A fixed MAC (dump's own, or generated 00:09:BF:11:22:33) collides
    // across instances and breaks PictoChat. Low 3 bytes generated once,
    // kept in config (the only part a DS lets differ; sent as `macadr` identity).
    {
      u32 suffix = static_cast<u32>(cfg.num("net.mac_suffix", -1));
      if (suffix > 0xFFFFFF) {
        std::random_device rd;
        suffix = rd() & 0xFFFFFF;
        if (!ds::sdl::Config::store(global_ini, "net.mac_suffix", std::to_string(suffix)))
          std::fprintf(stderr, "net: could not remember this console's MAC in %s\n", global_ini.c_str());
        cfg.set("net.mac_suffix", std::to_string(suffix));
      }
      nds.set_wifi_mac_suffix(suffix);
    }
  }
  if (!nds.bios_native)
    std::fprintf(stderr, "bios: %s, using the built-in FreeBIOS (direct boot only; timing is not Nintendo's)\n",
                 bios9.empty() ? "no dumps given" : "dumps not found");
  if (nds.firmware_synthetic)
    std::fprintf(stderr, "firmware: %s, using a generated one ([user] in %s)\n",
                 fw.empty() ? "no dump given" : "dump not found", global_ini.c_str());
  if (boot_firmware && !nds.can_boot_firmware()) {
    std::fprintf(stderr, "the DS menu needs real dumps: %s%s%s (--bios9/--bios7/--firmware or [paths] in %s)\n",
                 nds.bios_native ? "" : "bios9 and bios7", (!nds.bios_native && nds.firmware_synthetic) ? " and " : "",
                 nds.firmware_synthetic ? "firmware" : "", global_ini.c_str());
    return 2;
  }
  // FreeBIOS lacks the Blowfish key table and SWI behaviour the launcher
  // hand-off needs, so a title on it wedges instead of refusing cleanly.
  if (dsi_mode && !nds.bios_native) {
    std::fprintf(
        stderr,
        "a DSi session needs the DS BIOS dumps as well as the DSi pair: %s (--bios9/--bios7 or [paths] in %s).\n"
        "The built-in FreeBIOS cannot boot one.\n",
        bios9.empty() ? "no dumps given" : "dumps not found", global_ini.c_str());
    return 2;
  }
  // Opened once; a later DSiWare pick from the game list keeps it.
  const std::string dsi_sd_dir = dsi_sd ? std::string(dsi_sd) : cfg.str("paths.dsi_sd");
  auto open_dsi_sd = [&]() -> bool {
    if (dsi_sd_dir.empty() || nds.dsi_sd.valid()) return true;
    ds::io::SdCard::Report r;
    std::string err;
    if (!nds.dsi_sd.open(dsi_sd_dir, &r, &err)) {
      std::fprintf(stderr, "dsi sd: %s\n", err.c_str());
      return false;
    }
    VLOG("dsi sd: %s, %d files and %d folders on a %llu MB FAT%d card\n", dsi_sd_dir.c_str(), r.files, r.dirs,
         static_cast<unsigned long long>(nds.dsi_sd.length() >> 20), nds.dsi_sd.fat_bits());
    for (const std::string& n : r.notes) std::fprintf(stderr, "dsi sd: %s\n", n.c_str());
    return true;
  };
  if (dsi_hle) {
    std::string err;
    if (!nds.load_dsi_bios(dsi_bios9i, dsi_bios7i, &err)) {
      std::fprintf(stderr, "dsi bios: %s\n", err.c_str());
      return 1;
    }
    if (!nds.bios_native_dsi) {
      std::fprintf(stderr, "dsi bios: %s or %s not found\n", dsi_bios9i.c_str(), dsi_bios7i.c_str());
      return 1;
    }
    if (!open_dsi_sd()) return 1;
    if (rom_shortcut) {
      if (shortcut_nand.empty()) {
        std::fprintf(stderr, "%s is a NAND title shortcut and needs the NAND: --dsi-nand or paths.dsi_nand in %s\n",
                     rom, global_ini.c_str());
        return 1;
      }
      if (!nds.load_dsi_nand(shortcut_nand, &err)) {
        std::fprintf(stderr, "dsi nand: %s\n", err.c_str());
        return 1;
      }
    }
    nds.set_dsi(true);
    nds.dsi_hle_launch = true; // NAND and settings made once the title is in the slot
    std::fprintf(stderr, rom_shortcut ? "console: DSi, a title from its NAND without the DSi Menu (EXPERIMENTAL)\n"
                                      : "console: DSi without a NAND (EXPERIMENTAL)\n");
  } else if (dsi_mode) {
    std::string err;
    if (!nds.load_dsi_bios(dsi_bios9i, dsi_bios7i, &err)) {
      std::fprintf(stderr, "dsi bios: %s\n", err.c_str());
      return 1;
    }
    if (!nds.bios_native_dsi) {
      std::fprintf(stderr, "dsi bios: %s or %s not found\n", dsi_bios9i.c_str(), dsi_bios7i.c_str());
      return 1;
    }
    if (!open_dsi_sd()) return 1;
    // Opened read-only: writes held in memory until pulled out as files.
    if (!nds.load_dsi_nand(dsi_nand, &err)) {
      std::fprintf(stderr, "dsi nand: %s\n", err.c_str());
      return 1;
    }
    VLOG("dsi nand: %s (read-only; writes held in memory)\n", dsi_nand);
    const ds::io::NandPersistPaths pp = ds::io::NandPersistPaths::beside(dsi_nand, cfg.str("paths.saves"));
    if (dsi_hide_installed) {
      const int n = ds::io::nand_hide_installed_dsiware(nds.dsi_nand, nds.bus.bios7i.get(), &err);
      if (n < 0) {
        std::fprintf(stderr, "dsi: hiding the installed titles: %s\n", err.c_str());
        return 1;
      }
      std::fprintf(stderr, "dsi: %d installed titles hidden for this session\n", n);
    }
    // At most one, skipped if the NAND already has it.
    if (dsi_title) {
      std::vector<ds::u8> srl, embedded;
      if (!ds::io::read_dsiware(dsi_title, srl, &err, &embedded)) {
        std::fprintf(stderr, "dsi: %s\n", err.c_str());
        return 2;
      }
      const u32 lo = static_cast<u32>(srl[0x230] | (srl[0x231] << 8) | (srl[0x232] << 16) | (srl[0x233] << 24));
      if (ds::io::nand_has_title(nds.dsi_nand, nds.bus.bios7i.get(), lo)) {
        std::fprintf(stderr, "dsi: %.4s: already installed on the NAND\n", reinterpret_cast<const char*>(&srl[0x0C]));
        dsi_title_lo = lo;
      } else {
        std::string beside = dsi_tmd ? std::string(dsi_tmd) : rom_stem(std::string(dsi_title)) + ".tmd";
        const std::string cache =
            pp.saves_dir + "/" + std::string(reinterpret_cast<const char*>(&srl[0x0C]), 4) + ".tmd";
        ds::io::TmdFetch fetch;
        std::vector<std::string> tmd_log;
        const std::vector<ds::u8> tmd = ds::io::find_signed_tmd(srl, embedded, cache, beside, fetch, tmd_log);
        for (const std::string& l : tmd_log) VLOG("dsi: %s\n", l.c_str());
        if (tmd.empty()) {
          for (const std::string& l : tmd_log) std::fprintf(stderr, "dsi: %s\n", l.c_str());
          std::fprintf(
              stderr, "dsi: no signed DSi TMD for %.4s; the launcher cannot start it without one (put it at %s%s)\n",
              reinterpret_cast<const char*>(&srl[0x0C]), beside.c_str(), fetch ? "" : ", or allow the download");
          return 2;
        }
        const ds::io::TitleInstall r = ds::io::nand_install_title(nds.dsi_nand, nds.bus.bios7i.get(), srl, tmd);
        std::fprintf(stderr, "dsi: %.4s: %s\n", reinterpret_cast<const char*>(&srl[0x0C]), r.message.c_str());
        if (r.result == ds::io::TitleInstall::Result::Failed) return 1;
        dsi_title_lo = r.title_lo;
      }
    }
    // Earlier sessions' saves/system sidecar/photos go back in before boot
    // reads the NAND. Not under replay, which must start identically each time.
    // mark_state_base lets a save state carry the NAND's sectors from here.
    nds.dsi_nand.mark_state_base();
    if (!replay) {
      const ds::io::NandPersistReport r = ds::io::nand_import(nds.dsi_nand, nds.bus.bios7i.get(), pp);
      if (r.saves || r.system_files || r.photos)
        std::fprintf(stderr, "dsi: restored %d title saves, %d system files, %d photos\n", r.saves, r.system_files,
                     r.photos);
      for (const std::string& n : r.notes) std::fprintf(stderr, "dsi: %s\n", n.c_str());
    }
    nds.set_dsi(true);
    nds.dsi_nand_boot = true; // reset() builds the DSi machine, setup_direct_boot() boots the NAND
    std::fprintf(stderr, "console: DSi (EXPERIMENTAL)\n");
  }
  nds.reset();
  // Sidecar beside the dump, not written into it. None in DSi mode.
  std::string fw_override = dsi_mode ? std::string() : cfg.str("paths.firmware_override", fw + ".ovr");
  if (!nds.firmware_synthetic && !fw_override.empty()) {
    std::string err;
    if (!nds.load_firmware_override(fw_override, err)) {
      if (err != "cannot open") std::fprintf(stderr, "firmware settings: %s: %s\n", fw_override.c_str(), err.c_str());
    } else if (err.empty()) {
      VLOG("firmware settings: %s\n", fw_override.c_str()); // loaded cleanly
    } else {
      std::fprintf(stderr, "firmware settings: %s -- warning: %s\n", fw_override.c_str(), err.c_str());
    }
  }
  VideoSetup vs;
  if (!parse_video(cfg, vs)) return 2;
  const u32& grid = vs.grid;
  const u8& seam_blend = vs.seam_blend;
  u8& chunky = vs.chunky;
  const u32& chunky_thresh = vs.chunky_thresh;
  bool audio_on = cfg.flag("audio.enabled", true), mic_on = cfg.flag("audio.mic", true);
  const bool jit = cfg.flag("emu.jit", true);
  const bool& dual_window = vs.dual_window;
  using Disp = ds::sdl::Display;
  Disp::Layout& layout = vs.layout;
  std::vector<Disp::Mode>& layout_cycle = vs.layout_cycle;
  ds::prof::set_level(std::getenv("DS_PROFILE"));
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  // Display-engine tier owns the panel: dummy/offscreen video driver keeps
  // window/events/controllers but draws nothing. Decided before SDL_Init.
  auto go_headless = [](const char* tier) {
    if (std::getenv("SDL_VIDEODRIVER")) return;
    const char* pick = nullptr;
    for (const char* want : {"dummy", "offscreen"}) {
      for (int i = 0; i < SDL_GetNumVideoDrivers() && !pick; ++i)
        if (!std::strcmp(SDL_GetVideoDriver(i), want)) pick = want;
      if (pick) break;
    }
    if (pick) {
      setenv("SDL_VIDEODRIVER", pick, 1);
      // A headless window never has focus, and SDL drops joystick events
      // without focus; let them through since the pad is the only input.
      SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    } else
      std::fprintf(stderr, "%s: this SDL2 has no headless video driver; its own driver will also open the panel\n",
                   tier);
  };
  // Where frames go (frontend/video/select.h). The panel-owning sinks are
  // decided here, before SDL_Init, since they point SDL at a headless driver.
  {
    ds::frontend::BootProbe probe;
    probe.sink = cfg.str("video.sink");
    probe.disp = cfg.str("video.disp");
    probe.fbdev = cfg.str("video.fbdev");
    probe.dual_window = dual_window;
    if (const char* vd = std::getenv("SDL_VIDEODRIVER")) probe.sdl_videodriver = vd;
    for (int i = 0; i < SDL_GetNumVideoDrivers(); ++i)
      if (!std::strcmp(SDL_GetVideoDriver(i), "mali")) probe.sdl_has_mali = true;
    probe.display_env = std::getenv("DISPLAY") || std::getenv("WAYLAND_DISPLAY");
    probe.has_dri = ::access("/dev/dri", F_OK) == 0;
    probe.disp_ok = !dual_window && ds::sdl::DispOut::available();
    probe.fb0_ok = !dual_window && ds::sdl::FbdevOut::available();
    const ds::frontend::BootPlan plan = ds::frontend::plan_boot(probe);
    for (const std::string& n : plan.notes) std::fprintf(stderr, "%s\n", n.c_str());
    vs.panel_sink = plan.panel;
    vs.want_sink = plan.want;
    if (plan.unset_videodriver) unsetenv("SDL_VIDEODRIVER");
    if (plan.headless_driver)
      go_headless(plan.panel == ds::frontend::Sink::Disp ? "video.sink disp" : "video.sink fbdev");
  }
  if (vs.panel_sink == ds::frontend::Sink::Disp && chunky != 0 && chunky != 2) {
    std::fprintf(stderr,
                 "chunky %s: the display-engine tier draws chunky cells in the scaler as their mean; using mean\n",
                 cfg.str("video.chunky").c_str());
    chunky = 2;
  }

  // GPU present stage, default on. Display falls back to the scanline scaler
  // without dma-buf import, and GpuPresent::open() declines without Vulkan.
  {
    const std::string g = cfg.str("video.gpu_present", "true");
    vs.gpu_mode = g == "rga"      ? ds::sdl::Display::GpuPresentMode::Rga
                  : g == "vulkan" ? ds::sdl::Display::GpuPresentMode::Vulkan
                                  : ds::sdl::Display::GpuPresentMode::Auto;
    vs.gpu_present = g == "rga" || g == "vulkan" || g == "true" || g == "auto" || g == "on" || g == "1";
  }

  u32 init = SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER;
  if (audio_on || mic_on) init |= SDL_INIT_AUDIO;
  // Prefer SDL's native pipewire backend over Pulse (more daemon/wakeup cost).
  const std::string audio_driver = cfg.str("audio.driver", "pipewire");
  const bool driver_forced = std::getenv("SDL_AUDIODRIVER") != nullptr;
  if ((init & SDL_INIT_AUDIO) && !driver_forced && !audio_driver.empty())
    setenv("SDL_AUDIODRIVER", audio_driver.c_str(), 1);
#ifdef SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS
  // Pad a/b/x/y are positions (south/east/west/north) everywhere: bindings
  // and the menu's pips assume it. SDL2 otherwise uses labels on Nintendo pads.
  SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
#endif
  startup_stage("sdl init");
  if (SDL_Init(init) != 0) {
    if ((init & SDL_INIT_AUDIO) && !driver_forced && !audio_driver.empty()) {
      unsetenv("SDL_AUDIODRIVER");
      if (SDL_Init(init) == 0) goto sdl_ready;
    }
    std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    if (!(init & SDL_INIT_AUDIO) || SDL_Init(init & ~SDL_INIT_AUDIO) != 0) return 1;
    audio_on = mic_on = false; // no audio subsystem: run silent
  }
sdl_ready:

  ds::sdl::Display display;
  ds::sdl::Display display2;                            // dual-window: the lower panel's window
  std::vector<ds::sdl::Input::TouchRoute> touch_routes; // DS_DUAL_SCREENS
  if (dual_window) {
    if (SDL_GetNumVideoDisplays() < 2) {
      std::fprintf(stderr, "--dual-window needs two video displays\n");
      SDL_Quit();
      return 1;
    }
    // Under KMSDRM display 0 is DSI-1, the lower panel; sway orders the other way.
    const char* vd = SDL_GetCurrentVideoDriver();
    const bool kms = vd && !std::strcmp(vd, "KMSDRM");
    vs.upper_display = kms ? 1 : 0;
    vs.lower_display = kms ? 0 : 1;
    if (const char* map = std::getenv("DS_DUAL_SCREENS"); map && *map) {
      std::string err;
      if (!ds::sdl::Input::parse_dual_screens(map, vs.upper_display, vs.lower_display, touch_routes, err)) {
        std::fprintf(stderr, "DS_DUAL_SCREENS: %s\n", err.c_str());
        SDL_Quit();
        return 1;
      }
      const int nd = SDL_GetNumVideoDisplays();
      if (vs.upper_display >= nd || vs.lower_display >= nd || vs.upper_display == vs.lower_display) {
        std::fprintf(stderr, "DS_DUAL_SCREENS: displays %d/%d, %d available\n", vs.upper_display, vs.lower_display, nd);
        SDL_Quit();
        return 1;
      }
    }
  }
  startup_stage("sdl ready");
  if (!open_displays(vs, display, display2)) {
    SDL_Quit();
    return 1;
  }
  startup_stage("displays open");
  // video.gpu3d (DS_GPU3D=1 overrides): the 3D layer on the GPU (Renderer3D::set_gpu).
  // After the displays: the Vulkan device opens the DRM node, and under direct
  // KMS the display must take master first.
  {
    const char* g = std::getenv("DS_GPU3D");
    const bool want = g ? std::atoi(g) != 0 : cfg.flag("video.gpu3d", false);
    nds.gpu3d.renderer().set_aa(
        aa_on(cfg.str("video.aa", "false"))); // before the raster starts: its MSAA is fixed at creation
    if (want) {
      std::string why;
      if (!nds.gpu3d.renderer().set_gpu(true, &why)) {
        std::fprintf(stderr, "RG DS: high-resolution renderer unavailable (%s). Select Native in Graphics.\n",
                     why.c_str());
        SDL_Quit();
        return 1;
      }
      const char* requested = std::getenv("RGDS_VK_SCALE");
      const int scale = requested ? std::clamp(std::atoi(requested), 1, 5) : 1;
      std::fprintf(stderr, "gpu3d: on; RG DS true %dx 3D (%dx%d), 640x480 panel\n", scale, 256 * scale, 192 * scale);
      startup_stage("gpu3d ready");
    }
  }
  // Let fullscreen windows take their final size before the ROM loads (see
  // Display::settle_step); bounded, in case no configure comes.
  {
    static const std::vector<u32> blank(ds::SCREEN_W * ds::SCREEN_H, 0xFF000000u);
    const u32* const bfb[2] = {blank.data(), blank.data()};
    const Uint32 until = SDL_GetTicks() + 1000;
    for (;;) {
      bool done = display.settle_step(bfb);
      if (dual_window) {
        done = display2.settle_step(bfb) && done;
        sync_dual_modes(display, display2);
      }
      if (done || SDL_TICKS_PASSED(SDL_GetTicks(), until)) break;
      SDL_Delay(8);
    }
  }
  // Single-screen layout shows one screen (core skips the other's engine);
  // every other layout, and dual-window, shows both.
  auto apply_visibility = [&] {
    const Disp::Layout& l = display.current_layout();
    const bool single = !dual_window && l.mode == Disp::Mode::Single;
    for (int s = 0; s < 2; ++s) nds.gpu.set_screen_visible(s, !single || s == l.primary);
  };
  apply_visibility();

  ds::sdl::Audio audio;
  startup_stage("settled");
  if (audio_on) audio.open(cfg.flag("audio.native_rate", true));
  startup_stage("audio open");
  audio.set_volume(cfg.num("audio.volume", 100));
  // buffer_size (ms) supersedes latency_frames; the old key is still read
  // when buffer_size is unset.
  auto apply_audio_buffer = [&audio](const std::string& v) {
    if (v.empty() || v == "auto") {
      audio.set_buffer_auto();
      return;
    }
    audio.set_buffer_ms(std::atof(v.c_str()));
  };
  if (const std::string v = cfg.str("audio.buffer_size"); !v.empty()) apply_audio_buffer(v);
  else if (const int frames = cfg.num("audio.latency_frames", 0); frames > 0)
    audio.set_buffer_ms(frames * ds::sdl::Audio::FRAME_MS);
  else audio.set_buffer_auto();
  // Not during a replay: the log carries the mic.
  ds::sdl::MicAlsa mic_alsa;
  // Opened lazily on first AUX read: on the handhelds the capture PCM
  // shares a DAI with SDL's playback stream, and a failed setup can leave
  // playback stuck. `rejected`: don't poke the same codec twice through SDL.
  bool mic_tried = !(mic_on && !replay);
  auto open_mic = [&] {
    if (mic_tried) return;
    mic_tried = true;
    if (!mic_alsa.open(ds::spu::Spu::SAMPLE_RATE, cfg.str("audio.mic_dev").c_str()) && !mic_alsa.rejected())
      audio.open_capture();
  };

  ds::sdl::Input input;
  input.set_touch_routes(std::move(touch_routes));
  input.configure(cfg);
  input.open_controllers();
  {
    const std::string pl = cfg.str("cart.preload", "auto");
    ds::cart::RomSource::set_preload(pl == "true" || pl == "on" || pl == "1"     ? ds::cart::RomSource::Preload::On
                                     : pl == "false" || pl == "off" || pl == "0" ? ds::cart::RomSource::Preload::Off
                                                                                 : ds::cart::RomSource::Preload::Auto);
  }
  nds.rom_cache_dir = cfg.str("paths.cache", "");
  nds.rom_cache_max_bytes = static_cast<u64>(std::max(0, cfg.num("cart.cache_mb", 2048))) << 20;
  const bool cache_session = cfg.str("cart.cache", "keep") == "session";
  // Every unpacked image goes except the one this launch is about to use.
  if (clear_cache) {
    std::string keep;
    {
      std::ifstream f(rom_path, std::ios::binary);
      u8 magic[4] = {};
      f.read(reinterpret_cast<char*>(magic), 4);
      if (f.gcount() == 4 && ds::cart::is_zip(magic, 4)) keep = ds::cart::zip_cache_path(rom_path);
    }
    u64 freed = 0;
    std::vector<std::string> dirs = {ds::cart::zip_cache_dir(rom_dir_of(rom_path))};
    if (!nds.rom_cache_dir.empty()) dirs.push_back(nds.rom_cache_dir);
    if (!cfg.str("paths.games").empty()) dirs.push_back(ds::cart::zip_cache_dir(cfg.str("paths.games")));
    for (const std::string& d : dirs) freed += ds::cart::clear_cache(d, keep);
    std::fprintf(stderr, "cache: cleared %llu MB of unpacked games\n", static_cast<unsigned long long>(freed >> 20));
  }
  auto set_scale_targets = [&](const ds::sdl::Display::Target target[2], bool scaled) {
    const bool at_source = display.effects_at_source();
    for (int i = 0; i < 2; ++i) {
      if (!scaled) {
        nds.gpu.set_scale_target(i, ds::gpu::Gpu::ScaleTarget{});
        continue;
      }
      ds::gpu::Gpu::ScaleTarget st{target[i].px,
                                   target[i].pitch,
                                   target[i].h,
                                   target[i].xrun,
                                   at_source || !target[i].grid ? 256u : grid,
                                   display.chunky_on(i) ? chunky : static_cast<u8>(0),
                                   chunky_thresh,
                                   at_source ? static_cast<u8>(0) : seam_blend,
                                   target[i].seam_w,
                                   static_cast<const ds::gpu::Gpu::CellMap*>(
                                       (dual_window && display2.only_screen() == i ? display2 : display).cell_map(i)),
                                   vs.linear && !at_source,
                                   target[i].lin_sx,
                                   target[i].lin_wx};
      st.y_lo = target[i].y_lo;
      st.y_hi = target[i].y_hi; // crop window (integer overscale)
      nds.gpu.set_scale_target(i, st);
    }
  };
  // RGDS prepares archives and owns launch progress. Keep loading cancellable
  // without creating a second UI or publishing partially loaded frames.
  auto load_rom_cancellable = [&](const std::string& path) -> bool {
    std::atomic<bool> cancel{false}, done{false};
    bool ok = false;
    nds.rom_cancel = &cancel;
    std::thread worker([&] {
      ok = nds.load_rom(path);
      done = true;
    });
    while (!done) {
      SDL_Event e;
      while (SDL_PollEvent(&e)) input.handle(e, display, dual_window ? &display2 : nullptr);
      if (input.quit() || g_signalled) cancel = true;
      SDL_Delay(5);
    }
    worker.join();
    nds.rom_cancel = nullptr;
    return ok;
  };
  auto discard_session_cache = [&] {
    if (cache_session && !nds.rom_cache_path.empty()) {
      ds::cart::remove_cached(nds.rom_cache_path);
      nds.rom_cache_path.clear();
    }
  };
  // The open SDL display supplies cancellation events while the cartridge loads.
  const bool rom_is_cia = is_cia_container(rom_path);
  auto load_shortcut_title = [&](const std::string& path) -> bool {
    ds::io::NandShortcut sc;
    std::string err;
    if (!ds::io::read_shortcut(path, sc)) {
      std::fprintf(stderr, "dsi: %s is not a NAND title shortcut\n", path.c_str());
      return false;
    }
    if (!sc.from(nds.dsi_nand)) {
      std::fprintf(stderr, "dsi: %s was made from another NAND than %s\n", path.c_str(), shortcut_nand.c_str());
      return false;
    }
    if (!nds.load_dsi_nand_title(sc.title_lo, &err)) {
      std::fprintf(stderr, "dsi: %s: %s\n", path.c_str(), err.c_str());
      return false;
    }
    return true;
  };
  if (dsi_hle && rom_shortcut) {
    if (!load_shortcut_title(rom_path)) {
      SDL_Quit();
      return 1;
    }
  } else if (dsi_hle && rom_is_cia) {
    std::vector<ds::u8> srl;
    std::string err;
    if (!ds::io::read_dsiware(rom_path, srl, &err) || !nds.load_rom_image(std::move(srl))) {
      std::fprintf(stderr, "dsi: %s: %s\n", rom_path.c_str(), err.c_str());
      SDL_Quit();
      return 1;
    }
  } else if (!boot_firmware && !load_rom_cancellable(rom_path)) {
    std::fprintf(stderr, "could not read %s\n", rom_path.c_str());
    SDL_Quit();
    return 1;
  }
  startup_stage("rom loaded");
  auto prepare_dsiware = [&]() -> bool {
    std::string err;
    std::vector<std::string> made;
    nds.dsi_font_path = dsi_font ? std::string(dsi_font) : cfg.str("paths.dsi_font");
    if (!nds.dsi_font_path.empty() && !std::filesystem::exists(nds.dsi_font_path)) {
      std::fprintf(stderr, "dsi: font %s not found; using RG DS Core's own\n", nds.dsi_font_path.c_str());
      nds.dsi_font_path.clear();
    }
    if (!nds.prepare_dsi_hle(user, &err, &made)) {
      std::fprintf(stderr, "dsi: %s\n", err.c_str());
      return false;
    }
    for (const std::string& m : made) std::fprintf(stderr, "dsi: %s\n", m.c_str());
    return true;
  };
  if (dsi_hle && !prepare_dsiware()) {
    SDL_Quit();
    return 1;
  }
  // Firmware itself remains guest software; RGDS supplies the host library.
  if (boot_firmware && std::filesystem::exists(rom_path)) nds.load_rom(rom_path);
  // Merge again now the game code is known (first time for a zip).
  if (nds.cart) {
    merge_game_config(nds.cart->header().game_code);
    VLOG("game: %.12s [%.4s]\n", nds.cart->header().game_title, nds.cart->header().game_code);
    if (!nds.rom_zip_entry.empty()) VLOG("zip: %s\n", nds.rom_zip_entry.c_str());
  }
  Session session;
  session.open(nds, cfg, rom_path, save_arg);

  cfg.set("video.aa", aa_on(cfg.str("video.aa", "false")) ? "true" : "false");
  apply_aa(nds, aa_on(cfg.str("video.aa", "false")));
  if (!boot_firmware || nds.dsi) nds.setup_direct_boot(); // on a DSi this is the NAND boot
  if (nds.dsi && dsi_title_lo && !dsi_menu) nds.dsi_autoload(dsi_title_lo);
  // Off in the core by default for reproducibility. Not under --replay: a
  // game reading the date would replay differently each time.
  if (!replay || rtc_host) nds.io.start_rtc_clock();
  else VLOG("rtc: frozen for the replay\n");
  if (replay && rtc_host) std::fprintf(stderr, "rtc: --rtc-host over a replay; this run is not reproducible\n");
#if DSPERATE_JIT
  if (jit && !ds::jit::attach(nds, true, true)) return 1;
  // The DS menu boots under per-instruction budget checks: block-granularity
  // overshoot can intermittently stop it booting. Dropped once a game
  // launches; not the DSi Menu, which runs fine without it.
  if (jit && boot_firmware && !nds.dsi) {
    ds::jit::set_strict(true);
    VLOG("jit: strict timing for the firmware\n");
  }
#else
  (void)jit;
#endif
  // A replay must start from the same battery save every time, so it's loaded read-only.
  load_save(nds, session.sav);
  const bool save_readonly = replay != nullptr;
  if (save_readonly) VLOG("save: read-only for the replay\n");

  ds::input::Log log;
  if (record && replay) {
    std::fprintf(stderr, "--record and --replay are exclusive\n");
    return 2;
  }
  if (record && !log.open_write(record)) {
    std::fprintf(stderr, "cannot write %s\n", record);
    return 1;
  }
  if (replay) {
    if (!log.open_read(replay)) {
      std::fprintf(stderr, "cannot read %s\n", replay);
      return 1;
    }
    VLOG("replay: %u frames from %s\n", log.frames(), replay);
  }
  // After the battery save (state's SRAM wins) and after the replay log
  // opens (can wind forward to the state's frame). Skipped under
  // replay/recording, since the log runs inputs from boot.
  const std::string autoload = load_state || log.reading() || log.writing()
                                   ? std::string()
                                   : autoload_path(nds, session.states_dir, cfg.flag("emu.autoload", false));
  if (load_state || !autoload.empty()) {
    const char* start_state = load_state ? load_state : autoload.c_str();
    bool got_layout = false;
    // Unreadable --load-state fails the run; an unreadable auto state just boots normally.
    if (!load_state_file(nds, start_state, layout, got_layout)) {
      if (load_state) return 1;
    } else {
      startup_stage("state loaded");
      if (!load_state) std::fprintf(stderr, "state: autoloaded %s\n", start_state);
      // The state's view replaces the config's, dual-window aside.
      if (got_layout && !dual_window) {
        display.set_layout(layout);
        apply_visibility();
      }
      if (log.reading()) {
        ds::input::Frame f;
        for (u64 k = 0; k < nds.frame_count && log.read(f); ++k) {}
      }
    }
  }
  ds::sdl::Lid lid;
  if (!replay) lid.open();
  std::vector<s16> mic, mic_raw, mic_queue;
  // ADC sits well off zero; one-pole DC blocker, ~25 Hz at 32768 Hz.
  // audio.mic_gain scales what remains (default 0.25).
  double dc = 0.0;
  // Noise gate: floor is the slowest-rising rms seen; a frame under
  // audio.mic_gate times it (default 5, 0 = off) is sent as silence.
  const double mic_gate = cfg.real("audio.mic_gate", 5.0);
  double mic_floor = 1e9;
  const double mic_gain = cfg.real("audio.mic_gain", 0.25);
  const size_t mic_per_frame = ds::spu::Spu::SAMPLE_RATE * ds::CYCLES_PER_FRAME / ds::ARM9_CLOCK_HZ; // 547

  // Wall-clock pacing when there is no audio queue to pace against.
  const double frame_ns = 1e9 * ds::CYCLES_PER_FRAME / ds::ARM9_CLOCK_HZ;
  ds::sdl::Pacer pacer(frame_ns);
  // emu.pacing: a polling governor reads "sleep" as idle and clocks down
  // under us; "busy" holds the core to the deadline instead (idle power
  // cost, read-only on sysfs). "auto" (default) is "busy" exactly when a
  // polling governor is in charge.
  {
    const std::string pacing = cfg.str("emu.pacing", "auto");
    std::string gov;
    const bool polls = ds::sdl::host_governor_polls(&gov);
    const bool busy = pacing == "busy" || (pacing != "sleep" && pacing != "off" && polls);
    pacer.set_busy_wait(busy);
    if (busy)
      VLOG("pacing: busy-wait to the deadline (governor %s%s)\n", polls ? gov.c_str() : ds::sdl::cpu_governor().c_str(),
           pacing == "busy" ? ", asked for" : ": it decides by polling load");
    else VLOG("pacing: sleep to the deadline (governor %s)\n", ds::sdl::cpu_governor().c_str());
  }
  // emu.rt_relief (default on): stay under the kernel's RT bandwidth cap
  // (sched_rt_runtime_us/period_us). The kernel's lump reclaim would show as
  // a periodic hitch; this gives it back a frame at a time instead.
  if (cfg.flag("emu.rt_relief", true)) {
    const auto sysctl_num = [](const char* path) -> double {
      std::FILE* f = std::fopen(path, "r");
      if (!f) return 0.0;
      double v = 0.0;
      if (std::fscanf(f, "%lf", &v) != 1) v = 0.0;
      std::fclose(f);
      return v;
    };
    const double runtime_us = sysctl_num("/proc/sys/kernel/sched_rt_runtime_us");
    const double period_us = sysctl_num("/proc/sys/kernel/sched_rt_period_us");
    int policy = 0;
    sched_param sp{};
    const bool rt =
        pthread_getschedparam(pthread_self(), &policy, &sp) == 0 && (policy == SCHED_RR || policy == SCHED_FIFO);
    if (!rt || std::getenv("DS_EMU_OTHER")) VLOG("rt relief: not needed, this thread is not real-time\n");
    else if (runtime_us < 0 || period_us <= 0) VLOG("rt relief: not needed, the real-time class is uncapped\n");
    else if (runtime_us >= period_us)
      VLOG("rt relief: not needed, the cap is not a cap (%.0f of %.0f us)\n", runtime_us, period_us);
    else {
      pacer.set_rt_relief(runtime_us / period_us, period_us * 1000.0);
      VLOG(
          "rt relief: the real-time class is capped at %.1f %% of every %.0f ms; giving the rest back a frame at a time\n",
          100.0 * runtime_us / period_us, period_us / 1000.0);
    }
  }
  // "auto" (default): the console's own 59.8261 Hz. 60 is what a player
  // means by "60 fps" (slightly fast; absorbed by rate control). "off" still
  // presents/plays sound, just doesn't pace. emu.speed multiplies the result.
  int speed_pct = cfg.num("emu.speed", 100);
  std::string limiter_mode = cfg.str("emu.limiter", "auto");
  bool limiter_off = false;
  // Limiter, speed percent and fast forward all write this one scale factor.
  double base_scale = 1.0;
  double frame_budget_ms = frame_ns / 1e6; // one frame at the rate now in force; adaptive frameskip's budget
  auto apply_limiter = [&] {
    // Anything unrecognised falls back to the console's own rate.
    double hz = 0.0;
    if (limiter_mode == "off" || limiter_mode == "unlimited") {
      limiter_off = true;
    } else {
      limiter_off = false;
      const double parsed = std::atof(limiter_mode.c_str());
      hz = parsed >= 1.0 && parsed <= 1000.0 ? parsed : 0.0;
    }
    const double period_ns = hz > 0.0 ? 1e9 / hz : frame_ns;
    speed_pct = speed_pct > 0 ? std::clamp(speed_pct, 25, 400) : 100;
    const double scale = speed_pct / 100.0;
    base_scale = frame_ns / period_ns * scale;
    frame_budget_ms = period_ns / 1e6 / scale;
    // Unlimited is the exception: audio keeps the console's rate and the
    // drop path deals with the surplus, as for fast forward.
    audio.set_speed(limiter_off ? 1.0 : base_scale);
    pacer.reset();
  };
  apply_limiter();

  // video.pace (DS_PACE overrides): free | display | auto. "display" (KMSDRM
  // only, where the page flips are tracked) phase-locks the pacer to the
  // panel: the emulator runs one frame per refresh at the panel's own rate
  // and each frame starts at its vblank minus the learned start-to-swap time
  // and a margin (phase_lock.h), instead of its swap waiting 0..1 refresh at
  // the beat between the limiter's clock and the panel's. The audio
  // resampler takes the rate difference. Only at real-time speed: fast
  // forward, emu.speed and an unlimited or far-off limiter pace freely as
  // before. "auto" is "free" for now: on the RG DS (18-19 Oct 2026) the lock
  // cut the wait for a flip by 2-4 ms mean in games with headroom but cost
  // repeated frames, since the present's own jitter wants a margin nearly
  // as wide as the gain (docs/PERFORMANCE.md). DS_PACE_BAND=<ms> sets the
  // starting (and smallest) margin.
  ds::sdl::PhaseLock::Params lock_params;
  // The direct KMS tier (display_drm.cpp) is a mailbox: a frame held for the previous flip is dropped there, not shown a tick later.
  if (const char* b = std::getenv("DS_PACE_BAND"); b && std::atof(b) > 0.0) {
    lock_params.band_ns = std::atof(b) * 1e6;
    lock_params.band_min_ns = std::min(lock_params.band_ns, lock_params.band_min_ns);
    lock_params.band_max_ns = std::max(lock_params.band_ns, lock_params.band_max_ns);
  }
  ds::sdl::PhaseLock lock(lock_params);
  const bool pace_display = [&] {
    const char* e = std::getenv("DS_PACE");
    std::string mode = e && *e ? e : cfg.str("video.pace", "auto");
    const char* vd = SDL_GetCurrentVideoDriver();
    const bool kms = vd && !std::strcmp(vd, "KMSDRM");
    const bool tracked = display.flips() || dynamic_cast<ds::sdl::DrmOut*>(display.scanout());
    if (mode == "display") {
      if (!tracked) {
        std::fprintf(stderr,
                     "pace: display asked for, but this window's page flips are not tracked (%s); pacing freely\n",
                     vd ? vd : "?");
        return false;
      }
      return true;
    }
    if (mode != "free" && mode != "auto" && mode != "off")
      std::fprintf(stderr, "pace: unknown mode %s; auto\n", mode.c_str());
    (void)kms;
    return false; // auto: free, until the lock measures better than the free pacer on the RG DS
  }();
  VLOG("pace: %s\n", pace_display ? "locked to the display's page flips once their period holds"
                                  : "free-running at the limiter's rate");
  bool pace_locked = false; // this frame was paced at the display's period
  u64 pace_locked_frames = 0, pace_frames = 0;
  double pace_err_abs = 0.0; // sum of |lat - band| over locked frames with a sample, ns
  u64 pace_err_n = 0;
  u64 flip_seen[2] = {0, 0};     // newest flip number taken from each window
  bool frame_late_start = false; // this frame began past its deadline
  const auto mono_ns = [] {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return static_cast<u64>(t.tv_sec) * 1'000'000'000ull + static_cast<u64>(t.tv_nsec);
  };
  const double ticks_per_ns = static_cast<double>(SDL_GetPerformanceFrequency()) / 1e9;
  u64 frame_start_ns = mono_ns(); // when the wait for this frame ended (the input poll follows)

  const bool show_fps = std::getenv("DS_FPS") != nullptr;
  Uint64 fps_mark = SDL_GetPerformanceCounter();
  Uint64 emu_ticks = 0, draw_ticks = 0, wait_ticks = 0,
         fps_pace_ticks = 0; // wait: blocked in begin_frame for a free scanout buffer
  u64 frames = 0;
  // Only run_frame()'s slice goes in: present/vsync blocking would peg
  // every frame at the refresh interval and hide the clusters this finds.
  const double ticks_to_ms = 1e3 / static_cast<double>(SDL_GetPerformanceFrequency());
  std::vector<double> frame_ms, work_ms; // emu slice; emu + present slice
  if (frame_limit > 0) {
    frame_ms.reserve(static_cast<size_t>(frame_limit));
    work_ms.reserve(static_cast<size_t>(frame_limit));
  }
  Uint64 pace_ticks = 0, draw_ticks_total = 0;
  // Set once the transport has actually started (not from the flags). While
  // live, gates save states, fast forward, frameskip, speed knobs, and pause.
  bool net_live = false;
  int state_slot = 0;
  // Fast forward: ff_speed floors the limiter (0 = uncapped); ff_skip
  // presents one frame in ff_skip+1, every frame still emulated exactly.
  // Frameskip: emu.frameskip is the limit N. "fixed" always skips that
  // pattern; "adaptive" (default) skips only while behind real time (a debt
  // in ms, paid down by what a skipped frame saves).
  //
  // A skipped frame leaves out line rendering, 3D raster and scaling; the
  // decision reaches the core one frame early since 3D raster for a frame
  // runs during the frame before it (a display-capturing frame is never skipped).
  int fs_limit = cfg.num("emu.frameskip", 0); // the menu can change these
  bool fs_adaptive = cfg.str("emu.frameskip_mode", "adaptive") != "fixed";
  const bool fs_capture = cfg.flag("emu.frameskip_capture", true);
  nds.gpu.set_frameskip_capture(fs_capture);
  u64 fs_refused = 0; // skips the core would not take (capture / display FIFO)
  // Skipping runs in blocks of a whole display period: a game that renders
  // one screen per frame and swaps them needs every phase of a period
  // drawn, or the two screens look like they're swapping (stale vs fresh).
  int fs_left = 0;         // frames left in the current block
  bool fs_in_skip = false; // that block is a skip block
  int fs_blocks = 0;       // skip blocks run back to back, against the limit
  bool fs_period_warned = false;
  int fs_drawn_run = 0;  // drawn frames since the last skipped one
  u64 fs_skipped = 0;    // reported with the frame statistics
  double fs_debt_ms = 0; // adaptive: how far behind real time we are
  // One state to the unlisted ".auto" slot when the session ends, so a
  // kill/Ctrl-C can be resumed via --load-state. Nothing written while playing.
  bool autosave = cfg.flag("emu.autosave", false);
  // "true": <GAMECODE>.auto.png beside the state; any other value is the
  // file to write. Only taken when the state is written.
  const std::string autosave_png_cfg = cfg.str("emu.autosave_png", "false");
  const bool autosave_png = autosave_png_cfg != "false" && autosave_png_cfg != "0" && !autosave_png_cfg.empty();
  bool ff_toggle = cfg.flag("emu.fast_forward", false);
  int ff_speed = cfg.num("emu.ff_speed", 0), ff_skip = cfg.num("emu.ff_skip", 3);
  bool was_fast = false, was_ff_raster = false;
  // Keep an A/B switch for device profiling; this changes host rendering only.
  const bool ff_raster_enabled = !std::getenv("DS_FF_RASTER") || std::atoi(std::getenv("DS_FF_RASTER")) != 0;
  std::vector<u32> cursor_enhanced_fb;
  std::vector<u32> cursor_fb(ds::SCREEN_W * ds::SCREEN_H); // bottom screen with the pen crosshair
  // Status UI belongs to RG DS; state hotkeys retain diagnostic logging.
  auto show_slot = [](const std::string& text) { std::fprintf(stderr, "state: %s\n", text.c_str()); };
  int pip_touch_hold = std::max(0, cfg.num("video.pip_touch_hold", 60));
  constexpr int PIP_FADE_STEP = 24; // ~10 frames rest to opaque
  int pip_alpha = static_cast<int>(layout.pip_alpha * 255.0 + 0.5), pip_hold = 0;
  session.load_enabled(nds);
  // DSI NAND LINKS: a .dspr.nds shortcut file per DSiWare title on
  // paths.dsi_nand while on, none while off. Refreshed at every start and
  // when the row is toggled. Reads its own NAND copy; session's untouched.
  auto sync_nand_shortcuts = [&](bool on) -> std::string {
    // A folder paths.dsi_games has since moved away from keeps its
    // shortcuts; only the currently-named folder is updated or cleared.
    const std::string dir = shortcuts_dir(cfg);
    if (dir.empty()) return "NO SHORTCUT FOLDER";
    ds::io::NandImage nand;
    std::vector<ds::u8> b7;
    if (on) {
      if (shortcut_nand.empty() || !std::filesystem::exists(shortcut_nand)) return "NO DSI NAND (PATHS.DSI_NAND)";
      if (!have_dsi_bios()) return "NO DSI BIOS";
      if (!nand.open(shortcut_nand)) return "THE DSI NAND CANNOT BE READ";
      std::ifstream f(dsi_bios7i, std::ios::binary);
      b7.assign(std::istreambuf_iterator<char>(f), {});
      if (b7.size() < 0x10000) return "THE DSI BIOS CANNOT BE READ";
    }
    const ds::io::ShortcutSync r = ds::io::sync_shortcuts(dir, on ? &nand : nullptr, on ? b7.data() : nullptr, on);
    if (r.written || r.removed)
      std::fprintf(stderr, "dsi shortcuts: %d written, %d removed in %s\n", r.written, r.removed, dir.c_str());
    for (const std::string& n : r.notes) std::fprintf(stderr, "dsi shortcuts: %s\n", n.c_str());
    return std::to_string(r.written) + " ADDED, " + std::to_string(r.removed) + " REMOVED";
  };
  if (cfg.flag("emu.dsi_nand_shortcuts", false)) sync_nand_shortcuts(true);
  // Same deferral, opposite reason: a screenshot wants the unadorned
  // picture, taken from fb_ after one deliberately unscaled frame.
  bool shot_pending = false;
  u32 sram_writes_seen = nds.cart ? nds.cart->sram_writes() : 0;
  u64 sram_quiet_since = 0;
  // DSi equivalent: NAND writes go out as title saves/system sidecar/photos
  // once quiet for two seconds. Without a NAND, no system sidecar (DSi
  // settings remade from [user] each start).
  auto hle_paths = [&](const std::string& game) {
    ds::io::NandPersistPaths p;
    p.saves_dir = cfg.str("paths.saves");
    if (p.saves_dir.empty()) {
      const std::filesystem::path r(game);
      p.saves_dir = r.has_parent_path() ? r.parent_path().string() : ".";
    }
    p.photos_dir = p.saves_dir + "/dsi-photos";
    return p;
  };
  ds::io::NandPersistPaths dsi_paths = rom_shortcut
                                           ? ds::io::NandPersistPaths::beside(shortcut_nand, cfg.str("paths.saves"))
                                       : dsi_hle  ? hle_paths(rom_path)
                                       : dsi_mode ? ds::io::NandPersistPaths::beside(dsi_nand, cfg.str("paths.saves"))
                                                  : ds::io::NandPersistPaths{};
  // Earlier saves go back into the made-up NAND; that becomes the export baseline.
  auto import_dsiware_saves = [&] {
    if (replay) return;
    const ds::io::NandPersistReport r = ds::io::nand_import(nds.dsi_nand, nds.bus.bios7i.get(), dsi_paths);
    nds.dsi_nand.mark_baseline();
    if (r.saves || r.photos) std::fprintf(stderr, "dsi: restored %d title saves, %d photos\n", r.saves, r.photos);
    for (const std::string& n : r.notes) std::fprintf(stderr, "dsi: %s\n", n.c_str());
  };
  if (dsi_hle) import_dsiware_saves();
  u64 nand_writes_seen = nds.dsi_nand.writes, nand_quiet_since = 0, nand_exported = nds.dsi_nand.writes;
  auto flush_dsi = [&] {
    if (!dsi_mode || save_readonly || nds.dsi_nand.writes == nand_exported) return;
    nand_exported = nds.dsi_nand.writes;
    const ds::io::NandPersistReport r = ds::io::nand_export(nds.dsi_nand, nds.bus.bios7i.get(), dsi_paths);
    if (r.saves || r.system_files || r.photos)
      VLOG("dsi: saved %d title saves, %d system files, %d photos\n", r.saves, r.system_files, r.photos);
    for (const std::string& n : r.notes) std::fprintf(stderr, "dsi: %s\n", n.c_str());
  };
  // SD card changes go back to its folder once writes are quiet for two seconds.
  u64 sd_writes_seen = nds.dsi_sd.writes, sd_quiet_since = 0, sd_synced = nds.dsi_sd.writes;
  auto flush_sd = [&] {
    if (save_readonly || !nds.dsi_sd.valid() || nds.dsi_sd.writes == sd_synced) return;
    sd_synced = nds.dsi_sd.writes;
    const ds::io::SdCard::Report r = nds.dsi_sd.sync();
    if (r.files || r.dirs || r.removed)
      VLOG("dsi sd: %d files written, %d folders made, %d removed in %s\n", r.files, r.dirs, r.removed,
           nds.dsi_sd.folder().c_str());
    for (const std::string& n : r.notes) std::fprintf(stderr, "dsi sd: %s\n", n.c_str());
  };
  auto flush_save = [&] {
    if (!save_readonly && nds.cart && nds.cart->sram_dirty()) write_save(nds, session.sav);
    flush_dsi();
    flush_sd();
  };
  // Whether fb_ holds the last run frame's picture (true after an unscaled
  // frame; false after a scanline-tier frame written straight to the panel).
  bool fb_current = false;
  auto autosave_now = [&] {
    // net_live: quitting would otherwise leave a state resuming a dead conversation.
    if (!autosave || save_readonly || log.writing() || net_live) return;
    // Thumbnail comes from fb_; run one more unscaled frame if stale.
    if (autosave_png && !fb_current) {
      ds::sdl::Display::Target none[2] = {};
      set_scale_targets(none, false);
      nds.run_frame();
      fb_current = true;
    }
    if (!save_state_file(nds, auto_state_path(nds, session.states_dir), display.current_layout())) return;
    flush_save(); // the .sav and the state never diverge
    if (!autosave_png) return;
    const bool beside = autosave_png_cfg == "true" || autosave_png_cfg == "1";
    std::string png = autosave_png_cfg;
    if (beside) {
      png = auto_state_path(nds, session.states_dir);
      png.replace(png.size() - 3, 3, "png");
    }
    if (write_png(nds, png, display.current_layout())) std::fprintf(stderr, "state: thumbnail %s\n", png.c_str());
  };
  Disp::Layout loaded_layout = layout;
  bool got_layout = false; // from `layout`: a state carries no pip_alpha, the config's stays
  auto apply_loaded_layout = [&] {
    if (!got_layout || dual_window) return;
    loaded_layout.gap = display.current_layout().gap;
    loaded_layout.gap_auto =
        display.current_layout().gap_auto; // not in a state either, and the menu may have moved it since
    display.set_layout(loaded_layout);
    apply_visibility();
  };
  auto log_sd_note = [&] {
    if (!nds.sd_card_note.empty()) std::fprintf(stderr, "DS card: %s\n", nds.sd_card_note.c_str());
  };
  log_sd_note();
  bool dsp_warned = false;
  // The emulation thread takes its core; the audio, SDL and driver threads
  // started so far go to the aux core, and once more after the first frames
  // for the ones started lazily.
  ds::place_current_thread(ds::ThreadRole::Emu);
  ds::place_foreign_threads();
  rgds::graphics_handler = [&](int scale, bool smooth, int skip) {
    std::string why;
    if (!nds.gpu.set_presentation(scale, smooth, &why)) {
      std::fprintf(stderr, "RG DS graphics: keeping current renderer: %s\n", why.c_str());
      return false;
    }
    fs_limit = skip;
    fs_left = fs_blocks = fs_drawn_run = 0;
    fs_in_skip = false;
    fs_debt_ms = 0;
    nds.gpu.set_frame_skip(false);
    const auto value = std::to_string(scale);
    setenv("RGDS_VK_SCALE", value.c_str(), 1);
    setenv("RGDS_3D_SMOOTH", smooth ? "1" : "0", 1);
    pacer.reset();
    std::fprintf(stderr, "RG DS graphics applied: %dx, smoothing %d, frameskip %d\n", scale, smooth, skip);
    return true;
  };
  rgds::filter2d_handler = [&](int mode) {
    nds.gpu.set_filter2d(mode);
    setenv("RGDS_2D_FILTER", std::to_string(mode).c_str(), 1);
    std::fprintf(stderr, "RG DS 2D filter applied: %d\n", mode);
    return true;
  };
  rgds::adaptive_handler = [&](int on) {
    g_rgds_adaptive.store(on, std::memory_order_relaxed);
    setenv("DS_ADAPTIVE", on ? "1" : "0", 1);
    std::fprintf(stderr, "RG DS heavy 3D scenes: %s\n", on ? "adaptive" : "always sharp");
    return true;
  };
  // RG DS game menu cheats: the codes Session::open loaded for this cart,
  // toggled between frames on this thread (the ARM7 VBlank runs them), so
  // the engine never sees a half-applied change. Which codes are on is saved
  // beside the states at once, the way the engine's own hotkeys did.
  rgds::cheat_count_handler = [&] { return int(nds.cheats.codes.size()); };
  rgds::cheat_group_count_handler = [&] { return int(session.cheats.groups.size()); };
  rgds::cheat_entry_handler = [&](int index, rgds::CheatEntry& entry) {
    if (index < 0 || size_t(index) >= nds.cheats.codes.size()) return false;
    const ds::cheat::Code& code = nds.cheats.codes[size_t(index)];
    entry.name = code.name.c_str();
    entry.description = code.description.c_str();
    entry.group = code.group;
    entry.state = code.is_note() ? -1 : code.enabled ? 1 : 0;
    return true;
  };
  rgds::cheat_group_handler = [&](int index, rgds::CheatGroup& group) {
    if (index < 0 || size_t(index) >= session.cheats.groups.size()) return false;
    const ds::cheat::Group& source = session.cheats.groups[size_t(index)];
    group.name = source.name.c_str();
    group.exclusive = source.exclusive ? 1 : 0;
    return true;
  };
  rgds::cheat_set_handler = [&](int index, bool on) {
    if (index < 0 || size_t(index) >= nds.cheats.codes.size()) return false;
    ds::cheat::Code& code = nds.cheats.codes[size_t(index)];
    if (code.is_note()) return false;
    // An exclusive group (the database's flag) keeps one code on at a time.
    if (on && code.group >= 0 && size_t(code.group) < session.cheats.groups.size() &&
        session.cheats.groups[size_t(code.group)].exclusive)
      for (ds::cheat::Code& other : nds.cheats.codes)
        if (&other != &code && other.group == code.group) other.enabled = false;
    code.enabled = on;
    session.save_enabled(nds);
    std::fprintf(stderr, "RG DS cheat %s: %s\n", on ? "on" : "off", code.name.c_str());
    return true;
  };
  // RG DS live menu (rgds::menu_hold): no frames run while it is up, and the
  // last presented picture goes out again (renderer tier) so the bridge can
  // draw the menu over it. Events still flow, so its save/load/quit work.
  struct HeldFrame {
    const u32* src[2] = {}; // what the last renderer present drew
    u32 w = 0, h = 0;
    bool enhanced = false, flags[2] = {};
    std::vector<u32> px[2]; // copied once the hold starts
  } held;
  bool holding = false;
  Uint32 held_present_at = 0, held_sequence = 0, held_presents = 0;
  const auto remember_present = [&](const u32* const src[2], u32 w, u32 h, const bool* flags) {
    held.src[0] = src[0];
    held.src[1] = src[1];
    held.w = w;
    held.h = h;
    held.enhanced = flags != nullptr;
    for (int s = 0; s < 2; ++s) held.flags[s] = !flags || flags[s];
  };
  const auto present_held = [&] {
    if (!held.src[0] || display.disp()) return false;
    const u32* px[2] = {held.px[0].data(), held.px[1].data()};
    const bool* flags = held.enhanced ? held.flags : nullptr;
    ds::sdl::Display* const windows[2] = {&display, &display2};
    // On a scanout tier the held picture takes the direct present, where the
    // bridge's hook blends the menu over it (Display::draw_scanout_async).
    if (display.scanout_direct() && (!dual_window || display2.scanout_direct()))
      return ds::sdl::Display::draw_scanout_async(windows, dual_window ? 2 : 1, px, held.w, held.h, flags);
    if (!display.enhanced_present() || display.gpu_present()) return false;
    static const bool async = !std::getenv("DS_PRESENT_SYNC") &&
                              !(std::getenv("DS_RENDERER_SYNC") && std::atoi(std::getenv("DS_RENDERER_SYNC")) != 0);
    if (!async || !ds::sdl::Display::draw_renderer_async(windows, dual_window ? 2 : 1, px, held.w, held.h, flags)) {
      display.draw(px, held.w, held.h, flags);
      if (dual_window) display2.draw(px, held.w, held.h, flags);
    }
    return true;
  };
  while (!input.quit() && !g_signalled && (frame_limit == 0 || frames < static_cast<u64>(frame_limit))) {
    if (frames == 120) ds::place_foreign_threads();
    SDL_Event e;
    input.set_menu_open(false);
    while (SDL_PollEvent(&e)) input.handle(e, display, dual_window ? &display2 : nullptr);
    for (ds::sdl::Action a : input.take_actions()) {
      using A = ds::sdl::Action;
      switch (a) {
      case A::Pause: rgds::request_menu(); break;
      case A::Fullscreen:
        display.toggle_fullscreen();
        if (dual_window) display2.toggle_fullscreen();
        break;
      case A::LayoutNext:
      case A::LayoutPrev: {
        if (dual_window) break;
        Disp::Layout l = display.current_layout();
        const int n = static_cast<int>(layout_cycle.size());
        int at = 0;
        for (int i = 0; i < n; ++i)
          if (layout_cycle[static_cast<size_t>(i)] == l.mode) {
            at = a == A::LayoutNext ? (i + 1) % n : (i + n - 1) % n;
            break;
          }
        l.mode = layout_cycle[static_cast<size_t>(at)];
        display.set_layout(l);
        apply_visibility();

        VLOG("layout: %s\n", Disp::mode_name(l.mode));
        if (!session.game_ini.empty())
          ds::sdl::Config::store(session.game_ini, "video.layout", Disp::mode_name(l.mode));
        break;
      }
      case A::ScreenSwap: {
        if (dual_window) {
          vs.layout.primary = 1 - vs.layout.primary;
          display.set_only_screen(vs.layout.primary);
          display2.set_only_screen(1 - vs.layout.primary);

          if (!session.game_ini.empty())
            ds::sdl::Config::store(session.game_ini, "video.screen", vs.layout.primary ? "bottom" : "top");
          break;
        }
        Disp::Layout l = display.current_layout();
        l.primary = 1 - l.primary;
        display.set_layout(l);
        apply_visibility();

        if (!session.game_ini.empty())
          ds::sdl::Config::store(session.game_ini, "video.screen", l.primary ? "bottom" : "top");
        break;
      }
      case A::PipCornerNext: {
        if (dual_window) break;
        Disp::Layout l = display.current_layout();
        l.corner = static_cast<Disp::Corner>((static_cast<int>(l.corner) + 1) % static_cast<int>(Disp::Corner::Count));
        display.set_layout(l);

        if (!session.game_ini.empty())
          ds::sdl::Config::store(session.game_ini, "video.pip_corner", Disp::corner_name(l.corner));
        break;
      }
      case A::Screenshot: shot_pending = true; break;
      case A::Lid:
        input.set_lid(!input.lid());
        VLOG("lid: %s\n", input.lid() ? "closed" : "open");
        if (input.lid()) flush_save();
        break;
      case A::SlotNext:
        state_slot = (state_slot + 1) % 10;
        show_slot(std::to_string(state_slot));
        VLOG("state slot %d\n", state_slot);
        break;
      case A::SlotPrev:
        state_slot = (state_slot + 9) % 10;
        show_slot(std::to_string(state_slot));
        VLOG("state slot %d\n", state_slot);
        break;
      case A::SaveState:
        // A state freezes the machine mid-conversation, but nothing restores a network session.
        if (net_live) {
          std::fprintf(stderr, "state: not during a network session\n");
          break;
        }
        if (save_readonly) {
          std::fprintf(stderr, "state: not during a replay\n");
          break;
        }
        if (save_state_file(nds, state_path(nds, session.states_dir, state_slot), display.current_layout())) {
          flush_save(); // the .sav and the state never diverge
          show_slot("STATE " + std::to_string(state_slot) + " SAVED");
        }
        break;
      case A::LoadState:
        if (net_live) {
          std::fprintf(stderr, "state: not during a network session\n");
          break;
        }
        if (save_readonly) {
          std::fprintf(stderr, "state: not during a replay\n");
          break;
        }
        if (log.writing()) {
          std::fprintf(stderr, "state: not while recording\n");
          break;
        }
        if (load_state_file(nds, state_path(nds, session.states_dir, state_slot), loaded_layout, got_layout)) {
          apply_loaded_layout();
          audio.clear();
          pacer.reset();
          fs_debt_ms = 0;
          flush_save();
          show_slot("STATE " + std::to_string(state_slot) + " LOADED");
          log_sd_note();
        }
        break;
      case A::FastForwardToggle:
        if (net_live) {
          std::fprintf(stderr, "fast forward: not during a network session\n");
          break;
        }
        ff_toggle = !ff_toggle;
        VLOG("fast forward %s\n", ff_toggle ? "on" : "off");
        break;
      default: break;
      }
    }
    if (rgds::menu_hold()) {
      const Uint32 now = SDL_GetTicks();
      if (!holding) {
        holding = true;
        audio.pause(true);
        for (int s = 0; s < 2; ++s) {
          const size_t n =
              held.flags[s] ? static_cast<size_t>(held.w) * held.h : static_cast<size_t>(ds::SCREEN_W) * ds::SCREEN_H;
          if (held.src[s]) held.px[s].assign(held.src[s], held.src[s] + n);
        }
        held_present_at = now - 1000;
        held_presents = 0;
        std::fprintf(stderr, "RG DS menu: game held\n");
      }
      // Again when the menu publishes a new frame, else ~30 Hz for the HUD.
      const Uint32 sequence = rgds::overlay_sequence();
      if (sequence != held_sequence || now - held_present_at >= 33) {
        held_sequence = sequence;
        held_present_at = now;
        if (present_held()) ++held_presents;
      }
      SDL_Delay(4);
      continue;
    }
    if (holding) {
      holding = false;
      audio.pause(false);
      pacer.reset();
      fs_debt_ms = 0;
      std::fprintf(stderr, "RG DS menu: game resumed (%u presents while held)\n", held_presents);
    }
    input.update_stylus();
    ds::input::Frame in = input.frame();
    if (log.reading()) {
      if (!log.read(in)) break; // the controls still quit; the log ends the run
      ds::input::apply(nds, in);
    } else {
      bool closed;
      if (lid.poll(closed, nds.sleeping)) {
        input.set_lid(closed);
        if (closed) flush_save();
      }
      in.lid = input.lid();
      if (input.fake_mic()) input.fake_mic_frame(mic);
      else {
        if (nds.io.mic_used()) open_mic();
        if (mic_alsa.active()) mic_alsa.capture(mic_raw);
        else if (audio.capturing()) mic_raw = audio.capture();
        else mic_raw.clear();
        for (s16& v : mic_raw) {
          dc += (v - dc) * 0.005;
          const double y = (v - dc) * mic_gain;
          v = static_cast<s16>(y > 32767 ? 32767 : (y < -32768 ? -32768 : y));
        }
        // Capture arrives in bursts; queue and hand out a frame at a time,
        // dropping backlog so a stall doesn't turn into latency.
        mic_queue.insert(mic_queue.end(), mic_raw.begin(), mic_raw.end());
        if (mic_queue.size() > mic_per_frame * 4)
          mic_queue.erase(mic_queue.begin(), mic_queue.end() - mic_per_frame * 2);
        const size_t take = mic_queue.size() < mic_per_frame ? mic_queue.size() : mic_per_frame;
        mic.assign(mic_queue.begin(), mic_queue.begin() + take);
        mic_queue.erase(mic_queue.begin(), mic_queue.begin() + take);
        if (mic_gate > 0 && !mic.empty()) {
          double sq = 0;
          for (s16 v : mic) sq += double(v) * v;
          const double rms = std::sqrt(sq / mic.size());
          mic_floor = rms < mic_floor ? rms : mic_floor + (rms - mic_floor) * 0.002; // drops at once, creeps up
          if (rms < mic_floor * mic_gate + 16) std::fill(mic.begin(), mic.end(), 0);
        }
      }
      if (static const bool mic_log = std::getenv("DS_MIC_LOG") != nullptr; mic_log) {
        static u32 mframes = 0, mtotal = 0;
        static int mpeak = 0;
        static double msq = 0;
        for (s16 v : mic) {
          msq += double(v) * v;
          if (std::abs(int(v)) > mpeak) mpeak = std::abs(int(v));
        }
        mtotal += static_cast<u32>(mic.size());
        if (++mframes == 60) {
          std::fprintf(stderr, "[mic] capture: %u samples/60 frames, peak %d, rms %.0f\n", mtotal, mpeak,
                       mtotal ? std::sqrt(msq / mtotal) : 0.0);
          mframes = mtotal = 0;
          mpeak = 0;
          msq = 0;
        }
      }
      ds::input::decimate_mic(in, mic.data(), mic.size());
      if (log.writing()) log.write(in);
      ds::input::apply(nds, in, mic.data(), mic.size());
    }

    // With per-scanline scaling the core writes straight into the
    // panel-sized texture, so the scale cost lands inside run_frame()
    // rather than present; DS_FPS's emu/draw split shifts accordingly.
    bool fast = ff_toggle || input.fast_forward_held();
    if (net_live && fast) {
      static bool said = false;
      if (!said) {
        said = true;
        std::fprintf(stderr, "fast forward: not during a network session\n");
      }
      fast = false;
    }
    if (fast != was_fast) {
      was_fast = fast;
      pacer.reset();
    }
    // FF's presentation cadence can also save invisible raster work. Capture
    // and FIFO frames always render: their pixels can be read by the guest.
    nds.gpu.set_frameskip_capture(fast ? false : fs_capture);
    const bool ff_raster = fast && ff_raster_enabled && ff_skip > 0 && !shot_pending && nds.gpu.frame_skip_safe();
    if (ff_raster != was_ff_raster) {
      was_ff_raster = ff_raster;
      fs_left = fs_blocks = 0;
      fs_in_skip = false;
      fs_drawn_run = 0;
    }
    const int render_skip_limit = ff_raster ? std::max(fs_limit, ff_skip) : fs_limit;
    // Policy decides for the frame after the one about to run; what the
    // core settled for this one governs the present.
    if (render_skip_limit > 0) {
      const int period = nds.gpu.display_phase_period();
      // Limit counts blocks, not frames: on a game whose screens take a
      // whole period to come round, frameskip=3 skips three periods per
      // drawn one, the same ratio a period-1 game gets.
      const int max_blocks = render_skip_limit;
      if (period > 1 && !fs_period_warned) {
        fs_period_warned = true;
        std::fprintf(stderr, "frameskip: this game drives its screens over %d frames, so it skips %d of every %d\n",
                     period, fs_limit * period, (fs_limit + 1) * period);
      }
      if (fs_left == 0) { // a block ended: pick the next one
        // Fixed skips its blocks whatever the clock says; adaptive skips only
        // while it is behind, and both stop at the limit and draw a period.
        const bool want = ff_raster || !fs_adaptive || fs_debt_ms > frame_budget_ms * 0.5;
        const bool skip = max_blocks > 0 && want && fs_blocks < max_blocks;
        fs_blocks = skip ? fs_blocks + 1 : 0;
        fs_in_skip = skip;
        fs_left = period;
      }
      --fs_left;
      nds.gpu.set_frame_skip(fs_in_skip);
    } else {
      nds.gpu.set_frame_skip(false);
    }
    const bool skipped = nds.gpu.will_skip_frame();
    // Games that render one screen per frame (the other via capture) show one
    // stale screen right after a skip; draw the block through and present its last frame.
    const int fs_period = render_skip_limit > 0 ? nds.gpu.display_phase_period() : 1;
    if (skipped) {
      ++fs_skipped;
      fs_drawn_run = 0;
    } else ++fs_drawn_run;
    const bool fs_partial = fs_period > 1 && fs_drawn_run < fs_period;
    // Say why frameskip is doing nothing, once, rather than looking broken.
    if (!skipped && fs_in_skip && ++fs_refused == 120 && !fs_capture)
      std::fprintf(stderr,
                   "frameskip: this game display-captures its frames, which cannot be skipped exactly;\n"
                   "           drop --no-frameskip-capture (or [emu] frameskip_capture = true) to skip them anyway\n");
    const bool present = shot_pending
                             ? !skipped
                             : (!skipped && !fs_partial &&
                                (ff_raster || !fast || ff_skip <= 0 || frames % static_cast<u64>(ff_skip + 1) == 0));
    // Translucent PiP inset comes up to opaque while touched, holds
    // pip_touch_hold frames, then fades back. Only when the inset IS the bottom screen.
    {
      const Disp::Layout& l = display.current_layout();
      const int rest = static_cast<int>(l.pip_alpha * 255.0 + 0.5);
      const bool inset_is_bottom = l.mode == Disp::Mode::Pip && l.primary == 0;
      if (inset_is_bottom && (in.down || input.stylus_visible())) pip_hold = pip_touch_hold;
      else if (pip_hold > 0) --pip_hold;
      const int want = pip_hold > 0 && pip_touch_hold > 0 ? 255 : rest;
      pip_alpha += std::clamp(want - pip_alpha, -PIP_FADE_STEP, PIP_FADE_STEP);
      display.set_inset_alpha(static_cast<u8>(pip_alpha));
    }
    ds::sdl::Display::Target target[2] = {};
    bool scaled = false;
    display.set_present_tag(frame_late_start ? 1 : 0);
    if (dual_window) display2.set_present_tag(frame_late_start ? 1 : 0);
    // RG DS direct present (Display::draw_scanout_async): on a scanout tier a
    // frame goes to the panel once it is finished -- the 2x-5x compose as it
    // is, a native frame scaled on the present thread -- so no scanline
    // targets are handed to the core here. Taken at 2x-5x (no other tier
    // shows the compose) and, under the RG DS launcher (whose bridge blends
    // the pause menu through the present's hook), at 1x as well; elsewhere
    // 1x keeps the scanline path. DS_DIRECT_PRESENT=0/1 overrides.
    static const int direct_env = [] {
      const char* e = std::getenv("DS_DIRECT_PRESENT");
      return e ? std::atoi(e) : -1;
    }();
    const bool direct =
        direct_env != 0 && display.scanout_direct() && (!dual_window || display2.scanout_direct()) &&
        (direct_env == 1 || nds.gpu.enhanced_framebuffer(0) != nullptr || rgds::present_hook() != nullptr);
    if (present && !shot_pending && !direct) {
      // Scanout tiers block here until a buffer is free (DS_FPS "wait").
      const Uint64 tw = SDL_GetPerformanceCounter();
      if (dual_window) sync_dual_modes(display, display2);
      scaled = display.begin_frame(target);
      if (dual_window) scaled = display2.begin_frame(target) && scaled;
      wait_ticks += SDL_GetPerformanceCounter() - tw;
    }
    set_scale_targets(target, scaled);
    fb_current = present && !scaled; // a skipped frame renders nothing

    const Uint64 t0 = SDL_GetPerformanceCounter();
    nds.run_frame();
    rgds_completed_frames.fetch_add(1, std::memory_order_relaxed);
    if (nds.dsi_dsp_started && !dsp_warned) {
      dsp_warned = true;
      std::fprintf(stderr, "DSi DSP is not emulated.\n");
    }

    if (nds.exit_requested) {
      std::fprintf(stderr, "dsi: the title has left (a soft reset); quitting\n");
      flush_save();
      input.request_quit();
      nds.exit_requested = false;
    }
    if (nds.power_off) {
      if (boot_firmware) {
        std::string err;
        if (fw_override.empty()) {
          // DSi mode: nothing is kept (see fw_override)
        } else if (nds.firmware_override_dirty() && !nds.save_firmware_override(fw_override, err))
          std::fprintf(stderr, "firmware settings: cannot save %s: %s\n", fw_override.c_str(), err.c_str());
        else if (nds.firmware_override_dirty())
          std::fprintf(stderr, "firmware settings: saved to %s\n", fw_override.c_str());
        std::fprintf(stderr, "power off: rebooting the firmware\n");
        autosave_now(); // the reset below discards the session
#if DSPERATE_JIT
        if (jit) ds::jit::flush_all();
#endif
        nds.reset();                          // clears power_off, and re-seeds the clock
        if (nds.dsi) nds.setup_direct_boot(); // the DSi boots from its NAND again
      } else {
        nds.power_off = false;
      }
    }

    const Uint64 t1 = SDL_GetPerformanceCounter();
    if (present) {
      const bool cursor = input.stylus_visible() && !log.reading();
      // Crosshair is drawn in DS pixels, so it shrinks with a small view;
      // scale back up by the reduction so it stays findable.
      int cursor_size = input.stylus_size();
      {
        const Disp::Layout& cl = display.current_layout();
        double ratio = 1.0;
        if (!dual_window && cl.mode == Disp::Mode::Pip && cl.primary == 0) ratio = cl.pip;
        else if (!dual_window && (cl.mode == Disp::Mode::DominantV || cl.mode == Disp::Mode::DominantH) &&
                 cl.primary == 0)
          ratio = display.dominant_ratio();
        if (ratio < 1.0) cursor_size *= std::clamp(static_cast<int>(1.0 / ratio + 0.5), 1, 4);
      }
      if (scaled) {
        // Crosshair first, still in DS pixels. Must go down before
        // finish_views(): a PiP-inset bottom screen's target is a side
        // buffer finish_views() then copies into the canvas.
        if (cursor)
          draw_cursor(CursorDst{target[1].px, target[1].pitch, target[1].h, target[1].xrun}, input.stylus_x(),
                      input.stylus_y(), cursor_size);
        display.finish_views();
        if (dual_window) display2.finish_views();
        display.present();
        if (dual_window) display2.present();
      } else {
        const u32* fb[2] = {nds.gpu.framebuffer(0), nds.gpu.framebuffer(1)};
        if (cursor) {
          std::memcpy(cursor_fb.data(), fb[1], cursor_fb.size() * 4);
          draw_cursor(CursorDst{cursor_fb.data(), ds::SCREEN_W, ds::SCREEN_H, nullptr}, input.stylus_x(),
                      input.stylus_y(), cursor_size);
          fb[1] = cursor_fb.data();
        }
        // A GPU present goes to the present thread on the aux core
        // (present_thread.h); DS_PRESENT_SYNC=1 keeps it here.
        static const bool present_sync = std::getenv("DS_PRESENT_SYNC") != nullptr;

        if (!present_sync && display.gpu_present() && (!dual_window || display2.gpu_present())) {
          ds::sdl::Display* const ds_[2] = {&display, &display2};
          ds::sdl::Display::draw_async(ds_, dual_window ? 2 : 1, fb);
        } else {
          // Pinned (thread layout), the emulation thread presents here as a
          // normal task: the kernel work a present queues on this core (a
          // per-CPU kworker) cannot run under an RT thread that has the core
          // to itself, so it piled up until the per-CPU RT cap (950 ms/s, no
          // runtime sharing on ROCKNIX's kernel) idled the thread -- a 20-33 ms
          // stall about once a second on the heavy titles (ST, GS:DD).
          // DS_PRESENT_RT=1 keeps RT.
          static const bool present_nort = ds::thread_layout_on() && !std::getenv("DS_PRESENT_RT");
          // RG DS: an SDL_Renderer window's upload, draw and swap (~3 ms on
          // the A55 with both panels) go to the present thread as well.
          static const bool renderer_async =
              !present_sync && !(std::getenv("DS_RENDERER_SYNC") && std::atoi(std::getenv("DS_RENDERER_SYNC")) != 0);
          ds::sdl::Display* const windows[2] = {&display, &display2};
          int rt_pol = 0;
          sched_param rt_sp{};
          const bool drop_rt = !renderer_async && present_nort &&
                               pthread_getschedparam(pthread_self(), &rt_pol, &rt_sp) == 0 &&
                               (rt_pol == SCHED_RR || rt_pol == SCHED_FIFO);
          if (drop_rt) {
            sched_param z{};
            sched_setscheduler(0, SCHED_OTHER, &z);
          }
          const bool enhanced_screen[2] = {nds.gpu.enhanced_screen(0), nds.gpu.enhanced_screen(1)};
          // HD: the core's rules go with this frame to the bridge's GPU reconstruction (null when HD is off).
          const u32* const hd_rules[2] = {nds.gpu.hd_rules(0), nds.gpu.hd_rules(1)};
          ds::sdl::Display::set_hd_rules(hd_rules);
          const u32* enhanced[2] = {enhanced_screen[0] ? nds.gpu.enhanced_framebuffer(0) : fb[0],
                                    enhanced_screen[1] ? nds.gpu.enhanced_framebuffer(1) : fb[1]};
          if (cursor && enhanced_screen[1]) {
            cursor_enhanced_fb.assign(enhanced[1], enhanced[1] + ds::gpu::Gpu::PRESENT_W * ds::gpu::Gpu::PRESENT_H);
            std::array<u16, ds::SCREEN_W + 1> runs{};
            runs.back() = ds::gpu::Gpu::PRESENT_W;
            draw_cursor(
                CursorDst{cursor_enhanced_fb.data(), ds::gpu::Gpu::PRESENT_W, ds::gpu::Gpu::PRESENT_H, runs.data()},
                input.stylus_x(), input.stylus_y(), cursor_size);
            enhanced[1] = cursor_enhanced_fb.data();
          }
          const bool detail = enhanced[0] && enhanced[1] &&
                              (direct || (display.enhanced_present() && (!dual_window || display2.enhanced_present())));
          if (detail) remember_present(enhanced, ds::gpu::Gpu::PRESENT_W, ds::gpu::Gpu::PRESENT_H, enhanced_screen);
          else remember_present(fb, ds::SCREEN_W, ds::SCREEN_H, nullptr);
          if (direct) {
            // Each screen as the core finished it (the compose, or native), to the panel without a GPU job.
            if (!ds::sdl::Display::draw_scanout_async(windows, dual_window ? 2 : 1, enhanced, ds::gpu::Gpu::PRESENT_W,
                                                      ds::gpu::Gpu::PRESENT_H, enhanced_screen)) {
              static bool said = false;
              if (!said) {
                said = true;
                std::fprintf(stderr, "video: the direct present is unavailable on this tier; frames are not shown\n");
              }
            }
          } else if (detail) {
            if (!renderer_async ||
                !ds::sdl::Display::draw_renderer_async(windows, dual_window ? 2 : 1, enhanced, ds::gpu::Gpu::PRESENT_W,
                                                       ds::gpu::Gpu::PRESENT_H, enhanced_screen)) {
              display.draw(enhanced, ds::gpu::Gpu::PRESENT_W, ds::gpu::Gpu::PRESENT_H, enhanced_screen);
              if (dual_window)
                display2.draw(enhanced, ds::gpu::Gpu::PRESENT_W, ds::gpu::Gpu::PRESENT_H, enhanced_screen);
            }
          } else if (!renderer_async || !ds::sdl::Display::draw_renderer_async(windows, dual_window ? 2 : 1, fb)) {
            display.draw(fb);
            if (dual_window) display2.draw(fb);
          }
          if (drop_rt) sched_setscheduler(0, rt_pol, &rt_sp);
        }
      }
    }
    // Unlimited limiter outruns the speakers like fast forward: sheds audio the same way.
    audio.push(nds, fast || limiter_off);
    if (present) {
      static bool first_present = true;
      if (first_present) {
        first_present = false;
        startup_stage("first present");
      }
    }
    if (shot_pending && present) {
      shot_pending = false;
      screenshot(nds, session.shots_dir, display.current_layout());
    }
    const Uint64 t2 = SDL_GetPerformanceCounter();
    const u64 frame_done_ns = mono_ns(); // the frame is handed to the present thread (or drawn)
    emu_ticks += t1 - t0;
    draw_ticks += t2 - t1;
    draw_ticks_total += t2 - t1;
    if (static_cast<long>(frames) >= stats_from) {
      frame_ms.push_back(static_cast<double>(t1 - t0) * ticks_to_ms);
      work_ms.push_back(static_cast<double>(t2 - t0) * ticks_to_ms);
      // DS_FRAME_SERIES=<path>: "emu work start end" per frame in run order:
      // ms, then the emu slice's CLOCK_MONOTONIC bounds in ns, so a
      // `perf record --clockid monotonic` can be cut to chosen frames.
      static FILE* series = [] {
        const char* p = std::getenv("DS_FRAME_SERIES");
        return p ? std::fopen(p, "w") : nullptr;
      }();
      if (series) {
        timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        const double tick_ns = ticks_to_ms * 1e6;
        const Uint64 tc = SDL_GetPerformanceCounter(); // taken with ts: maps the counter onto CLOCK_MONOTONIC
        const long long now = static_cast<long long>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
        const long long emu_end = now - static_cast<long long>(static_cast<double>(tc - t1) * tick_ns);
        const long long start = emu_end - static_cast<long long>(static_cast<double>(t1 - t0) * tick_ns);
        std::fprintf(series, "%.3f %.3f %lld %lld\n", frame_ms.back(), work_ms.back(), start, emu_end);
      }
      // DS_HITCH_PNG=<ms>: write what is ON SCREEN for any frame whose work
      // exceeds that, named <index>-<ms>. Capped at DS_HITCH_MAX (default 12).
      static const double hitch_ms = [] {
        const char* p = std::getenv("DS_HITCH_PNG");
        return p ? std::atof(p) : 0.0;
      }();
      static const long hitch_max = [] {
        const char* p = std::getenv("DS_HITCH_MAX");
        return p ? std::atol(p) : 12;
      }();
      static long hitch_n = 0;
      if (hitch_ms > 0.0 && work_ms.back() > hitch_ms && hitch_n < hitch_max) {
        ++hitch_n;
        char nm[64];
        std::snprintf(nm, sizeof nm, "/hitch-%05zu-%.1fms.png", work_ms.size() - 1, work_ms.back());
        ::mkdir(session.shots_dir.c_str(), 0755);
        const std::string path = session.shots_dir + nm;
        if (write_png(nds, path, display.current_layout()))
          std::fprintf(stderr, "hitch: frame %zu at %.1f ms -> %s\n", work_ms.size() - 1, work_ms.back(), path.c_str());
      }
    }
    // RG DS adaptive resolution (video.adaptive, DS_ADAPTIVE=0 off): at 2x-5x
    // a run of frames over budget hands the 3D to the native raster for a
    // while, so motion and sound stay at full speed while sharpness dips
    // (adaptive_res.h has the rules).
    {
      // RG DS: the menu's Heavy 3D scenes choice switches it live (rgds_set_adaptive).
      if (g_rgds_adaptive.load(std::memory_order_relaxed) < 0) {
        const char* e = std::getenv("DS_ADAPTIVE");
        g_rgds_adaptive.store(e ? (std::atoi(e) != 0) : cfg.flag("video.adaptive", true), std::memory_order_relaxed);
      }
      const bool adaptive_on = g_rgds_adaptive.load(std::memory_order_relaxed) > 0;
      static ds::sdl::AdaptiveRes adaptive;
      // The first seconds of a start are slow anyway (pipelines, uploads), so
      // they never start a hold. RG DS KMS starts with adaptive on hit a GPU
      // job the kernel hard-stopped about 0.5 s in (2 of 2 harness starts,
      // 0 of 7 with it off). DS_ADAPTIVE_GRACE_MS overrides the 5 s.
      static const double grace_ms = [] {
        const char* e = std::getenv("DS_ADAPTIVE_GRACE_MS");
        return e ? std::atof(e) : 5000.0;
      }();
      static double adaptive_from_ms = -1;
      auto& r3d = nds.gpu3d.renderer();
      using Event = ds::sdl::AdaptiveRes::Event;
      const double now_ms = static_cast<double>(t2) * ticks_to_ms;
      if (adaptive_on && adaptive_from_ms < 0 && r3d.hires_active()) adaptive_from_ms = now_ms + grace_ms;
      const double gpu_wait =
          static_cast<double>(nds.gpu.take_hires_wait_ns()) / 1e6; // this frame's, taken every frame
      if (adaptive_on && r3d.hires_active() && !(fast && ff_speed <= 0) && now_ms >= adaptive_from_ms) {
        switch (adaptive.frame(now_ms, static_cast<double>(t2 - t0) * ticks_to_ms, frame_budget_ms, gpu_wait)) {
        case Event::Hold:
          r3d.set_hires_hold(true);
          std::fprintf(stderr, "adaptive: native for %.1f s (%u of 30 frames over budget)\n", adaptive.hold_ms() / 1000,
                       adaptive.last_over());
          break;
        case Event::Release:
          r3d.set_hires_hold(false);
          std::fprintf(stderr, "adaptive: back to high resolution (native %.1f ms vs %.1f ms)\n", adaptive.hold_avg(),
                       adaptive.hires_avg());
          break;
        case Event::ReleaseNoGain:
          r3d.set_hires_hold(false);
          std::fprintf(stderr, "adaptive: native was no faster (%.1f vs %.1f ms); high resolution for 60 s\n",
                       adaptive.hold_avg(), adaptive.hires_avg());
          break;
        case Event::None: break;
        }
      } else if (adaptive.pause()) r3d.set_hires_hold(false);
    }
    if (fs_adaptive && fs_limit > 0) {
      // Only real-time play has a budget to fall behind: unthrottled fast forward always is.
      const double work = static_cast<double>(t2 - t0) * ticks_to_ms;
      fs_debt_ms += (fast && ff_speed <= 0) ? 0.0 : work - frame_budget_ms;
      if (fs_debt_ms < 0) fs_debt_ms = 0;
      const double cap = frame_budget_ms * (fs_limit + 1);
      if (fs_debt_ms > cap) fs_debt_ms = cap; // a long stall must not buy a run of skips
    }
    // Auto buffer size needs to know if the machine keeps up; fast forward
    // doesn't get a vote, since it's behind by design.
    if (audio.active() && !(fast && ff_speed <= 0))
      audio.auto_tick(static_cast<double>(t2 - t0) * ticks_to_ms <= frame_budget_ms);
    // DS_AUDIO_QUEUE=<path>: one line a frame -- depth, rate-control ppm,
    // target depth, dry count, SPU output rate.
    if (audio.active()) {
      static FILE* aq = [] {
        const char* p = std::getenv("DS_AUDIO_QUEUE");
        return p ? std::fopen(p, "w") : nullptr;
      }();
      if (aq)
        std::fprintf(aq, "%.3f %.0f %.2f %llu %.1f\n", audio.queued_frames(), audio.rate_trim_ppm(),
                     audio.target_frames(), static_cast<unsigned long long>(audio.stats().dry), audio.input_rate());
    }
    ds::prof::frame_mark(); // marks the emu slice: present isn't in a stage, lands in "untimed" of work_ms

    const Uint64 t3 = SDL_GetPerformanceCounter();
    // The windows' page flips completed since last frame (KMSDRM; panel 0 is
    // the upper window and sets the period), for the phase lock and the log.
    ds::sdl::KmsFlips::Flip flips_new[2][ds::sdl::KmsFlips::RING];
    int flips_n[2] = {0, 0};
    {
      const ds::sdl::Display* wins[2] = {&display, dual_window ? &display2 : nullptr};
      for (int w = 0; w < 2; ++w) {
        if (!wins[w]) continue;
        if (wins[w]->flips())
          flips_n[w] =
              wins[w]->flips()->recent(wins[w]->flip_slot(), flip_seen[w], flips_new[w], ds::sdl::KmsFlips::RING);
        else if (auto* drm = dynamic_cast<ds::sdl::DrmOut*>(wins[w]->scanout()))
          flips_n[w] = drm->recent(flip_seen[w], flips_new[w], ds::sdl::KmsFlips::RING);
        else continue;
        for (int i = 0; i < flips_n[w]; ++i) {
          const u64 samples = lock.samples();
          lock.flip(w, flips_new[w][i].at_ns, flips_new[w][i].issue_ns, flips_new[w][i].wait_ns,
                    (flips_new[w][i].tag & 4) != 0);
          if (lock.samples() != samples) {
            pace_err_abs += std::fabs(lock.last_lat_ns() - lock.band_ns());
            ++pace_err_n;
          }
          flip_seen[w] = flips_new[w][i].seq;
        }
      }
    }
    // base_scale is the limiter+emu.speed clock; fast forward floors it
    // rather than multiplying (ff_speed is a multiple of real time). 0 uncaps.
    const double scale = fast && ff_speed > 0 ? std::max(base_scale, static_cast<double>(ff_speed)) : base_scale;
    // The display lock holds at real-time speed only (a limiter within 3 % of
    // the panel's rate), once the panel's period has held still.
    const bool want_lock = pace_display && !fast && !limiter_off && speed_pct == 100 && lock.locked() &&
                           std::fabs(frame_ns / lock.period_ns() - base_scale) < 0.03;
    if (want_lock != pace_locked) {
      pace_locked = want_lock;
      lock.reset();
      pacer.reset();
      audio.set_speed(want_lock ? frame_ns / lock.period_ns() : base_scale);
      VLOG("pace: %s (panel %.3f ms, %.2f Hz; band %.2f ms, work %.2f ms)\n",
           want_lock ? "locked to the display" : "free again", lock.period_ns() / 1e6, 1e9 / lock.period_ns(),
           lock.band_ns() / 1e6, lock.work_ns() / 1e6);
    }
    frame_late_start = false;
    if ((fast && ff_speed <= 0) || limiter_off) pacer.reset(); // unthrottled: do not bank the time it gains
    else if (pace_locked) {
      audio.set_speed(frame_ns / lock.period_ns()); // follows the period estimate; the trim takes the rest
      // The lock's deadline is on CLOCK_MONOTONIC; the pacer counts SDL ticks: convert by the distance from now.
      const u64 now_m = mono_ns();
      const Uint64 now_t = SDL_GetPerformanceCounter();
      const u64 d = lock.deadline(now_m);
      const double ahead = (static_cast<double>(d) - static_cast<double>(now_m)) * ticks_per_ns;
      const Uint64 deadline_t = ahead > 0.0 ? now_t + static_cast<Uint64>(ahead) : now_t;
      frame_late_start = pacer.wait_until(deadline_t) || ahead <= 0.0;
      ++pace_locked_frames;
    } else pacer.wait(scale);
    ++pace_frames;
    const u64 next_start_ns = mono_ns();
    if (pace_display && !fast && !limiter_off)
      lock.started(next_start_ns, frame_late_start); // free frames teach it the work too

    pace_ticks += SDL_GetPerformanceCounter() - t3;
    fps_pace_ticks += SDL_GetPerformanceCounter() - t3;

    // DS_CADENCE_LOG=<path>: one line per emulated frame -- frame index,
    // SWAP_BUFFERS count (30 Hz alternates 1,0,1,0; 60 Hz reads 1,1,1,1),
    // polygon count, whether presented (so frameskip isn't mistaken for a
    // cadence change); then CLOCK_MONOTONIC ns: when this frame's wait ended
    // (the input poll follows), when it was handed to the present, when the
    // next wait ended; whether it was display-locked, and the lock's band
    // (ns). Lines "f <panel> <seq> <completed_ns> <swap_ns> <tag> <wait_ns>"
    // are the page flips read this frame (KMSDRM): swap_ns 0 when the swap
    // was not seen, tag bit 0 when the frame it shows began past its
    // deadline and bit 2 when the frame queued behind it was dropped at its
    // vblank (the direct tier's mailbox), wait_ns how long the present held
    // the swap for the window's previous flip.
    if (static const char* cad_path = std::getenv("DS_CADENCE_LOG"); cad_path) {
      static std::FILE* cad = [&] {
        std::FILE* f = std::fopen(cad_path, "w");
        if (f) std::fprintf(f, "# frame swaps polygons presented start_ns done_ns next_ns locked band_ns\n");
        else std::fprintf(stderr, "DS_CADENCE_LOG: cannot write %s\n", cad_path);
        return f;
      }();
      static u64 cad_prev = 0;
      if (cad) {
        using ull = unsigned long long;
        for (int w = 0; w < 2; ++w)
          for (int i = 0; i < flips_n[w]; ++i)
            std::fprintf(cad, "f %d %llu %llu %llu %d %llu\n", w, static_cast<ull>(flips_new[w][i].seq),
                         static_cast<ull>(flips_new[w][i].at_ns), static_cast<ull>(flips_new[w][i].issue_ns),
                         flips_new[w][i].tag, static_cast<ull>(flips_new[w][i].wait_ns));
        const u64 now = nds.gpu3d.swap_count();
        std::fprintf(cad, "%llu %llu %u %d %llu %llu %llu %d %.0f\n", static_cast<ull>(frames),
                     static_cast<ull>(now - cad_prev), nds.gpu3d.render_polygon_count(), present ? 1 : 0,
                     static_cast<ull>(frame_start_ns), static_cast<ull>(frame_done_ns), static_cast<ull>(next_start_ns),
                     pace_locked ? 1 : 0, lock.band_ns());
        cad_prev = now;
        if (frames % 256 == 0) std::fflush(cad);
      }
    }
    frame_start_ns = next_start_ns;
    ++frames;
    // DS_SHOT_AT=N: a screenshot after frame N, for offscreen/replay runs.
    if (static const long shot_at =
            [] {
              const char* v = std::getenv("DS_SHOT_AT");
              return v ? std::atol(v) : -1L;
            }();
        shot_at >= 0 && frames == static_cast<u64>(shot_at))
      shot_pending = true;
    if (nds.cart && nds.cart->sram_dirty()) {
      if (nds.cart->sram_writes() != sram_writes_seen) {
        sram_writes_seen = nds.cart->sram_writes();
        sram_quiet_since = frames;
      } else if (frames - sram_quiet_since >= 60) flush_save();
    }
    if (dsi_mode && nds.dsi_nand.writes != nand_exported) {
      if (nds.dsi_nand.writes != nand_writes_seen) {
        nand_writes_seen = nds.dsi_nand.writes;
        nand_quiet_since = frames;
      } else if (frames - nand_quiet_since >= 120) flush_dsi();
    }
    if (nds.dsi_sd.writes != sd_synced) {
      if (nds.dsi_sd.writes != sd_writes_seen) {
        sd_writes_seen = nds.dsi_sd.writes;
        sd_quiet_since = frames;
      } else if (frames - sd_quiet_since >= 120) flush_sd();
    }

    if (show_fps && frames % 60 == 0) { // Diagnostic telemetry only; RG DS owns the counter.
      const Uint64 now = SDL_GetPerformanceCounter();
      const double secs = static_cast<double>(now - fps_mark) / SDL_GetPerformanceFrequency();
      const double to_ms = 1e3 / SDL_GetPerformanceFrequency() / 60.0;
      const double fps = secs > 0 ? 60.0 / secs : 0.0;
      if (show_fps) {
        const double wall_ms = static_cast<double>(now - fps_mark) * to_ms; // per frame, same unit as the rest
        std::fprintf(
            stderr,
            "%.1f fps (%.0f%%), emu %.1f ms, present %.1f ms, wait %.1f ms, pace %.1f ms (spin %.0f us), other %.1f ms, audio queued %.1f frames (%+.0f ppm, %.1f -> %u Hz)\n",
            fps, 100.0 * fps / (ds::ARM9_CLOCK_HZ / double(ds::CYCLES_PER_FRAME)), emu_ticks * to_ms,
            draw_ticks * to_ms, wait_ticks * to_ms, fps_pace_ticks * to_ms, pacer.spin_us(),
            wall_ms - (emu_ticks + draw_ticks + wait_ticks + fps_pace_ticks) * to_ms, audio.queued_frames(),
            audio.rate_trim_ppm(), audio.input_rate(), audio.rate());
        if (fs_limit > 0)
          std::fprintf(stderr, "  frameskip: %llu frames skipped (%s, limit %d)\n",
                       static_cast<unsigned long long>(fs_skipped), fs_adaptive ? "adaptive" : "fixed", fs_limit);
      }
      fps_mark = now;
      emu_ticks = draw_ticks = wait_ticks = fps_pace_ticks = 0;
    }
  }

  autosave_now();
  flush_save();
  discard_session_cache();
  // Not after a DSiWare session: nothing a DSi title wrote belongs beside the DS firmware.
  if (!fw_override.empty() && nds.firmware_override_dirty() && !nds.dsi) {
    std::string err;
    if (!nds.save_firmware_override(fw_override, err))
      std::fprintf(stderr, "firmware settings: cannot save %s: %s\n", fw_override.c_str(), err.c_str());
    else std::fprintf(stderr, "firmware settings: saved to %s\n", fw_override.c_str());
  }
  if (fs_limit > 0)
    VLOG("frameskip (%s, limit %d): %llu of %llu frames not drawn\n", fs_adaptive ? "adaptive" : "fixed", fs_limit,
         static_cast<unsigned long long>(fs_skipped), static_cast<unsigned long long>(frames));
  if (log.writing()) std::fprintf(stderr, "recorded %u frames to %s\n", log.frames(), record);
  // Printed only on request, unlike the headless frontend which always
  // does (its whole job is measuring). DS_PROFILE implies it: the stage
  // breakdown needs the frame series it annotates.
  // `dry` is the one that matters: queue was empty when a frame looked at
  // it, so the device played silence, the gap a player hears. Drops with
  // no dry frames means fast-forwarding or a lost device, a different fault.
  if (const ds::sdl::Audio::Stats& as = audio.stats(); as.frames) {
    VLOG(
        "audio: queue depth %.2f..%.2f frames over %llu, dry %llu, under 1 frame %llu, under 0.5 %llu, dropped %llu; device buffer %u samples (%.2f frames), ring overruns %llu\n",
        as.min_depth, as.max_depth, static_cast<unsigned long long>(as.frames), static_cast<unsigned long long>(as.dry),
        static_cast<unsigned long long>(as.under_one), static_cast<unsigned long long>(as.under_half),
        static_cast<unsigned long long>(as.dropped), audio.device_samples(), audio.device_buffer_frames(),
        static_cast<unsigned long long>(nds.spu.ring_overruns()));
  }
  if (pacer.rt_relief()) {
    unsigned relief_frames = 0;
    const double relief_us = pacer.relief_us(&relief_frames);
    VLOG("rt relief: %u of %llu frames gave time back, %.0f us each on those\n", relief_frames,
         static_cast<unsigned long long>(frames), relief_us);
  }
  if (pace_display && pace_frames)
    std::fprintf(
        stderr,
        "pace: display-locked %llu of %llu frames (panel %.3f ms, %.2f Hz), start-to-swap %.2f ms mean %.2f ms used, band %.2f ms, %llu missed ticks, |lat - band| %.2f ms mean\n",
        static_cast<unsigned long long>(pace_locked_frames), static_cast<unsigned long long>(pace_frames),
        lock.period_ns() / 1e6, lock.period_ns() > 0.0 ? 1e9 / lock.period_ns() : 0.0, lock.work_mean_ns() / 1e6,
        lock.work_ns() / 1e6, lock.band_ns() / 1e6, static_cast<unsigned long long>(lock.late()),
        pace_err_n ? pace_err_abs / 1e6 / static_cast<double>(pace_err_n) : 0.0);
  if (std::getenv("DS_FRAME_STATS") || ds::prof::enabled) {
    // Emulation work only; excluded costs named on their own line. Under
    // DS_PROFILE the probes' own cost comes off first.
    ds::prof::deduct_probe_overhead(frame_ms, &work_ms);
    ds::frame_report(frame_ms);
    // Same statistics over emulation + present: what a missed display
    // frame actually is. Only worth reading with --no-vsync (with vsync
    // the present blocks and the tail pins to the refresh).
    ds::frame_report(work_ms, "work");
    ds::prof::frame_breakdown(frame_ms);
    if (!frame_ms.empty())
      std::fprintf(stderr, "  (emulation only; excluded: present %.1f ms, pacing %.1f ms total over %zu frames)\n",
                   static_cast<double>(draw_ticks_total) * ticks_to_ms, static_cast<double>(pace_ticks) * ticks_to_ms,
                   frame_ms.size());
    for (int lane = 0; lane < ds::sdl::PresentThread::LANES; ++lane)
      if (const auto ps = ds::sdl::PresentThread::get(lane).stats(); ps.jobs)
        std::fprintf(
            stderr,
            "present thread%s: %llu frames, %.3f ms mean, %.3f ms max; the emulation thread waited on it %llu times, %.1f ms total, %llu over 0.5 ms\n",
            lane ? " 1" : "", static_cast<unsigned long long>(ps.jobs),
            static_cast<double>(ps.job_ns) / 1e6 / static_cast<double>(ps.jobs),
            static_cast<double>(ps.job_max_ns) / 1e6, static_cast<unsigned long long>(ps.waits),
            static_cast<double>(ps.wait_ns) / 1e6, static_cast<unsigned long long>(ps.long_waits));
    ds::handoff::report(stderr);
  }
  log.close();
  ds::prof::report();
  lid.close();
  mic_alsa.close();
  input.close();
  audio.close();
  display.close();
  display2.close();
  SDL_Quit();
  return 0;
}

int main(int argc, char** argv) { return run(argc, argv); }
