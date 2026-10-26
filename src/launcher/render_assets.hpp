#pragma once
// Cover and icon preparation, the pattern background and game titles.
// Members of App in main.cpp; included once, right after the class.
#include "launcher/cartridge_art.hpp"

void App::cart(SDL_Renderer* r, int x, int y, int width, int height, bool selected, int number) {
    rect(r, x + 4, y + 5, width, height, "line");
    rect(r, x, y, width, height, selected ? "accent" : "slot");
    rect(r, x + 10, y + 12, width - 20, height - 37, "panel");
    rect(r, x + width - 27, y, 27, 14, "background");
    text(r, "DS", x + 18, y + 22, 3, "ink");
    text(r, std::to_string(number), x + 18, y + 56, 2, "muted");
    for (int i = 0; i < 5; i++) rect(r, x + 13 + i * (width - 20) / 5, y + height - 18, 6, 10, "ink");
}

std::string App::time_text(const std::tm& t) const {
    char buf[16];
    if (system_preferences.time_format == 1) {
        std::snprintf(buf, sizeof(buf), "%d:%02d %s", t.tm_hour % 12 == 0 ? 12 : t.tm_hour % 12, t.tm_min,
                      t.tm_hour < 12 ? "AM" : "PM");
        return buf;
    }
    std::snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
    return buf;
}

std::string App::clock() {
    auto now = std::time(nullptr);
    return time_text(*std::localtime(&now));
}

std::string App::game_title(size_t index) {
    auto& game = model.games[index];
    if (game.demo) return o.game_menu ? locale.text(game.title) : game.title;
    auto& entry = library_cache.metadata(game);
    if (!entry.title_ready) {
        entry.title = entry.info["title"];
        if (entry.title.empty()) entry.title = entry.info["artwork_title"];
        if (entry.title.empty()) entry.title = library_title(game.title);
        if (entry.title.empty()) entry.title = entry.banner["title"];
        if (entry.title.empty()) entry.title = media.banner(game.path).title;
        entry.title_ready = true;
    }
    return entry.title;
}

inline constexpr const char* game_icon_pictures[] = {"title.bmp", "snap.bmp", "box.bmp"};

void App::warm_icon(SDL_Renderer* r, const Game& game) {
    if (!game.demo && !media.warm(r, game.path, true)) media.warm(r, folder_for_game(game) / "icon.bmp");
}

void App::request_icon(SDL_Renderer* r, const Game& game) {
    if (game.demo) return;
    media.request(r, game.path, true);
    media.request(r, folder_for_game(game) / "icon.bmp");
    // Only the title screen ahead of time: the cover and snap are large, and queue behind nothing the player waits on.
    if (game.system != "nds") media.request(r, folder_for_game(game) / "title.bmp");
}

void App::prepare_render_assets() {
    if (settings.page == Page::GameSwitcher)
        for (auto& panel : surfaces) {
            for (auto& game : switch_games) warm_icon(panel.renderer, game);
            if (settings.focus < int(switch_games.size()))
                media.warm(panel.renderer, folder_for_game(switch_games[settings.focus]) / "box.bmp");
        }
    // The focused cover is ready for the very first Games frame. Keep it
    // warm while on Home too; neighbours are prepared after presentation.
    if (!o.game_menu && settings.page == Page::Library && !model.games.empty())
        for (auto& panel : surfaces) {
            media.warm(panel.renderer, item_folder(model.selected) / "box.bmp");
            warm_icon(panel.renderer, model.selected);
        }
    if (home_menu && home_selected > 0 && home_selected < pictochat_tile) {
        const auto& last = summary.groups[home_selected > 1 ? home_selected - 1 : 0].last_game;
        if (last)
            for (auto& panel : surfaces) warm_icon(panel.renderer, *last);
    }
    if (home_menu && preferences.home_layout == 1 && summary.groups[0].last_game)
        for (auto& panel : surfaces) warm_icon(panel.renderer, *summary.groups[0].last_game);
    if (settings.page == Page::SaveGallery) {
        auto path = slots["image_" + std::to_string(settings.focus)];
        if (!path.empty())
            for (auto& panel : surfaces) media.warm(panel.renderer, path);
    }
    for (auto& panel : surfaces) {
        for (const char* name : {"brand/mark-48.bmp", "brand/mark-120.bmp"})
            media.warm(panel.renderer, asset_path(name));
        media.warm(panel.renderer, asset_path(preferences.dark ? "brand/status-dark.bmp" : "brand/status-light.bmp"));
        media.warm(panel.renderer,
                   asset_path(preferences.dark ? "brand/wordmark-dark.bmp" : "brand/wordmark-light.bmp"));
    }
    for (auto& panel : surfaces) {
        media.warm(panel.renderer, status_asset(sound_asset()));
        media.warm(panel.renderer, status_asset(wifi_asset()));
    }
    if (preferences.rich_icons && settings.page != Page::BootWarning)
        for (auto& panel : surfaces)
            for (const char* name :
                 {"settings", "games", "favorites", "recent",     "pictochat", "display",  "power",
                  "library",  "sound", "wifi",      "system",     "controls",  "shutdown", "play",
                  "save",     "load",  "shaders",   "brightness", "restart",   "home",     "artwork"}) {
                if (settings.page == Page::GameSwitcher && std::string(name) != "settings" &&
                    std::string(name) != "games" && std::string(name) != "recent")
                    continue;
                if (settings.page == Page::Library && std::string(name) != "settings" && std::string(name) != "games" &&
                    std::string(name) != "favorites" && std::string(name) != "recent" &&
                    std::string(name) != "pictochat" &&
                    !((filter_sheet_open || (picker_open && picker_title == "Browser layout")) &&
                      std::string(name) == "artwork"))
                    continue;
                media.warm(panel.renderer, asset_path(std::string("icons/") + name + ".bmp"));
            }
    // The Systems page draws each system's own artwork, so it is prepared
    // with the other icons instead of during the first frame.
    if (preferences.rich_icons)
        for (auto& panel : surfaces)
            for (const auto& system : listed_systems)
                media.warm(panel.renderer, asset_path(std::string("systems/") + system + ".bmp"));
    // Statistics uses game identities from the summary, independent of
    // the current library filter. Prepare only the bottom-screen icons.
    if (settings.page == Page::Statistics && summary_ready && !surfaces.empty())
        for (const auto& entry : summary.most_played) warm_icon(surfaces.back().renderer, entry.game);
    carousel.item_spacing = !home_menu && browse_layout() == 1 ? 158 : Carousel::spacing;
    if (settings.page == Page::ShaderPicker && !surfaces.empty())
        shader_preview.warm(surfaces.front().renderer, shader_preview_folder(), shader_preview_scene, preview_shader(),
                            true);
    if (probe || model.games.empty() || home_menu || systems_menu) return;
    // QuickMenu displays its own identity and controls. Defer ROM/banner
    // metadata until a page actually needs the game title.
    if (o.game_menu) {
        game_title(0);
        for (auto& panel : surfaces) media.warm(panel.renderer, o.data / "session/menu-background.bmp");
        return;
    }
    game_title(model.selected);
    if (o.game_menu || settings.page != Page::Library) return;
    library_cache.stats(model.games[model.selected]);
    browser_list.clamp(model.games.size());
    int first = preferences.layout == 2 ? browser_list.first() : int(std::floor(carousel.position)) - 3;
    int end = preferences.layout == 2 ? browser_list.last(model.games.size()) : first + 7;
    // List metadata and icon fallbacks are ready before either draw pass.
    if (preferences.layout == 2)
        for (int index = std::max(0, first); index < std::min(end, int(model.games.size())); index++)
            game_title(size_t(index));
    // Favourite hearts: the visible cartridges' options are read here, never while drawing.
    for (int index = std::max(0, first); index < std::min(end, int(model.games.size())); index++)
        if (!model.games[size_t(index)].demo) library_cache.options(model.games[size_t(index)]);
    if (!model.games[model.selected].demo) library_cache.options(model.games[model.selected]);
    for (size_t panel = 0; panel < surfaces.size(); panel++) {
        auto* renderer = surfaces[panel].renderer;
        // The selected cartridge is prepared in place so its cover and
        // title never pop in; the visible neighbours and a wide window
        // beyond them load on the loader thread, nearest first.
        media.warm(renderer, item_folder(model.selected) / "box.bmp");
        warm_icon(renderer, model.selected);
        if (!o.preview && panel == 0) continue;
        (void)first;
        (void)end;
        const int reach = preferences.layout == 2 ? 14 : 10, centre = int(model.selected),
                  count = int(model.games.size());
        for (int distance = 1; distance <= reach; distance++)
            for (int direction : {1, -1}) {
                int index = centre + direction * distance;
                if (index >= 0 && index < count) request_icon(renderer, model.games[size_t(index)]);
            }
    }
}

void App::prepare_next_cover() {
    if (o.game_menu || home_menu || settings.page != Page::Library || preferences.layout != 1 || model.games.empty())
        return;
    int center = int(std::lround(carousel.position));
    for (int distance = 1; distance <= 5; distance++)
        for (int direction : {-1, 1}) {
            int index = center + direction * distance;
            if (index < 0 || index >= int(model.games.size())) continue;
            media.request(surfaces.back().renderer, item_folder(size_t(index)) / "box.bmp", false, distance <= 2);
        }
}

void App::watch_library_assets(Uint32 now) {
    if (now - library_poll < 1000) return;
    library_poll = now;
    bool changed = !home_menu && !model.games.empty() && library_cache.refresh(model.games[model.selected]);
    if (media.refresh()) {
        library_cache.invalidate_titles();
        changed = true;
    }
    if (changed) dirty = true;
}

void App::pattern(SDL_Renderer* r, int y, int height, bool lower) {
    auto bg = theme.at("background"), fg = theme.at("line");
    if (preferences.background == 1 && lower) {
        // The original DS touch screen: plain square tiles, 8 DS pixels a
        // side (20 px here), each rule with a pale edge after it so the
        // tiles read as pressed into the screen. Quieter than the paper
        // above, so the controls on top own the contrast.
        const Color rule = blend(bg, fg, preferences.dark ? 30 : 34),
                    edge = blend(bg, Color{255, 255, 255}, preferences.dark ? 4 : 55);
        for (int row = y + 19; row < y + height; row += 20) {
            tint_colour(r, rule);
            s.RenderDrawLine(r, 0, row, 639, row);
            if (row + 1 < y + height) {
                tint_colour(r, edge);
                s.RenderDrawLine(r, 0, row + 1, 639, row + 1);
            }
        }
        for (int column = 19; column < 640; column += 20) {
            tint_colour(r, rule);
            s.RenderDrawLine(r, column, y, column, y + height - 1);
            tint_colour(r, edge);
            s.RenderDrawLine(r, column + 1, y, column + 1, y + height - 1);
        }
        return;
    }
    if (preferences.background == 1) {
        // The DSi's graph paper: faint rules every 4 px, a light grid every
        // 16 px and a firmer one every 64 px, each major line with a pale
        // edge under it so the paper reads as embossed rather than drawn on.
        auto shade = [&](int parts) {
            return Color{(unsigned char)((bg.r * (parts - 1) + fg.r) / parts),
                         (unsigned char)((bg.g * (parts - 1) + fg.g) / parts),
                         (unsigned char)((bg.b * (parts - 1) + fg.b) / parts)};
        };
        const Color fine = shade(22), minor = shade(9), major = shade(4),
                    edge = blend(bg, preferences.dark ? Color{255, 255, 255} : Color{255, 255, 255},
                                 preferences.dark ? 5 : 60);
        tint_colour(r, fine);
        for (int row = y; row < y + height; row += 4)
            if (row % 16) s.RenderDrawLine(r, 0, row, 639, row);
        tint_colour(r, minor);
        for (int row = y + (16 - y % 16) % 16; row < y + height; row += 16)
            if (row % 64) s.RenderDrawLine(r, 0, row, 639, row);
        for (int column = 16; column < 640; column += 16)
            if (column % 64) s.RenderDrawLine(r, column, y, column, y + height - 1);
        for (int row = y + (64 - y % 64) % 64; row < y + height; row += 64) {
            tint_colour(r, major);
            s.RenderDrawLine(r, 0, row, 639, row);
            if (row + 1 < y + height) {
                tint_colour(r, edge);
                s.RenderDrawLine(r, 0, row + 1, 639, row + 1);
            }
        }
        for (int column = 0; column < 640; column += 64) {
            tint_colour(r, major);
            s.RenderDrawLine(r, column, y, column, y + height - 1);
            tint_colour(r, edge);
            s.RenderDrawLine(r, column + 1, y, column + 1, y + height - 1);
        }
        return;
    }
    s.SetRenderDrawColor(r, (bg.r * 19 + fg.r) / 20, (bg.g * 19 + fg.g) / 20, (bg.b * 19 + fg.b) / 20, 255);
    for (int row = y; row < y + height; row += 4) s.RenderDrawLine(r, 0, row, 639, row);
}

void App::game_icon(SDL_Renderer* r, const Game& game, int x, int y, int size, size_t index) {
    SDL_Rect box{x, y, size, size};
    if (!game.demo && ((game.system == "nds" && media.draw(r, game.path, box, true)) ||
                       media.draw(r, folder_for_game(game) / "icon.bmp", box)))
        return;
    // A game of another system has no icon in its ROM: its title screen (or screenshot, or cover), fitted in the
    // square, stands in for it.
    if (!game.demo && game.system != "nds")
        for (const char* picture : game_icon_pictures) {
            const auto path = folder_for_game(game) / picture;
            if (!media.ready(r, path)) {
                media.request(r, path);
                continue;
            }
            const SDL_Rect fitted =
                media.fitted_box(r, path, {x + size / 16, y + size / 16, size * 7 / 8, size * 7 / 8});
            alpha_round(r, fitted.x - 1, fitted.y + 2, fitted.w + 2, fitted.h + 2, 4, shadow_colour(),
                        preferences.dark ? 70 : 30);
            if (media.draw(r, path, fitted, false, 255, false, 4)) return;
        }
    if (!game.demo) {
        // The system's own cartridge, with the badge on its label (the badge shrinks to fit a small row).
        const auto art = cartridge_art(game.system, theme.at("accent"));
        auto at = [&](int v) { return v * size / 100; };
        for (const auto& shape : art.shapes)
            fill_round(r, x + at(shape.x), y + at(shape.y), std::max(1, at(shape.w)), std::max(1, at(shape.h)),
                       at(shape.radius), shape.colour);
        const std::string badge = system_info(game.system).badge;
        const int room = at(art.label.w) - 4;
        int text = 24;
        while (text > 8 && label_width(badge, text, false) > room) text -= 2;
        if (size >= 28)
            raw_label_colour(r, badge, x + at(art.label.x) + (at(art.label.w) - label_width(badge, text, false)) / 2,
                             centred_label_y(badge, text, y + at(art.label.y) + at(art.label.h) / 2), text, art.ink,
                             at(art.label.w));
        return;
    }
    // Original preview symbols, never Nintendo artwork.
    int q = std::max(2, size / 8);
    round_rect(r, x, y, size, size, 4, "focus");
    switch (index % 5) {
    case 0:
        rect(r, x + q * 2, y + q * 2, q * 4, q * 4, "accent");
        rect(r, x + q * 3, y + q, q * 2, q * 6, "ink");
        rect(r, x + q, y + q * 3, q * 6, q * 2, "ink");
        rect(r, x + q * 3, y + q * 3, q * 2, q * 2, "on_accent");
        break;
    case 1:
        for (int i = 0; i < 4; i++)
            rect(r, x + q * (1 + (i % 2) * 3), y + q * (1 + (i / 2) * 3), q * 2, q * 2, i % 2 ? "accent" : "ink");
        break;
    case 2:
        rect(r, x + q * 3, y + q, q * 2, q * 6, "ink");
        for (int i = 0; i < 3; i++) rect(r, x + q * 3, y + q * (1 + i * 2), q * 2, q, "on_accent");
        break;
    case 3:
        ring(r, x + size / 2, y + size / 2, size / 3, "accent");
        rect(r, x + q * 3, y + q * 3, q * 2, q * 2, "ink");
        break;
    default:
        rect(r, x + q * 2, y + q * 2, q * 4, q * 4, "accent");
        rect(r, x + q * 3, y + q * 2, q, q * 4, "on_accent");
        rect(r, x + q * 2, y + q * 3, q * 4, q, "on_accent");
        break;
    }
}

void App::game_icon(SDL_Renderer* r, size_t index, int x, int y, int size) {
    game_icon(r, model.games[index], x, y, size, index);
}

bool App::cover_art(SDL_Renderer* r, const fs::path& path, SDL_Rect box, bool selected) {
    box = media.fitted_box(r, path, box);
    round_rect(r, box.x, box.y + 3, box.w + 6, box.h + 6, 10, "slot");
    round_rect(r, box.x - 3, box.y - 3, box.w + 6, box.h + 6, 10, selected ? "accent" : "line");
    round_rect(r, box.x, box.y, box.w, box.h, 7, "panel");
    return media.draw(r, path, box, false, 255, false, 7);
}

void App::wrapped(SDL_Renderer* r, const std::string& source, int x, int y, int pixels, int width, int rows,
                  bool translate) {
    auto value = translate ? locale.text(source) : source;
    std::string line;
    int row = 0;
    auto measure = [&](const std::string& str) {
        return preferences.pixel ? float(pixel_cells(str) * std::max(2, (pixels + 6) / 10) * 6)
                                 : rounded.width(str, pixels);
    };
    for (const auto& piece : line_pieces(value)) {
        std::string candidate = line.empty() ? piece.text : line + (piece.space_before ? " " : "") + piece.text;
        if (!line.empty() && measure(candidate) > width) {
            label(r, line, x, y + row * (pixels + 7), pixels, "ink", width, false);
            if (++row >= rows) return;
            line = piece.text;
        } else line = candidate;
    }
    if (!line.empty() && row < rows) label(r, line, x, y + row * (pixels + 7), pixels, "ink", width, false);
}
