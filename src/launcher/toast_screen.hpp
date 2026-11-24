#pragma once
// Toasts on the lower screen (the model is toast.hpp). Every notice, warning and error the launcher has to say goes
// through here: notify() for a page's own words (it also keeps service_notice for the pages and tests that read it),
// toast() for anything else, and watch_notices() for what workers and helpers write to their status files.

namespace {
// The four looks. Info takes the theme's accent so it belongs to whichever colour the player picked.
Color toast_hue(toast::Severity severity, Color accent) {
    switch (severity) {
    case toast::Severity::Success: return Color{38, 166, 91};
    case toast::Severity::Warning: return Color{230, 145, 20};
    case toast::Severity::Error: return Color{214, 58, 58};
    default: return accent;
    }
}
} // namespace

void App::toast(const std::string& source, toast::Severity severity, bool translate) {
    if (source.empty()) return;
    toasts.animations = preferences.animations;
    toasts.post(translate ? locale.text(source) : source, severity, s.GetTicks());
    dirty = true;
}

void App::toast(const std::string& source) { toast(source, toast::infer(source)); }

void App::notify(const std::string& source) {
    service_notice = source;
    toast(source);
}

void App::notify(const std::string& source, toast::Severity severity) {
    service_notice = source;
    toast(source, severity);
}

std::vector<std::string> App::toast_lines(const std::string& value, int pixels, int width, int rows) {
    auto measure = [&](const std::string& str) {
        return preferences.pixel ? float(pixel_cells(str) * std::max(2, (pixels + 6) / 10) * 6)
                                 : rounded.width(str, pixels);
    };
    std::vector<std::string> lines;
    std::string line;
    for (const auto& piece : line_pieces(value)) {
        std::string candidate = line.empty() ? piece.text : line + (piece.space_before ? " " : "") + piece.text;
        if (!line.empty() && measure(candidate) > width) {
            lines.push_back(line);
            line = piece.text;
        } else line = candidate;
    }
    if (!line.empty()) lines.push_back(line);
    if (int(lines.size()) > rows) lines.resize(size_t(rows)); // the last kept line ellipsises when drawn
    return lines;
}

namespace {
constexpr int toast_text = 18, toast_line = 24, toast_pad = 14, toast_badge = 30, toast_max_width = 584, toast_top = 12;
} // namespace

SDL_Rect App::toast_box(const toast::Card& card, int y) {
    const int width = std::min(toast_max_width, card.height ? toast_widths[card.id] : toast_max_width);
    return SDL_Rect{320 - width / 2, y + int(std::lround(card.shown_y)), width, card.height};
}

void App::draw_toasts(SDL_Renderer* r, int y) {
    const Uint32 now = s.GetTicks();
    toasts.animations = preferences.animations;
    toasts.tick(now);
    toast_widths.clear();
    if (toasts.empty()) return;
    // Lay out each card's words first: its height sets where the ones under it go.
    const int text_room = toast_max_width - toast_pad * 2 - toast_badge - 14;
    std::vector<std::vector<std::string>> lines(toasts.cards.size());
    for (size_t i = 0; i < toasts.cards.size(); i++) {
        auto& card = toasts.cards[i];
        lines[i] = toast_lines(card.text, toast_text, text_room, 3);
        int widest = 0;
        for (const auto& line : lines[i]) widest = std::max(widest, label_width(line, toast_text, false));
        toast_widths[card.id] = std::clamp(widest + toast_pad * 2 + toast_badge + 16, 240, toast_max_width);
        card.height = toast_pad * 2 + int(lines[i].size()) * toast_line + 4;
    }
    toasts.layout(toast_top);
    const Uint32 dt = toast_drawn_at ? std::min<Uint32>(100, now - toast_drawn_at) : 16;
    toast_drawn_at = now;
    toasts.glide(dt);

    const Color accent = theme.at("accent"), panel = theme.at("panel"), line_colour = theme.at("line"),
                ink = theme.at("ink"), white{255, 255, 255};
    const bool dark = preferences.dark;
    // Oldest first, so the newest card is drawn over the others while they shuffle.
    for (size_t n = toasts.cards.size(); n-- > 0;) {
        const auto& card = toasts.cards[n];
        const Uint32 age = now - card.born;
        double alpha = 1, lift = 0, dx = 0, squeeze = 0;
        if (toasts.animations) {
            alpha = toast::enter_alpha(age);
            lift = -(card.height + toast_top + 18) * toast::enter_offset(age);
            if (card.nudged) dx = toast::nudge_offset(now - card.nudged);
            if (card.left) {
                const double k = toast::leave_progress(now - card.left);
                alpha *= 1 - k;
                lift -= 22 * k;
                squeeze = k;
            }
        } else if (card.left) continue;
        if (alpha <= 0.01) continue;
        SDL_Rect box = toast_box(card, y);
        const int shrink = int(std::lround(box.w * .05 * squeeze));
        box.x += shrink / 2 + int(std::lround(dx));
        box.w -= shrink;
        box.y += int(std::lround(lift));
        const auto a = [&](double value) { return int(std::lround(value * alpha)); };
        const Color hue = toast_hue(card.severity, accent);

        // A soft two-step shadow, a fine rim in the card's hue, then the body with a whisper of that hue in it.
        const Color shadow = shadow_colour();
        alpha_round(r, box.x - 3, box.y + 2, box.w + 6, box.h + 10, 20, shadow, a(dark ? 50 : 22));
        alpha_round(r, box.x - 1, box.y + 3, box.w + 2, box.h + 3, 18, shadow, a(dark ? 70 : 34));
        alpha_round(r, box.x - 1, box.y - 1, box.w + 2, box.h + 2, 17, blend(line_colour, hue, 55), a(255));
        alpha_round(r, box.x, box.y, box.w, box.h, 16, blend(panel, hue, dark ? 14 : 7), a(252));
        // Glass along the top edge, as the DSi's cards have.
        alpha_round(r, box.x + 6, box.y + 1, box.w - 12, 1, 0, white, a(dark ? 26 : 120));

        // The badge: a round of the hue with a highlight, and its mark in white.
        const int bx = box.x + toast_pad + toast_badge / 2, by = box.y + toast_pad + 2 + toast_line / 2;
        alpha_round(r, bx - toast_badge / 2, by - toast_badge / 2 + 1, toast_badge, toast_badge, toast_badge / 2,
                    blend(hue, Color{0, 0, 0}, 30), a(255));
        alpha_round(r, bx - toast_badge / 2, by - toast_badge / 2, toast_badge, toast_badge, toast_badge / 2, hue,
                    a(255));
        alpha_round(r, bx - 10, by - 13, 20, 9, 5, white, a(60));
        s.SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        s.SetRenderDrawColor(r, 255, 255, 255, Uint8(a(255)));
        auto dot = [&](int px, int py, int w, int h) {
            SDL_Rect rect{px, py, w, h};
            s.RenderFillRect(r, &rect);
        };
        switch (card.severity) {
        case toast::Severity::Success:
            for (int i = 0; i <= 5; i++) dot(bx - 7 + i, by - 1 + i, 3, 3);
            for (int i = 0; i <= 9; i++) dot(bx - 2 + i, by + 4 - i, 3, 3);
            break;
        case toast::Severity::Warning:
            dot(bx - 2, by - 9, 4, 12);
            dot(bx - 2, by + 5, 4, 4);
            break;
        case toast::Severity::Error:
            for (int i = -6; i <= 6; i++) {
                dot(bx + i - 1, by + i - 1, 3, 3);
                dot(bx + i - 1, by - i - 1, 3, 3);
            }
            break;
        default:
            dot(bx - 2, by - 9, 4, 4);
            dot(bx - 2, by - 3, 4, 12);
            break;
        }

        // The words. The rounded font fades with the card; the classic dots wait until the card is mostly there.
        if (!preferences.pixel || alpha > .6) {
            rounded.opacity = a(255);
            const int tx = box.x + toast_pad + toast_badge + 14, tw = box.x + box.w - toast_pad - tx;
            for (size_t i = 0; i < lines[n].size(); i++) {
                const int cy = box.y + toast_pad + 2 + toast_line / 2 + int(i) * toast_line;
                raw_label_colour(r, lines[n][i], tx, centred_label_y(lines[n][i], toast_text, cy), toast_text, ink, tw);
            }
            rounded.opacity = 255;
        }
    }
    s.SetRenderDrawBlendMode(r, o.game_menu ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
}

bool App::toasts_animating(Uint32 now) const { return toasts.moving(now); }

// A tap on a card sends it away; the rest of that touch goes nowhere else.
bool App::toast_touch(int x, int y) {
    const Uint32 now = s.GetTicks();
    for (const auto& card : toasts.cards) {
        if (card.left || !card.height) continue;
        SDL_Rect box = toast_box(card, 0);
        box.x -= 6;
        box.y -= 6;
        box.w += 12;
        box.h += 12;
        if (point_in(box, x, y)) {
            toasts.dismiss(card.id, now);
            dirty = true;
            return true;
        }
    }
    return false;
}

// A worker that just ended in an error says so: the device helper (Wi-Fi, Bluetooth, USB, sharing), RetroAchievements
// and the artwork service each write the reason to their status file. Tied to the finish, so the same error after
// another try shows again. Called from poll_service once the files are read.
void App::worker_finished_toast(Uint32 now) {
    if (worker > 0 || worker_finished_at != now || worker_ok) return;
    std::string message;
    if (worker_action.rfind("device.", 0) == 0) {
        if (worker_action == "device.info" || worker_action == "device.startup-info") return; // background refreshes
        message = device_status["message"];
    } else if (worker_action.rfind("achievements.", 0) == 0) message = achievements_status["message"];
    else if (worker_action == "scrape" || worker_action == "login" || worker_action == "logout")
        message = service["message"];
    if (message.empty()) message = "This request did not finish. Please try again.";
    // A failed request is at least an error, unless its words ask for a step first ("Connect … first").
    toast(message,
          toast::infer(message) == toast::Severity::Warning ? toast::Severity::Warning : toast::Severity::Error);
}

// What changes outside a worker's finish: the launcher's own status line, the updater, the in-game menu's bridge
// and Controls' capture. Each becomes a toast the moment it changes. The first look only takes note, so a status
// file left from before the launcher started is not announced.
void App::watch_notices() {
    auto fire = [&](std::string& seen, const std::string& signature, const std::string& text,
                    std::optional<toast::Severity> severity = std::nullopt) {
        if (signature == seen) return;
        seen = signature;
        if (signature.empty() || text.empty() || !notice_seen.primed) return;
        toast(text, severity ? *severity : toast::infer(text));
    };
    // The launcher's own status line: "Settings saved", "Could not apply volume", a missing BIOS. Words still
    // under way ("Preparing…") are not news.
    const bool quiet_status = status.empty() || status == "READY" ||
                              (status.size() >= 3 && status.compare(status.size() - 3, 3, "\u2026") == 0);
    const bool primed = notice_seen.primed;
    notice_seen.primed = true; // the status line was set by this session, so it speaks from the start
    fire(notice_seen.status, quiet_status ? "" : status, status);
    notice_seen.primed = primed;
    const bool update_error = update_status["state"] == "error" && !update_status["message"].empty();
    fire(notice_seen.update, update_error ? update_status["message"] : "", update_status["message"],
         toast::Severity::Error);
    const bool menu_error = o.game_menu && menu_status["state"] == "error" && !menu_status["message"].empty();
    fire(notice_seen.menu, menu_error ? menu_status["message"] : "", menu_status["message"], toast::Severity::Error);
    const bool controls_problem =
        controls_notice.rfind("Could not", 0) == 0 || controls_notice.rfind("No button detected", 0) == 0;
    fire(notice_seen.controls, controls_problem ? controls_notice : "", controls_notice);
    notice_seen.primed = true;
}
