// SPDX-License-Identifier: GPL-3.0-or-later
#include "reader/pages.hpp"

#include "third_party/stb_image.h"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>

namespace play {

namespace {

namespace fs = std::filesystem;

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool picture_name(const std::string& name) {
    const std::string ext = lower(fs::path(name).extension().string());
    return ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".bmp" || ext == ".gif";
}

bool read_range(const std::string& path, uint64_t offset, uint64_t length, std::vector<uint8_t>& out) {
    if (length > (256ull << 20)) return false;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    out.resize(size_t(length));
    bool ok = fseeko(f, off_t(offset), SEEK_SET) == 0 && std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}

bool read_all(const std::string& path, std::vector<uint8_t>& out, uint64_t limit) {
    std::error_code ec;
    const uint64_t size = fs::file_size(path, ec);
    if (ec || size > limit) return false;
    return read_range(path, 0, size, out);
}

// zlib (raw = false) or raw deflate (zip), into at most `limit` bytes.
bool inflate_bytes(const uint8_t* in, size_t n, bool raw, std::vector<uint8_t>& out, size_t limit,
                   size_t expected = 0) {
    z_stream z{};
    if (inflateInit2(&z, raw ? -15 : 15 + 32) != Z_OK) return false;
    out.clear();
    out.reserve(expected ? std::min(expected, limit) : std::min(n * 4, limit));
    z.next_in = const_cast<Bytef*>(in);
    z.avail_in = uInt(n);
    uint8_t chunk[65536];
    int r = Z_OK;
    while (r == Z_OK) {
        z.next_out = chunk;
        z.avail_out = sizeof chunk;
        r = inflate(&z, Z_NO_FLUSH);
        if (r != Z_OK && r != Z_STREAM_END) break;
        out.insert(out.end(), chunk, chunk + (sizeof chunk - z.avail_out));
        if (out.size() > limit) {
            r = Z_MEM_ERROR;
            break;
        }
        if (z.avail_in == 0 && r == Z_OK && z.avail_out != 0) break; // all input used: what came out is all
    }
    inflateEnd(&z);
    return r == Z_STREAM_END || (r == Z_OK && !out.empty());
}

// ---- PDF: just enough of the object syntax to find each page's picture ----

struct Val {
    enum Type { Null, Num, Name, Str, Arr, Dict, Ref, Bool } type = Null;
    double num = 0;
    std::string s; // a name without its slash, or a string's bytes
    std::vector<Val> arr;
    std::vector<std::pair<std::string, Val>> dict;
    int ref = 0;
    const Val* get(const std::string& key) const {
        for (const auto& [k, v] : dict)
            if (k == key) return &v;
        return nullptr;
    }
};

bool delimiter(char c) {
    return std::isspace(static_cast<unsigned char>(c)) || c == '(' || c == ')' || c == '<' || c == '>' || c == '[' ||
           c == ']' || c == '{' || c == '}' || c == '/' || c == '%' || c == 0;
}

struct Lexer {
    const char* p;
    const char* end;
    int depth = 0;
    void space() {
        while (p < end) {
            if (std::isspace(static_cast<unsigned char>(*p)) || *p == 0) ++p;
            else if (*p == '%') {
                while (p < end && *p != '\n' && *p != '\r') ++p;
            } else break;
        }
    }
    bool keyword(const char* word) {
        space();
        size_t n = std::strlen(word);
        if (size_t(end - p) >= n && std::memcmp(p, word, n) == 0 && (p + n == end || delimiter(p[n]))) {
            p += n;
            return true;
        }
        return false;
    }
    bool number(double& out) {
        const char* q = p;
        if (q < end && (*q == '+' || *q == '-')) ++q;
        const char* digits = q;
        while (q < end && (std::isdigit(static_cast<unsigned char>(*q)) || *q == '.')) ++q;
        if (q == digits) return false;
        out = std::strtod(std::string(p, q).c_str(), nullptr);
        p = q;
        return true;
    }
    bool value(Val& v) {
        space();
        if (p >= end || ++depth > 64) return false;
        struct Leave {
            int& d;
            ~Leave() { --d; }
        } leave{depth};
        const char c = *p;
        if (c == '/') {
            const char* q = ++p;
            while (p < end && !delimiter(*p)) ++p;
            v.type = Val::Name;
            v.s.assign(q, p);
            return true;
        }
        if (c == '<' && p + 1 < end && p[1] == '<') {
            p += 2;
            v.type = Val::Dict;
            for (;;) {
                space();
                if (p + 1 < end && p[0] == '>' && p[1] == '>') {
                    p += 2;
                    return true;
                }
                Val key;
                if (!value(key) || key.type != Val::Name) return false;
                Val item;
                if (!value(item)) return false;
                v.dict.emplace_back(key.s, std::move(item));
            }
        }
        if (c == '<') {
            const char* q = static_cast<const char*>(std::memchr(p, '>', size_t(end - p)));
            if (!q) return false;
            v.type = Val::Str;
            p = q + 1;
            return true;
        }
        if (c == '(') {
            int nest = 0;
            v.type = Val::Str;
            for (++p; p < end; ++p) {
                if (*p == '\\') {
                    ++p;
                    continue;
                }
                if (*p == '(') ++nest;
                else if (*p == ')' && nest-- == 0) break;
                v.s.push_back(*p);
            }
            if (p < end) ++p;
            return true;
        }
        if (c == '[') {
            ++p;
            v.type = Val::Arr;
            for (;;) {
                space();
                if (p < end && *p == ']') {
                    ++p;
                    return true;
                }
                Val item;
                if (!value(item)) return false;
                v.arr.push_back(std::move(item));
            }
        }
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '-' || c == '+' || c == '.') {
            if (!number(v.num)) return false;
            v.type = Val::Num;
            // "12 0 R" is a reference.
            const char* back = p;
            double gen = 0;
            space();
            if (number(gen)) {
                space();
                if (p < end && *p == 'R' && (p + 1 == end || delimiter(p[1]))) {
                    ++p;
                    v.type = Val::Ref;
                    v.ref = int(v.num);
                    return true;
                }
            }
            p = back;
            return true;
        }
        if (keyword("true") || keyword("false")) {
            v.type = Val::Bool;
            return true;
        }
        if (keyword("null")) return true;
        return false;
    }
};

struct Object {
    Val val;
    bool stream = false;
    uint64_t data = 0, length = 0;
};

struct Pdf {
    std::map<int, Object> objects;
    std::vector<uint8_t> file;

    const Val* resolve(const Val* v, int hops = 0) const {
        while (v && v->type == Val::Ref && hops++ < 16) {
            auto it = objects.find(v->ref);
            v = it == objects.end() ? nullptr : &it->second.val;
        }
        return v;
    }
    const Object* object(const Val* v) const {
        if (!v || v->type != Val::Ref) return nullptr;
        auto it = objects.find(v->ref);
        return it == objects.end() ? nullptr : &it->second;
    }
    std::string name(const Val* v) const {
        v = resolve(v);
        return v && v->type == Val::Name ? v->s : std::string();
    }
    double num(const Val* v, double fallback = 0) const {
        v = resolve(v);
        return v && v->type == Val::Num ? v->num : fallback;
    }

    // Every "N G obj" in file order, jumping over stream data so a JPEG's bytes are never read as syntax.
    void scan() {
        const char* base = reinterpret_cast<const char*>(file.data());
        const size_t n = file.size();
        size_t at = 0;
        while (at + 3 < n) {
            const void* hit = memmem(base + at, n - at, "obj", 3);
            if (!hit) break;
            const size_t k = size_t(static_cast<const char*>(hit) - base);
            at = k + 3;
            if (k + 3 < n && !delimiter(base[k + 3])) continue;
            // Back over " G " and " N ".
            size_t q = k;
            auto back_space = [&] {
                size_t s = q;
                while (q > 0 && std::isspace(static_cast<unsigned char>(base[q - 1]))) --q;
                return q < s;
            };
            auto back_digits = [&] {
                size_t s = q;
                while (q > 0 && std::isdigit(static_cast<unsigned char>(base[q - 1]))) --q;
                return q < s;
            };
            if (!back_space() || !back_digits()) continue;
            if (!back_space() || !back_digits()) continue;
            if (q > 0 && !delimiter(base[q - 1])) continue;
            const int number = std::atoi(base + q);
            Lexer lex{base + k + 3, base + n};
            Object obj;
            if (!lex.value(obj.val)) continue;
            if (lex.keyword("stream")) {
                const char* d = lex.p;
                if (d < base + n && *d == '\r') ++d;
                if (d < base + n && *d == '\n') ++d;
                obj.stream = true;
                obj.data = uint64_t(d - base);
                const Val* len = obj.val.get("Length");
                uint64_t length = len && len->type == Val::Num && len->num >= 0 ? uint64_t(len->num) : 0;
                auto ends_at = [&](uint64_t l) {
                    const uint64_t e = obj.data + l;
                    if (e > n) return false;
                    const char* s = base + e;
                    const char* stop = base + std::min<uint64_t>(n, e + 32);
                    while (s < stop && std::isspace(static_cast<unsigned char>(*s))) ++s;
                    return size_t(stop - s) >= 9 && std::memcmp(s, "endstream", 9) == 0;
                };
                if (!len || len->type != Val::Num || !ends_at(length)) {
                    const void* e = memmem(base + obj.data, n - obj.data, "endstream", 9);
                    if (!e) break;
                    length = uint64_t(static_cast<const char*>(e) - base) - obj.data;
                    while (length && (base[obj.data + length - 1] == '\n' || base[obj.data + length - 1] == '\r'))
                        --length;
                }
                obj.length = length;
                at = size_t(obj.data + length);
            } else at = size_t(lex.p - base);
            objects[number] = std::move(obj);
        }
    }

    bool stream_bytes(const Object& o, std::vector<uint8_t>& out, size_t limit) const {
        if (!o.stream || o.data + o.length > file.size()) return false;
        const uint8_t* d = file.data() + o.data;
        const std::string filter = filter_of(o.val);
        if (filter.empty()) {
            out.assign(d, d + o.length);
            return true;
        }
        return filter == "FlateDecode" && inflate_bytes(d, size_t(o.length), false, out, limit);
    }

    // The first filter only ("FlateDecode", "DCTDecode"), with the rest in `rest`.
    std::string filter_of(const Val& dict, std::vector<std::string>* rest = nullptr) const {
        const Val* f = resolve(dict.get("Filter"));
        if (!f) return "";
        if (f->type == Val::Name) return f->s;
        if (f->type != Val::Arr || f->arr.empty()) return "";
        if (rest)
            for (size_t i = 1; i < f->arr.size(); ++i) rest->push_back(name(&f->arr[i]));
        return name(&f->arr[0]);
    }

    // Objects packed in object streams (PDF 1.5): never streams themselves, so pages and dictionaries.
    void unpack_object_streams() {
        std::vector<std::pair<int, Val>> found;
        for (const auto& [number, o] : objects) {
            if (!o.stream || name(o.val.get("Type")) != "ObjStm") continue;
            std::vector<uint8_t> bytes;
            if (!stream_bytes(o, bytes, 64u << 20)) continue;
            const int count = int(num(o.val.get("N"))), first = int(num(o.val.get("First")));
            const char* b = reinterpret_cast<const char*>(bytes.data());
            Lexer head{b, b + std::min<size_t>(bytes.size(), size_t(std::max(0, first)))};
            for (int i = 0; i < count; ++i) {
                double id = 0, off = 0;
                head.space();
                if (!head.number(id)) break;
                head.space();
                if (!head.number(off)) break;
                const size_t from = size_t(first) + size_t(off);
                if (from >= bytes.size()) continue;
                Lexer body{b + from, b + bytes.size()};
                Val v;
                if (body.value(v)) found.emplace_back(int(id), std::move(v));
            }
        }
        for (auto& [id, v] : found)
            if (!objects.count(id)) objects[id].val = std::move(v);
    }

    const Val* root() const {
        // The last /Root in the file: the newest trailer (or cross-reference stream).
        const char* base = reinterpret_cast<const char*>(file.data());
        for (size_t k = file.size(); k > 5; --k) {
            if (std::memcmp(base + k - 5, "/Root", 5) != 0) continue;
            Lexer lex{base + k, base + file.size()};
            static thread_local Val v;
            v = Val{};
            if (lex.value(v) && v.type == Val::Ref) return resolve(&v);
            break;
        }
        return nullptr;
    }
};

int components_of(const Pdf& pdf, const Val* cs) {
    cs = pdf.resolve(cs);
    if (!cs) return 0;
    std::string n = cs->type == Val::Name                      ? cs->s
                    : cs->type == Val::Arr && !cs->arr.empty() ? pdf.name(&cs->arr[0])
                                                               : "";
    if (n == "DeviceGray" || n == "CalGray" || n == "G") return 1;
    if (n == "DeviceRGB" || n == "CalRGB" || n == "RGB") return 3;
    if (n == "DeviceCMYK" || n == "CMYK") return 4;
    if (n == "ICCBased" && cs->arr.size() > 1) {
        const Object* o = pdf.object(&cs->arr[1]);
        const Val* d = o ? &o->val : pdf.resolve(&cs->arr[1]);
        return d ? int(pdf.num(d->get("N"))) : 0;
    }
    return 0; // Indexed, Lab, Separation: not read
}

// The page's picture: the largest image it draws (one level into form XObjects, as some scanners wrap them).
void page_picture(const Pdf& pdf, const Val* resources, PageSource& page, int depth = 0) {
    const Val* res = pdf.resolve(resources);
    const Val* xobjects = res ? pdf.resolve(res->get("XObject")) : nullptr;
    if (!xobjects || xobjects->type != Val::Dict) return;
    for (const auto& [key, ref] : xobjects->dict) {
        const Object* o = pdf.object(&ref);
        if (!o || !o->stream) continue;
        const std::string subtype = pdf.name(o->val.get("Subtype"));
        if (subtype == "Form" && depth == 0) {
            page_picture(pdf, o->val.get("Resources"), page, 1);
            continue;
        }
        if (subtype != "Image") continue;
        const int w = int(pdf.num(o->val.get("Width"))), h = int(pdf.num(o->val.get("Height")));
        if (w <= 0 || h <= 0 || int64_t(w) * h <= int64_t(page.width) * page.height) continue;
        std::vector<std::string> rest;
        const std::string filter = pdf.filter_of(o->val, &rest);
        PageSource found;
        found.offset = o->data;
        found.length = o->length;
        found.width = w;
        found.height = h;
        if (filter == "DCTDecode" && rest.empty()) found.kind = PageSource::Encoded;
        else if (filter == "FlateDecode" && rest.size() == 1 && rest[0] == "DCTDecode") {
            found.kind = PageSource::Encoded;
            found.deflated = true;
        } else if ((filter == "FlateDecode" || filter.empty()) && rest.empty()) {
            const int bpc = int(pdf.num(o->val.get("BitsPerComponent"), 8));
            const Val* parms = pdf.resolve(o->val.get("DecodeParms"));
            const bool predicted = parms && parms->type == Val::Dict && pdf.num(parms->get("Predictor"), 1) > 1;
            found.components = components_of(pdf, o->val.get("ColorSpace"));
            if (bpc == 8 && found.components && !predicted) {
                found.kind = PageSource::Pixels;
                found.deflated = !filter.empty();
            }
        }
        // A bigger picture this build cannot read still wins: the page says it is not shown, not a stamp on it.
        page = found;
        if (page.kind == PageSource::Missing) page.width = w, page.height = h;
    }
}

void walk(const Pdf& pdf, const Val* node, const Val* resources, int rotate, std::vector<PageSource>& out,
          int depth = 0) {
    node = pdf.resolve(node);
    if (!node || node->type != Val::Dict || depth > 32 || out.size() > 5000) return;
    if (const Val* r = node->get("Resources")) resources = r;
    if (const Val* r = pdf.resolve(node->get("Rotate")); r && r->type == Val::Num) rotate = int(r->num);
    const Val* kids = pdf.resolve(node->get("Kids"));
    if (pdf.name(node->get("Type")) == "Pages" || (kids && kids->type == Val::Arr)) {
        if (kids && kids->type == Val::Arr)
            for (const auto& kid : kids->arr) walk(pdf, &kid, resources, rotate, out, depth + 1);
        return;
    }
    PageSource page;
    page_picture(pdf, resources, page);
    page.rotate = ((rotate % 360) + 360) % 360 / 90 * 90;
    out.push_back(page);
}

} // namespace

bool natural_less(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (std::isdigit(static_cast<unsigned char>(a[i])) && std::isdigit(static_cast<unsigned char>(b[j]))) {
            size_t ie = i, je = j;
            while (ie < a.size() && std::isdigit(static_cast<unsigned char>(a[ie]))) ++ie;
            while (je < b.size() && std::isdigit(static_cast<unsigned char>(b[je]))) ++je;
            std::string x = a.substr(i, ie - i), y = b.substr(j, je - j);
            x.erase(0, std::min(x.find_first_not_of('0'), x.size()));
            y.erase(0, std::min(y.find_first_not_of('0'), y.size()));
            if (x.size() != y.size()) return x.size() < y.size();
            if (x != y) return x < y;
            i = ie;
            j = je;
            continue;
        }
        const int ca = std::tolower(static_cast<unsigned char>(a[i])),
                  cb = std::tolower(static_cast<unsigned char>(b[j]));
        if (ca != cb) return ca < cb;
        ++i;
        ++j;
    }
    return a.size() - i < b.size() - j;
}

Pages open_pdf(const std::string& path) {
    Pages doc;
    doc.path = path;
    Pdf pdf;
    if (!read_all(path, pdf.file, 512ull << 20)) {
        doc.error = "unreadable";
        return doc;
    }
    if (pdf.file.size() < 8 || std::memcmp(pdf.file.data(), "%PDF", 4) != 0) {
        doc.error = "not a PDF";
        return doc;
    }
    pdf.scan();
    pdf.unpack_object_streams();
    // Encrypted streams would decode to noise: say so instead.
    for (const auto& [n, o] : pdf.objects)
        if (pdf.name(o.val.get("Type")) == "XRef" && o.val.get("Encrypt")) doc.error = "encrypted";
    if (const char* base = reinterpret_cast<const char*>(pdf.file.data());
        const void* trailer = memmem(base, pdf.file.size(), "trailer", 7)) {
        const char* t = static_cast<const char*>(trailer);
        if (memmem(t, pdf.file.size() - size_t(t - base), "/Encrypt", 8)) doc.error = "encrypted";
    }
    if (!doc.error.empty()) return doc;
    if (const Val* root = pdf.root()) walk(pdf, root->get("Pages"), nullptr, 0, doc.pages);
    if (doc.pages.empty()) {
        // No usable page tree: every page object, in number order.
        for (const auto& [n, o] : pdf.objects)
            if (pdf.name(o.val.get("Type")) == "Page") {
                PageSource page;
                page_picture(pdf, o.val.get("Resources"), page);
                doc.pages.push_back(page);
            }
    }
    if (doc.pages.empty()) doc.error = "no pages";
    return doc;
}

Pages open_zip(const std::string& path) {
    Pages doc;
    doc.path = path;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        doc.error = "unreadable";
        return doc;
    }
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> closer(f, std::fclose);
    fseeko(f, 0, SEEK_END);
    const uint64_t size = uint64_t(ftello(f));
    const uint64_t tail = std::min<uint64_t>(size, 65557);
    std::vector<uint8_t> end(tail);
    fseeko(f, off_t(size - tail), SEEK_SET);
    if (std::fread(end.data(), 1, end.size(), f) != end.size()) {
        doc.error = "unreadable";
        return doc;
    }
    auto u16 = [](const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8; };
    auto u32 = [](const uint8_t* p) {
        return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
    };
    long found = -1;
    for (long i = long(tail) - 22; i >= 0; --i)
        if (u32(end.data() + i) == 0x06054b50) {
            found = i;
            break;
        }
    if (found < 0) {
        doc.error = "not a zip";
        return doc;
    }
    const uint8_t* e = end.data() + found;
    const uint32_t entries = u16(e + 10), dir_size = u32(e + 12), dir_at = u32(e + 16);
    if (dir_at == 0xFFFFFFFF || entries == 0xFFFF || uint64_t(dir_at) + dir_size > size) {
        doc.error = "zip64";
        return doc;
    }
    std::vector<uint8_t> dir(dir_size);
    fseeko(f, off_t(dir_at), SEEK_SET);
    if (std::fread(dir.data(), 1, dir.size(), f) != dir.size()) {
        doc.error = "unreadable";
        return doc;
    }
    std::vector<std::pair<std::string, PageSource>> pictures;
    size_t at = 0;
    for (uint32_t i = 0; i < entries && at + 46 <= dir.size(); ++i) {
        const uint8_t* c = dir.data() + at;
        if (u32(c) != 0x02014b50) break;
        const uint32_t flags = u16(c + 8), method = u16(c + 10), packed = u32(c + 20), unpacked = u32(c + 24);
        const uint32_t name_len = u16(c + 28), extra_len = u16(c + 30), comment_len = u16(c + 32), local = u32(c + 42);
        if (at + 46 + name_len > dir.size()) break;
        std::string name(reinterpret_cast<const char*>(c + 46), name_len);
        at += 46 + name_len + extra_len + comment_len;
        const std::string leaf = fs::path(name).filename().string();
        if (name.back() == '/' || leaf.empty() || leaf[0] == '.' || name.rfind("__MACOSX/", 0) == 0) continue;
        if (!picture_name(name) || (flags & 1) || (method != 0 && method != 8)) continue;
        uint8_t header[30];
        if (fseeko(f, off_t(local), SEEK_SET) != 0 || std::fread(header, 1, 30, f) != 30 || u32(header) != 0x04034b50)
            continue;
        PageSource page;
        page.kind = PageSource::Encoded;
        page.offset = uint64_t(local) + 30 + u16(header + 26) + u16(header + 28);
        page.length = packed;
        page.size = unpacked;
        page.deflated = page.raw_deflate = method == 8;
        pictures.emplace_back(name, page);
    }
    std::sort(pictures.begin(), pictures.end(),
              [](const auto& a, const auto& b) { return natural_less(a.first, b.first); });
    for (auto& [name, page] : pictures) doc.pages.push_back(page);
    if (doc.pages.empty()) doc.error = "no pictures";
    return doc;
}

Pages open_folder(const std::string& path) {
    Pages doc;
    doc.path = path;
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(path, ec)) {
        const std::string name = entry.path().filename().string();
        if (!name.empty() && name[0] != '.' && entry.is_regular_file(ec) && picture_name(name)) names.push_back(name);
    }
    std::sort(names.begin(), names.end(), natural_less);
    for (const auto& name : names) {
        PageSource page;
        page.kind = PageSource::Encoded;
        page.file = (fs::path(path) / name).string();
        doc.pages.push_back(page);
    }
    if (doc.pages.empty()) doc.error = "no pictures";
    return doc;
}

Pages open_pages(const std::string& path) {
    std::error_code ec;
    if (fs::is_directory(path, ec)) return open_folder(path);
    const std::string ext = lower(fs::path(path).extension().string());
    if (ext == ".pdf") return open_pdf(path);
    if (ext == ".cbz" || ext == ".zip") return open_zip(path);
    Pages doc;
    doc.path = path;
    if (picture_name(path)) {
        PageSource page;
        page.kind = PageSource::Encoded;
        page.file = path;
        doc.pages.push_back(page);
    } else doc.error = "unknown format";
    return doc;
}

bool read_page(const Pages& doc, size_t index, std::vector<uint32_t>& px, int& w, int& h, std::string& error,
               int max_side) {
    if (index >= doc.pages.size()) {
        error = "no such page";
        return false;
    }
    const PageSource& page = doc.pages[index];
    if (page.kind == PageSource::Missing) {
        error = "not a scan";
        return false;
    }
    std::vector<uint8_t> bytes;
    bool got = page.file.empty() ? read_range(doc.path, page.offset, page.length, bytes)
                                 : read_all(page.file, bytes, 128ull << 20);
    if (!got) {
        error = "unreadable";
        return false;
    }
    if (page.deflated) {
        std::vector<uint8_t> out;
        size_t expected = page.size                         ? size_t(page.size)
                          : page.kind == PageSource::Pixels ? size_t(page.width) * page.height * page.components
                                                            : 0;
        if (!inflate_bytes(bytes.data(), bytes.size(), page.raw_deflate, out, 256u << 20, expected)) {
            error = "damaged";
            return false;
        }
        bytes.swap(out);
    }
    if (page.kind == PageSource::Pixels) {
        const size_t need = size_t(page.width) * page.height * page.components;
        if (bytes.size() < need) {
            error = "damaged";
            return false;
        }
        w = page.width;
        h = page.height;
        px.resize(size_t(w) * h);
        const uint8_t* s = bytes.data();
        for (size_t i = 0; i < px.size(); ++i, s += page.components) {
            uint32_t r, g, b;
            if (page.components == 1) r = g = b = s[0];
            else if (page.components == 3) r = s[0], g = s[1], b = s[2];
            else {
                const uint32_t k = 255u - s[3];
                r = (255u - s[0]) * k / 255u, g = (255u - s[1]) * k / 255u, b = (255u - s[2]) * k / 255u;
            }
            px[i] = r | g << 8 | b << 16 | 0xFF000000u;
        }
    } else {
        int comps = 0;
        if (bytes.size() > 0x7FFFFFFF) {
            error = "too big";
            return false;
        }
        int iw = 0, ih = 0;
        if (!stbi_info_from_memory(bytes.data(), int(bytes.size()), &iw, &ih, &comps) ||
            int64_t(iw) * ih > 80'000'000) {
            error = "not a picture";
            return false;
        }
        stbi_uc* data = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &comps, 4);
        if (!data) {
            error = stbi_failure_reason() ? stbi_failure_reason() : "not a picture";
            return false;
        }
        px.resize(size_t(w) * h);
        std::memcpy(px.data(), data, px.size() * 4);
        stbi_image_free(data);
        for (auto& v : px) v |= 0xFF000000u;
    }
    // Halved (each pixel the mean of four) until it fits.
    while (std::max(w, h) > max_side && w > 1 && h > 1) {
        const int nw = w / 2, nh = h / 2;
        std::vector<uint32_t> half(size_t(nw) * nh);
        for (int y = 0; y < nh; ++y)
            for (int x = 0; x < nw; ++x) {
                const uint32_t* a = px.data() + size_t(2 * y) * w + 2 * x;
                const uint32_t* b = a + w;
                uint32_t out = 0xFF000000u;
                for (int c = 0; c < 24; c += 8)
                    out |=
                        ((((a[0] >> c) & 255) + ((a[1] >> c) & 255) + ((b[0] >> c) & 255) + ((b[1] >> c) & 255) + 2) /
                         4)
                        << c;
                half[size_t(y) * nw + x] = out;
            }
        px.swap(half);
        w = nw;
        h = nh;
    }
    if (page.rotate) {
        std::vector<uint32_t> turned(px.size());
        const int nw = page.rotate == 180 ? w : h, nh = page.rotate == 180 ? h : w;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                int tx, ty;
                if (page.rotate == 90) tx = h - 1 - y, ty = x;
                else if (page.rotate == 180) tx = w - 1 - x, ty = h - 1 - y;
                else tx = y, ty = w - 1 - x;
                turned[size_t(ty) * nw + tx] = px[size_t(y) * w + x];
            }
        px.swap(turned);
        w = nw;
        h = nh;
    }
    return true;
}

} // namespace play
