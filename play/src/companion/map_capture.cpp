// SPDX-License-Identifier: GPL-3.0-or-later
#include "companion/map_capture.hpp"

#include <algorithm>
#include <cstdlib>
#ifdef __linux__
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace play {
namespace {

bool close_enough(uint32_t px, uint32_t rgb, int tolerance) {
    uint32_t want = rgb_to_px(rgb);
    for (int shift = 0; shift < 24; shift += 8) {
        int a = (px >> shift) & 0xFF, b = (want >> shift) & 0xFF;
        if (std::abs(a - b) > tolerance) return false;
    }
    return true;
}

bool pixels_match(const Picture& p, const MapSpec& spec) {
    if (spec.pixels.empty()) return true;
    int hits = 0;
    for (const auto& probe : spec.pixels) {
        if (probe.x < 0 || probe.y < 0 || probe.x >= p.w || probe.y >= p.h) continue;
        if (close_enough(p.at(probe.x, probe.y), probe.rgb, spec.tolerance)) ++hits;
    }
    int need = spec.pixels_min > 0 ? spec.pixels_min : static_cast<int>(spec.pixels.size());
    return hits >= need;
}

Rect crop_of(const Picture& p, const MapSpec& spec) {
    Rect r = spec.crop.empty() ? Rect{0, 0, p.w, p.h} : spec.crop;
    r.x = std::clamp(r.x, 0, p.w - 1);
    r.y = std::clamp(r.y, 0, p.h - 1);
    r.w = std::min(r.w, p.w - r.x);
    r.h = std::min(r.h, p.h - r.y);
    return r;
}

std::vector<uint32_t> cut(const Picture& p, const Rect& r) {
    std::vector<uint32_t> out(static_cast<size_t>(r.w) * r.h);
    for (int y = 0; y < r.h; ++y)
        std::copy_n(p.px + static_cast<size_t>(r.y + y) * p.stride + r.x, r.w,
                    out.begin() + static_cast<size_t>(y) * r.w);
    for (auto& v : out) v |= 0xFF000000u;
    return out;
}

int count_diff(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
    int n = 0;
    for (size_t i = 0; i < a.size(); ++i) n += (a[i] & 0xFFFFFF) != (b[i] & 0xFFFFFF);
    return n;
}

// The map's empty colour: the profile's, or the most common colour on the
// image's border (maps sit on a plain field; rooms rarely reach every edge).
uint32_t background_of(const std::vector<uint32_t>& img, int w, int h, const MapSpec& spec) {
    if (spec.has_background) return rgb_to_px(spec.background);
    std::vector<uint32_t> edge;
    for (int x = 0; x < w; ++x) {
        edge.push_back(img[x] & 0xFFFFFF);
        edge.push_back(img[static_cast<size_t>(h - 1) * w + x] & 0xFFFFFF);
    }
    for (int y = 0; y < h; ++y) {
        edge.push_back(img[static_cast<size_t>(y) * w] & 0xFFFFFF);
        edge.push_back(img[static_cast<size_t>(y) * w + w - 1] & 0xFFFFFF);
    }
    std::sort(edge.begin(), edge.end());
    uint32_t best = edge.empty() ? 0 : edge[0];
    size_t run = 0, best_run = 0;
    for (size_t i = 0; i < edge.size(); ++i) {
        run = i > 0 && edge[i] == edge[i - 1] ? run + 1 : 1;
        if (run > best_run) {
            best_run = run;
            best = edge[i];
        }
    }
    return best | 0xFF000000u;
}

void lower_priority() {
#ifdef __linux__
    // The capture shares the A55s with the live game; it can always wait.
    setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 10);
#endif
}

} // namespace

int picture_brightness(const Picture& p) {
    if (!p.px || p.w <= 0 || p.h <= 0) return 0;
    long sum = 0;
    int n = 0;
    for (int y = p.h / 16; y < p.h; y += p.h / 8)
        for (int x = p.w / 16; x < p.w; x += p.w / 8) {
            uint32_t v = p.at(x, y);
            sum += ((v & 0xFF) * 2 + ((v >> 8) & 0xFF) * 5 + ((v >> 16) & 0xFF)) / 8;
            ++n;
        }
    return n ? static_cast<int>(sum / n) : 0;
}

MapImage capture_map(Core& shadow, const MapSpec& spec, const std::vector<uint8_t>& state) {
    MapImage out;
    if (!shadow.load_state(state)) {
        out.why = "the snapshot did not load";
        return out;
    }
    shadow.set_layers(spec.layers);
    int frames = 0;
    for (; frames < spec.release; ++frames) shadow.run_frame(0);
    for (const auto& step : spec.open)
        for (int i = 0; i < step.frames; ++i, ++frames) shadow.run_frame(step.buttons);

    Rect crop;
    std::vector<uint32_t> previous, current;
    int stable = 0;
    bool seen = false, done = false;
    for (int i = 0; i < spec.timeout && !done; ++i, ++frames) {
        shadow.run_frame(0);
        Picture p = shadow.picture();
        bool up = (spec.ready.empty() || spec.ready.match(shadow)) && pixels_match(p, spec);
        if (!up) {
            stable = 0;
            previous.clear();
            continue;
        }
        seen = true;
        crop = crop_of(p, spec);
        current = cut(p, crop);
        if (!previous.empty() && count_diff(previous, current) <= spec.max_diff) ++stable;
        else stable = 1;
        done = stable >= spec.stable;
        previous.swap(current);
    }
    if (!done) {
        out.why = seen ? "the map never settled" : "the map did not appear";
        shadow.set_layers(0x1F);
        return out;
    }
    out.w = crop.w;
    out.h = crop.h;
    out.frames.push_back(previous);
    for (int f = 1; f < spec.anim_frames; ++f) {
        for (int i = 0; i < spec.anim_step; ++i, ++frames) shadow.run_frame(0);
        if (!spec.ready.empty() && !spec.ready.match(shadow)) break;
        auto next = cut(shadow.picture(), crop);
        if (count_diff(next, out.frames.front()) > spec.max_diff) break; // the map closed or scrolled
        out.frames.push_back(std::move(next));
    }
    shadow.set_layers(0x1F);
    bool animated = false;
    for (size_t f = 1; f < out.frames.size() && !animated; ++f) animated = out.frames[f] != out.frames[0];
    if (!animated) out.frames.resize(1);
    out.frame_step = spec.anim_step;
    out.background = background_of(out.frames[0], out.w, out.h, spec);
    if (spec.min_foreground > 0) {
        int fg = 0;
        for (uint32_t v : out.frames[0]) fg += (v & 0xFFFFFF) != (out.background & 0xFFFFFF);
        if (fg < spec.min_foreground) {
            out.why = "the map was blank";
            out.frames.clear();
            return out;
        }
    }
    out.frames_to_open = frames;
    out.ok = true;
    return out;
}

MapCapture::MapCapture(const Core& live, const Profile& profile) : profile_(profile), shadow_(live.shadow()) {
    if (shadow_ && profile_.has_map) thread_ = std::thread(&MapCapture::worker, this);
}

MapCapture::~MapCapture() {
    {
        std::lock_guard<std::mutex> g(lock_);
        quit_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

bool MapCapture::request(Core& live) {
    if (!thread_.joinable()) return false;
    {
        std::lock_guard<std::mutex> g(lock_);
        if (has_job_ || working_) return false;
    }
    std::vector<uint8_t> state;
    if (!live.save_state(state)) return false;
    {
        std::lock_guard<std::mutex> g(lock_);
        pending_.swap(state);
        has_job_ = true;
    }
    wake_.notify_one();
    return true;
}

bool MapCapture::busy() const {
    std::lock_guard<std::mutex> g(lock_);
    return has_job_ || working_;
}

bool MapCapture::poll(uint32_t serial, MapImage& out) const {
    std::lock_guard<std::mutex> g(lock_);
    if (latest_.serial <= serial || !latest_.ok) return false;
    out = latest_;
    return true;
}

int MapCapture::failures(std::string* why) const {
    std::lock_guard<std::mutex> g(lock_);
    if (why) *why = last_failure_;
    return failures_;
}

MapImage MapCapture::capture_now(Core& live) {
    MapImage img;
    if (!shadow_ || !profile_.has_map || busy()) return img;
    std::vector<uint8_t> state;
    if (!live.save_state(state)) return img;
    img = capture_map(*shadow_, profile_.map, state);
    std::lock_guard<std::mutex> g(lock_);
    if (img.ok) {
        img.serial = ++serial_;
        latest_ = img;
        failures_ = 0;
    } else {
        ++failures_;
        last_failure_ = img.why;
    }
    return img;
}

void MapCapture::worker() {
    lower_priority();
    std::unique_lock<std::mutex> g(lock_);
    for (;;) {
        wake_.wait(g, [&] { return quit_ || has_job_; });
        if (quit_) return;
        std::vector<uint8_t> state;
        state.swap(pending_);
        has_job_ = false;
        working_ = true;
        g.unlock();
        MapImage img = capture_map(*shadow_, profile_.map, state);
        g.lock();
        working_ = false;
        if (img.ok) {
            img.serial = ++serial_;
            latest_ = std::move(img);
            failures_ = 0;
        } else {
            ++failures_;
            last_failure_ = img.why;
        }
    }
}

} // namespace play
