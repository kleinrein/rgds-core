// SPDX-License-Identifier: GPL-3.0-or-later
// The rewind timeline and its screen (docs/TIME_TRAVEL_PLAN.md).
//   play-timeline-tests [ROM DATA_DIR [SHOT.bmp]]
// With a ROM (tests/test_timeline.py builds the test cartridge), a real session is rewound too, and the Rewind
// screen is written to SHOT.bmp for a look.
#include "app/session.hpp"
#include "app/timeline.hpp"
#include "companion/rewind.hpp"
#include "ui/screens.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace play;

namespace {

int failures = 0, checks = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(cond)) {                                                                                                 \
            ++failures;                                                                                                \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                       \
        }                                                                                                              \
    } while (0)

// A machine whose whole state is its frame counter plus `padding` bytes that change with it, and a picture that
// shows the counter.
class Counter final : public Core {
public:
    uint32_t frame = 0;
    size_t padding = 1000;
    bool refuse = false;
    std::vector<uint32_t> px = std::vector<uint32_t>(240 * 160);
    const char* system() const override { return "gba"; }
    std::string game_code() const override { return "TEST"; }
    std::string header_title() const override { return "COUNTER"; }
    double frame_rate() const override { return 60; }
    void run_frame(uint32_t) override {
        ++frame;
        for (auto& p : px) p = 0xFF000000u | frame;
    }
    Picture picture() const override { return {px.data(), 240, 160, 240}; }
    bool save_state(std::vector<uint8_t>& out) override {
        if (refuse) return false;
        out.assign(4 + padding, 0);
        std::memcpy(out.data(), &frame, 4);
        // Not all zeros, so deflate has some work; still mostly the same from moment to moment.
        for (size_t i = 0; i < padding; i += 97) out[4 + i] = uint8_t(frame + i);
        return true;
    }
    bool load_state(const std::vector<uint8_t>& in) override {
        if (in.size() != 4 + padding) return false;
        std::memcpy(&frame, in.data(), 4);
        return true;
    }
    uint32_t peek(uint32_t, int) override { return 0; }
    size_t audio(int16_t*, size_t) override { return 0; }
    unsigned audio_rate() const override { return 32768; }
    std::vector<uint8_t> battery() const override { return {}; }
    uint64_t battery_revision() const override { return 0; }
    void set_layers(unsigned) override {}
    std::unique_ptr<Core> shadow() const override { return nullptr; }
};

void play_frames(Timeline& t, Counter& c, int n) {
    for (int i = 0; i < n; ++i) {
        c.run_frame(0);
        t.after_frame(c, c.picture());
        t.settle(); // the worker finishes each one, so a moment is never skipped for being busy
    }
}

void test_moments_and_branches() {
    Counter c;
    Timeline t;
    t.configure(60, {});
    play_frames(t, c, 600);
    // One a second: ten moments in a line, each the child of the one before.
    CHECK(t.size() == 10);
    CHECK(t.step_frames() == 60);
    const auto line = t.line(t.head());
    CHECK(line.size() == 10 && line.front() == 1 && line.back() == 10);
    CHECK(t.find(1)->position == 60 && t.find(10)->position == 600);
    CHECK(t.find(4)->thumb.w == 208 && t.find(4)->thumb.h == 138 && (t.find(4)->thumb.px[0] & 0xFFFFFF) == 240);

    // Back to moment 4 (frame 240), then on: the next moment branches from 4, and 5..10 stay as the old future.
    std::string error;
    CHECK(t.jump(c, 4, error) && c.frame == 240 && t.head() == 4 && t.position() == 240);
    play_frames(t, c, 120);
    CHECK(t.size() == 12);
    CHECK(t.find(11)->parent == 4 && t.find(11)->position == 300 && t.find(12)->parent == 11);
    CHECK((t.children(4) == std::vector<uint32_t>{5, 11}));
    CHECK(t.newest_leaf(4) == 12 && t.newest_leaf(5) == 10 && t.newest_leaf(12) == 12);
    CHECK((t.line(10) == std::vector<uint32_t>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10}));
    CHECK((t.line(12) == std::vector<uint32_t>{1, 2, 3, 4, 11, 12}));
    // The old future still loads.
    CHECK(t.jump(c, 9, error) && c.frame == 540);
    CHECK(!t.jump(c, 99, error) && !error.empty());

    // mark: a moment now, deflated before it returns; right after a jump (or a moment taken) that is the head.
    CHECK(t.mark(c, c.picture()) == 9);
    c.run_frame(0);
    t.after_frame(c, c.picture());
    const uint32_t now = t.mark(c, c.picture());
    CHECK(now == 13 && t.find(now)->parent == 9 && t.find(now)->position == 541);
}

void test_budget() {
    Counter c;
    c.padding = 200000;
    Timeline t;
    Timeline::Options o;
    o.budget = 64 << 10; // a few moments' worth
    o.thumb_w = o.thumb_h = 16;
    t.configure(60, o);
    play_frames(t, c, 60 * 40);
    const size_t kept = t.size();
    CHECK(kept >= 3 && kept < 40);
    CHECK(t.bytes() <= o.budget + (16 << 10)); // the newest may be one moment over until the next frame
    // The oldest went: the line starts later, and its first moment hangs from nothing.
    const auto line = t.line(t.head());
    CHECK(line.size() == kept && line.front() > 1 && t.find(line.front())->parent == 0);

    // A protected moment stays however old it gets.
    const uint32_t keep = line.front();
    t.protect(keep);
    play_frames(t, c, 60 * 20);
    CHECK(t.find(keep) != nullptr);
    CHECK(t.find(keep + 1) == nullptr);
}

void test_big_and_refused_states() {
    Counter big;
    big.padding = 2 << 20;
    Timeline t;
    Timeline::Options o;
    o.max_state = 1 << 20;
    t.configure(60, o);
    play_frames(t, big, 120);
    CHECK(!t.available() && t.size() == 0 && !t.unavailable_reason().empty());

    // Between step_bytes and max_state, the step widens with the size.
    Counter mid;
    mid.padding = (1 << 20) + 10; // three 512 KiB steps
    Timeline w;
    w.configure(60, {});
    play_frames(w, mid, 60);
    CHECK(w.available() && w.size() == 1 && w.step_frames() == 180);
    play_frames(w, mid, 170);
    CHECK(w.size() == 1);
    play_frames(w, mid, 10);
    CHECK(w.size() == 2);

    Counter none;
    none.refuse = true;
    Timeline r;
    r.configure(60, {});
    play_frames(r, none, 61);
    CHECK(!r.available());
}

void test_screen() {
    Counter c;
    Timeline t;
    t.configure(60, {});
    play_frames(t, c, 600); // moments 1..10
    std::string error;
    t.jump(c, 4, error);
    play_frames(t, c, 120); // 11, 12 from 4
    const uint32_t now = t.mark(c, c.picture());
    Words words;
    RewindScreen s;
    s.open(t, now);
    CHECK(now == 12); // just taken at frame 360
    CHECK(s.is_open() && s.chosen() == now && s.line().size() == 6); // 1 2 3 4 11 12
    CHECK(s.label(now, words) == "Now");
    CHECK(s.label(11, words) == "-0:01" && s.label(1, words) == "-0:05"); // 1 at frame 60, now at 360
    // The button that opened the screen does nothing until let go; then Left steps back.
    CHECK(!s.pad(Left));
    s.pad(0);
    s.set_time(1000, false);
    s.pad(Left);
    CHECK(s.chosen() == 11);
    s.pad(0);
    s.pad(L); // ten back, stopped at the oldest
    CHECK(s.chosen() == 1);
    s.pad(0);
    // On moment 4 there are two futures: the strip follows 11 (the newer line, the one it opened on).
    for (int i = 0; i < 3; ++i) {
        s.pad(Right);
        s.pad(0);
    }
    CHECK(s.chosen() == 4 && s.branch_count() == 2 && s.branch_index() == 1);
    s.pad(Down); // the other future: the strip now ends at 10
    s.pad(0);
    CHECK(s.chosen() == 4 && s.branch_index() == 0 && s.line().back() == 10 && s.line().size() == 10);
    CHECK(s.label(10, words) == "+0:04"); // 10 at frame 600
    // A drag to the right goes back in time; letting go plays from there.
    s.set_time(2000, false);
    const int x = 320;
    s.touch({Touch::Down, x, RewindScreen::flow_y});
    s.touch({Touch::Move, x + int(RewindScreen::drag_per_moment) * 2, RewindScreen::flow_y});
    CHECK(s.chosen() == 2);
    s.touch({Touch::Up, x + int(RewindScreen::drag_per_moment) * 2, RewindScreen::flow_y});
    uint32_t target = 0;
    CHECK(s.take(target) == RewindScreen::Result::Jump && target == 2);
    CHECK(s.take(target) == RewindScreen::Result::None);
    // A tap on a picture only looks at it; Back to now is a tap on its button.
    s.set_time(3000, false);
    const int right_one = int(s.centre_x(2));
    s.touch({Touch::Down, right_one, RewindScreen::flow_y});
    s.touch({Touch::Up, right_one, RewindScreen::flow_y});
    CHECK(s.chosen() == 3 && s.take(target) == RewindScreen::Result::None);
    s.pad(Right);
    s.pad(0);
    CHECK(s.chosen() == 4);
    const Box back = RewindScreen::box(RewindScreen::Part::Back);
    s.touch({Touch::Down, back.x + 10, back.y + 10});
    s.touch({Touch::Up, back.x + 10, back.y + 10});
    CHECK(s.take(target) == RewindScreen::Result::Back);
    // The branch chip, tapped, follows the other future again.
    const Box chip = RewindScreen::box(RewindScreen::Part::Branch);
    s.touch({Touch::Down, chip.x + 10, chip.y + 10});
    s.touch({Touch::Up, chip.x + 10, chip.y + 10});
    CHECK(s.branch_index() == 1 && s.line().back() == now);
    // A tap on the scrubber's track looks (the strip goes there); only a drag along it, let go, plays.
    uint32_t ignored = 0;
    s.take(ignored);
    const Box track = RewindScreen::box(RewindScreen::Part::Scrubber);
    s.touch({Touch::Down, track.x + 30, track.y + 20});
    s.touch({Touch::Up, track.x + 30, track.y + 20});
    CHECK(s.chosen() == s.line().front() && s.take(ignored) == RewindScreen::Result::None && s.settled());
    s.touch({Touch::Down, track.x + 30, track.y + 20});
    s.touch({Touch::Move, track.x + track.w - 30, track.y + 20});
    CHECK(!s.settled()); // a finger on it: nothing to load yet
    s.touch({Touch::Up, track.x + track.w - 30, track.y + 20});
    CHECK(s.take(target) == RewindScreen::Result::Jump && target == s.line().back());
    // With the springs on, A during a glide plays where the strip is heading, not what passes the middle.
    s.set_time(5000, true);
    s.pad(0);
    s.pad(L);
    s.pad(0);
    CHECK(s.moving() && !s.settled());
    s.pad(A);
    s.pad(0);
    CHECK(s.take(target) == RewindScreen::Result::Jump && target == s.line()[s.line().size() - 1 - std::min<size_t>(10, s.line().size() - 1)]);
    for (uint64_t ms = 5016; ms < 6500; ms += 16) s.set_time(ms, true);
    CHECK(s.settled());
    s.set_time(7000, false);
    // It draws (a font is not needed for the layout to hold).
    Canvas out(640, 480);
    Look look;
    Font font;
    const uint64_t k = s.key();
    s.draw(out, look, font, words);
    CHECK(s.key() == k);
}

// The bridge's files (app/moments.hpp), written as src/platform/emulator_bridge.cpp writes them, read back as the tree
// the game menu's Rewind screen shows: the same lines, branches and pictures as the engine's own timeline.
void put32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(uint8_t(v >> (8 * i)));
}
void test_moment_list() {
    Counter c;
    Timeline t;
    t.configure(60, {});
    play_frames(t, c, 300);
    std::string error;
    t.jump(c, 2, error);
    play_frames(t, c, 120);
    const uint32_t now = t.mark(c, c.picture());
    std::string text = "now=" + std::to_string(now) + "\nrate=60\ncount=" + std::to_string(t.size()) + "\n";
    std::vector<uint8_t> thumbs{'R', 'G', 'R', 'W'};
    put32(thumbs, uint32_t(t.size()));
    int i = 0;
    for (uint32_t id : t.ids()) {
        const Moment* m = t.find(id);
        text += "moment_" + std::to_string(i++) + "=" + std::to_string(m->id) + " " + std::to_string(m->parent) + " " +
                std::to_string(m->position) + "\n";
        put32(thumbs, id);
        put32(thumbs, uint32_t(m->thumb.w));
        put32(thumbs, uint32_t(m->thumb.h));
        for (uint32_t px : m->thumb.px) put32(thumbs, px);
    }
    MomentList list;
    CHECK(list.parse(text, thumbs));
    CHECK(list.now == now && list.ids() == t.ids() && list.frame_rate() == 60);
    CHECK(list.line(now) == t.line(now) && list.children(2) == t.children(2) && list.newest_leaf(3) == t.newest_leaf(3));
    CHECK(list.find(3)->thumb.px == t.find(3)->thumb.px && list.find(3)->thumb.w == 208);
    // The same screen over either.
    RewindScreen a, b;
    a.open(t, now);
    b.open(list, now);
    a.set_time(1, false);
    b.set_time(1, false);
    CHECK(a.line() == b.line() && a.branch_count() == b.branch_count());
    // A list cut short keeps what it has; one that names no "now" is refused.
    thumbs.resize(thumbs.size() / 2);
    CHECK(list.parse(text, thumbs) && list.find(1)->thumb.w == 208);
    CHECK(!list.parse("rate=60\n", {}));
    // What the engine shows: two DS screens.
    std::vector<uint8_t> shown{'R', 'G', 'R', 'S'};
    for (uint32_t v : {7u, 2u, 256u, 192u}) put32(shown, v);
    shown.resize(shown.size() + size_t(2) * 256 * 192 * 4, 0x40);
    ShownMoment sm;
    CHECK(sm.parse(shown) && sm.id == 7 && sm.screens == 2 && sm.px.size() == size_t(2) * 256 * 192);
    shown.pop_back();
    CHECK(!sm.parse(shown));
}

// A real game: rewind the test cartridge in a session, through the same calls the frontend makes.
void test_session(const std::string& rom, const std::string& data, const std::string& shot) {
    Session session;
    SessionOptions o;
    o.rom = rom;
    o.data_root = data;
    o.font_dir = PLAY_FONTS;
    std::string error;
    if (!session.open(o, error)) {
        std::fprintf(stderr, "FAIL session: %s\n", error.c_str());
        ++failures;
        return;
    }
    for (int i = 0; i < 60 * 8; ++i) session.frame(i % 90 < 30 ? Right : 0);
    session.timeline().settle();
    CHECK(session.timeline().available() && session.timeline().size() >= 6);
    std::vector<uint8_t> before;
    CHECK(session.core().save_state(before));
    std::string why;
    CHECK(session.open_rewind(why) && session.rewinding());
    // Two back on the pad: the top screen shows that moment (loaded, one frame run), the game held.
    session.rewind_input(0, 10, false);
    for (int i = 0; i < 2; ++i) {
        session.rewind_input(Left, 20, false);
        session.rewind_input(0, 30, false);
    }
    const uint32_t looked = session.rewind_screen().chosen();
    CHECK(looked != session.rewind_screen().now());
    if (!shot.empty()) {
        Canvas lower(640, 480), top(640, 480);
        session.render_companion(lower, session.look());
        const Picture g = session.game(true);
        top.blit(g.px, g.w, g.h, (640 - g.w * 2) / 2, (480 - g.h * 2) / 2, 2);
        CHECK(write_bmp(shot, top, &lower));
    }
    // B: back to now, exactly as the game was.
    session.rewind_input(B, 40, false);
    CHECK(!session.rewinding());
    std::vector<uint8_t> after;
    CHECK(session.core().save_state(after) && after == before);
    // B is still held: the game doesn't get it until it is let go.
    // Then play from an earlier moment: the game is that moment, and the next one branches from it.
    CHECK(session.open_rewind(why));
    session.rewind_input(0, 50, false);
    session.rewind_input(L, 60, false);
    session.rewind_input(0, 70, false);
    const uint32_t from = session.rewind_screen().chosen();
    session.rewind_input(A, 80, false);
    CHECK(!session.rewinding() && session.timeline().head() == from);
    for (int i = 0; i < 60 * 2; ++i) session.frame(0);
    session.timeline().settle();
    CHECK(session.timeline().children(from).size() >= 2);

    // The game menu's Rewind (the bridge's hooks): a look loads a moment; if the menu then lets the game go without
    // Back or Play from here, the next frame starts from now, exactly as it was.
    for (int i = 0; i < 60 * 3; ++i) session.frame(i % 50 < 20 ? Left : 0);
    std::vector<uint8_t> at_open;
    const uint32_t menu_now = session.menu_rewind_open();
    CHECK(menu_now && session.core().save_state(at_open));
    const uint32_t older = session.timeline().line(menu_now).front();
    Picture shown;
    CHECK(session.menu_rewind_show(older, shown) && shown.px && shown.w == 240 && shown.h == 160);
    std::vector<uint8_t> looking;
    CHECK(session.core().save_state(looking) && looking != at_open);
    // Saving now saves now.
    const std::string slot = data + "/rewind-check.state";
    CHECK(session.save_state_file(slot, why));
    std::vector<uint8_t> saved;
    CHECK(session.core().save_state(saved) && saved == at_open);
    // A look, then the game goes on: from now.
    CHECK(session.menu_rewind_show(older, shown));
    session.frame(0);
    CHECK(session.timeline().position() == session.timeline().find(menu_now)->position + 1);
    // Play from here leaves the game at that moment.
    const uint32_t again = session.menu_rewind_open();
    CHECK(again && session.menu_rewind_show(older, shown) && session.menu_rewind_jump(older, true));
    CHECK(session.timeline().head() == older);
}

} // namespace

int main(int argc, char** argv) {
    test_moments_and_branches();
    test_budget();
    test_big_and_refused_states();
    test_screen();
    test_moment_list();
    if (argc >= 3) test_session(argv[1], argv[2], argc >= 4 ? argv[3] : "");
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
