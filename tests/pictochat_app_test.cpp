// Built with -fsanitize=address,undefined by tests/test_pictochat_app.py. argv[1]: runtime/pictochat_net.py
#include "launcher/pictochat_app.hpp"
#include "launcher/keyboard.hpp"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace rgds;
using namespace rgds::pictochat;

struct BoxGlyphs : GlyphSource {
    void draw(const std::string& t, int x, int y, const std::function<void(int, int, uint8_t)>& put) override {
        int pen = x;
        for (size_t i = 0; i < t.size(); i++) {
            if ((t[i] & 0xc0) == 0x80) continue;
            for (int dy = 0; dy < 20; dy++)
                for (int dx = 0; dx < 12; dx++) put(pen + dx, y + dy, 220);
            pen += 14;
        }
    }
    int width(const std::string& t) override {
        int n = 0;
        for (char c : t)
            if ((c & 0xc0) != 0x80) n++;
        return n * 14;
    }
};

static std::string hex_of(const std::vector<uint8_t>& b) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (auto v : b) {
        s += d[v >> 4];
        s += d[v & 15];
    }
    return s;
}
static bool overlap(const Box& a, const Box& b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}
static bool inside(const Box& b) { return b.x >= 0 && b.y >= 0 && b.x + b.w <= 640 && b.y + b.h <= 480; }

static void layout_checks() {
    // The room: tools, pad, send and scroll arrows, inks, undo and clear never overlap and stay on the screen.
    std::vector<Box> room = {layout::pad,         layout::send, layout::scroll_up,
                             layout::scroll_down, layout::undo, layout::clear};
    for (int i = 0; i < 4; i++) room.push_back(layout::tool(i));
    for (int i = 0; i < 9; i++) room.push_back(layout::swatch(i));
    for (size_t i = 0; i < room.size(); i++) {
        assert(inside(room[i]));
        for (size_t j = i + 1; j < room.size(); j++) assert(!overlap(room[i], room[j]));
    }
    // The shared keyboard sits below them in every mode and layer, with no overlapping keys.
    for (auto mode : {KeyboardMode::Edit, KeyboardMode::Search, KeyboardMode::Chat})
        for (bool accents : {false, true})
            for (bool stamps : {false, true})
                for (int layer : {0, 1})
                    for (bool caps : {false, true}) {
                        auto rows = keyboard_layout(layer, mode, caps, accents, stamps,
                                                    mode == KeyboardMode::Chat ? layout::keyboard_top : 186,
                                                    mode == KeyboardMode::Chat ? layout::keyboard_scale : 1.f);
                        assert(!rows.empty());
                        std::vector<SDL_Rect> keys;
                        for (const auto& row : rows)
                            for (const auto& key : row) {
                                const Box b{key.bounds.x, key.bounds.y, key.bounds.w, key.bounds.h};
                                assert(inside(b));
                                if (mode == KeyboardMode::Chat)
                                    for (const auto& r : room)
                                        assert(!overlap(b, r)); // only the chat keyboard shares a screen with the room
                                for (const auto& other : keys) {
                                    const Box o{other.x, other.y, other.w, other.h};
                                    assert(!overlap(b, o));
                                }
                                keys.push_back(key.bounds);
                            }
                        assert(keyboard_submit(rows).first < int(rows.size()));
                    }
    assert(keyboard_layout(0, KeyboardMode::Chat).size() == 5 &&
           keyboard_layout(0, KeyboardMode::Chat, false, false, true).size() == 3);
    size_t stamps = 0;
    for (const auto& row : keyboard_layout(0, KeyboardMode::Chat, false, false, true))
        for (const auto& key : row)
            if (key.action == KeyAction::Stamp) stamps++;
    assert(stamps == size_t(stamp_count));
    // Chat's Enter makes a second row of text; everywhere else it submits.
    auto find = [&](KeyboardMode mode, const char* label) {
        for (const auto& row : keyboard_layout(0, mode))
            for (const auto& key : row)
                if (key.label == label) return key.action;
        return KeyAction::Cancel;
    };
    assert(find(KeyboardMode::Chat, "Enter") == KeyAction::Newline &&
           find(KeyboardMode::Search, "Enter") == KeyAction::Submit &&
           find(KeyboardMode::Search, "Search") == KeyAction::Submit &&
           find(KeyboardMode::Edit, "Done") == KeyAction::Submit &&
           find(KeyboardMode::Chat, "Send") == KeyAction::Submit);
    // The lobby: four rows, then the profile; naming: a field and seventeen colour chips.
    for (int i = 0; i < 4; i++) {
        assert(inside(layout::room(i)));
        for (int j = i + 1; j < 4; j++) assert(!overlap(layout::room(i), layout::room(j)));
        assert(!overlap(layout::room(i), layout::profile));
    }
    assert(inside(layout::profile));
    for (int i = 0; i < 17; i++) {
        assert(inside(layout::chip(i)));
        assert(!overlap(layout::chip(i), layout::field));
        if (i) assert(!overlap(layout::chip(i), layout::chip(i - 1)));
    }
    assert(inside(layout::field) && layout::chip(0).y + layout::chip(0).h <= 180);
    assert(keyboard_upper("a") == "A" && keyboard_upper("é") == "É" && keyboard_upper("ß") == "ß" &&
           keyboard_upper("5") == "5");
}

static void pad_checks() {
    BoxGlyphs glyphs;
    Chat chat;
    chat.glyphs = &glyphs;
    chat.join(1);
    assert(chat.view == View::Room && chat.room == 1 && chat.log.size() == 1);
    // Drawing live equals replaying; undo and clear.
    chat.begin_stroke(100, 60);
    for (int i = 0; i < 40; i++) chat.extend_stroke(100 + i * 5, 60 + (i % 7) * 3);
    chat.end_stroke();
    assert(chat.ops.size() == 1 && chat.ops[0].points.size() > 5);
    Canvas live = chat.pad;
    chat.replay();
    assert(chat.pad.px == live.px);
    chat.undo();
    assert(chat.ops.empty());
    // Typing: contiguous text merges into one op; the caret follows; lines wrap; the third line is refused.
    for (char c : std::string("hello")) chat.type(std::string(1, c));
    assert(chat.ops.size() == 1 && chat.ops[0].text == "hello" && chat.caret_x == 6 + 5 * 14);
    for (int i = 0; i < 60; i++) chat.type("m");
    assert(chat.caret_line == 1);
    for (int i = 0; i < 80; i++) chat.type("w");
    size_t before = chat.points_used();
    int line = chat.caret_line;
    chat.type("x");
    assert(chat.points_used() == before && chat.caret_line == line);
    for (const auto& op : chat.ops) assert(op.kind == Kind::Text && op.points[0].x + glyphs.width(op.text) <= canvas_w);
    assert(chat.ops.back().text.back() == 'w');
    chat.backspace();
    assert(chat.ops.back().text.back() == 'm');
    chat.clear_pad();
    assert(chat.ops.empty() && chat.caret_x == 6 && chat.caret_line == 0);
    // Backspace takes back a stroke when no text is last; stamps place where tapped.
    chat.type("a");
    chat.begin_stroke(150, 50);
    chat.extend_stroke(200, 60);
    chat.end_stroke();
    chat.backspace();
    assert(chat.ops.size() == 1 && chat.ops[0].kind == Kind::Text);
    chat.stamp_armed = true;
    chat.stamp = 3;
    chat.begin_stroke(200, 60);
    assert(chat.ops.back().kind == Kind::Stamp && chat.ops.back().points[0].x == 200 - layout::pad.x);
    chat.stamp_armed = false;
    chat.type("\n");
    chat.type("b");
    assert(chat.ops.back().points[0].y == Chat::text_y(1));
    // Strokes stay in bounds even when the finger leaves the pad.
    chat.clear_pad();
    chat.begin_stroke(-50, 900);
    chat.extend_stroke(9999, -9999);
    chat.end_stroke();
    for (auto p : chat.ops[0].points) assert(p.x >= 0 && p.x < canvas_w && p.y >= 0 && p.y < canvas_h);
}

static void messages_checks() {
    BoxGlyphs glyphs;
    Chat chat;
    chat.glyphs = &glyphs;
    chat.name = "Me";
    chat.colour = 4;
    chat.join(0);
    // Sending: canonical, logged as ours, pad emptied, rate limited.
    assert(!chat.send(100)); // nothing drawn
    chat.begin_stroke(100, 50);
    for (int i = 0; i < 200; i++) chat.extend_stroke(100 + i, 50 + int(20 * std::sin(i * .2)));
    chat.end_stroke();
    chat.type("Hi");
    assert(chat.send(100));
    assert(chat.ops.empty());
    assert(chat.log.back().mine && chat.log.back().ops.size() == 2);
    chat.type("again");
    assert(!chat.send(100.1));
    assert(chat.send(101));
    // Receiving: only from known users in this room, only valid payloads.
    chat.handle("USER 0102030405060708 0 9 Anna Lise");
    assert(chat.users.size() == 1 && chat.users["0102030405060708"].name == "Anna Lise" &&
           chat.log.back().notice == "Anna Lise joined");
    Op s;
    s.points = {{5, 5}, {40, 30}};
    s.colour = 2;
    auto bytes = encode({s});
    auto good = "MSG 0102030405060708 " + hex_of(bytes);
    size_t n = chat.log.size();
    chat.handle(good);
    assert(chat.log.size() == n + 1 && chat.log.back().name == "Anna Lise" && chat.log.back().colour == 9 &&
           !chat.log.back().mine);
    for (const std::string& bad :
         {std::string("MSG ffffffffffffffff ") + hex_of(bytes), std::string("MSG 0102030405060708 zz"),
          std::string("MSG 0102030405060708 abc"), std::string("MSG 0102030405060708 0100"),
          std::string("MSG 0102030405060708"), std::string("MSG")}) {
        n = chat.log.size();
        chat.handle(bad);
        assert(chat.log.size() == n);
    }
    chat.handle("USER 1111111111111111 2 3 Elsewhere");
    n = chat.log.size();
    chat.handle("MSG 1111111111111111 " + hex_of(bytes));
    assert(chat.log.size() == n); // another room
    for (const std::string& bad : {std::string("USER short 0 1 x"), std::string("USER 2222222222222222 9 1 x"),
                                   std::string("USER 2222222222222222 0 99 x"),
                                   std::string("USER 2222222222222222 a b c"), std::string("USER")}) {
        size_t users = chat.users.size();
        chat.handle(bad);
        assert(chat.users.size() == users);
    }
    chat.handle("GONE 0102030405060708");
    assert(!chat.users.count("0102030405060708") && chat.log.back().notice == "Anna Lise left");
    // The log is capped and scrolling holds its place while new messages arrive.
    chat.handle("USER 0102030405060708 0 9 Anna");
    for (int i = 0; i < 100; i++) chat.handle(good);
    assert(chat.log.size() == 60);
    chat.scrolled = 5;
    chat.handle(good);
    assert(chat.scrolled == 6 && chat.unread);
    // A full room turns the newcomer away, back to the lobby, and says so.
    {
        Chat full;
        full.glyphs = &glyphs;
        full.join(2);
        assert(full.view == View::Room);
        full.handle("FULL");
        assert(full.view == View::Lobby && full.status == "That room is full");
        full.join(1);
        assert(full.status.empty());
        Chat lobby;
        lobby.handle("FULL");
        assert(lobby.view == View::Lobby && lobby.status.empty());
    }
    // People counts and lobby colours.
    assert(chat.people(0) == 2 && chat.people(1) == 0 && chat.people(2) == 1); // us and Anna in A; Elsewhere in C
    chat.leave();
    assert(chat.view == View::Lobby && chat.people(0) == 1);
    // The profile survives a restart, and hostile files cannot break it.
    auto file = fs::temp_directory_path() / ("pc-profile-" + std::to_string(getpid()) + ".ini");
    chat.name = "Björn";
    chat.colour = 11;
    chat.colour_auto = false;
    chat.save_profile(file);
    Chat other;
    other.load_profile(file, 3);
    assert(other.name == "Björn" && other.colour == 11 && !other.colour_auto);
    // Automatic follows the launcher's accent, and the saved file says so.
    chat.colour_auto = true;
    chat.save_profile(file);
    Chat follows;
    follows.load_profile(file, 5);
    assert(follows.colour_auto && follows.colour == 5);
    follows.follow_accent(9);
    assert(follows.colour == 9);
    follows.follow_accent(-1);
    assert(follows.colour == 11);
    follows.colour_auto = false;
    follows.colour = 2;
    follows.follow_accent(9);
    assert(follows.colour == 2); // a chosen colour stays
    {
        std::ifstream in(file);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        assert(text.find("colour=auto") != std::string::npos);
    }
    {
        std::ofstream out(file);
        out << "name=" << std::string(500, 'x') << "\ncolour=99\n";
    }
    Chat odd;
    odd.load_profile(file, 7);
    assert(utf8_length(odd.name) == 16 && odd.colour == 7 && odd.colour_auto);
    fs::remove(file);
}

// Two real helper processes on loopback and two apps talking through them.
static bool wait_until(const std::function<bool()>& done, int milliseconds = 8000) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < end) {
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return done();
}
static void end_to_end(const char* script) {
    auto port = [](int offset) { return std::to_string(41000 + (getpid() % 2000) + offset); };
    BoxGlyphs glyphs;
    Chat a, b;
    a.glyphs = b.glyphs = &glyphs;
    a.name = "Anna";
    a.colour = 3;
    b.name = "Bjørn";
    b.colour = 9;
    assert(a.helper.start(script, {"--port", port(0), "--bind", "127.0.0.1", "--targets", "127.0.0.1:" + port(1)}));
    assert(b.helper.start(script, {"--port", port(1), "--bind", "127.0.0.1", "--targets", "127.0.0.1:" + port(0)}));
    assert(wait_until([&] {
        a.poll();
        b.poll();
        return a.helper_up && b.helper_up && a.online && b.online;
    }));
    a.join(2);
    b.join(2);
    assert(wait_until([&] {
        a.poll();
        b.poll();
        return a.users.size() == 1 && b.users.size() == 1;
    }));
    assert(a.users.begin()->second.name == "Bjørn" && b.users.begin()->second.colour == 3);
    a.begin_stroke(120, 60);
    for (int i = 0; i < 150; i++) a.extend_stroke(120 + i * 2, 60 + int(25 * std::sin(i * .15)));
    a.end_stroke();
    a.type("Hej!");
    std::vector<Op> sent;
    std::string why;
    assert(a.compose(sent, why));
    assert(a.send(50));
    assert(wait_until([&] {
        b.poll();
        return !b.log.empty() && !b.log.back().mine && !b.log.back().ops.empty();
    }));
    const auto& got = b.log.back();
    assert(got.name == "Anna" && got.colour == 3 && got.ops.size() == sent.size());
    Canvas x, y;
    render(x, sent, &glyphs);
    render(y, got.ops, &glyphs);
    assert(x.px == y.px); // the same pixels on both handhelds
    // Rooms are separate: C hears nothing from B in room 0.
    b.leave();
    b.join(0);
    size_t count = a.log.size();
    b.begin_stroke(100, 50);
    b.extend_stroke(200, 60);
    b.end_stroke();
    assert(b.send(60));
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    a.poll();
    for (size_t i = count; i < a.log.size(); i++)
        assert(!a.log[i].notice.empty() && a.log[i].ops.empty()); // only "left" notices: nothing drawn in another room
    // Closing stops the process and leaves no zombie.
    a.close();
    b.close();
    assert(!a.helper.running() && !b.helper.running());
    assert(waitpid(-1, nullptr, WNOHANG) == -1 && errno == ECHILD);
    // A helper that cannot start is reported, not fatal.
    Chat c;
    assert(!c.helper.start("/nonexistent/helper.py", {}));
    Chat d;
    assert(d.helper.start(script, {"--port", "not-a-number"}));
    assert(wait_until(
        [&] {
            d.poll();
            return !d.helper.running();
        },
        5000));
    d.close();
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--keys") { // every character a key can type, for the font check
        for (auto mode : {KeyboardMode::Edit, KeyboardMode::Chat})
            for (bool accents : {false, true})
                for (int layer : {0, 1})
                    for (bool caps : {false, true})
                        for (const auto& row : keyboard_layout(layer, mode, caps, accents, false))
                            for (const auto& key : row)
                                if (key.action == KeyAction::Character) {
                                    std::printf("%s\n", key.label.c_str());
                                    if (!key.alternate.empty()) std::printf("%s\n", key.alternate.c_str());
                                }
        return 0;
    }
    layout_checks();
    pad_checks();
    messages_checks();
    if (argc > 1) end_to_end(argv[1]);
    std::puts("pictochat app: ok");
}
