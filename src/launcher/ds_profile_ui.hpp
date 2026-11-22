#pragma once
// The DS profile's birthday and the clock the menus keep (ds_profile.hpp has the rules).
// Members of App in main.cpp; included once, right after the class.

// Local time. RGDS_CLOCK="YYYY-MM-DD HH:MM" stands in for it in previews and tests.
std::tm App::clock_now() const {
    std::tm t{};
    if (const char* fake = std::getenv("RGDS_CLOCK")) {
        int year = 0, month = 0, day = 0, hour = 12, minute = 0;
        if (std::sscanf(fake, "%d-%d-%d %d:%d", &year, &month, &day, &hour, &minute) >= 3) {
            t.tm_year = year - 1900;
            t.tm_mon = month - 1;
            t.tm_mday = day;
            t.tm_hour = hour;
            t.tm_min = minute;
            t.tm_isdst = -1;
            std::mktime(&t);
            return t;
        }
    }
    const std::time_t now = std::time(nullptr);
    localtime_r(&now, &t);
    return t;
}

DayLight App::current_day_light() const { return day_light(clock_now()); }

// Once a minute: the palette follows the hour in steps too small to see, and the menu music with it.
void App::day_light_tick(Uint32 now) {
    if (day_light_checked && now - day_light_checked < 60000) return;
    day_light_checked = now ? now : 1;
    const DayLight want = preferences.time_of_day && !preferences.high_contrast ? current_day_light() : DayLight{};
    sounds.set_music_tone(day_light_music_tone(want));
    const auto moved = [](float a, float b) { return std::abs(a - b) > .02f; };
    if (moved(want.morning, day_light_applied.morning) || moved(want.day, day_light_applied.day) ||
        moved(want.evening, day_light_applied.evening) || moved(want.night, day_light_applied.night)) {
        select_theme();
        dirty = true;
    }
}

std::string App::month_name(int month) const {
    static const std::array<const char*, 12> months = {"January",   "February", "March",    "April",
                                                       "May",       "June",     "July",     "August",
                                                       "September", "October",  "November", "December"};
    return month >= 1 && month <= 12 ? locale.text(months[size_t(month - 1)]) : "";
}

// "14 March", or "March 14" where dates are written month first.
std::string App::birthday_text(DsBirthday birthday) const {
    if (!birthday.set()) return "";
    const std::string month = month_name(birthday.month), day = std::to_string(birthday.day);
    return system_preferences.date_format == 1 ? month + " " + day : day + " " + month;
}

// Whoever is playing: their profile's ds.ini (the owner's is userdata's own).
DsBirthday App::ds_birthday() const {
    if (o.player.empty()) return {};
    return DsBirthday::parse(fields(o.player / "ds.ini")["birthday"]);
}

bool App::birthday_today() const { return ds_birthday().today(clock_now()); }

void App::save_ds_birthday(const Profile& profile, DsBirthday birthday) {
    const fs::path root = profile.id == profiles.active() && !o.player.empty() ? o.player : profile.root;
    if (!ds_ini_set(root / "ds.ini", "birthday", birthday.text())) status = "Could not save DS settings.";
    profiles.load(o.data);
    dirty = true;
}
