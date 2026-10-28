#pragma once
// The Filter sheet: one scrollable modal on the lower screen. Tiles choose the collection and the browser
// layout and start a search or a jump to a letter; below them the library is narrowed by what the game
// information knows. Chips (played state, genre, rating, online play, publisher, developer) toggle at once; players
// and release year are range sliders with two handles over a histogram of the library. The list behind the sheet
// follows every change and the header counts the matching games. Opened with Y in the library.
// Members of App in main.cpp, included after game_info_screen.hpp.

namespace {
constexpr int filter_chip_height = 34, filter_chip_gap = 8, filter_title_height = 26, filter_section_gap = 16;
constexpr int filter_inner_x = 40, filter_inner_width = 520, filter_top = 100, filter_bottom = 412;
constexpr int filter_tile_width = 168, filter_tile_height = 52;
constexpr int filter_slider_height = 108, filter_track_inset = 16;
// The scrollbar sits where the long lists put theirs (a picker's is at 572).
constexpr int filter_rail_x = 572;
// The slider ids: 0 players, 1 release year.
constexpr int slider_players = 0, slider_year = 1;
} // namespace

bool App::filter_sheet_active() const { return filter_sheet_open && settings.page == Page::Library; }

void App::open_filter_sheet(bool to_filters) {
    // Counts are read again each time, so a fetch that just finished shows up.
    if (library_ready) facet_index.request(o.data, all_games);
    filter_sheet_open = true;
    filter_focus = filter_scroll = filter_thumb = 0;
    filter_drag = -1;
    // The current collection has focus; coming from "Filter" the first chip does, with its section at the top.
    const auto layout = filter_layout();
    for (int i = 0; i < int(layout.chips.size()); i++) {
        const auto& chip = layout.chips[size_t(i)];
        if (to_filters ? chip.tile < 0 : chip.tile == preferences.view) {
            filter_focus = i;
            break;
        }
    }
    filter_pin = to_filters;
    dirty = true;
}

// A tile of the sheet: the collection, the browser layout, or a way to find a game.
void App::filter_tile(int tile) {
    if (tile < 3) {
        library_view(tile);
        remember_appearance();
    } else if (tile < 6) {
        preferences.layout = tile - 3;
        cancel_navigation();
        remember_appearance();
    } else if (tile == 6) {
        filter_sheet_open = false;
        jump_to_letter();
    } else if (tile == 7) {
        filter_sheet_open = false;
        quick_search();
    } else {
        search_query.clear();
        discovery_filter = 0;
        discovery_facets = {};
        library_view(preferences.view);
    }
    dirty = true;
}

void App::reset_filters() {
    if (!discovery_filter && !discovery_facets.any()) return;
    discovery_filter = 0;
    discovery_facets = {};
    library_view(preferences.view);
    dirty = true;
}

// The full list of one facet's values, in the long-list picker (the sheet only shows the commonest as chips).
void App::facet_picker(int facet) {
    const auto& spec = facet_rows().at(size_t(facet));
    const auto values = facet_index.values(spec.name, spec.by_count);
    if (values.empty()) return;
    std::vector<std::string> names = {"Any"}, choices = {""};
    int selected = 0;
    for (const auto& [value, count] : values) {
        if (value == discovery_facets.*spec.field) selected = int(names.size());
        names.push_back(value + " (" + std::to_string(count) + ")");
        choices.push_back(value);
    }
    show_picker(spec.title, names, selected, [this, facet, choices](int n) {
        discovery_facets.*facet_rows().at(size_t(facet)).field = choices.at(size_t(n));
        library_view(preferences.view);
    });
}

// What a slider spans (the library's own extent, so every stop is reachable) and where its handles are now.
bool App::filter_slider(int slider, FilterSlider& out) const {
    const auto& histogram = slider == slider_players ? facet_index.players() : facet_index.years();
    if (histogram.size() < 2) return false; // one stop: nothing to slide
    out.histogram = &histogram;
    out.low_bound = histogram.begin()->first;
    out.high_bound = histogram.rbegin()->first;
    const int low = slider == slider_players ? discovery_facets.players_low : discovery_facets.year_low,
              high = slider == slider_players ? discovery_facets.players_high : discovery_facets.year_high;
    out.low = low > 0 ? std::clamp(low, out.low_bound, out.high_bound) : out.low_bound;
    out.high = high > 0 ? std::clamp(high, out.low_bound, out.high_bound) : out.high_bound;
    if (out.low > out.high) out.low = out.high;
    return true;
}

// Store a range; the full extent is stored as "not narrowed".
void App::filter_slider_set(int slider, int low, int high) {
    FilterSlider span;
    if (!filter_slider(slider, span)) return;
    low = std::clamp(low, span.low_bound, span.high_bound);
    high = std::clamp(high, low, span.high_bound);
    const bool full = low == span.low_bound && high == span.high_bound;
    (slider == slider_players ? discovery_facets.players_low : discovery_facets.year_low) = full ? 0 : low;
    (slider == slider_players ? discovery_facets.players_high : discovery_facets.year_high) = full ? 0 : high;
}

void App::filter_slider_move(int slider, int thumb, int delta) {
    FilterSlider span;
    if (!filter_slider(slider, span)) return;
    const int before_low = span.low, before_high = span.high;
    if (thumb == 0) span.low = std::clamp(span.low + delta, span.low_bound, span.high);
    else span.high = std::clamp(span.high + delta, span.low, span.high_bound);
    if (span.low == before_low && span.high == before_high) return;
    filter_slider_set(slider, span.low, span.high);
    library_view(preferences.view);
    dirty = true;
}

std::string App::filter_slider_caption(int slider) const {
    if (slider == slider_players) {
        const auto text = discovery_facets.players_text();
        return text.empty() ? locale.text("Any") : locale.text(text);
    }
    const auto text = discovery_facets.year_text();
    return text.empty() ? locale.text("Any") : text;
}

// Which chips and sliders there are and where they sit, in content coordinates (the top of the first section is 0).
App::FilterLayout App::filter_layout() {
    struct Spec {
        const char* title;
        std::vector<int> facets; // -1 played state; -2 players slider; -3 release slider; -4 collection tiles;
                                 // -5 layout tiles; -6 search tiles; else a facet_rows() index
        size_t limit;            // chips before "More…" (0: all)
    };
    static const std::vector<Spec> specs = {{"Collection", {-4}, 0}, {"Browser layout", {-5}, 0}, {"Search", {-6}, 0},
                                            {"Played", {-1}, 0},     {"Genre", {0}, 10},          {"Players", {-2}, 0},
                                            {"Released", {-3}, 0},   {"Rating", {3}, 0},          {"More", {4, 5}, 0},
                                            {"Publisher", {1}, 8},   {"Developer", {2}, 8}};
    FilterLayout layout;
    layout.loading = !facet_index.ready();
    int y = 0, section = 0;
    for (const auto& spec : specs) {
        std::vector<FilterChip> chips;
        for (int facet : spec.facets) {
            if (facet == -2 || facet == -3) {
                FilterSlider span;
                if (!filter_slider(facet == -2 ? slider_players : slider_year, span)) continue;
                FilterChip item;
                item.slider = facet == -2 ? slider_players : slider_year;
                chips.push_back(std::move(item));
                continue;
            }
            if (facet <= -4) {
                // Tiles: 0-2 the collections, 3-5 the layouts, 6-8 jump, search and clear. All nine always exist, so
                // the focused item keeps its index while the filters change.
                const int first = (-4 - facet) * 3;
                for (int tile = first; tile < first + 3; tile++) {
                    static const std::array<const char*, 9> names = {"All games",      "Favourites",   "Recents",
                                                                     "Cartridges",     "Covers",       "List",
                                                                     "Jump to letter", "Search games", "Clear filters"};
                    FilterChip chip;
                    chip.tile = tile;
                    chip.label = tile == 0 && preferences.show_systems ? std::string(system_info(shown_system).name)
                                                                       : locale.text(names[size_t(tile)]);
                    chip.selected = tile < 3 ? tile == preferences.view : tile < 6 && tile - 3 == preferences.layout;
                    chips.push_back(std::move(chip));
                }
                continue;
            }
            if (facet < 0) {
                static const std::array<const char*, 3> played = {"Unplayed", "Played", "Missing artwork"};
                for (int code = 1; code <= 3; code++) {
                    FilterChip chip;
                    chip.facet = -1;
                    chip.label = locale.text(played[size_t(code - 1)]);
                    chip.value = std::to_string(code);
                    chip.selected = discovery_filter == code;
                    chips.push_back(std::move(chip));
                }
                continue;
            }
            const auto& row = facet_rows()[size_t(facet)];
            const auto values = facet_index.values(row.name, row.by_count);
            const std::string& chosen = discovery_facets.*row.field;
            size_t shown = 0;
            bool chosen_shown = chosen.empty();
            for (const auto& [value, count] : values) {
                if (spec.limit && shown >= spec.limit) break;
                FilterChip chip;
                chip.facet = facet;
                chip.value = value;
                chip.label = locale.text(value);
                chip.count = count;
                chip.selected = value == chosen;
                chosen_shown = chosen_shown || chip.selected;
                chips.push_back(std::move(chip));
                shown++;
            }
            // A value picked from the long list stays visible among the chips.
            if (!chosen_shown) {
                FilterChip chip;
                chip.facet = facet;
                chip.value = chosen;
                chip.label = chosen;
                chip.selected = true;
                chips.push_back(std::move(chip));
            }
            if (spec.limit && values.size() > spec.limit) {
                FilterChip chip;
                chip.facet = facet;
                chip.label = locale.text("More…");
                chip.more = true;
                chips.push_back(std::move(chip));
            }
        }
        if (chips.empty()) continue;
        if (y > 0) y += filter_section_gap;
        layout.titles.push_back({locale.text(spec.title), y});
        const int title_y = y;
        y += filter_title_height;
        int x = 0, row_bottom = y;
        for (auto& chip : chips) {
            if (chip.slider >= 0) {
                chip.box = {0, y, filter_inner_width, filter_slider_height};
                chip.section = section;
                chip.title_y = title_y;
                row_bottom = y + filter_slider_height;
                layout.chips.push_back(std::move(chip));
                continue;
            }
            const int count_width = chip.count >= 0 ? 8 + label_width(std::to_string(chip.count), 14, false) : 0;
            const int w = chip.tile >= 0
                              ? filter_tile_width
                              : std::min(filter_inner_width, label_width(chip.label, 16, false) + count_width + 30);
            const int h = chip.tile >= 0 ? filter_tile_height : filter_chip_height;
            if (x > 0 && x + w > filter_inner_width) {
                x = 0;
                y += h + filter_chip_gap;
            }
            chip.box = {x, y, w, h};
            chip.section = section;
            chip.title_y = title_y;
            row_bottom = y + h;
            x += w + filter_chip_gap;
            layout.chips.push_back(std::move(chip));
        }
        y = row_bottom;
        section++;
    }
    layout.height = y + 12;
    return layout;
}

void App::filter_scroll_to_focus(const FilterLayout& layout) {
    const int view = filter_bottom - filter_top, max = std::max(0, layout.height - view);
    if (!layout.chips.empty()) {
        const auto& chip = layout.chips[size_t(std::clamp(filter_focus, 0, int(layout.chips.size()) - 1))];
        // The first row of a section brings its title along.
        const int top = chip.box.y <= chip.title_y + filter_title_height ? chip.title_y : chip.box.y;
        if (top < filter_scroll) filter_scroll = top;
        if (chip.box.y + chip.box.h > filter_scroll + view) filter_scroll = chip.box.y + chip.box.h - view + 6;
    }
    filter_scroll = std::clamp(filter_scroll, 0, max);
}

void App::filter_activate(const FilterChip& chip) {
    if (chip.slider >= 0) {
        filter_thumb = 1 - filter_thumb; // A picks which end the arrow keys move
        dirty = true;
        return;
    }
    if (chip.more) {
        facet_picker(chip.facet);
        return;
    }
    if (chip.tile >= 0) {
        filter_tile(chip.tile);
        return;
    }
    if (chip.facet < 0) {
        const int code = std::atoi(chip.value.c_str());
        discovery_filter = discovery_filter == code ? 0 : code;
    } else {
        auto& field = discovery_facets.*facet_rows()[size_t(chip.facet)].field;
        field = field == chip.value ? "" : chip.value;
    }
    library_view(preferences.view);
    dirty = true;
}

void App::filter_sheet_key(int code) {
    filter_pin = false;
    auto layout = filter_layout();
    const int count = int(layout.chips.size());
    filter_focus = std::clamp(filter_focus, 0, std::max(0, count - 1));
    const FilterChip* focused = count ? &layout.chips[size_t(filter_focus)] : nullptr;
    if (code == 3) filter_sheet_open = false;
    else if (code == 4) {
        filter_sheet_open = false;
        apply_discovery();
    } else if (code == 11) reset_filters();
    else if (code == 15 || code == 16) {
        filter_sheet_open = false;
        if (code == 15) jump_to_letter();
        else quick_search();
        return;
    } else if (count == 0) {
    } else if (code == 2) filter_activate(*focused);
    else if ((code == 0 || code == 1) && focused->slider >= 0)
        filter_slider_move(focused->slider, filter_thumb, code == 0 ? -1 : 1);
    else if (code == 0 || code == 1) filter_focus = std::clamp(filter_focus + (code == 0 ? -1 : 1), 0, count - 1);
    else if (code == 8 || code == 9) {
        // The nearest chip, by its middle, on the next row up or down.
        const auto& from = *focused;
        const int middle = from.box.x + from.box.w / 2;
        int best = -1, best_row = 0, best_distance = 0;
        for (int i = 0; i < count; i++) {
            const auto& chip = layout.chips[size_t(i)];
            const bool beyond = code == 9 ? chip.box.y > from.box.y : chip.box.y < from.box.y;
            if (!beyond) continue;
            const int distance = std::abs(chip.box.x + chip.box.w / 2 - middle);
            const bool nearer_row = best < 0 || (code == 9 ? chip.box.y < best_row : chip.box.y > best_row);
            if (nearer_row || (chip.box.y == best_row && distance < best_distance)) {
                best = i;
                best_row = chip.box.y;
                best_distance = distance;
            }
        }
        if (best >= 0) filter_focus = best;
    } else if (code == 13 || code == 14) {
        // L and R jump between sections.
        const int section = focused->section + (code == 13 ? -1 : 1);
        for (int i = 0; i < count; i++)
            if (layout.chips[size_t(i)].section == section) {
                filter_focus = i;
                break;
            }
    }
    filter_scroll_to_focus(layout);
    dirty = true;
}

// The slider's track, in screen x: handles sit on the stops of the span.
static int filter_track_left() { return filter_inner_x + filter_track_inset; }
static int filter_track_width() { return filter_inner_width - 2 * filter_track_inset; }

// A touch that lands on a slider takes the nearest handle and follows the finger until it lifts.
bool App::filter_slider_press(int x, int y) {
    if (y < filter_top || y >= filter_bottom || x >= filter_rail_x) return false;
    auto layout = filter_layout();
    const int cy = y - filter_top + filter_scroll;
    for (int i = 0; i < int(layout.chips.size()); i++) {
        const auto& item = layout.chips[size_t(i)];
        if (item.slider < 0 || cy < item.box.y - 6 || cy >= item.box.y + item.box.h + 6) continue;
        FilterSlider span;
        if (!filter_slider(item.slider, span)) return false;
        const int stops = span.high_bound - span.low_bound;
        const int value = std::clamp(span.low_bound + int(std::lround(double(x - filter_track_left()) * stops /
                                                                      std::max(1, filter_track_width()))),
                                     span.low_bound, span.high_bound);
        // The handle nearest the finger; when they share a stop, the side the finger is on.
        filter_thumb = std::abs(value - span.low) < std::abs(value - span.high)   ? 0
                       : std::abs(value - span.low) > std::abs(value - span.high) ? 1
                       : value > span.high                                        ? 1
                                                                                  : 0;
        filter_focus = i;
        filter_drag = item.slider;
        filter_slider_drag(x);
        return true;
    }
    return false;
}

void App::filter_slider_drag(int x) {
    FilterSlider span;
    if (filter_drag < 0 || !filter_slider(filter_drag, span)) return;
    const int stops = span.high_bound - span.low_bound;
    const int value = std::clamp(
        span.low_bound + int(std::lround(double(x - filter_track_left()) * stops / std::max(1, filter_track_width()))),
        span.low_bound, span.high_bound);
    filter_slider_move(filter_drag, filter_thumb, filter_thumb == 0 ? value - span.low : value - span.high);
}

// The scrollbar's thumb height and the scroll range, sized as in the other lists.
void App::filter_rail_metrics(const FilterLayout& layout, int& thumb, int& max) const {
    const int view = filter_bottom - filter_top;
    max = std::max(0, layout.height - view);
    thumb = std::max(54, view * view / std::max(1, layout.height));
}

// A finger on the scrollbar: it grabs the thumb where it lands (the middle when it lands beside it) and drags it.
bool App::filter_rail_press(int x, int y) {
    if (x < filter_rail_x || y < filter_top || y >= filter_bottom) return false;
    const auto layout = filter_layout();
    int thumb = 0, max = 0;
    filter_rail_metrics(layout, thumb, max);
    if (max <= 0) return false;
    const int view = filter_bottom - filter_top, local = y - filter_top, offset = filter_scroll * (view - thumb) / max;
    filter_grab = local >= offset && local < offset + thumb ? local - offset : thumb / 2;
    filter_rail = true;
    filter_seek(y);
    return true;
}

void App::filter_seek(int y) {
    const auto layout = filter_layout();
    int thumb = 0, max = 0;
    filter_rail_metrics(layout, thumb, max);
    const int view = filter_bottom - filter_top;
    filter_scroll =
        std::clamp(int(std::lround((y - filter_top - filter_grab) * double(max) / std::max(1, view - thumb))), 0, max);
    dirty = true;
}

// Dragging the body scrolls it.
void App::filter_drag_scroll(int y) {
    filter_pin = false;
    auto layout = filter_layout();
    const int view = filter_bottom - filter_top;
    filter_scroll = std::clamp(filter_scroll_start + down_y - y, 0, std::max(0, layout.height - view));
    dirty = true;
}

void App::filter_sheet_touch(int x, int y) {
    filter_pin = false;
    if (y < 78 && x < 90) {
        filter_sheet_key(3);
        return;
    }
    if (y >= 424) {
        filter_sheet_key(x < 200 ? 3 : x >= 440 ? 4 : 11);
        return;
    }
    auto layout = filter_layout();
    const int cx = x - filter_inner_x, cy = y - filter_top + filter_scroll;
    for (int i = 0; i < int(layout.chips.size()); i++) {
        const auto& item = layout.chips[size_t(i)];
        const auto& box = item.box;
        if (item.slider >= 0) continue; // sliders answer to the press, not the tap
        if (cx >= box.x - 4 && cx < box.x + box.w + 4 && cy >= box.y - 4 && cy < box.y + box.h + 4 && y >= filter_top &&
            y < filter_bottom) {
            filter_focus = i;
            filter_activate(item);
            filter_scroll_to_focus(layout);
            return;
        }
    }
}

// A tile: outlined in the accent when it is the current choice, with a filled check; the focused one wears an accent
// ring so it reads on both palettes. Jump and search wear their own buttons on the top edge (L2, R2).
void App::filter_draw_tile(SDL_Renderer* r, const FilterChip& item, int x, int top, bool focused) {
    const auto& box = item.box;
    alpha_round(r, x, top + 3, box.w, box.h, 10, shadow_colour(), preferences.dark ? 70 : 22);
    if (focused)
        alpha_round(r, x - 3, top - 3, box.w + 6, box.h + 6, 13, theme.at("accent"), preferences.dark ? 120 : 70);
    round_rect(r, x, top, box.w, box.h, 10, item.selected || focused ? "accent" : "line");
    glossy_pill(r, x + 2, top + 2, box.w - 4, box.h - 4, 8, theme.at("panel"));
    const int chip = std::min(34, box.h - 14);
    fill_round(r, x + 8, top + (box.h - chip) / 2, chip, chip, 9,
               item.selected ? theme.at("accent") : theme.at("slot"));
    library_choice_icon(r, item.tile, x + 8 + chip / 2, top + box.h / 2, chip - 12,
                        item.selected ? "on_accent" : "accent");
    const int width = box.w - chip - (item.selected ? 50 : 26),
              font = label_width(item.label, 18, false) > width ? 15 : 18;
    raw_label_colour(r, item.label, x + chip + 16, centred_label_y(item.label, font, top + box.h / 2), font,
                     theme.at("ink"), width);
    if (item.selected) {
        fill_round(r, x + box.w - 28, top + box.h / 2 - 10, 20, 20, 10, theme.at("accent"));
        check_mark(r, x + box.w - 18, top + box.h / 2, theme.at("on_accent"));
    }
    const std::string hotkey = item.tile == 6 ? "L2" : item.tile == 7 ? "R2" : "";
    if (!hotkey.empty()) key_badge(r, x + box.w - 8 - key_badge_width(hotkey), top, hotkey);
}

// One slider: the caption, a histogram of the library over its span, the track and the two handles.
void App::filter_draw_slider(SDL_Renderer* r, const FilterChip& item, int top, bool focused) {
    FilterSlider span;
    if (!filter_slider(item.slider, span)) return;
    const int left = filter_track_left(), width = filter_track_width();
    const Color accent = theme.at("accent"), panel = theme.at("panel");
    if (focused) {
        round_rect(r, filter_inner_x - 8, top - 6, filter_inner_width + 16, item.box.h + 12, 14, "accent");
        round_rect(r, filter_inner_x - 6, top - 4, filter_inner_width + 12, item.box.h + 8, 12, "panel");
    }
    // What is chosen, in words.
    const bool narrowed =
        item.slider == slider_players ? discovery_facets.players_low > 0 : discovery_facets.year_low > 0;
    const std::string caption = filter_slider_caption(item.slider);
    raw_label_colour(r, caption, filter_inner_x, top + 1, 18, narrowed ? accent : theme.at("muted"), 400);

    // The histogram: one bar per stop, the chosen ones lit.
    const int stops = span.high_bound - span.low_bound, bars_top = top + 30, bars_height = 34;
    int peak = 1;
    for (const auto& [value, games] : *span.histogram) peak = std::max(peak, games);
    const int pitch = stops > 0 ? width / stops : width;
    const int bar = std::clamp(pitch - (stops > 12 ? 2 : 10), 3, 30);
    for (int value = span.low_bound; value <= span.high_bound; value++) {
        auto found = span.histogram->find(value);
        const int games = found == span.histogram->end() ? 0 : found->second;
        const int h = games ? 4 + games * (bars_height - 4) / peak : 2;
        const int cx = left + (value - span.low_bound) * width / std::max(1, stops);
        const bool inside = value >= span.low && value <= span.high;
        fill_round(r, cx - bar / 2, bars_top + bars_height - h, bar, h, std::min(3, bar / 2),
                   games == 0 ? theme.at("slot")
                   : inside   ? blend(accent, panel, 70)
                              : theme.at("slot"));
    }
    // The track and the handles.
    const int track_y = top + 74, low_x = left + (span.low - span.low_bound) * width / std::max(1, stops),
              high_x = left + (span.high - span.low_bound) * width / std::max(1, stops);
    round_rect(r, left, track_y - 3, width, 6, 3, "slot");
    fill_round(r, low_x, track_y - 3, std::max(6, high_x - low_x), 6, 3, accent);
    for (int thumb = 0; thumb < 2; thumb++) {
        const int cx = thumb == 0 ? low_x : high_x, cy = track_y;
        const bool active = focused && filter_thumb == thumb;
        if (active) alpha_round(r, cx - 17, cy - 17, 34, 34, 17, accent, 70);
        fill_round(r, cx - 12, cy - 12, 24, 24, 12, accent);
        fill_round(r, cx - 9, cy - 9, 18, 18, 9, active ? accent : panel);
        if (active) fill_round(r, cx - 4, cy - 4, 8, 8, 4, theme.at("on_accent"));
    }
    // Under the track: every stop when there are few (players), otherwise the two ends.
    auto stop_text = [&](int value) {
        return std::to_string(value) + (item.slider == slider_players && value >= players_cap ? "+" : "");
    };
    if (stops <= 6) {
        for (int value = span.low_bound; value <= span.high_bound; value++) {
            const std::string text = stop_text(value);
            const int cx = left + (value - span.low_bound) * width / std::max(1, stops);
            const bool inside = value >= span.low && value <= span.high;
            raw_label(r, text, cx - label_width(text, 14, false) / 2, top + 90, 14, inside ? "ink" : "muted", 60);
        }
    } else {
        const std::string low_text = stop_text(span.low_bound), high_text = stop_text(span.high_bound);
        raw_label(r, low_text, filter_inner_x + 4, top + 90, 14, "muted", 120);
        raw_label(r, high_text, filter_inner_x + filter_inner_width - 4 - label_width(high_text, 14, false), top + 90,
                  14, "muted", 120);
    }
}

void App::filter_sheet_render(SDL_Renderer* r, int y) {
    rect(r, 0, y, 640, 480, "background");
    pattern(r, y, 480, true);
    settings_header(r, y, "Filter", 300);
    // The result so far, top right.
    std::string count_text =
        collection_loading ? locale.text("Finding games…")
        : discovery_active()
            ? locale.text(std::to_string(model.games.size()) +
                          (model.games.size() == 1 ? " matching game" : " matching games"))
            : locale.text(std::to_string(all_games.size()) + (all_games.size() == 1 ? " game" : " games"));
    raw_label(r, count_text, 612 - std::min(240, label_width(count_text, 18, false)), y + 30, 18, "accent", 240);

    const int view = filter_bottom - filter_top;
    alpha_round(r, 26, y + filter_top - 6, 588, view + 18, 14, shadow_colour(), preferences.dark ? 60 : 18);
    round_rect(r, 24, y + filter_top - 10, 592, view + 20, 14, "panel");
    auto layout = filter_layout();
    const int count = int(layout.chips.size());
    filter_focus = std::clamp(filter_focus, 0, std::max(0, count - 1));
    if (filter_pin) {
        for (const auto& chip : layout.chips)
            if (chip.tile < 0) {
                filter_scroll = chip.title_y;
                break;
            }
        if (!layout.loading) filter_pin = false;
    }
    filter_scroll = std::clamp(filter_scroll, 0, std::max(0, layout.height - view));

    SDL_Rect clip{26, y + filter_top - 4, filter_rail_x - 28, view + 8};
    s.RenderSetClipRect(r, &clip);
    for (const auto& title : layout.titles)
        raw_label(r, title.text, filter_inner_x, y + filter_top + title.y - filter_scroll + 2, 14, "muted", 400);
    for (int i = 0; i < count; i++) {
        const auto& chip = layout.chips[size_t(i)];
        const int x = filter_inner_x + chip.box.x, top = y + filter_top + chip.box.y - filter_scroll;
        if (top + chip.box.h < y + filter_top - 12 || top > y + filter_bottom + 12) continue;
        const bool focused = i == filter_focus;
        if (chip.slider >= 0) {
            filter_draw_slider(r, chip, top, focused);
            continue;
        }
        if (chip.tile >= 0) {
            filter_draw_tile(r, chip, x, top, focused);
            continue;
        }
        if (focused) {
            round_rect(r, x - 3, top - 3, chip.box.w + 6, chip.box.h + 6, chip.box.h / 2 + 3, "accent");
            round_rect(r, x - 1, top - 1, chip.box.w + 2, chip.box.h + 2, chip.box.h / 2 + 1, "panel");
        }
        if (chip.selected) fill_round(r, x, top, chip.box.w, chip.box.h, chip.box.h / 2, theme.at("accent"));
        else {
            round_rect(r, x, top, chip.box.w, chip.box.h, chip.box.h / 2, "line");
            round_rect(r, x + 1, top + 1, chip.box.w - 2, chip.box.h - 2, chip.box.h / 2 - 1, "slot");
        }
        const Color ink = chip.selected ? theme.at("on_accent") : theme.at("ink");
        const int label_pixels = label_width(chip.label, 16, false);
        raw_label_colour(r, chip.label, x + 15, centred_label_y(chip.label, 16, top + chip.box.h / 2), 16, ink,
                         chip.box.w - 22);
        if (chip.count >= 0) {
            // The count, smaller and softer, after the name.
            const std::string number = std::to_string(chip.count);
            raw_label_colour(r, number, x + 15 + label_pixels + 8, centred_label_y(number, 14, top + chip.box.h / 2),
                             14, chip.selected ? blend(ink, theme.at("accent"), 30) : theme.at("muted"), 40);
        }
    }
    if (layout.loading) {
        const int top = y + filter_top + layout.height - 6 - filter_scroll;
        raw_label(r, locale.text("Reading your library…"), filter_inner_x, top, 16, "muted", 480);
    }
    s.RenderSetClipRect(r, nullptr);
    int thumb = 0, max = 0;
    filter_rail_metrics(layout, thumb, max);
    if (max > 0)
        draw_scrollbar(r, filter_rail_x, y + filter_top, view, thumb, filter_scroll * (view - thumb) / max, filter_rail,
                       scroll_hover);

    // Footer: Back, what the keys do, Done.
    rect(r, 0, y + 424, 640, 56, "background");
    rect(r, 24, y + 424, 592, 1, "line");
    footer_button(r, 12, y + 432, 160, "Back", "B", false, footer_pressed(true));
    const bool on_slider = count && layout.chips[size_t(filter_focus)].slider >= 0;
    key_hints(r, 320, y + 452, {{"A", on_slider ? "Other end" : "Toggle"}, {"X", "Reset"}}, 0, 16, 270);
    footer_button(r, 468, y + 432, 160, "Show games", "", true, footer_pressed(false));
}
