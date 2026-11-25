#pragma once
#include "launcher/model.hpp"
#include "launcher/asset_cache.hpp"
#include "launcher/bmp_budget.hpp"
#include "launcher/panel_scale.hpp"
#include "platform/sdl_dynamic.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
namespace rgds {
inline std::map<std::string, std::string> fields(const fs::path& path) {
    std::map<std::string, std::string> result;
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) {
        auto p = line.find('=');
        if (p != std::string::npos) result[line.substr(0, p)] = line.substr(p + 1);
    }
    return result;
}
inline std::string game_key(const fs::path& path) {
    uint64_t value = 14695981039346656037ull;
    for (unsigned char c : path.string()) {
        value ^= c;
        value *= 1099511628211ull;
    }
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << value;
    return out.str();
}
struct Banner {
    std::vector<unsigned char> pixels;
    std::string title;
};
// Where the game (the SRL) starts in a DS file: 0, or a DSiWare .cia's content after its header, certificates,
// ticket and TMD, each padded to 64 bytes (runtime/dsiware.py srl_span; an encrypted content has no readable icon).
inline uint64_t srl_start(std::ifstream& file) {
    unsigned char head[0x20]{};
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(head), sizeof head)) {
        file.clear();
        return 0;
    }
    auto word = [&](int at) {
        return uint64_t(head[at]) | (uint64_t(head[at + 1]) << 8) | (uint64_t(head[at + 2]) << 16) |
               (uint64_t(head[at + 3]) << 24);
    };
    if (word(0) != 0x2020) return 0;
    auto align = [](uint64_t x) { return (x + 0x3f) & ~uint64_t(0x3f); };
    return align(align(align(align(word(0)) + word(8)) + word(12)) + word(16));
}
inline Banner read_banner(const fs::path& path) {
    Banner result;
    if (is_archive(path)) return result;
    std::ifstream file(path, std::ios::binary);
    if (!file) return result;
    file.seekg(0, std::ios::end);
    auto length = file.tellg();
    const uint64_t base = srl_start(file);
    if (uint64_t(length) < base + 0x6c) return result;
    file.seekg(std::streamoff(base + 0x68));
    unsigned char offset[4]{};
    if (!file.read(reinterpret_cast<char*>(offset), 4)) return result;
    uint32_t begin = offset[0] | (uint32_t(offset[1]) << 8) | (uint32_t(offset[2]) << 16) | (uint32_t(offset[3]) << 24);
    if (begin < 0x200 || base + begin + 0x440 > static_cast<uint64_t>(length)) return result;
    std::array<unsigned char, 0x440> bytes{};
    file.seekg(std::streamoff(base + begin));
    if (!file.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) return result;
    result.pixels.resize(32 * 32 * 4);
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++) {
            int tile = (y / 8) * 4 + x / 8, index = bytes[0x20 + tile * 32 + (y % 8) * 4 + (x % 8) / 2];
            index = (x % 2) ? index >> 4 : index & 15;
            unsigned rgb = bytes[0x220 + index * 2] | (unsigned(bytes[0x221 + index * 2]) << 8);
            auto* p = &result.pixels[(y * 32 + x) * 4];
            p[0] = (rgb & 31) * 255 / 31;
            p[1] = ((rgb >> 5) & 31) * 255 / 31;
            p[2] = ((rgb >> 10) & 31) * 255 / 31;
            p[3] = index ? 255 : 0;
        }
    for (int i = 0x340; i < 0x440; i += 2) {
        unsigned cp = bytes[i] | (unsigned(bytes[i + 1]) << 8);
        if (!cp) break;
        if (cp == '\n') result.title += ' ';
        else if (cp < 128) result.title += static_cast<char>(cp);
        else if (cp < 2048) {
            result.title += static_cast<char>(0xc0 | (cp >> 6));
            result.title += static_cast<char>(0x80 | (cp & 63));
        } else if (cp < 0xd800 || cp > 0xdfff) {
            result.title += static_cast<char>(0xe0 | (cp >> 12));
            result.title += static_cast<char>(0x80 | ((cp >> 6) & 63));
            result.title += static_cast<char>(0x80 | (cp & 63));
        }
    }
    return result;
}
class Media {
    Sdl& s;
    struct CachedBanner {
        Banner value;
        AssetStamp stamp;
        uint64_t used = 0;
    };
    std::map<std::string, CachedBanner> banners;
    struct Image {
        SDL_Texture* texture = nullptr;
        int w = 0, h = 0;
        fs::path path;
        bool icon = false;
        AssetStamp stamp;
        uint64_t used = 0, bytes = 0;
        // Small pictures (icons, up to 256 px) keep their straight RGBA pixels: drawn at well under their size they
        // are averaged down once to the exact panel size (reduced), not sampled by bilinear filtering, which skips
        // most of a 128 px icon at 40 px and looks soft and uneven (2026-11-25, the RG DS Plus's dock icons).
        std::vector<unsigned char> rgba;
        std::map<std::pair<int, int>, SDL_Texture*> reduced;
    };
    std::map<std::pair<SDL_Renderer*, std::string>, Image> images;
    uint64_t clock = 0, texture_bytes = 0;
    // Cached misses are cheap entries; the byte budget bounds the textures.
    // A library zoom wall looks up a few hundred small entries (thumbnails and misses) at once.
    static constexpr size_t image_limit = 512, banner_limit = 256;
    static constexpr int thumb_side = 112; // a wall thumbnail's longer side: ~50 KB instead of a full picture
    static constexpr uint64_t byte_limit = 16 * 1024 * 1024;
    AssetStamp stamp(const fs::path& path) {
        counters.stamp_checks++;
        return asset_stamp(path);
    }
    void discard(decltype(images)::iterator entry) {
        if (entry->second.texture) s.DestroyTexture(entry->second.texture);
        for (auto& [size, texture] : entry->second.reduced) s.DestroyTexture(texture);
        texture_bytes -= entry->second.bytes;
        images.erase(entry);
    }
    void trim(size_t count, uint64_t bytes = 0) {
        while (!images.empty() && (images.size() > count || texture_bytes + bytes > byte_limit)) {
            auto oldest = std::min_element(images.begin(), images.end(),
                                           [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
            discard(oldest);
            counters.evictions++;
        }
    }
    void remember_banner(const std::string& key, Banner value, const AssetStamp& version) {
        if (banners.count(key)) return;
        if (banners.size() >= banner_limit) {
            auto oldest = std::min_element(banners.begin(), banners.end(),
                                           [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
            banners.erase(oldest);
            counters.evictions++;
        }
        CachedBanner entry;
        entry.stamp = version;
        entry.value = std::move(value);
        entry.used = ++clock;
        banners.emplace(key, std::move(entry));
    }
    // A decoded surface becomes this renderer's texture (or a cached miss).
    Image adopt_surface(SDL_Renderer* r, const fs::path& path, bool icon, const AssetStamp& version,
                        SDL_Surface* surface) {
        Image image;
        image.path = path;
        image.icon = icon;
        image.used = ++clock;
        image.stamp = version;
        if (surface) {
            uint64_t bytes = surface->w > 0 && surface->h > 0 ? uint64_t(surface->w) * surface->h * 4 : 0;
            if (bytes && bytes <= byte_limit) {
                trim(image_limit - 1, bytes);
                image.w = surface->w;
                image.h = surface->h;
                s.SetHint("SDL_RENDER_SCALE_QUALITY", icon || path.filename() == "icon.bmp" ? "0" : "1");
                // 24-bit pictures (Album thumbnails and views, the Files peek: runtime/screenshots.py writes them)
                // became blank textures on the handheld's GLES renderer (2026-11-24) while the 32-bit covers drew:
                // they go up as RGBA32, opaque, like every other picture.
                SDL_Surface* rgba = surface->format->BytesPerPixel == 3 ? opaque_rgba(s, surface) : nullptr;
                image.texture = s.CreateTextureFromSurface(r, rgba ? rgba : surface);
                if (image.texture && !icon && surface->w <= 256 && surface->h <= 256 && s.ConvertSurfaceFormat) {
                    if (SDL_Surface* copy = s.ConvertSurfaceFormat(rgba ? rgba : surface, SDL_PIXELFORMAT_RGBA32, 0)) {
                        image.rgba.resize(size_t(copy->w) * copy->h * 4);
                        for (int y = 0; y < copy->h; y++)
                            std::memcpy(image.rgba.data() + size_t(y) * copy->w * 4,
                                        static_cast<unsigned char*>(copy->pixels) + size_t(y) * copy->pitch,
                                        size_t(copy->w) * 4);
                        s.FreeSurface(copy);
                    }
                }
                if (rgba) s.FreeSurface(rgba);
                if (image.texture) {
                    image.bytes = bytes;
                    texture_bytes += bytes;
                    s.SetTextureBlendMode(image.texture, SDL_BLENDMODE_BLEND);
                }
            }
        }
        trim(image_limit - 1);
        return image;
    }
    static SDL_Surface* opaque_rgba(Sdl& s, SDL_Surface* source) {
        SDL_Surface* out = s.CreateRGBSurfaceWithFormat(0, source->w, source->h, 32, SDL_PIXELFORMAT_RGBA32);
        if (!out) return nullptr;
        const SDL_PixelFormat* f = source->format;
        for (int y = 0; y < source->h; y++) {
            const auto* from = static_cast<const unsigned char*>(source->pixels) + y * source->pitch;
            auto* to = static_cast<unsigned char*>(out->pixels) + y * out->pitch;
            for (int x = 0; x < source->w; x++, from += 3, to += 4) {
                const uint32_t v = uint32_t(from[0] | from[1] << 8 | from[2] << 16);
                to[0] = Uint8((v & f->Rmask) >> f->Rshift);
                to[1] = Uint8((v & f->Gmask) >> f->Gshift);
                to[2] = Uint8((v & f->Bmask) >> f->Bshift);
                to[3] = 255;
            }
        }
        return out;
    }
    static std::string image_key(const fs::path& path, bool icon) { return (icon ? "icon:" : "art:") + path.string(); }
    static std::string thumb_key(const fs::path& path) { return "thumb:" + path.string(); }
    static std::string job_key(const fs::path& path, bool icon, bool thumb) {
        return thumb ? thumb_key(path) : image_key(path, icon);
    }
    // The picture shrunk to fit thumb_side, averaged over each target pixel's block of source pixels. Any BMP the
    // loader reads (1-4 bytes a pixel, palettes, masks) comes out RGBA32. Runs on the loader thread.
    static SDL_Surface* shrink(Sdl& s, SDL_Surface* source) {
        if (!source || source->w <= 0 || source->h <= 0) return nullptr;
        const double scale = std::min(1.0, double(thumb_side) / std::max(source->w, source->h));
        const int w = std::max(1, int(std::lround(source->w * scale))),
                  h = std::max(1, int(std::lround(source->h * scale)));
        SDL_Surface* out = s.CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
        if (!out) return nullptr;
        const SDL_PixelFormat* f = source->format;
        const int bpp = f->BytesPerPixel;
        auto pixel = [&](int x, int y, unsigned& r, unsigned& g, unsigned& b, unsigned& a) {
            const auto* at = static_cast<const unsigned char*>(source->pixels) + y * source->pitch + x * bpp;
            uint32_t v = bpp == 1   ? at[0]
                         : bpp == 2 ? uint32_t(at[0] | at[1] << 8)
                         : bpp == 3 ? uint32_t(at[0] | at[1] << 8 | at[2] << 16)
                                    : uint32_t(at[0] | at[1] << 8 | at[2] << 16 | uint32_t(at[3]) << 24);
            if (bpp == 1 && f->palette && int(v) < f->palette->ncolors) {
                const SDL_Color c = f->palette->colors[v];
                r = c.r, g = c.g, b = c.b, a = 255;
                return;
            }
            auto channel = [&](uint32_t mask, uint8_t shift, uint8_t loss) {
                return mask ? (((v & mask) >> shift) << loss) & 255u : 255u;
            };
            r = channel(f->Rmask, f->Rshift, f->Rloss);
            g = channel(f->Gmask, f->Gshift, f->Gloss);
            b = channel(f->Bmask, f->Bshift, f->Bloss);
            a = f->Amask ? channel(f->Amask, f->Ashift, f->Aloss) : 255u;
        };
        for (int y = 0; y < h; y++) {
            const int y0 = y * source->h / h, y1 = std::max(y0 + 1, (y + 1) * source->h / h);
            auto* row = static_cast<unsigned char*>(out->pixels) + y * out->pitch;
            for (int x = 0; x < w; x++) {
                const int x0 = x * source->w / w, x1 = std::max(x0 + 1, (x + 1) * source->w / w);
                // Every second row and column of a big block is enough for a 112-pixel picture.
                const int step = (x1 - x0) > 4 ? 2 : 1;
                unsigned sum[4] = {0, 0, 0, 0}, count = 0;
                for (int sy = y0; sy < y1; sy += step)
                    for (int sx = x0; sx < x1; sx += step) {
                        unsigned r, g, b, a;
                        pixel(sx, sy, r, g, b, a);
                        sum[0] += r, sum[1] += g, sum[2] += b, sum[3] += a, count++;
                    }
                for (int c = 0; c < 4; c++) row[x * 4 + c] = static_cast<unsigned char>(sum[c] / std::max(1u, count));
            }
        }
        return out;
    }
    // ---- background loading: one worker reads and decodes; textures are
    // created on the main thread when adopt() runs. ----
    struct Job {
        fs::path path;
        bool icon = false, thumb = false;
        std::vector<SDL_Renderer*> renderers;
        AssetStamp stamp;
        SDL_Surface* surface = nullptr;
        Banner banner;
    };
    std::thread loader;
    mutable std::mutex guard;
    std::condition_variable wake;
    std::deque<Job> queue;
    std::vector<Job> finished;
    std::set<std::string> queued;
    bool loader_stop = false;
    void load_job(Job& job) {
        job.stamp = asset_stamp(job.path);
        if (!job.stamp.present) return;
        if (job.icon) {
            if (!is_archive(job.path)) job.banner = read_banner(job.path);
            if (!job.banner.pixels.empty()) {
                job.surface = s.CreateRGBSurfaceWithFormat(0, 32, 32, 32, SDL_PIXELFORMAT_RGBA32);
                if (job.surface)
                    for (int y = 0; y < 32; y++)
                        std::copy_n(job.banner.pixels.data() + y * 32 * 4, 32 * 4,
                                    static_cast<unsigned char*>(job.surface->pixels) + y * job.surface->pitch);
            }
        } else if (job.stamp.size <= 32 * 1024 * 1024) {
            auto* file = s.RWFromFile(job.path.c_str(), "rb");
            if (file) {
                if (bmp_within_budget(file, byte_limit)) job.surface = s.LoadBMP_RW(file, 1);
                else file->close(file);
            }
            if (job.thumb && job.surface) {
                SDL_Surface* small = shrink(s, job.surface);
                s.FreeSurface(job.surface);
                job.surface = small;
            }
        }
    }
    void loader_main() {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(guard);
                wake.wait(lock, [&] { return loader_stop || !queue.empty(); });
                if (loader_stop) return;
                job = std::move(queue.front());
                queue.pop_front();
            }
            load_job(job);
            {
                std::lock_guard<std::mutex> lock(guard);
                finished.push_back(std::move(job));
            }
            SDL_Event event{};
            event.type = SDL_USEREVENT;
            event.user.code = 1;
            s.PushEvent(&event); // wakes an idle menu for adopt()
        }
    }

public:
    CacheCounters counters;
    uint64_t banner_reads = 0, image_reads = 0, async_loads = 0;
    Media(Sdl& api) : s(api) {}
    ~Media() {
        stop();
        clear();
    }
    void clear() {
        for (auto& entry : images) {
            if (entry.second.texture) s.DestroyTexture(entry.second.texture);
            for (auto& [size, texture] : entry.second.reduced) s.DestroyTexture(texture);
        }
        images.clear();
        banners.clear();
        texture_bytes = 0;
    }
    size_t image_count() const { return images.size(); }
    size_t banner_count() const { return banners.size(); }
    uint64_t bytes() const { return texture_bytes; }
    bool prepared(SDL_Renderer* r, const fs::path& path) const {
        return images.count(std::make_pair(r, "art:" + path.string())) != 0;
    }
    bool ready(SDL_Renderer* r, const fs::path& path, bool icon = false) const {
        auto found = images.find(std::make_pair(r, image_key(path, icon)));
        return found != images.end() && found->second.texture;
    }
    // Requested but not adopted yet: draw a quiet placeholder, not "no cover".
    bool pending(const fs::path& path, bool icon = false) const {
        std::lock_guard<std::mutex> lock(guard);
        return queued.count(image_key(path, icon)) != 0;
    }
    bool loading() const {
        std::lock_guard<std::mutex> lock(guard);
        return !queue.empty() || !finished.empty();
    }
    Banner& banner(const fs::path& path) {
        counters.lookups++;
        auto key = path.string();
        auto found = banners.find(key);
        if (found == banners.end()) {
            if (banners.size() >= banner_limit) {
                auto oldest = std::min_element(banners.begin(), banners.end(), [](const auto& a, const auto& b) {
                    return a.second.used < b.second.used;
                });
                banners.erase(oldest);
                counters.evictions++;
            }
            CachedBanner value;
            value.stamp = stamp(path);
            if (value.stamp.present && !is_archive(path)) {
                banner_reads++;
                counters.file_reads++;
                value.value = read_banner(path);
            }
            found = banners.emplace(std::move(key), std::move(value)).first;
        } else counters.hits++;
        found->second.used = ++clock;
        return found->second.value;
    }
    // Loading happens before rendering. Missing/invalid files are cached too;
    // refresh() allows newly created or replaced artwork to become visible.
    bool warm(SDL_Renderer* r, const fs::path& path, bool icon = false) {
        counters.lookups++;
        auto key = std::make_pair(r, image_key(path, icon));
        auto found = images.find(key);
        if (found != images.end()) {
            counters.hits++;
            found->second.used = ++clock;
            return found->second.texture != nullptr;
        }
        AssetStamp version = stamp(path);
        SDL_Surface* surface = nullptr;
        if (version.present) {
            if (icon) {
                auto& b = banner(path);
                if (!b.pixels.empty()) {
                    surface = s.CreateRGBSurfaceWithFormat(0, 32, 32, 32, SDL_PIXELFORMAT_RGBA32);
                    if (surface)
                        for (int y = 0; y < 32; y++)
                            std::copy_n(b.pixels.data() + y * 32 * 4, 32 * 4,
                                        static_cast<unsigned char*>(surface->pixels) + y * surface->pitch);
                }
            } else if (version.size <= 32 * 1024 * 1024) {
                image_reads++;
                counters.file_reads++;
                auto* file = s.RWFromFile(path.c_str(), "rb");
                if (file) {
                    if (bmp_within_budget(file, byte_limit)) surface = s.LoadBMP_RW(file, 1);
                    else file->close(file);
                }
            }
        }
        Image image = adopt_surface(r, path, icon, version, surface);
        if (surface) s.FreeSurface(surface);
        bool ready = image.texture != nullptr;
        images.emplace(std::move(key), std::move(image));
        return ready;
    }
    // Queue a load for the worker; nothing happens for a path already cached
    // or already on its way. Several renderers can share one decode.
    void request(SDL_Renderer* r, const fs::path& path, bool icon = false, bool urgent = false) {
        request_job(r, path, icon, false, urgent);
    }
    // Loaded or known missing: nothing more to wait for.
    bool known(SDL_Renderer* r, const fs::path& path, bool icon = false) const {
        return images.count(std::make_pair(r, image_key(path, icon))) != 0;
    }
    // A picture shrunk for a zoomed-out library wall (thumb_side), kept beside the full one.
    void request_thumb(SDL_Renderer* r, const fs::path& path) { request_job(r, path, false, true, false); }
    bool thumb_ready(SDL_Renderer* r, const fs::path& path) const {
        auto found = images.find(std::make_pair(r, thumb_key(path)));
        return found != images.end() && found->second.texture;
    }
    // Known one way or the other: drawable, or a cached miss (no such file).
    bool thumb_known(SDL_Renderer* r, const fs::path& path) const {
        return images.count(std::make_pair(r, thumb_key(path))) != 0;
    }
    bool draw_thumb(SDL_Renderer* r, const fs::path& path, SDL_Rect box, int radius = 0) {
        return draw_key(r, thumb_key(path), box, 255, false, radius);
    }
    void request_job(SDL_Renderer* r, const fs::path& path, bool icon, bool thumb, bool urgent) {
        auto key = job_key(path, icon, thumb);
        if (images.count(std::make_pair(r, key))) return;
        std::lock_guard<std::mutex> lock(guard);
        if (!queued.insert(key).second) {
            auto attach = [&](auto& jobs) {
                for (auto& job : jobs)
                    if (job.icon == icon && job.thumb == thumb && job.path == path) {
                        if (std::find(job.renderers.begin(), job.renderers.end(), r) == job.renderers.end())
                            job.renderers.push_back(r);
                        return true;
                    }
                return false;
            };
            if (!attach(finished) && attach(queue) && urgent) {
                auto at = std::find_if(queue.begin(), queue.end(), [&](const Job& job) {
                    return job.icon == icon && job.thumb == thumb && job.path == path;
                });
                std::rotate(queue.begin(), at, at + 1);
            }
            return;
        }
        Job job;
        job.path = path;
        job.icon = icon;
        job.thumb = thumb;
        job.renderers = {r};
        if (urgent) queue.push_front(std::move(job));
        else queue.push_back(std::move(job));
        async_loads++;
        if (!loader.joinable()) loader = std::thread([this] { loader_main(); });
        wake.notify_one();
    }
    // Main thread: turn finished loads into textures. True when anything new is drawable.
    bool adopt() {
        std::vector<Job> ready;
        {
            std::lock_guard<std::mutex> lock(guard);
            if (finished.empty()) return false;
            ready.swap(finished);
            for (auto& job : ready) queued.erase(job_key(job.path, job.icon, job.thumb));
        }
        bool changed = false;
        for (auto& job : ready) {
            if (job.icon && job.stamp.present) remember_banner(job.path.string(), job.banner, job.stamp);
            for (auto* r : job.renderers) {
                auto key = std::make_pair(r, job_key(job.path, job.icon, job.thumb));
                if (images.count(key)) continue;
                images.emplace(std::move(key), adopt_surface(r, job.path, job.icon, job.stamp, job.surface));
                changed = true;
            }
            if (job.surface) {
                s.FreeSurface(job.surface);
                job.surface = nullptr;
            }
        }
        return changed;
    }
    // Before the renderers go: finish the worker and drop undecoded results.
    void stop() {
        {
            std::lock_guard<std::mutex> lock(guard);
            loader_stop = true;
        }
        wake.notify_all();
        if (loader.joinable()) loader.join();
        std::lock_guard<std::mutex> lock(guard);
        for (auto& job : finished)
            if (job.surface) s.FreeSurface(job.surface);
        finished.clear();
        queue.clear();
        queued.clear();
        loader_stop = false;
    }
    bool refresh() {
        bool changed = false;
        std::map<std::string, AssetStamp> versions;
        auto version = [&](const fs::path& path) {
            auto key = path.string();
            auto found = versions.find(key);
            if (found == versions.end()) found = versions.emplace(key, stamp(path)).first;
            return found->second;
        };
        for (auto it = images.begin(); it != images.end();) {
            if (version(it->second.path) != it->second.stamp) {
                auto old = it++;
                discard(old);
                changed = true;
                counters.invalidations++;
            } else ++it;
        }
        for (auto it = banners.begin(); it != banners.end();) {
            if (version(it->first) != it->second.stamp) {
                it = banners.erase(it);
                changed = true;
                counters.invalidations++;
            } else ++it;
        }
        return changed;
    }
    SDL_Rect fitted_box(SDL_Renderer* r, const fs::path& path, SDL_Rect box) const {
        auto found = images.find(std::make_pair(r, "art:" + path.string()));
        if (found == images.end() || !found->second.texture) return box;
        const auto& image = found->second;
        double scale = std::min(double(box.w) / image.w, double(box.h) / image.h);
        int width = std::max(1, int(std::lround(image.w * scale))),
            height = std::max(1, int(std::lround(image.h * scale)));
        return {box.x + (box.w - width) / 2, box.y + (box.h - height) / 2, width, height};
    }
    // A small picture drawn at under two thirds of its size: averaged down to the panel pixels it covers, once per
    // size, and copied onto whole panel pixels.
    bool draw_reduced(SDL_Renderer* r, Image& image, const SDL_Rect& destination, unsigned char alpha) {
        if (image.rgba.empty() || !s.RenderCopyF || !panel_part("reduced")) return false;
        const float k = panel_scale(s, r);
        const int x0 = int(std::lround(destination.x * k)), y0 = int(std::lround(destination.y * k));
        const int pw = int(std::lround((destination.x + destination.w) * k)) - x0,
                  ph = int(std::lround((destination.y + destination.h) * k)) - y0;
        if (pw < 1 || ph < 1 || pw * 3 > image.w * 2 || ph * 3 > image.h * 2) return false;
        SDL_Texture*& texture = image.reduced[{pw, ph}];
        if (!texture) {
            std::vector<unsigned char> out(size_t(pw) * ph * 4);
            const double sx = double(image.w) / pw, sy = double(image.h) / ph;
            for (int oy = 0; oy < ph; oy++)
                for (int ox = 0; ox < pw; ox++) {
                    // Every source pixel the output pixel covers, weighted by how much of it it covers, in
                    // premultiplied alpha so transparent edges do not darken.
                    const double left = ox * sx, right = left + sx, top = oy * sy, bottom = top + sy;
                    double acc[4] = {0, 0, 0, 0}, area = 0;
                    for (int y = int(top); y < std::min(image.h, int(std::ceil(bottom))); y++) {
                        const double wy = std::min(bottom, y + 1.0) - std::max(top, double(y));
                        for (int x = int(left); x < std::min(image.w, int(std::ceil(right))); x++) {
                            const double w = wy * (std::min(right, x + 1.0) - std::max(left, double(x)));
                            const unsigned char* p = image.rgba.data() + (size_t(y) * image.w + x) * 4;
                            const double a = p[3] / 255.0;
                            acc[0] += w * p[0] * a;
                            acc[1] += w * p[1] * a;
                            acc[2] += w * p[2] * a;
                            acc[3] += w * a;
                            area += w;
                        }
                    }
                    unsigned char* q = out.data() + (size_t(oy) * pw + ox) * 4;
                    const double a = area > 0 ? acc[3] / area : 0;
                    for (int c = 0; c < 3; c++) q[c] = Uint8(a > 0 ? std::clamp(acc[c] / area / a, 0.0, 255.0) + .5 : 0);
                    q[3] = Uint8(std::clamp(a * 255 + .5, 0.0, 255.0));
                }
            texture = s.CreateTexture(r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, pw, ph);
            if (!texture || s.UpdateTexture(texture, nullptr, out.data(), pw * 4) != 0) {
                if (texture) s.DestroyTexture(texture);
                texture = nullptr;
                image.reduced.erase({pw, ph});
                return false;
            }
            s.SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        }
        s.SetTextureAlphaMod(texture, alpha);
        const SDL_FRect to{x0 / k, y0 / k, pw / k, ph / k};
        return s.RenderCopyF(r, texture, nullptr, &to) == 0;
    }
    bool draw(SDL_Renderer* r, const fs::path& path, SDL_Rect box, bool icon = false, unsigned char alpha = 255,
              bool fill = false, int radius = 0) {
        return draw_key(r, image_key(path, icon), box, alpha, fill, radius);
    }
    bool draw_key(SDL_Renderer* r, const std::string& key, SDL_Rect box, unsigned char alpha = 255, bool fill = false,
                  int radius = 0) {
        counters.lookups++;
        auto found = images.find(std::make_pair(r, key));
        if (found == images.end()) return false;
        counters.hits++;
        auto& image = found->second;
        image.used = ++clock;
        if (!image.texture) return false;
        SDL_Rect source{0, 0, image.w, image.h}, destination = box;
        if (fill) {
            // Center-crop to fill the frame without stretching or letterboxing.
            double scale = std::max(double(box.w) / image.w, double(box.h) / image.h);
            int width = std::clamp(int(std::lround(box.w / scale)), 1, image.w),
                height = std::clamp(int(std::lround(box.h / scale)), 1, image.h);
            source = {(image.w - width) / 2, (image.h - height) / 2, width, height};
        } else {
            double scale = std::min(double(box.w) / image.w, double(box.h) / image.h);
            int width = std::max(1, int(std::lround(image.w * scale))),
                height = std::max(1, int(std::lround(image.h * scale)));
            destination = {box.x + (box.w - width) / 2, box.y + (box.h - height) / 2, width, height};
        }
        s.SetTextureAlphaMod(image.texture, alpha);
        radius = std::clamp(radius, 0, std::min(destination.w, destination.h) / 2);
        if (!radius && !fill && draw_reduced(r, image, destination, alpha)) return true;
        if (!radius) {
            s.RenderCopy(r, image.texture, &source, &destination);
            return true;
        }
        // Draw rounded strips inside the parent's clip. The software renderer
        // can discard one-pixel scissor regions after logical-size scaling.
        auto strip = [&](SDL_Rect clip) {
            if (clip.w <= 0 || clip.h <= 0) return;
            int left = (clip.x - destination.x) * source.w / destination.w,
                top = (clip.y - destination.y) * source.h / destination.h;
            int right = (clip.x + clip.w - destination.x) * source.w / destination.w,
                bottom = (clip.y + clip.h - destination.y) * source.h / destination.h;
            SDL_Rect sample{source.x + left, source.y + top, std::max(1, right - left), std::max(1, bottom - top)};
            s.RenderCopy(r, image.texture, &sample, &clip);
        };
        strip({destination.x, destination.y + radius, destination.w, destination.h - radius * 2});
        for (int dy = 0; dy < radius; dy++) {
            int inset = radius - int(std::sqrt(radius * radius - (radius - dy - .5) * (radius - dy - .5)));
            strip({destination.x + inset, destination.y + dy, destination.w - inset * 2, 1});
            strip({destination.x + inset, destination.y + destination.h - 1 - dy, destination.w - inset * 2, 1});
        }
        return true;
    }
};
}
