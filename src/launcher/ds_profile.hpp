#pragma once
// The rest of the DS firmware's user profile, and the clock the menus keep.
//  - A birthday (ds.ini birthday=MM-DD beside nickname=), handed to DS games as the firmware's, as the favourite
//    colour is (the launcher accent). On the day, the startup sound is a little chime and PictoChat says so.
//  - The favourite colour washes the whole menu very lightly, as it coloured the original's menus.
//  - The time of day: the palette leans warm in the morning and evening and settles deeper late at night.
// Pure functions, no SDL: tests/ds_profile_test.cpp covers them.
#include "launcher/accent.hpp"
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace rgds {
struct DsBirthday {
    int month = 0, day = 0; // 1-12 and 1-31; 0 when no birthday is set
    bool set() const { return month >= 1 && month <= 12 && day >= 1 && day <= days_in(month); }
    // February has its 29th: a leap-day birthday is kept as it is and celebrated on the 28th in other years.
    static int days_in(int month) {
        static constexpr int days[12] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        return month >= 1 && month <= 12 ? days[month - 1] : 0;
    }
    std::string text() const {
        if (!set()) return "";
        char out[8];
        std::snprintf(out, sizeof out, "%02d-%02d", month, day);
        return out;
    }
    static DsBirthday parse(const std::string& text) {
        DsBirthday b;
        int m = 0, d = 0;
        char tail = 0;
        if (std::sscanf(text.c_str(), "%d-%d%c", &m, &d, &tail) == 2) {
            b.month = m;
            b.day = d;
        }
        return b.set() ? b : DsBirthday{};
    }
    bool today(const std::tm& now) const {
        if (!set()) return false;
        const int year = now.tm_year + 1900;
        const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
        const int m = month, d = month == 2 && day == 29 && !leap ? 28 : day;
        return now.tm_mon + 1 == m && now.tm_mday == d;
    }
};

// How much of each part of the day the clock is in, crossfading over an hour at each edge so nothing jumps:
// morning 5-10, day 10-17, evening 17-22, night 22-5. The weights sum to one.
struct DayLight {
    float morning = 0, day = 1, evening = 0, night = 0;
};
inline DayLight day_light(double hour) {
    hour = std::fmod(std::fmod(hour, 24.0) + 24.0, 24.0);
    // The centre of each edge, and the part that begins there.
    static constexpr double edges[4] = {5, 10, 17, 22};
    float w[4] = {0, 0, 0, 0}; // morning, day, evening, night
    // The part in force at `h` without crossfades: night from 22 to 5.
    auto part = [](double h) { return h >= 5 && h < 10 ? 0 : h >= 10 && h < 17 ? 1 : h >= 17 && h < 22 ? 2 : 3; };
    double blend = -1;
    int from = 0, to = 0;
    for (int i = 0; i < 4; i++) {
        double d = hour - edges[i];
        if (d < -12) d += 24;
        if (d > 12) d -= 24;
        if (std::abs(d) < .5) {
            blend = d + .5;
            to = i;
            from = (i + 3) % 4;
        }
    }
    if (blend < 0) w[part(hour)] = 1;
    else {
        const float t = float(blend * blend * (3 - 2 * blend)); // smoothstep
        w[from] = 1 - t;
        w[to] += t;
    }
    return {w[0], w[1], w[2], w[3]};
}
inline DayLight day_light(const std::tm& now) { return day_light(now.tm_hour + now.tm_min / 60.0); }

// The favourite colour on everything, a breath of it: the backgrounds and cards take a few percent of the hue.
inline void apply_colour_wash(Theme& theme, Color colour, bool dark) {
    const double amount = dark ? .07 : .045;
    for (const char* key : {"background", "panel", "slot", "line"})
        if (theme.count(key)) theme[key] = mix_colour(theme.at(key), colour, key[0] == 'l' ? amount * 1.5 : amount);
}

// The palette by the clock. Light mode warms a little in the morning and evening and dims a touch at night; dark
// mode gets warmer browns in the evening and a deeper blue-black at night. Text keeps its contrast: only the
// surfaces move, by small amounts, and the accent is settled again afterwards by apply_accent.
inline void apply_day_light(Theme& theme, const DayLight& light, bool dark) {
    struct Tone {
        Color toward;
        double amount;
    };
    const Tone morning = dark ? Tone{{58, 44, 30}, .16} : Tone{{255, 232, 200}, .14};
    const Tone evening = dark ? Tone{{64, 34, 30}, .18} : Tone{{255, 214, 190}, .16};
    const Tone night = dark ? Tone{{0, 4, 14}, .45} : Tone{{196, 206, 228}, .16};
    for (const char* key : {"background", "panel", "slot", "line"}) {
        if (!theme.count(key)) continue;
        Color c = theme.at(key);
        // Cards move half as far as the background, so they still lift off it.
        const double scale = key[0] == 'b' ? 1.0 : key[0] == 'p' ? .5 : .7;
        if (light.morning > 0) c = mix_colour(c, morning.toward, morning.amount * light.morning * scale);
        if (light.evening > 0) c = mix_colour(c, evening.toward, evening.amount * light.evening * scale);
        if (light.night > 0) c = mix_colour(c, night.toward, night.amount * light.night * scale);
        theme[key] = c;
    }
}

// The menu music by the clock: 1 plays it as it is, lower values roll the treble off and bring it down a little.
inline float day_light_music_tone(const DayLight& light) {
    return std::clamp(1.f - .25f * light.evening - .6f * light.night, 0.f, 1.f);
}

// The birthday chime: an original music-box phrase, bells with a soft octave partial, mono floats at `rate`.
inline std::vector<float> birthday_chime(int rate) {
    struct Note {
        double at, midi, length;
    };
    // G major, rising and settling: a few seconds and done.
    static constexpr Note notes[] = {{0.00, 79, .9},  {0.16, 83, .9},  {0.32, 86, .9},  {0.48, 91, 1.2},
                                     {0.80, 88, .8},  {0.96, 86, .8},  {1.12, 88, .8},  {1.28, 90, 1.0},
                                     {1.60, 91, 1.6}, {1.60, 86, 1.6}, {1.60, 83, 1.6}, {1.60, 79, 1.8}};
    const double total = 3.6;
    std::vector<float> out(size_t(total * rate), 0.f);
    const double pi = 3.14159265358979323846;
    for (const auto& n : notes) {
        const double f = 440.0 * std::pow(2.0, (n.midi - 69) / 12.0);
        const size_t start = size_t(n.at * rate), length = size_t(n.length * rate);
        for (size_t i = 0; i < length && start + i < out.size(); i++) {
            const double t = double(i) / rate;
            const double attack = std::min(1.0, t / .004), decay = std::exp(-t * 3.2);
            const double v = std::sin(2 * pi * f * t) + .28 * std::sin(2 * pi * f * 2 * t) * std::exp(-t * 6) +
                             .08 * std::sin(2 * pi * f * 3.01 * t) * std::exp(-t * 9);
            out[start + i] += float(v * attack * decay * .16);
        }
    }
    // The last half second fades out, whatever rings on.
    const size_t tail = size_t(.5 * rate);
    for (size_t i = 0; i < tail && i < out.size(); i++) out[out.size() - 1 - i] *= float(i) / tail;
    for (auto& v : out) v = std::tanh(v);
    return out;
}
}
