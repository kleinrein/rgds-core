// SPDX-License-Identifier: GPL-3.0-or-later
#include "media/library.hpp"
#include "media/book.hpp"
#include "media/video.hpp"
#include "reader/pages.hpp"

#include <cctype>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

namespace media {
namespace fs = std::filesystem;

std::string key_of(const std::string& path) {
    uint64_t v = 14695981039346656037ull;
    for (unsigned char c : path) v = (v ^ c) * 1099511628211ull;
    char buf[20];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

static std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool video_ext(const std::string& e) {
    static const std::set<std::string> k = {".mkv", ".mp4", ".m4v",  ".webm", ".avi", ".mov", ".ts",  ".m2ts",
                                            ".mts", ".mpg", ".mpeg", ".wmv",  ".flv", ".ogv", ".3gp", ".vob"};
    return k.count(lower(e)) > 0;
}
bool book_ext(const std::string& e) {
    static const std::set<std::string> k = {".pdf", ".epub", ".mobi", ".fb2", ".xps", ".oxps", ".cbz",
                                            ".cbr", ".cb7",  ".cbt",  ".zip", ".txt", ".svg"};
    return k.count(lower(e)) > 0;
}
bool picture_ext(const std::string& e) {
    static const std::set<std::string> k = {".jpg", ".jpeg", ".png", ".webp", ".bmp", ".gif"};
    return k.count(lower(e)) > 0;
}

std::string tidy_name(const std::string& stem) {
    std::string s = stem;
    for (char& c : s)
        if (c == '.' || c == '_') c = ' ';
    // Bracketed release tags go: [GRP], {x}, (1080p) but not (1982).
    s = std::regex_replace(s, std::regex(R"(\[[^\]]*\]|\{[^}]*\})"), " ");
    std::smatch m;
    std::string year;
    // Everything after the year or a quality tag is the release's, not the title's.
    std::regex cut(R"(\b((19|20)\d\d)\b|\b(2160p|1080p|720p|480p|4k|uhd|bluray|blu-ray|brrip|bdrip|web-?dl|webrip|)"
                   R"(hdtv|dvdrip|x264|x265|h264|h265|hevc|xvid|aac|ac3|dts|remux|hdr|10bit)\b)",
                   std::regex::icase);
    if (std::regex_search(s, m, cut) && m.position(0) > 0) {
        if (m[1].matched) year = m[1].str();
        s = s.substr(0, size_t(m.position(0)));
    }
    s = std::regex_replace(s, std::regex(R"(\(\s*\))"), " ");
    // An episode code reads as S01E02.
    s = std::regex_replace(s, std::regex(R"(\b[sS](\d{1,2})[eE](\d{1,3})\b)"), "S$1E$2");
    s = std::regex_replace(s, std::regex(R"(\s+)"), " ");
    while (!s.empty() && (s.back() == ' ' || s.back() == '-' || s.back() == '(')) s.pop_back();
    while (!s.empty() && (s.front() == ' ' || s.front() == '-')) s.erase(s.begin());
    if (s.empty()) return stem;
    if (!year.empty() && s.find(year) == std::string::npos) s += " (" + year + ")";
    return s;
}

std::vector<Item> list_folder(const std::string& dir, bool books) {
    std::vector<Item> folders, files;
    std::error_code ec;
    for (auto it = fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const fs::path p = it->path();
        const std::string name = p.filename().string();
        if (name.empty() || name[0] == '.' || name == "run") continue;
        std::error_code e2;
        Item item;
        item.path = p.string();
        item.key = key_of(item.path);
        if (it->is_directory(e2)) {
            // A folder of pictures only is a book; any other folder is a shelf.
            int pictures = 0, media = 0, sub = 0;
            for (auto in = fs::directory_iterator(p, fs::directory_options::skip_permission_denied, e2);
                 !e2 && in != fs::directory_iterator(); in.increment(e2)) {
                const std::string e = in->path().extension().string();
                if (in->is_directory(e2)) ++sub;
                else if (picture_ext(e)) ++pictures;
                else if (books ? book_ext(e) : video_ext(e)) ++media;
            }
            if (books && pictures > 0 && media == 0 && sub == 0) {
                item.kind = Kind::Book;
                item.name = name;
                item.children = pictures;
                files.push_back(item);
            } else if (media + sub > 0) {
                item.kind = Kind::Folder;
                item.name = name;
                item.children = media + sub;
                folders.push_back(item);
            }
            continue;
        }
        item.ext = lower(p.extension().string());
        if (books ? !book_ext(item.ext) : !video_ext(item.ext)) continue;
        item.kind = books ? Kind::Book : Kind::Video;
        item.name = books ? p.stem().string() : tidy_name(p.stem().string());
        item.size = it->file_size(e2);
        auto t = it->last_write_time(e2);
        item.mtime = e2 ? 0 : int64_t(std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count());
        files.push_back(item);
    }
    auto by_name = [](const Item& a, const Item& b) {
        return play::natural_less(lower(fs::path(a.path).filename().string()),
                                  lower(fs::path(b.path).filename().string()));
    };
    std::sort(folders.begin(), folders.end(), by_name);
    std::sort(files.begin(), files.end(), by_name);
    folders.insert(folders.end(), files.begin(), files.end());
    return folders;
}

// ---- Places

void Places::load(const std::string& file) {
    file_ = file;
    places_.clear();
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream row(line);
        std::string key, pos, len, when, extra;
        if (!std::getline(row, key, '\t') || !std::getline(row, pos, '\t') || !std::getline(row, len, '\t') ||
            !std::getline(row, when, '\t'))
            continue;
        std::getline(row, extra);
        Place p;
        p.position = std::atof(pos.c_str());
        p.length = std::atof(len.c_str());
        p.when = std::atoll(when.c_str());
        p.extra = extra;
        places_[key] = p;
    }
}

void Places::save() const {
    if (file_.empty()) return;
    const std::string tmp = file_ + ".tmp";
    {
        std::ofstream out(tmp);
        for (const auto& [key, p] : places_)
            out << key << '\t' << p.position << '\t' << p.length << '\t' << p.when << '\t' << p.extra << '\n';
        if (!out) return;
    }
    std::error_code ec;
    fs::rename(tmp, file_, ec);
}

Place Places::get(const std::string& key) const {
    auto it = places_.find(key);
    return it == places_.end() ? Place{} : it->second;
}

void Places::set(const std::string& key, const Place& p) {
    Place q = p;
    if (!q.when) q.when = int64_t(std::time(nullptr));
    places_[key] = q;
}

std::vector<std::string> Places::recent(size_t max) const {
    std::vector<std::pair<int64_t, std::string>> all;
    for (const auto& [key, p] : places_)
        if (p.started() && !p.finished()) all.push_back({p.when, key});
    std::sort(all.rbegin(), all.rend());
    std::vector<std::string> out;
    for (size_t i = 0; i < all.size() && i < max; ++i) out.push_back(all[i].second);
    return out;
}

// ---- Posters

Posters::~Posters() { stop(); }

void Posters::start(const std::string& cache_dir) {
    stop();
    dir_ = cache_dir;
    std::error_code ec;
    fs::create_directories(dir_, ec);
    stop_ = false;
    thread_ = std::thread(&Posters::run, this);
}

void Posters::stop() {
    stop_ = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        wake_.notify_all();
    }
    if (thread_.joinable()) thread_.join();
}

void Posters::want(const Item& item) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (asked_.count(item.key)) {
        // Asked before and still waiting: move it to the front, the eye is there now.
        for (auto it = queue_.begin(); it != queue_.end(); ++it)
            if (it->key == item.key) {
                Item copy = *it;
                queue_.erase(it);
                queue_.push_front(copy);
                break;
            }
        return;
    }
    asked_[item.key] = true;
    queue_.push_front(item);
    wake_.notify_all();
}

std::vector<std::pair<std::string, Poster>> Posters::take() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::pair<std::string, Poster>> out;
    out.swap(done_);
    return out;
}

static std::string poster_file(const std::string& dir, const Item& item) {
    // The key holds the path; size and time make a changed file a new poster.
    return dir + "/" + item.key + "-" + std::to_string(item.size ^ uint64_t(item.mtime)) + ".px";
}

bool Posters::cached(const Item& item, Poster& out) const {
    std::ifstream f(poster_file(dir_, item), std::ios::binary);
    if (!f) return false;
    int32_t head[2] = {};
    double length = 0;
    f.read(reinterpret_cast<char*>(head), sizeof head);
    f.read(reinterpret_cast<char*>(&length), sizeof length);
    if (!f || head[0] <= 0 || head[1] <= 0 || head[0] > 2048 || head[1] > 2048) return false;
    out.w = head[0];
    out.h = head[1];
    out.length = length;
    out.px.resize(size_t(out.w) * out.h);
    f.read(reinterpret_cast<char*>(out.px.data()), std::streamsize(out.px.size() * 4));
    return bool(f);
}

void Posters::run() {
    while (!stop_) {
        Item item;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [&] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            item = queue_.front();
            queue_.pop_front();
        }
        Poster p;
        if (cached(item, p)) {
            std::lock_guard<std::mutex> lock(mutex_);
            done_.push_back({item.key, std::move(p)});
            continue;
        }
        std::string error, title;
        Item source = item;
        if (item.kind == Kind::Folder) { // a folder shows its first film or book
            bool books = false;
            for (const auto& in : list_folder(item.path, true))
                if (in.kind == Kind::Book) {
                    source = in, books = true;
                    break;
                }
            if (!books)
                for (const auto& in : list_folder(item.path, false))
                    if (in.kind == Kind::Video) {
                        source = in;
                        break;
                    }
            if (source.kind == Kind::Folder) p.failed = true;
        }
        if (!p.failed) {
            if (source.kind == Kind::Video) {
                double duration = 0;
                p.failed = !poster(source.path, .12, 320, p.px, p.w, p.h, duration, error);
                p.length = duration;
            } else {
                int pages = 0;
                p.failed = !Book::cover(source.path, 240, 340, p.px, p.w, p.h, pages, title, error);
                p.length = pages;
            }
        }
        if (item.kind == Kind::Folder) p.length = 0;
        if (!p.failed) {
            const std::string file = poster_file(dir_, item), tmp = file + ".tmp";
            std::ofstream f(tmp, std::ios::binary);
            int32_t head[2] = {p.w, p.h};
            f.write(reinterpret_cast<const char*>(head), sizeof head);
            f.write(reinterpret_cast<const char*>(&p.length), sizeof p.length);
            f.write(reinterpret_cast<const char*>(p.px.data()), std::streamsize(p.px.size() * 4));
            f.close();
            std::error_code ec;
            if (f) fs::rename(tmp, file, ec);
        } else {
            std::fprintf(stderr, "rgds-media: no picture for %s: %s\n", item.path.c_str(), error.c_str());
        }
        std::lock_guard<std::mutex> lock(mutex_);
        done_.push_back({item.key, std::move(p)});
    }
}

} // namespace media
