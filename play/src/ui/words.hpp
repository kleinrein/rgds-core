// SPDX-License-Identifier: GPL-3.0-or-later
// The launcher's translations (assets/locales/<language>.tsv: source, a tab,
// the translation) for the words the player's own screens show, so the second
// screen speaks the language chosen in Settings > System. Unknown words stay
// in English, as the launcher's do.
#pragma once
#include <map>
#include <string>

namespace play {

class Words {
public:
    bool load(const std::string& locales_dir, const std::string& language);
    const std::string& operator()(const std::string& source) const;
    const std::string& language() const { return language_; }

private:
    std::map<std::string, std::string> table_;
    std::string language_ = "en-US";
};

} // namespace play
