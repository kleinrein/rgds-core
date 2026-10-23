// SPDX-License-Identifier: GPL-3.0-or-later
#include "companion/view.hpp"

namespace play {

std::unique_ptr<View> make_automap(const ViewSpec& spec, std::string& error);

std::unique_ptr<View> make_view(const ViewSpec& spec, std::string& error) {
    if (spec.format == "metroid2") return make_automap(spec, error);
    error = "[view] format '" + spec.format + "' is not known to this build";
    return nullptr;
}

} // namespace play
