#include <vector>
#include <string>
// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include <cstdio>
#include <cstdlib>
#include "core/types.h"
#include "core/gpu/engine2d.h"
#include "core/gpu/vram_map.h"
#include "core/gpu/line_worker.h"
#include "core/gpu/render3d.h"
#include "rgds_sample_plan.hpp"
#include "rgds_filter2d.hpp"
#include "rgds_hd2d_rules.hpp"
#include "rgds_compose_pool.h"
#include "core/profile.h"

#include <array>

namespace ds {
struct NDS;
}

namespace ds::gpu {

// Display timing, the two 2D engines and the output stage. 3D plugs in via
// Engine2D::set_3d_line().
//
// Lazy 2D: engines aren't rendered every HBlank. Writes are journaled per
// engine with the display line they first affect; the frame renders in one
// batch at the last line's HBlank, replaying the journal per line. A trapped
// VRAM store, VRAMCNT remap, or captured LCDC bank catches lines up and
// forces a per-line burst before re-batching.
//
// Framebuffers: 256x192 u32 per screen, 0xAARRGGBB, 8-bit channels expanded
// from the hardware's 6 bits.
class Gpu {
public:
  explicit Gpu(NDS& nds);
  void reset();

  // Scheduler callbacks.
  void on_scanline_start(); // VCOUNT advance, VBlank/VCount flags
  void on_hblank();         // render (lazily), HBlank flag
  void on_display_fifo(u32 x);
  void begin_frame();
  void async_probe_start();
  void async_probe_check(bool at_line0);
  bool probe_enabled_ = std::getenv("DS_ASYNC_PROBE") != nullptr;
  bool probe_open_ = false;
  u64 probe_hash_ = 0, probe_vramcnt_at_l0_ = 0,
      probe_vramcnt_base_ = 0; // latches POWCNT/FIFO/capture state; runs at line 0
  bool frame_begun() const { return frame_begun_; }
  bool at_line_start() const { return !hblank_done_; }

  // Save states, taken at line 0. quiesce() joins the worker and drains
  // journals without changing guest state; prepare_load() also lifts the VRAM
  // trap; after_load() re-takes the lazy/trap decision as begin_frame() did.
  void quiesce();
  void prepare_load();
  template <class S> void sync_state(S& s);
  void after_load();

  // 2D register file (0x04000000-0x0400006F, 0x04001000-0x0400106F) minus
  // DISPSTAT/VCOUNT, which stay with the interrupt logic in Io.
  static bool owns_reg(u32 addr) {
    const u32 r = addr - 0x04000000;
    if (r < 0x70) return (r >= 8 || r < 4) && (r < 0x60 || r >= 0x64); // 0x60 is DISP3DCNT
    return r >= 0x1000 && r < 0x1070 && (r < 0x1004 || r >= 0x1008);
  }
  u32 reg_read(u32 addr, u32 width);
  void reg_write(u32 addr, u32 width, u32 value);
  void set_powcnt(u16 value);

  // Journal stamp for a write to engine `e`: twice the first display line it
  // can affect, plus one if that line's scanline start already passed.
  // NO_STAMP means the write applies at once (nothing in flight, frame done).
  static constexpr u32 NO_STAMP = 0xFFFF;
  u32 journal_stamp(int e) const {
    const u32 l = hblank_done_ ? line_ + 1u : line_;
    return (l < SCREEN_H || inflight_[e]) ? l * 2 + (hblank_done_ ? 0 : 1) : NO_STAMP;
  }
  // Slow-path stores (Bus): a VRAM store on a trapped page catches the render up.
  void palette_store(Cpu cpu, u32 addr, u32 width, u32 value);
  void oam_store(Cpu cpu, u32 addr, u32 width, u32 value);
  void vram_store_trap(Cpu cpu, u32 addr);
  // After vram_store_trap: nothing in flight and both engines per-line, so the rest of a
  // DMA run on that page may write straight through a lag-mode trap (no hand-off until
  // the next HBlank event, which cannot fire inside a run).
  bool vram_trap_settled() const { return !inflight_[0] && !inflight_[1] && per_line_[0] && per_line_[1]; }
  bool vram_remap_begin(
      u32 moved_2d,
      const VramMap* next =
          nullptr); // before a VRAMCNT remap: catch up, lift the trap; returns whether it was set (`next`: the map being switched to)
  void vram_remap_end(bool trapped);             // after: re-arm it
  void set_lazy(bool on) { lazy_enabled_ = on; } // off: per-line rendering (tests)

  bool set_presentation(int scale, bool smooth, std::string* why = nullptr);
  // 2D scaling on the enhanced (2x-5x) panel image: 0 native grid, 1 sharp, 2 smooth, 3 crisp; 4 HD (5 HD smooth)
  // keeps the native grid and marks 2D-owned pixels for the bridge's GPU reconstruction. Display only.
  void set_filter2d(int mode);
  int filter2d() const { return filter2d_.load(std::memory_order_relaxed); }
  static constexpr u32 PRESENT_W = 640, PRESENT_H = 480;
  bool enhanced_screen(int screen) const { return enhanced_screen_[screen]; }
  // RG DS: time composition stood waiting for the GPU's 3D since the last call (adaptive resolution's GPU-bound test).
  // A capture frame's derived native layer waits for the GPU there instead (Renderer3D::take_derive_wait_ns).
  u64 take_hires_wait_ns();
  // HD (rgds_2d=4 or 5): the CPU's HD rules for the screen's last finished frame (rgds::Hd2DRules words, 256x192), which
  // the bridge's GPU reconstruction draws from; null when HD is off or the screen has none yet.
  const u32* hd_rules(int screen) const {
    return hd_mode() && hd_valid_[screen] ? hd_rules_[screen].data() : nullptr;
  }
  const u32* enhanced_framebuffer(int screen) const {
    return enhanced_[screen].empty() ? nullptr : enhanced_[screen].data();
  }
  const u32* framebuffer(int screen) const { return fb_[screen].data(); } // 0 = top, 1 = bottom

  // A screen the frontend does not show. Its driving engine skips drawing but
  // keeps journal/latches/windows/lazy-2D bookkeeping running so it's exact
  // once shown again. Engine A never skips. Set between frames only.
  void set_screen_visible(int screen, bool on) { screen_visible_[screen] = on; }

  // Frameskip (frontend policy). A skipped frame runs unchanged, only
  // omitting line rendering/output and the 3D raster feeding it. Decided one
  // frame ahead, at line 215. Never skips a frame that captures or feeds the FIFO.
  void set_frame_skip(bool on) { skip_req_ = on; }
  // Skip frames that display-capture too (INEXACT): a game reading captured
  // pixels back with the CPU sees an older frame than hardware.
  void set_frameskip_capture(bool on) { skip_capture_ok_ = on; }

  // Frames until the display setup repeats. Drawing at a multiple of this
  // period freezes one screen on its off-frame, so keep the drawn cadence off
  // a multiple of it (1 = no alternation).
  u8 display_phase_period() const { return phase_period_; }
  bool lines_in_flight() const { return inflight_[0] || inflight_[1] || scale_inflight_; }
  bool lag_active() const { return lag_frame_ && !lazy_frame_; }
  // Bus::vram_read on an LCDC page under the capture read trap: join in-flight lines if reading the bank capture writes.
  bool lcdc_read_trapped() const { return read_trap_bank_ >= 0; }
  // A guest read reached the capture's bank through a mapping a remap gave
  // it while the capturing job is in flight: finish the job first.
  void capture_read_hit();
  void release_read_trap(); // whichever form the capture read trap is in
  void lcdc_read_hit(u32 addr) {
    if (static_cast<int>((addr >> 17) & 7) == read_trap_bank_) {
      prof::add(prof::C_2D_A_JOIN_READS, 1);
      join_worker(JoinSite::Trap);
    }
  }
  bool will_skip_frame() const { return skip_frame_; }
  bool frame_skip_safe() const { return skippable(); }

  // A frontend-owned, panel-sized destination for one screen. When set, the
  // output stage scales each line into it as produced instead of filling fb_.
  // Chunky with a cell of P panel pixels: cells_x * cells_y cells, each an
  // area-weighted box of the DS pixels it covers. Per axis, cell i takes taps
  // first[i]..first[i]+n[i]-1 with weights w[i][..] summing to 256.
  static constexpr u32 CELL_TAPS = 8;
  struct CellAxis {
    u32 cells = 0, cell_px = 0; // count, and panel pixels per cell
    std::vector<u16> first;     // per cell
    std::vector<u8> n;
    std::vector<u16> w; // cells * CELL_TAPS
  };
  struct CellMap {
    CellAxis x, y;
  };
  // Fills `a` for `cells` cells over `src_n` source pixels, `cell_px` panel
  // pixels each. False if a cell would need more than CELL_TAPS taps.
  static bool build_cell_axis(u32 src_n, u32 cells, u32 cell_px, CellAxis& a);

  struct ScaleTarget {
    u32* px = nullptr;             // top-left of this screen's rect in the frontend's buffer
    u32 pitch = 0;                 // destination pitch, in u32
    u32 h = 0;                     // destination rect height, in pixels
    const u16* xrun = nullptr;     // 257 entries; see kern::scale_row
    u32 grid = 256;                // LCD grid: brightness kept on the grid lines, 0..256 (256 = no grid)
    u8 chunky = 0;                 // 0 off; else 2x2 block -> one cell: 1 top-left, 2 mean, 3 dominant colour,
                                   // 4 darkest, 5 brightest, 6 farthest-from-mean luma beyond chunky_thresh (else mean)
    u32 chunky_thresh = 180 * 256; // luma units (0..255 * 256); mode 6 only
    u8 blend = 0;                  // box-filter seams: 1 blend in sRGB, 2 in linear light
    const u8* seam_w = nullptr; // 256 entries: weight (0..255 = 0..1) of pixel s+1 in run s's last pixel -- the part of
                                // that pixel past the s|s+1 boundary, 1 - frac((s+1)*w/256); 0 = no straddle
    const CellMap* cells = nullptr; // chunky with a panel-sized cell (see CellMap); null = the 2x2 pair path
    // Bilinear: blends the 2x2 source pixels around each sample point; takes
    // precedence over grid/seams/chunky. Column x samples between lin_sx[x]
    // and lin_sx[x]+1 at weight lin_wx[x]/256 (lin_sx <= 254).
    bool bilinear = false;
    const u16* lin_sx = nullptr;
    const u8* lin_wx = nullptr;
    // Rows of the rect, [y_lo, y_hi) (y_hi 0 = h: no crop); px points at row y_lo.
    u32 y_lo = 0, y_hi = 0;
  };
  // Both screens or neither: pass a null `px` to go back to fb_.
  void set_scale_target(int screen, const ScaleTarget& t) {
    scale_[screen] = t;
    if (scale_[screen].y_hi == 0) scale_[screen].y_hi = t.h;
  }
  // Runs a whole DS-resolution image (e.g. a frontend pause menu) through the
  // scanline scaler with the game's grid/chunky/seam treatment. No-op without a scale target.
  void scale_image(int screen, const u32* src);
  bool scaling() const { return scale_[0].px && scale_[1].px; }
  u16 line() const { return line_; }

  Engine2D engine[2];

  // Both screens forced white by MASTER_BRIGHT (mode 1, full factor). Reading
  // the register beats scanning fb_ (pixels go white a frame later there).
  bool screens_forced_white() const {
    for (u16 mb : master_bright_g_)
      if ((mb >> 14) != 1 || (mb & 0x1F) < 16) return false;
    return true;
  }

private:
  NDS& nds_;
  u16 line_ = 0;
  bool hblank_done_ = false; // this line's HBlank event has run (its render, if any, is behind us)
  bool frame_begun_ = false;
  bool screens_on_ = false; // POWCNT1 bit 0, latched at frame start
  bool screen_visible_[2] = {true, true};
  bool skipped_[2] = {false, false}; // this engine's last line was skipped: its next drawn line re-renders its sprites
  bool skip_req_ = false;            // frameskip: the frontend's request, taken at line 215
  bool skip_next_ = false;           // taken there: the next frame's display lines are skipped
  bool skip_frame_ = false;          // latched in begin_frame from skip_next_, gated by skippable()
  // Never skip a frame the guest reads back; capture_recent_ extends that a
  // few frames after the last capture too (3D raster skips a frame ahead of the display it feeds).
  static constexpr u8 CAPTURE_STICKY = 8;
  u8 capture_recent_ = 0;
  bool skip_capture_ok_ = false;
  bool skippable() const { return !run_fifo_ && (skip_capture_ok_ || (!capture_on_ && !capture_recent_)); }
  // RGDS: the enhanced path may defer the native 3D raster only while nothing
  // guest-visible reads it. Unlike skippable(), frameskip_capture never
  // relaxes this: a capture of a deferred raster copies an empty 3D layer.
  bool native_defer_safe() const { return !run_fifo_ && !capture_on_ && !capture_recent_; }
  // ...and where capture rules that out, it may derive the native layer from
  // the GPU image (Renderer3D::allow_native_derive, INEXACT captures).
  // Opt-in for now: RGDS_CAPTURE_GPU=1 (read once in the constructor).
  bool derive_capture_ = false;
  bool native_derive_safe() const { return derive_capture_ && !run_fifo_; }
  // Display-phase detection: signature of the last PHASE_HISTORY frames, newest last.
  static constexpr u32 PHASE_HISTORY = 8, PHASE_MAX = 4;
  u32 phase_sig_[PHASE_HISTORY] = {};
  u32 phase_seen_ = 0;
  u8 phase_period_ = 1;
  void update_phase();
  u16 master_bright_g_[2] = {0, 0}; // guest-visible; the engines hold the render-side value
  u32 capcnt_ = 0;
  bool capture_on_ = false;
  std::array<u16, 16> fifo_{};
  u8 fifo_rd_ = 0, fifo_wr_ = 0;
  alignas(16) std::array<u16, 256> fifo_line_{};
  bool run_fifo_ = false;
  std::array<std::array<u32, SCREEN_W * SCREEN_H>, 2> fb_{};
  ScaleTarget scale_[2];
  static constexpr u32 SCALED_ROW_MAX = 4096;
  alignas(16) u32 chunk_even_[2][SCREEN_W]; // chunky: the even line, held until the odd one completes the block
  alignas(16) u32 seam_prev_[2][SCREEN_W];  // blend: the previous source row, for the straddling row
  u32 seam_prev_line_[2] = {~0u, ~0u};
  // Cell chunky: last CELL_TAPS source lines, a ring by line number.
  alignas(16) u32 cell_lines_[2][CELL_TAPS][SCREEN_W];
  u32 cell_row_[2] = {0, 0}; // the cell row being gathered
  void emit_cells(int screen, u32 line, const u32* src);
  void emit_row_straddle(const ScaleTarget& t, const u32* src, u32* dst);
  // Bilinear: previous and current source lines widened horizontally, per
  // screen; a destination row between them is a lerp of the pair.
  alignas(16) u32 lin_row_[2][2][SCALED_ROW_MAX];
  u32 lin_cur_[2] = {0, 0}; // which of lin_row_[screen] holds the current line
  u32 lin_prev_line_[2] = {~0u, ~0u};
  void emit_bilinear(int screen, u32 line, const u32* src);
  void blend_rows(const ScaleTarget& t, const u32* a, const u32* b, u32 w, u32* out);
  // output_line writes here instead of fb_ when scaling; scale_row reads it back hot.
  alignas(16) std::array<std::array<u32, SCREEN_W>, 2> line_out_{};
  // Scaled row staged here before the target (scanout memory, uncached CMA):
  // duplicating straight out of it would read that memory back.
  alignas(16) u32 row_scratch_[2][SCALED_ROW_MAX];
  const u32* line3d_ = nullptr; // 3D output for the line being drawn (whichever thread draws engine A)

  // Lazy-2D state for the frame in progress.
  bool lazy_enabled_ = true;
  bool lazy_frame_ = false; // this frame may batch
  // Per engine: a trapped store takes only the engine its address reaches out
  // of batched mode (a store into one engine's BG/OBJ VRAM cannot change what
  // the other fetches).
  bool per_line_[2] = {false, false};
  bool per_line_prev_[2] = {false, false};
  u32 render_next_[2] = {SCREEN_H, SCREEN_H};
  bool frame_finished_ = false; // frame_done() called for both engines
  bool trap_armed_ = false, trap_lcdc_ = false;
  u32 trap_mask_ = 0; // windows trapped: bit 0 engine A's, bit 1 engine B's (Bus::set_vram_trap)
  // Per-job VRAM map snapshots (0: worker_'s job, 1: worker_b_'s): a batch
  // renders lines the panel already scanned against the map as it was when
  // the job was handed, so a remap can rebuild the live map without joining
  // -- when the write trap still covers every bank the job reads
  // (vram_remap_begin). Copied only when the map's generation changed.
  VramMap job_map_[2];
  u32 job_map_gen_[2] = {~0u, ~0u};
  bool remap_pending_[2] = {false, false}; // engine's vram_remapped() owed at the join
  // A remap went through under a job in flight: the job reads the map as it
  // was, so a trapped store is taken as reaching every in-flight engine
  // (reach_engines answers for the live map's windows). Cleared at the join.
  bool snapshot_stale_ = false;
  void snapshot_map(int k);
  // Engines an address can change: bit 0 A, bit 1 B; LCDC only A (and only when trapped); else both.
  u32 reach_engines(u32 addr) const {
    if ((addr >> 24) != 0x06) return 3;
    if (addr >= 0x06800000) return trap_lcdc_ ? 1 : 0;
    return ((addr >> 21) & 1) ? 2 : 1;
  }
  // Capture frames batch too. A trapped store renders LAZY_BURST_LINES per
  // line, then re-arms; past LAZY_BURST_LIMIT bursts the frame stays per line.
  bool burst_[2] = {false, false}; // per-line for the current burst of stores
  u32 burst_left_[2] = {0, 0};     // display lines left before re-batching
  u32 lazy_bursts_[2] = {0, 0};
  static constexpr u32 LAZY_BURST_LIMIT = 16, LAZY_BURST_LINES = 8;
  // Per-engine futility (restored with the split from 508b7bc; the both-engines
  // skip of 8bdcd13 it replaces never fired once stores were charged per engine,
  // across 46 titles). An engine that ended LAZY_FUTILE_LIMIT frames in a row
  // per line starts its frames per line, bar a re-probe every LAZY_PROBE_PERIOD
  // frames; both futile means the frame does not batch. Golden Sun streams
  // engine B's BG every scanline (HBlank DMA): B goes per line, engine A --
  // 3D composite and capture -- keeps its batch. The trap follows (arm_trap):
  // only the windows of a batching engine, and A's in a lag frame.
  static constexpr u32 LAZY_FUTILE_LIMIT = 4, LAZY_PROBE_PERIOD = 64;
  u32 eng_futile_[2] = {0, 0};
  u32 eng_probe_in_[2] = {LAZY_PROBE_PERIOD, LAZY_PROBE_PERIOD};
  bool lazy_tried_ = false; // last frame batched (its end state feeds eng_futile_)
  u32 frontier() const { return hblank_done_ ? line_ + 1u : line_; } // first line a write now can still affect
  void catch_up(u32 mask);           // render the masked engines' lines below the frontier
  void fall_back_per_line(u32 mask); // catch up and render the rest of the frame per line
  void arm_trap();
  void disarm_trap();
  // Per-engine ranges; first > last means "nothing pending for that engine".
  // Engine B's range goes to the worker and overlaps engine A's here.
  void render_ranges(u32 af, u32 al, u32 bf, u32 bl);
  // Which engines a VRAM store can change: bit 0 engine A, bit 1 engine B.
  // Only the fixed BG/OBJ address ranges are attributed; anything else (LCDC,
  // an unmapped alias) is charged to both, so the split can only ever be more
  // conservative than the address map.
  void step_engine(int e, u32 line); // one engine's display line: replay, latches, render, output

  // One worker thread beside the emulation thread, drawing a run of one
  // engine's display lines (worker_job); which engine depends on the frame.
  //
  // A batched frame hands engine A's 192 lines over at the last line's
  // HBlank without waiting (A carries the 3D composite, capture and half of
  // frontend scaling; B's batch draws here). Join is deferred to line 0.
  // Anything the guest does meanwhile that in-flight lines could observe
  // joins earlier: a trapped VRAM store, a VRAMCNT remap, a read of a
  // batched-capture bank, a full journal, a save state. VBlank writes and
  // latches go through the journal, applied in order at the join.
  //
  // Per-line frames (capture per line, display FIFO, a VRAM trap) run engine
  // A here and hand engine B's lines to the worker, as before.
  //
  // The two engines share no mutable state, so a line sees exactly the state
  // the sequential order would.
  LineWorker worker_;
  // Split mode (with the thread layout, or DS_2D_BWORKER=1): engine B's batch in
  // a frame whose engine A batch is deferred goes to its own worker (paired
  // with a 3D band on its core) instead of this thread, deferred to the same
  // join. Its frame end (frame_done) moves to that join too.
  LineWorker worker_b_;
  bool split_ = false;
  bool b_deferred_ = false; // engine B's batch is in flight on worker_b_ past line 191
  u32 bjob_first_ = 1, bjob_last_ = 0;
  static void worker_b_job(void* self);

public:
  // DS_WATCHDOG: where the display pipeline stands when a frame stalls.
  void debug_dump(FILE* f);

private:
  // The job: per engine, a run of lines (first > last = nothing for it).
  u32 job_first_[2] = {1, 1}, job_last_[2] = {0, 0};
  bool inflight_[2] = {false, false}; // that engine's lines are on the worker
  bool a_deferred_ = false;           // engine A's whole-frame batch is in flight past line 191 (finish_a at the join)
  // Frame-level capture state latched at each render_ranges: DISPCAPCNT's
  // enable bit clears at line 192 while a batched engine A is still drawing.
  u32 capcnt_render_ = 0;
  bool capture_render_ = false;
  // Gpu::capture() runs on the WORKER; vram_remap_begin() joins before a
  // remap changes vram_map_, so latching lcdc_mask_render_ here keeps capture off the shared object.
  u32 lcdc_mask_render_ = 0;
  int read_trap_bank_ = -1;        // LCDC bank under the capture read trap, or -1
  bool read_trap_generic_ = false; // ... as Bus::set_bank_read_trap (every page the bank backs), after a remap
  bool read_trap_reapply_ = false; // vram_remap_end re-applies it (generic) over the rebuilt table
  // Engine B's scaling, handed to the worker with engine A's lagged lines:
  // the join is only for buffer reuse and frame end. Lines drawn here in lag
  // mode stash their output (output_engine); the worker scales it after engine A's lines.
  struct StashedLine {
    u32 line;
    int screen;
    alignas(16) u32 px[SCREEN_W];
  };
  StashedLine bscale_[SCREEN_H];
  u32 bscale_n_ = 0;            // lines stashed for the job being built / in flight
  bool bscale_defer_ = false;   // output_engine stashes engine B's line instead of scaling it
  bool scale_inflight_ = false; // the worker holds a stash to scale
  Renderer3D::FrameRef ref3d_;  // the 3D frame these display lines read (begin_frame)
  // Lag mode: per-line frames hand BOTH engines' line to the
  // worker at its HBlank without waiting, joining at that line's HBlank or
  // earlier if observed. The last display line always joins. Past
  // LAG_TRAP_LIMIT joining stores, or LAG_STORE_LIMIT trapped stores of any
  // kind (each is a slow-path store, whether or not it had to wait), in a frame,
  // lag is dropped and the trap lifted. Display FIFO frames stay on this thread.
  bool lag_frame_ = false; // this frame's per-line lines may stay in flight
  u32 lag_trap_hits_ = 0, lag_trap_stores_ = 0;
  static constexpr u32 LAG_TRAP_LIMIT = 4096;
  static constexpr u32 LAG_STORE_LIMIT = 1024;
  // Wait for whatever is on the worker. Engine A's deferred batch also ends
  // its frame here (finish_a), as render_ranges does for a frame finished on this thread.
  enum class JoinSite {
    CatchUp,
    Trap,
    Journal,
    Line0,
    Remap,
    RangesPre,
    RangesPost,
    Other
  }; // where a join was taken from, to attribute its wait
  void join_worker(JoinSite site = JoinSite::Other);
  u64 join_wait_ns_ = 0;
  void finish_a();

public:
  void journal_full() { join_worker(JoinSite::Journal); } // Engine2D::queue on a full journal
private:
  static void worker_job(void* self);

  bool presentation_smooth_ = true;
  std::atomic<int> filter2d_{0};
  int pending_row_ = -1, snapshot_row_ = -1; // filtered rows wait one line for their lower neighbour
  u32 snapshot_run_ = 0; // consecutive snapshotted lines ending at snapshot_row_ (Crisp reads two above)
  std::atomic<bool> enhanced_screen_[2] = {}; // set by compose helpers; read after the line-0 join
  std::vector<u32> enhanced_[2];
  void output_enhanced(int e, u32 line, const u32* native);
  void output_enhanced_a(Engine2D& en, u32 line, const u32* native);
  // Dual-screen 3D: engine A alternates screens each frame and engine B shows
  // a display capture of A's previous frame on the other one. Where B's line
  // matches what A drew there, the panel keeps A's enhanced pixels.
  std::vector<u32> a_native_[2];
  u64 enhanced_frame_ = 0, a_enhanced_frame_[2] = {~0ull, ~0ull};
  bool b_reuse_[2] = {};
  void reuse_enhanced_b(int screen, u32 line, const u32* native);
  // One native line of engine A's enhanced output, snapshotted for composition.
  struct EnhancedRow {
    Engine2D::EnhancedPlan plan;
    u32 evy = 0;
    u16 master = 0;
    u32 line = 0, y0 = 0, y1 = 0, scale = 1, xpos = 0;
    u32* image = nullptr;
    bool filter = false;
    // RG DS: composed off engine A without waiting for the GPU. The native
    // line stands in where the enhanced 3D turns out to be empty.
    int screen = 0;
    bool native_3d = false;
    // 2D scaling: the neighbouring native lines (this line's own at the image edges).
    // `plain` rows carry no 3D plan and are only resampled.
    u8 filter2d = 0;
    bool plain = false;
    bool marks = false; // HD: mark the pixels 2D owns (blue's lowest bit)
    const u32* above2 = nullptr;
    const u32* above = nullptr;
    const u32* below = nullptr;
    alignas(16) u32 native[SCREEN_W];
  };
  // This frame's enhanced 3D record, resolved once by the first thread that
  // needs it (a compose helper, normally long after engine A drew the line).
  std::mutex hires_m_;
  std::atomic<bool> hires_resolved_{false};
  const u32* hires_ptr_ = nullptr;
  std::atomic<u64> hires_wait_ns_{0};
  const u32* frame_hires();
  int a_screen_now_ = -1; // the screen engine A drew this frame (dual-screen 3D bookkeeping at the join)
  static void native_rows(u32* image, u32 y0, u32 y1, const u32* native);
  static void mark_rows(u32* image, u32 y0, u32 y1, const Engine2D::EnhancedPlan* plan);
  // HD rules (rgds_hd2d_rules.hpp), per screen: the native frames and their 2D ownership, double-buffered so a band
  // of 8 lines can tell whether its lines (and the 2 around them) changed since its rules were made. A band is
  // published to the compose pool once the frame holds the 2 lines below it; both engines publish, under a lock.
  static constexpr u32 kHdBandLines = 8, kHdBands = SCREEN_H / kHdBandLines;
  static constexpr u32 kHdJob = 1u << 16; // compose job id: kHdJob | engine << 8 | band
  std::vector<u32> hd_native_[2][2], hd_rules_[2];
  std::vector<u8> hd_owner_[2][2];
  u8 hd_parity_[2] = {};
  int hd_screen_[2] = {-1, -1}; // per engine: the screen it draws this frame
  u32 hd_next_band_[2] = {};    // per engine: the next band to publish
  bool hd_valid_[2] = {};       // the screen's rules describe a finished frame
  std::array<std::array<u8, kHdBands>, 2> hd_fresh_{}; // the band's rules match the previous frame's lines
  std::atomic_flag hd_publish_lock_ = ATOMIC_FLAG_INIT;
  bool hd_mode() const { return filter2d_.load(std::memory_order_relaxed) >= int(rgds::Filter2D::HD); }
  void hd_start();
  void hd_line(int e, int screen, u32 line, const u32* native, const u8* owner);
  void hd_publish(u32 job);
  void hd_band(int e, u32 band);
  static void filtered_rows(const EnhancedRow& job);
  static void apply_2d_filter(const EnhancedRow& job, const u32* two_d, const u32* filtered);
  void submit_row(u32 line, Engine2D* verify);
  void submit_pending_row(u32 next_line);
  std::vector<EnhancedRow> enhanced_rows_;
  void enable_enhanced();
  static void compose_job(void* self, u32 job);
  void compose_row(const EnhancedRow& row, Engine2D* verify);
  ComposePool compose_; // after the rows: its threads stop before they go
  void output_engine(int e, u32 line);
  void emit_scaled(int screen, u32 line, const u32* src);
  void output_a(u32 line, u32* dst);
  void output_b(u32* dst);
  void capture(u32 line);
  void apply_master_brightness(u16 reg, u32* dst);
  void expand_colours(u32* dst);
  bool uses_fifo() const;
  void sample_fifo(u32 offset, u32 count);
};

constexpr u32 HBLANK_START = (48 + 256 * 6) * 2; // system cycles into the line (melonDS), in ARM9 cycles

} // namespace ds::gpu
