// SPDX-License-Identifier: GPL-3.0-or-later
// A book, comic or document: MuPDF opens PDF, EPUB, MOBI, FB2, XPS, CBZ/CBT (and CBR/CB7 where it was built with
// libarchive) and pictures; a folder of pictures goes through play/src/reader. Every MuPDF call happens on one worker
// thread that owns the context, so the draw thread only ever receives finished pages.
//
// Pages are asked for at a size (the panel, turned for book mode) and a zoom; finished pages wait in a small cache
// keyed by page, size, zoom and paper, and the neighbours of the page asked for are rendered next, so a turn never
// waits.
//
// Reflowing books (EPUB and the like) are laid out for the panel at the chosen text size; the place in the book is
// kept as a MuPDF bookmark, so a new size keeps the reader on the same words.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace media {

enum class Paper { White, Sepia, Night };

struct Heading {
    std::string title;
    int page = 0, level = 0;
};

struct Page {
    int index = -1, w = 0, h = 0; // the picture's own size
    std::vector<uint32_t> px;     // bytes R, G, B, A
    // What was asked for, so a stale result is never shown for a new request.
    int box_w = 0, box_h = 0, zoom = 100;
    Paper paper = Paper::White;
    uint64_t layout = 0;
    double page_w = 0, page_h = 0; // the page's own shape (points), for spreads
    bool failed = false;
};

class Book {
public:
    Book();
    ~Book();
    Book(const Book&) = delete;
    Book& operator=(const Book&) = delete;

    bool open(const std::string& path, std::string& error);
    void close();
    bool ok() const { return opened_; }
    const std::string& title() const { return title_; }
    const std::string& author() const { return author_; }
    bool reflows() const { return reflowable_; }
    bool comic() const { return comic_; } // pictures only: a comic or manga
    int count() const { return count_; }
    // EPUB: lays the book out again for a w x h page with text of `em` points, returns the page now holding
    // `keep` (a page of the old layout).
    int relayout(int w, int h, int em, int keep);
    uint64_t layout() const { return layout_; }
    std::vector<Heading> outline();

    // Asks for page `index` to fit box_w x box_h (whole page) at zoom percent; non-blocking.
    void want(int index, int box_w, int box_h, int zoom, Paper paper);
    // Pages to render after the asked one, in order (the next pages first).
    void prefetch(const std::vector<int>& pages, int box_w, int box_h, int zoom, Paper paper);
    // The finished page, or null while it is on its way.
    std::shared_ptr<const Page> get(int index, int box_w, int box_h, int zoom, Paper paper);
    // A page's shape in points (from the cache of shapes, filled as pages are seen; 0 when unknown).
    bool shape(int index, double& w, double& h);
    // Whether a finished page arrived since the last call.
    bool arrived();

    // A small picture of page 0 for the library.
    static bool cover(const std::string& path, int max_w, int max_h, std::vector<uint32_t>& px, int& w, int& h,
                      int& pages, std::string& title, std::string& error);

private:
    struct Job {
        int index, box_w, box_h, zoom;
        Paper paper;
        uint64_t layout;
    };
    void worker();
    std::shared_ptr<Page> render(const Job& j);
    bool same(const Page& p, const Job& j) const {
        return p.index == j.index && p.box_w == j.box_w && p.box_h == j.box_h && p.zoom == j.zoom &&
               p.paper == j.paper && p.layout == j.layout;
    }

    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string path_, title_, author_;
    bool opened_ = false, reflowable_ = false, comic_ = false;
    std::atomic<int> count_{0};
    std::atomic<uint64_t> layout_{1};

    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> jobs_;
    std::list<std::shared_ptr<Page>> cache_; // most recent first
    std::vector<std::pair<double, double>> shapes_;
    bool arrived_ = false;
    // Work the worker runs for the caller (relayout, outline), one at a time.
    std::function<void()>* task_ = nullptr;
    bool task_done_ = false;
    void run_task(std::function<void()> f);
};

// The paper: night inverts the light of the page but keeps its colours' hue, sepia warms the white.
void apply_paper(std::vector<uint32_t>& px, Paper paper, bool keep_pictures);

} // namespace media
