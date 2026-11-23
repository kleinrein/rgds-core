// SPDX-License-Identifier: GPL-3.0-or-later
// What is on the card: the Videos and Books folders as shelves, a picture for each (a frame of the film, a book's
// cover), and where each was left.
//
// Posters are made on a worker thread and kept in the state folder (posters/<key>.px: width, height, pixels), so a
// shelf opens with its pictures at once the second time. A folder is a shelf of its own; a folder holding only
// pictures is a book (a comic as loose pages).
#pragma once
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace media {

enum class Kind { Video, Book, Folder };

struct Item {
    Kind kind = Kind::Video;
    std::string path, name, ext;
    std::string key; // stable: a hash of the path
    uint64_t size = 0;
    int64_t mtime = 0;
    int children = 0; // a folder: what is inside
};

// Where a film or book was left.
struct Place {
    double position = 0; // seconds; a book: the page
    double length = 0;   // seconds; a book: its pages
    int64_t when = 0;    // unix time, for "Continue"
    std::string extra;   // a book's own settings: "dir=rtl;mode=book;paper=sepia;em=12"
    double fraction() const { return length > 0 ? std::clamp(position / length, 0.0, 1.0) : 0; }
    bool started() const { return position > 0.5; }
    bool finished() const { return length > 0 && position >= length * .97; }
};

class Places {
public:
    void load(const std::string& file);
    void save() const;
    Place get(const std::string& key) const;
    void set(const std::string& key, const Place& p);
    // Keys with a place, newest first.
    std::vector<std::string> recent(size_t max) const;

private:
    std::string file_;
    std::map<std::string, Place> places_;
};

std::string key_of(const std::string& path);
// "The.Thing.1982.1080p.BluRay.x264-GRP" -> "The Thing (1982)"; "s01e02" kept as "S01E02".
std::string tidy_name(const std::string& stem);
bool video_ext(const std::string& ext);
bool book_ext(const std::string& ext);
bool picture_ext(const std::string& ext);

// The items of one folder: subfolders first, then files, in natural order. `books`: the Books shelf's rules.
std::vector<Item> list_folder(const std::string& dir, bool books);

struct Poster {
    int w = 0, h = 0;
    std::vector<uint32_t> px;
    double length = 0; // seconds or pages, learnt while making it
    bool failed = false;
};

// Posters made in the background, nearest the eye first.
class Posters {
public:
    ~Posters();
    void start(const std::string& cache_dir);
    void stop();
    // Ask for an item's poster; the newest asks are made first.
    void want(const Item& item);
    // Finished ones since the last call: key -> poster.
    std::vector<std::pair<std::string, Poster>> take();
    // A poster already on the card, read at once (null when there is none).
    bool cached(const Item& item, Poster& out) const;
    // Without the thread (rgds-media --index): the folder posters are kept in, and one made (or read) now.
    void set_folder(const std::string& dir) { dir_ = dir; }
    bool make(const Item& item, Poster& out);

private:
    void run();
    std::string dir_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Item> queue_;
    std::map<std::string, bool> asked_;
    std::vector<std::pair<std::string, Poster>> done_;
};

// Where an item's poster is kept: <key>-<size^time>.px, the launcher's BMPs beside it (.bmp, .blur.bmp).
std::string poster_path(const std::string& dir, const Item& item);

// rgds-media --index: every shelf listed into STATE/index.tsv at once, then a poster (and its blur) made for
// each, the index rewritten as they come. What the launcher's Media level shows (src/launcher/media_shelf.hpp).
int make_index(const std::vector<std::string>& videos, const std::vector<std::string>& books,
               const std::string& state_dir);

} // namespace media
