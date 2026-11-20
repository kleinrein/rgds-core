// SPDX-License-Identifier: GPL-3.0-or-later
// Scanned manuals on the second screen (docs/MANUALS_PLAN.md): the page
// sources (folder, CBZ, a PDF built here), the reader's page turns and
// spreads, the page kept per manual, and the companion's switch with a
// manual behind it.
#include "companion/companion.hpp"
#include "reader/manual.hpp"
#include "reader/pages.hpp"
#include "ui/kit.hpp"
#include "platform/touch.hpp"

#include <zlib.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace play;
namespace fs = std::filesystem;

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

std::string bmp(int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    const int row = (w * 3 + 3) & ~3;
    std::string out(54 + size_t(row) * h, '\0');
    auto put32 = [&](size_t at, uint32_t v) {
        for (int i = 0; i < 4; ++i) out[at + i] = char(v >> (8 * i));
    };
    out[0] = 'B';
    out[1] = 'M';
    put32(2, uint32_t(out.size()));
    put32(10, 54);
    put32(14, 40);
    put32(18, uint32_t(w));
    put32(22, uint32_t(h));
    out[26] = 1;
    out[28] = 24;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t at = 54 + size_t(y) * row + size_t(x) * 3;
            out[at] = char(b), out[at + 1] = char(g), out[at + 2] = char(r);
        }
    return out;
}

void write(const fs::path& p, const std::string& bytes) {
    std::ofstream(p, std::ios::binary).write(bytes.data(), std::streamsize(bytes.size()));
}

std::string deflate_raw(const std::string& in) {
    z_stream z{};
    deflateInit2(&z, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
    std::string out(compressBound(uLong(in.size())) + 64, '\0');
    z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
    z.avail_in = uInt(in.size());
    z.next_out = reinterpret_cast<Bytef*>(out.data());
    z.avail_out = uInt(out.size());
    deflate(&z, Z_FINISH);
    out.resize(z.total_out);
    deflateEnd(&z);
    return out;
}

std::string zlib(const std::string& in) {
    uLongf n = compressBound(uLong(in.size()));
    std::string out(n, '\0');
    compress(reinterpret_cast<Bytef*>(out.data()), &n, reinterpret_cast<const Bytef*>(in.data()), uLong(in.size()));
    out.resize(n);
    return out;
}

// A zip with the entries as given: stored or deflated.
std::string zip(const std::vector<std::tuple<std::string, std::string, bool>>& entries) {
    std::string out, dir;
    auto u16 = [](std::string& s, uint32_t v) { s += char(v), s += char(v >> 8); };
    auto u32 = [](std::string& s, uint32_t v) {
        for (int i = 0; i < 4; ++i) s += char(v >> (8 * i));
    };
    for (const auto& [name, data, packed] : entries) {
        const std::string body = packed ? deflate_raw(data) : data;
        const uint32_t crc = uint32_t(crc32(0, reinterpret_cast<const Bytef*>(data.data()), uInt(data.size())));
        const uint32_t at = uint32_t(out.size());
        u32(out, 0x04034b50), u16(out, 20), u16(out, 0), u16(out, packed ? 8 : 0), u16(out, 0), u16(out, 0);
        u32(out, crc), u32(out, uint32_t(body.size())), u32(out, uint32_t(data.size()));
        u16(out, uint32_t(name.size())), u16(out, 3);
        out += name + std::string("\0\0\0", 3) + body;
        u32(dir, 0x02014b50), u16(dir, 20), u16(dir, 20), u16(dir, 0), u16(dir, packed ? 8 : 0), u16(dir, 0);
        u16(dir, 0), u32(dir, crc), u32(dir, uint32_t(body.size())), u32(dir, uint32_t(data.size()));
        u16(dir, uint32_t(name.size())), u16(dir, 0), u16(dir, 0), u16(dir, 0), u16(dir, 0), u32(dir, 0), u32(dir, at);
        dir += name;
    }
    const uint32_t dir_at = uint32_t(out.size());
    out += dir;
    u32(out, 0x06054b50), u16(out, 0), u16(out, 0), u16(out, uint32_t(entries.size())),
        u16(out, uint32_t(entries.size()));
    u32(out, uint32_t(dir.size())), u32(out, dir_at), u16(out, 0);
    return out;
}

// A PDF the way scanners write them, small: three pages, the second turned a quarter and its dictionary in an
// object stream, the third with no picture at all.
std::string pdf() {
    std::string pixels_a, pixels_b;
    for (int i = 0; i < 4 * 2; ++i) pixels_a += std::string("\xff\x00\x00", 3); // 4x2 red
    for (int i = 0; i < 2 * 4; ++i) pixels_b += std::string("\x00\x00\xff", 3); // 2x4 blue
    const std::string a = zlib(pixels_a), b = zlib(pixels_b);
    std::string out = "%PDF-1.7\n%\xe2\xe3\xcf\xd3\n";
    out += "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";
    out += "2 0 obj\n<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R] /Count 3 >>\nendobj\n";
    out += "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 4 2] /Resources << /XObject << /Im0 6 0 R >> >> "
           "/Contents 8 0 R >>\nendobj\n";
    // Page 4 lives in object stream 9.
    out += "5 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 4 4] /Contents 8 0 R >>\nendobj\n";
    out += "6 0 obj\n<< /Type /XObject /Subtype /Image /Width 4 /Height 2 /ColorSpace /DeviceRGB "
           "/BitsPerComponent 8 /Filter /FlateDecode /Length " +
           std::to_string(a.size()) + " >>\nstream\n" + a + "\nendstream\nendobj\n";
    // An indirect length, as Ghostscript writes them.
    out += "7 0 obj\n<< /Type /XObject /Subtype /Image /Width 2 /Height 4 /ColorSpace [/ICCBased 11 0 R] "
           "/BitsPerComponent 8 /Filter [/FlateDecode] /Length 10 0 R >>\nstream\n" +
           b + "\nendstream\nendobj\n";
    out += "8 0 obj\n<< /Length 4 >>\nstream\nq Q\nendstream\nendobj\n";
    const std::string page4 = "<< /Type /Page /Parent 2 0 R /Rotate 90 /Resources << /XObject << /Im1 7 0 R >> >> >>";
    const std::string packed = zlib("4 0 " + page4);
    out += "9 0 obj\n<< /Type /ObjStm /N 1 /First 4 /Filter /FlateDecode /Length " + std::to_string(packed.size()) +
           " >>\nstream\n" + packed + "\nendstream\nendobj\n";
    out += "10 0 obj\n" + std::to_string(b.size()) + "\nendobj\n";
    out += "11 0 obj\n<< /N 3 /Length 0 >>\nstream\n\nendstream\nendobj\n";
    out += "trailer\n<< /Root 1 0 R /Size 12 >>\n%%EOF\n";
    return out;
}

template <class F> bool eventually(F done) {
    for (int i = 0; i < 400; ++i) {
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return done();
}

class Quiet final : public Core {
public:
    explicit Quiet(const char* system) : system_(system) {}
    const char* system() const override { return system_; }
    std::string game_code() const override { return "TEST"; }
    std::string header_title() const override { return "TEST"; }
    double frame_rate() const override { return 60; }
    void run_frame(uint32_t) override {}
    Picture picture() const override { return {}; }
    bool save_state(std::vector<uint8_t>&) override { return false; }
    bool load_state(const std::vector<uint8_t>&) override { return false; }
    uint32_t peek(uint32_t, int) override { return 0; }
    size_t audio(int16_t*, size_t) override { return 0; }
    unsigned audio_rate() const override { return 32768; }
    std::vector<uint8_t> battery() const override { return {}; }
    uint64_t battery_revision() const override { return 0; }
    void set_layers(unsigned) override {}
    std::unique_ptr<Core> shadow() const override { return nullptr; }

private:
    const char* system_;
};

uint32_t colour_at(const Canvas& c, int x, int y) { return c.at(x, y) & 0xFFFFFF; }

void test_sources(const fs::path& dir) {
    CHECK(natural_less("page 2.jpg", "page 10.jpg") && !natural_less("page 10.jpg", "page 2.jpg"));
    CHECK(natural_less("A01", "a2") && !natural_less("x", "x"));

    // A folder of pictures, in reading order whatever the names' widths.
    const fs::path folder = dir / "Game.manual";
    fs::create_directories(folder);
    write(folder / "page 10.bmp", bmp(3, 2, 0, 255, 0));
    write(folder / "page 2.bmp", bmp(2, 3, 255, 0, 0));
    write(folder / ".hidden.bmp", bmp(1, 1, 0, 0, 0));
    write(folder / "notes.txt", "not a page");
    Pages f = open_pages(folder.string());
    CHECK(f.ok() && f.pages.size() == 2);
    std::vector<uint32_t> px;
    int w = 0, h = 0;
    std::string error;
    CHECK(read_page(f, 0, px, w, h, error) && w == 2 && h == 3 && (px[0] & 0xFFFFFF) == 0x0000FF);
    CHECK(read_page(f, 1, px, w, h, error) && w == 3 && h == 2 && (px[0] & 0xFFFFFF) == 0x00FF00);

    // A CBZ: stored and deflated entries, macOS litter and other files left out, sorted naturally.
    const fs::path cbz = dir / "Game.cbz";
    write(cbz, zip({{"b/10.bmp", bmp(4, 4, 0, 0, 255), true},
                    {"b/9.bmp", bmp(5, 5, 255, 255, 0), false},
                    {"__MACOSX/b/._9.bmp", "junk", false},
                    {"b/readme.txt", "hello", true}}));
    Pages z = open_pages(cbz.string());
    CHECK(z.ok() && z.pages.size() == 2);
    CHECK(read_page(z, 0, px, w, h, error) && w == 5 && (px[0] & 0xFFFFFF) == 0x00FFFF);
    CHECK(read_page(z, 1, px, w, h, error) && w == 4 && (px[0] & 0xFFFFFF) == 0xFF0000);
    write(dir / "broken.cbz", "PK not really");
    CHECK(!open_pages((dir / "broken.cbz").string()).ok());

    // A PDF: the page tree in order, a page packed in an object stream, an indirect length, a quarter turn.
    const fs::path doc = dir / "Game.pdf";
    write(doc, pdf());
    Pages p = open_pages(doc.string());
    CHECK(p.ok() && p.pages.size() == 3);
    if (p.pages.size() == 3) {
        CHECK(p.pages[0].kind == PageSource::Pixels && p.pages[1].kind == PageSource::Pixels);
        CHECK(p.pages[1].rotate == 90 && p.pages[2].kind == PageSource::Missing);
        CHECK(read_page(p, 0, px, w, h, error) && w == 4 && h == 2 && (px[0] & 0xFFFFFF) == 0x0000FF);
        CHECK(read_page(p, 1, px, w, h, error) && w == 4 && h == 2 && (px[0] & 0xFFFFFF) == 0xFF0000);
        CHECK(!read_page(p, 2, px, w, h, error) && error == "not a scan");
    }
    write(dir / "fake.pdf", "%PDF-1.4\nnothing here\n");
    CHECK(!open_pages((dir / "fake.pdf").string()).ok());

    // Halved until it fits the cap.
    write(folder / "page 30.bmp", bmp(64, 16, 9, 9, 9));
    f = open_pages(folder.string());
    CHECK(read_page(f, 2, px, w, h, error, 20) && w == 16 && h == 4);
    fs::remove(folder / "page 30.bmp");
}

void test_finding(const fs::path& dir) {
    const fs::path roms = dir / "roms", art = dir / "art";
    fs::create_directories(roms);
    fs::create_directories(art);
    write(roms / "Quest (Europe).gba", "rom");
    CHECK(find_manuals((roms / "Quest (Europe).gba").string(), art.string()).empty());
    write(roms / "Quest (Europe).cbz", "x");
    write(art / "manual.pdf", "x");
    const auto found = find_manuals((roms / "Quest (Europe).gba").string(), art.string());
    CHECK(found.size() == 2 && fs::path(found[0]).filename() == "Quest (Europe).cbz" &&
          fs::path(found[1]).filename() == "manual.pdf");
    // A zipped ROM by the zip's own name.
    write(roms / "Racer.zip", "zip");
    write(roms / "Racer.pdf", "x");
    CHECK(find_manuals((roms / "Racer.zip").string(), "").size() == 1);
}

void test_reader(const fs::path& dir) {
    // Page 1 an ordinary page, page 2 a two-page spread (left green, right blue), page 3 another page.
    const fs::path folder = dir / "Spread.manual";
    fs::create_directories(folder);
    write(folder / "1.bmp", bmp(300, 400, 255, 0, 0));
    std::string spread = bmp(800, 400, 0, 255, 0);
    for (int y = 0; y < 400; ++y)
        for (int x = 400; x < 800; ++x) {
            const size_t at = 54 + size_t(y) * 2400 + size_t(x) * 3;
            spread[at] = char(255), spread[at + 1] = 0, spread[at + 2] = 0;
        }
    write(folder / "2.bmp", spread);
    write(folder / "3.bmp", bmp(300, 400, 255, 255, 255));
    const std::string place = (dir / "manual-place.txt").string();
    Quiet core("gba");
    Look look;
    Font font;
    Canvas out(640, 480);
    auto shows = [&](View& v, uint32_t colour) {
        return eventually([&] {
            v.update(core, 1);
            v.draw(out, look, font, 1);
            return colour_at(out, 320, 240) == colour;
        });
    };
    auto tap = [&](View& v, int x) {
        v.touch({Touch::Down, x, 240});
        v.touch({Touch::Up, x, 240});
        std::this_thread::sleep_for(std::chrono::milliseconds(400)); // not a double tap
    };
    {
        auto v = make_manual(folder.string(), place);
        CHECK(v->shown());
        CHECK(shows(*v, 0x0000FF)); // red, fit to the panel
        // A tap on the right edge turns the page; the spread shows its left half first, then its right.
        tap(*v, 620);
        CHECK(shows(*v, 0x00FF00));
        tap(*v, 620);
        CHECK(shows(*v, 0xFF0000));
        // A swipe to the right goes back to the left half.
        v->touch({Touch::Down, 200, 240});
        v->touch({Touch::Move, 260, 240});
        v->touch({Touch::Move, 340, 240});
        v->touch({Touch::Up, 340, 240});
        CHECK(shows(*v, 0x00FF00));
        tap(*v, 20); // back to page 1, arriving on its last part (the whole page)
        CHECK(shows(*v, 0x0000FF));
        tap(*v, 20); // nothing before the first page
        CHECK(shows(*v, 0x0000FF));
        tap(*v, 620);
        tap(*v, 620);
        tap(*v, 620);
        CHECK(shows(*v, 0xFFFFFF)); // page 3
        tap(*v, 620);
        CHECK(shows(*v, 0xFFFFFF)); // nothing after the last
        // Double tap zooms (fit page, then fit width).
        const uint64_t before = v->key(1);
        v->touch({Touch::Down, 320, 240});
        v->touch({Touch::Up, 320, 240});
        v->touch({Touch::Down, 322, 242});
        v->touch({Touch::Up, 322, 242});
        CHECK(v->key(1) != before);
        tap(*v, 620);
        CHECK(shows(*v, 0xFFFFFF));
    }
    {
        // The page is kept: it opens again on page 3.
        auto v = make_manual(folder.string(), place);
        CHECK(shows(*v, 0xFFFFFF));
    }
    {
        auto v = make_manual((dir / "missing.pdf").string(), "");
        CHECK(eventually([&] {
            v->draw(out, look, font, 1);
            return colour_at(out, 320, 240) == (look.background.px() & 0xFFFFFF);
        }));
    }
}

void test_pinch(const fs::path& dir) {
    // The decoder: two slots down give two fingers, while its one-finger stream (what games get) is unchanged.
    rgds::TouchDecoder decoder;
    decoder.uses_slots = true;
    auto frame = [&](std::initializer_list<std::array<int, 3>> events) {
        std::vector<rgds::TouchPoint> out;
        for (const auto& e : events) {
            auto points = decoder.feed(e[0], e[1], e[2]);
            out.insert(out.end(), points.begin(), points.end());
        }
        return out;
    };
    auto one = frame({{3, 47, 0}, {3, 57, 11}, {3, 53, 100}, {3, 54, 200}, {0, 0, 0}});
    CHECK(one.size() == 1 && one[0].kind == 0 && decoder.fingers.size() == 1);
    auto two = frame({{3, 47, 1}, {3, 57, 12}, {3, 53, 300}, {3, 54, 220}, {0, 0, 0}});
    CHECK(two.empty() && decoder.fingers.size() == 2);
    if (decoder.fingers.size() == 2) CHECK(decoder.fingers[1].x == 300 && decoder.fingers[1].y == 220);
    auto lift = frame({{3, 47, 1}, {3, 57, -1}, {3, 47, 0}, {3, 57, -1}, {0, 0, 0}});
    CHECK(lift.size() == 1 && lift[0].kind == 2 && decoder.fingers.empty());

    // The reader: pinch out zooms around the fingers, lifting does not turn the page, pinch in snaps back.
    const fs::path folder = dir / "Pinch.manual";
    fs::create_directories(folder);
    std::string page = bmp(300, 400, 255, 0, 0);
    for (int y = 0; y < 400; ++y) // a blue left half, to see where the zoom went
        for (int x = 0; x < 150; ++x) {
            const size_t at = 54 + size_t(y) * 900 + size_t(x) * 3;
            page[at] = char(255), page[at + 1] = 0, page[at + 2] = 0;
        }
    write(folder / "1.bmp", page);
    write(folder / "2.bmp", bmp(300, 400, 255, 255, 255));
    Quiet core("gba");
    Look look;
    Font font;
    Canvas out(640, 480);
    auto view = make_manual(folder.string(), "");
    auto* pinch = dynamic_cast<Pinch*>(view.get());
    CHECK(pinch != nullptr);
    if (!pinch) return;
    CHECK(eventually([&] {
        view->update(core, 1);
        view->draw(out, look, font, 1);
        return colour_at(out, 300, 240) == 0xFF0000 && colour_at(out, 340, 240) == 0x0000FF;
    }));
    const uint64_t fit = view->key(1);
    // The first finger reaches the reader as a touch too, as on the handheld.
    view->touch({Touch::Down, 330, 240});
    CHECK(pinch->fingers({{330, 240}, {350, 240}}));
    view->touch({Touch::Move, 300, 240});
    CHECK(pinch->fingers({{300, 240}, {380, 240}})); // four times as far apart: as far in as it goes
    CHECK(view->key(1) != fit);
    view->draw(out, look, font, 1);
    // Zoomed around x 340 (the blue/red edge): blue still left of the fingers, red right of them.
    CHECK(colour_at(out, 200, 240) == 0xFF0000 && colour_at(out, 500, 240) == 0x0000FF);
    CHECK(pinch->fingers({}));
    view->touch({Touch::Up, 300, 240}); // not a tap on the left edge, not a swipe
    view->draw(out, look, font, 1);
    CHECK(colour_at(out, 200, 240) == 0xFF0000);
    const uint64_t zoomed = view->key(1);
    // Pinched back in near the whole page: it snaps to it.
    view->touch({Touch::Down, 200, 240});
    CHECK(pinch->fingers({{200, 240}, {440, 240}}));
    CHECK(pinch->fingers({{300, 240}, {340, 240}}));
    CHECK(pinch->fingers({}));
    view->touch({Touch::Up, 300, 240});
    CHECK(view->key(1) != zoomed && view->key(1) == fit);
}

void test_overview(const fs::path& dir) {
    // 23 pages, each its own colour step; page 1 red, the rest grey.
    const fs::path folder = dir / "Overview.manual";
    fs::create_directories(folder);
    write(folder / "1.bmp", bmp(300, 400, 255, 0, 0));
    for (int i = 2; i <= 23; ++i) write(folder / (std::to_string(i) + ".bmp"), bmp(300, 400, 90, 90, uint8_t(i)));
    write(folder / "7.bmp", bmp(300, 400, 0, 0, 255)); // page 7 blue
    Quiet core("gba");
    Look look;
    Font font;
    Canvas out(640, 480);
    auto view = make_manual(folder.string(), "");
    auto* pinch = dynamic_cast<Pinch*>(view.get());
    uint64_t frame = 1;
    auto settle = [&](auto done) {
        return eventually([&] {
            view->update(core, ++frame);
            view->draw(out, look, font, frame);
            return done();
        });
    };
    CHECK(settle([&] { return colour_at(out, 320, 240) == 0x0000FF; }));
    // A tap shows the counter; a tap on it opens the overview, its pictures made in the background.
    view->touch({Touch::Down, 320, 240});
    view->touch({Touch::Up, 320, 240});
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    view->touch({Touch::Down, 60, 440});
    view->touch({Touch::Up, 60, 440});
    // Page 1's picture: the first cell, red; page 7's: second row, second column, blue.
    const int cell = (640 - 48 - 48) / 5;
    auto centre = [&](int index, double scroll) {
        return std::pair<int, int>{24 + (index % 5) * (cell + 12) + cell / 2,
                                   int(92 + (index / 5) * 160 - scroll + 62)};
    };
    const auto first = centre(0, 0), seventh = centre(6, 0);
    CHECK(settle([&] {
        return colour_at(out, first.first, first.second) == 0x0000FF &&
               colour_at(out, seventh.first, seventh.second) == 0xFF0000;
    }));
    // A drag scrolls; lifted still, it stays; it never scrolls past the last row.
    view->touch({Touch::Down, 320, 400});
    view->touch({Touch::Move, 320, 300});
    view->touch({Touch::Move, 320, 100});
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    view->touch({Touch::Up, 320, 100});
    const uint64_t scrolled = view->key(frame);
    for (int i = 0; i < 30; ++i) view->update(core, ++frame);
    CHECK(view->key(frame) == scrolled); // no glide after holding still
    view->touch({Touch::Down, 320, 450});
    for (int y = 440; y > -2000; y -= 100) view->touch({Touch::Move, 320, y});
    view->touch({Touch::Up, 320, -2000});
    for (int i = 0; i < 200; ++i) view->update(core, ++frame);
    view->draw(out, look, font, frame);
    // 5 rows of 160 + top 92 + bottom 72 = 964: at most 484 down. Page 23 (row 5, col 3) is on screen.
    const auto last = centre(22, 484);
    CHECK(colour_at(out, last.first, last.second) == 0x175A5A ||
          colour_at(out, last.first, last.second) == (look.panel.px() & 0xFFFFFF));
    // A tap on page 23 opens it.
    view->touch({Touch::Down, last.first, last.second});
    view->touch({Touch::Up, last.first, last.second});
    CHECK(settle([&] { return colour_at(out, 320, 240) == 0x175A5A; }));
    // Pinched in from the whole page: the overview; fingers spread there: back to the page.
    view->touch({Touch::Down, 200, 240});
    pinch->fingers({{200, 240}, {440, 240}});
    pinch->fingers({{300, 240}, {340, 240}});
    pinch->fingers({});
    view->touch({Touch::Up, 300, 240});
    view->draw(out, look, font, ++frame);
    CHECK(colour_at(out, 320, 240) != 0x175A5A);
    view->touch({Touch::Down, 300, 240});
    pinch->fingers({{300, 240}, {340, 240}});
    pinch->fingers({{200, 240}, {440, 240}});
    pinch->fingers({});
    view->touch({Touch::Up, 200, 240});
    CHECK(settle([&] { return colour_at(out, 320, 240) == 0x175A5A; }));
}

void test_switch(const fs::path& dir) {
    const fs::path folder = dir / "Switch.manual";
    fs::create_directories(folder);
    write(folder / "1.bmp", bmp(10, 10, 1, 2, 3));
    const Touch flip{Touch::Down, 640 - kit::margin - 20, kit::margin + 20};
    const std::string remember = (dir / "second-screen.txt").string();
    {
        // No second screen of its own: the title card and the manual, which never shows by itself.
        Quiet core("gba");
        Companion c(core, nullptr, "T", "GAME BOY ADVANCE");
        CHECK(!c.has_switch());
        c.remember_switch_in(remember);
        c.set_manual(make_manual(folder.string(), ""));
        c.after_frame(core);
        CHECK(c.has_switch() && !c.showing_manual());
        CHECK(c.touch(flip) && c.showing_manual());
        // The finger lifting off the switch is not a tap on the manual's page edge.
        CHECK(!c.touch({Touch::Up, 640 - kit::margin - 20, kit::margin + 20}));
        CHECK(c.touch(flip) && !c.showing_manual());
        CHECK(c.touch(flip) && c.showing_manual());
    }
    {
        // Remembered beside the game: it comes back on the manual.
        Quiet core("gba");
        Companion c(core, nullptr, "T", "GAME BOY ADVANCE");
        c.remember_switch_in(remember);
        c.set_manual(make_manual(folder.string(), ""));
        CHECK(c.showing_manual());
    }
    {
        // A home computer: its keyboard stands in for the game's own screen, the switch steps keyboard <-> manual.
        Quiet core("zx");
        Companion c(core, nullptr, "T", "ZX SPECTRUM");
        c.set_manual(make_manual(folder.string(), ""));
        c.after_frame(core);
        CHECK(c.showing_alternative() && !c.showing_manual());
        CHECK(c.touch(flip) && c.showing_manual() && !c.showing_alternative());
        CHECK(c.touch(flip) && c.showing_alternative() && !c.showing_manual());
    }
}

} // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / ("rgds-reader-tests-" + std::to_string(::getpid()));
    fs::create_directories(dir);
    test_sources(dir);
    test_finding(dir);
    test_reader(dir);
    test_pinch(dir);
    test_overview(dir);
    test_switch(dir);
    std::error_code ec;
    fs::remove_all(dir, ec);
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
