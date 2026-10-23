#pragma once
// View transitions: the slide/zoom motion between pages and the input captured while it runs.
// Members of App in main.cpp; included once, right after the class.

void App::motion_input(int code) {
    view_route = (code == 3 || (code == 7 && settings.page != Page::Library) ||
                  (code == 2 && settings.page == Page::Display) || (code == 4 && settings.page == Page::Keyboard))
                     ? ViewTravel::Back
                 : code == 0 ? ViewTravel::Left
                 : code == 1 ? ViewTravel::Right
                             : ViewTravel::Forward;
    if (view_transitions_enabled() && preferences.view_style == ViewStyle::Spring)
        view_motion.interact(s.GetTicks(), motion_touch ? contact_x : 320, motion_touch ? contact_y : 240);
}

void App::move_element(uint32_t id, int& x, int& y, int width, int height, bool focused, int clip_top, int clip_bottom,
                       int clip_left, int clip_right) {
    if (!drawing_elements || !view_transitions_enabled() || layered(preferences.view_style)) return;
    MotionRect bounds{x, y - element_origin_y, width, height};
    auto offset = view_motion.element(id, bounds, focused, view_drawn_at);
    if (pointer_down && bounds.contains(contact_x, contact_y)) offset.y += 3;
    motion_hits.add(bounds, offset.x, offset.y, clip_top, clip_bottom, clip_left, clip_right);
    x += offset.x;
    y += offset.y;
    if (offset.x || offset.y) element_moves++;
}

void App::clear_view_canvases() {
    for (auto** texture : {&view_canvas, &view_previous, &view_presented})
        if (*texture) {
            s.DestroyTexture(*texture);
            *texture = nullptr;
        }
    view_snapshot_ready = view_previous_ready = false;
}

void App::start_view_motion(Uint32 now, ViewTravel route) {
    // Push composites the view captured just before the input that
    // changed it; nothing captured (no input) means no transition.
    if (layered(preferences.view_style) && !view_previous_ready) {
        view_motion.stop();
        return;
    }
    view_motion.start(now, route, preferences.view_style);
}

void App::capture_view_for_input(const SDL_Event& e) {
    if (!view_transitions_enabled() || !layered(preferences.view_style) || view_canvas_failed || view_motion.running ||
        surfaces.empty())
        return;
    const bool trigger = e.type == SDL_KEYDOWN || e.type == SDL_JOYBUTTONDOWN ||
                         (e.type == SDL_JOYHATMOTION && e.jhat.value) || e.type == SDL_MOUSEBUTTONUP ||
                         e.type == SDL_FINGERUP;
    if (!trigger || view_capture_frame == rendered_frames) return;
    SDL_Renderer* renderer = bottom_renderer();
    if (!view_previous)
        view_previous = s.CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, 640, 480);
    if (!view_previous || s.SetRenderTarget(renderer, view_previous) != 0) {
        view_canvas_failed = true;
        s.ClearError();
        return;
    }
    s.RenderSetLogicalSize(renderer, 640, 480);
    s.RenderSetClipRect(renderer, nullptr);
    s.SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    s.SetRenderDrawColor(renderer, 0, 0, 0, 0);
    s.RenderClear(renderer);
    paint_bottom(renderer, 0);
    if (s.SetRenderTarget(renderer, nullptr) != 0) throw std::runtime_error("Could not restore capture canvas");
    s.RenderSetLogicalSize(renderer, 640, o.preview ? 960 : 480);
    s.RenderSetClipRect(renderer, nullptr);
    view_previous_ready = true;
    view_capture_frame = rendered_frames;
}

void App::sync_view_motion(Uint32 now) {
    auto key = std::make_tuple(int(settings.page), home_menu, preferences.view, preferences.layout);
    bool enabled = view_transitions_enabled();
    view_motion.pace = view_pace(preferences.view_speed);
    if (last_view_style != preferences.view_style) {
        clear_view_canvases();
        view_motion.stop();
        view_motion.style = preferences.view_style;
        view_canvas_failed = false;
        last_view_style = preferences.view_style;
    }
    auto level = std::make_pair(int(settings.page), home_menu);
    if (view_seen && level != std::make_pair(std::get<0>(last_view), std::get<1>(last_view))) {
        auto parent = std::find(view_levels.begin(), view_levels.end(), level);
        if (parent != view_levels.end()) {
            view_route = ViewTravel::Back;
            view_levels.erase(parent + 1, view_levels.end());
        } else if (view_route == ViewTravel::Back) view_levels = {level};
        else {
            view_route = ViewTravel::Forward;
            view_levels.push_back(level);
        }
    } else if (!view_seen) view_levels = {level};
    const bool same_page = settings.page == Page::Library && std::get<0>(last_view) == int(Page::Library) &&
                           home_menu == std::get<1>(last_view);
    // A choice on the view sheet changes the page under the sheet, and the
    // sheet's own slide away is the whole transition. A crossfade as well
    // would keep the old frame, sheet and all, over the closing sheet until
    // the fade ran out.
    if (view_seen && key != last_view && enabled && !(same_page && (picker_open || sheet_leaving()))) {
        // Any other layout or collection change stays in place: a quick
        // crossfade, never a sideways slide of the whole screen.
        if (same_page) view_route = ViewTravel::Fade;
        start_view_motion(now, view_route);
        if (view_route == ViewTravel::Back) back_transitions++;
        view_route = ViewTravel::Forward;
        view_transitions++;
    }
    last_view = key;
    view_seen = true;
    if (!enabled) {
        if (view_motion.active()) dirty = true;
        view_motion.stop();
        motion_hits.clear();
        clear_view_canvases();
        view_canvas_failed = false;
    }
}
