// SPDX-License-Identifier: GPL-3.0-or-later
// RG DS Media's engine checks (tests/test_media.py makes the files and runs this):
//   media-tests video FILM     open, play, the clock, an exact seek, thumbnails, a poster
//   media-tests book FILE N    open, N pages, render page 0 and the last, a cover; EPUB: relayout keeps the place
//   media-tests names          tidy_name on release names
#include "media/book.hpp"
#include "media/library.hpp"
#include "media/video.hpp"

#include <SDL.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

using namespace media;

static int failures = 0;
#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::fprintf(stderr, "FAIL %s:%d %s: ", __FILE__, __LINE__, #cond);                                        \
            std::fprintf(stderr, __VA_ARGS__);                                                                         \
            std::fprintf(stderr, "\n");                                                                                \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

static void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

static int video(const char* path) {
    SDL_SetHint(SDL_HINT_AUDIODRIVER, "dummy");
    SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER);
    Player p;
    std::string error;
    CHECK(p.open(path, 0, error), "%s", error.c_str());
    if (!error.empty()) return 1;
    const Info& in = p.info();
    CHECK(in.duration > 9 && in.duration < 11, "duration %f", in.duration);
    CHECK(in.w == 320 && in.h == 240, "size %dx%d", in.w, in.h);
    CHECK(in.audio.size() == 1, "audio tracks %zu", in.audio.size());
    CHECK(in.chapters.size() == 2, "chapters %zu", in.chapters.size());
    CHECK(in.subtitles.size() == 1, "subtitle tracks %zu", in.subtitles.size());
    // The first picture comes while paused.
    bool fresh = false;
    const Picture* pic = nullptr;
    for (int i = 0; i < 200 && !pic; ++i, sleep_ms(5)) pic = p.picture(fresh);
    CHECK(pic && pic->w == 320, "no first picture");
    p.play();
    sleep_ms(600);
    for (int i = 0; i < 20; ++i, sleep_ms(10)) p.picture(fresh);
    double pos = p.position();
    CHECK(pos > .3 && pos < 1.2, "clock after 0.6 s: %f", pos);
    // Exact seek: the next picture shown is the one at 5.0 s (25 fps: 0.04 s apart).
    p.pause();
    p.seek(5.0, true);
    const Picture* at = nullptr;
    for (int i = 0; i < 300; ++i, sleep_ms(5)) {
        const Picture* q = p.picture(fresh);
        if (q && fresh && std::abs(q->pts - 5.0) < .5) {
            at = q;
            break;
        }
    }
    CHECK(at && std::abs(at->pts - 5.0) < .041, "seek landed at %f", at ? at->pts : -1);
    CHECK(std::abs(p.position() - 5.0) < .05, "clock after seek %f", p.position());
    // Subtitles: the line at 2 s (the file has one from 1 to 4 s).
    p.select_subtitles(0);
    p.seek(2.0, true);
    for (int i = 0; i < 200; ++i, sleep_ms(5)) p.picture(fresh);
    std::string line;
    for (int i = 0; i < 100 && line.empty(); ++i, sleep_ms(10)) {
        p.picture(fresh);
        line = p.subtitle_text();
    }
    CHECK(line == "Hello from the test", "subtitle line '%s'", line.c_str());
    Overlay o;
    p.subtitles(o, 640, 480, 100);
    CHECK(!o.empty, "subtitle overlay empty");
    // Speed: sound and clock run faster, the picture keeps up.
    p.seek(1.0, true);
    p.set_speed(2.0);
    p.play();
    sleep_ms(200);
    double a = p.position();
    sleep_ms(500);
    double b = p.position();
    CHECK(b - a > .7 && b - a < 1.4, "2x speed moved %f in 0.5 s", b - a);
    auto stats = p.stats();
    CHECK(stats.decoded > 10, "decoded %llu", static_cast<unsigned long long>(stats.decoded));
    p.close();
    // Thumbnails and the poster.
    Thumbnails t;
    t.start(path, 8, 96, 54);
    int got = 0;
    for (int i = 0; i < 400 && got < 8; ++i, sleep_ms(10)) got += int(t.take_new().size());
    CHECK(got == 8, "thumbnails %d of 8", got);
    std::vector<uint32_t> px;
    int w = 0, h = 0;
    double d = 0;
    CHECK(poster(path, .1, 160, px, w, h, d, error), "poster: %s", error.c_str());
    CHECK(w == 160 && h == 120 && d > 9, "poster %dx%d %f", w, h, d);
    SDL_Quit();
    return failures ? 1 : 0;
}

static int book(const char* path, int pages) {
    Book b;
    std::string error;
    CHECK(b.open(path, error), "%s", error.c_str());
    if (!b.ok()) return 1;
    CHECK(b.count() == pages || b.reflows(), "pages %d, wanted %d", b.count(), pages);
    for (int index : {0, b.count() - 1}) {
        b.want(index, 480, 640, 100, Paper::White);
        std::shared_ptr<const Page> pg;
        for (int i = 0; i < 500 && !pg; ++i, sleep_ms(5)) pg = b.get(index, 480, 640, 100, Paper::White);
        CHECK(pg && !pg->failed && pg->w > 100 && pg->h > 100, "page %d not rendered", index);
        if (pg && !pg->px.empty()) {
            // Ink on paper: not all one colour.
            uint32_t first = pg->px[0];
            bool varied = false;
            for (uint32_t v : pg->px)
                if (v != first) varied = true;
            CHECK(varied, "page %d is blank", index);
        }
    }
    if (b.reflows()) {
        int before = b.count();
        int keep = b.count() / 2;
        int now = b.relayout(200, 260, 16, keep); // larger text, smaller page: more pages
        CHECK(b.count() > before, "relayout pages %d -> %d", before, b.count());
        CHECK(now >= keep, "relayout kept %d -> %d", keep, now);
        auto toc = b.outline();
        CHECK(toc.size() >= 2, "outline %zu", toc.size());
    }
    // Night paper inverts the light, keeps the hue.
    std::vector<uint32_t> px = {0xFFFFFFFFu, 0xFF000000u};
    apply_paper(px, Paper::Night, false);
    CHECK((px[0] & 255) < 60 && (px[1] & 255) > 180, "night %08x %08x", px[0], px[1]);
    std::vector<uint32_t> cover;
    int w = 0, h = 0, n = 0;
    std::string title;
    CHECK(Book::cover(path, 120, 160, cover, w, h, n, title, error), "cover: %s", error.c_str());
    return failures ? 1 : 0;
}

static int names() {
    struct {
        const char* in;
        const char* out;
    } cases[] = {
        {"The.Thing.1982.1080p.BluRay.x264-GRP", "The Thing (1982)"},
        {"Show.Name.s01e02.720p.WEB-DL", "Show Name S01E02"},
        {"[Group] Some Anime - 05 [1080p]", "Some Anime - 05"},
        {"Holiday video", "Holiday video"},
    };
    for (auto& c : cases) {
        std::string got = tidy_name(c.in);
        CHECK(got == c.out, "tidy_name(%s) = '%s', wanted '%s'", c.in, got.c_str(), c.out);
    }
    return failures ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc >= 3 && !std::strcmp(argv[1], "video")) return video(argv[2]);
    if (argc >= 4 && !std::strcmp(argv[1], "book")) return book(argv[2], std::atoi(argv[3]));
    if (argc >= 2 && !std::strcmp(argv[1], "names")) return names();
    std::fprintf(stderr, "usage: media-tests video FILM | book FILE PAGES | names\n");
    return 2;
}
