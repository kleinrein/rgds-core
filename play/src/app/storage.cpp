// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/storage.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace play {

bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    out.clear();
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t buffer[65536];
    size_t n;
    while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0) {
        out.insert(out.end(), buffer, buffer + n);
        if (out.size() > 64u * 1024 * 1024) break;
    }
    bool ok = !std::ferror(f);
    std::fclose(f);
    return ok;
}

std::string parent_directory(const std::string& path) {
    auto slash = path.rfind('/');
    return slash == std::string::npos ? "." : slash == 0 ? "/" : path.substr(0, slash);
}

std::string file_stem(const std::string& path) {
    auto slash = path.rfind('/');
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    auto dot = name.rfind('.');
    return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
}

bool make_directories(const std::string& path) {
    if (path.empty()) return false;
    struct stat st;
    if (stat(path.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
    std::string parent = parent_directory(path);
    if (parent != path && !make_directories(parent)) return false;
    return mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
}

namespace {
bool fsync_directory(const std::string& dir) {
    int fd = open(dir.c_str(), O_RDONLY);
    if (fd < 0) return false;
    // FAT and some FUSE mounts refuse fsync on a directory; the rename stands.
    fsync(fd);
    close(fd);
    return true;
}
bool exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}
} // namespace

bool write_file_atomic(const std::string& path, const std::vector<uint8_t>& bytes, std::string& error) {
    std::string dir = parent_directory(path);
    if (!make_directories(dir)) {
        error = "cannot create " + dir;
        return false;
    }
    std::string tmp = path + ".writing";
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        error = "cannot write " + tmp + ": " + std::strerror(errno);
        return false;
    }
    size_t done = 0;
    while (done < bytes.size()) {
        ssize_t n = write(fd, bytes.data() + done, bytes.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            error = std::string("write failed: ") + std::strerror(errno);
            close(fd);
            unlink(tmp.c_str());
            return false;
        }
        done += static_cast<size_t>(n);
    }
    if (fsync(fd) != 0 || close(fd) != 0) {
        error = std::string("flush failed: ") + std::strerror(errno);
        unlink(tmp.c_str());
        return false;
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        error = std::string("rename failed: ") + std::strerror(errno);
        unlink(tmp.c_str());
        return false;
    }
    fsync_directory(dir);
    return true;
}

bool prepare_save(const std::string& root, const std::string& system, const std::string& rom_path,
                  const std::vector<std::string>& extra_sources, SaveSlot& out, std::string& error) {
    const std::string stem = file_stem(rom_path);
    const std::string dir = root + "/saves/" + system;
    out = SaveSlot{};
    out.path = dir + "/" + stem + ".sav";
    if (!make_directories(dir)) {
        error = "cannot create " + dir;
        return false;
    }
    if (exists(out.path)) {
        if (!read_file(out.path, out.bytes)) {
            // Never start a game over an unreadable save: it would be replaced.
            error = "the save " + out.path + " could not be read";
            return false;
        }
        // Rotate backups: .1 is the newest. Skip when .1 already holds these bytes.
        std::string backups = dir + "/backups";
        make_directories(backups);
        std::string b1 = backups + "/" + stem + ".sav.1";
        std::vector<uint8_t> newest;
        if (!out.bytes.empty() && !(read_file(b1, newest) && newest == out.bytes)) {
            for (int i = 2; i >= 1; --i) {
                std::string from = backups + "/" + stem + ".sav." + std::to_string(i);
                std::string to = backups + "/" + stem + ".sav." + std::to_string(i + 1);
                if (exists(from)) std::rename(from.c_str(), to.c_str());
            }
            std::string why;
            write_file_atomic(b1, out.bytes, why);
        }
        return true;
    }
    std::vector<std::string> candidates;
    std::vector<std::string> dirs = {parent_directory(rom_path)};
    dirs.insert(dirs.end(), extra_sources.begin(), extra_sources.end());
    for (const auto& d : dirs)
        for (const char* ext : {".sav", ".srm"}) candidates.push_back(d + "/" + stem + ext);
    for (const auto& c : candidates) {
        std::vector<uint8_t> bytes;
        if (!exists(c) || !read_file(c, bytes) || bytes.empty()) continue;
        std::string why;
        if (!write_file_atomic(out.path, bytes, why)) {
            error = "could not copy the save from " + c + ": " + why;
            return false;
        }
        out.bytes = std::move(bytes);
        out.imported_from = c;
        return true;
    }
    return true; // a new game: no save yet
}

SaveWriter::SaveWriter(std::string path) : path_(std::move(path)), thread_(&SaveWriter::run, this) {}

SaveWriter::~SaveWriter() {
    finish();
    {
        std::lock_guard<std::mutex> g(lock_);
        quit_ = true;
    }
    wake_.notify_all();
    thread_.join();
}

void SaveWriter::submit(std::vector<uint8_t> bytes) {
    {
        std::lock_guard<std::mutex> g(lock_);
        pending_ = std::move(bytes);
        has_pending_ = true;
    }
    wake_.notify_one();
}

void SaveWriter::finish() {
    std::unique_lock<std::mutex> g(lock_);
    idle_.wait(g, [&] { return !has_pending_ && !writing_; });
}

int SaveWriter::written() const {
    std::lock_guard<std::mutex> g(lock_);
    return written_;
}

std::string SaveWriter::last_error() const {
    std::lock_guard<std::mutex> g(lock_);
    return error_;
}

void SaveWriter::run() {
    std::unique_lock<std::mutex> g(lock_);
    for (;;) {
        wake_.wait(g, [&] { return quit_ || has_pending_; });
        if (!has_pending_ && quit_) return;
        std::vector<uint8_t> bytes;
        bytes.swap(pending_);
        has_pending_ = false;
        writing_ = true;
        g.unlock();
        std::string why;
        bool ok = write_file_atomic(path_, bytes, why);
        g.lock();
        writing_ = false;
        if (ok) ++written_;
        else {
            error_ = why;
            std::fprintf(stderr, "play: save not written: %s\n", why.c_str());
        }
        idle_.notify_all();
    }
}

} // namespace play
