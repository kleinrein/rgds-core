// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "display_drm.h"
#include "dmaheap.h"
#include "drm_uapi.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include <ctime>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace ds::sdl {

namespace {

// Every DrmOut on the same DRM fd, so completions can be routed by owner.
std::vector<DrmOut*> g_outs;
// Their state, and the event read on the fd they share: a dual-window layout
// presents its two panels on two threads (present_thread.h), and a completion
// read by either thread retires the panel it belongs to.
std::mutex g_m;
std::condition_variable g_cv;
bool g_polling = false;   // a thread is in poll() on the fd; the others wait on g_cv for what it reads
u64 mono_ns() { timespec t{}; clock_gettime(CLOCK_MONOTONIC, &t); return static_cast<u64>(t.tv_sec) * 1'000'000'000ull + static_cast<u64>(t.tv_nsec); }

constexpr u32 FMT_XRGB8888 = 0x34325258;   // 'XR24'; the core writes 0xAARRGGBB and we are opaque

} // namespace

// The union member is gated on SDL_VIDEO_DRIVER_KMSDRM from our headers, not
// the runtime library, so fall back to the byte view: SDL's ABI layout is
// (int dev_index; int drm_fd; struct gbm_device*).
int sdl_drm_fd(SDL_Window* win) {
  SDL_SysWMinfo wm;
  SDL_VERSION(&wm.version);
  if (!SDL_GetWindowWMInfo(win, &wm)) { std::fprintf(stderr, "drm: SDL_GetWindowWMInfo: %s\n", SDL_GetError()); return -1; }
  if (wm.subsystem != SDL_SYSWM_KMSDRM) { std::fprintf(stderr, "drm: not a KMSDRM window\n"); return -1; }
#if defined(SDL_VIDEO_DRIVER_KMSDRM)
  return wm.info.kmsdrm.drm_fd;
#else
  int ints[2] = {0, -1};
  std::memcpy(ints, wm.info.dummy, sizeof ints);
  return ints[1];
#endif
}

bool DrmOut::alloc_buf(Buf& b) {
  // The import is the probe: a heap the display controller can't scan
  // (system memory without an IOMMU) fails ADDFB2, and dmaheap tries the next.
  const size_t bytes = static_cast<size_t>(w_) * h_ * 4;
  b.fd = dmaheap::alloc(bytes, [&](int fd) {
    drmu::prime_handle ph = {};
    ph.fd = fd;
    if (ioctl(fd_, drmu::IOCTL_PRIME_FD_TO_HANDLE, &ph) < 0) { std::perror("drm: PRIME_FD_TO_HANDLE"); return false; }
    drmu::mode_fb_cmd2 f = {};
    f.width = static_cast<u32>(w_);
    f.height = static_cast<u32>(h_);
    f.pixel_format = FMT_XRGB8888;
    f.handles[0] = ph.handle;
    f.pitches[0] = static_cast<u32>(w_) * 4;
    if (ioctl(fd_, drmu::IOCTL_MODE_ADDFB2, &f) < 0) {
      std::perror("drm: ADDFB2");
      drmu::gem_close gc = {}; gc.handle = ph.handle; ioctl(fd_, drmu::IOCTL_GEM_CLOSE, &gc);
      return false;
    }
    b.handle = ph.handle;
    b.fb = f.fb_id;
    return true;
  }, "drm");
  if (b.fd < 0) return false;
  b.bytes = bytes;
  void* m = mmap(nullptr, b.bytes, PROT_READ | PROT_WRITE, MAP_SHARED, b.fd, 0);
  if (m == MAP_FAILED) { std::perror("drm: mmap"); return false; }
  b.px = static_cast<u32*>(m);
  std::memset(b.px, 0, b.bytes);
  return true;
}

void DrmOut::drop_buf(Buf& b) {
  if (b.fb) { unsigned int id = b.fb; ioctl(fd_, drmu::IOCTL_MODE_RMFB, &id); }
  if (b.handle) { drmu::gem_close gc = {}; gc.handle = b.handle; ioctl(fd_, drmu::IOCTL_GEM_CLOSE, &gc); }
  if (b.px) munmap(b.px, b.bytes);
  if (b.fd >= 0) ::close(b.fd);
  b = Buf{};
}

bool DrmOut::open(SDL_Window* win, int w, int h, int display_index) {
  fd_ = sdl_drm_fd(win);
  if (fd_ < 0) return false;
  display_ = display_index;
  w_ = w; h_ = h;

  drmu::mode_card_res res = {};
  if (ioctl(fd_, drmu::IOCTL_MODE_GETRESOURCES, &res) < 0) { std::perror("drm: GETRESOURCES"); return false; }
  std::vector<u32> conns(res.count_connectors), crtcs(res.count_crtcs), encs(res.count_encoders);
  res.connector_id_ptr = reinterpret_cast<uint64_t>(conns.data());
  res.crtc_id_ptr = reinterpret_cast<uint64_t>(crtcs.data());
  res.encoder_id_ptr = reinterpret_cast<uint64_t>(encs.data());
  res.count_fbs = 0; res.fb_id_ptr = 0;
  if (ioctl(fd_, drmu::IOCTL_MODE_GETRESOURCES, &res) < 0) { std::perror("drm: GETRESOURCES"); return false; }

  drmu::mode_modeinfo mode = {};
  bool found = false;
  int seen = 0;
  for (u32 c : conns) {
    drmu::mode_get_connector gc = {};
    gc.connector_id = c;
    if (ioctl(fd_, drmu::IOCTL_MODE_GETCONNECTOR, &gc) < 0) continue;
    std::vector<drmu::mode_modeinfo> modes(gc.count_modes);
    std::vector<u32> cencs(gc.count_encoders);
    gc.modes_ptr = reinterpret_cast<uint64_t>(modes.data());
    gc.encoders_ptr = reinterpret_cast<uint64_t>(cencs.data());
    gc.count_props = 0; gc.props_ptr = 0; gc.prop_values_ptr = 0;
    if (ioctl(fd_, drmu::IOCTL_MODE_GETCONNECTOR, &gc) < 0) continue;
    if (gc.connection != drmu::CONNECTOR_CONNECTED || gc.count_modes == 0) continue;
    if (seen++ != display_index) continue;

    conn_ = c;
    // A page flip's framebuffer must cover the CRTC, so only a mode matching
    // the window's exact size works -- no windowed (sub-panel) use here.
    for (const drmu::mode_modeinfo& m : modes)
      if (m.hdisplay == w && m.vdisplay == h) { mode = m; found = true; break; }
    if (!found) {
      std::fprintf(stderr, "drm: display %d has no %dx%d mode (panel is %ux%u); scanout needs a fullscreen window\n",
                   display_index, w, h, modes[0].hdisplay, modes[0].vdisplay);
      return false;
    }
    // The CRTC: the one an encoder of this connector drives now, else the
    // first it may drive (possible_crtcs bit i is crtcs[i]) that no other
    // DrmOut holds. After a compositor's teardown the encoders are unbound,
    // and on the RG DS crtcs[display_index] is the other panel's CRTC (DSI-1
    // may only use the second CRTC, DSI-2 only the first): SETCRTC EINVAL.
    auto taken = [](u32 id) {
      std::lock_guard<std::mutex> lk(g_m);
      for (const DrmOut* o : g_outs) if (o->crtc_ == id) return true;
      return false;
    };
    std::vector<u32> order;   // the connector's current encoder first
    if (gc.encoder_id) order.push_back(gc.encoder_id);
    for (u32 e : cencs) if (e != gc.encoder_id) order.push_back(e);
    for (u32 e : order) {
      drmu::mode_get_encoder ge = {};
      ge.encoder_id = e;
      if (ioctl(fd_, drmu::IOCTL_MODE_GETENCODER, &ge) < 0) continue;
      if (ge.crtc_id && !taken(ge.crtc_id)) { crtc_ = ge.crtc_id; break; }
      for (size_t i = 0; i < crtcs.size() && !crtc_; ++i)
        if (((ge.possible_crtcs >> i) & 1u) && !taken(crtcs[i])) crtc_ = crtcs[i];
      if (crtc_) break;
    }
    break;
  }
  if (!conn_ || !crtc_) { std::fprintf(stderr, "drm: no connector/crtc for display %d\n", display_index); return false; }

  for (int i = 0; i < nbufs_; ++i)
    if (!alloc_buf(bufs_[i])) { close(); return false; }
  if (const char* m = std::getenv("DS_KMS_MARGIN_MS"); m && std::atof(m) >= 0.0) margin_ns_ = static_cast<u64>(std::atof(m) * 1e6);

  // A modeset, not a flip: SDL's KMSDRM only modesets on its first GL swap,
  // which never happens in scanline mode.
  drmu::mode_crtc sc = {};
  sc.crtc_id = crtc_;
  sc.fb_id = bufs_[0].fb;
  sc.mode = mode;
  sc.mode_valid = 1;
  sc.set_connectors_ptr = reinterpret_cast<uint64_t>(&conn_);
  sc.count_connectors = 1;
  if (ioctl(fd_, drmu::IOCTL_MODE_SETCRTC, &sc) < 0) { std::perror("drm: SETCRTC"); close(); return false; }
  bufs_[0].busy = true;
  on_screen_ = 0;
  pending_ = -1;
  queued_ = -1;
  dead_ = false;
  { std::lock_guard<std::mutex> lk(g_m); g_outs.push_back(this); }
  std::fprintf(stderr, "drm: display %d on connector %u crtc %u, %dx%d@%u\n",
               display_index, conn_, crtc_, w_, h_, mode.vrefresh);
  return true;
}

void DrmOut::close() {
  std::unique_lock<std::mutex> lk(g_m);
  if (fd_ >= 0 && (presented_ || dropped_ || queued_n_))
    std::fprintf(stderr, "drm: display %d: %llu frames flipped at once, %llu queued behind a pending flip, %llu dropped at one (mailbox)\n",
                 display_, static_cast<unsigned long long>(presented_), static_cast<unsigned long long>(queued_n_), static_cast<unsigned long long>(dropped_));
  presented_ = dropped_ = queued_n_ = 0;
  if (fd_ >= 0) {
    // Let an outstanding flip retire before its buffer is unmapped.
    while (pending_ >= 0 && !dead_)
      if (!pump(lk, fd_, true)) break;
    for (Buf& b : bufs_) drop_buf(b);
  }
  g_outs.erase(std::remove(g_outs.begin(), g_outs.end(), this), g_outs.end());
  fd_ = -1; crtc_ = 0; conn_ = 0;
  cur_ = on_screen_ = pending_ = queued_ = -1;
  w_ = h_ = 0;
  dead_ = false;
}

void DrmOut::retire(u64 at_ns) {
  if (pending_ < 0) return;
  {
    std::lock_guard<std::mutex> lk(ring_m_);
    ring_[flips_ % KmsFlips::RING] = KmsFlips::Flip{flips_ + 1, at_ns, issue_ns_, 0, 0};
    ++flips_;
  }
  if (on_screen_ >= 0) bufs_[on_screen_].busy = false;
  on_screen_ = pending_;
  pending_ = -1;
  if (queued_ >= 0) {
    const int q = queued_;
    queued_ = -1;
    // Mailbox: a frame that has waited here longer than the margin has its
    // successor due before the next vblank (frames come one refresh apart),
    // so it goes and the successor is flipped straight onto that vblank;
    // flipping it now would show it then and put every later frame a
    // refresh behind. A frame queued just before this vblank (its flip
    // would have been direct a moment earlier) is flipped as usual.
    if (queued_may_drop_ && at_ns > queued_ns_ + margin_ns_) { bufs_[q].busy = false; ++dropped_; }
    else if (!flip(q)) dead_ = true;
  }
}

bool DrmOut::flip(int i) {
  drmu::mode_crtc_page_flip pf = {};
  pf.crtc_id = crtc_;
  pf.fb_id = bufs_[i].fb;
  pf.flags = drmu::PAGE_FLIP_EVENT;
  pf.user_data = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(this));
  if (ioctl(fd_, drmu::IOCTL_MODE_PAGE_FLIP, &pf) < 0) { std::perror("drm: PAGE_FLIP"); bufs_[i].busy = false; return false; }
  pending_ = i;
  { std::lock_guard<std::mutex> lk(ring_m_); issue_ns_ = mono_ns(); }
  return true;
}

int DrmOut::recent(u64 after, KmsFlips::Flip* out, int max) const {
  if (max <= 0) return 0;
  std::lock_guard<std::mutex> lk(ring_m_);
  const u64 newest = flips_;
  if (newest <= after) return 0;
  u64 first = after + 1;
  if (newest - first + 1 > static_cast<u64>(KmsFlips::RING)) first = newest - KmsFlips::RING + 1;
  if (newest - first + 1 > static_cast<u64>(max)) first = newest - max + 1;
  int n = 0;
  for (u64 q = first; q <= newest; ++q) out[n++] = ring_[(q - 1) % KmsFlips::RING];
  return n;
}

// Each flip-complete event carries its submitting DrmOut as user_data.
// Takes (`block`: waits up to a second for) completions and retires their
// owners. g_m is held on entry and exit and dropped around the wait. One
// thread reads the fd at a time: another that needs a completion waits on
// g_cv for that read's dispatch (its own event may be what is read) and then
// re-checks its state, so neither reads the other's event out from under it.
bool DrmOut::pump(std::unique_lock<std::mutex>& lk, int fd, bool block) {
  if (g_polling) {
    if (block) g_cv.wait_for(lk, std::chrono::seconds(1));
    return true;
  }
  g_polling = true;
  lk.unlock();
  pollfd p = {fd, POLLIN, 0};
  const int r = poll(&p, 1, block ? 1000 : 0);
  const int err = errno;
  alignas(8) char buf[512];
  const ssize_t n = r > 0 ? read(fd, buf, sizeof buf) : 0;
  lk.lock();
  g_polling = false;
  g_cv.notify_all();
  if (r < 0) return err == EINTR;
  if (r == 0) {
    if (!block) return true;
    std::fprintf(stderr, "drm: no flip completion in 1s\n");
    return false;
  }
  for (ssize_t off = 0; off + static_cast<ssize_t>(sizeof(drmu::event)) <= n;) {
    drmu::event e;
    std::memcpy(&e, buf + off, sizeof e);
    if (e.length < sizeof(drmu::event) || off + static_cast<ssize_t>(e.length) > n) break;
    if (e.type == drmu::EVENT_FLIP_COMPLETE && e.length >= sizeof(drmu::event_vblank)) {
      drmu::event_vblank v;
      std::memcpy(&v, buf + off, sizeof v);
      const u64 at = static_cast<u64>(v.tv_sec) * 1'000'000'000ull + static_cast<u64>(v.tv_usec) * 1000ull;   // CLOCK_MONOTONIC
      for (DrmOut* o : g_outs)
        if (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(o)) == v.user_data) { o->retire(at); break; }
    }
    off += static_cast<ssize_t>(e.length);
  }
  return true;
}

bool DrmOut::dmabuf_plane(int buf, DmabufPlane& out) const {
  if (buf < 0 || buf >= nbufs_ || bufs_[buf].fd < 0) return false;
  out.fd = bufs_[buf].fd; out.offset = 0; out.stride_bytes = static_cast<u32>(w_) * 4;
  out.width = static_cast<u32>(w_); out.height = static_cast<u32>(h_); out.fourcc = FMT_XRGB8888;
  return true;
}

u32* DrmOut::begin_frame() {
  std::unique_lock<std::mutex> lk(g_m);
  if (dead_ || fd_ < 0) return nullptr;
  pump(lk, fd_, false);
  for (;;) {
    if (dead_) return nullptr;
    for (int i = 0; i < nbufs_; ++i)
      if (!bufs_[i].busy) {
        cur_ = i;
        if (!gpu_writes_) dmaheap::sync_begin_write(bufs_[i].fd);
        return bufs_[i].px;
      }
    // All three taken: this wait is the vsync.
    if (pending_ < 0) { std::fprintf(stderr, "drm: no free buffer\n"); dead_ = true; return nullptr; }
    if (!pump(lk, fd_, true)) { dead_ = true; return nullptr; }
  }
}

void DrmOut::end_frame() {
  std::unique_lock<std::mutex> lk(g_m);
  if (dead_ || cur_ < 0) return;
  const int i = cur_;
  cur_ = -1;
  if (!gpu_writes_) dmaheap::sync_end_write(bufs_[i].fd);
  // A flip that completed since begin_frame() read the fd (the vblank fell
  // inside the frame's copy) is still marked pending; take its event first,
  // or the mailbox would drop a frame the panel is ready for.
  if (pending_ >= 0) pump(lk, fd_, false);
  if (dead_) return;
  if (pending_ >= 0) {
    // One flip per CRTC at a time: queued, and retire() decides at the
    // pending flip's vblank (scanout.h: issue it for the vblank after, or
    // drop it when the next frame is due before that one).
    bufs_[i].busy = true;
    queued_ = i; queued_ns_ = mono_ns(); queued_may_drop_ = drop_if_pending_; ++queued_n_;
    return;
  }
  bufs_[i].busy = true;
  ++presented_;
  if (!flip(i)) dead_ = true;
}

// A queued flip only issues when the pending one retires, which is only
// noticed from begin_frame(); wait it out here so a one-off present isn't
// left stranded with no next frame to notice the retire.
void DrmOut::flush() {
  std::unique_lock<std::mutex> lk(g_m);
  while (queued_ >= 0 && pending_ >= 0 && !dead_)
    if (!pump(lk, fd_, true)) { dead_ = true; return; }
}

} // namespace ds::sdl
