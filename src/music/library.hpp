#pragma once
// The music library on disk: every track under the music folders with its tags, as one tab-separated file the
// launcher reads, and the covers as small BMPs beside it. A scan only opens files that are new or changed.
#include "music/decoder.hpp"
#include "music/shared.hpp"
#include "music/tracks.hpp"
#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <sys/stat.h>

namespace rgds::music {
namespace fs = std::filesystem;

// Everything FFmpeg is asked to open. Tracker modules need its libopenmpt; a build
// without them fails those files' open and the scan leaves them out.
inline bool music_extension(std::string extension) {
    static const std::set<std::string> known = {
        ".mp3", ".mp2", ".flac", ".ogg", ".oga",  ".opus", ".m4a", ".m4b", ".aac", ".wav", ".aif", ".aiff", ".aifc",
        ".wma", ".ape", ".wv",   ".mpc", ".tta",  ".mka",  ".ac3", ".dsf", ".dff", ".caf", ".au",  ".amr",  ".spx",
        ".mod", ".xm",  ".s3m",  ".it",  ".mptm", ".stm",  ".669", ".mtm", ".med", ".okt", ".ult", ".umx",  ".far"};
    for (char& c : extension) c = char(std::tolower(static_cast<unsigned char>(c)));
    return known.count(extension) > 0;
}

class Scanner {
    // A folder's own picture, looked for once per folder.
    std::map<std::string, std::string> folder_art;
    fs::path folder;

    bool have(const std::string& key) const {
        std::error_code ec;
        for (int size : {art_small, art_medium, art_large})
            if (!fs::exists(folder / "art" / (key + "-" + std::to_string(size) + ".bmp"), ec)) return false;
        return true;
    }
    std::string art_for_folder(const fs::path& directory) {
        const auto found = folder_art.find(directory.string());
        if (found != folder_art.end()) return found->second;
        std::string key;
        static const char* names[] = {"cover", "folder", "front", "album", "albumart", "artwork"};
        static const char* types[] = {".jpg", ".jpeg", ".png", ".webp", ".bmp"};
        std::error_code ec;
        std::map<std::string, fs::path> present; // lower-case name to the file as it is spelt
        for (const auto& entry : fs::directory_iterator(directory, ec)) {
            std::string name = entry.path().filename().string();
            for (char& c : name) c = char(std::tolower(static_cast<unsigned char>(c)));
            present[name] = entry.path();
        }
        for (const char* name : names) {
            for (const char* type : types) {
                const auto at = present.find(std::string(name) + type);
                if (at == present.end()) continue;
                struct stat st{};
                if (stat(at->second.c_str(), &st) != 0) continue;
                const std::string identity =
                    at->second.string() + "|" + std::to_string(st.st_mtime) + "|" + std::to_string(st.st_size);
                const std::string candidate = hex64(fnv1a(identity.data(), identity.size()));
                if (have(candidate) ||
                    write_pictures_from_file(at->second.string(), (folder / "art" / candidate).string(),
                                             {art_small, art_medium, art_large})) {
                    key = candidate;
                    break;
                }
            }
            if (!key.empty()) break;
        }
        return folder_art[directory.string()] = key;
    }

public:
    explicit Scanner(fs::path library_folder) : folder(std::move(library_folder)) {}

    // Reads one file's tags and makes sure its cover exists. False when FFmpeg cannot open it as sound.
    bool read(Track& t) {
        Decoder file;
        if (!file.open(t.path, 48000, false)) return false;
        t.info = file.info;
        t.art.clear();
        if (const AVPacket* picture = file.picture()) {
            // Tracks of one album carry the same picture: keyed by its bytes, it is stored once.
            const size_t sample = std::min<size_t>(size_t(picture->size), 65536);
            const std::string key =
                hex64(fnv1a(picture->data, sample, 14695981039346656037ull ^ uint64_t(picture->size)));
            if (have(key) || write_pictures(file.picture_parameters(), picture, (folder / "art" / key).string(),
                                            {art_small, art_medium, art_large}))
                t.art = key;
        }
        if (t.art.empty()) t.art = art_for_folder(fs::path(t.path).parent_path());
        return true;
    }

    // Walks `roots`, keeps what `known` already has for unchanged files, reads the rest. `progress(done, total)`
    // is called as it goes and returns false to stop; `partial` gets the list so far every 200 new tracks.
    std::vector<Track> scan(const std::vector<fs::path>& roots, const std::vector<Track>& known,
                            const std::function<bool(size_t, size_t)>& progress,
                            const std::function<void(const std::vector<Track>&)>& partial) {
        std::error_code ec;
        fs::create_directories(folder / "art", ec);
        std::map<std::string, const Track*> cache;
        for (const Track& t : known) cache[t.path] = &t;
        std::vector<std::string> files;
        for (const fs::path& root : roots) {
            fs::recursive_directory_iterator walk(root, fs::directory_options::skip_permission_denied, ec), end;
            for (; !ec && walk != end; walk.increment(ec)) {
                const std::string name = walk->path().filename().string();
                // Hidden folders, and the "._" twins macOS leaves on exFAT cards, are not music.
                if (!name.empty() && name[0] == '.') {
                    if (walk->is_directory(ec)) walk.disable_recursion_pending();
                    continue;
                }
                if (walk->is_regular_file(ec) && music_extension(walk->path().extension().string()))
                    files.push_back(walk->path().string());
            }
        }
        std::sort(files.begin(), files.end());
        files.erase(std::unique(files.begin(), files.end()), files.end());
        std::vector<Track> tracks;
        tracks.reserve(files.size());
        size_t fresh = 0;
        for (size_t i = 0; i < files.size(); i++) {
            if (!progress(i, files.size())) break;
            struct stat st{};
            if (stat(files[i].c_str(), &st) != 0) continue;
            const auto cached = cache.find(files[i]);
            if (cached != cache.end() && cached->second->mtime == int64_t(st.st_mtime) &&
                cached->second->size == int64_t(st.st_size)) {
                tracks.push_back(*cached->second);
                continue;
            }
            Track t;
            t.path = files[i];
            t.mtime = int64_t(st.st_mtime);
            t.size = int64_t(st.st_size);
            if (!read(t)) continue;
            tracks.push_back(std::move(t));
            if (++fresh % 200 == 0) partial(tracks);
        }
        return tracks;
    }
};
} // namespace rgds::music
