#pragma once
// The Storage view: what fills the data card (Settings > System > Device information > Free space).
// A sheet on the picker's rails with a segmented bar or a ring and a legend. The helper that measures the card
// (runtime/storage_usage.py) starts when the sheet opens and stops when it closes: nothing is measured in the
// background. Members of App in main.cpp; included once, after the settings picker.

namespace {
struct StorageKind {
    const char* key;
    const char* name;
};
// The bar's order. Index 7 (Free space) is not measured: it is the card's size less what the rest takes.
constexpr std::array<StorageKind, 7> storage_kinds = {{{"games", "Games"},
                                                       {"saves", "Saves & states"},
                                                       {"artwork", "Artwork & info"},
                                                       {"music", "Music"},
                                                       {"cache", "Cache"},
                                                       {"system", "System"},
                                                       {"other", "Other"}}};
constexpr int storage_free_row = 7, storage_cache_row = 4;

std::string storage_bytes_text(double bytes) {
    char text[32];
    if (bytes >= 1073741824.0 * 10) std::snprintf(text, sizeof text, "%.0f GB", bytes / 1073741824.0);
    else if (bytes >= 1073741824.0) std::snprintf(text, sizeof text, "%.1f GB", bytes / 1073741824.0);
    else if (bytes >= 1048576.0) std::snprintf(text, sizeof text, "%.0f MB", bytes / 1048576.0);
    else if (bytes >= 1024.0) std::snprintf(text, sizeof text, "%.0f KB", bytes / 1024.0);
    else std::snprintf(text, sizeof text, "0 MB");
    return text;
}
} // namespace

double App::storage_bytes(int kind) {
    try {
        return std::max(0.0, std::stod(device_status[std::string("storage_") + storage_kinds.at(size_t(kind)).key]));
    } catch (...) {
        return 0;
    }
}

double App::storage_total_bytes() {
    double total = 0;
    try {
        total = std::stod(device_status["storage_total"]);
    } catch (...) {}
    if (total <= 0) try {
            total = std::stod(device_status["total_gb"]) * 1073741824.0;
        } catch (...) {}
    double used = 0;
    for (int i = 0; i < 7; i++) used += storage_bytes(i);
    return std::max(total, used + 1.0);
}

Color App::storage_colour(int row) {
    static const std::array<Color, 7> palette = {{{0, 0, 0}, // games: the accent, set below
                                                  {232, 166, 36},
                                                  {52, 176, 120},
                                                  {222, 92, 148},
                                                  {146, 154, 168},
                                                  {128, 108, 214},
                                                  {88, 164, 214}}};
    if (row == 0) return theme.at("accent");
    if (row == storage_free_row) return theme.at("line");
    return palette.at(size_t(row));
}

void App::open_storage() {
    picker_title = "Storage";
    picker_options.clear();
    detail_lines.clear();
    detail_scroll = 0;
    picker_apply = {};
    picker_storage = picker_detail = picker_open = true;
    storage_focus = 0;
    storage_frame_at = 0;
    storage_opened_at = s.GetTicks();
    storage_shown.fill(0);
    storage_drawn.fill(0);
    picker_at = s.GetTicks();
    hold.clear();
    // Measured now, only while this sheet is up. A request already running (say the card's status) is left alone.
    if (device_status["storage_state"] == "scanning") device_status["storage_state"] = "";
    start_worker("device.storage");
    dirty = true;
}

// Rows: the legend in the layout in use. Everything below is relative to the sheet's top.
SDL_Rect App::storage_row_rect(int row, int top) const {
    return {48 + (row / 4) * 270, top + 150 + (row % 4) * 34, 256, 30};
}

void App::storage_action(int code) {
    if (code == 3 || code == 7) {
        picker_open = false;
    } else if (code == 0 || code == 8) storage_focus = (storage_focus + 7) % 8;
    else if (code == 1 || code == 9) storage_focus = (storage_focus + 1) % 8;
    else if (code == 2 || code == 4) {
        picker_open = false;
        if (storage_focus == storage_cache_row) confirm("clear-cache", Page::DeviceInfo);
    }
    dirty = true;
}

void App::storage_touch(int x, int y) {
    const int top = picker_top();
    for (int row = 0; row < 8; row++) {
        auto box = storage_row_rect(row, top);
        if (point_in(box, x, y)) {
            if (storage_focus == row && row == storage_cache_row) storage_action(2);
            storage_focus = row;
            dirty = true;
            return;
        }
    }
    if (x >= 48 && x < 568 && y >= top + 100 && y < top + 140) {
        const double total = storage_total_bytes();
        double at = 0;
        for (int row = 0; row < 7; row++) {
            at += storage_bytes(row);
            if ((x - 50) < at / total * 516) {
                storage_focus = row;
                dirty = true;
                return;
            }
        }
        storage_focus = storage_free_row;
        dirty = true;
    }
}

// The sizes on screen ease toward the measured ones: they grow on opening and again as the scan reports in. Both
// panels call this each frame; the one that comes second finds no time has passed.
double App::storage_advance(bool closing) {
    const Uint32 now = s.GetTicks();
    const double total = storage_total_bytes();
    std::array<double, 8> target{};
    double used = 0;
    for (int i = 0; i < 7; i++) used += (target[size_t(i)] = storage_bytes(i));
    target[storage_free_row] = std::max(0.0, total - used);
    const double dt = storage_frame_at ? std::min<Uint32>(100, now - storage_frame_at) / 1000.0 : 0;
    storage_frame_at = now;
    const double k = preferences.animations ? 1 - std::exp(-dt / 0.16) : 1;
    bool moving = false;
    for (size_t i = 0; i < 8; i++) {
        if (dt > 0 || !preferences.animations) storage_shown[i] += (target[i] - storage_shown[i]) * k;
        if (std::fabs(target[i] - storage_shown[i]) / total > 0.0005) moving = true;
        else storage_shown[i] = target[i];
    }
    // On opening each kind sweeps in, one after another, then eases to what the scan has found so far.
    const double seconds = std::max(0.0, double(now - storage_opened_at)) / 1000.0;
    double drawn_used = 0;
    for (size_t i = 0; i < 7; i++) {
        double u = preferences.animations ? std::clamp((seconds - 0.07 * double(i)) / 0.75, 0.0, 1.0) : 1.0;
        storage_drawn[i] = storage_shown[i] * (1 - std::pow(1 - u, 3));
        drawn_used += storage_drawn[i];
    }
    storage_drawn[storage_free_row] = std::max(0.0, total - drawn_used);
    storage_intro = seconds < 1.15;
    // Frames keep coming while anything moves, the sheen runs, or the card is still being read.
    if ((moving || storage_intro || storage_scanning()) && preferences.animations && !closing)
        micro_until = std::max(micro_until, now + 40);
    return total;
}

bool App::storage_scanning() {
    return device_status["storage_state"] == "scanning" || (worker > 0 && worker_action == "device.storage");
}

void App::render_storage(SDL_Renderer* r, int y, int top, bool closing) {
    const double total = storage_advance(closing);
    double used_shown = 0;
    for (size_t i = 0; i < 7; i++) used_shown += storage_drawn[i];
    const bool measured = device_status["storage_state"] == "done" || device_status["storage_state"] == "scanning";
    if (storage_scanning()) raw_label(r, locale.text("Measuring…"), 150, top + 29, 16, "muted", 200);

    // The summary and the segmented bar; the ring and the focused kind's story are on the upper screen.
    raw_label(r, storage_bytes_text(used_shown), 48, top + 54, 28, "ink", 200);
    raw_label(r, locale.text("used"), 52 + label_width(storage_bytes_text(used_shown), 28, false), top + 63, 18,
              "muted", 120);
    const std::string free_text = storage_bytes_text(storage_drawn[storage_free_row]) + " " + locale.text("free");
    raw_label(r, free_text, 568 - label_width(free_text, 22, false), top + 58, 22, "accent", 260);
    const int bx = 48, by = top + 104, bw = 520, bh = 26, ix = bx + 2, iy = by + 2, iw = bw - 4, ih = bh - 4;
    sunken_track(r, bx, by, bw, bh, 13);
    std::array<int, 7> edge{};
    double at = 0;
    int previous = 0;
    for (size_t i = 0; i < 7; i++) {
        at += storage_drawn[i];
        int value = int(std::lround(at / total * iw));
        if (storage_drawn[i] > 0) value = std::max(value, previous + 3);
        edge[i] = previous = std::min(value, iw);
    }
    for (int i = 6; i >= 0; i--)
        if (edge[size_t(i)] > 0)
            fill_round(r, ix, iy, std::max(edge[size_t(i)], 2), ih, std::min(ih / 2, edge[size_t(i)] / 2),
                       storage_colour(i));
    for (int i = 0; i < 6; i++)
        if (storage_drawn[size_t(i)] > 0 && edge[size_t(i)] < edge[6] && edge[size_t(i)] > 1)
            tint_rect(r, ix + edge[size_t(i)] - 1, iy, 2, ih, theme.at("panel"));
    // While the card is read, a soft sheen sweeps over what has been counted.
    if (storage_scanning() && preferences.animations && edge[6] > 8) {
        const int sweep = int(s.GetTicks() % 1400) * (edge[6] + 80) / 1400 - 60;
        SDL_Rect only{ix, y + 0, edge[6], 480};
        SDL_Rect before{0, y, 640, 424};
        s.RenderSetClipRect(r, &only);
        alpha_round(r, ix + sweep, iy, 44, ih, ih / 2, Color{255, 255, 255}, preferences.dark ? 60 : 110);
        s.RenderSetClipRect(r, &before);
    }
    // A short bar under the focused kind, in its own colour.
    const int from = storage_focus == 0 ? 0 : (storage_focus <= 6 ? edge[size_t(storage_focus - 1)] : edge[6]);
    const int to = storage_focus <= 6 ? edge[size_t(storage_focus)] : iw;
    if (to > from) fill_round(r, ix + from, by + bh + 6, std::max(6, to - from), 5, 2, storage_colour(storage_focus));

    for (int row = 0; row < 8; row++) {
        const auto box = storage_row_rect(row, top);
        const bool focused = row == storage_focus;
        if (focused) {
            round_rect(r, box.x, box.y, box.w, box.h, 12, "accent");
            round_rect(r, box.x + 2, box.y + 2, box.w - 4, box.h - 4, 10, "focus");
        }
        fill_round(r, box.x + 10, box.y + box.h / 2 - 7, 14, 14, 4, storage_colour(row));
        const std::string name = row == storage_free_row ? "Free space" : storage_kinds.at(size_t(row)).name;
        const std::string value =
            measured || row == storage_free_row ? storage_bytes_text(storage_drawn[size_t(row)]) : "…";
        raw_label(r, locale.text(name), box.x + 34, centred_label_y(locale.text(name), 18, box.y + box.h / 2), 18,
                  "ink", box.w - 34 - 76);
        raw_label(r, value, box.x + box.w - 10 - label_width(value, 18, false),
                  centred_label_y(value, 18, box.y + box.h / 2), 18, focused ? "accent" : "muted", 76);
    }
    s.RenderSetClipRect(r, nullptr);
    if (closing) return;
    rect(r, 0, y + 424, 640, 56, "background");
    rect(r, 24, y + 424, 592, 1, "line");
    footer_button(r, 12, y + 432, 160, "Close", "B", false, footer_pressed(true));
    footer_button(r, 468, y + 432, 160, storage_focus == storage_cache_row ? "Clear cache" : "Done", "A", true,
                  footer_pressed(false));
}

// The upper screen while the sheet is up: one big ring, and what the focused kind is.
void App::storage_top(SDL_Renderer* r, int y) {
    const double total = storage_advance(!picker_open);
    double used_shown = 0;
    for (size_t i = 0; i < 7; i++) used_shown += storage_drawn[i];
    settings_card(r, 24, y + top_card_y, 592, top_card_h);
    label(r, "Storage", 48, y + top_card_y + 18, 24, "ink", 300);
    if (storage_scanning()) raw_label(r, locale.text("Measuring…"), 380, y + top_card_y + 24, 16, "muted", 200);

    const int cx = 320, cy = y + top_card_y + 176, radius = 104, band = 32;
    smooth_arc(r, cx, cy, radius, band, 0, 1, theme.at("slot"), 255, false);
    const double gap = 3.0 / (2 * 3.14159265358979 * radius); // flat ends with a 3 px seam
    int drawn = 0;
    for (size_t i = 0; i < 7; i++)
        if (storage_drawn[i] > 0) drawn++;
    double at = 0, focus_from = 0, focus_length = 0;
    for (size_t i = 0; i < 7; i++) {
        const double share = storage_drawn[i] / total;
        if (share <= 0) continue;
        // Whole degrees: the arc is baked per size, so a smaller step would bake a new one every frame.
        const double length = std::round(std::max(share - (drawn > 1 ? gap : 0), .003) * 360) / 360;
        const double start = at + (drawn > 1 ? gap / 2 : 0);
        smooth_arc(r, cx, cy, radius, band, start, length, storage_colour(int(i)), 255, false);
        if (int(i) == storage_focus) focus_from = start, focus_length = length;
        at += share;
    }
    // The focused kind's arc, marked on the outside in its own colour.
    if (focus_length > 0)
        smooth_arc(r, cx, cy, radius + band / 2 + 8, 5, focus_from, focus_length, storage_colour(storage_focus), 255,
                   false);
    const std::string percent = std::to_string(int(std::lround(used_shown / total * 100))) + "%";
    raw_label(r, percent, cx - label_width(percent, 44, false) / 2, cy - 36, 44, "ink", 160);
    raw_label(r, locale.text("used"), cx - label_width(locale.text("used"), 18, false) / 2, cy + 18, 18, "muted", 100);

    // What the focused kind is, in a card under the ring.
    static const std::array<const char*, 8> stories = {
        "ROMs, ports and homebrew.",
        "Save files, resume points and save states.",
        "Covers, screenshots and game details.",
        "Your music folder.",
        "Temporary files. Safe to clear.",
        "RG DS Core, its engines and settings.",
        "Everything else on the card, with the file system's own overhead.",
        "Room for more games."};
    const int cardy = y + top_card_y + 316, row = std::clamp(storage_focus, 0, 7);
    round_rect(r, 44, cardy, 552, 68, 14, "panel");
    fill_round(r, 60, cardy + 14, 16, 16, 5, storage_colour(row));
    const std::string name = row == storage_free_row ? "Free space" : storage_kinds.at(size_t(row)).name;
    label(r, name, 86, cardy + 8, 24, "ink", 340);
    const std::string size = storage_bytes_text(storage_drawn[size_t(row)]) + " · " +
                             std::to_string(int(std::lround(storage_drawn[size_t(row)] / total * 100))) + "%";
    raw_label(r, size, 580 - label_width(size, 22, false), cardy + 10, 22, "accent", 200);
    label(r, stories[size_t(row)], 60, cardy + 42, 16, "muted", 520);
}
