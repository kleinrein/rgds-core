#pragma once
#include "launcher/model.hpp"
#include <unordered_map>

namespace rgds {
// UTF-8, tab-separated source/translation catalogs. Never execute catalog data.
// English source is the stable fallback. User/game text is not a catalog key.
class Locale {
    std::unordered_map<std::string, std::string> messages;
    std::vector<std::pair<std::string, std::string>> available, patterns;
    std::string selected = "en-US";

public:
    void load(const fs::path& directory, const std::string& language) {
        messages.clear();
        patterns.clear();
        available.clear();
        selected = "en-US";
        std::ifstream index(directory / "languages.tsv");
        std::string line;
        while (std::getline(index, line)) {
            auto tab = line.find('\t');
            if (tab == std::string::npos) continue;
            auto id = line.substr(0, tab);
            if (id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-") !=
                std::string::npos)
                continue;
            available.emplace_back(id, line.substr(tab + 1));
        }
        if (available.empty()) available = {{"en-US", "English (US)"}, {"nb-NO", "Norsk bokmål"}};
        std::sort(available.begin(), available.end(), [](const auto& a, const auto& b) {
            if (a.first == "en-US" || b.first == "en-US") return a.first == "en-US" && b.first != "en-US";
            return a.second < b.second;
        });
        for (const auto& entry : available)
            if (entry.first == language) selected = language;
        std::ifstream file(directory / (selected + ".tsv"));
        while (std::getline(file, line)) {
            if (line.empty() || line.front() == '#') continue;
            auto tab = line.find('\t');
            if (tab != std::string::npos && tab > 0 && tab + 1 < line.size()) {
                auto key = line.substr(0, tab), value = line.substr(tab + 1);
                if (key.find("{0}") != std::string::npos) patterns.emplace_back(key, value);
                else messages.emplace(key, value);
            }
        }
    }
    const auto& languages() const { return available; }
    std::string name() const {
        for (const auto& entry : available)
            if (entry.first == selected) return entry.second;
        return "English (US)";
    }
    std::string text(const std::string& source) const {
        auto found = messages.find(source);
        if (found != messages.end()) return found->second;
        for (const auto& entry : patterns) {
            auto token = entry.first.find("{0}");
            auto prefix = entry.first.substr(0, token), suffix = entry.first.substr(token + 3);
            if (source.size() < prefix.size() + suffix.size() || source.compare(0, prefix.size(), prefix) != 0 ||
                source.compare(source.size() - suffix.size(), suffix.size(), suffix) != 0)
                continue;
            auto value = entry.second;
            auto at = value.find("{0}");
            if (at != std::string::npos)
                value.replace(at, 3, source.substr(prefix.size(), source.size() - prefix.size() - suffix.size()));
            return value;
        }
        return source;
    }
    bool latin() const { return selected != "en-US"; }
};
}
