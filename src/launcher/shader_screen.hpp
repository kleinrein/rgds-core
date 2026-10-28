#pragma once
// Game menu > Shader: the picker state, families and the preview carousel.
// Members of App in main.cpp; included once, right after the class.

void App::open_shader_picker(bool per_game) {
    if (superdrastic_game()) {
        status = "SuperDrastic currently uses native output. Your standard shader choice is kept.";
        dirty = true;
        return;
    }
    shader_per_game = per_game;
    shader_return = per_game ? Page::GameOptions : Page::LibraryOptions;
    shader_core = o.game_menu                              ? active_engine()
                  : per_game && game_preferences.core >= 0 ? game_preferences.core
                                                           : int(preferences.core);
    shader_choices.clear();
    if (per_game) shader_choices.push_back(-1);
    shader_system = !o.game_menu && !per_game ? "nds" : picker_game_system();
    for (auto place : shader_order)
        if (shader_supported(place.id, shader_core) && shader_fits_system(place.id, shader_system))
            shader_choices.push_back(place.id);
    int selected = per_game ? game_preferences.filter : preferences.filter;
    auto found = std::find(shader_choices.begin(), shader_choices.end(), selected);
    settings.page = Page::ShaderPicker;
    settings.focus = found == shader_choices.end() ? 0 : int(found - shader_choices.begin());
    shader_carousel.reset(size_t(settings.focus));
    shader_drag = false;
    dirty = true;
}

std::string App::picker_game_system() {
    if (model.games.empty()) return "nds";
    const Game& game = model.games.at(o.game_menu ? 0 : size_t(model.selected));
    std::string system = game.system != "nds" ? game.system : system_for_path(game.path);
    // A zipped game: RG DS Play is never a DS engine.
    if (system == "nds" && (o.game_menu ? active_engine() == play_core : other_system_selected())) system = "gba";
    return system;
}

void App::close_shader_picker() {
    settings.page = shader_return;
    settings.focus = shader_return == Page::QuickMenu      ? 3
                     : shader_return == Page::Graphics     ? 4
                     : shader_return == Page::PlayGraphics ? 3
                     : shader_per_game                     ? 2
                                                           : 4;
    dirty = true;
}

fs::path App::shader_preview_folder() const {
    return o.app.empty() ? fs::path("assets/shader-previews") : o.app / "assets/shader-previews";
}

int App::preview_shader() const {
    int selected =
        shader_choices.empty() ? 0 : shader_choices.at(std::clamp(settings.focus, 0, int(shader_choices.size()) - 1));
    return selected < 0 ? default_shader_for(shader_system.c_str(), preferences.filter) : selected;
}

void App::change_shader_preview(int code) {
    if (code == 11) shader_preview_scene = (shader_preview_scene + 1) % ShaderPreview::scene_count;
    else if (code == 12) shader_preview_mode = ShaderPreviewMode((int(shader_preview_mode) + 1) % 3);
    else {
        shader_preview_mode = ShaderPreviewMode::Split;
        shader_preview_split = std::clamp(shader_preview_split + (code == 13 ? -.1f : .1f), .1f, .9f);
    }
    dirty = true;
}

int App::shader_choice_family(int index) const {
    if (shader_choices.empty()) return 0;
    int value = shader_choices.at(std::clamp(index, 0, int(shader_choices.size()) - 1));
    return value < 0 ? 0 : shader_family(value);
}

void App::focus_shader(int index) {
    if (shader_choices.empty()) return;
    index = std::clamp(index, 0, int(shader_choices.size()) - 1);
    if (index != settings.focus) {
        settings.focus = index;
        selected_at = s.GetTicks();
    }
    if (preferences.animations) shader_carousel.select(size_t(index), s.GetTicks());
    else shader_carousel.reset(size_t(index));
    dirty = true;
}

void App::shader_family_jump(int family) {
    for (int i = 0; i < int(shader_choices.size()); i++)
        if (shader_choice_family(i) == family) {
            focus_shader(i);
            return;
        }
}

void App::shader_family_step(int direction) {
    const int family = shader_choice_family(settings.focus);
    for (int step = 1; step <= shader_family_count; step++) {
        const int wanted =
            ((family + direction * step) % shader_family_count + shader_family_count) % shader_family_count;
        for (int i = 0; i < int(shader_choices.size()); i++)
            if (shader_choice_family(i) == wanted) {
                focus_shader(i);
                return;
            }
    }
}

std::vector<int> App::shader_families_shown() {
    std::vector<int> shown;
    for (int f = 0; f < shader_family_count; f++)
        for (int i = 0; i < int(shader_choices.size()); i++)
            if (shader_choice_family(i) == f) {
                shown.push_back(f);
                break;
            }
    return shown;
}

bool App::shader_pointer_down(int x, int y) {
    const size_t count = shader_choices.size();
    if (!count) return false;
    if (y >= 409 && y < 480) {
        if (x < RailGeometry::left) {
            focus_shader(settings.focus - 1);
            return true;
        }
        if (x >= RailGeometry::left + RailGeometry::width) {
            focus_shader(settings.focus + 1);
            return true;
        }
        shader_carousel.seek_down(x, y, count, s.GetTicks());
        shader_drag = true;
        int index = int(shader_carousel.drag(x, count));
        if (index != settings.focus) {
            settings.focus = index;
            selected_at = s.GetTicks();
        }
        dirty = true;
        return true;
    }
    if (y >= 128 && y < 330) {
        shader_carousel.down(x, y, false, s.GetTicks());
        shader_drag = true;
        dirty = true;
        return true;
    }
    return false;
}

void App::render_shader_preview(SDL_Renderer* r, int y) {
    int selected = preview_shader();
    const int focused = shader_choices.empty() ? 0 : shader_choices.at(settings.focus);
    const bool in_use = (shader_per_game ? game_preferences.filter : preferences.filter) == focused;
    const std::string group =
        locale.text("Shader studio") + "  ·  " + locale.text(shader_family_names[shader_choice_family(settings.focus)]);
    raw_label(r, group, 30, y + 65, 16, "muted", in_use ? 440 : 580);
    label(r, shader_name(focused), 30, y + 90, 28, "ink", in_use ? 440 : 570);
    if (in_use) {
        const std::string text = locale.text("IN USE");
        const int w = label_width(text, 14, false) + 28;
        raised_pill(r, 616 - w, y + 94, w, 30, 15, theme.at("accent"));
        raw_label(r, text, 616 - w + 14, centred_label_y(text, 14, y + 109), 14, "on_accent", w - 20);
    }
    round_rect(r, 24, y + 137, 378, 282, 14, "line");
    round_rect(r, 26, y + 139, 374, 278, 12, "panel");
    SDL_Rect frame{30, y + 143, 366, 274};
    bool ready = shader_preview.draw(r, shader_preview_folder(), shader_preview_scene, selected, frame,
                                     shader_preview_mode, shader_preview_split);
    if (!ready) {
        wrapped(r, "Preview unavailable", 58, y + 251, 24, 314, 2);
        label(r, "Your shader choice is still available.", 42, y + 302, 16, "muted", 340);
    }
    round_rect(r, 416, y + 137, 200, 160, 12, "line");
    round_rect(r, 418, y + 139, 196, 156, 10, "panel");
    SDL_Rect detail{422, y + 145, 188, 141};
    shader_preview.detail(r, shader_preview_folder(), shader_preview_scene, selected, detail, shader_preview_mode,
                          shader_preview_split);
    label(r, "Magnified detail", 420, y + 308, 16, "muted", 188);
    label(r, ShaderPreview::scene_name(shader_preview_scene), 420, y + 334, 20, "ink", 188);
    label(r,
          shader_preview_mode == ShaderPreviewMode::Split      ? "Before / after"
          : shader_preview_mode == ShaderPreviewMode::Original ? "Original pixels"
                                                               : "With shader",
          420, y + 365, 16, "accent", 188);
    key_hints(r, 420, y + 403, {{"X", "Scene"}, {"Y", "Compare"}}, -1, 16, 192);
    if (ready && shader_preview_mode == ShaderPreviewMode::Split) {
        int x = frame.x + int(frame.w * shader_preview_split);
        rect(r, x, y + 143, 2, 274, "on_accent");
        round_rect(r, frame.x + 7, y + 150, 68, 24, 5, "panel");
        label(r, "Before", frame.x + 14, y + 152, 16, "ink", 60);
        round_rect(r, frame.x + frame.w - 73, y + 150, 66, 24, 5, "panel");
        label(r, "After", frame.x + frame.w - 64, y + 152, 16, "ink", 59);
    }
    std::string note = status.rfind("Could not save", 0) == 0           ? status
                       : o.game_menu && !menu_status["message"].empty() ? menu_status["message"]
                                                                        : "";
    if (note.empty()) key_hints(r, 30, y + 448, {{"L R", "Change group"}});
    else wrapped(r, note, 30, y + 433, 16, 580, 2);
}

void App::render_shader_carousel(SDL_Renderer* r, int y) {
    settings_header(r, y, "Shaders");
    secondary_button(r, 368, y + 24, 120, 42, 12,
                     pointer_down && contact_x >= 368 && contact_x <= 488 && contact_y >= 24 && contact_y <= 66);
    ui_icon(r, SettingsIcon::Art, 386, y + 45, 18, "accent");
    label(r, "Scene", 403, y + 35, 17, "ink", 74);
    secondary_button(r, 496, y + 24, 128, 42, 12,
                     pointer_down && contact_x >= 496 && contact_y >= 24 && contact_y <= 66);
    ui_icon(r, SettingsIcon::Shader, 514, y + 45, 18, "accent");
    label(r, "Compare", 530, y + 35, 16, "ink", 85);
    const int count = int(shader_choices.size()), selection = std::clamp(settings.focus, 0, std::max(0, count - 1)),
              family = shader_choice_family(selection);
    const auto shown = shader_families_shown();
    for (size_t place = 0; place < shown.size(); place++) {
        const int f = shown[place];
        const auto chip = shader_chip(int(place));
        const bool on = f == family, pressed = pointer_down && !shader_drag && point_in(chip, down_x, down_y) &&
                                               point_in(chip, contact_x, contact_y);
        if (on) raised_pill(r, chip.x, y + chip.y, chip.w, chip.h, 16, theme.at("accent"), pressed ? 1 : 0);
        else secondary_button(r, chip.x, y + chip.y, chip.w, chip.h, 16, pressed);
        const std::string text = locale.text(shader_family_names[f]);
        const int w = std::min(chip.w - 14, label_width(text, 14, false)), sink = pressed ? 2 : 0;
        raw_label(r, text, chip.x + (chip.w - w) / 2, centred_label_y(text, 14, y + chip.y + chip.h / 2 + sink), 14,
                  on ? "on_accent" : "ink", chip.w - 14);
    }
    // The Home tile, frame and action label; each tile shows its effect at 1:1.
    SDL_Rect clip{0, y + 124, 640, 206};
    s.RenderSetClipRect(r, &clip);
    const int tile = 128;
    int budget = 2;
    bool pending = false;
    const int first = int(std::floor(shader_carousel.position)) - 3;
    const int current = shader_per_game ? game_preferences.filter : preferences.filter;
    for (int pass = 0; pass < 2; pass++)
        for (int offset = 0; offset < 8; offset++) {
            const int index = first + offset;
            if (index < 0 || index >= count || (index == selection) != (pass == 1)) continue;
            const double distance = index - shader_carousel.position;
            const int x = int(std::lround(320 + distance * Carousel::spacing)) - tile / 2;
            if (x >= 640 || x + tile <= 0) continue;
            const bool selected = index == selection;
            const bool pressed = selected && pointer_down && shader_drag && !shader_carousel.moved &&
                                 !shader_carousel.range &&
                                 int(shader_carousel.tapped(down_x, size_t(count))) == selection;
            const int lift = pressed ? -2
                             : selected && preferences.animations
                                 ? int(std::max(0.0, 1.0 - (s.GetTicks() - selected_at) / 190.0) * 4)
                                 : 0,
                      row = y + 144 - lift;
            const int value = shader_choices[size_t(index)],
                      shown = value < 0 ? default_shader_for(shader_system.c_str(), preferences.filter) : value;
            if (selected) start_frame(r, x, row, tile, value == current ? "IN USE" : "APPLY", 30);
            round_rect(r, x, row + 4, tile, tile, 5, "line");
            round_rect(r, x, row, tile, tile, 5, "line");
            round_rect(r, x + 2, row + 2, tile - 4, tile - 4, 4, "panel");
            if (!shader_preview.thumb(r, shader_preview_folder(), shader_preview_scene, shown,
                                      {x + 4, row + 4, tile - 8, tile - 8}, budget)) {
                ui_icon(r, SettingsIcon::Shader, x + tile / 2, row + tile / 2, 44, "muted");
                if (!shader_preview.thumb_known(r, shader_preview_folder(), shader_preview_scene, shown))
                    pending = true;
            }
            if (value == current && !selected) {
                fill_round(r, x + tile - 24, row + 8, 16, 16, 8, theme.at("panel"));
                fill_round(r, x + tile - 22, row + 10, 12, 12, 6, theme.at("accent"));
            }
        }
    s.RenderSetClipRect(r, nullptr);
    shader_thumbs_pending = pending;
    if (!count) return;
    const int value = shader_choices[size_t(selection)];
    const std::string name = locale.text(shader_name(value));
    const int name_w = std::min(592, label_width(name, 22, false));
    raw_label(r, name, 320 - name_w / 2, y + 330, 22, "ink", 592);
    const std::string note =
        locale.text(value < 0 && system_default_shader(shader_system.c_str()) >= 0 ? "This handheld's own screen."
                    : value < 0                                                    ? "Follows your library shader."
                    : value == 0                                                   ? "Off keeps the original picture."
                                                                                   : shaders.at(value).description);
    const int note_w = label_width(note, 16, false);
    if (note_w <= 592) raw_label(r, note, 320 - note_w / 2, y + 364, 16, "muted", 592);
    else wrapped(r, note, 24, y + 358, 16, 592, 2, false);
    const bool held = pointer_down && !shader_drag;
    draw_rail(r, y, size_t(count), shader_carousel.position,
              {held && down_y >= 409 && down_x < RailGeometry::left && contact_x < RailGeometry::left ? 1.0 : 0.0,
               held && down_y >= 409 && down_x >= RailGeometry::left + RailGeometry::width &&
                       contact_x >= RailGeometry::left + RailGeometry::width
                   ? 1.0
                   : 0.0,
               shader_drag && shader_carousel.range ? 1.0 : 0.0},
              false);
}

void App::choose_shader() {
    if (o.game_menu && menu_status["state"] == "busy") {
        dirty = true;
        return;
    }
    int selected = shader_choices.at(settings.focus);
    if (shader_per_game) {
        auto next = game_preferences;
        next.filter = selected;
        if (!apply_game_preferences(next, "Shader saved for next launch")) return;
    } else {
        preferences.filter = selected;
        remember_appearance();
    }
    if (o.game_menu) {
        // Default: the global choice, or for another system's game its own screen; never another
        // system's screen (as emulators.py launches it).
        int effective = selected >= 0 ? selected : default_shader_for(shader_system.c_str(), preferences.filter);
        if (!shader_fits_system(effective, shader_system)) effective = 0;
        if (!shader_supported(effective, shader_core)) effective = 0;
        menu_request("shader:" + std::to_string(effective));
        dirty = true;
    } else close_shader_picker();
}
