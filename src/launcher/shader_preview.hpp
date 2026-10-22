#pragma once
#include "launcher/asset_cache.hpp"
#include "platform/sdl_dynamic.hpp"
#include "platform/shader_catalog.hpp"
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <vector>

namespace rgds {
enum class ShaderPreviewMode { Filtered = 0, Original = 1, Split = 2 };

// Real GLES-rendered reference images. The launcher neither approximates the
// filters nor compiles shaders during navigation. Warm before rendering; draw
// and detail only use cached textures, including cached missing-file results.
class ShaderPreview {
    Sdl& s;
    struct Image {
        SDL_Texture* texture = nullptr;
        fs::path stem;
        AssetStamp packed, bmp;
        int width = 0, height = 0;
        uint64_t used = 0, bytes = 0;
    };
    using Key = std::pair<SDL_Renderer*, std::string>;
    std::map<Key, Image> images;
    // Picker tiles: small 1:1 crops (no scaling, so the effect looks as it
    // will on the screen), cached apart from the full previews' byte budget.
    struct Thumb {
        SDL_Texture* texture = nullptr;
        uint64_t used = 0;
    };
    std::map<Key, Thumb> thumbs;
    static constexpr size_t thumb_limit = 64;
    uint64_t clock = 0, texture_bytes = 0;
    static constexpr size_t cache_limit = 8;
    static constexpr uint64_t byte_limit = 10 * 1024 * 1024;
    using ScaleMode = int (*)(SDL_Texture*, int);
    ScaleMode scale_mode = nullptr;
    static int bounded_scene(int scene) { return scene >= 0 && scene < scene_count ? scene : 0; }
    static const char* scene_stem(int scene) {
        static const char* names[] = {"grove", "circuit", "starfall"};
        return names[bounded_scene(scene)];
    }
    static fs::path stem(const fs::path& folder, int scene, int selected) {
        return folder / scene_stem(scene) / ("shader-" + std::to_string(std::clamp(selected, 0, shader_count - 1)));
    }
    static SDL_Rect patch(int scene) {
        static const SDL_Rect patches[] = {{36, 102, 64, 48}, {92, 80, 64, 48}, {94, 98, 64, 48}};
        return patches[bounded_scene(scene)];
    }
    static uint16_t u16(const std::vector<unsigned char>& data, size_t at) {
        return uint16_t(data[at]) | (uint16_t(data[at + 1]) << 8);
    }
    SDL_Surface* packed_surface(const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file) return nullptr;
        file.seekg(0, std::ios::end);
        auto length = file.tellg();
        if (length < 16 || length > 8 * 1024 * 1024) return nullptr;
        file.seekg(0);
        std::vector<unsigned char> data(static_cast<size_t>(length));
        if (!file.read(reinterpret_cast<char*>(data.data()), data.size()) || std::memcmp(data.data(), "RGSPV001", 8))
            return nullptr;
        const int w = u16(data, 8), h = u16(data, 10), side = u16(data, 12), count = u16(data, 14);
        if (w <= 0 || h <= 0 || w > 1280 || h > 960 || side <= 0 || side > 16 || !count || w % side || h % side)
            return nullptr;
        const size_t tile_bytes = size_t(side) * side * 4, atlas_bytes = size_t(count) * tile_bytes, columns = w / side,
                     rows = h / side;
        const size_t index_begin = 16 + atlas_bytes;
        if (index_begin + columns * rows * 2 != data.size()) return nullptr;
        for (size_t i = 0; i < columns * rows; i++)
            if (u16(data, index_begin + i * 2) >= count) return nullptr;
        auto* surface = s.CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
        if (!surface) return nullptr;
        for (size_t y = 0; y < rows; y++)
            for (size_t x = 0; x < columns; x++) {
                const auto* tile = data.data() + 16 + u16(data, index_begin + (y * columns + x) * 2) * tile_bytes;
                for (int row = 0; row < side; row++)
                    std::memcpy(static_cast<unsigned char*>(surface->pixels) + (y * side + row) * surface->pitch +
                                    x * side * 4,
                                tile + row * side * 4, side * 4);
            }
        return surface;
    }
    void discard(decltype(images)::iterator entry) {
        if (entry->second.texture) s.DestroyTexture(entry->second.texture);
        texture_bytes -= entry->second.bytes;
        images.erase(entry);
    }
    void trim(uint64_t incoming) {
        while (!images.empty() && (images.size() >= cache_limit || texture_bytes + incoming > byte_limit)) {
            auto oldest = std::min_element(images.begin(), images.end(),
                                           [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
            discard(oldest);
            counters.evictions++;
        }
    }
    bool warm_one(SDL_Renderer* renderer, const fs::path& folder, int scene, int selected) {
        counters.lookups++;
        auto base = stem(folder, scene, selected);
        Key key{renderer, base.string()};
        auto existing = images.find(key);
        if (existing != images.end()) {
            existing->second.used = ++clock;
            counters.hits++;
            return existing->second.texture != nullptr;
        }
        Image value;
        value.stem = base;
        value.used = ++clock;
        counters.stamp_checks += 2;
        value.packed = asset_stamp(base.string() + ".rgsp");
        value.bmp = asset_stamp(base.string() + ".bmp");
        SDL_Surface* surface = nullptr;
        if (value.packed.present) {
            counters.file_reads++;
            surface = packed_surface(base.string() + ".rgsp");
        }
        if (!surface && value.bmp.present) {
            counters.file_reads++;
            auto* file = s.RWFromFile((base.string() + ".bmp").c_str(), "rb");
            if (file) surface = s.LoadBMP_RW(file, 1);
        }
        if (surface) {
            const uint64_t bytes = surface->w > 0 && surface->h > 0 ? uint64_t(surface->w) * surface->h * 4 : 0;
            if (bytes && bytes <= byte_limit) {
                trim(bytes);
                value.width = surface->w;
                value.height = surface->h;
                s.SetHint("SDL_RENDER_SCALE_QUALITY", "1");
                value.texture = s.CreateTextureFromSurface(renderer, surface);
                if (value.texture) {
                    value.bytes = bytes;
                    texture_bytes += bytes;
                    s.SetTextureBlendMode(value.texture, SDL_BLENDMODE_NONE);
                }
            }
            s.FreeSurface(surface);
        }
        trim(0);
        bool ready = value.texture != nullptr;
        images.emplace(std::move(key), std::move(value));
        return ready;
    }
    Image* image(SDL_Renderer* renderer, const fs::path& folder, int scene, int selected) {
        counters.lookups++;
        auto found = images.find({renderer, stem(folder, scene, selected).string()});
        if (found == images.end() || !found->second.texture) return nullptr;
        counters.hits++;
        found->second.used = ++clock;
        return &found->second;
    }
    bool paint(SDL_Renderer* renderer, const fs::path& folder, int scene, int selected, SDL_Rect box,
               ShaderPreviewMode mode, float split, bool magnified) {
        if (box.w <= 0 || box.h <= 0 || selected < 0 || selected >= shader_count) return false;
        auto* original =
            (mode != ShaderPreviewMode::Filtered || selected == 0) ? image(renderer, folder, scene, 0) : nullptr;
        auto* filtered = mode != ShaderPreviewMode::Original ? image(renderer, folder, scene, selected) : nullptr;
        if ((mode != ShaderPreviewMode::Filtered && !original) || (mode != ShaderPreviewMode::Original && !filtered))
            return false;
        auto* reference = filtered ? filtered : original;
        if (!reference) return false;
        SDL_Rect source{0, 0, reference->width, reference->height};
        if (magnified) {
            auto area = patch(scene);
            source = {area.x * reference->width / 256, area.y * reference->height / 192,
                      area.w * reference->width / 256, area.h * reference->height / 192};
        }
        const double factor = std::min(double(box.w) / source.w, double(box.h) / source.h);
        SDL_Rect target{box.x + (box.w - int(source.w * factor)) / 2, box.y + (box.h - int(source.h * factor)) / 2,
                        int(source.w * factor), int(source.h * factor)};
        auto copy = [&](Image* entry, const SDL_Rect& src, const SDL_Rect& dst) {
            if (scale_mode) scale_mode(entry->texture, magnified ? 0 : 1);
            return s.RenderCopy(renderer, entry->texture, &src, &dst) == 0;
        };
        if (mode == ShaderPreviewMode::Original) return copy(original, source, target);
        if (mode == ShaderPreviewMode::Filtered || selected == 0) return copy(filtered, source, target);
        if (original->width != filtered->width || original->height != filtered->height) return false;
        split = std::isfinite(split) ? std::clamp(split, 0.f, 1.f) : .5f;
        const int source_cut = int(std::round(source.w * split)), target_cut = int(std::round(target.w * split));
        bool ok = true;
        if (source_cut > 0 && target_cut > 0) {
            SDL_Rect src = source, dst = target;
            src.w = source_cut;
            dst.w = target_cut;
            ok = copy(original, src, dst);
        }
        if (source_cut < source.w && target_cut < target.w) {
            SDL_Rect src = source, dst = target;
            src.x += source_cut;
            src.w -= source_cut;
            dst.x += target_cut;
            dst.w -= target_cut;
            ok = copy(filtered, src, dst) && ok;
        }
        return ok;
    }

public:
    static constexpr int scene_count = 3;
    CacheCounters counters;
    explicit ShaderPreview(Sdl& api) : s(api) {
        scale_mode = reinterpret_cast<ScaleMode>(dlsym(s.handle, "SDL_SetTextureScaleMode"));
    }
    ~ShaderPreview() { clear(); }
    ShaderPreview(const ShaderPreview&) = delete;
    ShaderPreview& operator=(const ShaderPreview&) = delete;
    static const char* scene_name(int scene) {
        static const char* names[] = {"Sunlit Grove", "Neon Circuit", "Starfall"};
        return names[bounded_scene(scene)];
    }
    // Off is always warmed for an immediate before/after comparison. Optional
    // neighbour prefetch makes the next selection a texture-cache hit.
    bool warm(SDL_Renderer* renderer, const fs::path& folder, int scene, int selected, bool neighbours = false) {
        if (!renderer || selected < 0 || selected >= shader_count) return false;
        const bool original = warm_one(renderer, folder, scene, 0);
        bool ready = selected == 0 ? original : warm_one(renderer, folder, scene, selected);
        if (neighbours) {
            warm_one(renderer, folder, scene, (selected + shader_count - 1) % shader_count);
            warm_one(renderer, folder, scene, (selected + 1) % shader_count);
        }
        return original && ready;
    }
    bool draw(SDL_Renderer* renderer, const fs::path& folder, int scene, int selected, SDL_Rect box,
              ShaderPreviewMode mode = ShaderPreviewMode::Filtered, float split = .5f) {
        return paint(renderer, folder, scene, selected, box, mode, split, false);
    }
    bool detail(SDL_Renderer* renderer, const fs::path& folder, int scene, int selected, SDL_Rect box,
                ShaderPreviewMode mode = ShaderPreviewMode::Filtered, float split = .5f) {
        return paint(renderer, folder, scene, selected, box, mode, split, true);
    }
    void clear() {
        for (auto& value : images)
            if (value.second.texture) s.DestroyTexture(value.second.texture);
        images.clear();
        texture_bytes = 0;
        clear_thumbs();
    }
    void clear_thumbs() {
        for (auto& value : thumbs)
            if (value.second.texture) s.DestroyTexture(value.second.texture);
        thumbs.clear();
    }
    size_t thumb_reads = 0;
    size_t thumb_count() const { return thumbs.size(); }
    // Draw one picker tile. Reads at most `budget` new files per call, so a
    // fast scroll never waits on storage; the caller redraws until all are in.
    // Returns false while the tile is still pending or has no preview.
    bool thumb(SDL_Renderer* renderer, const fs::path& folder, int scene, int selected, SDL_Rect box, int& budget) {
        if (!renderer || selected < 0 || selected >= shader_count || box.w <= 0 || box.h <= 0) return false;
        auto base = stem(folder, scene, selected);
        Key key{renderer, base.string()};
        auto found = thumbs.find(key);
        if (found == thumbs.end()) {
            if (budget <= 0) return false;
            budget--;
            Thumb value;
            value.used = ++clock;
            if (auto* full = packed_surface(base.string() + ".rgsp")) {
                thumb_reads++;
                const auto area = patch(scene);
                const int side = std::min(full->w, full->h) / 4;
                const int cx = (area.x + area.w / 2) * full->w / 256, cy = (area.y + area.h / 2) * full->h / 192;
                const int x0 = std::clamp(cx - side / 2, 0, full->w - side),
                          y0 = std::clamp(cy - side / 2, 0, full->h - side);
                if (auto* crop = s.CreateRGBSurfaceWithFormat(0, side, side, 32, SDL_PIXELFORMAT_RGBA32)) {
                    for (int row = 0; row < side; row++)
                        std::memcpy(static_cast<unsigned char*>(crop->pixels) + row * crop->pitch,
                                    static_cast<unsigned char*>(full->pixels) + (y0 + row) * full->pitch + x0 * 4,
                                    side * 4);
                    value.texture = s.CreateTextureFromSurface(renderer, crop);
                    if (value.texture) {
                        s.SetTextureBlendMode(value.texture, SDL_BLENDMODE_NONE);
                        if (scale_mode) scale_mode(value.texture, 0);
                    }
                    s.FreeSurface(crop);
                }
                s.FreeSurface(full);
            }
            while (thumbs.size() >= thumb_limit) {
                auto oldest = std::min_element(thumbs.begin(), thumbs.end(), [](const auto& a, const auto& b) {
                    return a.second.used < b.second.used;
                });
                if (oldest->second.texture) s.DestroyTexture(oldest->second.texture);
                thumbs.erase(oldest);
            }
            found = thumbs.emplace(std::move(key), value).first;
        }
        found->second.used = ++clock;
        return found->second.texture && s.RenderCopy(renderer, found->second.texture, nullptr, &box) == 0;
    }
    bool thumb_known(SDL_Renderer* renderer, const fs::path& folder, int scene, int selected) const {
        return thumbs.count({renderer, stem(folder, scene, selected).string()}) > 0;
    }
    size_t count() const { return images.size(); }
    uint64_t bytes() const { return texture_bytes; }
    bool refresh() {
        bool changed = false;
        for (auto it = images.begin(); it != images.end();) {
            counters.stamp_checks += 2;
            if (asset_stamp(it->second.stem.string() + ".rgsp") != it->second.packed ||
                asset_stamp(it->second.stem.string() + ".bmp") != it->second.bmp) {
                auto old = it++;
                discard(old);
                counters.invalidations++;
                changed = true;
            } else ++it;
        }
        if (changed) clear_thumbs();
        return changed;
    }
};
}
