#pragma once
#include "launcher/model.hpp"
#include "platform/sdl_dynamic.hpp"
#include "launcher/panel_scale.hpp"
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <functional>
#include <set>
#include <tuple>
#include <initializer_list>

namespace rgds {
// Fixed, bundled font data only. No host fonts or additional device libraries.
class RoundedFont {
    struct Glyph {
        int x, y, w, h, left, top, advance;
    };
    struct Atlas {
        int width = 0, height = 0, ascent = 0;
        std::map<unsigned, Glyph> glyphs;
        std::vector<unsigned char> alpha;
    };
    struct Limits {
        int width, height;
        unsigned count;
    };
    // A glyph and the atlas it came from: the Latin atlas or a CJK family's.
    struct Found {
        const Glyph* glyph;
        const Atlas* atlas;
    };
    // CJK glyphs reach the GPU one by one, copied into a texture per size the
    // first time they are drawn, so thousands of kanji never become one huge
    // texture. A full cache starts over; SDL flushes queued copies first.
    struct Cache {
        SDL_Texture* texture = nullptr;
        std::map<unsigned, SDL_Rect> slots;
        int x = 0, y = 0, row = 0;
    };
    static constexpr int cache_side = 1024;
    Sdl& s;
    fs::path directory;
    std::map<int, Atlas> atlases;
    // CJK families (rounded-ja-N.rgf, ...) load on the first glyph they supply.
    std::map<std::pair<std::string, int>, Atlas> extras;
    std::set<std::pair<std::string, int>> missing;
    std::vector<std::string> families{"ja", "zh", "ko"};
    std::map<std::pair<SDL_Renderer*, int>, SDL_Texture*> textures;
    // The Latin sizes at 1.6x (tools/bake_font.py HIRES_SIZES), for panels larger than 640x480: text is laid out
    // with the five sizes above, as everywhere, and each glyph is drawn from one of these onto whole panel pixels.
    // None of these sizes is one of the five, so they share `textures`. A missing file means stretched glyphs.
    static constexpr std::array<int, 5> hires_sizes = {26, 32, 38, 45, 58};
    std::map<int, Atlas> hires;
    std::set<int> hires_missing;
    std::map<std::pair<SDL_Renderer*, int>, Cache> caches;
    size_t bytes_loaded = 0;
    // Five shipped atlases cover all typography. Other requested point sizes
    // use proportional glyph metrics and the closest atlas, without creating
    // a separate texture or loading an unbounded collection of font files.
    static int baked_size(int size) {
        constexpr std::array<int, 5> sizes = {16, 20, 24, 28, 36};
        int selected = sizes.front();
        for (int candidate : sizes)
            if (std::abs(candidate - size) <= std::abs(selected - size)) selected = candidate;
        return selected;
    }
    static unsigned read32(std::ifstream& f) {
        unsigned char b[4];
        if (!f.read(reinterpret_cast<char*>(b), 4)) throw std::runtime_error("Truncated font atlas");
        return b[0] | (unsigned(b[1]) << 8) | (unsigned(b[2]) << 16) | (unsigned(b[3]) << 24);
    }
    Atlas read_atlas(const fs::path& path, Limits limits) {
        std::ifstream file(path, std::ios::binary);
        char magic[4];
        if (!file.read(magic, 4) || std::string(magic, 4) != "RGF1")
            throw std::runtime_error("Rounded font atlas unavailable");
        Atlas a;
        a.width = static_cast<int>(read32(file));
        a.height = static_cast<int>(read32(file));
        a.ascent = static_cast<int>(read32(file));
        auto count = read32(file);
        if (a.width < 1 || a.width > limits.width || a.height < 1 || a.height > limits.height || a.ascent > 128 ||
            count > limits.count)
            throw std::runtime_error("Invalid font atlas");
        for (unsigned i = 0; i < count; i++) {
            auto cp = read32(file);
            Glyph g{};
            g.x = static_cast<int32_t>(read32(file));
            g.y = static_cast<int32_t>(read32(file));
            g.w = static_cast<int32_t>(read32(file));
            g.h = static_cast<int32_t>(read32(file));
            g.left = static_cast<int32_t>(read32(file));
            g.top = static_cast<int32_t>(read32(file));
            g.advance = static_cast<int32_t>(read32(file));
            if (g.x < 0 || g.y < 0 || g.w < 0 || g.h < 0 || g.x + g.w > a.width || g.y + g.h > a.height ||
                g.advance < 0 || g.advance > 8192 || g.w > cache_side || g.h > cache_side)
                throw std::runtime_error("Invalid font glyph");
            a.glyphs[cp] = g;
        }
        a.alpha.resize(static_cast<size_t>(a.width) * a.height);
        if (!file.read(reinterpret_cast<char*>(a.alpha.data()), a.alpha.size()))
            throw std::runtime_error("Truncated font bitmap");
        bytes_loaded += 20 + count * 32 + a.alpha.size();
        return a;
    }
    Atlas& atlas(int size) {
        size = baked_size(size);
        auto found = atlases.find(size);
        if (found != atlases.end()) return found->second;
        auto a = read_atlas(directory / ("rounded-" + std::to_string(size) + ".rgf"), {1024, 2048, 512});
        if (!a.glyphs.count('?')) throw std::runtime_error("Font fallback glyph missing");
        return atlases.emplace(size, std::move(a)).first->second;
    }
    // A missing or damaged family file only costs its glyphs, which then draw as '?'.
    const Atlas* extra(const std::string& family, int size) {
        auto key = std::make_pair(family, baked_size(size));
        auto found = extras.find(key);
        if (found != extras.end()) return &found->second;
        if (missing.count(key)) return nullptr;
        auto path = directory / ("rounded-" + family + "-" + std::to_string(key.second) + ".rgf");
        try {
            if (fs::exists(path)) return &extras.emplace(key, read_atlas(path, {2048, 16384, 16384})).first->second;
        } catch (const std::exception&) {}
        missing.insert(key);
        return nullptr;
    }
    Found find(int size, unsigned cp) {
        auto& a = atlas(size);
        auto it = a.glyphs.find(cp);
        if (it != a.glyphs.end()) return {&it->second, &a};
        if (cp >= 0x2e80)
            for (const auto& family : families)
                if (const auto* e = extra(family, size)) {
                    auto g = e->glyphs.find(cp);
                    if (g != e->glyphs.end()) return {&g->second, e};
                }
        return {&a.glyphs.at('?'), &a};
    }
    const Atlas* hires_atlas(int px) {
        auto found = hires.find(px);
        if (found != hires.end()) return &found->second;
        if (hires_missing.count(px)) return nullptr;
        try {
            auto a = read_atlas(directory / ("rounded-" + std::to_string(px) + ".rgf"), {1024, 2048, 512});
            if (a.glyphs.count('?')) return &hires.emplace(px, std::move(a)).first->second;
        } catch (const std::exception&) {}
        hires_missing.insert(px);
        return nullptr;
    }
    // Panel pixels per layout pixel: 1 on the RG DS (and in any 640-wide target), 1.6 on the RG DS Plus.
    float pixel_scale(SDL_Renderer* r) {
        if (!panel_part("text")) return 1;
        int w = 0, h = 0, lw = 0, lh = 0;
        if (!s.RenderCopyF || !s.RenderGetLogicalSize || s.GetRendererOutputSize(r, &w, &h) != 0) return 1;
        s.RenderGetLogicalSize(r, &lw, &lh);
        return lw > 0 && w > lw ? float(w) / lw : 1;
    }
    SDL_Texture* texture(SDL_Renderer* r, int size) {
        size = baked_size(size);
        return atlas_texture(r, size, atlas(size));
    }
    SDL_Texture* atlas_texture(SDL_Renderer* r, int size, const Atlas& a) {
        auto key = std::make_pair(r, size);
        auto found = textures.find(key);
        if (found != textures.end()) return found->second;
        auto* surface = s.CreateRGBSurfaceWithFormat(0, a.width, a.height, 32, SDL_PIXELFORMAT_RGBA32);
        if (!surface) throw std::runtime_error(s.GetError());
        for (int y = 0; y < a.height; y++) {
            auto* row = static_cast<unsigned char*>(surface->pixels) + y * surface->pitch;
            for (int x = 0; x < a.width; x++) {
                row[x * 4] = row[x * 4 + 1] = row[x * 4 + 2] = 255;
                row[x * 4 + 3] = a.alpha[y * a.width + x];
            }
        }
        auto* result = s.CreateTextureFromSurface(r, surface);
        s.FreeSurface(surface);
        if (!result) throw std::runtime_error(s.GetError());
        s.SetTextureBlendMode(result, SDL_BLENDMODE_BLEND);
        textures[key] = result;
        return result;
    }
    // The cache texture and the glyph's place in it, uploading the glyph if it is new.
    std::pair<SDL_Texture*, SDL_Rect> cached(SDL_Renderer* r, int size, unsigned cp, const Found& found) {
        auto& cache = caches[std::make_pair(r, baked_size(size))];
        if (!cache.texture) {
            cache.texture =
                s.CreateTexture(r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, cache_side, cache_side);
            if (!cache.texture) throw std::runtime_error(s.GetError());
            s.SetTextureBlendMode(cache.texture, SDL_BLENDMODE_BLEND);
        }
        auto slot = cache.slots.find(cp);
        if (slot != cache.slots.end()) return {cache.texture, slot->second};
        const auto& g = *found.glyph;
        if (cache.x + g.w > cache_side) {
            cache.x = 0;
            cache.y += cache.row + 1;
            cache.row = 0;
        }
        if (cache.y + g.h > cache_side) {
            cache.slots.clear();
            cache.x = cache.y = cache.row = 0;
        }
        SDL_Rect place{cache.x, cache.y, g.w, g.h};
        if (g.w && g.h) {
            std::vector<unsigned char> pixels(static_cast<size_t>(g.w) * g.h * 4, 255);
            for (int y = 0; y < g.h; y++)
                for (int x = 0; x < g.w; x++)
                    pixels[(static_cast<size_t>(y) * g.w + x) * 4 + 3] =
                        found.atlas->alpha[static_cast<size_t>(g.y + y) * found.atlas->width + g.x + x];
            s.UpdateTexture(cache.texture, &place, pixels.data(), g.w * 4);
        }
        cache.x += g.w + 1;
        cache.row = std::max(cache.row, g.h);
        return {cache.texture, cache.slots[cp] = place};
    }

public:
    RoundedFont(Sdl& api, fs::path path) : s(api), directory(std::move(path)) {}
    ~RoundedFont() { clear(); }
    void clear() {
        for (auto entry : textures) s.DestroyTexture(entry.second);
        textures.clear();
        for (auto& entry : caches)
            if (entry.second.texture) s.DestroyTexture(entry.second.texture);
        caches.clear();
    }
    // Han characters shared by Japanese and Chinese take the interface language's
    // own shapes first: zh-* prefers the zh family, ko-* the ko family.
    void prefer(const std::string& language) {
        std::vector<std::string> order{"ja", "zh", "ko"};
        auto own = std::find(order.begin(), order.end(), language.substr(0, 2));
        if (own != order.end()) std::rotate(order.begin(), own, own + 1);
        if (order == families) return;
        families = order;
        for (auto& entry : caches) {
            entry.second.slots.clear();
            entry.second.x = entry.second.y = entry.second.row = 0;
        }
    }
    void check(std::initializer_list<int> sizes = {16, 20, 24, 28, 36}) {
        for (int size : sizes) atlas(size);
    }
    size_t loaded_atlases() const { return atlases.size(); }
    size_t loaded_extras() const { return extras.size(); }
    size_t loaded_bytes() const { return bytes_loaded; }
    // Whether this codepoint draws as itself rather than the '?' fallback.
    bool has(int size, unsigned cp) { return cp == '?' || find(size, cp).glyph != &atlas(size).glyphs.at('?'); }
    // The baseline's distance below a label's y at this point size.
    int ascent(int size) { return static_cast<int>(std::round(atlas(size).ascent * float(size) / baked_size(size))); }
    static std::vector<unsigned> codepoints(const std::string& value, int limit = 1000) {
        std::vector<unsigned> out;
        for (size_t i = 0; i < value.size() && static_cast<int>(out.size()) < limit;) {
            unsigned cp = static_cast<unsigned char>(value[i++]);
            int extra =
                cp < 128 ? 0 : ((cp & 0xe0) == 0xc0 ? 1 : ((cp & 0xf0) == 0xe0 ? 2 : ((cp & 0xf8) == 0xf0 ? 3 : -1)));
            if (extra < 0) {
                out.push_back('?');
                continue;
            }
            if (extra) cp &= (1u << (6 - extra)) - 1;
            for (int n = 0; n < extra; n++) {
                if (i >= value.size() || (static_cast<unsigned char>(value[i]) & 0xc0) != 0x80) {
                    cp = '?';
                    break;
                }
                cp = (cp << 6) | (static_cast<unsigned char>(value[i++]) & 63);
            }
            out.push_back(cp);
        }
        return out;
    }
    float width(const std::string& value, int size, int limit = 1000) {
        float width = 0, scale = float(size) / baked_size(size);
        for (auto cp : codepoints(value, limit)) width += find(size, cp).glyph->advance / 64.0f * scale;
        return width;
    }
    std::pair<int, int> ink_bounds(const std::string& value, int size) {
        auto& a = atlas(size);
        float scale = float(size) / baked_size(size);
        int top = 10000, bottom = -10000;
        for (auto cp : codepoints(value)) {
            const auto& g = *find(size, cp).glyph;
            if (!g.h) continue;
            int y = int(std::round((a.ascent + g.top) * scale));
            top = std::min(top, y);
            bottom = std::max(bottom, y + int(std::round(g.h * scale)));
        }
        return bottom < top ? std::pair<int, int>{0, 0} : std::pair<int, int>{top, bottom};
    }
    // The text's alpha, pixel by pixel, for drawing into memory instead of a renderer
    // (a chat message's pad). Only the baked sizes, placed exactly as draw() places them.
    void rasterize(const std::string& value, int size, int x, int y,
                   const std::function<void(int, int, uint8_t)>& put) {
        if (size != baked_size(size)) return;
        auto& a = atlas(size);
        float pen = static_cast<float>(x);
        for (auto cp : codepoints(value)) {
            auto found = find(size, cp);
            const auto& g = *found.glyph;
            const int left = static_cast<int>(std::round(pen + g.left)), top = y + a.ascent + g.top;
            for (int gy = 0; gy < g.h; gy++)
                for (int gx = 0; gx < g.w; gx++) {
                    const uint8_t alpha =
                        found.atlas->alpha[static_cast<size_t>(g.y + gy) * found.atlas->width + g.x + gx];
                    if (alpha) put(left + gx, top + gy, alpha);
                }
            pen += g.advance / 64.0f;
        }
    }
    int opacity = 255; // text that fades (the Media hero's cross-fade); back to 255 after
    void draw(SDL_Renderer* r, const std::string& value, int x, int y, int size, Color color, int limit,
              int max_width) {
        auto& a = atlas(size);
        auto* image = texture(r, size);
        s.SetTextureColorMod(image, color.r, color.g, color.b);
        s.SetTextureAlphaMod(image, Uint8(std::clamp(opacity, 0, 255)));
        auto chars = codepoints(value, limit);
        float total = width(value, size, limit), scale = float(size) / baked_size(size);
        // A panel larger than the layout: the 1.6x atlas nearest the size on the panel, if it is there.
        const float k = pixel_scale(r);
        const Atlas* hi = nullptr;
        SDL_Texture* hi_image = nullptr;
        float hi_scale = 0; // layout pixels per pixel of the 1.6x atlas
        if (k > 1.2f) {
            int px = hires_sizes.front();
            for (int candidate : hires_sizes)
                if (std::abs(candidate - size * k) < std::abs(px - size * k)) px = candidate;
            if ((hi = hires_atlas(px))) {
                hi_image = atlas_texture(r, px, *hi);
                hi_scale = float(size) / px;
                s.SetTextureColorMod(hi_image, color.r, color.g, color.b);
                s.SetTextureAlphaMod(hi_image, Uint8(std::clamp(opacity, 0, 255)));
            }
        }
        const bool clipped = total > max_width;
        float pen = static_cast<float>(x);
        float ellipsis = width("...", size);
        for (auto cp : chars) {
            auto found = find(size, cp);
            const auto& g = *found.glyph;
            if (clipped && pen - x + g.advance / 64.0f * scale > max_width - ellipsis) {
                draw(r, "...", static_cast<int>(pen), y, size, color, 3, static_cast<int>(ellipsis) + 1);
                break;
            }
            // Sharp: the same pen and baseline as the layout's glyph, the 1.6x glyph's shape on whole panel pixels.
            if (hi_image && found.atlas == &a) {
                auto h = hi->glyphs.find(cp);
                if (h != hi->glyphs.end()) {
                    const auto& hg = h->second;
                    if (hg.w && hg.h) {
                        const float baseline = y + a.ascent * scale;
                        const float px = std::round((pen + hg.left * hi_scale) * k),
                                    py = std::round((baseline + hg.top * hi_scale) * k);
                        const float pw = std::round(hg.w * hi_scale * k), ph = std::round(hg.h * hi_scale * k);
                        SDL_Rect source{hg.x, hg.y, hg.w, hg.h};
                        SDL_FRect dest{px / k, py / k, pw / k, ph / k};
                        s.RenderCopyF(r, hi_image, &source, &dest);
                    }
                    pen += g.advance / 64.0f * scale;
                    continue;
                }
            }
            SDL_Rect source{g.x, g.y, g.w, g.h},
                dest{static_cast<int>(std::round(pen + g.left * scale)),
                     y + static_cast<int>(std::round((a.ascent + g.top) * scale)),
                     static_cast<int>(std::round(g.w * scale)), static_cast<int>(std::round(g.h * scale))};
            if (g.w && g.h) {
                if (found.atlas == &a) s.RenderCopy(r, image, &source, &dest);
                else {
                    auto slot = cached(r, size, cp, found);
                    s.SetTextureColorMod(slot.first, color.r, color.g, color.b);
                    s.SetTextureAlphaMod(slot.first, Uint8(std::clamp(opacity, 0, 255)));
                    s.RenderCopy(r, slot.first, &slot.second, &dest);
                }
            }
            pen += g.advance / 64.0f * scale;
        }
    }
};
}
