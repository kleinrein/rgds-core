#pragma once
// Library zoom (library_zoom.hpp): a finger on each screen, pinched toward the hinge, zooms the library out to a
// wall of covers and then a dense wall of icons; spreading them zooms back in. Every tile travels between its place
// in the library as it is and its place on the wall, so the zoom follows the fingers. Nothing else changes: at
// level 0 the library draws and behaves exactly as before.
// Members of App in main.cpp; included once, right after the class.

void App::open_top_touch() {
#ifdef __linux__
    const char* configured = std::getenv("RGDS_TOP_TOUCH_DEV");
    std::string device = configured ? configured : "";
    if (device == "none") return;
    // The upper panel's own Goodix, as RG DS Play finds it (play/src/frontend/main.cpp).
    std::error_code error;
    if (device.empty())
        for (const auto& entry : fs::directory_iterator("/sys/class/input", error)) {
            const auto name = entry.path().filename().string();
            std::error_code resolve;
            if (name.rfind("event", 0) == 0 &&
                fs::canonical(entry.path() / "device", resolve).string().find("fe5c0000") != std::string::npos)
                device = "/dev/input/" + name;
        }
    if (device.empty() || device == touchscreen.path()) return;
    if (top_touch.open_device(device)) {
        top_touch.hold_initial_contact();
        std::cout << "Top touch: " << top_touch.path() << " (library zoom)" << std::endl;
    }
#endif
}

bool App::zoom_allowed() {
    return !probe && !launching && departure_action.empty() && settings.page == Page::Library && !home_menu &&
           !systems_menu && !picker_open && !filter_sheet_active() && !chat_open && !music_open && !vt.open &&
           !sn.open && !o.port_menu && !o.game_menu && model.games.size() > 1;
}

// The game the wall is about: the selection when it shows on the wall at rest, else the game in its middle.
size_t App::zoom_focus() {
    const size_t count = model.games.size();
    if (!count) return 0;
    const int level = int(std::lround(std::clamp(zoom.level, 0.0, double(LibraryZoom::deepest))));
    if (level == 0) return model.selected;
    const auto wall = ZoomWall::at(level);
    const int scroll = zoom_scroll[size_t(level)], y = wall.y(model.selected, scroll);
    if (y + wall.tile > ZoomWall::top && y < ZoomWall::bottom) return model.selected;
    const int middle = wall.hit(320, ZoomWall::top + ZoomWall::height / 2, scroll, count);
    return middle >= 0 ? size_t(middle) : std::min(count - 1, size_t(wall.first(scroll)));
}

// Every wall but the one showing centres on the focus, so the zoom lands around it.
void App::zoom_centre() {
    const size_t count = model.games.size(), focus = zoom_focus();
    const int showing = int(std::lround(std::clamp(zoom.level, 0.0, double(LibraryZoom::deepest))));
    for (int level = 1; level <= LibraryZoom::deepest; level++)
        if (level != showing) zoom_scroll[size_t(level)] = ZoomWall::at(level).centred(focus, count);
    zoom_revealed = model.selected;
    zoom_motion.stop();
}

void App::zoom_go(int level, Uint32 now) {
    const int before = zoom.target;
    zoom_centre();
    zoom_finger = false;
    zoom.go(level, now, preferences.animations);
    if (zoom.target != before) {
        haptics.play(zoom.target > before ? Haptic::Open : Haptic::Back);
        sound_intent(zoom.target > before ? MenuCue::Open : MenuCue::Back);
    }
    dirty = true;
}

// After either finger moved: start, follow or end the pinch.
void App::zoom_pinch_step(bool changed, Uint32 now) {
    if (changed && cross_pinch.both()) {
        if (!zoom_allowed()) return;
        if (pointer_down) cancel_navigation(); // the lower finger's tap or drag gives way to the pinch
        zoom_owned = true;
        zoom_finger = false;
        zoom_centre();
        zoom.pinch_begin();
        dirty = true;
    } else if (changed && zoom.pinching) {
        const int before = int(std::lround(zoom.base)), to = zoom.release(now, preferences.animations);
        if (to != before) {
            haptics.play(to > before ? Haptic::Open : Haptic::Back);
            sound_intent(to > before ? MenuCue::Open : MenuCue::Back);
        }
        zoom_revealed = model.selected;
        dirty = true;
    } else if (zoom.pinching && cross_pinch.both()) {
        const double previous = zoom.level;
        zoom.pinch(cross_pinch.levels());
        // A detent each time the zoom passes a level, as the lists give one a row.
        if (std::floor(previous + .5) != std::floor(zoom.level + .5)) haptic_detent();
        if (previous != zoom.level) dirty = true;
    }
}

void App::zoom_top_touch(int kind, int x, int y, Uint32 now) {
    last_activity = now;
    // Snake steers from the upper screen too; on Home the battery hides the way into it.
    if (sn.open) {
        snake_top_touch(kind, std::clamp(x, 0, 639), std::clamp(y, 0, 479));
        return;
    }
    if (snake_secret_tap(kind, x, y, now)) return;
    if (kind == 0 && !zoom_allowed()) return; // not followed: its moves and lift change nothing
    zoom_pinch_step(cross_pinch.touch(true, kind, std::clamp(x, 0, 639), std::clamp(y, 0, 479)), now);
}

// Every lower contact passes here first; true when it belongs to the pinch rather than to the page.
bool App::zoom_bottom_touch(int kind, int x, int y, Uint32 now) {
    const bool owned = zoom_owned;
    zoom_pinch_step(cross_pinch.touch(false, kind, x, y), now);
    const bool consumed = owned || zoom_owned;
    if (kind == 2 || kind == 3) zoom_owned = false;
    if (consumed) last_activity = now;
    return consumed;
}

// A touch on the wall: a drag scrolls it, a tap picks a game, a tap on the picked one zooms into it.
bool App::zoom_pointer(int kind, int x, int y) {
    const Uint32 now = s.GetTicks();
    if (kind == 0) {
        zoom_finger = false;
        if (!zoom.shown() || !zoom_allowed() || y < ZoomWall::top - 8) return false; // the header keeps its taps
        // The edges keep their gestures (Home from the bottom, Back from the left), as on every page.
        if (gestures_available() && (y >= 470 || x <= 10)) return false;
        if (!zoom.pinching && zoom.target == 0) { // zooming back in: land now and let the library have the touch
            zoom.reset();
            dirty = true;
            return false;
        }
        zoom_finger = true;
        zoom_dragged = false;
        zoom_down_x = contact_x = down_x = x;
        zoom_down_y = contact_y = down_y = y;
        pointer_down = true;
        pointer_at = now;
        if (zoom.live()) {
            zoom_down_scroll = zoom_scroll[size_t(zoom.target)];
            zoom_motion.begin(now, zoom_down_scroll);
        }
        dirty = true;
        return true;
    }
    if (!zoom_finger) return false;
    contact_x = x;
    contact_y = y;
    const size_t count = model.games.size();
    const auto wall = ZoomWall::at(zoom.target);
    int& scroll = zoom_scroll[size_t(zoom.target)];
    if (zoom.live() && (zoom_dragged || std::abs(y - zoom_down_y) > 8 || std::abs(x - zoom_down_x) > 12))
        zoom_dragged = true;
    if (kind == 1) {
        if (zoom.live() && zoom_dragged && zoom_motion.dragging) {
            scroll = zoom_motion.drag(now, zoom_down_scroll - (y - zoom_down_y), 0, wall.maximum(count));
            dirty = true;
        }
        return true;
    }
    zoom_finger = false;
    pointer_down = false;
    pointer_source = PointerSource::None;
    dirty = true;
    if (!zoom.live()) return true;
    if (zoom_dragged) {
        zoom_motion.release(now, 0, wall.maximum(count));
        return true;
    }
    zoom_motion.stop();
    if (kind != 2) return true;
    const int index = wall.hit(x, y, scroll, count);
    if (index < 0) return true;
    if (size_t(index) == model.selected) zoom_go(0, now);
    else {
        sound_intent(MenuCue::Navigate);
        haptics.play(Haptic::Select);
        select_game(size_t(index), true);
        zoom_revealed = model.selected; // tapped where it is: the wall stays put
    }
    return true;
}

// Keys on a wall: the D-pad moves over the grid, L and R page, B zooms back in a level. A still plays.
bool App::zoom_action(int code) {
    if (!zoom.shown() || !zoom_allowed()) return false;
    if (!zoom.pinching && zoom.target == 0) { // zooming back in: land now and let the library have the key
        zoom.reset();
        dirty = true;
        return false;
    }
    const size_t count = model.games.size(), selected = model.selected;
    if (!zoom.live()) // mid-pinch the fingers have it
        return code == 0 || code == 1 || code == 3 || code == 8 || code == 9 || code == 13 || code == 14;
    const auto wall = ZoomWall::at(zoom.target);
    const long columns = wall.columns, page = columns * std::max(1, ZoomWall::height / wall.stride());
    long next = long(selected);
    if (code == 0 || code == 1) {
        next += code == 0 ? -1 : 1;
        next = (next + long(count)) % long(count);
    } else if (code == 8 || code == 9) {
        next += code == 8 ? -columns : columns;
        // Down from the last full row lands on the last game; past either end stays.
        if (next >= long(count) && wall.rows(count) > int(selected / size_t(columns)) + 1) next = long(count) - 1;
        if (next < 0 || next >= long(count)) next = long(selected);
    } else if (code == 13 || code == 14) next = std::clamp(next + (code == 13 ? -page : page), 0L, long(count) - 1);
    else return false; // B goes back as it always does; the wall stays as it is for next time

    if (size_t(next) != selected) {
        sound_intent(MenuCue::Navigate);
        select_game(size_t(next), true);
    }
    dirty = true;
    return true;
}

// A quiet button at the end of the title row: a tap steps to the next level and from the densest back to the
// library. Its glyph is the wall it is on, four or nine, and follows a pinch as it goes.
bool App::zoom_button_shown() {
    return settings.page == Page::Library && !home_menu && !systems_menu && model.games.size() > 1 && !o.game_menu &&
           !o.port_menu;
}

void App::zoom_button(SDL_Renderer* r, int y, SDL_Rect bounds) {
    if (library_control_active(LibraryPress::Zoom, bounds))
        round_rect(r, bounds.x + 6, y + bounds.y + 6, bounds.w - 12, bounds.h - 12, 10, "focus");
    // The library at rest shows four, muted: the way to the walls. A wall shows its own grid in the accent.
    const int n = std::max(2, 1 + int(std::lround(std::clamp(zoom.level, 0.0, double(LibraryZoom::deepest)))));
    constexpr int side = 18, gap = 2;
    const int cell = (side - (n - 1) * gap) / n, span = n * cell + (n - 1) * gap;
    const int left = bounds.x + (bounds.w - span) / 2, top = y + bounds.y + (bounds.h - span) / 2;
    for (int row = 0; row < n; row++)
        for (int column = 0; column < n; column++)
            round_rect(r, left + column * (cell + gap), top + row * (cell + gap), cell, cell, n == 2 ? 2 : 1,
                       zoom.target > 0 ? "accent" : "muted");
}

bool App::zoom_update(Uint32 now) {
    const size_t games = model.games.size();
    // The wall it was left at, back on the first library frame after a start.
    if (!zoom_restored && library_ready && !collection_loading && games > 1) {
        zoom_restored = true;
        if (preferences.library_zoom > 0 && zoom.level == 0 && !zoom.pinching) {
            zoom.level = zoom.base = zoom.target = std::min(preferences.library_zoom, LibraryZoom::deepest);
            for (int level = 1; level <= LibraryZoom::deepest; level++)
                zoom_scroll[size_t(level)] = ZoomWall::at(level).centred(model.selected, games);
            zoom_revealed = model.selected;
            return true;
        }
    }
    // A level the zoom came to rest on is kept, for the next visit and the next start.
    if (zoom_restored && !zoom.pinching && zoom.target != preferences.library_zoom) {
        preferences.library_zoom = zoom.target;
        save_preferences(o.preferences, preferences);
    }
    if (!zoom.shown()) return false;
    if (settings.page != Page::Library || home_menu || systems_menu || games < 2) {
        // Away from the games the wall waits as it is: settled, never reset.
        zoom_motion.stop();
        zoom_finger = false;
        if (zoom.pinching || zoom.animating) {
            zoom.pinching = zoom.animating = false;
            zoom.level = zoom.base = zoom.target;
            return true;
        }
        return false;
    }
    bool moving = zoom.update(now);
    if (!zoom.live()) return moving;
    const size_t count = model.games.size();
    const auto wall = ZoomWall::at(zoom.target);
    int& scroll = zoom_scroll[size_t(zoom.target)];
    scroll = std::clamp(scroll, 0, wall.maximum(count));
    if (zoom_motion.active()) moving |= zoom_motion.step(now, scroll, 0, wall.maximum(count));
    // The selection moved (keys, a search, Jump to letter): the wall glides to show it.
    if (model.selected != zoom_revealed && !zoom_motion.dragging) {
        const int to = wall.revealed(model.selected, count, scroll);
        zoom_motion.glide(now, scroll, to, preferences.animations);
        scroll = to;
        zoom_revealed = model.selected;
        moving = true;
    }
    return moving;
}

// Where a game sits at level 0: in the library as it is drawn now (panels.hpp), or just past the stage's edge.
SDL_Rect App::zoom_rest_rect(size_t index) {
    const double selected = double(model.selected);
    if (browse_layout() == 2) {
        const int row = LibraryViewport::top + int(index) * LibraryViewport::stride - browser_list.scroll;
        return {38, std::clamp(row + 6, LibraryViewport::top - 120, 600), 36, 36};
    }
    const double distance = std::clamp(double(index) - carousel.position, -4.0, 4.0);
    if (browse_layout() == 1) {
        const double depth = std::min(2.0, std::abs(distance));
        const int size = int(std::lround(224 - depth * 34));
        return {int(std::lround(320 + distance * carousel.item_spacing)) - size / 2, 151 + int(depth * 18), size, size};
    }
    (void)selected;
    return {int(std::round(320 + distance * carousel.item_spacing)) - 64, 196, 128, 128};
}

SDL_Rect App::zoom_wall_rect(int level, size_t index, int shift) {
    const auto wall = ZoomWall::at(level);
    const int y = wall.y(index, zoom_scroll[size_t(level)] + shift);
    return {wall.x(index), std::clamp(y, ZoomWall::top - 160, 600), wall.tile, wall.tile};
}

// Which picture a tile shows, asking for the ones it lacks: 0 the cover, 1 the DS ROM icon, 2 icon.bmp, 3 and on
// the game_icon_pictures, -1 none (the cartridge or demo icon). Pictures come from small thumbnails (media.hpp,
// thumb_side), never the full-size covers and title screens: a wall shows dozens at once, and full pictures
// overflowed the texture budget, so they were evicted and reloaded every frame and the wall flashed. Each tile asks
// for one picture at a time, in order, until one is there. `final` says no better picture is still on its way.
int App::zoom_pick(SDL_Renderer* r, size_t index, bool cover, bool& final) {
    final = true;
    const Game& game = model.games[index];
    if (game.demo) return -1;
    const fs::path folder = item_folder(index);
    // 1 there, 0 known missing, -1 asked for now.
    auto thumb = [&](const fs::path& path) {
        if (media.thumb_ready(r, path)) return 1;
        if (media.thumb_known(r, path)) return 0;
        media.request_thumb(r, path);
        return -1;
    };
    if (cover) {
        const int box = thumb(folder / "box.bmp");
        if (box > 0) return 0;
        if (box < 0) final = false; // no cover yet: the icon stands in meanwhile
    }
    if (game.system == "nds") {
        if (media.ready(r, game.path, true)) return 1;
        if (!media.known(r, game.path, true)) {
            media.request(r, game.path, true);
            final = false;
            return -1;
        }
    }
    std::vector<fs::path> pictures{folder / "icon.bmp"};
    if (game.system != "nds")
        for (const char* name : game_icon_pictures) pictures.push_back(folder / name);
    for (size_t i = 0; i < pictures.size(); i++) {
        const int there = thumb(pictures[i]);
        if (there > 0) return int(2 + i);
        if (there < 0) {
            final = false;
            return -1;
        }
    }
    return -1;
}

void App::zoom_paint(SDL_Renderer* r, size_t index, SDL_Rect box, bool selected, double level, int pick) {
    const int size = box.w;
    const int radius = std::clamp(size / 8, 4, card_radius);
    if (selected) {
        alpha_round(r, box.x - 5, box.y - 3, size + 10, size + 10, radius + 5, theme.at("accent"), 70);
        round_rect(r, box.x - 3, box.y - 3, size + 6, size + 6, radius + 3, "accent");
    } else {
        if (size >= 64) round_rect(r, box.x, box.y + 3, size, size, radius, "slot"); // the lip; too small to see below
        round_rect(r, box.x, box.y, size, size, radius, "line");
    }
    round_rect(r, box.x + 1, box.y + 1, size - 2, size - 2, radius - 1, "panel");
    const Game& game = model.games[index];
    const int inset = int(std::lround(size * std::clamp(.22 - .06 * level, .1, .22))), pad = std::max(2, size / 20);
    const SDL_Rect inner{box.x + inset, box.y + inset, size - 2 * inset, size - 2 * inset},
        full{box.x + pad, box.y + pad, size - 2 * pad, size - 2 * pad};
    bool drawn = false;
    if (pick == 0) drawn = media.draw_thumb(r, item_folder(index) / "box.bmp", full, std::max(2, radius - 2));
    else if (pick == 1) drawn = media.draw(r, game.path, inner, true);
    else if (pick == 2) drawn = media.draw_thumb(r, item_folder(index) / "icon.bmp", inner, 4);
    else if (pick > 2 && size_t(pick - 3) < std::size(game_icon_pictures))
        drawn = media.draw_thumb(r, item_folder(index) / game_icon_pictures[pick - 3], inner, 4);
    if (!drawn) {
        if (game.demo) game_icon(r, index, inner.x, inner.y, inner.w);
        else cartridge_icon(r, game, inner.x, inner.y, inner.w);
    }
    if (size >= 60 && game_loved(index)) favourite_badge(r, box.x + size - 10, box.y + 10, size >= 100 ? 22 : 18);
}

// A tile drawn straight to the panel: the selected one, which pops and wears the ring, and any the sprites could not
// take this frame.
void App::zoom_tile(SDL_Renderer* r, size_t index, SDL_Rect box, bool selected, bool cover, double level) {
    if (box.w < 4 || box.x > 640 || box.x + box.w < 0) return; // the stage's clip trims the rest
    bool final = false;
    zoom_paint(r, index, box, selected, level, zoom_pick(r, index, cover, final));
}

// Every rounded card, corner and picture strip of a tile is a draw of its own: a wall of 60 tiles was some 5000
// draws a frame, and scrolling crawled on the handheld. So each tile, as it stands at rest on a level, is drawn once
// to a texture and copied whole after that, one draw a tile; mid-morph the nearer level's texture is scaled. A tile
// still waiting for its picture is looked at again now and then and redrawn once the picture is there. `budget`
// caps the textures drawn in one frame, so a fast fling never stalls on a row of new tiles: the rest draw directly.
SDL_Texture* App::zoom_sprite(SDL_Renderer* r, size_t index, int size, bool cover, int level, int& budget) {
    if (size < 8 || size > 256) return nullptr;
    const Game& game = model.games[index];
    const bool loved = size >= 60 && game_loved(index);
    const Color line = theme.at("line"), panel = theme.at("panel"), slot = theme.at("slot"),
                accent = theme.at("accent");
    std::string key = std::to_string(reinterpret_cast<uintptr_t>(r));
    key += '|';
    key += std::to_string(index);
    key += '|';
    key += game.path.string();
    for (int v : {size, level, int(cover), int(loved), line.r | line.g << 8 | line.b << 16,
                  panel.r | panel.g << 8 | panel.b << 16, slot.r | slot.g << 8 | slot.b << 16,
                  accent.r | accent.g << 8 | accent.b << 16, int(preferences.dark)}) {
        key += '|';
        key += std::to_string(v);
    }
    auto& sprite = zoom_sprites[key];
    sprite.used = ++zoom_sprite_clock;
    const Uint32 now = s.GetTicks();
    if (sprite.texture && (sprite.final || now - sprite.checked < 120)) return sprite.texture;
    if (budget <= 0) return sprite.texture; // drawing the frame: only the pass before it draws textures
    bool final = false;
    const int pick = zoom_pick(r, index, cover, final);
    sprite.checked = now;
    if (sprite.texture && pick == sprite.pick) {
        sprite.final = final;
        return sprite.texture;
    }
    if (budget <= 0) return sprite.texture; // the old picture for one more frame, or drawn directly
    budget--;
    zoom_sprite_builds++;
    if (!sprite.texture) {
        // Smooth scaling for the morph; at rest each texture is copied at its own size.
        const char* quality = s.GetHint("SDL_RENDER_SCALE_QUALITY");
        const std::string before = quality ? quality : "";
        s.SetHint("SDL_RENDER_SCALE_QUALITY", "1");
        sprite.texture = s.CreateTexture(r, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, size + 4, size + 6);
        s.SetHint("SDL_RENDER_SCALE_QUALITY", before.empty() ? "0" : before.c_str());
        if (!sprite.texture) {
            s.ClearError();
            zoom_sprites.erase(key);
            return nullptr;
        }
        s.SetTextureBlendMode(sprite.texture, SDL_BLENDMODE_BLEND);
    }
    SDL_Texture* previous = s.GetRenderTarget(r);
    if (s.SetRenderTarget(r, sprite.texture) != 0) {
        s.ClearError();
        s.DestroyTexture(sprite.texture);
        zoom_sprites.erase(key);
        return nullptr;
    }
    // Clear to the card's own edge colour, see-through, so a scaled edge blends into the card and not into black.
    s.SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    s.SetRenderDrawColor(r, line.r, line.g, line.b, 0);
    s.RenderClear(r);
    zoom_paint(r, index, {2, 2, size, size}, false, level, pick);
    s.SetRenderTarget(r, previous);
    s.SetRenderDrawBlendMode(r, o.game_menu ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
    s.ClearError();
    sprite.pick = pick;
    sprite.final = final;
    // A bounded cache: the least recently drawn tiles go first.
    constexpr size_t limit = 320;
    if (zoom_sprites.size() > limit) {
        std::vector<std::pair<uint64_t, std::string>> ages;
        ages.reserve(zoom_sprites.size());
        for (const auto& [name, entry] : zoom_sprites) ages.emplace_back(entry.used, name);
        std::nth_element(ages.begin(), ages.begin() + long(ages.size() - limit * 3 / 4), ages.end());
        for (size_t i = 0; i < ages.size() - limit * 3 / 4; i++) {
            auto found = zoom_sprites.find(ages[i].second);
            if (found->second.texture) s.DestroyTexture(found->second.texture);
            zoom_sprites.erase(found);
        }
        auto again = zoom_sprites.find(key);
        return again == zoom_sprites.end() ? nullptr : again->second.texture;
    }
    return sprite.texture;
}

void App::zoom_sprites_clear() {
    for (auto& [name, sprite] : zoom_sprites)
        if (sprite.texture) s.DestroyTexture(sprite.texture);
    zoom_sprites.clear();
}

// Before drawing: the titles (letter tags) and options (hearts) of the games on the walls in view.
void App::zoom_prepare() {
    if (!zoom.shown() || model.games.empty()) return;
    const size_t count = model.games.size();
    for (int level = 1; level <= LibraryZoom::deepest; level++) {
        const auto wall = ZoomWall::at(level);
        const int scroll = zoom_scroll[size_t(level)];
        for (int i = std::max(0, wall.first(scroll) - wall.columns); i < wall.last(scroll, count); i++) {
            game_title(size_t(i));
            if (!model.games[size_t(i)].demo) library_cache.options(model.games[size_t(i)]);
        }
    }
}

// The wall, or the morph between two levels. False at level 0, where the library draws itself.
bool App::zoom_render(SDL_Renderer* r, int y, bool prepass) {
    const size_t count = model.games.size();
    if (!zoom.shown() || home_menu || systems_menu || !count || zoom.level <= .0005) {
        if (zoom.shown() && !zoom.pinching && !zoom.animating && zoom.target == 0) zoom.reset();
        return false;
    }
    constexpr int deepest = LibraryZoom::deepest;
    const double clamped = std::clamp(zoom.level, 0.0, double(deepest));
    const int lo = std::min(int(std::floor(clamped)), deepest - 1), hi = lo + 1;
    const double f = clamped - lo;
    // Past either end the whole wall gives a little, about the focus.
    const double past = zoom.level < 0         ? zoom.level
                        : zoom.level > deepest ? zoom.level - deepest
                                               : 0,
                 scale = 1 - past * .45;
    const size_t focus = zoom_focus();
    const int resting = zoom.live() ? zoom.target : -1;
    const int shift = resting > 0 ? int(std::lround(zoom_motion.stretch(zoom_scroll[size_t(resting)]))) : 0;
    auto rect_at = [&](int level, size_t index) {
        return level == 0 ? zoom_rest_rect(index) : zoom_wall_rect(level, index, level == resting ? shift : 0);
    };
    // Which games to draw: those on screen at either end of the morph.
    std::vector<size_t> shown;
    auto add = [&](int from, int to) {
        for (int i = std::max(0, from); i < std::min(int(count), to) && shown.size() < 400; i++)
            shown.push_back(size_t(i));
    };
    for (int level : {lo, hi}) {
        if (level == 0) {
            if (browse_layout() == 2) add(browser_list.first(), browser_list.last(count));
            else add(int(std::floor(carousel.position)) - 4, int(std::floor(carousel.position)) + 6);
        } else {
            const auto wall = ZoomWall::at(level);
            const int scroll = zoom_scroll[size_t(level)] + (level == resting ? shift : 0);
            add(wall.first(scroll), wall.last(scroll, count));
        }
    }
    std::sort(shown.begin(), shown.end());
    shown.erase(std::unique(shown.begin(), shown.end()), shown.end());
    // The selected game last, over its neighbours.
    std::stable_partition(shown.begin(), shown.end(), [&](size_t i) { return i != model.selected; });

    const SDL_Rect focus_rect = rect_at(int(std::lround(clamped)), focus);
    const double fx = focus_rect.x + focus_rect.w / 2.0, fy = focus_rect.y + focus_rect.h / 2.0;
    // The wall unfolds from the focus outward and folds back into it.
    const auto grid = ZoomWall::at(hi);
    const int focus_row = int(focus / size_t(grid.columns)), focus_column = int(focus % size_t(grid.columns));
    const bool covers_at_rest = browse_layout() == 1;

    // Where each tile stands this frame, and the level whose texture it is copied from.
    struct Placed {
        size_t index;
        SDL_Rect box;
        bool cover;
        double level;
        int rest, rest_size;
        bool rest_cover;
        SDL_Texture* sprite = nullptr;
    };
    std::vector<Placed> placed;
    placed.reserve(shown.size());
    for (size_t index : shown) {
        const double distance = std::hypot(double(int(index / size_t(grid.columns)) - focus_row),
                                           double(int(index % size_t(grid.columns)) - focus_column));
        const double delay = std::min(1.0, distance / 7) * .35, t = std::clamp((f - delay) / (1 - .35), 0.0, 1.0),
                     e = t * t * (3 - 2 * t);
        const SDL_Rect a = rect_at(lo, index), b = rect_at(hi, index);
        double x = a.x + (b.x - a.x) * e, top = a.y + (b.y - a.y) * e, size = a.w + (b.w - a.w) * e;
        if (scale != 1) {
            x = fx + (x - fx) * scale;
            top = fy + (top - fy) * scale;
            size *= scale;
        }
        const double tile_level = lo + e;
        const bool cover = tile_level < 1 ? covers_at_rest : tile_level < 1.5;
        if (index == model.selected && preferences.animations) {
            // A newly picked tile pops: a quick swell and back, about its centre.
            const double t = std::min(1.0, (s.GetTicks() - selected_at) / 170.0), pop = std::sin(t * 3.14159265) * .08;
            x -= size * pop / 2;
            top -= size * pop / 2;
            size *= 1 + pop;
        }
        SDL_Rect box{int(std::lround(x)), y + int(std::lround(top)), int(std::lround(size)), int(std::lround(size))};
        if (box.w < 4 || box.x > 640 || box.x + box.w < 0 || box.y > y + 480 || box.y + box.h < y + ZoomWall::top - 8)
            continue;
        const int rest = e < .5 ? lo : hi;
        placed.push_back(
            {index, box, cover, tile_level, rest, (rest == lo ? a : b).w, rest == 0 ? covers_at_rest : rest == 1});
    }
    // New tile textures are drawn in a pass of their own before the panel's frame begins (frame_draw.hpp). On the
    // handheld's tiled GPU every switch of render target in the middle of a frame stores the half-drawn panel and
    // loads it back; before anything is drawn there is nothing to keep. The frame itself only copies.
    int budget = prepass ? 32 : 0;
    for (auto& tile : placed)
        if (tile.index != model.selected)
            tile.sprite = zoom_sprite(r, tile.index, tile.rest_size, tile.rest_cover, tile.rest, budget);
    if (prepass) return true;

    SDL_Rect clip{0, y + ZoomWall::top - 8, 640, 480 - ZoomWall::top + 8};
    s.RenderSetClipRect(r, &clip);
    // The rail lingers as the zoom begins, then gives its room to the wall.
    if (clamped < .35 && browse_layout() != 2) draw_rail(r, y, count, carousel.position, {0, 0, 0}, false);
    for (const auto& tile : placed) {
        if (!tile.sprite) {
            zoom_tile(r, tile.index, tile.box, tile.index == model.selected, tile.cover, tile.level);
            continue;
        }
        const double k = double(tile.box.w) / tile.rest_size;
        const int margin = int(std::lround(2 * k));
        SDL_Rect destination{tile.box.x - margin, tile.box.y - margin, int(std::lround((tile.rest_size + 4) * k)),
                             int(std::lround((tile.rest_size + 6) * k))};
        if (tile.box.w == tile.rest_size)
            destination = {tile.box.x - 2, tile.box.y - 2, tile.rest_size + 4, tile.rest_size + 6};
        s.RenderCopy(r, tile.sprite, nullptr, &destination);
    }
    // At the dense wall, the first game of each letter carries the letter.
    if (clamped > 1.6 && preferences.view == 0 && !discovery_active()) {
        const auto wall = ZoomWall::at(deepest);
        auto initial = [&](size_t i) {
            const std::string title = game_title(i);
            const unsigned char c = title.empty() ? '#' : static_cast<unsigned char>(title[0]);
            return std::isalpha(c) ? char(std::toupper(c)) : '#';
        };
        for (size_t index : shown) {
            if (index > 0 && initial(index) == initial(index - 1)) continue;
            const SDL_Rect box = rect_at(deepest, index);
            if (box.y + wall.tile < ZoomWall::top || box.y > 480) continue;
            const std::string letter(1, initial(index));
            const int w = std::max(20, label_width(letter, 14, false) + 10);
            round_rect(r, box.x - 4, y + box.y - 5, w, 20, 10, "accent");
            raw_label(r, letter, box.x - 4 + (w - label_width(letter, 14, false)) / 2,
                      centred_label_y(letter, 14, y + box.y + 5), 14, "on_accent", w);
        }
    }
    s.RenderSetClipRect(r, nullptr);
    // A wall at rest that is taller than the screen has a scrollbar, as the list does.
    if (clamped >= 1) {
        const int level = int(std::lround(clamped));
        const auto wall = ZoomWall::at(level);
        const int maximum = wall.maximum(count);
        if (maximum > 0) {
            const int h = ZoomWall::height, content = h + maximum, thumb = std::max(40, h * h / std::max(h, content)),
                      offset = std::clamp(zoom_scroll[size_t(level)] + (level == resting ? shift : 0), 0, maximum) *
                               (h - thumb) / maximum;
            draw_scrollbar(r, 612, y + ZoomWall::top, h, thumb, offset, zoom_finger && zoom_dragged, false, 28);
        }
    }
    return true;
}
