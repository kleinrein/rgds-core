// ds_profile.hpp: the birthday, the clock's palette and the chime (tests/test_ds_profile.py runs it).
#include "launcher/ds_profile.hpp"
#include <cassert>
#include <cstdio>

using namespace rgds;

static std::tm day(int year, int month, int mday, int hour = 12) {
    std::tm t{};
    t.tm_year = year - 1900;
    t.tm_mon = month - 1;
    t.tm_mday = mday;
    t.tm_hour = hour;
    return t;
}

int main() {
    // Parsing and writing.
    assert(DsBirthday::parse("03-14").month == 3 && DsBirthday::parse("03-14").day == 14);
    assert(DsBirthday::parse("03-14").text() == "03-14");
    for (const char* bad : {"", "13-01", "00-10", "02-30", "04-31", "3-14x", "abc"})
        assert(!DsBirthday::parse(bad).set());
    // The day itself, and a leap-day birthday on the 28th in other years.
    assert(DsBirthday::parse("03-14").today(day(2026, 3, 14)));
    assert(!DsBirthday::parse("03-14").today(day(2026, 3, 15)));
    assert(DsBirthday::parse("02-29").today(day(2028, 2, 29)));
    assert(DsBirthday::parse("02-29").today(day(2026, 2, 28)));
    assert(!DsBirthday::parse("02-29").today(day(2028, 2, 28)));
    assert(!DsBirthday{}.today(day(2026, 1, 1)));

    // The day's parts sum to one at every minute and never jump more than a little between minutes.
    DayLight last = day_light(0.0);
    for (int minute = 0; minute <= 24 * 60; minute++) {
        const DayLight d = day_light(minute / 60.0);
        const float sum = d.morning + d.day + d.evening + d.night;
        assert(sum > .999f && sum < 1.001f);
        assert(std::abs(d.morning - last.morning) < .06f && std::abs(d.night - last.night) < .06f &&
               std::abs(d.evening - last.evening) < .06f && std::abs(d.day - last.day) < .06f);
        last = d;
    }
    assert(day_light(7.0).morning == 1 && day_light(13.0).day == 1 && day_light(19.0).evening == 1);
    assert(day_light(2.0).night == 1 && day_light(23.0).night == 1);
    assert(day_light_music_tone(day_light(13.0)) == 1 && day_light_music_tone(day_light(2.0)) < .5f);

    // Text keeps AA contrast on every surface in every part of the day, with every favourite colour.
    for (bool dark : {false, true})
        for (int colour = -1; colour < 16; colour++)
            for (double hour : {7.0, 13.0, 19.0, 2.0}) {
                Theme theme = dark ? default_dark_theme() : default_theme();
                if (colour >= 0) apply_colour_wash(theme, ds_colours[size_t(colour)].rgb, dark);
                apply_day_light(theme, day_light(hour), dark);
                if (colour >= 0) apply_accent(theme, colour, dark);
                else {
                    theme["accent"] = readable_accent(theme.at("accent"), theme, dark);
                    settle_accent(theme, dark, 4.5);
                }
                for (const char* surface : {"background", "panel"}) {
                    assert(contrast_ratio(theme.at("ink"), theme.at(surface)) >= 7.0);
                    assert(contrast_ratio(theme.at("accent"), theme.at(surface)) >= 4.5);
                }
                assert(contrast_ratio(theme.at("muted"), theme.at("background")) >= 3.0);
            }

    // The chime: a few seconds, never clipping, and silent at the end.
    const auto chime = birthday_chime(16000);
    assert(chime.size() > 16000 * 3 && chime.size() < 16000 * 5);
    float peak = 0;
    for (float v : chime) peak = std::max(peak, std::abs(v));
    assert(peak > .1f && peak < 1.f && std::abs(chime.back()) < 1e-3f);
    std::puts("ds profile: ok");
}
