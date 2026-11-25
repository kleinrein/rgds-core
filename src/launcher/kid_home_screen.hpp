#pragma once
// A Little kid's whole launcher (Page::KidHome): every game this profile may play, from every system, as big
// pictures. Six to a page on the lower screen, the chosen one large on the upper screen with its name. A tap or A
// plays it. The pages follow the finger and are thrown or dropped back, as on a phone. There are no menus, settings,
// search or switcher here, enforce_lock brings every other page back to this one, and no button leads anywhere: a
// grown-up holds the small picture at the bottom for a second (Who's playing?), then the owner's PIN.
// Members of App in main.cpp; included once, right after the class.

namespace kid_layout {
inline constexpr int per_page = 6, columns = 3, tile = 164, gap = 20, top = 22; // the lower screen's grid
inline constexpr int left = (640 - (columns * tile + (columns - 1) * gap)) / 2;
inline constexpr SDL_Rect back{24, 412, 112, 56}, next{504, 412, 112, 56}, who{292, 412, 56, 56};
inline constexpr Uint32 hold_ms = 1000, settle_ms = 280; // a grown-up's long press; a page landing
inline constexpr int page_width = 640;
} // namespace kid_layout

// The games in a fixed order, by name: a small child finds a game again by where it was. Kept until the library
// itself changes.
const std::vector<Game>& App::kid_home_games() {
    std::string stamp = std::to_string(all_games.size());
    if (!all_games.empty()) stamp += all_games.front().path.string() + "|" + all_games.back().path.string();
    if (stamp == kid_games_stamp) return kid_games;
    kid_games_stamp = stamp;
    std::vector<std::pair<std::string, const Game*>> sorted;
    for (const auto& game : all_games)
        if (!game.demo && !is_tool_entry(game.path)) sorted.emplace_back(lower(library_title(game.title)), &game);
    std::stable_sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    kid_games.clear();
    for (const auto& item : sorted) kid_games.push_back(*item.second);
    kid_focus = std::min(kid_focus, kid_games.empty() ? size_t(0) : kid_games.size() - 1);
    return kid_games;
}

// Opens it, or keeps it open. `reset` (a new player) starts on the game they played last.
void App::open_kid_home(bool reset) {
    const bool arriving = settings.page != Page::KidHome;
    settings.page = Page::KidHome;
    settings.focus = 0;
    home_switcher = false;
    picker_open = false;
    if (arriving || reset) {
        const auto& games = kid_home_games();
        if (reset) {
            kid_focus = 0;
            const auto& last = active_profile().last_path;
            for (size_t i = 0; !last.empty() && i < games.size(); i++)
                if (games[i].path == last) kid_focus = i;
        }
        kid_page_shown = int(kid_focus) / kid_layout::per_page;
        kid_moved_at = s.GetTicks();
    }
    dirty = true;
}

SDL_Rect App::kid_tile_box(int slot) const {
    namespace k = kid_layout;
    return {k::left + (slot % k::columns) * (k::tile + k::gap), k::top + (slot / k::columns) * (k::tile + k::gap),
            k::tile, k::tile};
}

int App::kid_page_count() {
    const int count = int(kid_home_games().size());
    return std::max(1, (count + kid_layout::per_page - 1) / kid_layout::per_page);
}

void App::kid_move_to(size_t index, int direction) {
    const auto& games = kid_home_games();
    if (games.empty() || index >= games.size() || index == kid_focus) return;
    const int page = int(index) / kid_layout::per_page;
    if (page != kid_page_shown) {
        // The new page comes in from the side it lies on, from wherever a finger let go of the old one.
        kid_slide_from = kid_slide_offset(s.GetTicks()) + direction * kid_layout::page_width;
        kid_slide_at = s.GetTicks();
        kid_page_shown = page;
        micro_until = std::max(micro_until, kid_slide_at + kid_layout::settle_ms + 20);
    }
    kid_focus = index;
    kid_moved_at = s.GetTicks();
    micro_until = std::max(micro_until, kid_moved_at + 200);
    emit_sound(MenuCue::Navigate, direction);
    dirty = true;
}

// A whole page on: the same place on it, or its last game. Round and round, so there is never a dead end.
void App::kid_page_step(int direction) {
    const auto& games = kid_home_games();
    const int pages = kid_page_count();
    if (games.empty() || pages < 2) return;
    const int page = (int(kid_focus) / kid_layout::per_page + direction + pages) % pages;
    const size_t slot = kid_focus % kid_layout::per_page;
    kid_move_to(std::min(games.size() - 1, size_t(page * kid_layout::per_page) + slot), direction);
}

void App::kid_play(size_t index) {
    const auto& games = kid_home_games();
    if (index >= games.size()) return;
    kid_focus = index;
    kid_pressed_at = s.GetTicks();
    emit_sound(MenuCue::Launch);
    play_game(Game(games[index])); // a copy: the list can be rebuilt while the game starts
}

// Where the page stands now: under the finger, or settling from where it was let go.
double App::kid_slide_offset(Uint32 now) const {
    if (kid_dragging) return kid_drag_dx;
    const Uint32 settle = kid_layout::settle_ms;
    if (!preferences.animations || now - kid_slide_at >= settle) return 0;
    const double t = double(now - kid_slide_at) / settle;
    return kid_slide_from * (1 - t) * (1 - t) * (1 - t);
}

void App::kid_home_action(int code) {
    const auto& games = kid_home_games();
    const int count = int(games.size()), focus = int(kid_focus);
    using kid_layout::columns;
    if (count == 0 || code == 7) return; // Select and Menu do nothing here: no button leads to a PIN
    else if (code == 2 || code == 4) kid_play(kid_focus);
    else if (code == 13 || code == 14) kid_page_step(code == 13 ? -1 : 1);
    else if (code == 0 || code == 1) kid_move_to(size_t((focus + (code == 0 ? -1 : 1) + count) % count), code ? 1 : -1);
    else if (code == 8 || code == 9) {
        // Up and down stay in the column; past the top or bottom row they turn the page.
        const int slot = focus % kid_layout::per_page, row = slot / columns;
        if (code == 8 && row > 0) kid_move_to(size_t(focus - columns), -1);
        else if (code == 9 && row == 0 && focus + columns < count) kid_move_to(size_t(focus + columns), 1);
        else if (code == 9 && row == 0 && focus / kid_layout::per_page == (count - 1) / kid_layout::per_page &&
                 count % kid_layout::per_page > columns)
            kid_move_to(size_t(count - 1), 1); // nothing straight below: the last one
        else kid_page_step(code == 8 ? -1 : 1);
    }
}

void App::kid_home_tap(int x, int y) {
    const auto& games = kid_home_games();
    if (point_in(kid_layout::who, x, y)) return; // held, not tapped (kid_hold_tick)
    if (kid_page_count() > 1 && point_in(kid_layout::back, x, y)) {
        kid_page_step(-1);
        return;
    }
    if (kid_page_count() > 1 && point_in(kid_layout::next, x, y)) {
        kid_page_step(1);
        return;
    }
    for (int slot = 0; slot < kid_layout::per_page; slot++) {
        const size_t index = size_t(kid_page_shown * kid_layout::per_page + slot);
        const auto box = kid_tile_box(slot);
        if (index < games.size() && point_in({box.x - 8, box.y - 8, box.w + 16, box.h + 16}, x, y)) {
            // One tap plays: little fingers do not double-tap.
            kid_play(index);
            return;
        }
    }
}

void App::kid_home_press(int x, int y) {
    const Uint32 now = s.GetTicks();
    // A finger on a page still landing catches it where it is.
    kid_drag_dx = kid_slide_offset(now);
    kid_dragging = std::abs(kid_drag_dx) > 1;
    kid_slide_at = 0;
    kid_grab_x = x - int(std::lround(kid_drag_dx));
    kid_last_x = x;
    kid_last_at = now;
    kid_velocity = 0;
    kid_hold_fired = false;
    (void)y;
}

// A finger moving: once it is plainly sideways the page is in its hand, and follows it exactly.
void App::kid_home_drag(int x, int y) {
    const Uint32 now = s.GetTicks();
    const int dx = x - kid_grab_x;
    if (!kid_dragging && std::abs(x - down_x) >= 14 && std::abs(x - down_x) > std::abs(y - down_y)) kid_dragging = true;
    if (kid_dragging) {
        // One page has nowhere to go: it gives a little and comes back.
        kid_drag_dx = kid_page_count() > 1 ? dx : dx * 0.3;
        if (now > kid_last_at) {
            const double sample = double(x - kid_last_x) / double(now - kid_last_at);
            kid_velocity = kid_velocity * 0.4 + sample * 0.6; // px per ms, the last moves weighed most
        }
        micro_until = std::max(micro_until, now + 60);
    }
    kid_last_x = x;
    kid_last_at = now;
}

// A finger lifted: a page dragged far enough or thrown goes; one that is not drops back. A short touch is a tap.
void App::kid_home_release(int x, int y) {
    const Uint32 now = s.GetTicks();
    if (kid_hold_fired) return;
    if (kid_dragging) {
        // Decided from the last place the finger was seen moving: a lift reports no movement of its own.
        const double dx = kid_drag_dx,
                     speed = now - kid_last_at > 90 ? 0 : kid_velocity; // a pause before lifting stops a throw
        int direction = 0;
        if (kid_page_count() > 1) {
            if (std::abs(speed) >= 0.35) direction = speed < 0 ? 1 : -1;
            else if (std::abs(dx) >= kid_layout::page_width / 4) direction = dx < 0 ? 1 : -1;
        }
        kid_slide_from = dx;
        kid_slide_at = now;
        kid_dragging = false;
        if (direction) {
            kid_slide_at = now;
            kid_page_step(direction); // starts from where it was let go
        } else micro_until = std::max(micro_until, now + kid_layout::settle_ms + 20);
        dirty = true;
        return;
    }
    if (std::abs(x - down_x) < 28 && std::abs(y - down_y) < 28) touch(down_x, down_y);
}

// A grown-up's long press: the small picture on the wall (Who's playing?), the lock in the game menu (the whole
// menu, behind the owner's PIN). A ring fills while it is held; letting go early does nothing.
SDL_Rect App::kid_hold_box() const {
    return settings.page == Page::KidMenu ? SDL_Rect{566, 432, 44, 40} : kid_layout::who;
}

double App::kid_hold_progress(Uint32 now) const {
    if (!pointer_down || kid_dragging || kid_hold_fired || !point_in(kid_hold_box(), down_x, down_y) ||
        !point_in(kid_hold_box(), contact_x, contact_y))
        return 0;
    return std::min(1.0, double(now - pointer_at) / kid_layout::hold_ms);
}

void App::kid_hold_tick(Uint32 now) {
    if (settings.page != Page::KidHome && settings.page != Page::KidMenu) return;
    const double held = kid_hold_progress(now);
    if (held > 0) micro_until = std::max(micro_until, now + 40);
    if (held < 1) return;
    kid_hold_fired = true;
    kid_swallow_release = true; // the finger still down must not tap the screen this opens
    emit_sound(MenuCue::Open);
    if (settings.page == Page::KidHome) open_profile_picker(false);
    else if (owner_pin().empty()) {
        settings_unlocked = true;
        settings.page = Page::QuickMenu;
        settings.focus = 0;
    } else {
        pin_target_page = Page::QuickMenu;
        pin_target_focus = 0;
        open_pin(PinPurpose::Unlock, owner_pin());
    }
    dirty = true;
}

void App::kid_hold_ring(SDL_Renderer* r, int y) {
    const double held = kid_hold_progress(s.GetTicks());
    if (held <= 0) return;
    const auto box = kid_hold_box();
    smooth_arc(r, box.x + box.w / 2, y + box.y + box.h / 2, std::max(box.w, box.h) / 2 + 6, 5, 0, held,
               theme.at("accent"));
}

// A solid triangle pointing left (-1) or right (1), its tip rounded off by drawing it as spans: arrows and Play.
void App::kid_triangle(SDL_Renderer* r, int cx, int cy, int size, int direction, Color fill) {
    s.SetRenderDrawColor(r, fill.r, fill.g, fill.b, 255);
    const int half = size / 2, depth = size * 7 / 8;
    for (int row = -half; row <= half; row++) {
        const int length = depth * (half - std::abs(row)) / std::max(1, half);
        if (length <= 0) continue;
        const int start = direction > 0 ? cx - depth / 3 : cx + depth / 3 - length;
        SDL_Rect span{start, cy + row, length, 1};
        s.RenderFillRect(r, &span);
    }
}

// One game's picture: its cover, or its cartridge for a game with none yet. Cards with a white rim, like stickers.
void App::kid_cover(SDL_Renderer* r, const Game& game, SDL_Rect box, bool focused, bool urgent) {
    const auto path = folder_for_game(game) / "box.bmp";
    media.request(r, path, false, urgent);
    const int radius = std::max(10, box.w / 9);
    const Color accent = theme.at("accent");
    if (focused) {
        alpha_round(r, box.x - 9, box.y - 5, box.w + 18, box.h + 18, radius + 9, accent, 90);
        fill_round(r, box.x - 7, box.y - 7, box.w + 14, box.h + 14, radius + 7, accent);
    } else alpha_round(r, box.x - 2, box.y + 4, box.w + 4, box.h + 4, radius + 2, shadow_colour(), 60);
    fill_round(r, box.x - 2, box.y - 2, box.w + 4, box.h + 4, radius + 2, {255, 255, 255});
    const SDL_Rect inner{box.x + 4, box.y + 4, box.w - 8, box.h - 8};
    round_rect(r, inner.x, inner.y, inner.w, inner.h, radius - 3, "panel");
    if (!media.draw(r, path, inner, false, 255, true, radius - 3)) {
        const int size = inner.w * 3 / 4;
        cartridge_icon(r, game, inner.x + (inner.w - size) / 2, inner.y + (inner.h - size) / 2, size);
    }
}

void App::kid_home_top(SDL_Renderer* r, int y) {
    rect(r, 0, y, 640, 480, "background");
    pattern(r, y + 48, 432);
    status_bar(r, y);
    const auto& games = kid_home_games();
    const auto& who = active_profile();
    if (games.empty()) {
        draw_profile_tile(r, who, 320 - 90, y + 110, 180);
        const std::string text = locale.text("No games yet");
        raw_label(r, text, 320 - label_width(text, 30, false) / 2, y + 340, 30, "ink", 600);
        return;
    }
    const Game& game = games[std::min(kid_focus, games.size() - 1)];
    // The chosen game, big, with a small hop each time it changes.
    const Uint32 now = s.GetTicks();
    const double t = preferences.animations ? std::clamp((now - kid_moved_at) / 200.0, 0.0, 1.0) : 1;
    const int hop = int(std::lround(std::sin(t * 3.14159) * 10)), size = 272;
    kid_cover(r, game, {320 - size / 2, y + 68 - hop, size, size}, false, true);
    const std::string title = library_title(game.title);
    const int text = label_width(title, 30, false) > 600 ? 24 : 30;
    raw_label(r, title, 320 - std::min(600, label_width(title, text, false)) / 2, y + 372, text, "ink", 600);
    // Who is playing, small, in the corner; the play button a picture, not a word.
    Profile face = who;
    face.restricted = false;
    draw_profile_tile(r, face, 24, y + 64, 56);
    const Color accent = theme.at("accent");
    raised_pill(r, 320 - 40, y + 414, 80, 50, 25, accent);
    kid_triangle(r, 322, y + 438, 24, 1, tile_ink(accent));
}

void App::kid_home_bottom(SDL_Renderer* r, int y) {
    rect(r, 0, y, 640, 480, "background");
    pattern(r, y, 480, true);
    const auto& games = kid_home_games();
    const Uint32 now = s.GetTicks();
    if (games.empty()) {
        draw_profile_tile(r, active_profile(), kid_layout::who.x, y + kid_layout::who.y, kid_layout::who.w);
        return;
    }
    // The page under the finger, and its neighbour on the side it is pulled from (round and round).
    const int pages = kid_page_count();
    const double slide = kid_slide_offset(now);
    const bool pressing =
        pointer_down && !kid_dragging && std::abs(contact_x - down_x) < 28 && std::abs(contact_y - down_y) < 28;
    std::vector<std::pair<int, int>> shown = {{kid_page_shown, int(std::lround(slide))}};
    if (pages > 1 && std::abs(slide) >= 1) {
        const int side = slide > 0 ? -1 : 1;
        shown.push_back(
            {(kid_page_shown + side + pages) % pages, int(std::lround(slide)) + side * kid_layout::page_width});
    }
    for (const auto& [page, offset] : shown)
        for (int slot = 0; slot < kid_layout::per_page; slot++) {
            const size_t index = size_t(page * kid_layout::per_page + slot);
            if (index >= games.size()) break;
            auto box = kid_tile_box(slot);
            box.x += offset;
            if (box.x + box.w < -16 || box.x > 656) continue;
            box.y += y;
            const bool focused = index == kid_focus;
            const bool pressed = pressing && point_in(kid_tile_box(slot), down_x, down_y);
            if (focused && preferences.animations && now - kid_moved_at < 160) {
                const int grow = int(std::lround(8 * std::sin((now - kid_moved_at) / 160.0 * 3.14159)));
                box = {box.x - grow / 2, box.y - grow / 2, box.w + grow, box.h + grow};
            }
            if (pressed) box = {box.x + 6, box.y + 8, box.w - 12, box.h - 12};
            kid_cover(r, games[index], box, focused, focused);
        }
    // Where we are: a dot per page, the current one long; past sixteen pages a bar.
    if (pages > 1) {
        const Color accent = theme.at("accent"), line = theme.at("line");
        if (pages <= 16) {
            const int width = pages * 16 + 14, start = 320 - width / 2;
            for (int i = 0, x = start; i < pages; i++) {
                const int w = i == kid_page_shown ? 26 : 12;
                fill_round(r, x, y + 386, w, 12, 6, i == kid_page_shown ? accent : line);
                x += w + 4;
            }
        } else {
            fill_round(r, 170, y + 388, 300, 8, 4, line);
            const int at = 170 + (300 - 40) * kid_page_shown / (pages - 1);
            fill_round(r, at, y + 386, 40, 12, 6, accent);
        }
        for (int side = 0; side < 2; side++) {
            const SDL_Rect button = side == 0 ? kid_layout::back : kid_layout::next;
            const bool down = pressing && point_in(button, down_x, down_y);
            raised_pill(r, button.x, y + button.y + (down ? 2 : 0), button.w, button.h, button.h / 2,
                        theme.at("panel"));
            kid_triangle(r, button.x + button.w / 2, y + button.y + button.h / 2 + (down ? 2 : 0), 24,
                         side == 0 ? -1 : 1, accent);
        }
    }
    Profile face = active_profile();
    face.restricted = false; // their own face, without the Kids mark
    draw_profile_tile(r, face, kid_layout::who.x, y + kid_layout::who.y, kid_layout::who.w);
    kid_hold_ring(r, y);
}

// ---- In a game: Play and Home ----------------------------------------------------------------------------------------
// The game menu a Little kid sees (Page::KidMenu, in `rgds-launcher --game-menu`): two big buttons over the paused
// game. Play goes back to it; Home leaves it as Return home does, saving as the profile's auto-save says, and the
// launcher opens on their games again. B, Select and the Menu button play on. A grown-up holds the small lock in the
// corner for a second: the owner's PIN, then the whole game menu (straight to it when the owner has no PIN).

namespace kid_layout {
inline constexpr SDL_Rect play{28, 36, 584, 232}, home{28, 292, 584, 132};
} // namespace kid_layout

void App::kid_menu_choose(int choice) {
    if (menu_status["state"] == "busy") return;
    settings.focus = choice;
    kid_pressed_at = s.GetTicks();
    if (choice == 0) menu_request("resume");
    else if (menu_status["state"] == "error") {
        settings.page = Page::LeaveGame; // the game could not be saved: the grown-up choices, as for anyone
        settings.focus = 0;
    } else menu_request("quit");
    dirty = true;
}

void App::kid_menu_action(int code) {
    if (code == 3 || code == 7) kid_menu_choose(0); // B, and Menu or Select (one code here): back to the game
    else if (code == 8 || code == 9 || code == 0 || code == 1) {
        const int next = code == 8 || code == 0 ? 0 : 1;
        if (next != settings.focus) {
            settings.focus = next;
            kid_moved_at = s.GetTicks();
            emit_sound(MenuCue::Navigate, next ? 1 : -1);
        }
    } else if (code == 2 || code == 4) kid_menu_choose(settings.focus);
    dirty = true;
}

void App::kid_menu_tap(int x, int y) {
    if (point_in(kid_layout::play, x, y)) kid_menu_choose(0);
    else if (point_in(kid_layout::home, x, y)) kid_menu_choose(1);
}

void App::kid_menu_top(SDL_Renderer* r, int y) {
    // The paused game itself, with a pause sign on it: nothing to read.
    rect(r, 0, y, 640, 480, "background");
    media.draw(r, o.data / "session/menu-background.bmp", {0, y + 48, 640, 432}, false, 150);
    status_bar(r, y);
    alpha_round(r, 320 - 70, y + 196 - 64, 140, 140, 70, shadow_colour(), 70);
    fill_round(r, 320 - 66, y + 196 - 70, 132, 132, 66, {255, 255, 255});
    const Color accent = theme.at("accent");
    fill_round(r, 320 - 30, y + 196 - 34, 20, 64, 6, accent);
    fill_round(r, 320 + 10, y + 196 - 34, 20, 64, 6, accent);
    if (!model.games.empty()) {
        const std::string title = library_title(model.games[0].title);
        const int text = label_width(title, 30, false) > 560 ? 24 : 30,
                  width = std::min(560, label_width(title, text, false));
        round_rect(r, 320 - width / 2 - 24, y + 360, width + 48, 64, 32, "panel");
        raw_label(r, title, 320 - width / 2, centred_label_y(title, text, y + 392), text, "ink", 560);
    }
}

void App::kid_menu_bottom(SDL_Renderer* r, int y) {
    rect(r, 0, y, 640, 480, "background");
    pattern(r, y, 480, true);
    const bool busy = menu_status["state"] == "busy";
    const bool pressing = pointer_down && std::abs(contact_x - down_x) < 28 && std::abs(contact_y - down_y) < 28;
    const Uint32 now = s.GetTicks();
    for (int i = 0; i < 2; i++) {
        SDL_Rect box = i == 0 ? kid_layout::play : kid_layout::home;
        const bool focused = settings.focus == i, pressed = pressing && point_in(box, down_x, down_y);
        const Color fill = i == 0 ? Color{46, 158, 68} : Color{245, 124, 0}, ink = {255, 255, 255};
        box.y += y;
        if (focused && preferences.animations && now - kid_moved_at < 160) {
            const int grow = int(std::lround(10 * std::sin((now - kid_moved_at) / 160.0 * 3.14159)));
            box = {box.x - grow / 2, box.y - grow / 2, box.w + grow, box.h + grow};
        }
        if (focused) fill_round(r, box.x - 8, box.y - 8, box.w + 16, box.h + 16, 34, theme.at("ink"));
        raised_pill(r, box.x, box.y + (pressed ? 4 : 0), box.w, box.h, 28, fill, pressed ? 1 : 0);
        const int cy = box.y + box.h / 2 + (pressed ? 4 : 0);
        const std::string word = locale.text(i == 0 ? "Play" : "Home");
        const int size = i == 0 ? 48 : 36, glyph = i == 0 ? 72 : 52;
        const int width = glyph + 28 + label_width(word, size, false), left = box.x + (box.w - width) / 2;
        if (i == 0) kid_triangle(r, left + glyph / 2 + 6, cy, glyph, 1, ink);
        else {
            // A house: a roof over a square with a door, in the button's own white.
            const int hx = left, hy = cy - glyph / 2;
            s.SetRenderDrawColor(r, ink.r, ink.g, ink.b, 255);
            for (int row = 0; row < glyph / 2; row++) {
                SDL_Rect span{hx + glyph / 2 - row, hy + row, row * 2, 1};
                s.RenderFillRect(r, &span);
            }
            fill_round(r, hx + glyph / 6, hy + glyph / 2 - 2, glyph * 2 / 3, glyph / 2 + 2, 4, ink);
            fill_round(r, hx + glyph / 2 - glyph / 10, hy + glyph * 3 / 4, glyph / 5, glyph / 4, 3, fill);
        }
        raw_label_colour(r, word, left + glyph + 28, centred_label_y(word, size, cy), size, ink, 400);
    }
    // The grown-up's lock: small, in the corner, and it has to be held.
    const auto lock = kid_hold_box();
    fill_round(r, lock.x, y + lock.y, lock.w, lock.h, lock.h / 2, theme.at("panel"));
    ui_icon(r, SettingsIcon::Key, lock.x + lock.w / 2, y + lock.y + lock.h / 2, 18, "muted");
    kid_hold_ring(r, y);
    if (busy) {
        // Saving: the buttons wait, under a veil.
        s.SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        s.SetRenderDrawColor(r, 0, 0, 0, 90);
        SDL_Rect veil{0, y, 640, 480};
        s.RenderFillRect(r, &veil);
        s.SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    }
}
