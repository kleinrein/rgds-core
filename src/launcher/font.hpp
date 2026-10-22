#pragma once
#include "launcher/model.hpp"
#include "platform/sdl_dynamic.hpp"
#include <cstdint>
#include <cmath>
#include <functional>
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
    Sdl& s;
    fs::path directory;
    std::map<int, Atlas> atlases;
    std::map<std::pair<SDL_Renderer*, int>, SDL_Texture*> textures;
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
    Atlas& atlas(int size) {
        size = baked_size(size);
        auto found = atlases.find(size);
        if (found != atlases.end()) return found->second;
        std::ifstream file(directory / ("rounded-" + std::to_string(size) + ".rgf"), std::ios::binary);
        char magic[4];
        if (!file.read(magic, 4) || std::string(magic, 4) != "RGF1")
            throw std::runtime_error("Rounded font atlas unavailable");
        Atlas a;
        a.width = static_cast<int>(read32(file));
        a.height = static_cast<int>(read32(file));
        a.ascent = static_cast<int>(read32(file));
        auto count = read32(file);
        if (a.width < 1 || a.width > 1024 || a.height < 1 || a.height > 2048 || a.ascent > 128 || count > 512)
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
                g.advance < 0 || g.advance > 8192)
                throw std::runtime_error("Invalid font glyph");
            a.glyphs[cp] = g;
        }
        if (!a.glyphs.count('?')) throw std::runtime_error("Font fallback glyph missing");
        a.alpha.resize(static_cast<size_t>(a.width) * a.height);
        if (!file.read(reinterpret_cast<char*>(a.alpha.data()), a.alpha.size()))
            throw std::runtime_error("Truncated font bitmap");
        bytes_loaded += 20 + count * 32 + a.alpha.size();
        return atlases.emplace(size, std::move(a)).first->second;
    }
    SDL_Texture* texture(SDL_Renderer* r, int size) {
        size = baked_size(size);
        auto key = std::make_pair(r, size);
        auto found = textures.find(key);
        if (found != textures.end()) return found->second;
        auto& a = atlas(size);
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

public:
    RoundedFont(Sdl& api, fs::path path) : s(api), directory(std::move(path)) {}
    ~RoundedFont() { clear(); }
    void clear() {
        for (auto entry : textures) s.DestroyTexture(entry.second);
        textures.clear();
    }
    void check(std::initializer_list<int> sizes = {16, 20, 24, 28, 36}) {
        for (int size : sizes) atlas(size);
    }
    size_t loaded_atlases() const { return atlases.size(); }
    size_t loaded_bytes() const { return bytes_loaded; }
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
        auto& a = atlas(size);
        float width = 0, scale = float(size) / baked_size(size);
        for (auto cp : codepoints(value, limit)) {
            auto it = a.glyphs.find(cp);
            if (it == a.glyphs.end()) it = a.glyphs.find('?');
            width += it->second.advance / 64.0f * scale;
        }
        return width;
    }
    std::pair<int, int> ink_bounds(const std::string& value, int size) {
        auto& a = atlas(size);
        float scale = float(size) / baked_size(size);
        int top = 10000, bottom = -10000;
        for (auto cp : codepoints(value)) {
            auto it = a.glyphs.find(cp);
            if (it == a.glyphs.end()) it = a.glyphs.find('?');
            const auto& g = it->second;
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
            auto it = a.glyphs.find(cp);
            if (it == a.glyphs.end()) it = a.glyphs.find('?');
            const auto& g = it->second;
            const int left = static_cast<int>(std::round(pen + g.left)), top = y + a.ascent + g.top;
            for (int gy = 0; gy < g.h; gy++)
                for (int gx = 0; gx < g.w; gx++) {
                    const uint8_t alpha = a.alpha[static_cast<size_t>(g.y + gy) * a.width + g.x + gx];
                    if (alpha) put(left + gx, top + gy, alpha);
                }
            pen += g.advance / 64.0f;
        }
    }
    void draw(SDL_Renderer* r, const std::string& value, int x, int y, int size, Color color, int limit,
              int max_width) {
        auto& a = atlas(size);
        auto* image = texture(r, size);
        s.SetTextureColorMod(image, color.r, color.g, color.b);
        auto chars = codepoints(value, limit);
        float total = width(value, size, limit), scale = float(size) / baked_size(size);
        const bool clipped = total > max_width;
        float pen = static_cast<float>(x);
        float ellipsis = width("...", size);
        for (auto cp : chars) {
            auto it = a.glyphs.find(cp);
            if (it == a.glyphs.end()) it = a.glyphs.find('?');
            auto g = it->second;
            if (clipped && pen - x + g.advance / 64.0f * scale > max_width - ellipsis) {
                draw(r, "...", static_cast<int>(pen), y, size, color, 3, static_cast<int>(ellipsis) + 1);
                break;
            }
            SDL_Rect source{g.x, g.y, g.w, g.h},
                dest{static_cast<int>(std::round(pen + g.left * scale)),
                     y + static_cast<int>(std::round((a.ascent + g.top) * scale)),
                     static_cast<int>(std::round(g.w * scale)), static_cast<int>(std::round(g.h * scale))};
            if (g.w && g.h) s.RenderCopy(r, image, &source, &dest);
            pen += g.advance / 64.0f * scale;
        }
    }
};
}
