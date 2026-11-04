#pragma once
// Rectangle fills on the in-game menu's software canvases. SDL's software renderer goes through its command queue and a
// per-pixel loop for each fill, opaque ones too: on the handheld's CPU that was most of a menu frame (113 ms). These
// write the same pixels straight into the canvas; anything else (a render target, a scale) still goes to SDL.
#include "platform/sdl_dynamic.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <type_traits>

namespace rgds {
struct FillCanvas {
    SDL_Renderer* renderer = nullptr;
    SDL_Surface* surface = nullptr;
};
struct CanvasFills {
    std::array<FillCanvas, 2> canvases{};
    decltype(&SDL_RenderFillRect) sdl_fill = nullptr;
    decltype(&SDL_RenderDrawLine) sdl_line = nullptr;
    decltype(&SDL_RenderFlush) flush = nullptr;
    decltype(&SDL_GetRenderDrawColor) color = nullptr;
    decltype(&SDL_GetRenderDrawBlendMode) mode = nullptr;
    decltype(&SDL_GetRenderTarget) target = nullptr;
    decltype(&SDL_RenderGetViewport) viewport = nullptr;
    decltype(&SDL_RenderGetClipRect) clip = nullptr;
    decltype(&SDL_RenderIsClipEnabled) clipped = nullptr;
    decltype(&SDL_RenderGetScale) scale = nullptr;
};
inline CanvasFills canvas_fills;

// SDL's own BLEND on 8-bit channels: dst = src * a / 255 + dst * (255 - a) / 255, each division rounded down.
inline void blend_row(uint32_t* row, int count, uint32_t source, uint32_t inverse) {
    const auto over_255 = [](uint32_t x) { return ((x + 0x00010001u + ((x >> 8) & 0x00FF00FFu)) >> 8) & 0x00FF00FFu; };
    for (int i = 0; i < count; i++) {
        const uint32_t pixel = row[i];
        row[i] = source + over_255((pixel & 0x00FF00FFu) * inverse) +
                 (over_255(((pixel >> 8) & 0x00FF00FFu) * inverse) << 8);
    }
}

inline int canvas_fill(SDL_Renderer* renderer, const SDL_Rect* box) {
    auto& f = canvas_fills;
    SDL_Surface* surface = nullptr;
    for (const auto& canvas : f.canvases)
        if (canvas.renderer == renderer) surface = canvas.surface;
    Uint8 r = 0, g = 0, b = 0, a = 0;
    SDL_BlendMode blend = SDL_BLENDMODE_NONE;
    float sx = 1, sy = 1;
    f.scale(renderer, &sx, &sy);
    if (!surface || f.target(renderer) || sx != 1 || sy != 1 || f.color(renderer, &r, &g, &b, &a) != 0 ||
        f.mode(renderer, &blend) != 0 || (blend != SDL_BLENDMODE_NONE && blend != SDL_BLENDMODE_BLEND))
        return f.sdl_fill(renderer, box);
    f.flush(renderer);
    SDL_Rect view{};
    f.viewport(renderer, &view);
    // SDL paints at least one pixel each way, so a corner row with nothing left in it still leaves a dot.
    SDL_Rect area = box ? SDL_Rect{box->x + view.x, box->y + view.y, std::max(box->w, 1), std::max(box->h, 1)} : view;
    int x0 = std::max({area.x, view.x, 0}), y0 = std::max({area.y, view.y, 0});
    int x1 = std::min({area.x + area.w, view.x + view.w, surface->w}),
        y1 = std::min({area.y + area.h, view.y + view.h, surface->h});
    if (f.clipped(renderer)) {
        SDL_Rect clip{};
        f.clip(renderer, &clip);
        x0 = std::max(x0, clip.x + view.x);
        y0 = std::max(y0, clip.y + view.y);
        x1 = std::min(x1, clip.x + view.x + clip.w);
        y1 = std::min(y1, clip.y + view.y + clip.h);
    }
    if (x1 <= x0 || y1 <= y0) return 0;
    const bool plain = blend == SDL_BLENDMODE_NONE || a == 255;
    if (!plain && !a) return 0;
    const auto over_255 = [](uint32_t x, uint32_t y) { return x * y / 255; };
    const uint32_t word = plain ? uint32_t(r) | uint32_t(g) << 8 | uint32_t(b) << 16 | uint32_t(a) << 24
                                : over_255(r, a) | over_255(g, a) << 8 | over_255(b, a) << 16 | uint32_t(a) << 24;
    auto* base = static_cast<unsigned char*>(surface->pixels);
    for (int y = y0; y < y1; y++) {
        auto* row = reinterpret_cast<uint32_t*>(base + size_t(y) * surface->pitch) + x0;
        if (plain) std::fill_n(row, x1 - x0, word);
        else blend_row(row, x1 - x0, word, 255u - a);
    }
    return 0;
}

// Straight lines are one-pixel fills; the panel patterns draw dozens of them.
inline int canvas_line(SDL_Renderer* renderer, int x1, int y1, int x2, int y2) {
    if (x1 != x2 && y1 != y2) return canvas_fills.sdl_line(renderer, x1, y1, x2, y2);
    const SDL_Rect box{std::min(x1, x2), std::min(y1, y2), std::abs(x2 - x1) + 1, std::abs(y2 - y1) + 1};
    return canvas_fill(renderer, &box);
}

// `surface` must be RGBA32 and the canvas `renderer` draws to. Without the calls it needs, SDL keeps doing the fills.
inline void add_fill_canvas(Sdl& s, SDL_Renderer* renderer, SDL_Surface* surface) {
    auto& f = canvas_fills;
    const auto load = [&](auto& function, const char* name) {
        function = reinterpret_cast<std::decay_t<decltype(function)>>(dlsym(s.handle, name));
        return function != nullptr;
    };
    if (!load(f.color, "SDL_GetRenderDrawColor") || !load(f.mode, "SDL_GetRenderDrawBlendMode") ||
        !load(f.target, "SDL_GetRenderTarget") || !load(f.viewport, "SDL_RenderGetViewport") ||
        !load(f.clip, "SDL_RenderGetClipRect") || !load(f.clipped, "SDL_RenderIsClipEnabled") ||
        !load(f.scale, "SDL_RenderGetScale"))
        return;
    for (auto& canvas : f.canvases)
        if (!canvas.renderer) {
            canvas = {renderer, surface};
            if (!f.sdl_fill) {
                f.sdl_fill = s.RenderFillRect;
                f.flush = s.RenderFlush;
                s.RenderFillRect = canvas_fill;
                f.sdl_line = s.RenderDrawLine;
                s.RenderDrawLine = canvas_line;
            }
            return;
        }
}
} // namespace rgds
