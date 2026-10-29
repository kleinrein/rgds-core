#pragma once
// Other systems (RG DS Play): categories, the Systems page and the play view.
// Members of App in main.cpp; included once, right after the class.
#include "launcher/system_icons.hpp"
#include <algorithm>
#include <cmath>

// A system's own pixel icon: the 16 x 16 glyph of tools/system_pixel_art.py,
// drawn in whole-pixel steps only, so the simple style stays crisp.
inline bool draw_system_pixel(Sdl& s, SDL_Renderer* renderer, std::string_view id, int cx, int cy, int size,
                              Color ink) {
    const std::string_view pixels = system_pixel_glyph(id);
    if (pixels.size() != 256) return false;
    const int scale = std::max(1, size / 16), left = cx - 8 * scale, top = cy - 8 * scale;
    s.SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    for (char tone : {'#', '+'}) {
        s.SetRenderDrawColor(renderer, ink.r, ink.g, ink.b, tone == '#' ? 255 : 110);
        for (size_t i = 0; i < pixels.size(); i++)
            if (pixels[i] == tone) {
                SDL_Rect cell{left + int(i % 16) * scale, top + int(i / 16) * scale, scale, scale};
                s.RenderFillRect(renderer, &cell);
            }
    }
    s.SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE); // the launcher's resting mode
    return true;
}

void App::open_last_played() {
    const auto& last = summary.groups[0].last_game;
    if (!summary_ready || !last) {
        status = summary_ready ? "Nothing played yet" : "Preparing…";
        dirty = true;
        return;
    }
    play_game(*last);
}

void App::open_category() {
    cancel_navigation();
    if (home_selected == last_played_tile) open_last_played();
    else if (home_selected == pictochat_tile) open_pictochat();
    else if (home_selected == 0) settings.open();
    else if (home_selected == 1 && preferences.show_systems) open_systems();
    else {
        home_menu = false;
        library_view(int(home_selected) - 1);
        remember_appearance();
    }
    dirty = true;
}

bool App::other_system_selected() const {
    return !o.game_menu && model.selected < model.games.size() && model.games[model.selected].system != "nds";
}

void App::refresh_other_systems() {
    all_games = ds_games;
    listed_systems = {"nds"};
    port_entry.reset();
    if (preferences.show_systems && !o.game_menu)
        for (const auto& info : systems()) {
            if (std::string(info.id) == "nds") continue;
            std::vector<fs::path> roots;
            for (const auto& [id, path] : o.system_roots)
                if (id == info.id) roots.push_back(path);
            auto games = scan_system(roots, info.id);
            // PortMaster is not a game: it leaves the list (counts, Recents, the switcher) and is pinned in view_source.
            auto entry = std::find_if(games.begin(), games.end(), [](const Game& g) { return is_port_entry(g.path); });
            if (entry != games.end()) {
                port_entry = *entry;
                port_entry->title = "PortMaster";
                games.erase(entry);
            }
            if (std::string(info.id) == "homebrew") {
                // last in the carousel, where any of its shelves has a folder to fill
                if (!homebrew_shelves().empty()) listed_systems.push_back(info.id);
                continue;
            }
            if (games.empty() && !(std::string(info.id) == "ports" && port_entry)) continue;
            listed_systems.push_back(info.id);
            all_games.insert(all_games.end(), games.begin(), games.end());
        }
    if (std::find(listed_systems.begin(), listed_systems.end(), shown_system) == listed_systems.end())
        shown_system = "nds";
}

std::vector<Game> App::view_source(int view) const {
    // A search or filter looks through every game, not just the system being browsed.
    if (view != 0 || !preferences.show_systems || discovery_active())
        return all_games;
    std::vector<Game> games;
    if (shown_system == "ports" && port_entry) games.push_back(*port_entry); // always first in Ports
    for (const auto& game : all_games)
        if (game.system == shown_system) games.push_back(game);
    return games;
}

size_t App::system_games(const std::string& id) const {
    return size_t(std::count_if(all_games.begin(), all_games.end(), [&](const Game& g) { return g.system == id; }));
}

void App::open_systems() {
    cancel_navigation();
    settings.page = Page::Library;
    home_menu = false;
    systems_menu = true;
    auto at = std::find(listed_systems.begin(), listed_systems.end(), shown_system);
    systems_selected = at == listed_systems.end() ? 0 : size_t(at - listed_systems.begin());
    // Each card's game count now; its play numbers are read when the card is shown (system_stats_for).
    system_stats.assign(listed_systems.size(), {});
    for (const auto& game : all_games)
        if (!game.demo) {
            auto at = std::find(listed_systems.begin(), listed_systems.end(), game.system);
            if (at != listed_systems.end()) system_stats[size_t(at - listed_systems.begin())].games++;
        }
    selected_at = s.GetTicks();
    carousel.reset(systems_selected);
    status.clear();
    dirty = true;
}

const App::SystemStats& App::system_stats_for(size_t index) {
    static const SystemStats none;
    if (index >= system_stats.size()) return none;
    auto& stats = system_stats[index];
    if (!stats.played_ready) {
        // A big system has thousands of stats.ini files on a slow card: read a slice per frame so the carousel keeps
        // up, and show the card's numbers as pending until the last slice is in.
        const uint32_t start = s.GetTicks();
        while (stats.played_next < all_games.size()) {
            const auto& game = all_games[stats.played_next++];
            if (game.system == listed_systems[index] && !game.demo) {
                const auto& played = library_cache.stats(game);
                stats.sessions += played.sessions;
                stats.seconds += played.seconds;
            }
            if (s.GetTicks() - start >= 3) break;
        }
        if (stats.played_next >= all_games.size()) stats.played_ready = true;
        else dirty = true;
    }
    return stats;
}

void App::choose_system(size_t index) {
    if (index >= listed_systems.size()) return;
    if (listed_systems[index] == "homebrew") {
        // the shelves page, over the carousel: Back returns to this tile
        systems_selected = index;
        const auto shelves = homebrew_shelves();
        const bool known = std::find(shelves.begin(), shelves.end(), homebrew_system) != shelves.end();
        homebrew_open(known ? homebrew_system : shelves.front());
        return;
    }
    shown_system = listed_systems[index];
    systems_menu = false;
    library_view(0);
    remember_appearance();
    dirty = true;
}

void App::system_icon(SDL_Renderer* r, size_t index, int x, int y, int size) {
    const auto& info = system_info(index < listed_systems.size() ? listed_systems[index] : "nds");
    const std::string id = info.id;
    // Rich style: the system's own drawing (assets/systems/<id>.bmp).
    if (preferences.rich_icons && size >= 24 &&
        media.draw(r, asset_path("systems/" + id + ".bmp"), {x, y, size, size}, false))
        return;
    // Simple pixel style: the system's own 16 x 16 glyph.
    if (!preferences.rich_icons && draw_system_pixel(s, r, id, x + size / 2, y + size / 2, size, theme.at("accent")))
        return;
    // No artwork for this system yet: the cartridge and its badge.
    ui_icon(r, SettingsIcon::Cartridge, x + size / 2, y + size * 2 / 5, size * 5 / 8, "accent");
    const std::string badge = info.badge;
    raw_label(r, badge, x + (size - label_width(badge, 16, false)) / 2, y + size * 3 / 4, 16, "ink", size);
}

static const std::array<std::string, 3> play_fit_slugs = {"sharp", "integer", "smooth"};
static const std::array<std::string, 4> play_second_slugs = {"companion", "off", "swap", "info"};

App::SystemView App::system_view(const std::string& system) const {
    auto cached = system_views.find(system);
    if (cached != system_views.end()) return cached->second;
    SystemView view;
    auto values = fields(system_defaults_path(system));
    auto found = std::find(play_fit_slugs.begin(), play_fit_slugs.end(), values["fit"]);
    if (found != play_fit_slugs.end()) view.fit = int(found - play_fit_slugs.begin());
    auto second = std::find(play_second_slugs.begin(), play_second_slugs.end(), values["second"]);
    if (second != play_second_slugs.end()) view.second = int(second - play_second_slugs.begin());
    for (int i = 0; i < play::gb_palette_count; i++)
        if (values["palette"] == play::gb_palettes[i].slug) view.palette = i;
    for (int i = 0; i < play::gb_correction_count; i++)
        if (values["colour"] == play::gb_corrections[i].slug) view.correction = i;
    if (!values["frame"].empty()) {
        auto choices = play_frame_choices(system);
        if (std::find(choices.begin(), choices.end(), values["frame"]) != choices.end()) view.frame = values["frame"];
    }
    try {
        size_t end = 0;
        const int shader = values["filter"].empty() ? -1 : std::stoi(values["filter"], &end);
        if (end == values["filter"].size() && shader >= 0 && shader < shader_count) view.filter = shader;
    } catch (...) {}
    system_views[system] = view;
    return view;
}

void App::save_system_view(const std::string& system, const SystemView& view) {
    std::error_code ec;
    const auto path = system_defaults_path(system);
    fs::create_directories(path.parent_path(), ec);
    auto pending = fs::path(path.string() + ".pending");
    {
        std::ofstream out(pending);
        out << "schema=1\nfit=" << play_fit_slugs[size_t(view.fit)] << "\nsecond=" << play_second_slugs[size_t(view.second)]
            << "\nframe=" << view.frame << "\npalette=" << play::gb_palettes[view.palette].slug
            << "\ncolour=" << play::gb_corrections[view.correction].slug << '\n';
        if (view.filter >= 0) out << "filter=" << view.filter << '\n';
    }
    fs::rename(pending, path, ec);
    system_views.erase(system);
    dirty = true;
}

int App::default_shader(const std::string& system) const {
    const int own = system_view(system).filter;
    return own >= 0 ? own : default_shader_for(system.c_str(), preferences.filter);
}

void App::load_play_view() {
    auto values = fields(play_view_path());
    system_views.erase(play_system());
    play_default = system_view(play_system());
    // A game keeps only what differs from its system's defaults; the rest follows them.
    play_fit = play_default.fit;
    play_second = play_default.second;
    auto found = std::find(play_fit_slugs.begin(), play_fit_slugs.end(), values["fit"]);
    if (found != play_fit_slugs.end()) play_fit = int(found - play_fit_slugs.begin());
    auto second = std::find(play_second_slugs.begin(), play_second_slugs.end(), values["second"]);
    if (second != play_second_slugs.end()) play_second = int(second - play_second_slugs.begin());
    auto choices = play_frame_choices();
    play_frame = play_default.frame;
    if (std::find(choices.begin(), choices.end(), values["frame"]) != choices.end()) play_frame = values["frame"];
    play_palette = play_default.palette;
    play_correction = play_default.correction;
    for (int i = 0; i < play::gb_palette_count; i++)
        if (values["palette"] == play::gb_palettes[i].slug) play_palette = i;
    for (int i = 0; i < play::gb_correction_count; i++)
        if (values["colour"] == play::gb_corrections[i].slug) play_correction = i;
}

int App::play_colour_kind() const {
    if (model.games.empty()) return 0;
    const auto& game = model.games.at(0);
    const std::string system = game.system == "nds" ? system_for_path(game.path) : game.system;
    if (system != "gb") return system == "gbc" ? 2 : 0;
    // A .gb made for both handhelds (header flag 0x80, Pokémon Yellow) runs on the Game Boy Color,
    // in its own colours, as the player does (play/src/core/mgba_gb.h).
    std::ifstream rom(game.path, std::ios::binary);
    char flag = 0;
    return rom.seekg(0x143) && rom.get(flag) && (static_cast<unsigned char>(flag) & 0x80) ? 2 : 1;
}

std::string App::play_system() const {
    const auto& game = model.games.at(0);
    const std::string system = game.system == "nds" ? system_for_path(game.path) : game.system;
    return system == "nds" ? "gba" : system;
}

fs::path App::play_frames_folder() { return o.data / "frames" / play_system(); }

std::vector<std::string> App::play_frame_choices(std::string system) const {
    std::vector<std::string> choices, pictures;
    if (system.empty()) system = play_system();
    for (const auto& style : play::frame_styles)
        if (play::frame_fits(style, system)) choices.push_back(style.slug);
    std::error_code ec;
    for (fs::directory_iterator it(o.data / "frames" / system, ec), end; !ec && it != end; it.increment(ec)) {
        const auto name = it->path().stem().string();
        if (lower(it->path().extension().string()) == ".png" && it->is_regular_file(ec) && !name.empty() &&
            name.size() <= 64 && name[0] != '.' && std::find(choices.begin(), choices.end(), name) == choices.end())
            pictures.push_back(name);
    }
    std::sort(pictures.begin(), pictures.end());
    choices.insert(choices.end(), pictures.begin(), pictures.end());
    return choices;
}

std::string App::play_frame_label(const std::string& frame) const {
    for (const auto& style : play::frame_styles)
        if (frame == style.slug) return style.name;
    return frame;
}

void App::apply_play_view(int fit, int second, const std::string& frame) {
    if (fit == play_fit && second == play_second && frame == play_frame) return;
    if (o.game_menu && !o.preview) {
        menu_request("play-graphics:" + std::to_string(fit) + ":" + std::to_string(second) + ":" + frame);
        return;
    }
    std::error_code ec;
    fs::create_directories(play_view_path().parent_path(), ec);
    auto pending = fs::path(play_view_path().string() + ".pending");
    {
        // Only what differs from the system's defaults, as the runtime keeps it (runtime/play_options.py).
        std::ofstream out(pending);
        out << "schema=1\n";
        if (fit != play_default.fit) out << "fit=" << play_fit_slugs[size_t(fit)] << '\n';
        if (second != play_default.second) out << "second=" << play_second_slugs[size_t(second)] << '\n';
        if (frame != play_default.frame) out << "frame=" << frame << '\n';
        if (play_palette != play_default.palette) out << "palette=" << play::gb_palettes[play_palette].slug << '\n';
        if (play_correction != play_default.correction)
            out << "colour=" << play::gb_corrections[play_correction].slug << '\n';
    }
    fs::rename(pending, play_view_path(), ec);
    play_fit = fit;
    play_second = second;
    play_frame = frame;
    status = "Graphics applied (preview).";
    dirty = true;
}

void App::apply_play_colour(int palette, int correction) {
    if (palette == play_palette && correction == play_correction) return;
    if (o.game_menu && !o.preview) {
        menu_request("play-color:" + std::to_string(palette) + ":" + std::to_string(correction));
        return;
    }
    play_palette = palette;
    play_correction = correction;
    const int fit = play_fit;
    play_fit = -1; // apply_play_view skips an unchanged view; this one writes the file with the colours
    apply_play_view(fit, play_second, play_frame);
}

const char* App::play_fit_name(int fit) const {
    return fit == 1 ? "Whole pixels" : fit == 2 ? "Smooth fill" : "Sharp fill";
}

const char* App::play_second_name(int second) const {
    return second == 1 ? "Off" : second == 2 ? "Swap screens" : second == 3 ? "Info screen" : "Companion";
}

// A picker line naming the system's default, so choosing it again means following it.
static std::string marked_default(Locale& locale, const std::string& name, bool is_default) {
    return is_default ? locale.text(name) + " (" + locale.text("Default") + ")" : name;
}

int App::play_graphics_row(int shown) const {
    // Game screen, Second screen, Bezel, Shader, [Screen colours], Save as system default
    return shown < 4 || !play_colour_kind() ? (shown < 4 ? shown : 5) : shown == 4 ? 4 : 5;
}

void App::save_play_default() {
    SystemView view = play_default;
    view.fit = play_fit;
    view.second = play_second;
    view.frame = play_frame;
    view.palette = play_palette;
    view.correction = play_correction;
    if (game_preferences.filter >= 0) view.filter = game_preferences.filter;
    save_system_view(play_system(), view);
    play_default = system_view(play_system());
    status = "Saved as the default for " + std::string(system_info(play_system()).badge) + " games.";
    dirty = true;
}

void App::play_graphics_action() {
    static constexpr std::array<int, 4> order = {0, 3, 1, 2};
    switch (play_graphics_row(settings.focus)) {
    case 0: {
        std::vector<std::string> names;
        for (int i = 0; i < 3; i++) names.push_back(marked_default(locale, play_fit_name(i), i == play_default.fit));
        show_picker("Game screen", names, play_fit, [this](int n) { apply_play_view(n, play_second, play_frame); });
        break;
    }
    case 1: {
        // The picker's order (the info screen beside the companion) over the menu's numbers (play-graphics).
        const int shown = int(std::find(order.begin(), order.end(), play_second) - order.begin()) % 4;
        std::vector<std::string> names;
        for (int id : order) names.push_back(marked_default(locale, play_second_name(id), id == play_default.second));
        show_picker("Second screen", names, shown, [this](int n) {
            if (n >= 0 && n < 4) apply_play_view(play_fit, order[size_t(n)], play_frame);
        });
        break;
    }
    case 2: {
        auto choices = play_frame_choices();
        std::vector<std::string> labels;
        for (const auto& choice : choices)
            labels.push_back(marked_default(locale, play_frame_label(choice), choice == play_default.frame));
        int selected = int(std::find(choices.begin(), choices.end(), play_frame) - choices.begin());
        show_picker("Bezel", labels, selected < int(choices.size()) ? selected : 0, [this, choices](int n) {
            if (n >= 0 && n < int(choices.size())) apply_play_view(play_fit, play_second, choices[size_t(n)]);
        });
        break;
    }
    case 4:
        if (play_colour_kind() == 1) {
            std::vector<std::string> names;
            for (int i = 0; i < play::gb_palette_count; i++)
                names.push_back(marked_default(locale, play::gb_palettes[i].name, i == play_default.palette));
            show_picker("Screen colours", names, play_palette, [this](int n) { apply_play_colour(n, play_correction); });
        } else {
            std::vector<std::string> names;
            for (int i = 0; i < play::gb_correction_count; i++)
                names.push_back(marked_default(locale, play::gb_corrections[i].name, i == play_default.correction));
            show_picker("Screen colours", names, play_correction,
                        [this](int n) { apply_play_colour(play_palette, n); });
        }
        break;
    case 5: save_play_default(); break;
    default:
        // The same shader carousel as the DS engines: the bridge's pass draws GBA and Game Boy screens too.
        open_shader_picker(true);
        shader_return = Page::PlayGraphics;
    }
}

// ---- Library > System defaults: one page for what every game of a system shares

std::vector<std::string> App::default_systems() const {
    std::vector<std::string> ids;
    for (const auto& info : systems())
        if (std::string(info.id) != "nds" && std::string(info.id) != "ports" && std::string(info.id) != "homebrew")
            ids.push_back(info.id);
    return ids;
}

void App::open_system_defaults() {
    system_views.clear(); // the owner's picture bezels may have changed
    const auto ids = default_systems();
    if (std::find(ids.begin(), ids.end(), defaults_system) == ids.end()) defaults_system = ids.front();
    settings.page = Page::SystemDefaults;
    settings.focus = 0;
    status.clear();
    dirty = true;
}

void App::open_system_default_shader() {
    shader_per_game = false;
    shader_defaults = true;
    shader_return = Page::SystemDefaults;
    shader_core = int(preferences.core);
    shader_system = defaults_system;
    shader_choices.assign(1, -1);
    for (auto place : shader_order)
        if (shader_supported(place.id, shader_core) && shader_fits_system(place.id, shader_system))
            shader_choices.push_back(place.id);
    auto found = std::find(shader_choices.begin(), shader_choices.end(), system_view(defaults_system).filter);
    settings.page = Page::ShaderPicker;
    settings.focus = found == shader_choices.end() ? 0 : int(found - shader_choices.begin());
    shader_carousel.reset(size_t(settings.focus));
    shader_drag = false;
    dirty = true;
}

void App::system_defaults_action(int code) {
    auto view = system_view(defaults_system);
    auto save = [this](const SystemView& next) { save_system_view(defaults_system, next); };
    const bool palette = defaults_colour_kind() == 1;
    const int row = settings.focus;
    if (row == 0) {
        const auto ids = default_systems();
        const int at = int(std::find(ids.begin(), ids.end(), defaults_system) - ids.begin());
        if (code == 0 || code == 1) {
            defaults_system = ids[size_t((at + (code == 0 ? int(ids.size()) - 1 : 1)) % int(ids.size()))];
            settings.focus = std::min(settings.focus, list_count() - 1);
            dirty = true;
            return;
        }
        std::vector<std::string> names;
        for (const auto& id : ids) names.push_back(system_info(id).name);
        show_picker("System", names, at, [this, ids](int n) {
            if (n >= 0 && n < int(ids.size())) defaults_system = ids[size_t(n)];
            dirty = true;
        });
    } else if (row == 1) {
        show_picker("Game screen", {"Sharp fill", "Whole pixels", "Smooth fill"}, view.fit, [view, save](int n) {
            auto next = view;
            next.fit = n;
            save(next);
        });
    } else if (row == 2) {
        static constexpr std::array<int, 4> order = {0, 3, 1, 2};
        const int shown = int(std::find(order.begin(), order.end(), view.second) - order.begin()) % 4;
        show_picker("Second screen", {"Companion", "Info screen", "Off", "Swap screens"}, shown,
                    [view, save](int n) {
                        auto next = view;
                        next.second = order[size_t(std::clamp(n, 0, 3))];
                        save(next);
                    });
    } else if (row == 3) {
        auto choices = play_frame_choices(defaults_system);
        std::vector<std::string> labels;
        for (const auto& choice : choices) labels.push_back(play_frame_label(choice));
        int selected = int(std::find(choices.begin(), choices.end(), view.frame) - choices.begin());
        show_picker("Bezel", labels, selected < int(choices.size()) ? selected : 0, [view, save, choices](int n) {
            auto next = view;
            if (n >= 0 && n < int(choices.size())) next.frame = choices[size_t(n)];
            save(next);
        });
    } else if (row == 4) {
        open_system_default_shader();
    } else if (palette) {
        std::vector<std::string> names;
        for (const auto& item : play::gb_palettes) names.push_back(item.name);
        show_picker("Screen colours", names, view.palette, [view, save](int n) {
            auto next = view;
            next.palette = std::clamp(n, 0, play::gb_palette_count - 1);
            save(next);
        });
    } else {
        std::vector<std::string> names;
        for (const auto& item : play::gb_corrections) names.push_back(item.name);
        show_picker("Screen colours", names, view.correction, [view, save](int n) {
            auto next = view;
            next.correction = std::clamp(n, 0, play::gb_correction_count - 1);
            save(next);
        });
    }
}
