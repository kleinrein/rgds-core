#pragma once
// The gallery and its save-slot actions.
// Members of App in main.cpp; included once, right after the class.

void App::open_gallery(GalleryMode mode) {
    if (model.games.empty()) return;
    gallery_return = settings.page;
    gallery_mode = o.game_menu ? mode : GalleryMode::Manage;
    auto local = library_cache.options(model.games[model.selected]);
    gallery_core = o.game_menu               ? active_engine()
                   : other_system_selected() ? play_core
                   : local.core < 0          ? int(preferences.core)
                                             : local.core;
    settings.page = Page::SaveGallery;
    settings.focus = 0;
    slots = fields(gallery_root() / "index.ini");
    dirty = true;
    if (!o.game_menu) slot_worker("slots", 0);
}

void App::slot_worker(const std::string& action, int slot, const std::string& file, bool overwrite) {
    std::ostringstream body;
    body << "{\"action\":" << std::quoted(action)
         << ",\"rom\":" << std::quoted(model.games[model.selected].path.string())
         << ",\"core\":" << std::quoted(core_ids[gallery_core]) << ",\"slot\":" << slot
         << ",\"file\":" << std::quoted(file) << ",\"overwrite\":" << (overwrite ? "true" : "false") << '}';
    start_worker("device.library", body.str());
}

const char* App::gallery_title() const {
    return gallery_mode == GalleryMode::Save   ? "Save checkpoint"
           : gallery_mode == GalleryMode::Load ? "Load checkpoint"
                                               : "Save gallery";
}

SettingsIcon App::gallery_icon() const {
    return gallery_mode == GalleryMode::Load ? SettingsIcon::Load : SettingsIcon::Save;
}

bool App::slot_occupied(int slot) const {
    auto found = slots.find("slot_" + std::to_string(slot));
    return found != slots.end() && !found->second.empty();
}

const char* App::slot_primary_name() const {
    return gallery_mode == GalleryMode::Save ? "Save here" : gallery_mode == GalleryMode::Load ? "Load" : "Export";
}

const char* App::slot_hint(bool occupied) const {
    if (gallery_mode == GalleryMode::Save)
        return occupied ? "A replaces this slot with your current progress. X deletes it."
                        : "A saves your current progress here.";
    if (gallery_mode == GalleryMode::Load)
        return occupied ? "A loads this slot. Undo last load, under Y, brings back the progress you have now."
                        : "Nothing to load here yet.";
    return occupied ? "A exports this slot to userdata/exports. X deletes it." : "Y imports a save into this slot.";
}

std::string App::slot_question(const char* verb, int slot) const {
    return std::string(verb) + " slot " + std::to_string(slot + 1) + "?";
}

void App::slot_primary() {
    const int slot = settings.focus;
    const bool occupied = slot_occupied(slot);
    if (gallery_mode == GalleryMode::Save) {
        if (!occupied) {
            menu_request("slot-save:" + std::to_string(slot));
            return;
        }
        show_picker(slot_question("Replace", slot), {"Cancel", "Replace"}, 0, [this, slot](int yes) {
            if (yes) menu_request("slot-save:" + std::to_string(slot) + ":yes");
        });
    } else if (gallery_mode == GalleryMode::Load) {
        if (occupied)
            show_picker(slot_question("Load", slot), {"Cancel", "Load"}, 0, [this, slot](int yes) {
                if (yes) menu_request("slot-load:" + std::to_string(slot));
            });
    } else if (occupied) slot_worker("export", slot);
}

void App::slot_delete() {
    const int slot = settings.focus;
    if (!slot_occupied(slot)) return;
    show_picker(slot_question("Delete", slot), {"Cancel", "Delete"}, 0, [this, slot](int yes) {
        if (!yes) return;
        if (o.game_menu) menu_request("slot-delete:" + std::to_string(slot));
        else slot_worker("delete", slot);
    });
}

void App::slot_import(int slot) {
    std::vector<std::string> names;
    std::error_code ec;
    auto folder = o.data / "imports";
    if (fs::exists(folder))
        for (const auto& file : fs::directory_iterator(folder, ec)) {
            auto name = file.path().filename().string();
            if (file.is_regular_file() && name.size() > 12 && name.substr(name.size() - 12) == ".rgdssave.zip" &&
                names.size() < 100)
                names.push_back(name);
        }
    std::sort(names.begin(), names.end());
    if (names.empty()) {
        show_detail(
            "Import slot",
            locale.text("Copy .rgdssave.zip files into userdata/imports. Imports must match this game and emulator."));
        return;
    }
    const bool occupied = slot_occupied(slot);
    show_picker("Import slot", names, 0, [this, names, slot, occupied](int n) {
        auto name = names.at(n);
        if (occupied)
            show_picker(slot_question("Replace", slot), {"Cancel", "Replace"}, 0, [this, name, slot](int yes) {
                if (yes) slot_worker("import", slot, name, true);
            });
        else slot_worker("import", slot, name);
    });
}

void App::slot_more() {
    const int slot = settings.focus;
    std::vector<std::string> actions;
    if (o.game_menu) {
        if (slot_occupied(slot)) actions.push_back("Export slot");
        actions.push_back("Undo last slot change");
        actions.push_back("Undo last load");
    } else actions = {"Import into this slot", "Undo last slot change", "Import help"};
    show_picker(locale.text("Slot") + " " + std::to_string(slot + 1), actions, 0, [this, slot, actions](int choice) {
        const auto& action = actions.at(choice);
        if (action == "Export slot") menu_request("slot-export:" + std::to_string(slot));
        else if (action == "Undo last slot change") {
            if (o.game_menu) menu_request("slot-undo");
            else slot_worker("undo", slot);
        } else if (action == "Undo last load") menu_request("slot-undo-load");
        else if (action == "Import into this slot") slot_import(slot);
        else
            show_detail(
                "Import help",
                locale.text(
                    "Copy .rgdssave.zip files into userdata/imports. Imports must match this game and emulator."));
    });
}
