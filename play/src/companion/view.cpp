// SPDX-License-Identifier: GPL-3.0-or-later
#include "companion/view.hpp"

namespace play {

std::unique_ptr<View> make_automap(const ViewSpec& spec, std::string& error);
std::unique_ptr<View> make_board(const ViewSpec& spec, std::string& error);
std::unique_ptr<View> make_pokemon_battle(const ViewSpec& spec, std::string& error);
std::unique_ptr<View> make_mosaic(const ViewSpec& spec, std::string& error);
std::unique_ptr<View> make_grid(const ViewSpec& spec, std::string& error);
std::unique_ptr<View> make_trace(const ViewSpec& spec, std::string& error);

std::unique_ptr<View> make_view(const ViewSpec& spec, std::string& error) {
    if (spec.format == "metroid2") return make_automap(spec, error);
    if (spec.format == "board") return make_board(spec, error);
    if (spec.format == "pokemon_battle") return make_pokemon_battle(spec, error);
    if (spec.format == "mosaic") return make_mosaic(spec, error);
    if (spec.format == "grid") return make_grid(spec, error);
    if (spec.format == "trace") return make_trace(spec, error);
    error = "[view] format '" + spec.format + "' is not known to this build";
    return nullptr;
}

} // namespace play
