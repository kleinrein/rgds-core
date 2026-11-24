// SPDX-License-Identifier: GPL-3.0-or-later
// The game's recent past as a tree of moments (docs/TIME_TRAVEL_PLAN.md). Once a second of play the whole machine
// is taken (on the game thread) and deflated (on a worker), with a small picture of the screen. Each moment knows the
// one before it; a jump back makes the next moment a child of the one jumped to, so the future that was left stays
// as a branch. Past the memory budget the oldest moments go, and their children hang from their parent instead.
#pragma once
#include "app/moments.hpp"
#include "core/core.hpp"

#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace play {

class Timeline final : public MomentTree {
public:
    struct Options {
        size_t budget = size_t(48) << 20;      // deflated states and thumbnails kept
        double seconds = 1.0;                  // between moments, for a state up to `step_bytes`
        size_t step_bytes = size_t(512) << 10; // each more of these in a state adds `seconds` to the step
        size_t max_state = size_t(24) << 20;   // a bigger state gets no rewind
        int thumb_w = 208, thumb_h = 156; // the Rewind screen's middle picture, so it is sharp; counted in the budget
    };

    Timeline();
    ~Timeline();
    Timeline(const Timeline&) = delete;
    Timeline& operator=(const Timeline&) = delete;

    void configure(double frame_rate, const Options& options);
    // After every live frame: takes a moment when one is due.
    void after_frame(Core& core, const Picture& picture);
    // A moment now, ready to jump to (the rewind screen's "now"); 0 when the game can't be taken.
    uint32_t mark(Core& core, const Picture& picture);
    // The game as it was at `id`; the next moment is then that one's child.
    bool jump(Core& core, uint32_t id, std::string& error);

    // Off: the game's state is too big, or the core could not save it.
    bool available() const { return available_; }
    const std::string& unavailable_reason() const { return why_; }
    uint32_t head() const { return head_; }
    uint64_t position() const { return position_; }
    uint64_t step_frames() const { return step_; }
    double frame_rate() const override { return frame_rate_; }

    // The tree, for the rewind screen (game thread only; the worker touches only the deflated bytes).
    const Moment* find(uint32_t id) const override;
    std::vector<uint32_t> ids() const override;
    size_t size() const { return moments_.size(); }
    size_t bytes() const;
    // Keeps `id` through evictions (the rewind screen's now), 0 for none.
    void protect(uint32_t id) { protected_ = id; }
    // Waits until every moment taken is deflated (tests, and before a jump).
    void settle();

private:
    struct Stored {
        Moment m;
        std::vector<uint8_t> packed;
        uint32_t raw = 0;
        bool ready = false;
    };
    uint32_t take(Core& core, const Picture& picture, bool wait);
    void evict();
    void work();
    static Thumb make_thumb(const Picture& p, int max_w, int max_h);

    Options options_;
    double frame_rate_ = 60;
    uint64_t base_step_ = 60, step_ = 60, position_ = 0, due_ = 0;
    bool available_ = true, sized_ = false;
    int refused_ = 0; // save states refused in a row before the first one was taken
    std::string why_;
    uint32_t next_id_ = 1, head_ = 0, protected_ = 0;
    std::map<uint32_t, std::unique_ptr<Stored>> moments_;

    // The worker: one state at a time is deflated; a moment due while it is busy waits a frame.
    mutable std::mutex lock_;
    std::condition_variable wake_, done_;
    std::thread worker_;
    bool stop_ = false;
    uint32_t job_id_ = 0;
    std::vector<uint8_t> job_raw_;
    size_t packed_bytes_ = 0;
};

} // namespace play
