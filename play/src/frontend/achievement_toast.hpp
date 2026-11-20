// SPDX-License-Identifier: GPL-3.0-or-later
// RetroAchievements notices on the lower screen: a card that rises from the bottom edge, stays a few seconds and
// goes, one notice at a time, over whatever the screen shows (the companion, the info screen or a swapped game).
// The game's picture on the top screen is never covered.
#pragma once
#include "app/achievements.hpp"
#include "ui/canvas.hpp"
#include "ui/kit.hpp"
#include "ui/look.hpp"
#include "ui/png.hpp"

#include <SDL2/SDL.h>

#include <algorithm>
#include <deque>
#include <string>
#include <unistd.h>
#include <vector>

namespace play {

class AchievementToast {
public:
    static constexpr int width = 592, height = 104; // in the panel's 640x480

    ~AchievementToast() {
        if (texture_) SDL_DestroyTexture(texture_);
    }

    void add(AchievementNotice notice) {
        if (queue_.size() < 8) queue_.push_back(std::move(notice));
    }
    bool busy() const { return showing_ || !queue_.empty(); }

    // Over the panel at `area` (the renderer's own coordinates), after the panel's picture is drawn.
    void draw(SDL_Renderer* renderer, const SDL_Rect& area, const Look& look, const Font& font, Uint32 now) {
        if (!showing_) {
            if (queue_.empty()) return;
            current_ = std::move(queue_.front());
            queue_.pop_front();
            showing_ = true;
            started_ = now;
            badge_ready_ = false;
            badge_checked_ = 0;
            badge_px_.clear();
            drawn_ = false;
        }
        const Uint32 stay = current_.kind == AchievementNotice::Unlocked || current_.kind == AchievementNotice::Mastered
                                ? 5000
                                : 3500;
        const Uint32 slide = look.animations ? 260 : 0;
        const Uint32 age = now - started_;
        if (age >= stay + slide) {
            showing_ = false;
            return; // the next one starts on the next frame
        }
        // A badge still downloading is looked for again now and then, and drawn in once it is there.
        if (!badge_ready_ && !current_.badge.empty() && (badge_checked_ == 0 || now - badge_checked_ > 250) &&
            age < stay) {
            badge_checked_ = now;
            std::string error;
            int bw = 0, bh = 0;
            if (access(current_.badge.c_str(), R_OK) == 0 && read_png(current_.badge, badge_px_, bw, bh, error) &&
                bw > 0 && bh > 0) {
                badge_ready_ = true;
                badge_w_ = bw, badge_h_ = bh;
                drawn_ = false;
            }
        }
        if (!drawn_ || !texture_) render(renderer, look, font);
        if (!texture_) return;
        // Eased in from below the edge, and back down at the end.
        double shown = 1;
        if (slide) {
            if (age < slide) shown = double(age) / slide;
            else if (age > stay) shown = 1 - double(age - stay) / slide;
            shown = kit_ease(std::clamp(shown, 0.0, 1.0));
        }
        const double sx = area.w / 640.0, sy = area.h / 480.0;
        const int w = int(width * sx), h = int(height * sy), margin = int(12 * sy);
        const int rest = area.y + area.h - margin - h;
        const int y = int(area.y + area.h + (rest - (area.y + area.h)) * shown);
        SDL_Rect dest{area.x + (area.w - w) / 2, y, w, h};
        SDL_RenderCopy(renderer, texture_, nullptr, &dest);
    }

private:
    static double kit_ease(double t) { return 1 - (1 - t) * (1 - t) * (1 - t); }

    void render(SDL_Renderer* renderer, const Look& look, const Font& font) {
        Canvas c(width, height);
        // The corners stay see-through: every pixel still exactly this colour is made transparent below.
        const Rgb clear = look.background;
        c.fill(clear);
        const Box box{0, 0, width, height - 4};
        card(c, look, box, false, kit::radius);
        // The badge (64 x 64 from RetroAchievements), else a trophy.
        const int bx = 16, by = (box.h - 64) / 2;
        if (badge_ready_) {
            const int scale = std::max(1, 64 / std::max(badge_w_, badge_h_));
            c.rounded_rect(bx - 2, by - 2, 68, 68, 10, look.line);
            c.blit(badge_px_.data(), badge_w_, badge_h_, bx + (64 - badge_w_ * scale) / 2,
                   by + (64 - badge_h_ * scale) / 2, scale);
        } else {
            c.rounded_rect(bx, by, 64, 64, 12, look.slot);
            icon(c, look, Icon::Trophy, bx + 32, by + 32, 36, current_.kind == AchievementNotice::Problem ? look.muted : look.accent);
        }
        const int tx = bx + 64 + 16, right = box.w - 18;
        std::string heading;
        switch (current_.kind) {
        case AchievementNotice::Unlocked: heading = look.tr("Achievement unlocked"); break;
        case AchievementNotice::Mastered: heading = look.tr("Game mastered"); break;
        case AchievementNotice::GameReady:
        case AchievementNotice::NoSet: heading = look.tr("RetroAchievements"); break;
        case AchievementNotice::Problem: heading = look.tr("RetroAchievements"); break;
        }
        int heading_right = right;
        if (current_.points) {
            const std::string points = std::to_string(current_.points);
            const int pw = font.measure(points, kit::small) + 30;
            c.rounded_rect(right - pw, 12, pw, 26, 13, look.accent);
            icon(c, look, Icon::Star, right - pw + 13, 25, 14, look.on_accent);
            font.draw(c, right - pw + 24, font.centred(kit::small, 25), points, kit::small, look.on_accent);
            heading_right = right - pw - 8;
        }
        font.draw(c, tx, font.centred(kit::small, 25), font.fit(heading, kit::small, heading_right - tx), kit::small,
                  look.accent);
        const std::string title = current_.kind == AchievementNotice::Problem || current_.kind == AchievementNotice::NoSet
                                      ? look.tr(current_.detail)
                                      : current_.title;
        font.draw(c, tx, font.centred(kit::body, 52), font.fit(title, kit::body, right - tx), kit::body, look.ink);
        std::string detail;
        if (current_.kind == AchievementNotice::Unlocked) detail = current_.detail;
        else if (current_.kind == AchievementNotice::Mastered) detail = look.tr(current_.detail);
        else if (current_.kind == AchievementNotice::GameReady) detail = current_.detail + "  " + look.tr("achievements");
        else if (current_.kind == AchievementNotice::NoSet) detail = current_.title;
        if (!detail.empty())
            font.draw(c, tx, font.centred(kit::small, 78), font.fit(detail, kit::small, right - tx), kit::small,
                      look.muted);
        const uint32_t key = clear.px();
        for (int i = 0; i < width * height; ++i)
            if (c.data()[i] == key) c.data()[i] = 0;
        if (!texture_) {
            texture_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, width, height);
            if (texture_) {
                SDL_SetTextureBlendMode(texture_, SDL_BLENDMODE_BLEND);
                SDL_SetTextureScaleMode(texture_, SDL_ScaleModeLinear);
            }
        }
        if (texture_) SDL_UpdateTexture(texture_, nullptr, c.data(), width * 4);
        drawn_ = true;
    }

    std::deque<AchievementNotice> queue_;
    AchievementNotice current_;
    bool showing_ = false, drawn_ = false, badge_ready_ = false;
    Uint32 started_ = 0, badge_checked_ = 0;
    std::vector<uint32_t> badge_px_;
    int badge_w_ = 0, badge_h_ = 0;
    SDL_Texture* texture_ = nullptr;
};

} // namespace play
