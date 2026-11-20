// SPDX-License-Identifier: GPL-3.0-or-later
// The pages of a scanned manual: a PDF of scans, a comic-book zip (CBZ) or a
// folder of pictures, as one list of pictures. Nothing is converted or copied:
// a PDF page is the JPEG (or deflated pixels) already inside the file, read
// from its place when the page is shown. A PDF page that is not one picture
// (text, drawings) is kept in the list and says so, rather than shown blank.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace play {

struct PageSource {
    enum Kind { Encoded, Pixels, Missing } kind = Missing; // a JPEG/PNG file's bytes; raw pixels to inflate; none
    uint64_t offset = 0, length = 0;                       // where in the file
    bool deflated = false;                                 // Pixels and zip entries: zlib (PDF) or raw deflate (zip)
    bool raw_deflate = false;
    uint64_t size = 0;                         // a zip entry's size once inflated
    int width = 0, height = 0, components = 0; // Pixels
    int rotate = 0;                            // degrees clockwise the page is turned (PDF /Rotate)
    std::string file;                          // a folder's picture: its own file
};

struct Pages {
    std::string path, error;
    std::vector<PageSource> pages;
    bool ok() const { return error.empty() && !pages.empty(); }
};

// Reads only the structure (a PDF's objects, a zip's directory, a folder's names).
Pages open_pages(const std::string& path);
// One page as pixels in the canvas's order (bytes R, G, B, 255), at most max_side on its longer side
// (scaled down by whole steps). False with `error` when it cannot be read.
bool read_page(const Pages& doc, size_t index, std::vector<uint32_t>& px, int& w, int& h, std::string& error,
               int max_side = 2048);

// "page 2" before "page 10".
bool natural_less(const std::string& a, const std::string& b);
// The parts used by open_pages, for tests.
Pages open_pdf(const std::string& path);
Pages open_zip(const std::string& path);
Pages open_folder(const std::string& path);

} // namespace play
