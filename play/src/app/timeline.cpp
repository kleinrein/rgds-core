// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/timeline.hpp"

#include <algorithm>
#include <cmath>
#include <zlib.h>

namespace play {

// Started here, not in the member list: every member it reads is set by now.
Timeline::Timeline() { worker_ = std::thread([this] { work(); }); }

Timeline::~Timeline() {
    {
        std::lock_guard<std::mutex> guard(lock_);
        stop_ = true;
    }
    wake_.notify_all();
    worker_.join();
}

void Timeline::configure(double frame_rate, const Options& options) {
    options_ = options;
    frame_rate_ = frame_rate > 1 ? frame_rate : 60;
    base_step_ = std::max<uint64_t>(1, uint64_t(std::lround(frame_rate_ * options_.seconds)));
    step_ = base_step_;
    due_ = position_ + step_;
}

void Timeline::work() {
    std::unique_lock<std::mutex> guard(lock_);
    for (;;) {
        wake_.wait(guard, [this] { return stop_ || job_id_ != 0; });
        if (stop_) return;
        const uint32_t id = job_id_;
        std::vector<uint8_t> raw;
        raw.swap(job_raw_);
        guard.unlock();
        uLongf size = compressBound(uLong(raw.size()));
        std::vector<uint8_t> packed(size);
        const bool ok = compress2(packed.data(), &size, raw.data(), uLong(raw.size()), 1) == Z_OK;
        packed.resize(ok ? size : 0);
        packed.shrink_to_fit();
        guard.lock();
        if (auto it = moments_.find(id); it != moments_.end()) {
            if (ok) {
                it->second->packed = std::move(packed);
                it->second->raw = uint32_t(raw.size());
                packed_bytes_ += it->second->packed.size();
            }
            it->second->ready = true; // an empty packed state never loads: jump says so
        }
        job_id_ = 0;
        done_.notify_all();
    }
}

void Timeline::settle() {
    std::unique_lock<std::mutex> guard(lock_);
    done_.wait(guard, [this] { return job_id_ == 0; });
}

Thumb Timeline::make_thumb(const Picture& p, int max_w, int max_h) {
    Thumb t;
    if (!p.px || p.w <= 0 || p.h <= 0) return t;
    // The game's own shape inside max_w x max_h.
    const double scale = std::min(double(max_w) / p.w, double(max_h) / p.h);
    t.w = std::max(1, int(p.w * scale));
    t.h = std::max(1, int(p.h * scale));
    t.px.resize(size_t(t.w) * t.h);
    // Each thumb pixel averages the 2x2 nearest source pixels around its centre: soft enough not to shimmer.
    for (int y = 0; y < t.h; ++y) {
        const int sy0 = std::min(p.h - 1, int((y + 0.25) * p.h / t.h)), sy1 = std::min(p.h - 1, int((y + 0.75) * p.h / t.h));
        for (int x = 0; x < t.w; ++x) {
            const int sx0 = std::min(p.w - 1, int((x + 0.25) * p.w / t.w)),
                      sx1 = std::min(p.w - 1, int((x + 0.75) * p.w / t.w));
            const uint32_t a = p.at(sx0, sy0), b = p.at(sx1, sy0), c = p.at(sx0, sy1), d = p.at(sx1, sy1);
            uint32_t out = 0xFF000000u;
            for (int s = 0; s < 24; s += 8) {
                const uint32_t sum = ((a >> s) & 0xFF) + ((b >> s) & 0xFF) + ((c >> s) & 0xFF) + ((d >> s) & 0xFF);
                out |= ((sum + 2) / 4) << s;
            }
            t.px[size_t(y) * t.w + x] = out;
        }
    }
    return t;
}

uint32_t Timeline::take(Core& core, const Picture& picture, bool wait) {
    if (!available_) return 0;
    {
        std::unique_lock<std::mutex> guard(lock_);
        if (job_id_ != 0) {
            if (!wait) return 0; // the worker is still on the last one: try again next frame
            done_.wait(guard, [this] { return job_id_ == 0; });
        }
    }
    if (core.still_loading()) return 0;
    std::vector<uint8_t> raw;
    if (!core.save_state(raw) || raw.empty()) {
        // A core that can't save at all has no rewind; one that refuses now and then (a tape loading) tries again.
        if (!sized_) {
            available_ = false;
            why_ = "This game can't be rewound";
        }
        return 0;
    }
    if (!sized_) {
        sized_ = true;
        if (raw.size() > options_.max_state) {
            available_ = false;
            why_ = "This game is too big to rewind";
            return 0;
        }
        // A big state is taken less often, so the game thread and memory spend about the same on any system.
        const size_t steps = std::max<size_t>(1, (raw.size() + options_.step_bytes - 1) / options_.step_bytes);
        step_ = base_step_ * steps;
    }
    auto stored = std::make_unique<Stored>();
    stored->m.id = next_id_++;
    stored->m.parent = head_;
    stored->m.position = position_;
    stored->m.thumb = make_thumb(picture, options_.thumb_w, options_.thumb_h);
    const uint32_t id = stored->m.id;
    {
        std::lock_guard<std::mutex> guard(lock_);
        packed_bytes_ += stored->m.thumb.px.size() * 4;
        moments_[id] = std::move(stored);
        job_id_ = id;
        job_raw_ = std::move(raw);
    }
    wake_.notify_all();
    head_ = id;
    evict();
    if (wait) settle();
    return id;
}

void Timeline::after_frame(Core& core, const Picture& picture) {
    ++position_;
    if (!available_) return;
    evict();
    if (position_ < due_) return;
    if (take(core, picture, false)) due_ = position_ + step_;
}

uint32_t Timeline::mark(Core& core, const Picture& picture) {
    // Just taken: the head is now.
    if (const Moment* h = find(head_); h && h->position == position_) {
        settle();
        return head_;
    }
    const uint32_t id = take(core, picture, true);
    if (id) due_ = position_ + step_;
    return id;
}

bool Timeline::jump(Core& core, uint32_t id, std::string& error) {
    settle();
    std::vector<uint8_t> raw;
    uint64_t position = 0;
    {
        std::lock_guard<std::mutex> guard(lock_);
        auto it = moments_.find(id);
        if (it == moments_.end()) {
            error = "that moment is no longer kept";
            return false;
        }
        const Stored& s = *it->second;
        if (s.packed.empty()) {
            error = "that moment could not be kept";
            return false;
        }
        raw.resize(s.raw);
        uLongf size = s.raw;
        if (uncompress(raw.data(), &size, s.packed.data(), uLong(s.packed.size())) != Z_OK || size != s.raw) {
            error = "that moment is damaged";
            return false;
        }
        position = s.m.position;
    }
    if (!core.load_state(raw)) {
        error = "the emulator did not take that moment";
        return false;
    }
    head_ = id;
    position_ = position;
    due_ = position_ + step_;
    return true;
}

void Timeline::evict() {
    std::lock_guard<std::mutex> guard(lock_);
    // Deflated bytes only: the one on the worker counts once it is done (the next frame's check).
    while (packed_bytes_ > options_.budget && moments_.size() > 2) {
        auto victim = moments_.end();
        for (auto it = moments_.begin(); it != moments_.end(); ++it)
            if (it->first != head_ && it->first != protected_ && it->first != job_id_) {
                victim = it; // the oldest: ids count up
                break;
            }
        if (victim == moments_.end()) return;
        const uint32_t gone = victim->first, parent = victim->second->m.parent;
        for (auto& [id, s] : moments_)
            if (s->m.parent == gone) s->m.parent = parent;
        packed_bytes_ -= victim->second->packed.size() + victim->second->m.thumb.px.size() * 4;
        moments_.erase(victim);
    }
}

const Moment* Timeline::find(uint32_t id) const {
    auto it = moments_.find(id);
    return it == moments_.end() ? nullptr : &it->second->m;
}

std::vector<uint32_t> Timeline::ids() const {
    std::vector<uint32_t> out;
    out.reserve(moments_.size());
    for (const auto& [id, s] : moments_) out.push_back(id);
    return out;
}

size_t Timeline::bytes() const {
    std::lock_guard<std::mutex> guard(lock_);
    return packed_bytes_;
}

} // namespace play
