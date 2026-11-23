#pragma once
// Photo mode (Album > Y > Photo mode, or A on a picture you are viewing): the lower screen is the desk you draw on,
// the upper screen shows the finished photo as it will be saved. The pictures are photo_mode.hpp's; this file is the
// page around them. A saved photo is a new picture in the same game's album; the screenshot is kept.
//
// A stitched screenshot (both screens, one above the other) is edited as two pages: the desk shows one screen at a
// time (Select, or the Upper/Lower switch in the footer), the upper screen shows the whole stitched photo with the
// page in hand lit, and Save writes both screens back as one stitched picture. Lenses and frames dress both screens;
// the date goes in the lower screen's corner, the end of the picture, like a film camera's.
//
// Motion (all of it skipped with Settings > Motion > Animations off): the desk springs up when the page opens, the
// tool highlight slides along the rail, a tool's options slide in, a lens, frame, undo or Clear all cross-fades,
// a stamp lands big and settles with a burst of sparkles, the rainbow pen sheds glitter and the eraser dust, the
// desk slides between the two screens, and Save prints the photo: it shrinks, tilts and drops out of the desk.
// Members of App in main.cpp; included once, right after the class.

namespace {
constexpr SDL_Rect photo_desk{16, 16, 448, 336}; // the picture on the lower screen (0.7 of the panel)
constexpr int photo_rail_x = 480, photo_rail_w = 144, photo_tool_h = 60, photo_tool_pitch = 68;
constexpr int photo_strip_y = 362, photo_strip_h = 52;
// The footer of a stitched photo: Back, Undo, the Upper/Lower switch, Save.
constexpr int photo_back_w = 140, photo_undo_x = 162, photo_undo_w = 60, photo_switch_x = 234, photo_switch_w = 200;
constexpr Uint32 photo_open_ms = 380, photo_tool_ms = 200, photo_page_ms = 280, photo_fade_ms = 260,
                 photo_save_ms = 640, photo_pop_ms = 280, photo_trail_ms = 40;
// Swatches: ten inks leave room at 44 px; twelve stamps need 42 so the three sizes still fit.
int photo_swatch_pitch(photo::Tool tool) { return tool == photo::Tool::Pen ? 44 : 42; }
// The pen strip's last card, after the sizes: Clear all.
constexpr int photo_clear_x = 16 + 10 * 44 + 8 + 3 * 34 + 10, photo_clear_w = 52;

// The option strip's cards in button order: the pen's ten inks, three sizes and Clear; the stamps' twelve and
// three sizes; one wide card for a frame, lens or the date.
int photo_slot_count(photo::Tool tool) {
    return tool == photo::Tool::Pen ? 14 : tool == photo::Tool::Stamp ? 15 : 1;
}
SDL_Rect photo_slot_box(photo::Tool tool, int slot) {
    if (tool == photo::Tool::Pen || tool == photo::Tool::Stamp) {
        const int count = tool == photo::Tool::Pen ? 10 : 12, pitch = photo_swatch_pitch(tool);
        if (slot < count) return {16 + slot * pitch, 6, 40, 40};
        if (slot < count + 3) return {16 + count * pitch + 8 + (slot - count) * 34, 6, 32, 40};
        return {photo_clear_x, 6, photo_clear_w, 40};
    }
    return {16, 4, 608, 44};
}

double photo_ease_out(double t) { return 1 - std::pow(1 - std::clamp(t, 0.0, 1.0), 3); }
// Past the end and back: the desk's spring as it opens.
double photo_ease_back(double t) {
    t = std::clamp(t, 0.0, 1.0) - 1;
    return 1 + 2.4 * t * t * t + 1.4 * t * t;
}
Color photo_colour(uint32_t c) {
    return Color{(unsigned char)photo::red(c), (unsigned char)photo::green(c), (unsigned char)photo::blue(c)};
}
uint32_t photo_seed = 0x2545F491u;
float photo_random() { // 0..1, for the sparkles only
    photo_seed ^= photo_seed << 13, photo_seed ^= photo_seed >> 17, photo_seed ^= photo_seed << 5;
    return float(photo_seed & 0xFFFFFF) / float(0x1000000);
}

// A 24-bit BMP as written by screenshots.py (either way up) into a 640x480 picture, centred on black.
bool read_photo_bmp(const fs::path& path, std::vector<uint32_t>& out) {
    std::ifstream file(path, std::ios::binary);
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    auto u32 = [&](size_t at) {
        return uint32_t(data[at]) | data[at + 1] << 8 | data[at + 2] << 16 | uint32_t(data[at + 3]) << 24;
    };
    if (data.size() < 54 || data[0] != 'B' || data[1] != 'M') return false;
    const int width = int(u32(18)), height = int(u32(22)), bits = data[28] | data[29] << 8;
    const size_t offset = u32(10);
    const int h = std::abs(height);
    if (bits != 24 || width <= 0 || width > photo::W || h <= 0 || h > photo::H) return false;
    const size_t stride = (size_t(width) * 3 + 3) & ~size_t(3);
    if (data.size() < offset + stride * size_t(h)) return false;
    out.assign(size_t(photo::W) * photo::H, photo::rgba(0, 0, 0));
    const int ox = (photo::W - width) / 2, oy = (photo::H - h) / 2;
    for (int y = 0; y < h; y++) {
        const unsigned char* row = data.data() + offset + stride * size_t(height < 0 ? y : h - 1 - y);
        for (int x = 0; x < width; x++)
            out[size_t(oy + y) * photo::W + ox + x] = photo::rgba(row[x * 3 + 2], row[x * 3 + 1], row[x * 3]);
    }
    return true;
}

bool write_photo_bmp(const fs::path& path, const std::vector<uint32_t>& px) {
    const uint32_t stride = photo::W * 3, size = stride * photo::H;
    std::vector<unsigned char> data(54 + size);
    auto put = [&](size_t at, uint32_t v, int bytes) {
        for (int i = 0; i < bytes; i++) data[at + size_t(i)] = (unsigned char)(v >> (8 * i));
    };
    data[0] = 'B', data[1] = 'M';
    put(2, 54 + size, 4), put(10, 54, 4), put(14, 40, 4), put(18, photo::W, 4);
    put(22, uint32_t(-photo::H), 4), put(26, 1, 2), put(28, 24, 2), put(34, size, 4);
    for (size_t i = 0; i < px.size(); i++) {
        data[54 + i * 3] = (unsigned char)photo::blue(px[i]);
        data[54 + i * 3 + 1] = (unsigned char)photo::green(px[i]);
        data[54 + i * 3 + 2] = (unsigned char)photo::red(px[i]);
    }
    auto pending = path;
    pending += ".pending";
    std::ofstream file(pending, std::ios::binary | std::ios::trunc);
    if (!file.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()))) return false;
    file.close();
    std::error_code ec;
    fs::rename(pending, path, ec);
    return !ec;
}
} // namespace

// The player's frames print a console's name in the launcher's font: loaded once, when photo mode first opens.
void App::open_photo(bool lower) {
    if (album.empty()) return;
    photo_shot = album[size_t(std::clamp(settings.focus, 0, int(album.size()) - 1))];
    photo_lower = lower;
    if (!album_viewing || !fs::exists(o.data / "session/album-view-top.bmp")) {
        album_view_then_photo = true;
        album_view();
        return;
    }
    photo_begin();
}

void App::photo_begin() {
    std::vector<uint32_t> upper, lower;
    if (!read_photo_bmp(o.data / "session/album-view-top.bmp", upper)) {
        emit_sound(MenuCue::Error);
        status = "This picture could not be opened.";
        dirty = true;
        return;
    }
    // album-view-bottom.bmp is there only for a stitched picture (screenshots.py view removes it otherwise).
    const bool stitched = read_photo_bmp(o.data / "session/album-view-bottom.bmp", lower);
    if (!photo_font) {
        photo_font = std::make_unique<play::Font>();
        photo_font->load(asset_path("fonts").string());
    }
    const auto& d = photo_shot.date;
    const int year = d.size() == 8 ? std::atoi(d.substr(0, 4).c_str()) : 2026,
              month = d.size() == 8 ? std::atoi(d.substr(4, 2).c_str()) : 1,
              day = d.size() == 8 ? std::atoi(d.substr(6, 2).c_str()) : 1;
    photo_pages = stitched ? 2 : 1;
    photo_editors[0].open(std::move(upper), year, month, day);
    if (stitched) photo_editors[1].open(std::move(lower), year, month, day);
    for (auto& e : photo_editors) {
        e.font = photo_font.get();
        // The date reads the way the player writes dates (System > Date format).
        e.date_order = system_preferences.date_format == 1 ? 1 : system_preferences.date_format == 3 ? 2 : 0;
        e.tool = photo::Tool::Pen;
    }
    photo_page = stitched && photo_lower ? 1 : 0;
    photo_pen_down = photo_cursor_shown = photo_touch_drawing = photo_on_desk = photo_buttons = false;
    photo_cursor_x = photo::W / 2, photo_cursor_y = photo::H / 2;
    photo_slot = photo_slot_from = 0;
    photo_slot_at = 0;
    photo_tool_at = photo_page_at = photo_fade_at = photo_save_at = photo_pop_at = 0;
    photo_sparks.clear();
    photo_fade_px.clear();
    photo_saved_key.clear();
    photo_open_at = std::max<Uint32>(1, s.GetTicks());
    photo_version++;
    album_viewing = false;
    settings.page = Page::Photo;
    emit_sound(MenuCue::Open);
    dirty = true;
}

void App::photo_close() {
    for (auto& [key, texture] : photo_textures)
        if (texture) s.DestroyTexture(texture);
    photo_textures.clear();
    photo_uploaded.clear();
    photo_sparks.clear();
    photo_save_at = 0;
    settings.page = Page::Album;
    // A save that already finished while the photo printed: show the new picture (album_poll does it otherwise).
    if (!photo_saved_key.empty() && album_child <= 0)
        for (size_t i = 0; i < album.size(); i++)
            if (album[i].key == photo_saved_key) {
                settings.focus = int(i);
                break;
            }
    dirty = true;
}

bool App::photo_edited() const {
    return photo_editors[0].edited || (photo_pages > 1 && photo_editors[1].edited);
}

void App::photo_leave() {
    if (!photo_edited()) {
        photo_close();
        emit_sound(MenuCue::Back);
        return;
    }
    show_picker("Leave photo mode?", {"Keep editing", "Leave without saving", "Save"}, 0, [this](int choice) {
        if (choice == 1) {
            photo_close();
            emit_sound(MenuCue::Back);
        } else if (choice == 2) photo_save();
    });
}

void App::photo_save() {
    if (photo_save_at) return;
    const auto top = o.data / "session/photo-edit-top.bmp", bottom = o.data / "session/photo-edit-bottom.bmp";
    if (album_child > 0 || !write_photo_bmp(top, photo_editors[0].compose()) ||
        (photo_pages > 1 && !write_photo_bmp(bottom, photo_editors[1].compose()))) {
        emit_sound(MenuCue::Error);
        return;
    }
    // Both screens go back as one stitched picture, whatever Album > Screenshot is set to.
    std::vector<std::string> command = {"save", "--top", top.string()};
    if (photo_pages > 1) command.insert(command.end(), {"--bottom", bottom.string()});
    command.insert(command.end(), {"--key", photo_shot.key, "--screens", photo_pages > 1 ? "both" : "top"});
    if (!photo_shot.title.empty() && photo_shot.key != "launcher")
        command.insert(command.end(), {"--title", photo_shot.title});
    if (!album_helper(command, "photo-save")) {
        emit_sound(MenuCue::Error);
        return;
    }
    const Uint32 now = std::max<Uint32>(1, s.GetTicks());
    shutter_at = now;
    emit_sound(MenuCue::Save);
    photo_saved_key = photo_shot.key;
    photo_pen_down = photo_touch_drawing = photo_cursor_shown = false;
    photo_sparks.clear();
    photo_pop_at = photo_fade_at = 0;
    // The photo prints: it leaves the desk, then the Album opens on it (photo_tick).
    if (preferences.animations) photo_save_at = now;
    else photo_close();
}

bool App::photo_animating() const {
    if (!preferences.animations) return false;
    const Uint32 now = s.GetTicks();
    auto running = [&](Uint32 at, Uint32 ms) { return at && now - at < ms; };
    return running(photo_open_at, photo_open_ms + 5 * 40) || running(photo_tool_at, photo_tool_ms) ||
           running(photo_page_at, photo_page_ms) || running(photo_fade_at, photo_fade_ms) ||
           running(photo_pop_at, photo_pop_ms) || photo_save_at || !photo_sparks.empty() ||
           (photo_pen_down && photo_cursor_shown);
}

void App::photo_tick() {
    const Uint32 now = s.GetTicks();
    if (photo_save_at && now - photo_save_at >= photo_save_ms) {
        photo_close();
        return;
    }
    photo_sparks.erase(std::remove_if(photo_sparks.begin(), photo_sparks.end(),
                                      [&](const PhotoSpark& p) { return now - p.at >= p.life; }),
                       photo_sparks.end());
    if (photo_animating()) dirty = true;
}

// Picture pixels for a point on the lower screen, or false outside the desk.
bool App::photo_point(int x, int y, double& px, double& py) const {
    px = (x - photo_desk.x) * double(photo::W) / photo_desk.w;
    py = (y - photo_desk.y) * double(photo::H) / photo_desk.h;
    return x >= photo_desk.x && x < photo_desk.x + photo_desk.w && y >= photo_desk.y && y < photo_desk.y + photo_desk.h;
}

int App::photo_option_count() const {
    switch (photo_editors[size_t(photo_page)].tool) {
    case photo::Tool::Pen: return int(photo::ink_names.size()) + int(photo::pen_sizes.size()) + 1;
    case photo::Tool::Stamp: return int(photo::stamps.size()) + int(photo::stamp_scales.size());
    default: return 0;
    }
}

void App::photo_set_tool(int tool) {
    tool = ((tool % int(photo::Tool::Count)) + int(photo::Tool::Count)) % int(photo::Tool::Count);
    if (tool == int(photo_editor().tool)) return;
    photo_tool_from = int(photo_editor().tool);
    photo_tool_at = std::max<Uint32>(1, s.GetTicks());
    for (auto& e : photo_editors) e.tool = photo::Tool(tool);
    photo_pen_down = photo_on_desk = false;
    photo_sync_slot();
    // The date goes on the lower screen of a stitched photo: show it there.
    if (photo::Tool(tool) == photo::Tool::Date && photo_pages > 1) photo_set_page(1);
    emit_sound(MenuCue::Navigate);
    photo_version++;
    dirty = true;
}

// The desk shows the other screen of a stitched photo; the pen, stamp and sizes in hand go with it.
void App::photo_set_page(int page) {
    if (photo_pages < 2 || page == photo_page || page < 0 || page > 1) return;
    const auto& from = photo_editor();
    auto& to = photo_editors[size_t(page)];
    to.tool = from.tool, to.ink = from.ink, to.pen_size = from.pen_size;
    to.stamp = from.stamp, to.stamp_size = from.stamp_size;
    photo_page_from = photo_page;
    photo_page = page;
    photo_page_at = std::max<Uint32>(1, s.GetTicks());
    photo_fade_at = photo_pop_at = 0;
    photo_pen_down = photo_touch_drawing = false;
    photo_sparks.clear();
    emit_sound(MenuCue::Navigate);
    photo_version++;
    dirty = true;
}

// Keep the picture as it is now, to fade from it into the next look (lens, frame, undo, Clear all).
void App::photo_fade_from_here() {
    if (!preferences.animations) return;
    photo_fade_px = photo_editor().compose();
    photo_fade_page = photo_page;
    photo_fade_at = std::max<Uint32>(1, s.GetTicks());
    photo_versions[2]++;
}

// Sparkles from a point of the picture in hand (picture pixels), flying out and falling.
void App::photo_burst(double px, double py, int count, bool rainbow, Color colour) {
    if (!preferences.animations) return;
    const Uint32 now = s.GetTicks();
    for (int i = 0; i < count; i++) {
        const float angle = 6.2832f * (i + photo_random() * .6f) / std::max(1, count),
                    speed = 140 + photo_random() * 220;
        Color c = colour;
        if (rainbow) c = photo_colour(photo::rainbow(photo_random()));
        else if (i % 3 == 2) c = Color{255, 255, 255};
        photo_sparks.push_back({float(px), float(py), std::cos(angle) * speed, std::sin(angle) * speed - 60, 520,
                                7 + photo_random() * 8, now, Uint32(380 + photo_random() * 260), c, photo_page});
    }
    if (photo_sparks.size() > 160) photo_sparks.erase(photo_sparks.begin(), photo_sparks.end() - 160);
}

void App::photo_stamp_at(double px, double py) {
    auto& e = photo_editor();
    e.place_stamp(px, py);
    photo_pop_at = std::max<Uint32>(1, s.GetTicks());
    photo_pop_x = px, photo_pop_y = py, photo_pop_stamp = e.stamp, photo_pop_size = e.stamp_size;
    photo_pop_page = photo_page;
    photo_burst(px, py, 10 + 3 * e.stamp_size, false, photo_colour(photo::stamp_tint(e.stamp)));
    emit_sound(MenuCue::Confirm);
}

// The option strip: a row of swatches (pen, stamps) or one wide picker (frames, lenses, the date).
void App::photo_strip_press(int x) {
    auto& e = photo_editor();
    if (e.tool == photo::Tool::Pen && x >= photo_clear_x && x < photo_clear_x + photo_clear_w) {
        photo_fade_from_here();
        if (e.clear()) {
            // Dust off the whole desk.
            for (int i = 0; i < 4; i++)
                photo_burst(photo::W * (.2 + .2 * i), photo::H * (.3 + .15 * (i % 2)), 6, false, Color{214, 214, 222});
            emit_sound(MenuCue::Unfavourite);
        } else {
            photo_fade_at = 0;
            emit_sound(MenuCue::Error);
        }
        photo_version++;
        dirty = true;
        return;
    }
    if (e.tool == photo::Tool::Pen || e.tool == photo::Tool::Stamp) {
        const int count = e.tool == photo::Tool::Pen ? int(photo::ink_names.size()) : int(photo::stamps.size());
        const int pitch = photo_swatch_pitch(e.tool), swatch = (x - 16) / pitch;
        if (x >= 16 && swatch < count) (e.tool == photo::Tool::Pen ? e.ink : e.stamp) = swatch;
        else if (x >= 16 + count * pitch + 8) {
            const int size = std::clamp((x - (16 + count * pitch + 8)) / 34, 0, 2);
            (e.tool == photo::Tool::Pen ? e.pen_size : e.stamp_size) = size;
        }
    } else photo_step(x < 320 ? -1 : 1);
    photo_sync_slot();
    emit_sound(MenuCue::Navigate);
    photo_version++;
    dirty = true;
}

// The strip's highlight sits on the ink or stamp in hand.
void App::photo_sync_slot() {
    const auto& e = photo_editor();
    photo_slot = e.tool == photo::Tool::Pen ? e.ink : e.tool == photo::Tool::Stamp ? e.stamp : 0;
    photo_slot_from = photo_slot;
    photo_slot_at = 0;
}

// Left/Right on the strip: the highlight moves and picks what it lands on (Clear waits for A).
void App::photo_move_slot(int delta) {
    auto& e = photo_editor();
    if (e.tool != photo::Tool::Pen && e.tool != photo::Tool::Stamp) {
        photo_step(delta);
        emit_sound(MenuCue::Navigate);
        return;
    }
    const int count = photo_slot_count(e.tool), swatches = e.tool == photo::Tool::Pen ? 10 : 12;
    photo_slot_from = photo_slot;
    photo_slot = ((photo_slot + delta) % count + count) % count;
    photo_slot_at = std::max<Uint32>(1, s.GetTicks());
    if (photo_slot < swatches) (e.tool == photo::Tool::Pen ? e.ink : e.stamp) = photo_slot;
    else if (photo_slot < swatches + 3) (e.tool == photo::Tool::Pen ? e.pen_size : e.stamp_size) = photo_slot - swatches;
    emit_sound(MenuCue::Navigate);
    photo_version++;
    dirty = true;
}

// Y (or the strip's arrows): the next ink, stamp, frame or lens; the date stamp on or off.
// Lenses and frames dress both screens of a stitched photo; the date sits on the lower one.
void App::photo_step(int delta) {
    auto& e = photo_editor();
    auto wrap = [](int v, int n) { return ((v % n) + n) % n; };
    switch (e.tool) {
    case photo::Tool::Pen: e.ink = wrap(e.ink + delta, int(photo::ink_names.size())); break;
    case photo::Tool::Stamp: e.stamp = wrap(e.stamp + delta, int(photo::stamps.size())); break;
    case photo::Tool::Frame: {
        photo_fade_from_here();
        const int frame = wrap(e.frame + delta, e.frames());
        for (int p = 0; p < photo_pages; p++) photo_editors[size_t(p)].set_frame(frame);
        break;
    }
    case photo::Tool::Lens: {
        photo_fade_from_here();
        const auto lens = photo::Lens(wrap(int(e.lens) + delta, int(photo::Lens::Count)));
        for (int p = 0; p < photo_pages; p++) photo_editors[size_t(p)].set_lens(lens);
        break;
    }
    default: {
        auto& dated = photo_editors[size_t(photo_pages - 1)];
        photo_fade_from_here();
        dated.set_date(!dated.date);
    }
    }
    photo_version++;
    dirty = true;
}

// Buttons. Everywhere: L/R the tool, X undo, Y the next ink, stamp, frame or lens, Select the other screen, Start save.
// On the strip (where the page opens): Left/Right walk the inks, sizes and Clear, or step a frame, lens or the date;
// Up or A goes onto the picture. On the picture: the D-pad moves the cursor, A puts the pen down (and up again) or
// places a stamp, and B goes back to the strip. B on the strip leaves.
void App::photo_action(int code) {
    if (photo_save_at) return; // the photo is printing
    auto& e = photo_editor();
    const bool drawing_tool = e.tool == photo::Tool::Pen || e.tool == photo::Tool::Stamp;
    if (!photo_buttons) {
        photo_buttons = true;
        photo_sync_slot();
    }
    const int step = 16;
    switch (code) {
    case 3:
        if (photo_on_desk) {
            photo_on_desk = photo_pen_down = photo_cursor_shown = false;
            emit_sound(MenuCue::Back);
            break;
        }
        photo_leave();
        return;
    case 4: photo_save(); return;
    case 7: // Select: the other screen of a stitched photo
        if (photo_pages > 1) photo_set_page(1 - photo_page);
        else emit_sound(MenuCue::Error);
        return;
    case 11:
        photo_fade_from_here();
        if (e.undo()) emit_sound(MenuCue::Back);
        else {
            photo_fade_at = 0;
            emit_sound(MenuCue::Error);
        }
        break;
    case 12:
        photo_step(1);
        photo_sync_slot();
        emit_sound(MenuCue::Navigate);
        return;
    case 13:
    case 14: photo_set_tool(int(e.tool) + (code == 13 ? -1 : 1)); return;
    case 0:
    case 1:
    case 8:
    case 9: {
        if (!drawing_tool) {
            // A frame, lens or the date: Left/Right step it; Up/Down pick the screen of a stitched photo.
            if (code == 0 || code == 1) photo_move_slot(code == 0 ? -1 : 1);
            else if (photo_pages > 1) photo_set_page(code == 8 ? 0 : 1);
            return;
        }
        if (!photo_on_desk) {
            if (code == 0 || code == 1) photo_move_slot(code == 0 ? -1 : 1);
            else if (code == 8) {
                photo_on_desk = photo_cursor_shown = true; // Up: onto the picture
                emit_sound(MenuCue::Navigate);
                break;
            } else if (photo_pages > 1) photo_set_page(1 - photo_page); // Down: the other screen
            return;
        }
        // The cursor; with the pen down it draws as it goes.
        photo_cursor_shown = true;
        photo_cursor_x = std::clamp(photo_cursor_x + (code == 0 ? -step : code == 1 ? step : 0), 0, photo::W - 1);
        photo_cursor_y = std::clamp(photo_cursor_y + (code == 8 ? -step : code == 9 ? step : 0), 0, photo::H - 1);
        if (photo_pen_down && e.tool == photo::Tool::Pen) {
            e.stroke(photo_cursor_x, photo_cursor_y);
            if (e.ink == photo::rainbow_ink) photo_burst(photo_cursor_x, photo_cursor_y, 2, true, Color{255, 255, 255});
        }
        break;
    }
    case 2:
        if (!drawing_tool) {
            photo_move_slot(1);
            return;
        }
        if (!photo_on_desk) {
            if (e.tool == photo::Tool::Pen && photo_slot == 13) {
                photo_strip_press(photo_clear_x + 1); // Clear all
                return;
            }
            photo_on_desk = photo_cursor_shown = true; // A: start drawing with what is chosen
            emit_sound(MenuCue::Confirm);
            break;
        }
        if (e.tool == photo::Tool::Pen) {
            photo_pen_down = !photo_pen_down;
            photo_cursor_shown = true;
            if (photo_pen_down) e.begin(photo_cursor_x, photo_cursor_y);
            emit_sound(photo_pen_down ? MenuCue::Confirm : MenuCue::Navigate);
        } else {
            photo_cursor_shown = true;
            photo_stamp_at(photo_cursor_x, photo_cursor_y);
        }
        break;
    default: return;
    }
    photo_version++;
    dirty = true;
}

void App::photo_pointer(int x, int y, int phase) {
    if (photo_save_at) return;
    auto& e = photo_editor();
    double px = 0, py = 0;
    const bool on_desk = photo_point(x, y, px, py);
    const Uint32 now = s.GetTicks();
    // The rainbow pen sheds glitter as it goes, the eraser a little dust.
    auto trail = [&](double tx, double ty) {
        if (e.tool != photo::Tool::Pen || now - photo_trail_at < photo_trail_ms) return;
        if (e.ink == photo::rainbow_ink) photo_burst(tx, ty, 2, true, Color{255, 255, 255});
        else if (e.ink == photo::eraser_ink) photo_burst(tx, ty, 1, false, Color{200, 200, 208});
        else return;
        photo_trail_at = now;
    };
    if (phase == 0) {
        photo_touch_drawing = false;
        photo_cursor_shown = photo_buttons = photo_pen_down = false;
        photo_on_desk = on_desk;
        if (on_desk) {
            if (e.tool == photo::Tool::Stamp) photo_stamp_at(px, py);
            else if (e.tool == photo::Tool::Pen) {
                e.begin(px, py);
                photo_touch_drawing = true;
                trail(px, py);
            } else photo_step(1);
        } else if (x >= photo_rail_x && y >= photo_desk.y && y < photo_desk.y + 5 * photo_tool_pitch) {
            const int tool = (y - photo_desk.y) / photo_tool_pitch;
            if ((y - photo_desk.y) % photo_tool_pitch < photo_tool_h && tool < int(photo::Tool::Count))
                photo_set_tool(tool);
        } else if (y >= photo_strip_y && y < photo_strip_y + photo_strip_h) photo_strip_press(x);
        else if (y >= 424 && photo_pages > 1) {
            // Back, Undo, the Upper/Lower switch, Save.
            if (x < photo_back_w + 12) photo_leave();
            else if (x >= 440) photo_save();
            else if (x < photo_switch_x - 4) photo_action(11);
            else photo_set_page(x < photo_switch_x + photo_switch_w / 2 ? 0 : 1);
        } else if (y >= 424 && x < 200) photo_leave();
        else if (y >= 424 && x >= 440) photo_save();
        else if (y >= 424) photo_action(11);
    } else if (phase == 1 && photo_touch_drawing) {
        const double cx = std::clamp(px, 0.0, photo::W - 1.0), cy = std::clamp(py, 0.0, photo::H - 1.0);
        e.stroke(cx, cy);
        trail(cx, cy);
    } else if (phase == 2) photo_touch_drawing = false;
    photo_version++;
    dirty = true;
}

// A page (0, 1) or the picture faded from (2) as a texture on `r`, uploaded again only when it changed.
SDL_Texture* App::photo_texture(SDL_Renderer* r, int slot) {
    if (slot == 2 && photo_fade_px.size() != size_t(photo::W) * photo::H) return nullptr;
    auto& texture = photo_textures[{r, slot}];
    if (!texture) {
        texture = s.CreateTexture(r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, photo::W, photo::H);
        if (!texture) return nullptr;
        s.SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        photo_uploaded[{r, slot}] = ~0u;
    }
    const uint32_t version = slot == 2 ? photo_versions[2] : photo_editors[size_t(slot)].revision();
    auto& uploaded = photo_uploaded[{r, slot}];
    if (uploaded != version) {
        const auto& px = slot == 2 ? photo_fade_px : photo_editors[size_t(slot)].compose();
        s.UpdateTexture(texture, nullptr, px.data(), photo::W * 4);
        uploaded = version;
    }
    return texture;
}

// One page of the photo into `to`, with the look it is fading from over it while a lens, frame or undo settles.
void App::photo_picture(SDL_Renderer* r, int page, const SDL_Rect& to, bool fade) {
    if (auto* texture = photo_texture(r, page)) {
        s.SetTextureAlphaMod(texture, 255);
        s.RenderCopy(r, texture, nullptr, &to);
    }
    const Uint32 now = s.GetTicks();
    if (!fade || !photo_fade_at || photo_fade_page != page || now - photo_fade_at >= photo_fade_ms) return;
    if (auto* old = photo_texture(r, 2)) {
        const double t = double(now - photo_fade_at) / photo_fade_ms;
        s.SetTextureAlphaMod(old, Uint8(std::lround(255 * (1 - photo_ease_out(t)))));
        s.RenderCopy(r, old, nullptr, &to);
        s.SetTextureAlphaMod(old, 255);
    }
}

// The landing stamp and the sparkles, over a page drawn into `desk`.
void App::photo_effects(SDL_Renderer* r, const SDL_Rect& desk, int page, Uint32 now) {
    if (!preferences.animations) return;
    const double kx = double(desk.w) / photo::W, ky = double(desk.h) / photo::H;
    s.RenderSetClipRect(r, &desk);
    // The stamp lands big and settles into the one already on the picture.
    if (photo_pop_at && photo_pop_page == page && now - photo_pop_at < photo_pop_ms) {
        const double t = double(now - photo_pop_at) / photo_pop_ms, grow = 1 + .7 * std::pow(1 - t, 2.2);
        const auto& stamp = photo::stamps[size_t(photo_pop_stamp)];
        const double cell = photo::stamp_scales[size_t(photo_pop_size)] * grow;
        const double x0 = photo_pop_x - 6 * cell, y0 = photo_pop_y - 6 * cell;
        for (int sy = 0; sy < 12; sy++)
            for (int sx = 0; sx < 12; sx++)
                if (const uint32_t c = photo::stamp_colour(stamp.rows[size_t(sy)][sx])) {
                    const int ax = desk.x + int(std::floor((x0 + sx * cell) * kx)),
                              ay = desk.y + int(std::floor((y0 + sy * cell) * ky)),
                              bx = desk.x + int(std::ceil((x0 + (sx + 1) * cell) * kx)),
                              by = desk.y + int(std::ceil((y0 + (sy + 1) * cell) * ky));
                    tint_rect(r, ax, ay, std::max(1, bx - ax), std::max(1, by - ay), photo_colour(c));
                }
    }
    // Sparkles: four-pointed glints that fly out, fall and fade.
    s.SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    for (const auto& p : photo_sparks) {
        if (p.page != page || now - p.at >= p.life) continue;
        const float t = float(now - p.at) / 1000.f, life = float(now - p.at) / float(p.life);
        const float x = p.x + p.vx * t, y = p.y + p.vy * t + .5f * p.gravity * t * t;
        const int cx = desk.x + int(x * kx), cy = desk.y + int(y * ky);
        const int size = std::max(2, int(p.size * (1 - life * .6f) * float(kx) / .7f));
        const Uint8 alpha = Uint8(std::lround(255 * (1 - life * life)));
        s.SetRenderDrawColor(r, p.colour.r, p.colour.g, p.colour.b, alpha);
        SDL_Rect across{cx - size, cy - 1, 2 * size + 1, 3}, down{cx - 1, cy - size, 3, 2 * size + 1};
        s.RenderFillRect(r, &across);
        s.RenderFillRect(r, &down);
        s.SetRenderDrawColor(r, 255, 255, 255, alpha);
        SDL_Rect core{cx - 1, cy - 1, 3, 3};
        s.RenderFillRect(r, &core);
    }
    s.RenderSetClipRect(r, nullptr);
}

// What A and B do right now, for the hints: on the strip A starts drawing, on the picture it puts the pen down or
// places a stamp and B goes back to the strip.
std::vector<App::KeyHint> App::photo_hints(bool compact) const {
    const auto& e = photo_editors[size_t(photo_page)];
    const bool drawing_tool = e.tool == photo::Tool::Pen || e.tool == photo::Tool::Stamp;
    std::vector<KeyHint> hints;
    if (!drawing_tool) hints = {{"A", "Next"}};
    else if (!photo_on_desk)
        hints = {{"A", e.tool == photo::Tool::Pen && photo_slot == 13 && photo_buttons ? "Clear" : "Draw"}};
    else if (e.tool == photo::Tool::Pen) hints = {{"A", photo_pen_down ? "Lift pen" : "Pen down"}, {"B", "Tools"}};
    else hints = {{"A", "Stamp"}, {"B", "Tools"}};
    if (compact && hints.size() < 2) hints.push_back({"L R", "Tools"});
    if (!compact) hints.push_back({"L R", "Tools"});
    return hints;
}

void App::photo_top(SDL_Renderer* r, int y) {
    const Uint32 now = s.GetTicks();
    if (photo_pages < 2) {
        SDL_Rect all{0, y, 640, 480};
        photo_picture(r, 0, all, true);
        photo_effects(r, all, 0, now);
        return;
    }
    // A stitched photo: the whole picture, both screens one above the other as it will be saved, the page on the
    // desk lit and the other one dimmed.
    rect(r, 0, y, 640, 480, "background");
    pattern(r, y, 480);
    const SDL_Rect whole{160, y, 320, 480};
    settings_card(r, whole.x - 6, y + 4, whole.w + 12, 472);
    double lit = photo_page; // which half is lit, sliding with the desk
    if (photo_page_at && preferences.animations && now - photo_page_at < photo_page_ms)
        lit = photo_page_from + (photo_page - photo_page_from) * photo_ease_out(double(now - photo_page_at) / photo_page_ms);
    for (int page = 0; page < 2; page++) {
        const SDL_Rect half{whole.x, y + 8 + page * 232, whole.w, 232};
        photo_picture(r, page, half, true);
        photo_effects(r, half, page, now);
        const double dim = std::clamp(std::abs(lit - page), 0.0, 1.0);
        if (dim > .01) {
            s.SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
            s.SetRenderDrawColor(r, 0, 0, 0, Uint8(std::lround(120 * dim)));
            s.RenderFillRect(r, &half);
        }
    }
    // The names beside each half, the lit one in the accent with a pointer.
    for (int page = 0; page < 2; page++) {
        const bool on = page == photo_page;
        const int cy = y + 8 + page * 232 + 116;
        if (on) fill_round(r, 498, cy - 5, 10, 10, 5, theme.at("accent"));
        label(r, page ? "Lower screen" : "Upper screen", 516, cy - 11, 18, on ? "ink" : "muted", 116);
    }
    // The keys for the parts the footer below has no room to explain.
    auto hints = photo_hints(false);
    hints.insert(hints.end(), {{"SELECT", "Screen"}, {"X", "Undo"}});
    for (size_t i = 0; i < hints.size(); i++)
        key_hints(r, 80, y + 240 - int(hints.size()) * 20 + int(i) * 40, {hints[i]}, 0, 14, 140);
}

void App::photo_bottom(SDL_Renderer* r, int y) {
    auto& e = photo_editor();
    const Uint32 now = s.GetTicks();
    const bool motion = preferences.animations;
    auto progress = [&](Uint32 at, Uint32 ms, Uint32 delay = 0) {
        if (!motion || !at) return 1.0;
        return std::clamp((double(now - at) - delay) / ms, 0.0, 1.0);
    };
    rect(r, 0, y, 640, 480, "background");
    pattern(r, y, 480, true);

    // The desk: the picture with a card's edge round it. It springs up as the page opens and prints on Save.
    const double open = photo_ease_back(progress(photo_open_at, photo_open_ms));
    const double grow = .82 + .18 * open;
    SDL_Rect desk{photo_desk.x + int(std::lround(photo_desk.w * (1 - grow) / 2)),
                  y + photo_desk.y + int(std::lround(photo_desk.h * (1 - grow) / 2)), int(std::lround(photo_desk.w * grow)),
                  int(std::lround(photo_desk.h * grow))};
    if (photo_save_at) {
        // Printing: drawn last, over everything (the end of this function).
    } else {
        // Buttons on the picture: an accent edge round the desk says the D-pad draws here now.
        if (photo_buttons && photo_on_desk) fill_round(r, desk.x - 10, desk.y - 10, desk.w + 20, desk.h + 20, 16, theme.at("accent"));
        settings_card(r, desk.x - 6, desk.y - 6, desk.w + 12, desk.h + 12);
        const double slide = progress(photo_page_at, photo_page_ms);
        if (slide < 1) {
            // Between the two screens: the old one slides out as the new one comes in from its side.
            const int dir = photo_page > photo_page_from ? 1 : -1, shift = int(std::lround(photo_ease_out(slide) * desk.h));
            s.RenderSetClipRect(r, &desk);
            photo_picture(r, photo_page_from, {desk.x, desk.y - dir * shift, desk.w, desk.h}, false);
            photo_picture(r, photo_page, {desk.x, desk.y + dir * (desk.h - shift), desk.w, desk.h}, false);
            s.RenderSetClipRect(r, nullptr);
        } else {
            photo_picture(r, photo_page, desk, true);
            photo_effects(r, desk, photo_page, now);
        }
        if (photo_pages > 1) {
            // Which screen this is, in the desk's corner.
            const std::string name = photo_page ? "Lower screen" : "Upper screen";
            const int w = label_width(name, 14) + 20;
            fill_round(r, desk.x + 8, desk.y + 8, w, 24, 12, Color{20, 20, 24});
            label(r, name, desk.x + 18, desk.y + 11, 14, "on_accent", w - 16);
        }
    }
    if (photo_cursor_shown && !photo_save_at && (e.tool == photo::Tool::Pen || e.tool == photo::Tool::Stamp)) {
        const int cx = photo_desk.x + photo_cursor_x * photo_desk.w / photo::W,
                  cy = y + photo_desk.y + photo_cursor_y * photo_desk.h / photo::H;
        const Color ink = photo_pen_down ? theme.at("accent") : Color{255, 255, 255};
        // The pen breathes while it is down, so you can see it will draw.
        const int arm = 10 + (photo_pen_down && motion ? int(std::lround(2 * std::sin(now / 110.0))) : 0);
        for (int outline = 1; outline >= 0; outline--) {
            const Color c = outline ? Color{20, 20, 24} : ink;
            tint_rect(r, cx - arm - outline, cy - 1 - outline, 2 * arm + 1 + 2 * outline, 3 + 2 * outline, c);
            tint_rect(r, cx - 1 - outline, cy - arm - outline, 3 + 2 * outline, 2 * arm + 1 + 2 * outline, c);
        }
    }

    // The tools, top to bottom; they slide in one after another as the page opens, and the highlight glides
    // from the last tool to the new one.
    static const std::array<SettingsIcon, size_t(photo::Tool::Count)> icons = {
        SettingsIcon::Type, SettingsIcon::Heart, SettingsIcon::Art, SettingsIcon::Shader, SettingsIcon::Clock};
    auto rail_x = [&](int i) {
        return photo_rail_x + int(std::lround((1 - photo_ease_out(progress(photo_open_at, photo_open_ms, i * 40))) * 170));
    };
    for (int i = 0; i < int(photo::Tool::Count); i++)
        settings_card(r, rail_x(i), y + photo_desk.y + i * photo_tool_pitch, photo_rail_w, photo_tool_h, false);
    {
        const double t = photo_ease_out(progress(photo_tool_at, photo_tool_ms));
        const double at = photo_tool_from + (int(e.tool) - photo_tool_from) * t;
        const int chosen = int(e.tool);
        settings_card(r, rail_x(chosen), y + photo_desk.y + int(std::lround(at * photo_tool_pitch)), photo_rail_w,
                      photo_tool_h, true);
    }
    for (int i = 0; i < int(photo::Tool::Count); i++) {
        const int ty = y + photo_desk.y + i * photo_tool_pitch, x = rail_x(i);
        const bool on = int(e.tool) == i;
        ui_icon(r, icons[size_t(i)], x + 26, ty + photo_tool_h / 2, 24, on ? "accent" : "muted");
        label(r, photo::tool_names[size_t(i)], x + 46, ty + 18, 16, on ? "ink" : "muted", photo_rail_w - 52);
    }

    // The strip for the tool in hand: it slides in from the side the tool came from, and rises as the page opens.
    const double rise = photo_ease_out(progress(photo_open_at, photo_open_ms, 120));
    const int sy = y + photo_strip_y + int(std::lround((1 - rise) * 60));
    const double swap = photo_ease_out(progress(photo_tool_at, photo_tool_ms));
    const int ox = int(std::lround((1 - swap) * 56 * (int(e.tool) >= photo_tool_from ? 1 : -1)));
    SDL_Rect strip_clip{0, y + photo_strip_y - 2, 640, photo_strip_h + 6};
    s.RenderSetClipRect(r, &strip_clip);
    // Buttons on the strip: an accent ring that glides to the card the D-pad is on (drawn under the cards).
    if (photo_buttons && !photo_on_desk && !photo_save_at) {
        const int slot = std::min(photo_slot, photo_slot_count(e.tool) - 1),
                  from = std::min(photo_slot_from, photo_slot_count(e.tool) - 1);
        const SDL_Rect a = photo_slot_box(e.tool, from), b = photo_slot_box(e.tool, slot);
        const double k = photo_ease_out(progress(photo_slot_at, 150));
        const int rx = ox + int(std::lround(a.x + (b.x - a.x) * k)), rw = int(std::lround(a.w + (b.w - a.w) * k));
        fill_round(r, rx - 4, sy + b.y - 4, rw + 8, b.h + 8, 14, theme.at("accent"));
    }
    if (e.tool == photo::Tool::Pen || e.tool == photo::Tool::Stamp) {
        const bool pen = e.tool == photo::Tool::Pen;
        const int count = pen ? int(photo::ink_names.size()) : int(photo::stamps.size());
        const int pitch = photo_swatch_pitch(e.tool);
        for (int i = 0; i < count; i++) {
            const int x = ox + 16 + i * pitch;
            const bool on = (pen ? e.ink : e.stamp) == i;
            const int lift = on ? 3 : 0; // the chosen swatch sits up a little
            settings_card(r, x, sy + 6 - lift, 40, 40, on);
            if (pen && i < int(photo::inks.size())) {
                fill_round(r, x + 8, sy + 14 - lift, 24, 24, 12, photo_colour(photo::inks[size_t(i)]));
            } else if (pen && i == photo::rainbow_ink) {
                // The rainbow drifts, as the pen does.
                const double phase = motion ? now / 1600.0 : 0;
                for (int b = 0; b < 6; b++)
                    tint_rect(r, x + 8 + b * 4, sy + 14 - lift, 4, 24, photo_colour(photo::rainbow(b / 6.0 + phase)));
            } else if (pen) {
                ui_icon(r, SettingsIcon::Trash, x + 20, sy + 26 - lift, 22, on ? "accent" : "muted");
            } else {
                const auto& stamp = photo::stamps[size_t(i)];
                for (int py = 0; py < 12; py++)
                    for (int px = 0; px < 12; px++)
                        if (const uint32_t c = photo::stamp_colour(stamp.rows[size_t(py)][px]))
                            tint_rect(r, x + 2 + px * 3, sy + 8 - lift + py * 3, 3, 3, photo_colour(c));
            }
        }
        const int size_x = ox + 16 + count * pitch + 8, chosen = pen ? e.pen_size : e.stamp_size;
        for (int i = 0; i < 3; i++) {
            const int x = size_x + i * 34;
            settings_card(r, x, sy + 6, 32, 40, chosen == i);
            const int dot = 4 + i * 4;
            fill_round(r, x + 16 - dot / 2, sy + 26 - dot / 2, dot, dot, dot / 2,
                       theme.at(chosen == i ? "accent" : "muted"));
        }
        if (pen) {
            // Clear all: a card of its own after the sizes.
            const int x = ox + photo_clear_x;
            settings_card(r, x, sy + 6, photo_clear_w, 40, false);
            ui_icon(r, SettingsIcon::Spark, x + photo_clear_w / 2, sy + 20, 16, "muted");
            label(r, "Clear", x + (photo_clear_w - std::min(photo_clear_w - 6, label_width("Clear", 12))) / 2, sy + 30,
                  12, "muted", photo_clear_w - 6);
        }
    } else {
        settings_card(r, ox + 16, sy + 4, 608, 44);
        const std::string value = e.tool == photo::Tool::Frame  ? e.frame_title()
                                  : e.tool == photo::Tool::Lens ? std::string(photo::lens_names[size_t(e.lens)])
                                  : std::string(photo_editors[size_t(photo_pages - 1)].date ? "On" : "Off");
        ui_icon(r, SettingsIcon::Back, ox + 44, sy + 26, 20, "accent");
        ui_icon(r, SettingsIcon::Chevron, ox + 596, sy + 26, 20, "accent");
        const int width = std::min(500, label_width(value, 20));
        label(r, value, ox + 320 - width / 2, sy + 13, 20, "ink", 500);
    }
    s.RenderSetClipRect(r, nullptr);

    // The footer. One screen: Back, the keys, Save. A stitched photo: Back, Undo, the Upper/Lower switch, Save.
    const int fy = y + 424 + int(std::lround((1 - rise) * 60));
    rect(r, 0, fy, 640, 56, "background");
    rect(r, 24, fy, 592, 1, "line");
    const bool held = pointer_down && contact_y >= 424;
    if (photo_pages > 1) {
        footer_button(r, 12, fy + 8, photo_back_w, "Back", "B", false, held && contact_x < photo_back_w + 12);
        const bool undo_held = held && contact_x >= photo_back_w + 12 && contact_x < photo_switch_x - 4;
        secondary_button(r, photo_undo_x, fy + 8, photo_undo_w, 40, 20, undo_held);
        ui_icon(r, SettingsIcon::Back, photo_undo_x + photo_undo_w / 2, fy + 28 + (undo_held ? 1 : 0), 18,
                e.can_undo() ? "ink" : "muted");
        // The switch: a sunken track with a pill that glides to the screen on the desk.
        sunken_track(r, photo_switch_x, fy + 8, photo_switch_w, 40, 20);
        const double t = photo_ease_out(progress(photo_page_at, photo_page_ms));
        const double at = photo_page_from + (photo_page - photo_page_from) * t;
        const int half = photo_switch_w / 2;
        raised_pill(r, photo_switch_x + 3 + int(std::lround(at * (half - 3))), fy + 11, half - 3, 34, 17,
                    theme.at("accent"), 0, false);
        for (int page = 0; page < 2; page++) {
            const std::string name = page ? "Lower screen" : "Upper screen";
            const int w = std::min(half - 12, label_width(name, 16));
            label(r, name, photo_switch_x + page * half + (half - w) / 2, centred_label_y(locale.text(name), 16, fy + 28),
                  16, std::abs(at - page) < .5 ? "on_accent" : "ink", half - 12);
        }
    } else {
        footer_button(r, 12, fy + 8, 160, "Back", "B", false, footer_pressed(true));
        key_hints(r, 320, fy + 28, photo_hints(true), 0, 16, 280);
    }
    footer_button(r, 468, fy + 8, 160, "Save", "", true, footer_pressed(false));

    // Save prints the photo: a white-bordered print that shrinks, tilts and drops out over everything else.
    if (photo_save_at) {
        const double t = progress(photo_save_at, photo_save_ms);
        const double shrink = 1 - .3 * photo_ease_out(std::min(1.0, t * 2.2)),
                     drop = t < .3 ? 0 : std::pow((t - .3) / .7, 2) * 560, angle = -9 * photo_ease_out(t);
        const SDL_Rect print{photo_desk.x + int(photo_desk.w * (1 - shrink) / 2) - 8,
                             y + photo_desk.y + int(photo_desk.h * (1 - shrink) / 2) - 8 + int(drop),
                             int(photo_desk.w * shrink) + 16, int(photo_desk.h * shrink) + 16};
        const SDL_Point centre{print.w / 2, print.h / 2};
        auto& paper = photo_textures[{r, 3}]; // one white pixel, stretched and turned for the border and shadow
        if (!paper) {
            paper = s.CreateTexture(r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, 1, 1);
            const uint32_t white = 0xFFFFFFFFu;
            if (paper) {
                s.UpdateTexture(paper, nullptr, &white, 4);
                s.SetTextureBlendMode(paper, SDL_BLENDMODE_BLEND);
            }
        }
        if (paper) {
            const SDL_Rect shadow{print.x + 5, print.y + 9, print.w, print.h};
            s.SetTextureColorMod(paper, 0, 0, 0);
            s.SetTextureAlphaMod(paper, 60);
            s.RenderCopyEx(r, paper, nullptr, &shadow, angle, &centre, SDL_FLIP_NONE);
            s.SetTextureColorMod(paper, 250, 250, 246);
            s.SetTextureAlphaMod(paper, 255);
            s.RenderCopyEx(r, paper, nullptr, &print, angle, &centre, SDL_FLIP_NONE);
        }
        if (auto* texture = photo_texture(r, photo_page)) {
            // The photo turns about the print's centre, so it stays inside its border.
            const SDL_Rect inner{print.x + 8, print.y + 8, print.w - 16, print.h - 16};
            const SDL_Point inner_centre{inner.w / 2, inner.h / 2};
            s.SetTextureAlphaMod(texture, 255);
            s.RenderCopyEx(r, texture, nullptr, &inner, angle, &inner_centre, SDL_FLIP_NONE);
        }
    }
}

// Album > Send in PictoChat: the picture's upper screen as a pad picture (pictochat::make_picture), waiting on the
// pad for the next room you join.
void App::album_to_pictochat() {
    std::vector<uint32_t> source;
    if (!read_photo_bmp(o.data / "session/album-view-top.bmp", source)) {
        emit_sound(MenuCue::Error);
        status = "This picture could not be opened.";
        dirty = true;
        return;
    }
    chat.pending_picture = {pictochat::make_picture(source.data(), photo::W, photo::H, photo::W, 2, 0)};
    album_viewing = false;
    open_pictochat();
    chat.status = "Join a room: your picture goes on the pad.";
    dirty = true;
}
