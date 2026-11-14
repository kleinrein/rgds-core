#pragma once
// Music: the player that Settings > Sound > Music player adds to Home. Like PictoChat it is a level of its own over
// Home. The lower screen is the library (five tabs and what opens from them), Now playing, the queue and the
// equaliser; the upper screen is the visualiser's display with what is playing under it. The sound itself is the
// engine's (src/music/engine.cpp), a process of its own that keeps playing when this level is left.
// Members of App in main.cpp; included once, right after the class.

namespace music_layout {
// The lists: five rows between the header's rule and the bar along the bottom.
constexpr int rows_top = 92, row_height = 56, stride = 62, rows_height = 310, rail_x = 596;
constexpr int bar_y = 418, bar_height = 54; // the mini player
constexpr int tabs = 5;                     // as wide as their longest word needs: App::music_tab_width
constexpr std::array<const char*, tabs> tab_names = {"Songs", "Albums", "Artists", "Folders", "Lists"};
// Now playing.
constexpr SDL_Rect art{32, 96, 200, 200};
constexpr int seek_left = 88, seek_right = 552, seek_y = 330;
constexpr int transport_y = 386;
constexpr std::array<int, 5> transport_x = {140, 232, 320, 408, 500}; // shuffle, previous, play, next, repeat
constexpr std::array<int, 5> transport_size = {44, 56, 68, 56, 44};
constexpr std::array<int, 3> tool_x = {468, 520, 572}; // visualiser, equaliser, favourite
// The equaliser: ten sliders, 12 dB either side of the middle line.
constexpr int band_left = 40, band_pitch = 56, band_top = 176, band_bottom = 364;
constexpr SDL_Rect preset{76, 92, 488, 44};
inline int band_x(int band) { return band_left + band * band_pitch + band_pitch / 2; }
inline int band_y(float gain) {
    return (band_top + band_bottom) / 2 - int(std::lround(gain / music::eq_range * float(band_bottom - band_top) / 2));
}
} // namespace music_layout

// The tabs are as wide as the longest of their words in this language and typeface, between the 72 px the other
// tab strips use and the 104 px that five of them have room for beside Back.
int App::music_tab_width() {
    int widest = 0;
    for (const char* name : music_layout::tab_names) widest = std::max(widest, label_width(name, 16));
    return std::clamp(widest + 16, 72, 104);
}

bool App::music_available() const { return preferences.music_player && !o.game_menu && !probe; }

// The Music player switch moved (Settings > Sound, a profile's page, another profile now playing).
void App::music_switched() {
    settings.sound_rows = sound_row_count();
    if (!preferences.music_player) {
        // Off is off: the song stops with the tile.
        if (music_ready && music.alive) music.send("quit", s.GetTicks());
        home_selected = home_fit(home_selected);
    }
    profiles.load(o.data);
    dirty = true;
}

void App::music_setup() {
    if (music_ready) return;
    music_ready = true;
    std::error_code ec;
    // RG DS OS keeps what only matters while it is up in /run/rgds (memory, gone at power off).
    const fs::path run =
        fs::is_directory("/run/rgds", ec) && !o.preview ? fs::path("/run/rgds/music") : o.data / "music/run";
    std::vector<fs::path> roots = o.music_dirs;
    if (roots.empty()) roots.push_back(fs::absolute(o.data, ec).parent_path() / "music");
    fs::path engine;
    if (const char* named = std::getenv("RGDS_MUSIC_ENGINE")) engine = named;
    else if (fs::exists(o.executable.parent_path() / "rgds-music", ec))
        engine = o.executable.parent_path() / "rgds-music";
    else engine = "/usr/bin/rgds-music";
    if (const char* sink = std::getenv("RGDS_BT_SINK")) music.bt_sink = sink;
    music.configure(run, o.data / "music", roots, engine);
    mu.levels.clear();
}

std::string App::music_time(int64_t ms) const {
    const int64_t seconds = std::max<int64_t>(0, ms) / 1000;
    char text[24];
    if (seconds >= 3600)
        std::snprintf(text, sizeof text, "%d:%02d:%02d", int(seconds / 3600), int(seconds / 60 % 60),
                      int(seconds % 60));
    else std::snprintf(text, sizeof text, "%d:%02d", int(seconds / 60), int(seconds % 60));
    return text;
}

std::string App::music_count(size_t count, const char* one, const char* many) {
    return locale.text(std::to_string(count) + (count == 1 ? one : many));
}

void App::music_toast(const std::string& text) {
    mu.toast = text;
    mu.toast_until = s.GetTicks() + 1600;
    mu.bottom_changed = dirty = true;
}

// One of the player's symbols, centred on (cx, cy): smooth in the rounded icon style, on the 12 px grid in the
// pixel one.
void App::music_glyph(SDL_Renderer* r, MusicGlyph glyph, int cx, int cy, int size, Color ink, int alpha) {
    if (!preferences.rich_icons) {
        static std::array<std::string, size_t(MusicGlyph::Count)> pixels;
        std::string& grid = pixels[size_t(glyph)];
        if (grid.empty()) grid = music_glyph_pixels(glyph);
        draw_pixel_glyph(s, r, grid, cx, cy, size, ink);
        s.SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
        return;
    }
    const auto key = std::make_tuple(r, int(glyph), size);
    auto found = music_glyph_textures.find(key);
    if (found == music_glyph_textures.end()) {
        const std::vector<uint8_t> coverage = music_glyph_coverage(glyph, size);
        std::vector<uint32_t> pixels(coverage.size());
        for (size_t i = 0; i < coverage.size(); i++) pixels[i] = uint32_t(coverage[i]) << 24 | 0xffffff;
        SDL_Texture* texture = s.CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, size, size);
        if (texture && s.UpdateTexture(texture, nullptr, pixels.data(), size * 4) != 0) {
            s.DestroyTexture(texture);
            texture = nullptr;
        }
        if (texture) s.SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        found = music_glyph_textures.emplace(key, texture).first;
    }
    if (!found->second) return;
    s.SetTextureColorMod(found->second, ink.r, ink.g, ink.b);
    s.SetTextureAlphaMod(found->second, Uint8(std::clamp(alpha, 0, 255)));
    SDL_Rect to{cx - size / 2, cy - size / 2, size, size};
    s.RenderCopy(r, found->second, nullptr, &to);
}

// A cover at one of the three sizes the engine keeps (44, 88, 200), or the symbol standing in for one.
void App::music_art(SDL_Renderer* r, const std::string& key, SDL_Rect box, int radius, MusicGlyph stand_in) {
    if (!key.empty()) {
        const fs::path file = music.folder / "art" / (key + "-" + std::to_string(box.w) + ".bmp");
        if (!media.ready(r, file)) media.request(r, file);
        if (media.draw(r, file, box, false, 255, true, radius)) return;
    }
    fill_round(r, box.x, box.y, box.w, box.h, radius, theme.at("slot"));
    int size = box.w * 5 / 9;
    if (!preferences.rich_icons) size = std::max(12, size / 12 * 12);
    music_glyph(r, stand_in, box.x + box.w / 2, box.y + box.h / 2, size, theme.at("muted"));
}

// ---- the library's pages

MusicLevel& App::music_level() {
    if (mu.levels.empty()) {
        MusicLevel root;
        root.kind = MusicLevel::Kind(std::clamp(mu.tab, 0, music_layout::tabs - 1));
        mu.levels.push_back(std::move(root));
    }
    MusicLevel& level = mu.levels.back();
    if (level.revision != music.revision) music_build(level);
    return level;
}

void App::music_build(MusicLevel& level) {
    using Kind = MusicLevel::Kind;
    const MusicLibrary& library = music.library;
    level.rows.clear();
    level.revision = music.revision;
    const std::string unknown_artist = locale.text("Unknown artist");
    const auto track_row = [&](int track, bool with_album) {
        MusicRow row;
        row.kind = MusicRow::Kind::Track;
        row.id = track;
        row.title = library.title(track);
        row.note = library.artist(track).empty() ? unknown_artist : library.artist(track);
        if (with_album) row.note += "  ·  " + library.album(track);
        row.side = music_time(library.tracks[size_t(track)].info.duration_ms);
        row.art = library.tracks[size_t(track)].art;
        row.path = library.tracks[size_t(track)].path;
        return row;
    };
    const auto shuffle_row = [&](size_t count) {
        MusicRow row;
        row.kind = MusicRow::Kind::Shuffle;
        row.title = locale.text("Shuffle all");
        row.note = music_count(count, " song", " songs");
        return row;
    };
    const auto album_row = [&](int album) {
        const MusicAlbum& a = library.albums[size_t(album)];
        MusicRow row;
        row.kind = MusicRow::Kind::Album;
        row.id = album;
        row.title = a.name;
        row.note = (a.artist.empty() ? locale.text("Various artists") : a.artist) + "  ·  " +
                   music_count(a.tracks.size(), " song", " songs");
        row.side = a.year ? std::to_string(a.year) : "";
        row.art = a.art;
        return row;
    };
    const auto find_album = [&]() {
        for (size_t i = 0; i < library.albums.size(); i++)
            if (library.albums[i].name == level.name && library.albums[i].artist == level.owner) return int(i);
        return -1;
    };
    switch (level.kind) {
    case Kind::Songs:
        if (library.songs.size() > 1) level.rows.push_back(shuffle_row(library.songs.size()));
        for (int track : library.songs) level.rows.push_back(track_row(track, true));
        break;
    case Kind::Albums:
        for (size_t i = 0; i < library.albums.size(); i++) level.rows.push_back(album_row(int(i)));
        break;
    case Kind::Artists:
        for (size_t i = 0; i < library.artists.size(); i++) {
            const MusicArtist& a = library.artists[i];
            MusicRow row;
            row.kind = MusicRow::Kind::Artist;
            row.id = int(i);
            row.title = a.name.empty() ? locale.text("Various artists") : a.name;
            row.note = music_count(a.albums.size(), " album", " albums") + "  ·  " +
                       music_count(a.tracks.size(), " song", " songs");
            row.art = a.albums.empty() ? "" : library.albums[size_t(a.albums[0])].art;
            level.rows.push_back(std::move(row));
        }
        break;
    case Kind::Lists: {
        const auto list_row = [&](int id, const char* title, size_t count) {
            MusicRow row;
            row.kind = MusicRow::Kind::List;
            row.id = id;
            row.title = locale.text(title);
            row.note = music_count(count, " song", " songs");
            level.rows.push_back(std::move(row));
        };
        list_row(0, "Favourites", music.favourite_tracks().size());
        list_row(1, "Recently added", library.recent.size());
        break;
    }
    case Kind::Favourites:
    case Kind::Recent: {
        const std::vector<int> tracks = level.kind == Kind::Recent ? library.recent : music.favourite_tracks();
        if (tracks.size() > 1) level.rows.push_back(shuffle_row(tracks.size()));
        for (int track : tracks) level.rows.push_back(track_row(track, true));
        break;
    }
    case Kind::Album: {
        const int album = find_album();
        if (album < 0) break;
        const MusicAlbum& a = library.albums[size_t(album)];
        if (a.tracks.size() > 1) level.rows.push_back(shuffle_row(a.tracks.size()));
        for (int track : a.tracks) {
            MusicRow row = track_row(track, false);
            row.number = library.tracks[size_t(track)].info.track;
            level.rows.push_back(std::move(row));
        }
        break;
    }
    case Kind::Artist:
        for (const MusicArtist& a : library.artists) {
            if (a.name != level.name) continue;
            if (a.tracks.size() > 1) level.rows.push_back(shuffle_row(a.tracks.size()));
            for (int album : a.albums) level.rows.push_back(album_row(album));
        }
        break;
    case Kind::Folders:
    case Kind::Folder: {
        // With one music folder the Folders tab is that folder; with several it lists them.
        std::string directory = level.path;
        if (level.kind == Kind::Folders) {
            if (music.roots.size() == 1) directory = music.roots[0].string();
            else {
                for (const fs::path& root : music.roots) {
                    MusicRow row;
                    row.kind = MusicRow::Kind::Folder;
                    row.title = root.filename().string();
                    row.path = root.string();
                    row.note = music_count(library.under(row.path).size(), " song", " songs");
                    level.rows.push_back(std::move(row));
                }
                break;
            }
        }
        for (const MusicFolderEntry& entry : library.folder(directory)) {
            if (entry.folder) {
                MusicRow row;
                row.kind = MusicRow::Kind::Folder;
                row.title = entry.name;
                row.path = entry.path;
                row.note = music_count(size_t(entry.count), " song", " songs");
                level.rows.push_back(std::move(row));
            } else level.rows.push_back(track_row(entry.track, false));
        }
        break;
    }
    }
    level.focus = std::clamp(level.focus, 0, std::max(0, int(level.rows.size()) - 1));
    level.scroll = std::clamp(
        level.scroll, 0, std::max(0, int(level.rows.size()) * music_layout::stride - 6 - music_layout::rows_height));
}

// The tracks a page plays, in its order: what A on one of its songs starts, and what Shuffle all shuffles.
std::vector<int> App::music_level_tracks(const MusicLevel& level) {
    std::vector<int> tracks;
    if (level.kind == MusicLevel::Kind::Artist) {
        for (const MusicArtist& a : music.library.artists)
            if (a.name == level.name) tracks = a.tracks;
        return tracks;
    }
    for (const MusicRow& row : level.rows)
        if (row.kind == MusicRow::Kind::Track) tracks.push_back(row.id);
    return tracks;
}

std::vector<int> App::music_row_tracks(const MusicRow& row) {
    switch (row.kind) {
    case MusicRow::Kind::Track: return {row.id};
    case MusicRow::Kind::Album: return music.library.albums[size_t(row.id)].tracks;
    case MusicRow::Kind::Artist: return music.library.artists[size_t(row.id)].tracks;
    case MusicRow::Kind::Folder: return music.library.under(row.path);
    case MusicRow::Kind::List: return row.id == 0 ? music.favourite_tracks() : music.library.recent;
    default: return music_level_tracks(music_level());
    }
}

void App::music_set_tab(int tab) {
    tab = std::clamp(tab, 0, music_layout::tabs - 1);
    if (mu.levels.size() == 1 && mu.tab == tab) return;
    // Tabs sit side by side: the next one slides in from the right, the one before from the left.
    const int step = (tab - mu.tab + music_layout::tabs) % music_layout::tabs;
    if (step) view_route = step <= music_layout::tabs / 2 ? ViewTravel::Right : ViewTravel::Left;
    mu.tab = tab;
    mu.levels.clear();
    mu.motion.stop();
    music_level();
    emit_sound(MenuCue::Navigate);
}

void App::music_show(MusicUi::View view) {
    if (view == mu.view) return;
    if (view == MusicUi::View::Queue || view == MusicUi::View::Equaliser)
        mu.came_from = mu.view == MusicUi::View::Browse ? MusicUi::View::Browse : MusicUi::View::Player;
    mu.view = view;
    mu.motion.stop();
    mu.seeking = false;
    mu.art_shift = 0;
    if (view == MusicUi::View::Queue) {
        // The queue opens on what is playing.
        mu.queue_focus = int(music.now.queue_index);
        mu.queue_scroll =
            std::clamp(mu.queue_focus * music_layout::stride - music_layout::stride, 0,
                       std::max(0, int(music.queue.size()) * music_layout::stride - 6 - music_layout::rows_height));
    }
    page_at = s.GetTicks();
    mu.bottom_changed = dirty = true;
}

// The list the lower screen shows now: the library page, or the queue.
int App::music_rows() {
    return mu.view == MusicUi::View::Queue ? int(music.queue.size()) : int(music_level().rows.size());
}
int& App::music_focus() { return mu.view == MusicUi::View::Queue ? mu.queue_focus : music_level().focus; }
int& App::music_scroll() { return mu.view == MusicUi::View::Queue ? mu.queue_scroll : music_level().scroll; }
int App::music_max_scroll() {
    return std::max(0, music_rows() * music_layout::stride - (music_layout::stride - music_layout::row_height) -
                           music_layout::rows_height);
}

void App::music_move_focus(int delta) {
    const int count = music_rows();
    if (!count) return;
    int& focus = music_focus();
    int& scroll = music_scroll();
    const int before_focus = focus, before = scroll;
    focus = std::clamp(focus + delta, 0, count - 1);
    mu.focus_visible = true;
    const int top = focus * music_layout::stride, bottom = top + music_layout::row_height;
    if (top < scroll) scroll = top;
    if (bottom > scroll + music_layout::rows_height) scroll = bottom - music_layout::rows_height;
    scroll = std::clamp(scroll, 0, music_max_scroll());
    if (scroll != before) mu.motion.glide(s.GetTicks(), before, scroll, preferences.animations);
    if (focus != before_focus) emit_sound(MenuCue::Navigate, delta < 0 ? -1 : 1);
}

void App::music_activate(int index) {
    const Uint32 now = s.GetTicks();
    if (mu.view == MusicUi::View::Queue) {
        if (index < 0 || index >= int(music.queue.size())) return;
        music.send("jump " + std::to_string(index), now);
        music.send("play", now);
        emit_sound(MenuCue::Confirm);
        return;
    }
    MusicLevel& level = music_level();
    if (index < 0 || index >= int(level.rows.size())) return;
    const MusicRow row = level.rows[size_t(index)];
    level.focus = index;
    using Kind = MusicLevel::Kind;
    const auto open = [&](Kind kind, const std::string& title) {
        MusicLevel next;
        next.kind = kind;
        next.title = title;
        mu.motion.stop();
        mu.levels.push_back(std::move(next));
        emit_sound(MenuCue::Open);
        page_at = now;
        return &mu.levels.back();
    };
    switch (row.kind) {
    case MusicRow::Kind::Track: {
        // The page is the queue: the album from this song on, the folder, every song.
        const std::vector<int> tracks = music_level_tracks(level);
        const auto at = std::find(tracks.begin(), tracks.end(), row.id);
        music.play(tracks, at == tracks.end() ? 0 : int(at - tracks.begin()), now);
        emit_sound(MenuCue::Confirm);
        music_show(MusicUi::View::Player);
        break;
    }
    case MusicRow::Kind::Shuffle: {
        const std::vector<int> tracks = music_level_tracks(level);
        if (tracks.empty()) break;
        if (!music.shuffle) {
            music.shuffle = true;
            music.save_settings();
        }
        std::uniform_int_distribution<int> pick(0, int(tracks.size()) - 1);
        std::mt19937 dice{std::random_device{}()};
        music.play(tracks, pick(dice), now);
        emit_sound(MenuCue::Confirm);
        music_show(MusicUi::View::Player);
        break;
    }
    case MusicRow::Kind::Album: {
        const MusicAlbum& album = music.library.albums[size_t(row.id)];
        MusicLevel* next = open(Kind::Album, album.name);
        next->name = album.name;
        next->owner = album.artist;
        break;
    }
    case MusicRow::Kind::Artist: {
        MusicLevel* next = open(Kind::Artist, row.title);
        next->name = music.library.artists[size_t(row.id)].name;
        break;
    }
    case MusicRow::Kind::Folder: open(Kind::Folder, row.title)->path = row.path; break;
    case MusicRow::Kind::List: open(row.id == 0 ? Kind::Favourites : Kind::Recent, row.title); break;
    }
    mu.bottom_changed = dirty = true;
}

// X or a long press on a row: what else can be done with it.
void App::music_row_options(int index) {
    const Uint32 now = s.GetTicks();
    if (mu.view == MusicUi::View::Queue) {
        if (index < 0 || index >= int(music.queue.size())) return;
        const bool current = music.loaded() && index == int(music.now.queue_index);
        const int track = music.queue_track(size_t(index));
        const std::string path = music.queue[size_t(index)];
        const std::string title = track >= 0 ? music.library.title(track) : MusicLibrary::stem(path);
        std::vector<std::string> options = {"Play", music.loved(path) ? "Remove from favourites" : "Add to favourites"};
        if (!current) options.push_back("Remove from queue");
        show_picker(title, options, 0, [this, index, path](int choice) {
            const Uint32 at = s.GetTicks();
            if (choice == 0) music_activate(index);
            else if (choice == 1) music.toggle_favourite(path);
            else if (music.remove(size_t(index), at))
                mu.queue_focus = std::clamp(mu.queue_focus, 0, std::max(0, int(music.queue.size()) - 1));
            mu.bottom_changed = dirty = true;
        });
        return;
    }
    MusicLevel& level = music_level();
    if (index < 0 || index >= int(level.rows.size())) return;
    const MusicRow row = level.rows[size_t(index)];
    if (row.kind == MusicRow::Kind::Shuffle) return; // it has one use, and A is it
    level.focus = index;
    const std::vector<int> tracks = music_row_tracks(row);
    if (tracks.empty()) return;
    std::vector<std::string> options = {"Play", "Play next", "Add to queue"};
    if (row.kind == MusicRow::Kind::Track)
        options.push_back(music.loved(row.path) ? "Remove from favourites" : "Add to favourites");
    else options.insert(options.begin() + 1, "Shuffle");
    show_picker(row.title, options, 0, [this, row, tracks, options](int choice) {
        const Uint32 at = s.GetTicks();
        const std::string& chosen = options[size_t(std::clamp(choice, 0, int(options.size()) - 1))];
        if (chosen == "Play" || chosen == "Shuffle") {
            if (chosen == "Shuffle" && !music.shuffle) {
                music.shuffle = true;
                music.save_settings();
            }
            music.play(tracks, 0, at);
            music_show(MusicUi::View::Player);
        } else if (chosen == "Play next") {
            // In order: the last goes in first, right behind what plays.
            for (auto track = tracks.rbegin(); track != tracks.rend(); ++track) music.add(*track, false, at);
            music_toast(locale.text("Plays next"));
        } else if (chosen == "Add to queue") {
            for (int track : tracks) music.add(track, true, at);
            music_toast(locale.text("Added to the queue"));
        } else music.toggle_favourite(row.path);
        mu.bottom_changed = dirty = true;
    });
    (void)now;
}

bool App::music_back() {
    view_route = ViewTravel::Back; // however Back was asked for (key, touch, gesture), the page travels out
    if (mu.view == MusicUi::View::Queue || mu.view == MusicUi::View::Equaliser) {
        music_show(mu.came_from);
        emit_sound(MenuCue::Back);
        return true;
    }
    if (mu.view == MusicUi::View::Player) {
        music_show(MusicUi::View::Browse);
        emit_sound(MenuCue::Back);
        return true;
    }
    if (mu.levels.size() > 1) {
        mu.levels.pop_back();
        mu.motion.stop();
        page_at = s.GetTicks();
        emit_sound(MenuCue::Back);
        mu.bottom_changed = dirty = true;
        return true;
    }
    return false;
}

void App::open_music() {
    if (music_open || launching || probe || !music_available()) return;
    cancel_navigation();
    music_setup();
    const Uint32 now = s.GetTicks();
    music.poll(now);
    // Opening the player is when new music is looked for: an engine already running reads the folder again (only
    // new and changed files are opened), a new one does so as it starts.
    if (music.alive) music.send("rescan", now);
    else music.start(now);
    music_open = true;
    // Straight to what is playing, when something is.
    mu.view = music.loaded() ? MusicUi::View::Player : MusicUi::View::Browse;
    mu.focus_visible = false;
    mu.touch = MusicUi::Touch::None;
    mu.motion.stop();
    mu.seeking = false;
    music_level();
    emit_sound(MenuCue::Open);
    mu.bottom_changed = dirty = true;
}

void App::close_music() {
    if (!music_open) return;
    music_open = false;
    mu.touch = MusicUi::Touch::None;
    mu.motion.stop();
    pointer_down = false;
    music.save_settings();
    emit_sound(MenuCue::Back);
    dirty = true;
}

// A game is starting, or the handheld is going to sleep: the music stops (it fades), and the engine lets go of the
// sound device so the game finds it free.
void App::music_pause() {
    if (music_ready && music.alive && music.playing()) music.send("release", s.GetTicks());
}

bool App::music_lid_hold(std::optional<bool> closed, Uint32 now) {
    const bool shut = closed && *closed;
    if (shut && preferences.music_lid && music_available() && music_ready && music.playing()) {
        if (!mu.lid_dark) {
            system.undim_panels();
            system.dim_panels(0);
            mu.lid_dark = true;
        }
        mu.lid_quiet_at = 0;
        last_activity = now; // not idle: the idle sleep stays away too
        return true;
    }
    if (!mu.lid_dark) return false;
    if (shut) {
        // Paused, or the queue ran out, with the lid still shut: wait half a minute for a headset's Play button,
        // then let the lid do what a closed lid does.
        if (!mu.lid_quiet_at) mu.lid_quiet_at = now ? now : 1;
        if (now - mu.lid_quiet_at < 30000) {
            last_activity = now;
            return true;
        }
    }
    system.undim_panels();
    mu.lid_dark = false;
    mu.lid_quiet_at = 0;
    last_activity = now;
    dirty = mu.bottom_changed = true;
    return false;
}

// Home's tile: what is playing, or how much there is to play.
std::string App::music_home_note() {
    if (!music_ready) return {};
    if (music.loaded()) {
        const int track = music.current_track();
        return track >= 0 ? music.library.title(track) : MusicLibrary::stem(music.now.path);
    }
    if (music.library.tracks.empty()) return locale.text("Add your music");
    return music_count(music.library.tracks.size(), " song", " songs");
}

// Every turn of the main loop. True while the player's screens are moving.
bool App::music_tick(Uint32 now) {
    if (!music_available()) {
        if (music_open) close_music();
        return false;
    }
    music_setup();
    // An engine may be playing from before the launcher restarted: look once a second even when the player is shut.
    if (!music_open && !music.alive && now - mu.polled_at < 1000) return false;
    mu.polled_at = now;
    if (music.poll(now)) {
        mu.bottom_changed = true;
        if (music_open || home_menu) dirty = true;
    }
    const int level = hardware.volume ? hardware.volume_percent : 100;
    if (music.alive && (level != mu.volume_sent || !mu.was_alive)) {
        music.send("volume " + std::to_string(level), now);
        mu.volume_sent = level;
    }
    mu.was_alive = music.alive;
    // The menu's own music and cues stand down while a song plays, and come back after.
    if (music.playing() != mu.was_playing) {
        mu.was_playing = music.playing();
        configure_sounds();
    }
    if (music.alive && music.now.error_serial != mu.error_seen) {
        mu.error_seen = music.now.error_serial;
        if (music_open && music.now.error[0]) music_toast(music.now.error);
    }
    if (!music_open) return false;
    bool moving = false;
    if (mu.motion.active()) {
        if (mu.view == MusicUi::View::Browse || mu.view == MusicUi::View::Queue) {
            const int row = music_scroll() / music_layout::stride;
            moving |= mu.motion.step(now, music_scroll(), 0, music_max_scroll());
            if (mu.motion.ticking() && music_scroll() / music_layout::stride != row) haptic_detent();
        } else mu.motion.stop();
    }
    // A finger resting on a row for half a second asks for its options.
    if (pointer_down && mu.touch == MusicUi::Touch::List && !mu.swiped && !mu.held && now - pointer_at >= 550) {
        mu.held = true;
        haptics.play(static_cast<Haptic>(MenuCue::Confirm));
        music_row_options(mu.touch_row);
        moving = true;
    }
    // The cover slides back to its place once it is let go.
    if (mu.art_shift && mu.touch != MusicUi::Touch::Art) {
        const float t = std::min(1.f, float(now - mu.art_released_at) / 180);
        mu.art_shift = preferences.animations ? int(std::lround(float(mu.art_released_from) * (1 - t) * (1 - t))) : 0;
        moving = true;
    }
    if (mu.toast_until && now >= mu.toast_until) {
        mu.toast_until = 0;
        mu.toast.clear();
        mu.bottom_changed = dirty = true;
    }
    // The queue follows the song while nobody is looking through it.
    if (mu.view == MusicUi::View::Queue && music.now.queue_index != mu.queue_place_seen) {
        if (!mu.focus_visible && !pointer_down) mu.queue_focus = int(music.now.queue_index);
        mu.queue_place_seen = music.now.queue_index;
    }
    if (moving) mu.bottom_changed = true;
    // The display: a frame every 16 ms while something sounds, and for a moment after, while the bars fall.
    const bool sounding =
        music.playing() || (music.shared && now - mu.frame_at < 2000 && mu.heard.bands[4] + mu.heard.bands[20] > .01f);
    if (music.loaded() && (sounding || music.playing())) moving = true;
    if (moving && now - feedback_frame_at >= 16) {
        feedback_frame_at = now;
        dirty = true;
    }
    return moving;
}

// ---- the upper screen

// The visualiser's display: a dark window with a bezel, the picture in big pixels. On the equaliser page it is
// always the spectrum, with the curve being shaped drawn over it.
void App::music_display(SDL_Renderer* r, int x, int y, int w, int h, bool curve) {
    const Uint32 now = s.GetTicks();
    alpha_round(r, x, y + 3, w, h, card_radius, shadow_colour(), preferences.dark ? 70 : 30);
    fill_round(r, x, y, w, h, card_radius, Color{22, 25, 32});
    fill_round(r, x + 2, y + 2, w - 4, h - 4, card_radius - 2, Color{8, 9, 13});
    const int fw = w - 8, fh = h - 8;
    if (mu.frame_width != fw || mu.frame_height != fh) {
        mu.frame_width = fw;
        mu.frame_height = fh;
        mu.frame.assign(size_t(fw) * size_t(fh), 0);
        // Rounded corners, as coverage: the picture is cut to the window's shape.
        mu.mask.assign(size_t(fw) * size_t(fh), 255);
        const float radius = float(card_radius - 4);
        for (int py = 0; py < fh; py++)
            for (int px = 0; px < fw; px++) {
                const float qx = std::max(
                                0.f, std::max(radius - (float(px) + .5f), (float(px) + .5f) - (float(fw) - radius))),
                            qy = std::max(
                                0.f, std::max(radius - (float(py) + .5f), (float(py) + .5f) - (float(fh) - radius)));
                if (qx > 0 && qy > 0)
                    mu.mask[size_t(py) * size_t(fw) + size_t(px)] =
                        uint8_t(std::clamp(radius + .5f - std::hypot(qx, qy), 0.f, 1.f) * 255);
            }
        mu.picture.resize(fw / 2, fh / 2);
        for (auto& entry : music_display_textures)
            if (entry.second) s.DestroyTexture(entry.second);
        music_display_textures.clear();
        mu.frame_at = 0;
    }
    // One new picture per frame of time, however many renderers draw it.
    if (!mu.frame_at || now - mu.frame_at >= 12) {
        const float seconds = mu.frame_at ? std::min(.1f, float(now - mu.frame_at) / 1000) : .016f;
        mu.frame_at = now;
        if (music.shared) mu.heard.take(music.shared->viz);
        mu.picture.tint(theme.at("accent"));
        const VizMode mode = curve ? VizMode::Bars : VizMode(std::clamp(music.visualiser, 0, int(VizMode::Count) - 1));
        const std::vector<uint32_t>& small = mu.picture.draw(mode, mu.heard, seconds);
        const int sw = mu.picture.width;
        for (int py = 0; py < fh; py++) {
            const uint32_t* from = &small[size_t(py / 2) * size_t(sw)];
            uint32_t* to = &mu.frame[size_t(py) * size_t(fw)];
            const uint8_t* cover = &mu.mask[size_t(py) * size_t(fw)];
            for (int px = 0; px < fw; px++) to[px] = (from[px / 2] & 0xffffff) | uint32_t(cover[px]) << 24;
        }
        if (curve) {
            // The curve over the bars, on the bars' own frequency scale: 0 dB on the middle line, 12 dB a third of
            // the window either way.
            const std::array<float, music::eq_bands> gains = music.eq_gains();
            const int floor_y = mu.picture.bar_floor() * 2, middle = floor_y / 2 + 4, reach = floor_y / 2 - 12;
            const int first = mu.picture.bar_left(0) * 2, last = mu.picture.bar_left(32) * 2;
            const auto plot = [&](int px, int py, uint32_t colour) {
                if (px >= 0 && px < fw && py >= 0 && py < fh)
                    mu.frame[size_t(py) * size_t(fw) + size_t(px)] =
                        (colour & 0xffffff) | uint32_t(mu.mask[size_t(py) * size_t(fw) + size_t(px)]) << 24;
            };
            for (int px = first; px < last; px += 6) plot(px, middle, 0x5a6472), plot(px + 1, middle, 0x5a6472);
            const uint32_t line = music.now.eq_on ? 0xffffff : 0x7c8794;
            int before = 0;
            for (int px = first; px < last; px++) {
                const float frequency = 40 * std::pow(400.f, float(px - first) / float(last - first));
                const int py =
                    middle - int(std::lround(music::eq_response(gains, frequency) / music::eq_range * float(reach)));
                // Joined up where the curve is steep.
                for (int fill = px == first ? py : std::min(before, py);
                     fill <= (px == first ? py : std::max(before, py)); fill++) {
                    plot(px, fill, line);
                    plot(px, fill + 1, line);
                }
                before = py;
            }
        }
        for (auto& entry : music_display_textures)
            if (entry.second) s.UpdateTexture(entry.second, nullptr, mu.frame.data(), fw * 4);
    }
    auto found = music_display_textures.find(r);
    if (found == music_display_textures.end()) {
        SDL_Texture* texture = s.CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, fw, fh);
        if (texture) {
            s.SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
            s.UpdateTexture(texture, nullptr, mu.frame.data(), fw * 4);
        }
        found = music_display_textures.emplace(r, texture).first;
    }
    if (found->second) {
        SDL_Rect to{x + 4, y + 4, fw, fh};
        s.RenderCopy(r, found->second, nullptr, &to);
    }
}

void App::music_progress(SDL_Renderer* r, int x, int y, int w, int64_t position, int64_t duration, bool knob) {
    sunken_track(r, x, y, w, 8, 4);
    const double part = duration > 0 ? std::clamp(double(position) / double(duration), 0.0, 1.0) : 0;
    const int lit = int(std::lround(part * (w - 4)));
    if (lit > 0) fill_round(r, x + 2, y + 2, std::max(4, lit), 4, 2, theme.at("accent"));
    if (knob) {
        const int cx = x + 2 + lit;
        alpha_round(r, cx - 11, y - 5, 22, 22, 11, shadow_colour(), preferences.dark ? 90 : 40);
        raised_pill(r, cx - 11, y - 7, 22, 22, 11, theme.at(mu.seeking ? "accent" : "panel"), mu.seeking ? 1 : 0,
                    false);
        fill_round(r, cx - 3, y + 1, 6, 6, 3, theme.at(mu.seeking ? "on_accent" : "accent"));
    }
}

std::string App::music_format_line() {
    const music::Now& now = music.now;
    std::string line = now.codec;
    char text[32];
    if (now.sample_rate) {
        if (now.sample_rate % 1000 == 0) std::snprintf(text, sizeof text, "%u kHz", now.sample_rate / 1000);
        else std::snprintf(text, sizeof text, "%.1f kHz", double(now.sample_rate) / 1000);
        line += std::string(line.empty() ? "" : "  ·  ") + text;
    }
    if (now.bits) line += "  ·  " + std::to_string(now.bits) + "-bit";
    else if (now.bitrate) line += "  ·  " + std::to_string(now.bitrate) + " kbps";
    return line;
}

void App::music_top(SDL_Renderer* r, int y) {
    rect(r, 0, y, 640, 480, "background");
    pattern(r, y + 48, 432);
    status_bar(r, y);
    const MusicLibrary& library = music.library;
    if (!music.loaded()) {
        // Nothing playing yet: what there is to play, or how to get music onto the card.
        round_rect(r, 22, y + 82, 596, 320, card_radius + 2, "line");
        round_rect(r, 24, y + 84, 592, 316, card_radius, "panel");
        music_glyph(r, MusicGlyph::Note, 320, y + 150, 96, theme.at("accent"));
        centered(r, "Music", y + 216, 2, "ink");
        if (music.alive && music.now.scanning && music.now.scan_total) {
            const std::string line = locale.text("Looking for music…") + "  " + std::to_string(music.now.scan_done) +
                                     " / " + std::to_string(music.now.scan_total);
            raw_label(r, line, 320 - label_width(line, 18, false) / 2, y + 262, 18, "muted", 520);
        } else if (library.tracks.empty()) {
            centered_wrapped(r, "Put your music in the music folder on the card: with file sharing, or as a USB drive.",
                             y + 258, 18, 500, 3, "muted");
        } else {
            const std::array<std::string, 3> values = {std::to_string(library.tracks.size()),
                                                       std::to_string(library.albums.size()),
                                                       std::to_string(library.artists.size())};
            const std::array<std::string, 3> captions = {"Songs", "Albums", "Artists"};
            for (int i = 0; i < 3; i++) {
                const int x = 52 + i * 182;
                raw_label(r, values[size_t(i)], x + (172 - label_width(values[size_t(i)], 24, false)) / 2, y + 282, 24,
                          "ink", 172);
                label(r, captions[size_t(i)], x + (172 - label_width(captions[size_t(i)], 16)) / 2, y + 316, 16,
                      "muted", 172);
            }
        }
        if (!library.tracks.empty()) key_hints(r, 320, y + 432, {{"A", "Play"}, {"X", "Options"}, {"L R", "Tabs"}}, 0);
        return;
    }
    const int track = music.current_track();
    const std::string title = track >= 0 ? library.title(track) : MusicLibrary::stem(music.now.path);
    std::string artist = track >= 0 ? library.artist(track) : std::string();
    if (artist.empty()) artist = locale.text("Unknown artist");
    const std::string album = track >= 0 ? library.album(track) : std::string();
    const std::string art = track >= 0 ? library.tracks[size_t(track)].art : std::string();
    const bool shaping = mu.view == MusicUi::View::Equaliser;
    const int64_t position = mu.seeking ? mu.seek_ms : music.now.position_ms;
    if (music.wide && !shaping) {
        // The display fills the screen; the song is a line along its lower edge.
        music_display(r, 24, y + 64, 592, 400, false);
        blend_rect(r, 28, y + 414, 584, 46, Color{0, 0, 0}, 120);
        raw_label_colour(r, title, 44, y + 420, 20, Color{255, 255, 255}, 400);
        const std::string clock = music_time(position) + " / " + music_time(music.now.duration_ms);
        raw_label_colour(r, clock, 596 - label_width(clock, 16, false), y + 424, 16, Color{214, 220, 228}, 180);
        return;
    }
    music_display(r, 24, y + 64, 592, 280, shaping);
    // What is playing, under the display.
    const int card = y + 356;
    settings_card(r, 24, card, 592, 108);
    music_art(r, art, {34, card + 10, 88, 88}, control_radius, MusicGlyph::Note);
    const std::string format = music_format_line();
    const int format_width = format.empty() ? 0 : label_width(format, 14, false) + 20;
    if (format_width) {
        round_rect(r, 606 - format_width, card + 12, format_width, 24, 12, "slot");
        raw_label(r, format, 616 - format_width, centred_label_y(format, 14, card + 24), 14, "muted", format_width);
    }
    raw_label(r, title, 136, card + 8, 24, "ink", 606 - format_width - 12 - 136);
    raw_label(r, album.empty() ? artist : artist + "  ·  " + album, 136, card + 42, 16, "accent", 466);
    const std::string elapsed = music_time(position), length = music_time(music.now.duration_ms);
    raw_label(r, elapsed, 136, card + 76, 14, "muted", 60);
    raw_label(r, length, 606 - label_width(length, 14, false), card + 76, 14, "muted", 60);
    const int bar_left = 136 + std::max(44, label_width(elapsed, 14, false) + 12),
              bar_right = 606 - std::max(44, label_width(length, 14, false) + 12);
    music_progress(r, bar_left, card + 80, bar_right - bar_left, position, music.now.duration_ms, false);
}

// The top card on Home while the Music tile is selected (carousel layout).
void App::music_home_top(SDL_Renderer* r, int y) {
    music_glyph(r, MusicGlyph::Note, 320, y + 152, 96, theme.at("accent"));
    centered(r, "Music", y + 216, 2, "ink");
    const std::string note = music_home_note();
    raw_label(r, note, 320 - std::min(500, label_width(note, 18, false)) / 2, y + 262, 18, "muted", 500);
    if (music_ready && music.loaded()) {
        const std::string chip = locale.text(music.playing() ? "Playing" : "Paused");
        const int width = label_width(chip, 18, false) + 48, left = 320 - width / 2;
        round_rect(r, left, y + 302, width, 38, 19, music.playing() ? "accent" : "slot");
        raw_label_colour(r, chip, left + 24, y + 310, 18, music.playing() ? theme.at("on_accent") : theme.at("muted"),
                         width - 30);
    }
    key_hints(r, 320, y + 432, {{"A", "Open"}}, 0);
}

// ---- the lower screen

void App::music_header(SDL_Renderer* r, int y, const std::string& title, bool translate, int width) {
    const bool pressed =
        pointer_down && mu.touch == MusicUi::Touch::Button && mu.touch_button == 0 && contact_x < 84 && contact_y < 78;
    back_button(r, 28, y + 20, pressed);
    label(r, title, 88, y + 25, 28, "ink", width, translate);
    rect(r, 24, y + 77, 592, 1, "line");
    rect(r, 24, y + 76, 44, 2, "accent");
}

// A square button in a header, like Back: a symbol on a raised panel; `lit` wears the accent.
void App::music_tool(SDL_Renderer* r, int x, int y, MusicGlyph glyph, bool pressed, bool lit) {
    round_rect(r, x, y + 3, back_size, back_size, 12, "slot");
    const int top = y + (pressed ? 2 : 0);
    const bool accent = pressed || lit;
    round_rect(r, x, top, back_size, back_size, 12, accent ? "accent" : "line");
    round_rect(r, x + 1, top + 1, back_size - 2, back_size - 2, 11, accent ? "accent" : "panel");
    music_glyph(r, glyph, x + back_size / 2, top + back_size / 2, 24, theme.at(accent ? "on_accent" : "accent"));
}

// Three bars that dance on the row that is playing.
void App::music_playing_mark(SDL_Renderer* r, int cx, int cy, Color ink) {
    const Uint32 now = s.GetTicks();
    for (int i = 0; i < 3; i++) {
        float level = .35f;
        if (music.playing()) {
            // From what is really sounding when the engine says, else a steady sway.
            // The music's own loudness in three parts of the spectrum, over a slow sway so quiet passages still move.
            const float sway = .38f + .14f * std::sin(float(now) / 170 + float(i) * 2.1f);
            float heard = 0;
            for (int band = i * 20; band < i * 20 + 20; band++) heard = std::max(heard, mu.heard.bands[size_t(band)]);
            level = std::clamp(std::max(sway, heard), .2f, 1.f);
        }
        const int tall = std::max(4, int(std::lround(level * 18)));
        fill_round(r, cx - 8 + i * 6, cy + 9 - tall, 4, tall, 2, ink);
    }
}

void App::music_row(SDL_Renderer* r, int x, int top, const MusicRow& row, bool focused, bool playing, bool pressed) {
    using namespace music_layout;
    settings_card(r, x, top, 566, row_height, focused);
    const Color accent = theme.at("accent");
    const SDL_Rect well{x + 6, top + 6, 44, 44};
    if (playing) {
        fill_round(r, well.x, well.y, 44, 44, 8, theme.at(focused ? "panel" : "slot"));
        music_playing_mark(r, well.x + 22, well.y + 22, accent);
    } else if (row.kind == MusicRow::Kind::Shuffle) {
        fill_round(r, well.x, well.y, 44, 44, 8, accent);
        music_glyph(r, MusicGlyph::Shuffle, well.x + 22, well.y + 22, 24, theme.at("on_accent"));
    } else if (row.number > 0) {
        fill_round(r, well.x, well.y, 44, 44, 8, theme.at("slot"));
        const std::string number = std::to_string(row.number);
        raw_label(r, number, well.x + 22 - label_width(number, 18, false) / 2, centred_label_y(number, 18, well.y + 22),
                  18, "muted", 44);
    } else {
        const MusicGlyph stand_in = row.kind == MusicRow::Kind::Folder   ? MusicGlyph::Folder
                                    : row.kind == MusicRow::Kind::Artist ? MusicGlyph::Person
                                    : row.kind == MusicRow::Kind::Album  ? MusicGlyph::Disc
                                    : row.kind == MusicRow::Kind::List
                                        ? (row.id == 0 ? MusicGlyph::Heart : MusicGlyph::List)
                                        : MusicGlyph::Note;
        music_art(r, row.kind == MusicRow::Kind::Folder || row.kind == MusicRow::Kind::List ? std::string() : row.art,
                  well, 8, stand_in);
    }
    const int side_width = row.side.empty() ? 0 : label_width(row.side, 14, false);
    const bool loved = row.kind == MusicRow::Kind::Track && music.loved(row.path);
    const int text_width = 566 - 62 - 14 - (side_width ? side_width + 12 : 0) - (loved ? 22 : 0);
    raw_label(r, row.title, x + 62, top + 5, 20, playing ? "accent" : "ink", text_width);
    raw_label(r, row.note, x + 62, top + 31, 14, focused ? "accent" : "muted", text_width);
    if (side_width) raw_label(r, row.side, x + 566 - 14 - side_width, top + 20, 14, "muted", side_width + 4);
    if (loved)
        music_glyph(r, MusicGlyph::Heart, x + 566 - 14 - side_width - (side_width ? 12 : 0) - 9, top + 28,
                    preferences.rich_icons ? 16 : 12, accent);
    if (pressed) alpha_round(r, x, top, 566, row_height, card_radius, accent, 28);
}

void App::music_list(SDL_Renderer* r, int y, const std::vector<MusicRow>& rows, int focus, int scroll,
                     int playing_row) {
    using namespace music_layout;
    const int shift = int(std::lround(mu.motion.stretch(scroll)));
    SDL_Rect clip{20, y + rows_top, 575, rows_height};
    s.RenderSetClipRect(r, &clip);
    const int first = std::max(0, (scroll + shift) / stride - 1),
              last = std::min(int(rows.size()), first + rows_height / stride + 3);
    for (int i = first; i < last; i++) {
        const int top = y + rows_top + i * stride - scroll - shift;
        if (top + row_height < y + rows_top || top > y + rows_top + rows_height) continue;
        const bool pressed = pointer_down && mu.touch == MusicUi::Touch::List && !mu.swiped && mu.touch_row == i;
        music_row(r, 24, top, rows[size_t(i)], mu.focus_visible && i == focus, i == playing_row, pressed);
    }
    s.RenderSetClipRect(r, nullptr);
    const int content = int(rows.size()) * stride - (stride - row_height), maximum = std::max(0, content - rows_height);
    if (maximum > 0) {
        const int thumb = std::max(54, rows_height * rows_height / content);
        draw_scrollbar(r, rail_x, y + rows_top, rows_height, thumb,
                       std::clamp(scroll, 0, maximum) * (rows_height - thumb) / maximum,
                       mu.touch == MusicUi::Touch::Rail, false);
    }
}

// The bar along the bottom of the library and the queue: what plays, play or pause, next. A tap opens Now playing.
void App::music_mini(SDL_Renderer* r, int y) {
    using namespace music_layout;
    if (!music.loaded()) {
        rect(r, 24, y + 424, 592, 1, "line");
        footer_button(r, 12, y + 432, 160, "Back", "B", false,
                      pointer_down && mu.touch == MusicUi::Touch::Button && mu.touch_button == 0 && contact_y >= 424);
        if (music.alive && music.now.scanning && music.now.scan_total) {
            const std::string line = locale.text("Looking for music…") + "  " + std::to_string(music.now.scan_done) +
                                     " / " + std::to_string(music.now.scan_total);
            raw_label(r, line, 616 - label_width(line, 16, false), y + 442, 16, "muted", 420);
        } else if (!music.library.tracks.empty())
            key_hints(r, 616, y + 452, {{"X", "Options"}, {"L R", "Tabs"}}, 1, 16, 420);
        return;
    }
    const int top = y + bar_y, track = music.current_track();
    const bool held = pointer_down && mu.touch == MusicUi::Touch::Button;
    alpha_round(r, 24, top + 3, 592, bar_height, card_radius, shadow_colour(), preferences.dark ? 60 : 20);
    glossy_pill(r, 24, top, 592, bar_height, card_radius, theme.at("panel"), held && mu.touch_button == 10 ? 1 : 0,
                false);
    const std::string art = track >= 0 ? music.library.tracks[size_t(track)].art : std::string();
    music_art(r, art, {29, top + 5, 44, 44}, 8, MusicGlyph::Note);
    const std::string title = track >= 0 ? music.library.title(track) : MusicLibrary::stem(music.now.path);
    std::string artist = track >= 0 ? music.library.artist(track) : std::string();
    if (artist.empty()) artist = locale.text("Unknown artist");
    raw_label(r, title, 84, top + 5, 18, "ink", 396);
    raw_label(r, artist, 84, top + 29, 14, "muted", 396);
    // Play or pause, and next: round glass buttons.
    const Color accent = theme.at("accent");
    raised_pill(r, 506, top + 5, 44, 44, 22, accent, held && mu.touch_button == 11 ? 1 : 0);
    music_glyph(r, music.playing() ? MusicGlyph::Pause : MusicGlyph::Play, 528 + (music.playing() ? 0 : 1), top + 27,
                24, theme.at("on_accent"));
    raised_pill(r, 560, top + 5, 44, 44, 22, theme.at("panel"), held && mu.touch_button == 12 ? 1 : 0);
    music_glyph(r, MusicGlyph::Next, 582, top + 27, 24, accent);
    // How far along, as a line inside the bar's lower edge.
    if (music.now.duration_ms > 0) {
        const int lit = int(
            std::lround(476.0 * std::clamp(double(music.now.position_ms) / double(music.now.duration_ms), 0.0, 1.0)));
        if (lit > 2) fill_round(r, 84 - 2, top + bar_height - 5, lit, 3, 1, accent);
    }
}

void App::music_browse(SDL_Renderer* r, int y) {
    using namespace music_layout;
    MusicLevel& level = music_level();
    const bool root = mu.levels.size() == 1;
    const int tab_width = music_tab_width(), tabs_left = 616 - tabs * tab_width;
    // Wide tabs leave no room for the page's name; the open tab says where this is.
    const bool named = !root || 88 + label_width("Music", 28) + 12 <= tabs_left;
    music_header(r, y, !named ? "" : root ? "Music" : level.title, root, root ? tabs_left - 96 : 520);
    if (root) {
        const int pressed = pointer_down && mu.touch == MusicUi::Touch::Button && mu.touch_button == 1 &&
                                    contact_y < 78 && contact_x >= tabs_left
                                ? (contact_x - tabs_left) / tab_width
                                : -1;
        tab_strip(r, tabs_left, y + 77, tab_width, std::vector<std::string>(tab_names.begin(), tab_names.end()), mu.tab,
                  pressed);
    }
    if (level.rows.empty()) {
        // An empty library says how to fill it; an empty list says so.
        const bool nothing = music.library.tracks.empty();
        music_glyph(r, nothing ? MusicGlyph::Note : MusicGlyph::List, 320, y + 190, preferences.rich_icons ? 64 : 60,
                    theme.at("muted"));
        if (nothing && music.alive && music.now.scanning) centered(r, "Looking for music…", y + 240, 2, "muted");
        else if (nothing) {
            centered(r, "No music yet", y + 240, 2, "ink");
            centered_wrapped(r, "Put your music in the music folder on the card: with file sharing, or as a USB drive.",
                             y + 280, 16, 480, 3, "muted");
        } else
            centered(r, level.kind == MusicLevel::Kind::Favourites ? "No favourites yet" : "Nothing here", y + 240, 2,
                     "muted");
    } else {
        int playing = -1;
        if (music.loaded())
            for (size_t i = 0; i < level.rows.size(); i++)
                if (level.rows[i].kind == MusicRow::Kind::Track && level.rows[i].path == music.now.path)
                    playing = int(i);
        music_list(r, y, level.rows, level.focus, level.scroll, playing);
    }
    music_mini(r, y);
}

void App::music_queue(SDL_Renderer* r, int y) {
    music_header(r, y, "Queue", true, 300);
    if (music.loaded() && !music.queue.empty()) {
        const std::string place =
            std::to_string(music.now.queue_index + 1) + " / " + std::to_string(music.queue.size());
        raw_label(r, place, 616 - label_width(place, 18, false), y + 34, 18, "muted", 200);
    }
    // The queue's rows are made each frame it is shown: it is short-lived and follows the engine.
    std::vector<MusicRow> rows;
    const int first = std::max(0, mu.queue_scroll / music_layout::stride - 2),
              last = std::min(int(music.queue.size()), first + 10);
    rows.resize(music.queue.size());
    const std::string unknown = locale.text("Unknown artist");
    for (int i = first; i < last; i++) {
        MusicRow& row = rows[size_t(i)];
        const int track = music.queue_track(size_t(i));
        row.path = music.queue[size_t(i)];
        if (track >= 0) {
            row.id = track;
            row.title = music.library.title(track);
            row.note = (music.library.artist(track).empty() ? unknown : music.library.artist(track)) + "  ·  " +
                       music.library.album(track);
            row.side = music_time(music.library.tracks[size_t(track)].info.duration_ms);
            row.art = music.library.tracks[size_t(track)].art;
        } else row.title = MusicLibrary::stem(row.path);
    }
    if (rows.empty()) centered(r, "The queue is empty", y + 240, 2, "muted");
    else music_list(r, y, rows, mu.queue_focus, mu.queue_scroll, music.loaded() ? int(music.now.queue_index) : -1);
    music_mini(r, y);
}

void App::music_player(SDL_Renderer* r, int y) {
    using namespace music_layout;
    music_header(r, y, "Now playing", true, 360);
    const bool held = pointer_down && mu.touch == MusicUi::Touch::Button;
    const std::string path = music.now.path;
    music_tool(r, tool_x[0], y + 20, MusicGlyph::Bars, held && mu.touch_button == 20, false);
    music_tool(r, tool_x[1], y + 20, MusicGlyph::Equaliser, held && mu.touch_button == 21, music.now.eq_on != 0);
    music_tool(r, tool_x[2], y + 20, music.loved(path) ? MusicGlyph::Heart : MusicGlyph::HeartLine,
               held && mu.touch_button == 22, false);
    const MusicLibrary& library = music.library;
    const int track = music.current_track();
    const Color accent = theme.at("accent");
    if (!music.loaded()) {
        music_glyph(r, MusicGlyph::Note, 320, y + 200, preferences.rich_icons ? 72 : 60, theme.at("muted"));
        centered(r, "Nothing is playing", y + 256, 2, "muted");
    } else {
        const std::string title = track >= 0 ? library.title(track) : MusicLibrary::stem(path);
        std::string artist = track >= 0 ? library.artist(track) : std::string();
        if (artist.empty()) artist = locale.text("Unknown artist");
        const std::string album = track >= 0 ? library.album(track) : std::string();
        // The cover, which follows a finger sideways: a swipe is the next or the previous song.
        const int ax = art.x + mu.art_shift, fade = 255 - std::min(170, std::abs(mu.art_shift) * 2);
        alpha_round(r, ax + 2, y + art.y + 6, art.w - 4, art.h, card_radius, shadow_colour(),
                    (preferences.dark ? 90 : 46) * fade / 255);
        music_art(r, track >= 0 ? library.tracks[size_t(track)].art : std::string(), {ax, y + art.y, art.w, art.h},
                  card_radius, MusicGlyph::Note);
        if (std::abs(mu.art_shift) > 24) {
            const bool forward = mu.art_shift < 0, armed = std::abs(mu.art_shift) >= 72;
            music_glyph(r, forward ? MusicGlyph::Next : MusicGlyph::Previous, forward ? art.x + art.w - 28 : art.x + 28,
                        y + art.y + art.h / 2, preferences.rich_icons ? 40 : 36, armed ? accent : theme.at("muted"));
        }
        const int tx = 252, tw = 608 - tx;
        // A title that fits one line leaves no hole under it.
        const int lines = label_width(title, 24, false) > tw ? 2 : 1, under = y + 98 + lines * 34;
        raw_wrapped(r, title, tx, y + 98, 24, tw, 2);
        raw_label(r, artist, tx, under + 2, 18, "accent", tw);
        raw_label(r, album, tx, under + 30, 16, "muted", tw);
        // Two chips: what the file is, and where the sound is going.
        const std::string format = music_format_line();
        if (!format.empty()) {
            const int width = std::min(tw, label_width(format, 14, false) + 24);
            round_rect(r, tx, under + 66, width, 26, 13, "slot");
            raw_label(r, format, tx + 12, centred_label_y(format, 14, under + 79), 14, "muted", width - 20);
        }
        const music::Route route = music.now.route;
        const std::string where = locale.text(route == music::Route::Bluetooth    ? "Bluetooth"
                                              : route == music::Route::Headphones ? "Headphones"
                                                                                  : "Speakers");
        const int width = label_width(where, 14, false) + 46;
        round_rect(r, tx, under + 100, width, 26, 13, "slot");
        music_glyph(r,
                    route == music::Route::Bluetooth    ? MusicGlyph::Bluetooth
                    : route == music::Route::Headphones ? MusicGlyph::Headphones
                                                        : MusicGlyph::Speaker,
                    tx + 18, under + 113, preferences.rich_icons ? 16 : 12, accent);
        raw_label(r, where, tx + 32, centred_label_y(where, 14, under + 113), 14, "muted", width - 36);
        // The seek bar.
        const int64_t position = mu.seeking ? mu.seek_ms : music.now.position_ms;
        const std::string elapsed = music_time(position), length = music_time(music.now.duration_ms);
        raw_label(r, elapsed, 32, centred_label_y(elapsed, 14, y + seek_y + 4), 14, mu.seeking ? "accent" : "muted",
                  56);
        raw_label(r, length, 608 - label_width(length, 14, false), centred_label_y(length, 14, y + seek_y + 4), 14,
                  "muted", 60);
        music_progress(r, seek_left, y + seek_y, seek_right - seek_left, position, music.now.duration_ms, true);
    }
    // Shuffle, previous, play or pause, next, repeat.
    const std::array<MusicGlyph, 5> glyphs = {
        MusicGlyph::Shuffle, MusicGlyph::Previous, music.playing() ? MusicGlyph::Pause : MusicGlyph::Play,
        MusicGlyph::Next, music.now.repeat == music::Repeat::One ? MusicGlyph::RepeatOne : MusicGlyph::Repeat};
    for (int i = 0; i < 5; i++) {
        const int size = transport_size[size_t(i)], x = transport_x[size_t(i)] - size / 2,
                  top = y + transport_y - size / 2;
        const bool pressed = held && mu.touch_button == 30 + i;
        const bool lit = i == 2 || (i == 0 && music.shuffle) || (i == 4 && music.now.repeat != music::Repeat::Off);
        // The side buttons light up as glass when they are on; play is always the accent.
        raised_pill(r, x, top, size, size, size / 2, i == 2 ? accent : theme.at(lit ? "focus" : "panel"),
                    pressed ? 1 : 0);
        int glyph = i == 2 ? 32 : i == 1 || i == 3 ? 28 : 22;
        if (!preferences.rich_icons) glyph = i == 2 ? 36 : 24;
        music_glyph(r, glyphs[size_t(i)], transport_x[size_t(i)] + (i == 2 && !music.playing() ? 2 : 0),
                    y + transport_y + (pressed ? 2 : 0), glyph,
                    i == 2                    ? theme.at("on_accent")
                    : lit || i == 1 || i == 3 ? accent
                                              : theme.at("muted"));
    }
    rect(r, 24, y + 424, 592, 1, "line");
    footer_button(r, 12, y + 432, 160, "Library", "B", false, held && mu.touch_button == 40);
    key_hints(r, 320, y + 452, {{"L R", "Skip"}, {"Y", "Visualiser"}}, 0, 16, 280);
    footer_button(r, 468, y + 432, 160, "Queue", "", false, held && mu.touch_button == 41);
}

void App::music_equaliser(SDL_Renderer* r, int y) {
    using namespace music_layout;
    music_header(r, y, "Equaliser", true, 300);
    const bool on = music.now.eq_on != 0, held = pointer_down && mu.touch == MusicUi::Touch::Button;
    const Color accent = theme.at("accent");
    draw_toggle(r, 568, y + 28, "Equaliser", on, true);
    // The curve is kept for each output: say which one this is.
    const music::Route route = music.now.route;
    const std::string where = locale.text(route == music::Route::Bluetooth    ? "Bluetooth"
                                          : route == music::Route::Headphones ? "Headphones"
                                                                              : "Speakers");
    {
        const int width = label_width(where, 14, false) + 46, left = 88 + label_width("Equaliser", 28) + 16;
        if (left + width < 500) {
            round_rect(r, left, y + 31, width, 26, 13, "slot");
            music_glyph(r,
                        route == music::Route::Bluetooth    ? MusicGlyph::Bluetooth
                        : route == music::Route::Headphones ? MusicGlyph::Headphones
                                                            : MusicGlyph::Speaker,
                        left + 18, y + 44, preferences.rich_icons ? 16 : 12, accent);
            raw_label(r, where, left + 32, centred_label_y(where, 14, y + 44), 14, "muted", width - 36);
        }
    }
    // The preset: L and R step through them, as do taps on either half.
    const int preset_index = music.now.eq_preset;
    const std::string name = locale.text(preset_index >= 0 && preset_index < int(music::eq_presets.size())
                                             ? music::eq_presets[size_t(preset_index)].name
                                             : "Custom");
    raised_pill(r, preset.x, y + preset.y, preset.w, preset.h, 22, theme.at("panel"),
                held && (mu.touch_button == 50 || mu.touch_button == 51) ? 1 : 0);
    key_badge(r, preset.x + 14, y + preset.y + preset.h / 2, "L");
    key_badge(r, preset.x + preset.w - 14 - key_badge_width("R"), y + preset.y + preset.h / 2, "R");
    raw_label(r, name, 320 - label_width(name, 20, false) / 2, centred_label_y(name, 20, y + preset.y + preset.h / 2),
              20, on ? "ink" : "muted", 360);
    // The sliders.
    const int middle = (band_top + band_bottom) / 2;
    for (int level : {band_top, middle, band_bottom})
        for (int x = band_left; x < band_left + band_pitch * music::eq_bands; x += 8)
            tint_rect(r, x, y + level, 4, 1, theme.at("line"));
    raw_label(r, "+12", 8, y + band_top - 8, 14, "muted", 30);
    raw_label(r, "0", 20, y + middle - 8, 14, "muted", 30);
    raw_label(r, "-12", 10, y + band_bottom - 8, 14, "muted", 30);
    static const std::array<const char*, music::eq_bands> names = {"31", "62", "125", "250", "500",
                                                                   "1k", "2k", "4k",  "8k",  "16k"};
    for (int band = 0; band < music::eq_bands; band++) {
        const float gain = music.now.eq_gain[band];
        const int cx = band_x(band), ky = y + band_y(gain);
        const bool focused =
            (mu.focus_visible && mu.band == band) || (mu.touch == MusicUi::Touch::Band && mu.touch_row == band);
        sunken_track(r, cx - 4, y + band_top - 4, 8, band_bottom - band_top + 8, 4);
        // Lit from the middle line to the knob.
        if (ky != y + middle)
            fill_round(r, cx - 2, std::min(ky, y + middle), 4, std::abs(ky - (y + middle)), 2,
                       on ? accent : theme.at("muted"));
        alpha_round(r, cx - 18, ky - 8, 36, 20, 10, shadow_colour(), preferences.dark ? 90 : 40);
        raised_pill(r, cx - 18, ky - 10, 36, 20, 10, theme.at(focused ? "accent" : "panel"), 0, false);
        fill_round(r, cx - 8, ky - 1, 16, 2, 1, theme.at(focused ? "on_accent" : on ? "accent" : "muted"));
        const std::string label_text = names[size_t(band)];
        raw_label(r, label_text, cx - label_width(label_text, 14, false) / 2, y + band_bottom + 18, 14,
                  focused ? "accent" : "muted", 50);
        if (focused) {
            char value[16];
            std::snprintf(value, sizeof value, "%+d dB", int(std::lround(gain)));
            const std::string text = int(std::lround(gain)) == 0 ? "0 dB" : value;
            const int width = label_width(text, 14, false) + 16, left = std::clamp(cx - width / 2, 40, 600 - width);
            fill_round(r, left, y + band_top - 34, width, 22, 11, accent);
            raw_label_colour(r, text, left + 8, centred_label_y(text, 14, y + band_top - 23), 14, theme.at("on_accent"),
                             width);
        }
    }
    rect(r, 24, y + 424, 592, 1, "line");
    footer_button(r, 12, y + 432, 160, "Back", "B", false, held && mu.touch_button == 0 && contact_y >= 424);
    key_hints(r, 320, y + 452, {{"A", on ? "Switch off" : "Switch on"}}, 0, 16, 280);
    footer_button(r, 468, y + 432, 160, "Flat", "X", false, held && mu.touch_button == 52);
}

void App::music_bottom(SDL_Renderer* r, int y) {
    rect(r, 0, y, 640, 480, "background");
    pattern(r, y, 480, true);
    switch (mu.view) {
    case MusicUi::View::Player: music_player(r, y); break;
    case MusicUi::View::Queue: music_queue(r, y); break;
    case MusicUi::View::Equaliser: music_equaliser(r, y); break;
    default: music_browse(r, y); break;
    }
    if (!mu.toast.empty()) {
        const int width = std::min(560, label_width(mu.toast, 16, false) + 36), left = 320 - width / 2;
        alpha_round(r, left, y + 372, width, 36, 18, Color{20, 24, 30}, 230);
        raw_label_colour(r, mu.toast, left + 18, centred_label_y(mu.toast, 16, y + 390), 16, Color{255, 255, 255},
                         width - 28);
    }
    if (picker_open || picker_closing()) render_picker(r, y);
}

// ---- buttons and keys

void App::music_set_gain(int band, float gain) {
    std::array<float, music::eq_bands> gains = music.eq_gains();
    gain = std::clamp(std::round(gain), -music::eq_range, music::eq_range);
    if (gains[size_t(band)] == gain && music.now.eq_on) return;
    gains[size_t(band)] = gain;
    // Moving a slider is the listener's own curve, and switches the equaliser on: a slider that does nothing reads
    // as broken.
    music.set_eq(true, -1, gains, s.GetTicks());
    haptic_detent();
}

void App::music_step_preset(int delta) {
    const int count = int(music::eq_presets.size());
    int preset = music.now.eq_preset;
    preset = preset < 0 ? (delta > 0 ? 0 : count - 1) : (preset + delta + count) % count;
    music.set_eq(true, preset, music::eq_presets[size_t(preset)].gain, s.GetTicks());
    emit_sound(MenuCue::Adjust);
}

void App::music_next_visualiser() {
    music.visualiser = (music.visualiser + 1) % int(VizMode::Count);
    music.save_settings();
    music_toast(locale.text(viz_names[size_t(music.visualiser)]));
}

void App::music_choose_visualiser() {
    std::vector<std::string> options(viz_names.begin(), viz_names.end());
    options.push_back(music.wide ? "Smaller display" : "Larger display");
    show_picker("Visualiser", options, music.visualiser, [this](int choice) {
        if (choice >= int(VizMode::Count)) music.wide = !music.wide;
        else music.visualiser = std::clamp(choice, 0, int(VizMode::Count) - 1);
        music.save_settings();
        mu.bottom_changed = dirty = true;
    });
}

void App::music_toggle_favourite() {
    if (!music.loaded()) return;
    const std::string path = music.now.path;
    music.toggle_favourite(path);
    emit_sound(music.loved(path) ? MenuCue::Favourite : MenuCue::Unfavourite);
    music_toast(locale.text(music.loved(path) ? "Added to favourites" : "Removed from favourites"));
}

void App::music_action(int code) {
    const Uint32 now = s.GetTicks();
    last_activity = now;
    mu.bottom_changed = dirty = true;
    using View = MusicUi::View;
    // Everywhere: Start plays and pauses, L2 shuffles, R2 repeats, the Anbernic button leaves.
    if (code == 4) {
        if (music.loaded()) music.send("toggle", now);
        return;
    }
    if (code == 7) return close_music();
    if (code == 15) {
        music.set_shuffle(!music.shuffle, now);
        music_toast(locale.text(music.shuffle ? "Shuffle on" : "Shuffle off"));
        return;
    }
    if (code == 16) {
        const int next = (int(music.now.repeat) + 1) % 3;
        music.cycle_repeat(now);
        music_toast(locale.text(next == 0 ? "Repeat off" : next == 1 ? "Repeat all" : "Repeat this song"));
        return;
    }
    if (code == 3) {
        if (!music_back()) close_music();
        return;
    }
    switch (mu.view) {
    case View::Browse:
    case View::Queue:
        if (code == 8 || code == 9) music_move_focus(code == 8 ? -1 : 1);
        else if (code == 0 || code == 1) music_move_focus(code == 0 ? -5 : 5); // a page at a time
        else if (code == 2) {
            mu.focus_visible = true;
            music_activate(music_focus());
        } else if (code == 11) {
            mu.focus_visible = true;
            music_row_options(music_focus());
        } else if (code == 12) {
            if (music.loaded()) music_show(View::Player);
        } else if (code == 13 || code == 14) {
            if (mu.view == View::Queue) music.send(code == 13 ? "prev" : "next", now);
            else if (mu.levels.size() == 1)
                music_set_tab((mu.tab + (code == 13 ? music_layout::tabs - 1 : 1)) % music_layout::tabs);
        }
        break;
    case View::Player:
        if (code == 2) {
            if (music.loaded()) music.send("toggle", now);
        } else if (code == 0 || code == 1) music.send(code == 0 ? "seekby -10000" : "seekby 10000", now);
        else if (code == 13 || code == 14) music.send(code == 13 ? "prev" : "next", now);
        else if (code == 8) {
            music.wide = !music.wide;
            music.save_settings();
        } else if (code == 9) music_show(View::Queue);
        else if (code == 11) music_show(View::Equaliser);
        else if (code == 12) music_next_visualiser();
        break;
    case View::Equaliser:
        mu.focus_visible = true;
        if (code == 0 || code == 1) {
            mu.band = std::clamp(mu.band + (code == 0 ? -1 : 1), 0, music::eq_bands - 1);
            emit_sound(MenuCue::Navigate, code == 0 ? -1 : 1);
        } else if (code == 8 || code == 9) music_set_gain(mu.band, music.now.eq_gain[mu.band] + (code == 8 ? 1 : -1));
        else if (code == 13 || code == 14) music_step_preset(code == 13 ? -1 : 1);
        else if (code == 2) {
            music.set_eq(!music.now.eq_on, music.now.eq_preset, music.eq_gains(), now);
            emit_sound(music.now.eq_on ? MenuCue::Enable : MenuCue::Disable);
        } else if (code == 11) {
            music.set_eq(music.now.eq_on, 0, music::eq_presets[0].gain, now);
            emit_sound(MenuCue::Adjust);
        }
        break;
    }
}

// ---- touch

// The button under a point on the page shown, or -1. Numbers are the ones the drawing checks for its pressed look.
int App::music_button_at(int x, int y) {
    using namespace music_layout;
    using View = MusicUi::View;
    if (x < 84 && y < 78) return 0; // Back, in every header
    const auto round_hit = [&](int cx, int cy, int size) {
        return std::abs(x - cx) <= std::max(24, size / 2 + 4) && std::abs(y - cy) <= std::max(24, size / 2 + 4);
    };
    if (mu.view == View::Browse || mu.view == View::Queue) {
        if (mu.view == View::Browse && mu.levels.size() == 1 && y < 78 && x >= 616 - tabs * music_tab_width() &&
            x < 616)
            return 1;
        if (!music.loaded()) return y >= 424 && x < 200 ? 0 : -1;
        if (y >= bar_y - 4)
            return round_hit(528, bar_y + 27, 44)   ? 11
                   : round_hit(582, bar_y + 27, 44) ? 12
                   : x >= 24 && x < 616             ? 10
                                                    : -1;
        return -1;
    }
    if (mu.view == View::Player) {
        for (int i = 0; i < 3; i++)
            if (y >= 14 && y < 72 && x >= tool_x[size_t(i)] - 4 && x < tool_x[size_t(i)] + back_size + 4) return 20 + i;
        for (int i = 0; i < 5; i++)
            if (round_hit(transport_x[size_t(i)], transport_y, transport_size[size_t(i)])) return 30 + i;
        if (y >= 424) return x < 200 ? 40 : x >= 440 ? 41 : -1;
        return -1;
    }
    // The equaliser.
    if (y >= 20 && y < 72 && x >= 500) return 53; // the switch
    if (y >= preset.y - 4 && y < preset.y + preset.h + 4 && x >= preset.x && x < preset.x + preset.w)
        return x < 320 ? 50 : 51;
    if (y >= 424) return x < 200 ? 0 : x >= 440 ? 52 : -1;
    return -1;
}

void App::music_button(int button) {
    const Uint32 now = s.GetTicks();
    using View = MusicUi::View;
    switch (button) {
    case 0: action(3); break;
    case 1: music_set_tab((contact_x - (616 - music_layout::tabs * music_tab_width())) / music_tab_width()); break;
    case 10:
        music_show(View::Player);
        emit_sound(MenuCue::Open);
        break;
    case 11: music.send("toggle", now); break;
    case 12: music.send("next", now); break;
    case 20: music_choose_visualiser(); break;
    case 21:
        music_show(View::Equaliser);
        emit_sound(MenuCue::Open);
        break;
    case 22: music_toggle_favourite(); break;
    case 30: action(15); break;
    case 31: music.send("prev", now); break;
    case 32:
        if (music.loaded()) music.send("toggle", now);
        break;
    case 33: music.send("next", now); break;
    case 34: action(16); break;
    case 40: action(3); break;
    case 41:
        music_show(View::Queue);
        emit_sound(MenuCue::Open);
        break;
    case 50: music_step_preset(-1); break;
    case 51: music_step_preset(1); break;
    case 52: music_action(11); break;
    case 53: music_action(2); break;
    default: break;
    }
}

void App::music_press(int x, int y) {
    using namespace music_layout;
    using View = MusicUi::View;
    using Touch = MusicUi::Touch;
    const Uint32 now = s.GetTicks();
    last_activity = now;
    mu.bottom_changed = dirty = true;
    mu.focus_visible = false;
    mu.swiped = mu.held = false;
    mu.touch = Touch::None;
    mu.touch_row = mu.touch_button = -1;
    const int button = music_button_at(x, y);
    if (button >= 0) {
        mu.touch = Touch::Button;
        mu.touch_button = button;
        return;
    }
    if (mu.view == View::Browse || mu.view == View::Queue) {
        if (y < rows_top || y >= rows_top + rows_height) return;
        const int maximum = music_max_scroll();
        if (x >= rail_x - 6 && maximum > 0) {
            // The rail: grab the thumb where it is, or bring its middle under the finger.
            const int content = music_rows() * stride - (stride - row_height),
                      thumb = std::max(54, rows_height * rows_height / content);
            const int offset = music_scroll() * (rows_height - thumb) / maximum;
            mu.motion.stop();
            mu.touch = Touch::Rail;
            mu.rail_grab = y - rows_top >= offset && y - rows_top < offset + thumb ? y - rows_top - offset : thumb / 2;
            music_drag(x, y);
            return;
        }
        mu.touch = Touch::List;
        const int local = y - rows_top + music_scroll();
        mu.touch_row = local % stride < row_height && local / stride < music_rows() ? local / stride : -1;
        mu.scroll_at_press = music_scroll();
        mu.motion.begin(now, music_scroll());
        return;
    }
    if (mu.view == View::Player && music.loaded()) {
        if (y >= seek_y - 20 && y < seek_y + 28 && x >= seek_left - 24 && x < seek_right + 24) {
            mu.touch = Touch::Seek;
            mu.seeking = true;
            music_drag(x, y);
            haptic_detent();
        } else if (y >= art.y && y < art.y + art.h) mu.touch = Touch::Art; // the cover and the words beside it
        return;
    }
    if (mu.view == View::Equaliser && y >= band_top - 24 && y < band_bottom + 40 && x >= band_left &&
        x < band_left + band_pitch * music::eq_bands) {
        mu.touch = Touch::Band;
        mu.touch_row = mu.band = (x - band_left) / band_pitch;
        music_drag(x, y);
    }
}

void App::music_drag(int x, int y) {
    using namespace music_layout;
    using Touch = MusicUi::Touch;
    const Uint32 now = s.GetTicks();
    mu.bottom_changed = dirty = true;
    switch (mu.touch) {
    case Touch::List:
        if (std::abs(y - down_y) > 8) mu.swiped = true;
        if (mu.swiped && !mu.held)
            music_scroll() = mu.motion.drag(now, mu.scroll_at_press - (y - down_y), 0, music_max_scroll());
        break;
    case Touch::Rail: {
        const int maximum = music_max_scroll(), content = music_rows() * stride - (stride - row_height),
                  thumb = std::max(54, rows_height * rows_height / std::max(1, content));
        music_scroll() = std::clamp(
            int(std::lround(double(y - rows_top - mu.rail_grab) * maximum / std::max(1, rows_height - thumb))), 0,
            maximum);
        break;
    }
    case Touch::Seek: {
        const double part = std::clamp(double(x - seek_left - 2) / double(seek_right - seek_left - 4), 0.0, 1.0);
        mu.seek_ms = int64_t(part * double(music.now.duration_ms));
        break;
    }
    case Touch::Art:
        if (std::abs(x - down_x) > 10 || mu.art_shift) mu.art_shift = std::clamp(x - down_x, -120, 120);
        break;
    case Touch::Band:
        music_set_gain(mu.touch_row, float(band_y(0) - y) * music::eq_range / (float(band_bottom - band_top) / 2));
        break;
    default: break;
    }
}

void App::music_release(int x, int y) {
    using Touch = MusicUi::Touch;
    const Uint32 now = s.GetTicks();
    mu.bottom_changed = dirty = true;
    const Touch touch = mu.touch;
    mu.touch = Touch::None;
    switch (touch) {
    case Touch::Button:
        // A button acts when the finger lifts on it; sliding off cancels.
        if (music_button_at(x, y) == mu.touch_button) {
            contact_x = x;
            music_button(mu.touch_button);
        }
        break;
    case Touch::List:
        if (mu.swiped) mu.motion.release(now, 0, music_max_scroll());
        else {
            mu.motion.stop();
            if (!mu.held && mu.touch_row >= 0) music_activate(mu.touch_row);
        }
        break;
    case Touch::Seek:
        music.send("seek " + std::to_string(mu.seek_ms), now);
        // Shown where it was dropped until the engine says where it is.
        music.now.position_ms = mu.seek_ms;
        mu.seeking = false;
        break;
    case Touch::Art:
        if (std::abs(mu.art_shift) >= 72) {
            music.send(mu.art_shift < 0 ? "next" : "prev", now);
            haptics.play(static_cast<Haptic>(MenuCue::Confirm));
        } else if (!mu.art_shift && music.loaded() && x < music_layout::art.x + music_layout::art.w)
            music.send("toggle", now); // a tap on the cover
        mu.art_released_at = now;
        mu.art_released_from = mu.art_shift;
        break;
    default: break;
    }
}
