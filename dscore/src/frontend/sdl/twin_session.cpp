// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "frontend/sdl/twin_session.h"

#include "net/input_link.h"
#include "net/lan_mp.h"
#include "net/twin_mp.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sched.h>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace ds::sdl {

namespace {
// Each console's inputs go to the other handheld's copy of it: the host's console listens there on one port, the
// guest's on the other.
constexpr int kHostConsolePort = 7101;
constexpr int kGuestConsolePort = 7102;
constexpr u32 kSeatVersion = 1;

using Seat = std::map<std::string, std::string>;

std::string encode(const Seat& s) {
  std::string out;
  auto put32 = [&](u32 v) { out.append(reinterpret_cast<const char*>(&v), 4); };
  put32(kSeatVersion);
  for (const auto& [k, v] : s) {
    put32(static_cast<u32>(k.size()));
    out += k;
    put32(static_cast<u32>(v.size()));
    out += v;
  }
  return out;
}

bool decode(const std::string& in, Seat& s) {
  size_t i = 0;
  auto get32 = [&](u32& v) {
    if (i + 4 > in.size()) return false;
    std::memcpy(&v, in.data() + i, 4);
    i += 4;
    return true;
  };
  u32 version;
  if (!get32(version) || version != kSeatVersion) return false;
  while (i < in.size()) {
    u32 kl, vl;
    if (!get32(kl) || i + kl > in.size()) return false;
    std::string k = in.substr(i, kl);
    i += kl;
    if (!get32(vl) || i + vl > in.size()) return false;
    s[k] = in.substr(i, vl);
    i += vl;
  }
  return true;
}

std::string slurp(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

bool spill(const std::string& path, const std::string& data) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  return f && f.write(data.data(), static_cast<std::streamsize>(data.size()));
}

u8 bcd(int v) { return static_cast<u8>(((v / 10) % 10) << 4 | (v % 10)); }

// This handheld's local time now, as the RTC's seven BCD bytes.
void now_bcd(u8 out[7]) {
  const std::time_t t = std::time(nullptr);
  std::tm lt{};
  localtime_r(&t, &lt);
  out[0] = bcd(lt.tm_year % 100);
  out[1] = bcd(lt.tm_mon + 1);
  out[2] = bcd(lt.tm_mday);
  out[3] = static_cast<u8>(lt.tm_wday);
  out[4] = bcd(lt.tm_hour);
  out[5] = bcd(lt.tm_min);
  out[6] = bcd(std::min(lt.tm_sec, 59));
}

std::string hex(const u8* p, size_t n) {
  static const char* d = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; ++i) {
    s += d[p[i] >> 4];
    s += d[p[i] & 15];
  }
  return s;
}

Seat seat_of(const TwinSession::Params& p) {
  Seat s;
  s["rom.name"] = std::filesystem::path(p.rom).filename().string();
  std::error_code ec;
  s["rom.size"] = std::to_string(std::filesystem::file_size(p.rom, ec));
  s["save"] = slurp(p.save);
  s["fw_override"] = slurp(p.fw_override);
  s["user.nickname"] = p.user.nickname;
  s["user.message"] = p.user.message;
  s["user.birthday"] = std::to_string(p.user.birthday_month) + "/" + std::to_string(p.user.birthday_day);
  s["user.colour"] = std::to_string(p.user.favourite_colour);
  s["user.language"] = std::to_string(p.user.language);
  s["mac_suffix"] = std::to_string(p.mac_suffix);
  s["timing"] = p.timing;
  s["idle_skip"] = p.idle_skip;
  s["jit"] = p.jit ? "1" : "0";
  s["delay"] = std::to_string(p.delay);
  return s;
}
} // namespace

std::unique_ptr<TwinSession> TwinSession::start(const Params& p, int scan_ms, int wait_ms, std::string& err) {
  std::unique_ptr<TwinSession> t(new TwinSession);
  t->delay_ = p.delay;
  Seat mine = seat_of(p), theirs;
  u32 peer_ip = 0;
  {
    net::LanMp lan;
    if (!lan.ok()) {
      err = lan.error();
      return nullptr;
    }
    auto role = lan.start_auto(p.name, scan_ms, 2, true);
    if (role == net::LanMp::Role::None) {
      err = "no session heard and none could be hosted";
      return nullptr;
    }
    std::string blob;
    if (role == net::LanMp::Role::Host) {
      std::fprintf(stderr, "twin: hosting, waiting for the other handheld\n");
      const auto t0 = std::chrono::steady_clock::now();
      bool got = false;
      while (!got) {
        const int left =
            wait_ms -
            static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
        if (left <= 0) break;
        got = lan.wait_seat(blob, peer_ip, std::min(left, 1000));
        if (got || !lan.rival_host()) continue;
        // Both handhelds started at once and both host: the one with the higher address joins the other.
        std::fprintf(stderr, "twin: the other handheld hosts too; joining it\n");
        lan.end_session();
        role = lan.start_auto(p.name, 3000, 2, false);
        if (role == net::LanMp::Role::Guest) break;
        role = lan.start_auto(p.name, 0, 2, true); // it went: host again
        if (role == net::LanMp::Role::None) break;
      }
      if (role == net::LanMp::Role::Host && (!got || !decode(blob, theirs))) {
        err = "nobody joined";
        return nullptr;
      }
      if (role == net::LanMp::Role::None) {
        err = "no session heard and none could be hosted";
        return nullptr;
      }
    }
    t->host_ = role == net::LanMp::Role::Host;
    if (t->host_) {
      now_bcd(t->rtc_);
      mine["rtc"] = std::string(reinterpret_cast<const char*>(t->rtc_), 7);
      lan.send_seat(encode(mine));
      std::string ok; // the guest says it has the seat before the session's channel goes
      u32 ip2;
      if (!lan.wait_seat(ok, ip2, 3000)) std::fprintf(stderr, "twin: the guest did not confirm its seat\n");
    } else {
      lan.send_seat(encode(mine));
      if (!lan.wait_seat(blob, peer_ip, wait_ms) || !decode(blob, theirs) || theirs["rtc"].size() != 7) {
        err = "the host never sent its seat";
        return nullptr;
      }
      std::memcpy(t->rtc_, theirs["rtc"].data(), 7);
      lan.send_seat("ok");
      lan.process(); // flush
      usleep(100000);
    }
    t->peer_name_ = theirs["user.nickname"];
    lan.end_session();
  }
  if (theirs["rom.name"] != mine["rom.name"] || theirs["rom.size"] != mine["rom.size"]) {
    err = "the other handheld runs " + theirs["rom.name"] + ", not " + mine["rom.name"];
    return nullptr;
  }
  if (theirs["delay"] != mine["delay"] || theirs["jit"] != mine["jit"] || theirs["timing"] != mine["timing"]) {
    err = "the two handhelds' emulation settings differ";
    return nullptr;
  }

  // The session's files.
  t->tmp_ = "/tmp/rgds-twin-" + std::to_string(getpid());
  std::filesystem::create_directories(t->tmp_);
  const std::string block = t->tmp_ + "/link", peer_save = t->tmp_ + "/other.sav", peer_ovr = t->tmp_ + "/other.ovr";
  std::filesystem::remove(block);
  const bool have_save = !theirs["save"].empty() && spill(peer_save, theirs["save"]);
  const bool have_ovr = !theirs["fw_override"].empty() && spill(peer_ovr, theirs["fw_override"]);

  // The hidden console: the other player's, run as on their handheld.
  char in_addr[INET_ADDRSTRLEN] = {};
  in_addr_t a = peer_ip;
  inet_ntop(AF_INET, &a, in_addr, sizeof in_addr);
  const int my_port = t->host_ ? kHostConsolePort : kGuestConsolePort;
  const int their_port = t->host_ ? kGuestConsolePort : kHostConsolePort;
  std::vector<std::string> args = {p.child,
                                   p.rom,
                                   "--direct",
                                   "--frames",
                                   "0",
                                   "--twin",
                                   block,
                                   "--twin-role",
                                   t->host_ ? "guest" : "host",
                                   "--input-recv",
                                   std::to_string(their_port),
                                   "--input-delay",
                                   theirs["delay"],
                                   "--frameskip",
                                   "1000",
                                   "--mac-suffix",
                                   theirs["mac_suffix"],
                                   "--user-name",
                                   theirs["user.nickname"],
                                   "--user-message",
                                   theirs["user.message"],
                                   "--user-birthday",
                                   theirs["user.birthday"],
                                   "--user-colour",
                                   theirs["user.colour"],
                                   "--user-language",
                                   theirs["user.language"],
                                   "--rtc-at",
                                   hex(t->rtc_, 7)};
  if (have_save) args.insert(args.end(), {"--save", peer_save});
  if (have_ovr) args.insert(args.end(), {"--firmware-override", peer_ovr});
  if (!theirs["timing"].empty()) args.insert(args.end(), {"--timing", theirs["timing"]});
  if (theirs["jit"] == "0") args.push_back("--interp");
  const std::string idle = theirs["idle_skip"];
  const std::string log = t->tmp_ + "/hidden.log";
  t->hashes_ = std::fopen((t->tmp_ + "/visible.log").c_str(), "we");
  t->child_ = fork();
  if (t->child_ == 0) {
    if (p.child_core >= 0) {
      cpu_set_t set;
      CPU_ZERO(&set);
      CPU_SET(p.child_core, &set);
      sched_setaffinity(0, sizeof set, &set);
    }
    if (!idle.empty()) setenv("DS_IDLE_SKIP", idle.c_str(), 1);
    const int fd = ::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
      dup2(fd, 1);
      dup2(fd, 2);
      close(fd);
    }
    std::vector<char*> argv;
    for (auto& s : args) argv.push_back(s.data());
    argv.push_back(nullptr);
    execv(argv[0], argv.data());
    std::fprintf(stderr, "twin: cannot run %s: %s\n", argv[0], std::strerror(errno));
    _exit(127);
  }
  if (t->child_ < 0) {
    err = "cannot start the hidden console";
    return nullptr;
  }

  // This console's end of the link: the host's console makes the shared block, the guest's waits for it.
  t->link_ = std::make_unique<net::TwinMp>(block, t->host_, p.now);
  if (!t->link_->ok()) {
    err = t->link_->error();
    return nullptr;
  }
  t->inputs_ = net::InputLink::sender(std::string(in_addr) + ":" + std::to_string(my_port));
  if (!t->inputs_->ok()) {
    err = t->inputs_->error();
    return nullptr;
  }
  for (int k = 0; k < t->delay_; ++k) {
    t->pending_[static_cast<u32>(k)] = input::Frame{};
    t->inputs_->push(static_cast<u32>(k), input::Frame{});
  }
  std::fprintf(stderr, "twin: %s %s at %s, %d frames of input delay\n", t->host_ ? "hosting" : "joined",
               t->peer_name_.c_str(), in_addr, t->delay_);
  return t;
}

TwinSession::~TwinSession() {
  link_.reset();   // says this console is gone, so the hidden one never waits for it
  inputs_.reset(); // and tells the other handheld, whose copy of this console then ends
  if (hashes_) std::fclose(hashes_);
  if (child_ > 0) {
    ::kill(child_, SIGTERM);
    int st;
    for (int i = 0; i < 100 && waitpid(child_, &st, WNOHANG) == 0; ++i) usleep(10000);
    if (waitpid(child_, &st, WNOHANG) == 0) {
      ::kill(child_, SIGKILL);
      waitpid(child_, &st, 0);
    }
  }
  if (!tmp_.empty()) {
    std::error_code ec;
    std::filesystem::remove(tmp_ + "/link", ec);
    std::filesystem::remove(tmp_ + "/other.sav", ec);
    std::filesystem::remove(tmp_ + "/other.ovr", ec);
    // hidden.log stays for a look after a session; the next session's directory is another
  }
}

input::Frame TwinSession::frame(const input::Frame& in) {
  // What InputLink carries, and nothing else: both copies of this console must apply the same.
  input::Frame pad;
  pad.buttons = in.buttons;
  pad.x = in.x;
  pad.y = in.y;
  pad.down = in.down;
  pad.lid = in.lid;
  std::memcpy(pad.mic, in.mic, sizeof pad.mic);
  const u32 n = frame_++;
  pending_[n + static_cast<u32>(delay_)] = pad;
  inputs_->push(n + static_cast<u32>(delay_), pad);
  const input::Frame out = pending_[n];
  pending_.erase(n);
  return out;
}

void TwinSession::after_frame(const u8* ram, size_t bytes) {
  if (link_) link_->publish();
  net::log_ram_hash(frame_ - 1, ram, bytes, hashes_); // frame_ counts the frames begun; this is the one just run
}

bool TwinSession::child_running() {
  if (child_ <= 0) return false;
  int st;
  if (waitpid(child_, &st, WNOHANG) == child_) {
    child_ = -1;
    return false;
  }
  return true;
}

} // namespace ds::sdl
