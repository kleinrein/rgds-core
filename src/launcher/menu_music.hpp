#pragma once
// The menu music track: one IMA ADPCM WAV (mono, 4-bit) decoded once into
// 16-bit PCM for a looping voice in MenuAudio. Independent of the SDL
// version on the device: the decoder is ours, and a malformed file simply
// yields no track. `tools/build_menu_music.py` writes the file.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace rgds {
struct MusicTrack {
    std::vector<int16_t> samples;
    int rate = 0;
    bool empty() const { return samples.empty(); }
};
namespace ima {
inline constexpr int16_t steps[89] = {
    7,    8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,   28,
    31,   34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,  118,
    130,  143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,  494,
    544,  598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878, 2066,
    2272, 2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845, 8630,
    9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
inline constexpr int8_t index_shift[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
inline int16_t decode_nibble(uint8_t nibble, int& predictor, int& index) {
    const int step = steps[index];
    int diff = step >> 3;
    if (nibble & 1) diff += step >> 2;
    if (nibble & 2) diff += step >> 1;
    if (nibble & 4) diff += step;
    predictor += (nibble & 8) ? -diff : diff;
    if (predictor > 32767) predictor = 32767;
    else if (predictor < -32768) predictor = -32768;
    index += index_shift[nibble & 15];
    if (index < 0) index = 0;
    else if (index > 88) index = 88;
    return int16_t(predictor);
}
}
// Parses a RIFF/WAVE file with format 0x11 (IMA ADPCM), one channel. Returns
// an empty track for anything else or for a file above `limit` bytes.
inline MusicTrack decode_ima_adpcm(const std::vector<uint8_t>& file) {
    MusicTrack track;
    auto u16 = [&](size_t at) { return uint32_t(file[at]) | uint32_t(file[at + 1]) << 8; };
    auto u32 = [&](size_t at) { return u16(at) | u16(at + 2) << 16; };
    if (file.size() < 12 || std::string(file.begin(), file.begin() + 4) != "RIFF" ||
        std::string(file.begin() + 8, file.begin() + 12) != "WAVE")
        return track;
    size_t at = 12;
    int format = 0, channels = 0, rate = 0, block = 0, bits = 0, per_block = 0;
    const uint8_t* data = nullptr;
    size_t data_size = 0;
    while (at + 8 <= file.size()) {
        std::string id(file.begin() + long(at), file.begin() + long(at) + 4);
        size_t size = u32(at + 4);
        at += 8;
        if (at + size > file.size()) return track;
        if (id == "fmt " && size >= 16) {
            format = int(u16(at));
            channels = int(u16(at + 2));
            rate = int(u32(at + 4));
            block = int(u16(at + 12));
            bits = int(u16(at + 14));
            if (size >= 20) per_block = int(u16(at + 18));
        } else if (id == "data") {
            data = file.data() + at;
            data_size = size;
        }
        at += size + (size & 1);
    }
    if (format != 0x11 || channels != 1 || bits != 4 || block < 5 || rate < 8000 || rate > 48000 || !data) return track;
    if (!per_block) per_block = (block - 4) * 2 + 1;
    if (per_block > (block - 4) * 2 + 1) return track;
    track.rate = rate;
    track.samples.reserve(data_size / size_t(block) * size_t(per_block));
    for (size_t offset = 0; offset + size_t(block) <= data_size; offset += size_t(block)) {
        const uint8_t* b = data + offset;
        int predictor = int(int16_t(uint16_t(b[0]) | uint16_t(b[1]) << 8)), index = b[2];
        if (index > 88) return {};
        track.samples.push_back(int16_t(predictor));
        int produced = 1;
        for (int i = 4; i < block && produced < per_block; i++) {
            track.samples.push_back(ima::decode_nibble(b[i] & 15, predictor, index));
            if (++produced >= per_block) break;
            track.samples.push_back(ima::decode_nibble(b[i] >> 4, predictor, index));
            ++produced;
        }
    }
    return track;
}
inline MusicTrack load_music(const std::string& path, size_t limit = 6 * 1024 * 1024) {
    std::vector<uint8_t> bytes;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        std::fseek(f, 0, SEEK_END);
        long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (size > 0 && size_t(size) <= limit) {
            bytes.resize(size_t(size));
            if (std::fread(bytes.data(), 1, bytes.size(), f) != bytes.size()) bytes.clear();
        }
        std::fclose(f);
    }
    return bytes.empty() ? MusicTrack{} : decode_ima_adpcm(bytes);
}
}
