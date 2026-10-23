// SPDX-License-Identifier: GPL-3.0-or-later
// Where the player keeps a game's battery save, and how it gets there safely.
//
// Rules (docs/PLAY.md, "Saves"):
//  - Everything lives under the player's own data root (userdata/play); the
//    DS library, its saves, statistics and settings are never touched.
//  - A save on the card is replaced only by an atomic rename of a complete,
//    fsynced file, so power loss or a lid sleep mid-write leaves the old one.
//  - Each session start keeps the previous save in backups/ (three kept).
//  - A save found elsewhere (next to the ROM, stock RetroArch's folder) is
//    copied in once; the original is never changed or moved.
#pragma once
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace play {

bool read_file(const std::string& path, std::vector<uint8_t>& out);
// Write to a temporary file beside `path`, fsync, rename over it, fsync the directory.
bool write_file_atomic(const std::string& path, const std::vector<uint8_t>& bytes, std::string& error);
bool make_directories(const std::string& path);
std::string file_stem(const std::string& path);
std::string parent_directory(const std::string& path);

struct SaveSlot {
    std::string path;           // <root>/saves/<system>/<stem>.sav
    std::string imported_from;  // set when an outside save was copied in
    std::vector<uint8_t> bytes; // what the game starts with
};

// Finds or imports the battery save and rotates the backups.
// `extra_sources` are directories searched for <stem>.sav / .srm (stock RetroArch's saves).
bool prepare_save(const std::string& root, const std::string& system, const std::string& rom_path,
                  const std::vector<std::string>& extra_sources, SaveSlot& out, std::string& error);

// Writes saves on its own thread: an SD card fsync can take 100+ ms, which
// would be a visible hitch on the game thread. Only the newest pending copy
// is written; finish() writes what is left before returning.
class SaveWriter {
public:
    explicit SaveWriter(std::string path);
    ~SaveWriter();
    void submit(std::vector<uint8_t> bytes);
    void finish();
    int written() const;
    std::string last_error() const;

private:
    void run();
    std::string path_;
    mutable std::mutex lock_;
    std::condition_variable wake_, idle_;
    std::thread thread_;
    std::vector<uint8_t> pending_;
    bool has_pending_ = false, writing_ = false, quit_ = false;
    int written_ = 0;
    std::string error_;
};

} // namespace play
