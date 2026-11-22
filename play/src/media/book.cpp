// SPDX-License-Identifier: GPL-3.0-or-later
#include "media/book.hpp"
#include "reader/pages.hpp"

extern "C" {
#include <mupdf/fitz.h>
}

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

namespace media {
namespace fs = std::filesystem;

void apply_paper(std::vector<uint32_t>& px, Paper paper, bool keep_pictures) {
    if (paper == Paper::White) return;
    for (uint32_t& p : px) {
        int r = p & 255, g = (p >> 8) & 255, b = (p >> 16) & 255;
        if (paper == Paper::Sepia) {
            // White becomes warm paper (#F4ECD8), black a soft brown-black; colours keep their hue.
            r = 0x2A + r * (0xF4 - 0x2A) / 255;
            g = 0x22 + g * (0xEC - 0x22) / 255;
            b = 0x18 + b * (0xD8 - 0x18) / 255;
        } else {
            // Night: the lightness turns over (white paper dark, black text light), the hue stays, so a red heading
            // is still red. A comic keeps its pictures as drawn, only dimmed (keep_pictures).
            if (keep_pictures) {
                r = r * 200 / 255, g = g * 200 / 255, b = b * 200 / 255;
            } else {
                int mx = std::max({r, g, b}), mn = std::min({r, g, b});
                int shift = 255 - mx - mn; // L -> 1 - L, chroma kept
                r = std::clamp(r + shift, 0, 255), g = std::clamp(g + shift, 0, 255), b = std::clamp(b + shift, 0, 255);
                // Paper at #15171C rather than pure black, text at #D8DCE2: softer on the eyes in the dark.
                r = 0x15 + r * (0xD8 - 0x15) / 255;
                g = 0x17 + g * (0xDC - 0x17) / 255;
                b = 0x1C + b * (0xE2 - 0x1C) / 255;
            }
        }
        p = uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16) | (p & 0xFF000000u);
    }
}

struct Book::Impl {
    fz_context* ctx = nullptr;
    fz_document* doc = nullptr;
    play::Pages pictures; // a folder of pictures
    bool folder = false;
    int em = 0, lw = 0, lh = 0;
    ~Impl() {
        if (ctx) {
            if (doc) fz_drop_document(ctx, doc);
            fz_drop_context(ctx);
        }
    }
};

Book::Book() {}
Book::~Book() { close(); }

void Book::close() {
    stop_ = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        wake_.notify_all();
    }
    if (thread_.joinable()) thread_.join();
    impl_.reset();
    jobs_.clear();
    cache_.clear();
    shapes_.clear();
    opened_ = false;
    count_ = 0;
}

static std::string meta(fz_context* ctx, fz_document* doc, const char* key) {
    char buf[512] = {};
    fz_try(ctx) {
        if (fz_lookup_metadata(ctx, doc, key, buf, sizeof buf) <= 0) buf[0] = 0;
    }
    fz_catch(ctx) { buf[0] = 0; }
    return buf;
}

bool Book::open(const std::string& path, std::string& error) {
    close();
    stop_ = false;
    path_ = path;
    impl_ = std::make_unique<Impl>();
    std::error_code ec;
    if (fs::is_directory(path, ec)) {
        impl_->folder = true;
        impl_->pictures = play::open_folder(path);
        if (!impl_->pictures.ok()) {
            error = impl_->pictures.error.empty() ? "no pictures in the folder" : impl_->pictures.error;
            return false;
        }
        count_ = int(impl_->pictures.pages.size());
        comic_ = true;
        title_ = fs::path(path).filename().string();
    } else {
        impl_->ctx = fz_new_context(nullptr, nullptr, 64u << 20); // a 64 MiB store: fonts, images, glyphs
        if (!impl_->ctx) {
            error = "out of memory";
            return false;
        }
        fz_context* ctx = impl_->ctx;
        bool ok = true;
        fz_try(ctx) {
            fz_register_document_handlers(ctx);
            impl_->doc = fz_open_document(ctx, path.c_str());
            reflowable_ = fz_is_document_reflowable(ctx, impl_->doc);
            if (reflowable_) {
                // A first layout for a 480x640 page; the reader lays it out again for its own size.
                fz_layout_document(ctx, impl_->doc, 360, 480, 11);
                impl_->lw = 360, impl_->lh = 480, impl_->em = 11;
            }
            count_ = fz_count_pages(ctx, impl_->doc);
        }
        fz_catch(ctx) {
            error = fz_caught_message(ctx);
            ok = false;
        }
        if (!ok || count_ <= 0) {
            if (ok) error = "the file has no pages";
            return false;
        }
        title_ = meta(ctx, impl_->doc, FZ_META_INFO_TITLE);
        author_ = meta(ctx, impl_->doc, FZ_META_INFO_AUTHOR);
        std::string format = meta(ctx, impl_->doc, FZ_META_FORMAT);
        for (auto& c : format) c = char(std::toupper(static_cast<unsigned char>(c)));
        comic_ = format.find("CBZ") != std::string::npos || format.find("CBT") != std::string::npos ||
                 format.find("CBR") != std::string::npos || format.find("CB7") != std::string::npos ||
                 format.find("IMAGE") != std::string::npos || format.find("ARCHIVE") != std::string::npos;
        if (title_.empty()) title_ = fs::path(path).stem().string();
    }
    shapes_.assign(size_t(count_), {0.0, 0.0});
    opened_ = true;
    thread_ = std::thread(&Book::worker, this);
    return true;
}

void Book::run_task(std::function<void()> f) {
    std::unique_lock<std::mutex> lock(mutex_);
    task_ = &f;
    task_done_ = false;
    wake_.notify_all();
    wake_.wait(lock, [&] { return task_done_ || stop_; });
    task_ = nullptr;
}

int Book::relayout(int w, int h, int em, int keep) {
    if (!reflowable_ || !impl_ || !impl_->doc) return keep;
    if (impl_->lw == w && impl_->lh == h && impl_->em == em) return keep;
    int result = keep;
    run_task([&] {
        fz_context* ctx = impl_->ctx;
        fz_try(ctx) {
            fz_location loc = fz_location_from_page_number(ctx, impl_->doc, std::clamp(keep, 0, count_ - 1));
            fz_bookmark mark = fz_make_bookmark(ctx, impl_->doc, loc);
            fz_layout_document(ctx, impl_->doc, float(w), float(h), float(em));
            impl_->lw = w, impl_->lh = h, impl_->em = em;
            count_ = fz_count_pages(ctx, impl_->doc);
            result = fz_page_number_from_location(ctx, impl_->doc, fz_lookup_bookmark(ctx, impl_->doc, mark));
        }
        fz_catch(ctx) { result = std::min(keep, count_ - 1); }
    });
    std::lock_guard<std::mutex> lock(mutex_);
    ++layout_;
    cache_.clear();
    jobs_.clear();
    shapes_.assign(size_t(std::max(0, count_.load())), {0.0, 0.0});
    return std::clamp(result, 0, std::max(0, count_ - 1));
}

std::vector<Heading> Book::outline() {
    std::vector<Heading> out;
    if (!impl_ || !impl_->doc) return out;
    run_task([&] {
        fz_context* ctx = impl_->ctx;
        fz_outline* root = nullptr;
        fz_try(ctx) { root = fz_load_outline(ctx, impl_->doc); }
        fz_catch(ctx) { root = nullptr; }
        std::function<void(fz_outline*, int)> walk = [&](fz_outline* o, int level) {
            for (; o; o = o->next) {
                Heading h;
                h.title = o->title ? o->title : "";
                h.level = level;
                fz_try(ctx) {
                    fz_location loc = o->page;
                    // EPUB outlines carry a link to resolve rather than a page.
                    if (loc.chapter < 0 && o->uri) loc = fz_resolve_link(ctx, impl_->doc, o->uri, nullptr, nullptr);
                    h.page = loc.chapter < 0 ? -1 : fz_page_number_from_location(ctx, impl_->doc, loc);
                }
                fz_catch(ctx) { h.page = -1; }
                if (!h.title.empty() && h.page >= 0) out.push_back(h);
                if (o->down && level < 3) walk(o->down, level + 1);
            }
        };
        walk(root, 0);
        if (root) fz_drop_outline(ctx, root);
    });
    return out;
}

void Book::want(int index, int box_w, int box_h, int zoom, Paper paper) {
    if (index < 0 || index >= count_) return;
    std::lock_guard<std::mutex> lock(mutex_);
    Job j{index, box_w, box_h, zoom, paper, layout_};
    for (const auto& p : cache_)
        if (same(*p, j)) return;
    // The page asked for goes to the front: it is the one the reader is waiting to see.
    jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(),
                               [&](const Job& o) {
                                   return o.index == j.index && o.box_w == j.box_w && o.box_h == j.box_h &&
                                          o.zoom == j.zoom && o.paper == j.paper;
                               }),
                jobs_.end());
    jobs_.push_front(j);
    wake_.notify_all();
}

void Book::prefetch(const std::vector<int>& pages, int box_w, int box_h, int zoom, Paper paper) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Old prefetches go: the reader has moved on.
    while (jobs_.size() > 1) jobs_.pop_back();
    for (int index : pages) {
        if (index < 0 || index >= count_) continue;
        Job j{index, box_w, box_h, zoom, paper, layout_};
        bool have = false;
        for (const auto& p : cache_)
            if (same(*p, j)) have = true;
        if (!have) jobs_.push_back(j);
    }
    wake_.notify_all();
}

std::shared_ptr<const Page> Book::get(int index, int box_w, int box_h, int zoom, Paper paper) {
    std::lock_guard<std::mutex> lock(mutex_);
    Job j{index, box_w, box_h, zoom, paper, layout_};
    for (auto it = cache_.begin(); it != cache_.end(); ++it)
        if (same(**it, j)) {
            auto p = *it;
            if (it != cache_.begin()) cache_.splice(cache_.begin(), cache_, it);
            return p;
        }
    return nullptr;
}

bool Book::shape(int index, double& w, double& h) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (index < 0 || size_t(index) >= shapes_.size()) return false;
    w = shapes_[size_t(index)].first;
    h = shapes_[size_t(index)].second;
    return w > 0 && h > 0;
}

bool Book::arrived() {
    std::lock_guard<std::mutex> lock(mutex_);
    bool a = arrived_;
    arrived_ = false;
    return a;
}

void Book::worker() {
    for (;;) {
        Job job{};
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [&] { return stop_ || task_ || !jobs_.empty(); });
            if (stop_) return;
            if (task_ && !task_done_) {
                (*task_)();
                task_done_ = true;
                wake_.notify_all();
                continue;
            }
            if (jobs_.empty()) continue;
            job = jobs_.front();
            jobs_.pop_front();
            if (job.layout != layout_) continue;
            bool have = false;
            for (const auto& p : cache_)
                if (same(*p, job)) have = true;
            if (have) continue;
        }
        auto page = render(job);
        std::lock_guard<std::mutex> lock(mutex_);
        if (job.layout != layout_ || !page) continue;
        if (size_t(job.index) < shapes_.size() && page->page_w > 0)
            shapes_[size_t(job.index)] = {page->page_w, page->page_h};
        cache_.push_front(page);
        // About 12 panels' worth of pages (480x640 RGBA is 1.2 MB): the reader, both neighbours, a spread back.
        size_t budget = 0;
        for (auto it = cache_.begin(); it != cache_.end();) {
            budget += (*it)->px.size() * 4;
            if (budget > (24u << 20) && it != cache_.begin()) it = cache_.erase(it);
            else ++it;
        }
        arrived_ = true;
    }
}

std::shared_ptr<Page> Book::render(const Job& j) {
    auto page = std::make_shared<Page>();
    page->index = j.index;
    page->box_w = j.box_w;
    page->box_h = j.box_h;
    page->zoom = j.zoom;
    page->paper = j.paper;
    page->layout = j.layout;
    const double zoom = std::clamp(j.zoom, 25, 800) / 100.0;
    if (impl_->folder) {
        std::vector<uint32_t> px;
        int w = 0, h = 0;
        std::string error;
        if (!play::read_page(impl_->pictures, size_t(j.index), px, w, h, error, 4096)) {
            page->failed = true;
            return page;
        }
        page->page_w = w;
        page->page_h = h;
        // Scaled to the box by area averaging, done once here.
        double s = std::min(double(j.box_w) / w, double(j.box_h) / h) * zoom;
        int tw = std::max(1, int(w * s)), th = std::max(1, int(h * s));
        tw = std::min(tw, 4096), th = std::min(th, 4096);
        page->w = tw;
        page->h = th;
        page->px.resize(size_t(tw) * th);
        for (int y = 0; y < th; ++y) {
            int y0 = int(int64_t(y) * h / th), y1 = std::max(y0 + 1, int(int64_t(y + 1) * h / th));
            for (int x = 0; x < tw; ++x) {
                int x0 = int(int64_t(x) * w / tw), x1 = std::max(x0 + 1, int(int64_t(x + 1) * w / tw));
                uint32_t r = 0, g = 0, b = 0, n = 0;
                for (int yy = y0; yy < y1; yy += std::max(1, (y1 - y0) / 3))
                    for (int xx = x0; xx < x1; xx += std::max(1, (x1 - x0) / 3), ++n) {
                        uint32_t p = px[size_t(yy) * w + xx];
                        r += p & 255, g += (p >> 8) & 255, b += (p >> 16) & 255;
                    }
                page->px[size_t(y) * tw + x] = (r / n) | ((g / n) << 8) | ((b / n) << 16) | 0xFF000000u;
            }
        }
        apply_paper(page->px, j.paper, true);
        return page;
    }
    fz_context* ctx = impl_->ctx;
    fz_page* p = nullptr;
    fz_pixmap* pix = nullptr;
    fz_try(ctx) {
        p = fz_load_page(ctx, impl_->doc, j.index);
        fz_rect bounds = fz_bound_page(ctx, p);
        double pw = bounds.x1 - bounds.x0, ph = bounds.y1 - bounds.y0;
        page->page_w = pw;
        page->page_h = ph;
        double s = std::min(j.box_w / std::max(1.0, pw), j.box_h / std::max(1.0, ph)) * zoom;
        // Never past 4096 a side (the GPU's texture limit on the Mali G52 is 8192; memory is the real limit).
        s = std::min(s, 4096.0 / std::max(pw, ph));
        fz_matrix ctm = fz_scale(float(s), float(s));
        pix = fz_new_pixmap_from_page(ctx, p, ctm, fz_device_rgb(ctx), 0);
        int w = fz_pixmap_width(ctx, pix), h = fz_pixmap_height(ctx, pix), n = fz_pixmap_components(ctx, pix);
        int stride = fz_pixmap_stride(ctx, pix);
        const unsigned char* src = fz_pixmap_samples(ctx, pix);
        page->w = w;
        page->h = h;
        page->px.resize(size_t(w) * h);
        for (int y = 0; y < h; ++y) {
            const unsigned char* row = src + size_t(y) * stride;
            uint32_t* out = page->px.data() + size_t(y) * w;
            for (int x = 0; x < w; ++x, row += n)
                out[x] = uint32_t(row[0]) | (uint32_t(row[1]) << 8) | (uint32_t(row[2]) << 16) | 0xFF000000u;
        }
    }
    fz_always(ctx) {
        if (pix) fz_drop_pixmap(ctx, pix);
        if (p) fz_drop_page(ctx, p);
    }
    fz_catch(ctx) {
        page->failed = true;
        page->px.clear();
        return page;
    }
    apply_paper(page->px, j.paper, comic_);
    return page;
}

bool Book::cover(const std::string& path, int max_w, int max_h, std::vector<uint32_t>& px, int& w, int& h, int& pages,
                 std::string& title, std::string& error) {
    Book b;
    if (!b.open(path, error)) return false;
    pages = b.count();
    title = b.title();
    b.want(0, max_w, max_h, 100, Paper::White);
    for (int i = 0; i < 400; ++i) { // at most 4 s
        if (auto p = b.get(0, max_w, max_h, 100, Paper::White)) {
            if (p->failed) break;
            px = p->px;
            w = p->w;
            h = p->h;
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    error = "the first page did not render";
    return false;
}

} // namespace media
