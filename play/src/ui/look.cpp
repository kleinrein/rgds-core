// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/look.hpp"
#include "ui/words.hpp"

#include "launcher/accent.hpp"
#include "launcher/settings.hpp"

#include <cstdio>
#include <filesystem>

namespace play {
namespace {
Rgb from(const rgds::Color& c) { return {c.r, c.g, c.b}; }
rgds::Color to(Rgb c) { return {c.r, c.g, c.b}; }
} // namespace

const std::string& Look::tr(const std::string& text) const { return words ? (*words)(text) : text; }

Rgb Look::ink_on(Rgb fill) {
    const rgds::Color white{255, 255, 255}, ink{18, 24, 30};
    return from(rgds::contrast_ratio(white, to(fill)) >= rgds::contrast_ratio(ink, to(fill)) ? white : ink);
}

double contrast(Rgb a, Rgb b) { return rgds::contrast_ratio(to(a), to(b)); }

Look make_look(bool dark, int accent, bool high_contrast) {
    rgds::Theme t = dark ? rgds::default_dark_theme() : rgds::default_theme();
    rgds::apply_accent(t, accent, dark);
    if (high_contrast) rgds::apply_high_contrast(t, dark);
    Look l;
    l.background = from(t.at("background"));
    l.panel = from(t.at("panel"));
    l.ink = from(t.at("ink"));
    l.muted = from(t.at("muted"));
    l.accent = from(t.at("accent"));
    l.line = from(t.at("line"));
    l.slot = from(t.at("slot"));
    l.on_accent = from(t.at("on_accent"));
    l.focus = from(t.at("focus"));
    l.dark = dark;
    l.high_contrast = high_contrast;
    return l;
}

Look load_look(const std::string& preferences_file) {
    rgds::Preferences p;
    try {
        p = rgds::load_preferences(preferences_file);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "play: appearance: %s, the defaults instead\n", e.what());
    }
    Look l = make_look(p.dark, p.accent, p.high_contrast);
    l.grid = p.background == 1;
    l.pixel_text = p.pixel;
    l.rich_icons = p.rich_icons;
    l.animations = p.animations;
    l.language = p.language;
    try {
        const auto system = std::filesystem::path(preferences_file).parent_path() / "system.ini";
        if (std::filesystem::exists(system)) l.twelve_hour = rgds::load_system_preferences(system).time_format == 1;
    } catch (const std::exception&) {}
    return l;
}

} // namespace play
