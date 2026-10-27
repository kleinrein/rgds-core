// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Direct KMS scanout: the KMSDRM counterpart of the Wayland dmabuf tier.
// SDL2's KMSDRM backend has no window framebuffer -- every SDL presentation
// path there secretly routes through a hidden GLES renderer and a blocking
// eglSwapBuffers. Instead we write scanline frames straight into CMA
// dma-heap buffers, import them as DRM framebuffers, and page-flip onto the
// CRTC directly (SDL keeps the window/input/DRM master; we borrow its fd via
// SDL_SysWMinfo). No libdrm/kernel DRM headers needed: ioctls go through
// drm_uapi.h. open() fails cleanly if unsupported, and Display falls back to
// SDL's window surface.
#pragma once

#include "core/types.h"
#include "kms_flips.h"
#include "scanout.h"

#include <cstddef>
#include <mutex>

namespace ds::sdl {

// SDL KMSDRM's DRM fd (the one every window of the process shares); -1 on
// another video driver.
int sdl_drm_fd(SDL_Window* win);

class DrmOut : public ScanoutOut {
public:
  // One on screen, one flip pending (only one outstanding per CRTC), one
  // queued behind it. begin_frame() blocks only when all three are taken.
  static constexpr int BUFS = 4; // array size; nbufs_ is the count in use (default 3, 4 under GPU present)
  static constexpr int DEFAULT_BUFS = 3;

  // False if any precondition is missing (wrong video driver, no usable
  // connector, window size doesn't match a panel mode, allocation failed).
  // `display_index` is SDL's: KMSDRM enumerates one display per connected
  // connector in DRM resource order.
  bool open(SDL_Window* win, int w, int h, int display_index);

  bool reopen(SDL_Window* win, int w, int h) override {
    const int d = display_;
    close();
    return open(win, w, h, d);
  }
  void close() override;

  // The frame's own size as the buffers' size, scaled onto the panel by the
  // display engine (an atomic KMS plane; VOP2 on the RG DS filters, where the
  // CPU's scanline path only picks nearest pixels, and the present thread no
  // longer scales 512x384 up or 768x576 down into a 640x480 canvas). Reopens
  // when open; false (and the panel's size kept) where the plane cannot.
  bool set_source(int w, int h) override;
  int width() const override { return w_; }
  int height() const override { return h_; }
  int panel_width() const override { return pw_; }
  int panel_height() const override { return ph_; }
  int bufs() const override { return nbufs_; }
  void set_bufs(int n) { nbufs_ = n < 2 ? 2 : n > BUFS ? BUFS : n; }
  int current() const override { return cur_; }

  // Waits for a flip to retire only when no buffer is free; taken here
  // rather than end_frame() so the wait overlaps emulation, not the present.
  // begin_frame/end_frame/flush/close may run on different threads for
  // different panels (one present lane each); the tier serialises them.
  u32* begin_frame() override;
  bool dmabuf_plane(int buf, DmabufPlane& out) const override;
  void set_gpu_writes(bool on) override { gpu_writes_ = on; }
  void set_drop_if_pending(bool on) override { drop_if_pending_ = on; }
  void set_present_tag(int tag) override { tag_ = tag; }
  void end_frame()
      override; // flip now, or queue behind (or drop at, see set_drop_if_pending) the pending flip; does not wait
  void flush() override; // wait until no flip is queued behind a pending one

  // This panel's completed flips after number `after` (KmsFlips::Flip: the
  // DRM event's CLOCK_MONOTONIC stamp, when the flip was issued, the frame's
  // present tag, and how long the frame was queued behind the flip before
  // it -- the issue time is then that flip's vblank, not the frame's
  // completion, and the pacer must not read the queueing as work), oldest
  // first, at most `max` of the last KmsFlips::RING. The frame pacer locks
  // to them (phase_lock.h) as it does to the SDL renderer's.
  int recent(u64 after, KmsFlips::Flip* out, int max) const;

private:
  struct Buf {
    int fd = -1; // dma-heap buffer
    u32* px = nullptr;
    size_t bytes = 0;
    u32 handle = 0;    // GEM handle from the PRIME import
    u32 fb = 0;        // DRM framebuffer id
    bool busy = false; // on screen, flip pending, or queued
  };
  // Issue the page flip for bufs_[i], with the frame's tag; queued_at_ns:
  // when end_frame() queued the frame (0: flipped at once). False = driver error.
  bool flip(int i, int tag, u64 queued_at_ns);
  bool alloc_buf(Buf& b);
  void drop_buf(Buf& b);
  // Atomic KMS: the CRTC's primary plane and the property ids a commit
  // names, the modeset that puts the first buffer on the plane (scaled when
  // the buffers are not the panel's size), and a per-frame flip of FB_ID.
  bool atomic_setup(u32 crtc_index);
  u32 property(u32 object, u32 type, const char* name, u64* value = nullptr) const;
  bool atomic_modeset(bool test_only);
  bool atomic_flip(u32 fb);
  // Reads events on the shared DRM fd and routes each to its owning DrmOut
  // (a dual-window layout has one instance per panel). `block` waits for one.
  // Called with the tier's lock held (see display_drm.cpp), which it drops
  // around the wait.
  static bool pump(std::unique_lock<std::mutex>& lk, int fd, bool block);
  void retire(u64 at_ns); // our flip completed, at `at_ns` (the event stamp)

  int fd_ = -1; // SDL's DRM fd; not ours to close
  u32 crtc_ = 0, conn_ = 0;
  int display_ = 0;
  int w_ = 0, h_ = 0;   // the buffers
  int pw_ = 0, ph_ = 0; // the panel's mode
  int sw_ = 0, sh_ = 0; // set_source(): wanted buffer size (0: the panel's)
  SDL_Window* win_ = nullptr;
  bool atomic_ = false; // commits go through the atomic ioctl (needed to scale)
  u32 plane_ = 0, mode_blob_ = 0;
  u32 mode_[32] = {}; // the panel's mode (drmu::mode_modeinfo, 68 bytes fit) for the atomic modeset's blob
  struct Props {
    u32 fb_id = 0, plane_crtc_id = 0, src_x = 0, src_y = 0, src_w = 0, src_h = 0, crtc_x = 0, crtc_y = 0, crtc_w = 0,
        crtc_h = 0, mode_id = 0, active = 0, conn_crtc_id = 0;
  } props_;
  Buf bufs_[BUFS];
  int nbufs_ = DEFAULT_BUFS;
  bool gpu_writes_ = false;
  bool drop_if_pending_ = false;
  bool queued_may_drop_ = false;         // the queued frame's set_drop_if_pending()
  u64 queued_ns_ = 0;                    // when it was queued
  int tag_ = 0;                          // set_present_tag(): the frame between begin_frame() and end_frame()
  int queued_tag_ = 0, pending_tag_ = 0; // the tags of the queued frame and of the one whose flip is pending
  u64 pending_wait_ns_ = 0;              // how long the pending flip's frame was queued (0: flipped at once)
  u64 margin_ns_ = 2'000'000;            // DS_KMS_MARGIN_MS: queued longer than this at the vblank -> dropped (mailbox)
  u64 presented_ = 0, dropped_ = 0, queued_n_ = 0; // end_frame() outcomes, for the close report
  int cur_ = -1;                                   // buffer handed out by begin_frame()
  mutable std::mutex ring_m_;                      // the flip record below (pump() runs on whichever thread waits)
  KmsFlips::Flip ring_[KmsFlips::RING];
  u64 flips_ = 0, issue_ns_ = 0;
  int on_screen_ = -1; // buffer the CRTC is scanning out
  int pending_ = -1;   // buffer whose flip has not completed
  int queued_ = -1;    // buffer drawn while a flip was pending; flipped on its retire
  bool dead_ = false;  // driver error; stop submitting
};

} // namespace ds::sdl
