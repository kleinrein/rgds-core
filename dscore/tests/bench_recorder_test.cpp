// BenchRecorder: the numbers the Impeccable benchmark ROM scores (bench/ds).
#include "check.h"
#include "frontend/sdl/bench_recorder.h"

using ds::u32;
using ds::u64;
using Rec = ds::sdl::BenchRecorder;

static const u64 REFRESH = 16'666'667;

static std::vector<u64> grid(int count, int skip_at = -1, int skipped = 0) {
  std::vector<u64> at;
  u64 t = 1'000'000'000;
  for (int i = 0; i < count; ++i) {
    at.push_back(t);
    t += REFRESH * (i == skip_at ? 1 + skipped : 1);
  }
  return at;
}

int main() {
  { // A perfect run misses nothing, and its longest gap is one refresh.
    const auto p = Rec::read_panel(grid(600));
    CHECK(p.missed == 0);
    CHECK(p.period_us == 16666);
    CHECK(p.gap_us == 16666);
  }
  { // One stall of three extra refreshes.
    const auto p = Rec::read_panel(grid(600, 300, 3));
    CHECK(p.missed == 3);
    CHECK(p.gap_us == 66666);
  }
  { // A steady 30 fps must not read as a perfect run at a 30 Hz refresh.
    std::vector<u64> at;
    for (int i = 0; i < 300; ++i) at.push_back(u64(i) * REFRESH * 2);
    CHECK(Rec::read_panel(at).missed >= 290);
  }
  { // Nothing measured.
    CHECK(Rec::read_panel({}).missed == 0);
    CHECK(Rec::percentile({}, 990) == 0);
  }
  { // Percentiles take the sample at or above the rank.
    std::vector<u32> v;
    for (u32 i = 1; i <= 1000; ++i) v.push_back(i);
    CHECK(Rec::percentile(v, 500) == 500);
    CHECK(Rec::percentile(v, 990) == 990);
    CHECK(Rec::percentile(v, 999) == 999);
    CHECK(Rec::percentile({7}, 999) == 7);
  }
  { // The whole window: frames count, the work is timed, flips before begin() and after end() do not.
    Rec rec;
    rec.flip(0, 5, 1, 0);
    CHECK(!rec.active());
    rec.begin(41000, 7);
    CHECK(rec.active());
    u64 t = 2'000'000'000;
    for (int i = 0; i < 100; ++i) {
      rec.frame(t, t + 8'000'000, i % 2 == 0, false, true, true);
      rec.flip(0, t + 12'000'000, t + 8'000'000, i == 50 ? 4 : 0);
      t += REFRESH;
    }
    const auto& s = rec.end(43000, 9);
    CHECK(s[Rec::FRAMES] == 100);
    CHECK(s[Rec::PRESENTS] == 50);
    CHECK(s[Rec::HIRES] == 100);
    CHECK(s[Rec::LOCKED] == 100);
    CHECK(s[Rec::FLIPS_0] == 100);
    CHECK(s[Rec::FLIPS_1] == 0);
    CHECK(s[Rec::MISSED_0] == 0);
    CHECK(s[Rec::DROPPED] == 1);
    CHECK(s[Rec::WORK_P99_US] == 8000);
    CHECK(s[Rec::LAT_P50_US] == 4000);
    CHECK(s[Rec::AUDIO_DRY] == 2);
    CHECK(s[Rec::TEMP_START] == 41000);
    CHECK(s[Rec::TEMP_END] == 43000);
    CHECK(s[Rec::WALL_US] == u32(99 * REFRESH / 1000));
    rec.frame(t, t + 1, true, false, true, true);
    CHECK(rec.end(0, 9)[Rec::FRAMES] == 100);
  }
  return 0;
}
