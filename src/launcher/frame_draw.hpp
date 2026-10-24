#pragma once
// Painting a frame: the bottom panel, view pushes and the top-level draw.
// Members of App in main.cpp; included once, right after the class.

void App::paint_bottom(SDL_Renderer* renderer, int y) {
    drawing_elements = true;
    element_origin_y = y;
    view_drawn_at = s.GetTicks();
    motion_hits.clear();
    bottom(renderer, y);
    drawing_elements = false;
}

void App::animated_bottom(SDL_Renderer* renderer, int y) {
    if (chat_open) {
        chat_bottom(renderer, y);
        return;
    }
    if (view_transitions_enabled() && layered(preferences.view_style) && !view_canvas_failed) {
        push_bottom(renderer, y);
        return;
    }
    if (!view_motion.running || view_canvas_failed) {
        paint_bottom(renderer, y);
        return;
    }
    if (!view_canvas)
        view_canvas = s.CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, 640, 480);
    if (!view_canvas || s.SetRenderTarget(renderer, view_canvas) != 0) {
        view_canvas_failed = true;
        view_motion.stop();
        s.ClearError();
        paint_bottom(renderer, y);
        return;
    }
    // SDL restores the window viewport/logical size when leaving a target.
    s.RenderSetLogicalSize(renderer, 640, 480);
    s.RenderSetClipRect(renderer, nullptr);
    paint_bottom(renderer, 0);
    s.RenderSetClipRect(renderer, nullptr);
    if (s.SetRenderTarget(renderer, nullptr) != 0) throw std::runtime_error("Could not restore menu canvas");
    s.RenderSetLogicalSize(renderer, 640, o.preview ? 960 : 480);
    rect(renderer, 0, y, 640, 480, "background");
    SDL_Rect clip{0, y, 640, 480};
    s.RenderSetClipRect(renderer, &clip);
    const auto pose = view_motion.pose(view_drawn_at);
    SDL_Rect destination{int(std::round(320 - 320 * pose.sx + pose.x)),
                         y + int(std::round(240 - 240 * pose.sy + pose.y)), int(std::round(640 * pose.sx)),
                         int(std::round(480 * pose.sy))};
    s.SetTextureBlendMode(view_canvas, SDL_BLENDMODE_BLEND);
    // One stable surface keeps text readable throughout the small spring.
    s.SetTextureColorMod(view_canvas, 255, 255, 255);
    s.SetTextureAlphaMod(view_canvas, 255);
    s.RenderCopyEx(renderer, view_canvas, nullptr, &destination, pose.tilt, nullptr, SDL_FLIP_NONE);
    s.RenderSetClipRect(renderer, nullptr);
    view_animation_frames++;
}

void App::push_bottom(SDL_Renderer* renderer, int y) {
    // Idle: straight to the panel. The capture that a transition needs
    // was taken when the input arrived (capture_view_for_input).
    if (!view_motion.running || !view_previous_ready) {
        view_previous_ready = false;
        paint_bottom(renderer, y);
        return;
    }
    if (!view_canvas)
        view_canvas = s.CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, 640, 480);
    auto fallback = [&]() {
        s.ClearError();
        clear_view_canvases();
        view_canvas_failed = true;
        view_motion.stop();
        paint_bottom(renderer, y);
    };
    if (!view_canvas || s.SetRenderTarget(renderer, view_canvas) != 0) {
        fallback();
        return;
    }
    s.RenderSetLogicalSize(renderer, 640, 480);
    s.RenderSetClipRect(renderer, nullptr);
    s.SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    s.SetRenderDrawColor(renderer, 0, 0, 0, 0);
    s.RenderClear(renderer);
    s.SetRenderDrawBlendMode(renderer, o.game_menu ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
    paint_bottom(renderer, 0);
    if (s.SetRenderTarget(renderer, nullptr) != 0) throw std::runtime_error("Could not restore push canvas");
    s.RenderSetLogicalSize(renderer, 640, o.preview ? 960 : 480);
    SDL_Rect clip{0, y, 640, 480};
    s.RenderSetClipRect(renderer, &clip);
    rect(renderer, 0, y, 640, 480, "background");
    const auto layers = view_motion.push(view_drawn_at);
    auto layer = [&](SDL_Texture* texture, double x, double shade = 0, double alpha = 1) {
        int light = int(std::round(255 * (1 - shade)));
        s.SetTextureColorMod(texture, light, light, light);
        s.SetTextureAlphaMod(texture, Uint8(std::lround(255 * alpha)));
        s.SetTextureBlendMode(texture, alpha < 1 ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
        SDL_Rect destination{int(std::round(x)), y, 640, 480};
        s.RenderCopy(renderer, texture, nullptr, &destination);
        s.SetTextureColorMod(texture, 255, 255, 255);
        s.SetTextureAlphaMod(texture, 255);
    };
    if (view_motion.style == ViewStyle::Zoom) {
        // Scaled about the panel centre; the outgoing view underneath.
        const auto zoom = view_motion.zoom(view_drawn_at);
        auto scaled = [&](SDL_Texture* texture, double scale, double alpha, double dx) {
            if (alpha <= .004) return;
            s.SetTextureAlphaMod(texture, Uint8(std::lround(255 * std::clamp(alpha, 0.0, 1.0))));
            s.SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
            const int w = int(std::lround(640 * scale)), h = int(std::lround(480 * scale));
            SDL_Rect destination{320 - w / 2 + int(std::lround(dx)), y + 240 - h / 2, w, h};
            s.RenderCopy(renderer, texture, nullptr, &destination);
            s.SetTextureAlphaMod(texture, 255);
        };
        scaled(view_previous, zoom.out_scale, zoom.out_alpha, zoom.out_dx);
        scaled(view_canvas, zoom.in_scale, zoom.in_alpha, zoom.in_dx);
    } else if (layers.incoming_above) {
        layer(view_previous, layers.outgoing, layers.shade);
        layer(view_canvas, layers.incoming, 0, layers.alpha);
    } else {
        layer(view_canvas, layers.incoming, layers.shade);
        layer(view_previous, layers.outgoing);
    }
    s.RenderSetClipRect(renderer, nullptr);
    view_animation_frames++;
    push_animation_frames++;
}

void App::draw(bool screenshot) {
    if (picker_was_open && !picker_open) picker_closed_at = s.GetTicks();
    picker_was_open = picker_open;
    sync_view_motion(s.GetTicks());
    const auto reads =
                   library_cache.counters.file_reads + media.counters.file_reads + shader_preview.counters.file_reads,
               checks = library_cache.counters.stamp_checks + media.counters.stamp_checks +
                        shader_preview.counters.stamp_checks;
    auto begin_panel = [&](SDL_Renderer* renderer) {
        // A swapped back buffer has undefined contents. Clear the complete
        // target, independently of the previous page's clip/blend state.
        if (s.RenderSetClipRect(renderer, nullptr) != 0 || s.SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE) != 0)
            throw std::runtime_error(std::string("Panel state: ") + s.GetError());
        const auto background = theme.at("background");
        s.SetRenderDrawColor(renderer, background.r, background.g, background.b, o.game_menu ? 0 : 255);
        if (s.RenderClear(renderer) != 0) throw std::runtime_error(std::string("Panel clear: ") + s.GetError());
        s.SetRenderDrawBlendMode(renderer, o.game_menu ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
    };
    auto flush_panel = [&](SDL_Renderer* renderer) {
        if (s.RenderFlush(renderer) != 0) throw std::runtime_error(std::string("Panel draw: ") + s.GetError());
    };
    auto present_panel = [&](SDL_Renderer* renderer) {
        s.ClearError();
        s.RenderPresent(renderer);
        if (const char* error = s.GetError(); error && *error)
            throw std::runtime_error(std::string("Panel presentation: ") + error);
    };
    if (o.preview) {
        begin_panel(surfaces[0].renderer);
        top(surfaces[0].renderer, 0);
        animated_bottom(surfaces[0].renderer, 480);
        touch_feedback(surfaces[0].renderer, 480);
        loading_overlay(surfaces[0].renderer, 0, false);
        loading_overlay(surfaces[0].renderer, 480, true);
        shutdown_overlay(surfaces[0].renderer, 0, true);
        shutdown_overlay(surfaces[0].renderer, 480, false);
        if (!embedded_menu) volume_feedback(surfaces[0].renderer, 0);
        flush_panel(surfaces[0].renderer);
        if (screenshot) save(surfaces[0].renderer, o.screenshot);
        present_panel(surfaces[0].renderer);
    } else {
        // SDL GLES2 readback/present relies on the current GL context when
        // batching is disabled. Finish each panel before drawing the next
        // so the upper window is never read or swapped with the lower
        // renderer's context still current.
        auto* pixels = embedded_menu ? overlay.begin() : nullptr;
        for (int i = 0; i < 2; i++) {
            auto* renderer = surfaces[i].renderer;
            if (i == 0 && chat_open && (chat.drawing || chat.view == pictochat::View::Lobby) &&
                chat_top_signature() == chat_top_seen)
                continue;
            if (i == 0 && !screenshot) {
                const auto signature = top_signature(s.GetTicks());
                if (!signature.empty() && signature == top_seen) {
                    top_skips++;
                    continue;
                }
                top_seen = signature;
                top_drawn_at = s.GetTicks();
            }
            ui_trace.mark(i == 0 ? UiTrace::TopDraw : UiTrace::BottomDraw, rendered_frames, uint32_t(settings.page));
            begin_panel(renderer);
            if (i == 0) top(renderer, 0);
            else {
                animated_bottom(renderer, 0);
                touch_feedback(renderer, 0);
            }
            loading_overlay(renderer, 0, i == 1);
            shutdown_overlay(renderer, 0, i == 0);
            if (i == 0 && !embedded_menu) volume_feedback(renderer, 0);
            ui_trace.mark(i == 0 ? UiTrace::TopFlush : UiTrace::BottomFlush, rendered_frames, uint32_t(settings.page));
            flush_panel(renderer);
            if (screenshot) save(renderer, o.screenshot.string() + (i == 0 ? "-top.bmp" : "-bottom.bmp"));
            if (embedded_menu) {
                if (s.RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_RGBA32, pixels + i * overlay_panel_bytes,
                                       640 * 4) != 0)
                    throw std::runtime_error("Menu frame capture failed");
            } else {
                ui_trace.mark(i == 0 ? UiTrace::TopPresent : UiTrace::BottomPresent, rendered_frames,
                              uint32_t(settings.page));
                present_panel(renderer);
            }
        }
        if (embedded_menu) overlay.publish();
    }
    rendered_frames++;
    render_file_reads +=
        library_cache.counters.file_reads + media.counters.file_reads + shader_preview.counters.file_reads - reads;
    render_stamp_checks += library_cache.counters.stamp_checks + media.counters.stamp_checks +
                           shader_preview.counters.stamp_checks - checks;
    ui_trace.mark(UiTrace::FrameDone, rendered_frames, uint32_t(settings.page));
    update_health(true);
    if (rendered_frames == 1) launch_profile.finish(o.game_menu);
    if (rendered_frames == 1 && o.game_menu && !embedded_menu && !o.preview) {
        if (const char* ready = std::getenv("RGDS_MENU_READY_FILE")) {
            auto pending = std::string(ready) + ".pending";
            std::ofstream file(pending);
            file << getpid() << '\n';
            file.close();
            if (file) {
                std::error_code ec;
                fs::rename(pending, ready, ec);
            }
        }
    }
    if (o.boot_ready_fd >= 0) {
        const char ready = '1';
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags = MSG_NOSIGNAL;
#elif defined(SO_NOSIGPIPE)
        const int suppress = 1;
        setsockopt(o.boot_ready_fd, SOL_SOCKET, SO_NOSIGPIPE, &suppress, sizeof(suppress));
#endif
        // The guardian may have exited. A failed notification never interrupts
        // presentation, and the nonblocking socket cannot delay the first frame.
        send(o.boot_ready_fd, &ready, 1, flags);
        ::close(o.boot_ready_fd);
        o.boot_ready_fd = -1;
    }
    if (settings.page == Page::BootWarning && !boot_displayed) {
        boot_displayed = true;
        boot_displayed_at = s.GetTicks();
    }
    dirty = false;
}
