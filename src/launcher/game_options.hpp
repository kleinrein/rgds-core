#pragma once
#include "launcher/media.hpp"
#include "launcher/shaders.hpp"
namespace rgds {
struct GameOptions {
    int core = -1, filter = -1;
    bool favourite = false;
    int threaded = -1, hires = -1, frameskip = -1, drastic_backend = -1, drastic_screen_swap = -1, gengis_scale = 0,
        rgds_scale = 1, rgds_smooth = 1, rgds_2d = 0, rgds_adaptive = 1;
};
inline GameOptions load_game_options(const fs::path& path) {
    auto values = fields(path);
    GameOptions result;
    auto integer = [&](const std::string& key, int fallback, int low, int high) {
        if (!values.count(key)) return fallback;
        try {
            size_t end = 0;
            int value = std::stoi(values[key], &end);
            if (end == values[key].size() && value >= low && value <= high) return value;
        } catch (...) {}
        return fallback;
    };
    result.core = integer("core", -1, -1, ds_core_count - 1);
    if (core_retired(result.core)) result.core = -1;
    result.filter = integer("filter", -1, -1, shader_count - 1);
    result.favourite = integer("favourite", 0, 0, 1);
    result.threaded = integer("threaded_3d", -1, -1, 1);
    result.hires = integer("hires_3d", -1, -1, 1);
    result.frameskip = integer("frameskip", -1, -1, 3);
    result.drastic_backend = integer("drastic_backend", -1, -1, 1);
    result.drastic_screen_swap = integer("drastic_screen_swap", -1, -1, 1);
    result.gengis_scale = integer("gengis_scale", 0, 2, 3);
    result.rgds_scale = integer("rgds_scale", 1, 1, 5);
    result.rgds_smooth = integer("rgds_smooth", 1, 0, 1);
    result.rgds_2d = integer("rgds_2d", 0, 0, 5);
    result.rgds_adaptive = integer("rgds_adaptive", 1, 0, 1);
    return result;
}
inline void save_game_options(const fs::path& path, GameOptions value) {
    fs::create_directories(path.parent_path());
    auto pending = path.string() + ".pending";
    std::ofstream file(pending);
    file << "schema=1\ncore=" << value.core << "\nfilter=" << value.filter << "\nfavourite=" << value.favourite << '\n';
    file << "threaded_3d=" << value.threaded << "\nhires_3d=" << value.hires << "\nframeskip=" << value.frameskip
         << '\n';
    if (value.drastic_backend >= 0) file << "drastic_backend=" << value.drastic_backend << '\n';
    if (value.drastic_screen_swap >= 0) file << "drastic_screen_swap=" << value.drastic_screen_swap << '\n';
    if (value.gengis_scale >= 2 && value.gengis_scale <= 3) file << "gengis_scale=" << value.gengis_scale << '\n';
    if (value.rgds_scale > 1) file << "rgds_scale=" << value.rgds_scale << '\n';
    if (!value.rgds_smooth) file << "rgds_smooth=0\n";
    if (value.rgds_2d > 0) file << "rgds_2d=" << value.rgds_2d << '\n';
    if (!value.rgds_adaptive) file << "rgds_adaptive=0\n";
    file.close();
    if (!file) throw std::runtime_error("Could not save game options");
    fs::rename(pending, path);
}
}
