// SPDX-License-Identifier: GPL-3.0-or-later
// KmsFlips against a pipe standing in for SDL KMSDRM's DRM fd: a fake
// "kernel" writes flip-complete events carrying the user data SDL gave the
// flip (its window's waiting_for_flip), and fake "SDL" swaps set that flag
// the way KMSDRM_GLES_SwapWindow does.
#include "check.h"
#include "frontend/sdl/drm_uapi.h"
#include "frontend/sdl/kms_flips.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <mutex>
#include <random>
#include <thread>

#include <unistd.h>

using ds::sdl::KmsFlips;
namespace drmu = ds::sdl::drmu;

namespace {

void write_event(int fd, volatile int* flag, uint32_t crtc) {
  timespec t{};
  clock_gettime(CLOCK_MONOTONIC, &t);
  drmu::event_vblank v{};
  v.base.type = drmu::EVENT_FLIP_COMPLETE;
  v.base.length = sizeof v;
  v.user_data = reinterpret_cast<uintptr_t>(flag);
  v.tv_sec = static_cast<uint32_t>(t.tv_sec);
  v.tv_usec = static_cast<uint32_t>(t.tv_nsec / 1000);
  v.crtc_id = crtc;
  CHECK(write(fd, &v, sizeof v) == static_cast<ssize_t>(sizeof v));
}

// Flips complete in queue order, each `delay` after it was queued.
struct Kernel {
  int fd;
  std::mutex m;
  std::condition_variable cv;
  std::deque<std::pair<std::chrono::steady_clock::time_point, volatile int*>> q;
  bool quit = false;
  std::thread t;
  explicit Kernel(int fd_) : fd(fd_), t([this] { run(); }) {}
  ~Kernel() {
    {
      std::lock_guard<std::mutex> lk(m);
      quit = true;
    }
    cv.notify_all();
    t.join();
  }
  void flip(volatile int* flag, std::chrono::microseconds delay) {
    std::lock_guard<std::mutex> lk(m);
    q.emplace_back(std::chrono::steady_clock::now() + delay, flag);
    cv.notify_all();
  }
  void run() {
    std::unique_lock<std::mutex> lk(m);
    for (;;) {
      cv.wait(lk, [&] { return quit || !q.empty(); });
      if (q.empty()) return;
      const auto at = q.front().first;
      if (std::chrono::steady_clock::now() < at) {
        cv.wait_until(lk, at);
        continue;
      }
      volatile int* f = q.front().second;
      q.pop_front();
      write_event(fd, f, 7);
    }
  }
};

// One SDL swap of a window that already has a mode set: SDL asserts here
// that it has no flip to wait for (it would poll the fd otherwise), queues
// the flip, then marks it pending.
void swap(Kernel& k, volatile int& flag, std::chrono::microseconds delay) {
  CHECK(flag == 0);
  k.flip(&flag, delay);
  flag = 1;
}

} // namespace

int main() {
  // Learning, then two windows on two threads.
  {
    int p[2];
    CHECK(pipe(p) == 0);
    KmsFlips f(p[0], 500'000'000, 200'000'000);
    Kernel k(p[1]);
    volatile int top = 0, bottom = 0;
    const int a = f.add(), b = f.add();
    CHECK(a == 0 && b == 1);
    CHECK(!f.ready(a) && !f.ready(b));
    // First present: SDL sets the mode, no flip.
    f.before(a);
    f.after(a);
    f.before(b);
    f.after(b);
    CHECK(!f.ready(a));
    // Second: one flip each, learned one at a time.
    f.before(a);
    swap(k, top, std::chrono::microseconds(3000));
    f.after(a);
    CHECK(f.ready(a) && top == 0);
    f.before(b);
    swap(k, bottom, std::chrono::microseconds(3000));
    f.after(b);
    CHECK(f.ready(b) && bottom == 0);

    std::atomic<int> errors{0};
    auto lane = [&](int slot, volatile int& flag, unsigned seed) {
      std::mt19937 rng(seed);
      for (int i = 0; i < 300; ++i) {
        f.before(slot);
        if (flag != 0) ++errors;
        // 0 us: the event can be read (by the other lane) before "SDL" sets the flag.
        swap(k, flag, std::chrono::microseconds(rng() % 4 == 0 ? 0 : rng() % 3000));
        f.after(slot);
        std::this_thread::sleep_for(std::chrono::microseconds(rng() % 1500));
      }
      f.before(slot);
    };
    std::thread t1([&] { lane(a, top, 1); });
    std::thread t2([&] { lane(b, bottom, 2); });
    t1.join();
    t2.join();
    CHECK(errors == 0);
    CHECK(top == 0 && bottom == 0);
    for (int s : {a, b}) {
      const KmsFlips::Stats st = f.stats(s);
      CHECK(st.presents == 302);
      CHECK(st.flips == 301);
      CHECK(st.timeouts == 0);
    }
    CHECK(f.strays() == 0);
    CHECK(!f.failed());
    f.report(a, "top", stderr);
    // An event for no known window (none is learning) is cleared like SDL would.
    volatile int other = 1;
    write_event(p[1], &other, 9);
    f.before(a);
    swap(k, top, std::chrono::microseconds(1000));
    f.after(a);
    f.before(a);
    CHECK(other == 0 && f.strays() == 1);
    close(p[1]);
    close(p[0]);
  }
  // A pending flip that never completes: before() gives up and clears the flag.
  {
    int p[2];
    CHECK(pipe(p) == 0);
    KmsFlips f(p[0], 50'000'000, 200'000'000);
    volatile int flag = 0;
    const int a = f.add();
    f.before(a);
    f.after(a);
    f.before(a);
    flag = 1;
    write_event(p[1], &flag, 7);
    f.after(a); // learned
    CHECK(f.ready(a));
    f.before(a);
    flag = 1;
    f.after(a); // queued, never completes
    const auto t0 = std::chrono::steady_clock::now();
    f.before(a);
    CHECK(std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(45));
    CHECK(flag == 0 && f.stats(a).timeouts == 1);
    close(p[1]);
    close(p[0]);
  }
  // No events at all: learning gives up after three tries and SDL waits itself.
  {
    int p[2];
    CHECK(pipe(p) == 0);
    KmsFlips f(p[0], 50'000'000, 20'000'000);
    const int a = f.add();
    for (int i = 0; i < 4; ++i) {
      f.before(a);
      f.after(a);
    }
    CHECK(f.failed() && !f.ready(a));
    close(p[1]);
    close(p[0]);
  }
  std::printf("kms_flips ok\n");
  return 0;
}
