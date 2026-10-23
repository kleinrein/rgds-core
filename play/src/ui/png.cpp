// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/png.hpp"

#include <zlib.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

namespace play {
namespace {

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

int paeth(int a, int b, int c) {
    int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

} // namespace

bool read_png(const std::string& path, std::vector<uint32_t>& pixels, int& width, int& height, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (data.size() < 8 || std::memcmp(data.data(), signature, 8) != 0) {
        error = "not a PNG";
        return false;
    }
    int depth = 0, type = -1, interlace = 0;
    std::vector<uint8_t> compressed, palette, transparency;
    for (size_t at = 8; at + 12 <= data.size();) {
        uint32_t length = be32(&data[at]);
        if (length > data.size() - at - 12) break;
        const uint8_t* chunk = &data[at + 8];
        std::string tag(reinterpret_cast<const char*>(&data[at + 4]), 4);
        if (tag == "IHDR" && length >= 13) {
            width = int(be32(chunk));
            height = int(be32(chunk + 4));
            depth = chunk[8];
            type = chunk[9];
            interlace = chunk[12];
        } else if (tag == "PLTE") palette.assign(chunk, chunk + length);
        else if (tag == "tRNS") transparency.assign(chunk, chunk + length);
        else if (tag == "IDAT") compressed.insert(compressed.end(), chunk, chunk + length);
        else if (tag == "IEND") break;
        at += length + 12;
    }
    const int channels = type == 6 ? 4 : type == 2 ? 3 : type == 4 ? 2 : type == 0 || type == 3 ? 1 : 0;
    if (depth != 8 || !channels || interlace || width <= 0 || height <= 0 || width > 4096 || height > 4096) {
        error = "only 8-bit, non-interlaced PNGs up to 4096 pixels are read";
        return false;
    }
    const size_t stride = size_t(width) * channels;
    std::vector<uint8_t> raw((stride + 1) * height);
    uLongf size = uLongf(raw.size());
    if (uncompress(raw.data(), &size, compressed.data(), uLong(compressed.size())) != Z_OK || size != raw.size()) {
        error = "the picture data is damaged";
        return false;
    }
    // Undo each row's filter in place.
    std::vector<uint8_t> previous(stride, 0);
    for (int y = 0; y < height; ++y) {
        uint8_t* row = &raw[y * (stride + 1) + 1];
        const int filter = row[-1];
        for (size_t i = 0; i < stride; ++i) {
            int left = i >= size_t(channels) ? row[i - channels] : 0, up = previous[i],
                corner = i >= size_t(channels) ? previous[i - channels] : 0;
            int add = filter == 1 ? left : filter == 2 ? up : filter == 3 ? (left + up) / 2 : filter == 4 ? paeth(left, up, corner) : 0;
            row[i] = uint8_t(row[i] + add);
        }
        std::memcpy(previous.data(), row, stride);
    }
    pixels.resize(size_t(width) * height);
    for (int y = 0; y < height; ++y) {
        const uint8_t* row = &raw[y * (stride + 1) + 1];
        for (int x = 0; x < width; ++x) {
            const uint8_t* p = row + size_t(x) * channels;
            uint32_t r, g, b, a = 255;
            if (type == 3) {
                size_t i = p[0];
                if (i * 3 + 2 >= palette.size()) {
                    error = "a pixel uses a colour the palette lacks";
                    return false;
                }
                r = palette[i * 3];
                g = palette[i * 3 + 1];
                b = palette[i * 3 + 2];
                if (i < transparency.size()) a = transparency[i];
            } else if (channels <= 2) {
                r = g = b = p[0];
                if (channels == 2) a = p[1];
            } else {
                r = p[0];
                g = p[1];
                b = p[2];
                if (channels == 4) a = p[3];
            }
            pixels[size_t(y) * width + x] = r | g << 8 | b << 16 | a << 24;
        }
    }
    return true;
}

} // namespace play
