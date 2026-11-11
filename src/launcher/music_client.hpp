#pragma once
// The launcher's side of the music player: the library as lists to browse (songs, albums, artists, folders), the
// queue and its shuffle, favourites, and the engine (src/music/engine.cpp) as a process to start, talk to and read.
// No SDL here: times are plain milliseconds, so the rules can be tested without a window.
#include "music/eq.hpp"
#include "music/shared.hpp"
#include "music/tracks.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <map>
#include <random>
#include <set>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace rgds {
struct MusicAlbum {
    std::string name, artist, art;
    int year = 0;
    int64_t duration_ms = 0;
    std::vector<int> tracks; // by disc, then track number
};
struct MusicArtist {
    std::string name;
    std::vector<int> albums;
    std::vector<int> tracks; // album by album
};
struct MusicFolderEntry {
    bool folder = false;
    std::string name, path;
    int track = -1; // a file: its track
    int count = 0;  // a folder: the tracks under it
};

class MusicLibrary {
public:
    std::vector<music::Track> tracks; // in path order, as the engine wrote them
    std::vector<int> songs;           // by title
    std::vector<int> recent;          // newest files first, a hundred at most
    std::vector<MusicAlbum> albums;   // by name
    std::vector<MusicArtist> artists; // by name
    std::map<std::string, int> by_path;

    // What lists sort by: lower case, without a leading "The".
    static std::string sort_key(const std::string& value) {
        std::string key;
        key.reserve(value.size());
        for (unsigned char c : value) key.push_back(char(c < 128 ? std::tolower(c) : c));
        if (key.rfind("the ", 0) == 0) key.erase(0, 4);
        return key;
    }
    static std::string stem(const std::string& path) {
        const size_t slash = path.find_last_of('/'), from = slash == std::string::npos ? 0 : slash + 1;
        const size_t dot = path.find_last_of('.');
        return path.substr(from, dot != std::string::npos && dot > from ? dot - from : std::string::npos);
    }
    static std::string parent(const std::string& path) {
        const size_t slash = path.find_last_of('/');
        return slash == std::string::npos ? std::string() : path.substr(0, slash);
    }
    // A file without a title tag goes by its name.
    std::string title(int track) const {
        const music::Track& t = tracks[size_t(track)];
        return t.info.title.empty() ? stem(t.path) : t.info.title;
    }
    // Empty when the file names no artist: the screen says "Unknown artist" in the listener's language.
    const std::string& artist(int track) const { return tracks[size_t(track)].info.artist; }
    // A file without an album tag belongs to its folder: a ripped album without tags still stays together.
    std::string album(int track) const {
        const music::Track& t = tracks[size_t(track)];
        return t.info.album.empty() ? stem(parent(t.path) + ".") : t.info.album;
    }
    std::string album_artist(int track) const {
        const music::TrackInfo& i = tracks[size_t(track)].info;
        return i.album_artist.empty() ? i.artist : i.album_artist;
    }

    void build() {
        by_path.clear();
        songs.clear();
        recent.clear();
        albums.clear();
        artists.clear();
        for (size_t i = 0; i < tracks.size(); i++) by_path[tracks[i].path] = int(i);
        std::vector<std::string> keys(tracks.size());
        for (size_t i = 0; i < tracks.size(); i++) {
            keys[i] = sort_key(title(int(i)));
            songs.push_back(int(i));
            recent.push_back(int(i));
        }
        std::stable_sort(songs.begin(), songs.end(), [&](int a, int b) { return keys[size_t(a)] < keys[size_t(b)]; });
        std::stable_sort(recent.begin(), recent.end(),
                         [&](int a, int b) { return tracks[size_t(a)].mtime > tracks[size_t(b)].mtime; });
        if (recent.size() > 100) recent.resize(100);
        // Albums: one per (album artist, album). A compilation whose tracks name different artists and no album
        // artist is still one album when the files share a folder.
        std::map<std::pair<std::string, std::string>, int> album_at;
        std::map<std::pair<std::string, std::string>, int> by_folder; // (folder, album) to album
        for (size_t i = 0; i < tracks.size(); i++) {
            const music::TrackInfo& info = tracks[i].info;
            const std::string name = album(int(i)), who = album_artist(int(i)), folder = parent(tracks[i].path);
            int index;
            const auto sibling = by_folder.find({folder, name});
            if (sibling != by_folder.end()) {
                index = sibling->second;
                if (albums[size_t(index)].artist != who && info.album_artist.empty())
                    albums[size_t(index)].artist.clear();
            } else {
                const auto found = album_at.find({sort_key(who), sort_key(name)});
                if (found != album_at.end()) index = found->second;
                else {
                    index = int(albums.size());
                    album_at[{sort_key(who), sort_key(name)}] = index;
                    MusicAlbum a;
                    a.name = name;
                    a.artist = who;
                    albums.push_back(std::move(a));
                }
                by_folder[{folder, name}] = index;
            }
            MusicAlbum& a = albums[size_t(index)];
            a.tracks.push_back(int(i));
            a.duration_ms += info.duration_ms;
            if (a.art.empty()) a.art = tracks[i].art;
            if (!a.year) a.year = info.year;
        }
        for (MusicAlbum& a : albums)
            std::stable_sort(a.tracks.begin(), a.tracks.end(), [&](int x, int y) {
                const music::TrackInfo &p = tracks[size_t(x)].info, &q = tracks[size_t(y)].info;
                return std::make_tuple(p.disc, p.track, tracks[size_t(x)].path) <
                       std::make_tuple(q.disc, q.track, tracks[size_t(y)].path);
            });
        std::stable_sort(albums.begin(), albums.end(),
                         [](const MusicAlbum& a, const MusicAlbum& b) { return sort_key(a.name) < sort_key(b.name); });
        // Artists: whoever an album is by; an album by nobody in particular (a compilation) lists under each of
        // its tracks' artists only through Songs.
        std::map<std::string, int> artist_at;
        for (size_t i = 0; i < albums.size(); i++) {
            const std::string key = sort_key(albums[i].artist);
            auto found = artist_at.find(key);
            if (found == artist_at.end()) {
                found = artist_at.emplace(key, int(artists.size())).first;
                MusicArtist artist;
                artist.name = albums[i].artist;
                artists.push_back(std::move(artist));
            }
            MusicArtist& artist = artists[size_t(found->second)];
            artist.albums.push_back(int(i));
            artist.tracks.insert(artist.tracks.end(), albums[i].tracks.begin(), albums[i].tracks.end());
        }
        // Sorting artists moves them: do it through an order, then rebuild.
        std::stable_sort(artists.begin(), artists.end(), [](const MusicArtist& a, const MusicArtist& b) {
            // The nameless one goes last.
            if (a.name.empty() != b.name.empty()) return b.name.empty();
            return sort_key(a.name) < sort_key(b.name);
        });
    }
    bool load(const std::filesystem::path& file) {
        tracks = music::read_library(file);
        build();
        return !tracks.empty();
    }
    // What is directly inside `directory`: its folders first, then its files, each group by name.
    std::vector<MusicFolderEntry> folder(const std::string& directory) const {
        std::vector<MusicFolderEntry> folders, files;
        const std::string prefix = directory + "/";
        for (auto at = by_path.lower_bound(prefix);
             at != by_path.end() && at->first.compare(0, prefix.size(), prefix) == 0; ++at) {
            const size_t slash = at->first.find('/', prefix.size());
            if (slash == std::string::npos) {
                MusicFolderEntry e;
                e.name = at->first.substr(prefix.size());
                e.path = at->first;
                e.track = at->second;
                files.push_back(std::move(e));
            } else {
                const std::string name = at->first.substr(prefix.size(), slash - prefix.size());
                if (folders.empty() || folders.back().name != name) {
                    MusicFolderEntry e;
                    e.folder = true;
                    e.name = name;
                    e.path = prefix + name;
                    folders.push_back(std::move(e));
                }
                folders.back().count++;
            }
        }
        const auto by_name = [](const MusicFolderEntry& a, const MusicFolderEntry& b) {
            return sort_key(a.name) < sort_key(b.name);
        };
        std::stable_sort(folders.begin(), folders.end(), by_name);
        std::stable_sort(files.begin(), files.end(), by_name);
        folders.insert(folders.end(), files.begin(), files.end());
        return folders;
    }
    // Every track under `directory`, folder by folder.
    std::vector<int> under(const std::string& directory) const {
        std::vector<int> found;
        const std::string prefix = directory + "/";
        for (auto at = by_path.lower_bound(prefix);
             at != by_path.end() && at->first.compare(0, prefix.size(), prefix) == 0; ++at)
            found.push_back(at->second);
        return found;
    }
    // Tracks whose title, artist or album holds every word of `query`.
    std::vector<int> search(const std::string& query) const {
        std::vector<std::string> words;
        std::istringstream in(sort_key(query));
        for (std::string word; in >> word;) words.push_back(word);
        std::vector<int> found;
        if (words.empty()) return found;
        for (int track : songs) {
            const std::string text =
                sort_key(title(track)) + "\n" + sort_key(artist(track)) + "\n" + sort_key(album(track));
            bool all = true;
            for (const std::string& word : words) all = all && text.find(word) != std::string::npos;
            if (all) found.push_back(track);
        }
        return found;
    }
};

class MusicClient {
    uint64_t heartbeat_seen = 0, heartbeat_at = 0, started_at = 0, mapped_at = 0;
    bool spawned = false;
    std::vector<std::string> pending; // commands waiting for an engine that is still starting
    std::mt19937 dice{std::random_device{}()};

    void unmap() {
        if (shared) munmap(const_cast<void*>(static_cast<const void*>(shared)), sizeof(music::Shared));
        shared = nullptr;
    }
    bool map() {
        unmap();
        const int fd = ::open((run / music::shared_file).c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return false;
        struct stat st{};
        void* memory = fstat(fd, &st) == 0 && size_t(st.st_size) >= sizeof(music::Shared)
                           ? mmap(nullptr, sizeof(music::Shared), PROT_READ, MAP_SHARED, fd, 0)
                           : MAP_FAILED;
        ::close(fd);
        if (memory == MAP_FAILED) return false;
        shared = static_cast<const music::Shared*>(memory);
        if (shared->magic != music::shared_magic || shared->version != music::shared_version) {
            unmap();
            return false;
        }
        return true;
    }
    bool deliver(const std::string& text) const {
        sockaddr_un name{};
        name.sun_family = AF_UNIX;
        const std::string address = (run / music::control_socket).string();
        if (address.size() >= sizeof name.sun_path) return false;
        std::snprintf(name.sun_path, sizeof name.sun_path, "%s", address.c_str());
        const int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
        if (fd < 0) return false;
        const bool ok = sendto(fd, text.data(), text.size(), 0, reinterpret_cast<sockaddr*>(&name), sizeof name) ==
                        ssize_t(text.size());
        ::close(fd);
        return ok;
    }
    void write_queue() const {
        const std::filesystem::path file = folder / music::queue_file, temporary = file.string() + ".part";
        {
            std::ofstream out(temporary, std::ios::trunc);
            for (size_t i = 0; i < queue.size(); i++) out << queue[i] << '\t' << queue_origin[i] << '\n';
        }
        std::error_code ec;
        std::filesystem::rename(temporary, file, ec);
    }
    void read_queue() {
        queue.clear();
        queue_origin.clear();
        std::ifstream in(folder / music::queue_file);
        std::string line;
        while (std::getline(in, line)) {
            const size_t tab = line.find('\t');
            if (line.empty() || tab == 0) continue;
            queue.push_back(line.substr(0, tab));
            queue_origin.push_back(tab == std::string::npos ? int(queue.size()) - 1
                                                            : std::atoi(line.c_str() + tab + 1));
        }
    }

public:
    std::filesystem::path run, folder, engine;
    std::vector<std::filesystem::path> roots;
    std::filesystem::path bt_sink;
    MusicLibrary library;
    const music::Shared* shared = nullptr;
    music::Now now{};
    bool alive = false;
    // The queue in play order, and where each entry stood before shuffling (so shuffle can be undone).
    std::vector<std::string> queue;
    std::vector<int> queue_origin;
    bool shuffle = false;
    std::set<std::string> favourites;
    int visualiser = 0;
    bool wide = false; // the visualiser takes the whole upper screen
    uint32_t library_seen = 0, queue_seen = 0;
    uint32_t revision = 0; // moves whenever the lists a screen shows may have changed

    ~MusicClient() { unmap(); }

    void configure(const std::filesystem::path& run_folder, const std::filesystem::path& library_folder,
                   const std::vector<std::filesystem::path>& music_roots, const std::filesystem::path& binary) {
        run = run_folder;
        folder = library_folder;
        roots = music_roots;
        engine = binary;
        std::error_code ec;
        std::filesystem::create_directories(run, ec);
        std::filesystem::create_directories(folder, ec);
        for (const auto& root : roots) std::filesystem::create_directories(root, ec);
        std::ifstream in(folder / "player.ini");
        for (std::string line; std::getline(in, line);) {
            if (line == "shuffle=1") shuffle = true;
            else if (line.rfind("visualiser=", 0) == 0) visualiser = std::clamp(std::atoi(line.c_str() + 11), 0, 5);
            else if (line == "wide=1") wide = true;
        }
        std::ifstream loved(folder / "favourites.txt");
        for (std::string line; std::getline(loved, line);)
            if (!line.empty()) favourites.insert(line);
        read_queue();
        library.load(folder / music::library_file);
        revision++;
    }
    void save_settings() const {
        std::ofstream out(folder / "player.ini", std::ios::trunc);
        out << "shuffle=" << shuffle << "\nvisualiser=" << visualiser << "\nwide=" << wide << '\n';
    }

    // Starts the engine unless one answers. It is its own session, a grandchild handed to init: it outlives a
    // launcher restart and never becomes a zombie of ours.
    bool start(uint64_t at) {
        if (alive || engine.empty()) return alive;
        if (spawned && at - started_at < 3000) return false;
        std::vector<std::string> words = {engine.string(), "serve",     "--run",
                                          run.string(),    "--library", folder.string()};
        for (const auto& root : roots) {
            words.push_back("--root");
            words.push_back(root.string());
        }
        if (!bt_sink.empty()) {
            words.push_back("--bt-sink");
            words.push_back(bt_sink.string());
        }
        std::vector<char*> argv;
        for (std::string& word : words) argv.push_back(word.data());
        argv.push_back(nullptr);
        const std::string log = (run / "engine.log").string();
        const long limit = sysconf(_SC_OPEN_MAX);
        const pid_t child = fork();
        if (child < 0) return false;
        if (child == 0) {
            // Only async-signal-safe calls from here: the launcher has other threads.
            setsid();
            if (fork() != 0) _exit(0);
            const int out = ::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644),
                      null = ::open("/dev/null", O_RDONLY);
            if (null >= 0) dup2(null, 0);
            if (out >= 0) {
                dup2(out, 1);
                dup2(out, 2);
            }
            // The panels, the input devices and the menu's own sockets are the launcher's alone.
            for (int fd = 3; fd < int(std::min(limit > 0 ? limit : 1024, 4096L)); fd++) ::close(fd);
            execv(argv[0], argv.data());
            _exit(127);
        }
        int status = 0;
        waitpid(child, &status, 0);
        spawned = true;
        started_at = at;
        return true;
    }

    void send(const std::string& text, uint64_t at) {
        if (alive && deliver(text)) return;
        // Not there (yet): keep the last few and start it. A stale "seek" is of no use to anyone later.
        if (pending.size() < 16) pending.push_back(text);
        start(at);
    }

    // Reads what the engine publishes. True when something a screen shows has changed.
    bool poll(uint64_t at) {
        if (!shared && at - mapped_at >= 200) {
            mapped_at = at;
            map();
        }
        const bool was = alive;
        const music::Now before = now;
        if (shared && music::read_now(*shared, now)) {
            if (now.heartbeat != heartbeat_seen) {
                heartbeat_seen = now.heartbeat;
                heartbeat_at = at;
            }
            alive = now.heartbeat != 0 && at - heartbeat_at < 1500;
        } else alive = false;
        if (!alive) {
            now.status = music::Status::Stopped;
            // An engine that left and came back made a new state file: look again.
            if (was || (spawned && at - mapped_at >= 200)) {
                mapped_at = at;
                map();
            }
        }
        if (alive && !pending.empty()) {
            for (const std::string& text : pending) deliver(text);
            pending.clear();
        }
        bool changed = was != alive || before.status != now.status || before.track_serial != now.track_serial ||
                       before.queue_index != now.queue_index || before.repeat != now.repeat ||
                       before.route != now.route || before.scanning != now.scanning ||
                       before.scan_done != now.scan_done || before.eq_on != now.eq_on ||
                       before.eq_preset != now.eq_preset || before.error_serial != now.error_serial ||
                       before.position_ms / 1000 != now.position_ms / 1000;
        if (alive && now.library_serial != library_seen) {
            library_seen = now.library_serial;
            library.load(folder / music::library_file);
            revision++;
            changed = true;
        }
        return changed;
    }

    bool playing() const { return alive && now.status == music::Status::Playing; }
    bool loaded() const { return alive && now.status != music::Status::Stopped; }
    // The library's track for what plays now; -1 when it is not in the library (any more).
    int current_track() const {
        if (!loaded()) return -1;
        const auto found = library.by_path.find(now.path);
        return found == library.by_path.end() ? -1 : found->second;
    }
    int queue_track(size_t place) const {
        const auto found = place < queue.size() ? library.by_path.find(queue[place]) : library.by_path.end();
        return found == library.by_path.end() ? -1 : found->second;
    }

    // ---- the queue
    // Plays `tracks` from the one at `start`. With shuffle on, that one first and the rest in any order.
    void play(const std::vector<int>& tracks, int start, uint64_t at) {
        if (tracks.empty()) return;
        start = std::clamp(start, 0, int(tracks.size()) - 1);
        queue.clear();
        queue_origin.clear();
        for (size_t i = 0; i < tracks.size(); i++) {
            queue.push_back(library.tracks[size_t(tracks[i])].path);
            queue_origin.push_back(int(i));
        }
        int place = start;
        if (shuffle) place = shuffle_around(start);
        write_queue();
        revision++;
        send("load " + std::to_string(place) + " 0 1", at);
    }
    // Shuffles the queue with the entry at `keep` moved to the front. Returns its new place (0).
    int shuffle_around(int keep) {
        std::vector<size_t> order(queue.size());
        for (size_t i = 0; i < order.size(); i++) order[i] = i;
        std::swap(order[0], order[size_t(keep)]);
        std::shuffle(order.begin() + 1, order.end(), dice);
        std::vector<std::string> paths;
        std::vector<int> origins;
        for (size_t i : order) {
            paths.push_back(queue[i]);
            origins.push_back(queue_origin[i]);
        }
        queue = std::move(paths);
        queue_origin = std::move(origins);
        return 0;
    }
    void set_shuffle(bool on, uint64_t at) {
        if (shuffle == on) return;
        shuffle = on;
        save_settings();
        if (queue.empty()) return;
        int place = std::min<int>(int(now.queue_index), int(queue.size()) - 1);
        if (on) place = shuffle_around(place);
        else {
            const int origin = queue_origin[size_t(place)];
            std::vector<size_t> order(queue.size());
            for (size_t i = 0; i < order.size(); i++) order[i] = i;
            std::stable_sort(order.begin(), order.end(),
                             [&](size_t a, size_t b) { return queue_origin[a] < queue_origin[b]; });
            std::vector<std::string> paths;
            std::vector<int> origins;
            for (size_t i : order) {
                if (queue_origin[i] == origin) place = int(paths.size());
                paths.push_back(queue[i]);
                origins.push_back(queue_origin[i]);
            }
            queue = std::move(paths);
            queue_origin = std::move(origins);
        }
        write_queue();
        revision++;
        if (loaded()) send("requeue " + std::to_string(place), at);
    }
    // Puts `track` right after the playing one (`last` false) or at the end of the queue.
    void add(int track, bool last, uint64_t at) {
        if (queue.empty() || !loaded()) return play({track}, 0, at);
        const size_t current = std::min<size_t>(now.queue_index, queue.size() - 1);
        const size_t place = last ? queue.size() : current + 1;
        // In the unshuffled order it stands after the playing track too, or last.
        const int origin = last ? int(queue.size()) : queue_origin[current] + 1;
        for (int& o : queue_origin)
            if (o >= origin) o++;
        queue.insert(queue.begin() + long(place), library.tracks[size_t(track)].path);
        queue_origin.insert(queue_origin.begin() + long(place), origin);
        write_queue();
        revision++;
        send("requeue " + std::to_string(current), at);
    }
    // Takes an entry out of the queue; the playing one stays.
    bool remove(size_t place, uint64_t at) {
        const size_t current = now.queue_index;
        if (place >= queue.size() || (loaded() && place == current)) return false;
        const int origin = queue_origin[place];
        queue.erase(queue.begin() + long(place));
        queue_origin.erase(queue_origin.begin() + long(place));
        for (int& o : queue_origin)
            if (o > origin) o--;
        write_queue();
        revision++;
        if (loaded()) send("requeue " + std::to_string(place < current ? current - 1 : current), at);
        return true;
    }
    void cycle_repeat(uint64_t at) { send("repeat " + std::to_string((int(now.repeat) + 1) % 3), at); }

    // ---- the equaliser, for the output in use
    std::array<float, music::eq_bands> eq_gains() const {
        std::array<float, music::eq_bands> gains{};
        for (int i = 0; i < music::eq_bands; i++) gains[size_t(i)] = now.eq_gain[i];
        return gains;
    }
    void set_eq(bool on, int preset, const std::array<float, music::eq_bands>& gains, uint64_t at) {
        std::string text = "eq " + std::to_string(int(on)) + " " + std::to_string(preset);
        char number[16];
        for (float gain : gains) {
            std::snprintf(number, sizeof number, " %.1f", double(gain));
            text += number;
        }
        // Shown at once; the engine's own copy follows within a frame.
        now.eq_on = on;
        now.eq_preset = preset;
        for (int i = 0; i < music::eq_bands; i++) now.eq_gain[i] = gains[size_t(i)];
        send(text, at);
    }

    // ---- favourites
    bool loved(const std::string& path) const { return favourites.count(path) > 0; }
    void toggle_favourite(const std::string& path) {
        if (!favourites.erase(path)) favourites.insert(path);
        std::ofstream out(folder / "favourites.txt", std::ios::trunc);
        for (const std::string& entry : favourites) out << entry << '\n';
        revision++;
    }
    std::vector<int> favourite_tracks() const {
        std::vector<int> found;
        for (int track : library.songs)
            if (favourites.count(library.tracks[size_t(track)].path)) found.push_back(track);
        return found;
    }
};
} // namespace rgds
