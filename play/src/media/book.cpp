// SPDX-License-Identifier: GPL-3.0-or-later
#include "media/book.hpp"
#include "reader/pages.hpp"

extern "C" {
#include <mupdf/fitz.h>
}

#include <algorithm>
#include <cctype>
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
            // A picture taller than the page is pushed to the next one by MuPDF when the body's own top and bottom
            // margins leave it short of room, leaving a blank page in front (Project Gutenberg's cover page does
            // this): the page has its own margins, so the body's vertical ones go.
            static const char* css = "body{margin-top:0 !important;margin-bottom:0 !important}img{margin:0}";
#if FZ_VERSION_MAJOR == 1 && FZ_VERSION_MINOR < 26
            fz_set_user_css(ctx, css); // the image's MuPDF 1.23
#endif
            impl_->doc = fz_open_document(ctx, path.c_str());
            reflowable_ = fz_is_document_reflowable(ctx, impl_->doc);
#if !(FZ_VERSION_MAJOR == 1 && FZ_VERSION_MINOR < 26)
            if (reflowable_) fz_style_document(ctx, impl_->doc, 1, css);
#endif
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

namespace {

// A file inside the EPUB's zip, or empty.
std::string archive_entry(fz_context* ctx, fz_archive* zip, const std::string& name) {
    std::string out;
    fz_buffer* buf = nullptr;
    fz_try(ctx) {
        if (fz_has_archive_entry(ctx, zip, name.c_str())) {
            buf = fz_read_archive_entry(ctx, zip, name.c_str());
            unsigned char* data = nullptr;
            size_t n = fz_buffer_storage(ctx, buf, &data);
            out.assign(reinterpret_cast<const char*>(data), n);
        }
    }
    fz_always(ctx) {
        if (buf) fz_drop_buffer(ctx, buf);
    }
    fz_catch(ctx) { out.clear(); }
    return out;
}

// The value of attribute `name` in the tag starting at `at` ("" when absent).
std::string attribute(const std::string& xml, size_t at, const std::string& name) {
    size_t end = xml.find('>', at);
    std::string tag = xml.substr(at, end == std::string::npos ? std::string::npos : end - at);
    for (size_t i = tag.find(name); i != std::string::npos; i = tag.find(name, i + 1)) {
        if (i > 0 && !std::isspace(static_cast<unsigned char>(tag[i - 1]))) continue;
        size_t eq = tag.find_first_not_of(" \t\r\n", i + name.size());
        if (eq == std::string::npos || tag[eq] != '=') continue;
        size_t q = tag.find_first_not_of(" \t\r\n", eq + 1);
        if (q == std::string::npos || (tag[q] != '"' && tag[q] != '\'')) continue;
        size_t close = tag.find(tag[q], q + 1);
        if (close == std::string::npos) return "";
        return tag.substr(q + 1, close - q - 1);
    }
    return "";
}

// Every tag named `name` (`<name ` or `<prefix:name `), as offsets.
std::vector<size_t> tags(const std::string& xml, const std::string& name) {
    std::vector<size_t> out;
    for (size_t i = xml.find('<'); i != std::string::npos; i = xml.find('<', i + 1)) {
        size_t s = i + 1, e = xml.find_first_of(" \t\r\n/>", s);
        if (e == std::string::npos) break;
        std::string t = xml.substr(s, e - s);
        size_t colon = t.find(':');
        if (colon != std::string::npos) t = t.substr(colon + 1);
        if (t == name) out.push_back(i);
    }
    return out;
}

std::string url_decode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += char(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else out += s[i];
    }
    return out;
}

// "a/b/../c.png" relative to the folder of `base`.
std::string resolve(const std::string& base, const std::string& href) {
    std::string h = url_decode(href.substr(0, href.find('#')));
    size_t slash = base.rfind('/');
    std::string joined = (slash == std::string::npos ? "" : base.substr(0, slash + 1)) + h;
    std::vector<std::string> parts;
    size_t i = 0;
    while (i <= joined.size()) {
        size_t j = joined.find('/', i);
        if (j == std::string::npos) j = joined.size();
        std::string part = joined.substr(i, j - i);
        if (part == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!part.empty() && part != ".") parts.push_back(part);
        i = j + 1;
    }
    std::string out;
    for (const auto& part : parts) out += (out.empty() ? "" : "/") + part;
    return out;
}

// The cover picture an EPUB names (EPUB 3 `cover-image`, EPUB 2 `<meta name="cover">`, or the first picture of the
// guide's cover page): its path in the zip, or empty.
std::string epub_cover_entry(fz_context* ctx, fz_archive* zip) {
    std::string container = archive_entry(ctx, zip, "META-INF/container.xml");
    std::string opf_path;
    for (size_t at : tags(container, "rootfile"))
        if (opf_path.empty()) opf_path = attribute(container, at, "full-path");
    if (opf_path.empty()) return "";
    std::string opf = archive_entry(ctx, zip, opf_path);
    if (opf.empty()) return "";
    std::vector<size_t> items = tags(opf, "item");
    auto is_picture = [&](size_t at) { return attribute(opf, at, "media-type").rfind("image/", 0) == 0; };
    for (size_t at : items) {
        std::string props = " " + attribute(opf, at, "properties") + " ";
        if (props.find(" cover-image ") != std::string::npos) return resolve(opf_path, attribute(opf, at, "href"));
    }
    std::string id;
    for (size_t at : tags(opf, "meta"))
        if (attribute(opf, at, "name") == "cover") id = attribute(opf, at, "content");
    if (!id.empty())
        for (size_t at : items)
            if (attribute(opf, at, "id") == id && is_picture(at))
                return resolve(opf_path, attribute(opf, at, "href"));
    // An item called cover that is a picture.
    for (size_t at : items) {
        std::string i = attribute(opf, at, "id"), h = attribute(opf, at, "href");
        for (auto& ch : i) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        for (auto& ch : h) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        if (is_picture(at) && (i.find("cover") != std::string::npos || h.find("cover") != std::string::npos))
            return resolve(opf_path, attribute(opf, at, "href"));
    }
    // The guide's cover page, and the first picture on it.
    for (size_t at : tags(opf, "reference")) {
        if (attribute(opf, at, "type") != "cover") continue;
        std::string page_path = resolve(opf_path, attribute(opf, at, "href"));
        std::string page = archive_entry(ctx, zip, page_path);
        for (size_t img : tags(page, "img"))
            if (std::string src = attribute(page, img, "src"); !src.empty()) return resolve(page_path, src);
        for (size_t img : tags(page, "image")) {
            std::string src = attribute(page, img, "xlink:href");
            if (src.empty()) src = attribute(page, img, "href");
            if (!src.empty()) return resolve(page_path, src);
        }
    }
    return "";
}

// The EPUB's own cover picture, fitted into max_w x max_h, over white where it is see-through.
bool epub_cover(const std::string& path, int max_w, int max_h, std::vector<uint32_t>& px, int& w, int& h) {
    fz_context* ctx = fz_new_context(nullptr, nullptr, 32u << 20);
    if (!ctx) return false;
    bool ok = false;
    fz_archive* zip = nullptr;
    fz_buffer* buf = nullptr;
    fz_image* image = nullptr;
    fz_pixmap *pix = nullptr, *rgb = nullptr, *scaled = nullptr;
    fz_try(ctx) {
        zip = fz_open_archive(ctx, path.c_str());
        std::string entry = epub_cover_entry(ctx, zip);
        if (!entry.empty() && fz_has_archive_entry(ctx, zip, entry.c_str())) {
            buf = fz_read_archive_entry(ctx, zip, entry.c_str());
            image = fz_new_image_from_buffer(ctx, buf);
            pix = fz_get_pixmap_from_image(ctx, image, nullptr, nullptr, nullptr, nullptr);
            rgb = fz_convert_pixmap(ctx, pix, fz_device_rgb(ctx), nullptr, nullptr, fz_default_color_params, 1);
            int iw = fz_pixmap_width(ctx, rgb), ih = fz_pixmap_height(ctx, rgb);
            // A tiny picture is a logo, not a cover.
            if (iw >= 64 && ih >= 64) {
                double s = std::min({double(max_w) / iw, double(max_h) / ih, 1.0});
                int tw = std::max(1, int(iw * s + .5)), th = std::max(1, int(ih * s + .5));
                scaled = fz_scale_pixmap(ctx, rgb, 0, 0, float(tw), float(th), nullptr);
                int sw = fz_pixmap_width(ctx, scaled), sh = fz_pixmap_height(ctx, scaled);
                int n = fz_pixmap_components(ctx, scaled), stride = fz_pixmap_stride(ctx, scaled);
                bool alpha = fz_pixmap_alpha(ctx, scaled) != 0;
                const unsigned char* src = fz_pixmap_samples(ctx, scaled);
                px.assign(size_t(sw) * sh, 0xFFFFFFFFu);
                for (int y = 0; y < sh; ++y) {
                    const unsigned char* row = src + size_t(y) * stride;
                    for (int x = 0; x < sw; ++x, row += n) {
                        int r = row[0], g = row[1], b = row[2];
                        if (alpha) {
                            // Premultiplied: over white.
                            int a = row[n - 1];
                            r = std::min(255, r + 255 - a), g = std::min(255, g + 255 - a),
                            b = std::min(255, b + 255 - a);
                        }
                        px[size_t(y) * sw + x] = uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16) | 0xFF000000u;
                    }
                }
                w = sw, h = sh;
                ok = true;
            }
        }
    }
    fz_always(ctx) {
        if (scaled) fz_drop_pixmap(ctx, scaled);
        if (rgb) fz_drop_pixmap(ctx, rgb);
        if (pix) fz_drop_pixmap(ctx, pix);
        if (image) fz_drop_image(ctx, image);
        if (buf) fz_drop_buffer(ctx, buf);
        if (zip) fz_drop_archive(ctx, zip);
    }
    fz_catch(ctx) { ok = false; }
    fz_drop_context(ctx);
    return ok;
}

// How much of a page is not paper: a blank page (white, or one colour) is near 0.
double ink(const std::vector<uint32_t>& px) {
    if (px.empty()) return 0;
    const uint32_t ground = px[0] & 0xFFFFFFu;
    size_t marked = 0, seen = 0;
    for (size_t i = 0; i < px.size(); i += 7, ++seen) {
        uint32_t p = px[i] & 0xFFFFFFu;
        int dr = int(p & 255) - int(ground & 255), dg = int((p >> 8) & 255) - int((ground >> 8) & 255),
            db = int((p >> 16) & 255) - int((ground >> 16) & 255);
        if (std::abs(dr) + std::abs(dg) + std::abs(db) > 48) ++marked;
    }
    return seen ? double(marked) / double(seen) : 0;
}

} // namespace

bool Book::cover(const std::string& path, int max_w, int max_h, std::vector<uint32_t>& px, int& w, int& h, int& pages,
                 std::string& title, std::string& error) {
    Book b;
    if (!b.open(path, error)) return false;
    pages = b.count();
    title = b.title();
    std::string ext = fs::path(path).extension().string();
    for (auto& ch : ext) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    if (ext == ".epub" && epub_cover(path, max_w, max_h, px, w, h)) return true;
    // The first page that has something on it (a book may open on an empty page): at most the first three.
    bool have = false;
    for (int index = 0; index < std::min(3, b.count()); ++index) {
        b.want(index, max_w, max_h, 100, Paper::White);
        std::shared_ptr<const Page> p;
        for (int i = 0; i < 400 && !p; ++i) { // at most 4 s a page
            p = b.get(index, max_w, max_h, 100, Paper::White);
            if (!p) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!p || p->failed) break;
        if (!have || ink(p->px) > .004) {
            px = p->px;
            w = p->w;
            h = p->h;
            have = true;
        }
        if (ink(p->px) > .004) break;
    }
    if (have) return true;
    error = "the first page did not render";
    return false;
}

} // namespace media
