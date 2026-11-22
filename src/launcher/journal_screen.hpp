#pragma once
// The play journal (Page::Journal): Playtime > A. Every sitting with a game, newest first under its day, told from
// what the device already keeps (play_journal.hpp builds it in the summary worker): how long, what was unlocked,
// the pictures taken, and whether it was the first time in a long while. In December and January a quiet recap
// of the year comes first. Members of App in main.cpp; included once, after the class and playtime_screen.hpp.

void App::open_journal() {
    settings.remember(Page::Statistics, 0);
    settings.page = Page::Journal;
    settings.focus = 0;
    journal_refresh();
    emit_sound(MenuCue::Open);
    dirty = true;
}

int App::journal_count() const {
    if (!summary_ready) return 0;
    return int(summary.journal.entries.size()) + (summary.journal.recap ? 1 : 0);
}

bool App::journal_recap_row(int row) const { return summary_ready && summary.journal.recap && row == 0; }

const JournalEntryT<Game>* App::journal_entry(int row) const {
    if (!summary_ready) return nullptr;
    const int index = row - (summary.journal.recap ? 1 : 0);
    return index >= 0 && index < int(summary.journal.entries.size()) ? &summary.journal.entries[size_t(index)]
                                                                     : nullptr;
}

std::string App::journal_month(long long when, bool with_year) const {
    static const std::array<const char*, 12> months = {"January",   "February", "March",    "April",
                                                       "May",       "June",     "July",     "August",
                                                       "September", "October",  "November", "December"};
    std::time_t t = std::time_t(when);
    std::tm local{};
    if (!localtime_r(&t, &local)) return "";
    std::string text = locale.text(months[size_t(local.tm_mon)]);
    if (with_year) text += " " + std::to_string(local.tm_year + 1900);
    return text;
}

// A day as the journal names it: Today, Yesterday, or the weekday and date (the year only when it is not this one).
std::string App::journal_day(long long when) const {
    static const std::array<const char*, 7> weekdays = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                                                        "Thursday", "Friday", "Saturday"};
    const std::time_t now = std::time(nullptr), t = std::time_t(when);
    const long today = journal_local_day(now), day = journal_local_day(t);
    if (day == today) return locale.text("Today");
    if (day == today - 1) return locale.text("Yesterday");
    std::tm local{};
    if (!localtime_r(&t, &local)) return "";
    const std::string weekday = locale.text(weekdays[size_t(local.tm_wday)]), month = journal_month(when, false),
                      date = std::to_string(local.tm_mday);
    std::string text =
        system_preferences.date_format == 1 ? weekday + ", " + month + " " + date : weekday + " " + date + " " + month;
    if (journal_local_year(t) != journal_local_year(now)) text += " " + std::to_string(local.tm_year + 1900);
    return text;
}

// "First time since August": the month alone inside a year, with the year past that.
std::string App::journal_since(const JournalEntryT<Game>& entry) const {
    if (entry.first) return locale.text("First time playing");
    if (!entry.since) return "";
    const bool year = journal_local_year(std::time_t(entry.since)) != journal_local_year(std::time_t(entry.began)) ||
                      entry.began - entry.since > 300LL * 24 * 3600;
    return locale.text("First time since " + journal_month(entry.since, year));
}

std::string App::journal_detail(const JournalEntryT<Game>& entry) const {
    std::time_t began = std::time_t(entry.began);
    std::tm local{};
    std::vector<std::string> parts;
    if (localtime_r(&began, &local)) parts.push_back(time_text(local));
    if (!entry.unlocks.empty())
        parts.push_back(locale.text("Unlocked " + entry.unlocks[0]) +
                        (entry.unlocks.size() > 1 ? " +" + std::to_string(entry.unlocks.size() - 1) : ""));
    if (entry.photos)
        parts.push_back(entry.photos == 1 ? locale.text("1 photo")
                                          : locale.text(std::to_string(entry.photos) + " photos"));
    if (const auto since = journal_since(entry); !since.empty()) parts.push_back(since);
    std::string text;
    for (const auto& part : parts) text += (text.empty() ? "" : "  ·  ") + part;
    return text;
}

// The day captions above the rows, kept with the summary so list_group can hand out stable pointers.
void App::journal_refresh() {
    journal_captions.clear();
    const int count = journal_count();
    long previous = -1;
    for (int row = 0; row < count; row++) {
        if (journal_recap_row(row)) {
            journal_captions.push_back(locale.text("Your year"));
            continue;
        }
        const auto* entry = journal_entry(row);
        journal_captions.push_back(entry && entry->day != previous ? journal_day(entry->began) : std::string());
        if (entry) previous = entry->day;
    }
    if (settings.page == Page::Journal) settings.focus = count ? std::clamp(settings.focus, 0, count - 1) : 0;
    dirty = true;
}

const char* App::journal_group(int row) const {
    if (row < 0 || row >= int(journal_captions.size())) return nullptr;
    const auto& caption = journal_captions[size_t(row)];
    return caption.empty() ? nullptr : caption.c_str(); // nullptr: the row carries on the day above
}

// The first row of the day before (or after) the focused one.
int App::journal_day_step(int delta) const {
    const int count = journal_count();
    int row = std::clamp(settings.focus, 0, std::max(0, count - 1));
    if (delta > 0) {
        for (row++; row < count && !journal_group(row); row++) {}
        return std::min(row, count - 1);
    }
    while (row > 0 && !journal_group(row)) row--; // the start of this day
    if (row == settings.focus && row > 0)
        for (row--; row > 0 && !journal_group(row); row--) {}
    return std::max(0, row);
}

void App::journal_action(int code) {
    const int count = journal_count();
    if (code == 3) {
        settings.page = Page::Statistics;
        settings.focus = 0;
    } else if (!count) return;
    else if (code == 8 || code == 9) settings.focus = std::clamp(settings.focus + (code == 8 ? -1 : 1), 0, count - 1);
    else if (code == 0 || code == 1 || code == 13 || code == 14) // Left/Right and L/R: a day at a time
        settings.focus = journal_day_step(code == 0 || code == 13 ? -1 : 1);
    else if (code == 2 || code == 4) {
        // A picture of this sitting opens in the Album, at that picture.
        const auto* entry = journal_entry(settings.focus);
        if (journal_recap_row(settings.focus) && !summary.journal.recap->first_photo.empty())
            journal_open_photo(summary.journal.recap->first_photo);
        else if (entry && !entry->photo.empty()) journal_open_photo(entry->photo);
    }
    dirty = true;
}

void App::journal_open_photo(const std::string& thumb) {
    const std::string suffix = ".thumb.bmp";
    if (thumb.size() <= suffix.size()) return;
    const std::string png = thumb.substr(0, thumb.size() - suffix.size()) + ".png";
    open_album();
    for (size_t i = 0; i < album.size(); i++)
        if (album[i].path == png) {
            settings.focus = int(i);
            album_view();
            return;
        }
}

void App::journal_row(SDL_Renderer* r, int y, int i) {
    int row = y + list_top + row_top(i) - list_scroll - list_shift();
    const bool focused = i == list_selected();
    int dx = 24;
    move_element(uint32_t(i), dx, row, 566, 66, focused, list_top, list_top + list_height, 20, 595);
    dx -= 24;
    settings_card(r, 24 + dx, row, 566, 66, focused);
    if (journal_recap_row(i)) {
        const auto& recap = *summary.journal.recap;
        round_rect(r, 38 + dx, row + 11, 44, 44, well_radius, focused ? "accent" : "slot");
        ui_icon(r, SettingsIcon::Spark, 60 + dx, row + 33, 25, focused ? "on_accent" : "accent");
        label(r, "Your " + std::to_string(recap.year), 98 + dx, row + 8, 22, "ink", 440);
        const std::string note = playtime_span(uint64_t(recap.seconds)) + "  ·  " + playtime_days(recap.days) +
                                 "  ·  " + locale.text(std::to_string(recap.games) + " games");
        raw_label(r, note, 98 + dx, row + 38, 15, focused ? "ink" : "muted", 440);
        return;
    }
    const auto* entry = journal_entry(i);
    if (!entry) return;
    game_icon(r, entry->game, 38 + dx, row + 11, 44, size_t(i));
    const std::string span = playtime_span(uint64_t(entry->seconds));
    const int span_w = label_width(span, 20, false), right = 576 + dx;
    raw_label(r, span, right - span_w, row + 8, 20, "accent", span_w + 4);
    int text_right = right - span_w - 16;
    if (!entry->photo.empty()) {
        // The sitting's first picture, small, left of the time.
        const SDL_Rect slot{text_right - 56, row + 9, 56, 48};
        const SDL_Rect box = media.fitted_box(r, entry->photo, slot);
        if (media.draw(r, entry->photo, box, false, 255, false, 4)) text_right = slot.x - 12;
    }
    const int width = std::max(80, text_right - (98 + dx));
    raw_label(r, entry->title, 98 + dx, row + 8, 22, "ink", width);
    raw_label(r, journal_detail(*entry), 98 + dx, row + 38, 15, focused ? "ink" : "muted", width);
}

void App::journal_recap_top(SDL_Renderer* r, int y) {
    const auto& recap = *summary.journal.recap;
    const Color accent = theme.at("accent");
    const std::string heading = locale.text("Your " + std::to_string(recap.year));
    raw_label(r, heading, 320 - label_width(heading, 36, false) / 2, y + 86, 36, "ink", 560);
    {
        const std::string sub = locale.text("A quiet look back at your year");
        raw_label(r, sub, 320 - label_width(sub, 16, false) / 2, y + 136, 16, "muted", 560);
    }
    const std::array<std::string, 4> values = {playtime_span(uint64_t(recap.seconds)), std::to_string(recap.days),
                                               std::to_string(recap.games), std::to_string(recap.unlocks)};
    static const std::array<const char*, 4> captions = {"Time played", "Days played", "Games played", "Achievements"};
    for (int i = 0; i < 4; i++) {
        const int tx = 44 + i * 140, ty = y + 168;
        round_rect(r, tx, ty, 132, 76, 12, "slot");
        round_rect(r, tx + 1, ty + 1, 130, 74, 11, "background");
        raw_label(r, values[size_t(i)], tx + 14, ty + 10, 24, "ink", 110);
        const int size = label_width(captions[size_t(i)], 14) <= 112 ? 14 : 12;
        label(r, captions[size_t(i)], tx + 14, ty + 46, size, "muted", 112);
    }
    // Three moments: the longest sitting, the game played on the most days, the first picture.
    const bool picture = !recap.first_photo.empty();
    const int text_w = picture ? 380 : 540;
    int line = y + 262;
    auto moment = [&](const char* caption, const std::string& value, long long when) {
        if (value.empty()) return;
        fill_round(r, 46, line + 6, 8, 8, 4, accent);
        raw_label(r, locale.text(caption) + (when ? "  ·  " + journal_day(when) : ""), 64, line, 14, "muted", text_w);
        raw_label(r, value, 64, line + 18, 20, "ink", text_w);
        line += 54;
    };
    if (recap.longest)
        moment("Longest session", playtime_span(uint64_t(recap.longest->seconds)) + "  ·  " + recap.longest->title,
               recap.longest->began);
    if (recap.favourite)
        moment("The game you kept coming back to",
               recap.favourite_title + "  ·  " + playtime_days(recap.favourite_days), 0);
    if (picture) {
        moment("Your first screenshot", recap.first_photo_title, recap.first_photo_at);
        const SDL_Rect box = media.fitted_box(r, recap.first_photo, {456, y + 266, 140, 150});
        alpha_round(r, box.x - 1, box.y + 2, box.w + 2, box.h + 2, 6, shadow_colour(), preferences.dark ? 70 : 30);
        media.draw(r, recap.first_photo, box, false, 255, false, 6);
    }
}

void App::journal_top(SDL_Renderer* r, int y) {
    settings_card(r, 24, y + top_card_y, 592, top_card_h);
    if (!summary_ready) {
        centered(r, "Preparing your journal…", y + 240, 2, "muted");
        return;
    }
    if (!journal_count()) {
        ui_icon(r, SettingsIcon::Clock, 320, y + 180, 64, "muted");
        centered(r, "Your journal is empty", y + 236, 2, "ink");
        wrapped(r, "Play a game and it starts here: how long you played, what you unlocked and the pictures you took.",
                64, y + 280, 18, 512, 3);
        return;
    }
    if (journal_recap_row(settings.focus)) {
        journal_recap_top(r, y);
        return;
    }
    const auto* entry = journal_entry(settings.focus);
    if (!entry) return;
    // The picture from this sitting, or the game's cover, or its icon.
    const SDL_Rect frame{40, y + 104, 300, 288};
    const fs::path cover = folder_for_game(entry->game) / "box.bmp";
    const fs::path picture = !entry->photo.empty() ? fs::path(entry->photo) : cover;
    const SDL_Rect box = media.fitted_box(r, picture, frame);
    if (!(media.ready(r, picture) && media.draw(r, picture, box, false, 255, false, 8)))
        game_icon(r, entry->game, frame.x + frame.w / 2 - 64, frame.y + frame.h / 2 - 64, 128, 0);
    // The story, one short line each.
    const int x = 364, w = 232;
    raw_label(r, journal_day(entry->began), x, y + 108, 18, "accent", w);
    raw_wrapped(r, entry->title, x, y + 134, 26, w, 2);
    int line = y + 206;
    auto tell = [&](const std::string& text, const char* ink) {
        if (text.empty() || line > y + 420) return;
        fill_round(r, x, line + 8, 6, 6, 3, theme.at(ink));
        raw_label(r, text, x + 14, line, 18, ink == std::string("accent") ? "ink" : "muted", w - 14);
        line += 30;
    };
    {
        std::time_t began = std::time_t(entry->began), ended = std::time_t(entry->began + entry->seconds);
        std::tm from{}, to{};
        std::string when;
        if (localtime_r(&began, &from) && localtime_r(&ended, &to)) when = time_text(from) + " – " + time_text(to);
        tell(locale.text("Played for " + playtime_span(uint64_t(entry->seconds))), "accent");
        tell(when, "muted");
    }
    for (size_t i = 0; i < entry->unlocks.size() && i < 3; i++)
        tell(locale.text("Unlocked " + entry->unlocks[i]), "accent");
    if (entry->unlocks.size() > 3)
        tell(locale.text(std::to_string(entry->unlocks.size() - 3) + " more achievements"), "muted");
    if (entry->photos)
        tell(entry->photos == 1 ? locale.text("1 photo") : locale.text(std::to_string(entry->photos) + " photos"),
             "accent");
    tell(journal_since(*entry), "accent");
}

void App::journal_warm() {
    if (settings.page != Page::Journal || !summary_ready || surfaces.empty()) return;
    const int count = journal_count();
    // The upper screen's picture, and the rows near the focus on the lower one.
    if (const auto* entry = journal_entry(settings.focus)) {
        const fs::path picture =
            !entry->photo.empty() ? fs::path(entry->photo) : folder_for_game(entry->game) / "box.bmp";
        media.warm(surfaces.front().renderer, picture);
    } else if (journal_recap_row(settings.focus) && !summary.journal.recap->first_photo.empty())
        media.warm(surfaces.front().renderer, summary.journal.recap->first_photo);
    for (int row = std::max(0, settings.focus - 4); row < std::min(count, settings.focus + 6); row++)
        if (const auto* entry = journal_entry(row)) {
            warm_icon(surfaces.back().renderer, entry->game);
            if (!entry->photo.empty()) media.warm(surfaces.back().renderer, entry->photo);
        }
}

// ---- Where you left off -------------------------------------------------------------------------------------------
// A game that starts on its automatic checkpoint gets a word about it: while it starts, the upper panel shows the
// frame it was left on (last-frame.bmp, kept by runtime/emulators.py from the menu's capture), how long ago that was
// and the last thing done; once it is up, the bridge shows the same note as a card (RGDS_RESUME_NOTE, which the
// runner keeps only when the launch really resumes). Nothing for a game never played or with no checkpoint.
void App::prepare_left_off(const Game& game, const std::string& core) {
    left_off = {};
    if (game.demo || !summary_ready || !(preferences.auto_load || resume_launch)) return;
    const fs::path folder = library_cache.player_folder(game);
    std::error_code ec;
    const fs::path checkpoint = folder / ("auto-" + core + ".dss");
    if (!fs::is_regular_file(checkpoint, ec)) return;
    const auto key = folder.filename().string();
    const auto& entries = summary.journal.entries;
    const auto found = std::find_if(entries.begin(), entries.end(), [&](const auto& e) { return e.key == key; });
    if (found == entries.end()) return;
    const auto& entry = *found;
    const long days =
        journal_local_day(std::time(nullptr)) - journal_local_day(std::time_t(entry.began + entry.seconds));
    left_off.when = days <= 0   ? locale.text("Earlier today")
                    : days == 1 ? locale.text("Yesterday")
                                : locale.text(std::to_string(days) + " days ago");
    left_off.detail = !entry.unlocks.empty() ? locale.text("Unlocked " + entry.unlocks.back())
                      : entry.photos ? (entry.photos == 1 ? locale.text("1 photo")
                                                          : locale.text(std::to_string(entry.photos) + " photos"))
                                     : locale.text("Played for " + playtime_span(uint64_t(entry.seconds)));
    left_off.title = entry.title;
    // The frame only when it belongs to this checkpoint: taken as the game was left, just before the save.
    const fs::path frame = folder / "last-frame.bmp";
    const auto frame_time = fs::last_write_time(frame, ec);
    if (!ec) {
        const auto state_time = fs::last_write_time(checkpoint, ec);
        if (!ec && frame_time + std::chrono::minutes(2) >= state_time) left_off.frame = frame;
    }
    left_off.shown = true;
}

std::string App::left_off_note() const {
    if (!left_off.shown) return "";
    return locale.text("Where you left off") + "\t" + left_off.when + "  ·  " + left_off.detail;
}

void App::left_off_card(SDL_Renderer* r, int y) {
    {
        // Over the launch wash, nearly opaque: the page underneath (Home's clock) must not show through the story.
        const Color background = theme.at("background");
        s.SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        s.SetRenderDrawColor(r, background.r, background.g, background.b, 236);
        SDL_Rect panel{0, y, 640, 480};
        s.RenderFillRect(r, &panel);
    }
    const std::string header = locale.text("Where you left off");
    raw_label(r, header, 320 - label_width(header, 18, false) / 2, y + 62, 18, "accent", 560);
    const SDL_Rect frame{128, y + 96, 384, 288};
    if (!left_off.frame.empty() && media.ready(r, left_off.frame)) {
        const SDL_Rect box = media.fitted_box(r, left_off.frame, frame);
        alpha_round(r, box.x - 2, box.y + 4, box.w + 4, box.h + 4, 12, shadow_colour(), preferences.dark ? 90 : 40);
        round_rect(r, box.x - 3, box.y - 3, box.w + 6, box.h + 6, 12, "line");
        media.draw(r, left_off.frame, box, false, 255, false, 10);
    } else if (!model.games.empty())
        game_icon(r, model.games[model.selected], 320 - 64, frame.y + frame.h / 2 - 64, 128, model.selected);
    raw_label(r, left_off.title, 320 - std::min(560, label_width(left_off.title, 24, false)) / 2, y + 396, 24, "ink",
              560);
    const std::string line = left_off.when + "  ·  " + left_off.detail;
    raw_label(r, line, 320 - std::min(560, label_width(line, 18, false)) / 2, y + 432, 18, "muted", 560);
}
