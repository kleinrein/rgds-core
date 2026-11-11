#pragma once
// A track as the library file keeps it: one line of tab-separated cells. The engine writes the file
// (music/library.hpp), the launcher reads it; this header has no FFmpeg in it so both can include it.
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace rgds::music {
namespace fs = std::filesystem;

struct TrackInfo {
    std::string title, artist, album, album_artist, genre, codec;
    int track = 0, disc = 0, year = 0;
    int64_t duration_ms = 0;
    int sample_rate = 0, channels = 0, bitrate = 0, bits = 0;
};

struct Track {
    std::string path;
    int64_t mtime = 0, size = 0;
    TrackInfo info;
    std::string art; // art/<art>-<size>.bmp; empty for none
};

inline constexpr const char* library_header = "rgds-music-library\t2";

inline std::string one_line(std::string value) {
    for (char& c : value)
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    while (!value.empty() && value.back() == ' ') value.pop_back();
    size_t start = 0;
    while (start < value.size() && value[start] == ' ') start++;
    return value.substr(start);
}

inline std::string track_row(const Track& t) {
    std::ostringstream row;
    const TrackInfo& i = t.info;
    row << t.path << '\t' << t.mtime << '\t' << t.size << '\t' << i.duration_ms << '\t' << one_line(i.title) << '\t'
        << one_line(i.artist) << '\t' << one_line(i.album) << '\t' << one_line(i.album_artist) << '\t'
        << one_line(i.genre) << '\t' << i.track << '\t' << i.disc << '\t' << i.year << '\t' << i.codec << '\t'
        << i.sample_rate << '\t' << i.channels << '\t' << i.bitrate << '\t' << i.bits << '\t' << t.art;
    return row.str();
}

inline bool parse_track(const std::string& line, Track& t) {
    std::vector<std::string> cells;
    size_t from = 0;
    for (;;) {
        const size_t tab = line.find('\t', from);
        cells.push_back(line.substr(from, tab == std::string::npos ? tab : tab - from));
        if (tab == std::string::npos) break;
        from = tab + 1;
    }
    if (cells.size() < 18 || cells[0].empty()) return false;
    const auto number = [&](size_t i) { return std::atoll(cells[i].c_str()); };
    TrackInfo& i = t.info;
    t.path = cells[0];
    t.mtime = number(1);
    t.size = number(2);
    i.duration_ms = number(3);
    i.title = cells[4];
    i.artist = cells[5];
    i.album = cells[6];
    i.album_artist = cells[7];
    i.genre = cells[8];
    i.track = int(number(9));
    i.disc = int(number(10));
    i.year = int(number(11));
    i.codec = cells[12];
    i.sample_rate = int(number(13));
    i.channels = int(number(14));
    i.bitrate = int(number(15));
    i.bits = int(number(16));
    t.art = cells[17];
    return true;
}

inline std::vector<Track> read_library(const fs::path& file) {
    std::vector<Track> tracks;
    std::ifstream in(file);
    std::string line;
    if (!std::getline(in, line) || line != library_header) return tracks;
    Track t;
    while (std::getline(in, line))
        if (parse_track(line, t)) tracks.push_back(t);
    return tracks;
}

inline bool write_library(const fs::path& file, const std::vector<Track>& tracks) {
    const fs::path temporary = file.string() + ".part";
    {
        std::ofstream out(temporary, std::ios::trunc);
        out << library_header << '\n';
        for (const Track& t : tracks) out << track_row(t) << '\n';
        if (!out.flush()) return false;
    }
    std::error_code ec;
    fs::rename(temporary, file, ec);
    return !ec;
}

} // namespace rgds::music
