#pragma once
// Photo mode (Album > Y > Photo mode, or A on a picture you are viewing): the lower screen is the desk you draw on,
// the upper screen shows the finished photo as it will be saved. The pictures are photo_mode.hpp's; this file is the
// page around them. A saved photo is a new picture in the same game's album; the screenshot is kept.
// Members of App in main.cpp; included once, right after the class.

namespace {
constexpr SDL_Rect photo_desk{16, 16, 448, 336}; // the picture on the lower screen (0.7 of the panel)
constexpr int photo_rail_x = 480, photo_rail_w = 144, photo_tool_h = 60, photo_tool_pitch = 68;
constexpr int photo_strip_y = 362, photo_strip_h = 52;
// Swatches: ten inks leave room at 44 px; twelve stamps need 42 so the three sizes still fit.
int photo_swatch_pitch(photo::Tool tool) { return tool == photo::Tool::Pen ? 44 : 42; }

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
    std::vector<uint32_t> source;
    const auto view = o.data / (photo_lower ? "session/album-view-bottom.bmp" : "session/album-view-top.bmp");
    if (!read_photo_bmp(view, source) && !read_photo_bmp(o.data / "session/album-view-top.bmp", source)) {
        emit_sound(MenuCue::Error);
        status = "This picture could not be opened.";
        dirty = true;
        return;
    }
    if (!photo_font) {
        photo_font = std::make_unique<play::Font>();
        photo_font->load(asset_path("fonts").string());
    }
    photo_editor.font = photo_font.get();
    const auto& d = photo_shot.date;
    photo_editor.open(std::move(source), d.size() == 8 ? std::atoi(d.substr(0, 4).c_str()) : 2026,
                      d.size() == 8 ? std::atoi(d.substr(4, 2).c_str()) : 1,
                      d.size() == 8 ? std::atoi(d.substr(6, 2).c_str()) : 1);
    // The date reads the way the player writes dates (System > Date format).
    photo_editor.date_order = system_preferences.date_format == 1 ? 1 : system_preferences.date_format == 3 ? 2 : 0;
    photo_editor.tool = photo::Tool::Pen;
    photo_pen_down = photo_cursor_shown = false;
    photo_cursor_x = photo::W / 2, photo_cursor_y = photo::H / 2;
    photo_version++;
    album_viewing = false;
    settings.page = Page::Photo;
    emit_sound(MenuCue::Open);
    dirty = true;
}

void App::photo_close() {
    for (auto& [renderer, texture] : photo_textures)
        if (texture) s.DestroyTexture(texture);
    photo_textures.clear();
    photo_uploaded.clear();
    settings.page = Page::Album;
    dirty = true;
}

void App::photo_leave() {
    if (!photo_editor.edited) {
        photo_close();
        emit_sound(MenuCue::Back);
        return;
    }
    show_picker("Leave photo mode?", {"Keep editing", "Leave without saving", "Save"}, 0, [this](int choice) {
        if (choice == 1) photo_close();
        else if (choice == 2) photo_save();
    });
}

void App::photo_save() {
    const auto path = o.data / "session/photo-edit.bmp";
    if (album_child > 0 || !write_photo_bmp(path, photo_editor.compose())) {
        emit_sound(MenuCue::Error);
        return;
    }
    std::vector<std::string> command = {"save", "--top", path.string(), "--key", photo_shot.key, "--screens", "top"};
    if (!photo_shot.title.empty() && photo_shot.key != "launcher")
        command.insert(command.end(), {"--title", photo_shot.title});
    if (!album_helper(command, "photo-save")) {
        emit_sound(MenuCue::Error);
        return;
    }
    shutter_at = std::max<Uint32>(1, s.GetTicks());
    emit_sound(MenuCue::Save);
    photo_saved_key = photo_shot.key;
    photo_close();
}

// Picture pixels for a point on the lower screen, or false outside the desk.
bool App::photo_point(int x, int y, double& px, double& py) const {
    px = (x - photo_desk.x) * double(photo::W) / photo_desk.w;
    py = (y - photo_desk.y) * double(photo::H) / photo_desk.h;
    return x >= photo_desk.x && x < photo_desk.x + photo_desk.w && y >= photo_desk.y && y < photo_desk.y + photo_desk.h;
}

int App::photo_option_count() const {
    switch (photo_editor.tool) {
    case photo::Tool::Pen: return int(photo::ink_names.size()) + int(photo::pen_sizes.size());
    case photo::Tool::Stamp: return int(photo::stamps.size()) + int(photo::stamp_scales.size());
    default: return 0;
    }
}

// The option strip: a row of swatches (pen, stamps) or one wide picker (frames, lenses, the date).
void App::photo_strip_press(int x) {
    auto& e = photo_editor;
    if (e.tool == photo::Tool::Pen || e.tool == photo::Tool::Stamp) {
        const int count = e.tool == photo::Tool::Pen ? int(photo::ink_names.size()) : int(photo::stamps.size());
        const int pitch = photo_swatch_pitch(e.tool), swatch = (x - 16) / pitch;
        if (x >= 16 && swatch < count) (e.tool == photo::Tool::Pen ? e.ink : e.stamp) = swatch;
        else if (x >= 16 + count * pitch + 8) {
            const int size = std::clamp((x - (16 + count * pitch + 8)) / 34, 0, 2);
            (e.tool == photo::Tool::Pen ? e.pen_size : e.stamp_size) = size;
        }
    } else photo_step(x < 320 ? -1 : 1);
    emit_sound(MenuCue::Navigate);
    photo_version++;
    dirty = true;
}

// Y (or the strip's arrows): the next ink, stamp, frame or lens; the date stamp on or off.
void App::photo_step(int delta) {
    auto& e = photo_editor;
    auto wrap = [](int v, int n) { return ((v % n) + n) % n; };
    switch (e.tool) {
    case photo::Tool::Pen: e.ink = wrap(e.ink + delta, int(photo::ink_names.size())); break;
    case photo::Tool::Stamp: e.stamp = wrap(e.stamp + delta, int(photo::stamps.size())); break;
    case photo::Tool::Frame: e.set_frame(wrap(e.frame + delta, e.frames())); break;
    case photo::Tool::Lens: e.set_lens(photo::Lens(wrap(int(e.lens) + delta, int(photo::Lens::Count)))); break;
    default: e.set_date(!e.date);
    }
    photo_version++;
    dirty = true;
}

void App::photo_action(int code) {
    auto& e = photo_editor;
    const int step = 16;
    switch (code) {
    case 3: photo_leave(); return;
    case 4: photo_save(); return;
    case 11: emit_sound(e.undo() ? MenuCue::Back : MenuCue::Error); break;
    case 12: photo_step(1); return;
    case 13:
    case 14:
        e.tool = photo::Tool((int(e.tool) + (code == 13 ? int(photo::Tool::Count) - 1 : 1)) % int(photo::Tool::Count));
        photo_pen_down = false;
        emit_sound(MenuCue::Navigate);
        break;
    case 0:
    case 1:
    case 8:
    case 9: {
        // The D-pad moves a cursor over the picture; with the pen down (A) it draws as it goes.
        if (e.tool == photo::Tool::Frame || e.tool == photo::Tool::Lens || e.tool == photo::Tool::Date) {
            if (code == 0 || code == 1) photo_step(code == 0 ? -1 : 1);
            return;
        }
        photo_cursor_shown = true;
        photo_cursor_x = std::clamp(photo_cursor_x + (code == 0 ? -step : code == 1 ? step : 0), 0, photo::W - 1);
        photo_cursor_y = std::clamp(photo_cursor_y + (code == 8 ? -step : code == 9 ? step : 0), 0, photo::H - 1);
        if (photo_pen_down && e.tool == photo::Tool::Pen) e.stroke(photo_cursor_x, photo_cursor_y);
        break;
    }
    case 2:
        if (e.tool == photo::Tool::Pen) {
            photo_pen_down = !photo_pen_down;
            photo_cursor_shown = true;
            if (photo_pen_down) e.begin(photo_cursor_x, photo_cursor_y);
        } else if (e.tool == photo::Tool::Stamp) {
            photo_cursor_shown = true;
            e.place_stamp(photo_cursor_x, photo_cursor_y);
            emit_sound(MenuCue::Confirm);
        } else photo_step(1);
        break;
    default: return;
    }
    photo_version++;
    dirty = true;
}

void App::photo_pointer(int x, int y, int phase) {
    auto& e = photo_editor;
    double px = 0, py = 0;
    const bool on_desk = photo_point(x, y, px, py);
    if (phase == 0) {
        photo_touch_drawing = false;
        photo_cursor_shown = false;
        if (on_desk) {
            if (e.tool == photo::Tool::Stamp) {
                e.place_stamp(px, py);
                emit_sound(MenuCue::Confirm);
            } else if (e.tool == photo::Tool::Pen) {
                e.begin(px, py);
                photo_touch_drawing = true;
            } else photo_step(1);
        } else if (x >= photo_rail_x && y >= photo_desk.y && y < photo_desk.y + 5 * photo_tool_pitch) {
            const int tool = (y - photo_desk.y) / photo_tool_pitch;
            if ((y - photo_desk.y) % photo_tool_pitch < photo_tool_h && tool < int(photo::Tool::Count)) {
                e.tool = photo::Tool(tool);
                emit_sound(MenuCue::Navigate);
            }
        } else if (y >= photo_strip_y && y < photo_strip_y + photo_strip_h) photo_strip_press(x);
        else if (y >= 424 && x < 200) photo_leave();
        else if (y >= 424 && x >= 440) photo_save();
        else if (y >= 424) emit_sound(e.undo() ? MenuCue::Back : MenuCue::Error);
    } else if (phase == 1 && photo_touch_drawing) {
        e.stroke(std::clamp(px, 0.0, photo::W - 1.0), std::clamp(py, 0.0, photo::H - 1.0));
    } else if (phase == 2) photo_touch_drawing = false;
    photo_version++;
    dirty = true;
}

// The composed photo as a texture on `r`, uploaded again only when it changed.
SDL_Texture* App::photo_texture(SDL_Renderer* r) {
    auto& texture = photo_textures[r];
    if (!texture) {
        texture = s.CreateTexture(r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, photo::W, photo::H);
        if (!texture) return nullptr;
        photo_uploaded[r] = ~0u;
    }
    if (photo_uploaded[r] != photo_version) {
        const auto& px = photo_editor.compose();
        s.UpdateTexture(texture, nullptr, px.data(), photo::W * 4);
        photo_uploaded[r] = photo_version;
    }
    return texture;
}

void App::photo_top(SDL_Renderer* r, int y) {
    SDL_Rect all{0, y, 640, 480};
    if (auto* texture = photo_texture(r)) s.RenderCopy(r, texture, nullptr, &all);
}

void App::photo_bottom(SDL_Renderer* r, int y) {
    auto& e = photo_editor;
    rect(r, 0, y, 640, 480, "background");
    pattern(r, y, 480, true);
    // The desk: the picture with a card's edge round it.
    settings_card(r, photo_desk.x - 6, y + photo_desk.y - 6, photo_desk.w + 12, photo_desk.h + 12);
    SDL_Rect desk{photo_desk.x, y + photo_desk.y, photo_desk.w, photo_desk.h};
    if (auto* texture = photo_texture(r)) s.RenderCopy(r, texture, nullptr, &desk);
    if (photo_cursor_shown && (e.tool == photo::Tool::Pen || e.tool == photo::Tool::Stamp)) {
        const int cx = photo_desk.x + photo_cursor_x * photo_desk.w / photo::W,
                  cy = y + photo_desk.y + photo_cursor_y * photo_desk.h / photo::H;
        const Color ink = photo_pen_down ? theme.at("accent") : Color{255, 255, 255};
        for (int outline = 1; outline >= 0; outline--) {
            const Color c = outline ? Color{20, 20, 24} : ink;
            tint_rect(r, cx - 10 - outline, cy - 1 - outline, 21 + 2 * outline, 3 + 2 * outline, c);
            tint_rect(r, cx - 1 - outline, cy - 10 - outline, 3 + 2 * outline, 21 + 2 * outline, c);
        }
    }
    // The tools, top to bottom.
    static const std::array<SettingsIcon, size_t(photo::Tool::Count)> icons = {
        SettingsIcon::Type, SettingsIcon::Heart, SettingsIcon::Art, SettingsIcon::Shader, SettingsIcon::Clock};
    for (int i = 0; i < int(photo::Tool::Count); i++) {
        const int ty = y + photo_desk.y + i * photo_tool_pitch;
        const bool on = int(e.tool) == i;
        settings_card(r, photo_rail_x, ty, photo_rail_w, photo_tool_h, on);
        ui_icon(r, icons[size_t(i)], photo_rail_x + 26, ty + photo_tool_h / 2, 24, on ? "accent" : "muted");
        label(r, photo::tool_names[size_t(i)], photo_rail_x + 46, ty + 18, 16, on ? "ink" : "muted", photo_rail_w - 52);
    }
    // The strip for the tool in hand.
    const int sy = y + photo_strip_y;
    if (e.tool == photo::Tool::Pen || e.tool == photo::Tool::Stamp) {
        const bool pen = e.tool == photo::Tool::Pen;
        const int count = pen ? int(photo::ink_names.size()) : int(photo::stamps.size());
        const int pitch = photo_swatch_pitch(e.tool);
        for (int i = 0; i < count; i++) {
            const int x = 16 + i * pitch;
            const bool on = (pen ? e.ink : e.stamp) == i;
            settings_card(r, x, sy + 6, 40, 40, on);
            if (pen && i < int(photo::inks.size())) {
                const uint32_t c = photo::inks[size_t(i)];
                fill_round(
                    r, x + 8, sy + 14, 24, 24, 12,
                    Color{(unsigned char)photo::red(c), (unsigned char)photo::green(c), (unsigned char)photo::blue(c)});
            } else if (pen && i == photo::rainbow_ink) {
                for (int b = 0; b < 6; b++) {
                    const uint32_t c = photo::rainbow(b / 6.0);
                    tint_rect(r, x + 8 + b * 4, sy + 14, 4, 24,
                              Color{(unsigned char)photo::red(c), (unsigned char)photo::green(c),
                                    (unsigned char)photo::blue(c)});
                }
            } else if (pen) {
                ui_icon(r, SettingsIcon::Trash, x + 20, sy + 26, 22, on ? "accent" : "muted");
            } else {
                const auto& stamp = photo::stamps[size_t(i)];
                for (int py = 0; py < 12; py++)
                    for (int px = 0; px < 12; px++)
                        if (const uint32_t c = photo::stamp_colour(stamp.rows[size_t(py)][px]))
                            tint_rect(r, x + 2 + px * 3, sy + 8 + py * 3, 3, 3,
                                      Color{(unsigned char)photo::red(c), (unsigned char)photo::green(c),
                                            (unsigned char)photo::blue(c)});
            }
        }
        const int size_x = 16 + count * pitch + 8, chosen = pen ? e.pen_size : e.stamp_size;
        for (int i = 0; i < 3; i++) {
            const int x = size_x + i * 34;
            settings_card(r, x, sy + 6, 32, 40, chosen == i);
            const int dot = 4 + i * 4;
            fill_round(r, x + 16 - dot / 2, sy + 26 - dot / 2, dot, dot, dot / 2,
                       theme.at(chosen == i ? "accent" : "muted"));
        }
    } else {
        settings_card(r, 16, sy + 4, 608, 44);
        const std::string value = e.tool == photo::Tool::Frame  ? e.frame_title()
                                  : e.tool == photo::Tool::Lens ? std::string(photo::lens_names[size_t(e.lens)])
                                                                : std::string(e.date ? "On" : "Off");
        ui_icon(r, SettingsIcon::Back, 44, sy + 26, 20, "accent");
        ui_icon(r, SettingsIcon::Chevron, 596, sy + 26, 20, "accent");
        const int width = std::min(500, label_width(value, 20));
        label(r, value, 320 - width / 2, sy + 13, 20, "ink", 500);
    }
    // The footer: Back, the keys, Save.
    rect(r, 0, y + 424, 640, 56, "background");
    rect(r, 24, y + 424, 592, 1, "line");
    footer_button(r, 12, y + 432, 160, "Back", "B", false, footer_pressed(true));
    key_hints(r, 320, y + 452, {{"X", "Undo"}, {"L R", "Tools"}}, 0, 16, 280);
    footer_button(r, 468, y + 432, 160, "Save", "", true, footer_pressed(false));
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
