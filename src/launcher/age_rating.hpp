#pragma once
#include <string>

namespace rgds {
// From a game's rating to the age it is for, -1 when there is no usable rating: runtime/age_rating.py holds the same
// table, and a test keeps them equal. "Everyone" is every age. A board without a value is no rating.
inline int minimum_age(std::string rating) {
    while (!rating.empty() && rating.back() == ' ') rating.pop_back();
    const auto gap = rating.find(' ');
    if (gap == std::string::npos) return -1;
    const std::string board = rating.substr(0, gap);
    std::string value = rating.substr(gap + 1);
    while (!value.empty() && value.front() == ' ') value.erase(0, 1);
    struct Entry {
        const char* value;
        int age;
    };
    const auto find = [&](std::initializer_list<Entry> entries) {
        for (const auto& entry : entries)
            if (value == entry.value) return entry.age;
        return -1;
    };
    if (board == "ESRB") return find({{"EC", 3}, {"E", 3}, {"KA", 3}, {"E10+", 10}, {"T", 13}, {"M", 17}, {"AO", 18}});
    if (board == "CERO") return find({{"A", 3}, {"B", 12}, {"C", 15}, {"D", 17}, {"Z", 18}});
    if (board == "GRB") return find({{"ALL", 3}, {"12", 12}, {"15", 15}, {"18", 18}});
    if (board == "CSRR") return find({{"ALL", 3}, {"6", 6}, {"12", 12}, {"15", 15}, {"18", 18}});
    if (board == "PEGI") return find({{"3", 3}, {"4", 4}, {"6", 6}, {"7", 7}, {"12", 12}, {"16", 16}, {"18", 18}});
    if (board == "USK") return find({{"0", 3}, {"6", 6}, {"12", 12}, {"16", 16}, {"18", 18}});
    return -1;
}
}
