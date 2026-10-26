// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/words.hpp"

#include <fstream>

namespace play {

bool Words::load(const std::string& locales_dir, const std::string& language) {
    table_.clear();
    language_ = language;
    std::ifstream f(locales_dir + "/" + language + ".tsv");
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto tab = line.find('\t');
        if (line.empty() || line[0] == '#' || tab == std::string::npos) continue;
        table_[line.substr(0, tab)] = line.substr(tab + 1);
    }
    return true;
}

const std::string& Words::operator()(const std::string& source) const {
    auto it = table_.find(source);
    return it == table_.end() || it->second.empty() ? source : it->second;
}

} // namespace play
