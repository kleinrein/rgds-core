#pragma once
// Touch kinetics in motion: each frame advances the lists that coast, bounce or glide and the page an edge
// gesture is moving (kinetics.hpp). Members of App in main.cpp; included once, right after the class.

void App::page_drag_follow(int x, int y) {
    if (!preferences.animations || gesture_canvas_failed || edge_gesture == EdgeGesture::None || !pointer_down) return;
    const bool home = edge_gesture == EdgeGesture::Bottom;
    const int pull = home ? down_y - y : x - down_x;
    if (pull <= 4 && !page_drag.active()) return; // a tap on the edge leaves the page still
    page_drag.follow(home ? PageDrag::Kind::Home : PageDrag::Kind::Back, pull);
    dirty = true;
}

bool App::kinetics_update(Uint32 now) {
    bool moving = false;
    if (list_motion.active()) {
        if (settings.page == Page::Library || picker_open) list_motion.stop();
        else {
            const int row = list_scroll / list_stride;
            moving |= list_motion.step(now, list_scroll, 0, list_max_scroll());
            if (list_motion.ticking() && list_scroll / list_stride != row) haptic_detent();
        }
    }
    if (picker_motion.active()) {
        if (!picker_open || !picker_scrollable()) picker_motion.stop();
        else {
            int scroll = picker_scroll;
            const int row = picker_scroll / picker_stride();
            moving |= picker_motion.step(now, scroll, 0, picker_max_scroll());
            if (scroll != picker_scroll) scroll_picker(scroll);
            if (picker_motion.ticking() && picker_scroll / picker_stride() != row) haptic_detent();
        }
    }
    if (filter_motion.active()) {
        if (!filter_sheet_active()) filter_motion.stop();
        else moving |= filter_motion.step(now, filter_scroll, 0, filter_max_scroll());
    }
    if (browser_motion.active()) {
        const size_t count = navigation_count();
        if (home_menu || systems_menu || settings.page != Page::Library || browse_layout() != 2 || !count)
            browser_motion.stop();
        else {
            const int first = browser_list.first();
            moving |= browser_motion.step(now, browser_list.scroll, 0, browser_list.maximum(count));
            if (browser_motion.coasting && browser_list.first() != first) {
                // The selection rides along, as under the finger.
                model.selected = size_t(std::min(browser_list.first(), int(count) - 1));
                carousel.reset(model.selected);
                if (browser_motion.ticking()) haptic_detent();
            }
        }
    }
    if (page_drag.active() && !page_drag.holding()) moving |= page_drag.update(now, view_pace(preferences.view_speed));
    if (moving) kinetic_frames++;
    return moving;
}
