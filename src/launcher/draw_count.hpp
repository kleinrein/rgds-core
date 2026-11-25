#pragma once
// RGDS_FRAME_LOG's draw counts (developers, for measuring): every SDL draw call goes through a counting forwarder, so
// the log shows how many separate draws a frame issues. The GPU pays per draw on the handheld (Panfrost), so a page
// of small draws can cost more than a page of large ones.
#include "platform/sdl_dynamic.hpp"

namespace rgds::draw_count {
inline long copies = 0, fills = 0, lines = 0, uploads = 0, upload_px = 0; // this frame: copies, fills, lines, UpdateTexture
inline Sdl* api = nullptr;
inline decltype(&SDL_RenderCopy) copy = nullptr;
inline decltype(&SDL_RenderCopyF) copy_f = nullptr;
inline decltype(&SDL_RenderCopyEx) copy_ex = nullptr;
inline decltype(&SDL_RenderFillRect) fill = nullptr;
inline decltype(&SDL_RenderFillRectF) fill_f = nullptr;
inline decltype(&SDL_RenderFillRects) fill_n = nullptr;
inline decltype(&SDL_RenderFillRectsF) fill_nf = nullptr;
inline decltype(&SDL_RenderDrawLine) line = nullptr;
inline decltype(&SDL_RenderDrawRect) rect = nullptr;
inline decltype(&SDL_UpdateTexture) update = nullptr;
inline decltype(&SDL_SetRenderTarget) target = nullptr;
inline long targets = 0;
inline void install(Sdl& s) {
    if (api) return;
    api = &s;
    copy = s.RenderCopy, copy_f = s.RenderCopyF, copy_ex = s.RenderCopyEx, fill = s.RenderFillRect;
    fill_f = s.RenderFillRectF, fill_n = s.RenderFillRects, fill_nf = s.RenderFillRectsF, line = s.RenderDrawLine;
    rect = s.RenderDrawRect, update = s.UpdateTexture, target = s.SetRenderTarget;
    s.UpdateTexture = [](SDL_Texture* t, const SDL_Rect* a, const void* p, int pitch) {
        uploads++;
        upload_px += a ? long(a->w) * a->h : 0;
        return update(t, a, p, pitch);
    };
    s.SetRenderTarget = [](SDL_Renderer* r, SDL_Texture* t) {
        targets++;
        return target(r, t);
    };
    s.RenderCopy = [](SDL_Renderer* r, SDL_Texture* t, const SDL_Rect* a, const SDL_Rect* b) {
        copies++;
        return copy(r, t, a, b);
    };
    if (copy_f)
        s.RenderCopyF = [](SDL_Renderer* r, SDL_Texture* t, const SDL_Rect* a, const SDL_FRect* b) {
            copies++;
            return copy_f(r, t, a, b);
        };
    s.RenderCopyEx = [](SDL_Renderer* r, SDL_Texture* t, const SDL_Rect* a, const SDL_Rect* b, double angle,
                        const SDL_Point* c, SDL_RendererFlip flip) {
        copies++;
        return copy_ex(r, t, a, b, angle, c, flip);
    };
    s.RenderFillRect = [](SDL_Renderer* r, const SDL_Rect* a) {
        fills++;
        return fill(r, a);
    };
    if (fill_f)
        s.RenderFillRectF = [](SDL_Renderer* r, const SDL_FRect* a) {
            fills++;
            return fill_f(r, a);
        };
    s.RenderFillRects = [](SDL_Renderer* r, const SDL_Rect* a, int n) {
        fills++;
        return fill_n(r, a, n);
    };
    if (fill_nf)
        s.RenderFillRectsF = [](SDL_Renderer* r, const SDL_FRect* a, int n) {
            fills++;
            return fill_nf(r, a, n);
        };
    s.RenderDrawLine = [](SDL_Renderer* r, int x1, int y1, int x2, int y2) {
        lines++;
        return line(r, x1, y1, x2, y2);
    };
    s.RenderDrawRect = [](SDL_Renderer* r, const SDL_Rect* a) {
        lines += 4;
        return rect(r, a);
    };
}
}
