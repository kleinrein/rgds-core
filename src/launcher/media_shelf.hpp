#pragma once
// Media: Music, Videos and Books as one app (docs/MEDIA_PLAN.md). The music player's level grows two more sections
// when RG DS Media is installed: the section pills along the top of the lower screen pick Music, Videos or Books
// (L2/R2, or a tap), and the last one is remembered. Videos and Books are drawn here, in the launcher, so moving
// between the three is instant: a poster grid below, the picked film or book large over a blur of itself above.
// Only playing a film or reading a book hands the panels to rgds-media (media_launch.hpp), the way a game starts.
//
// What is on the card comes from `rgds-media --index` (play/src/media/library.cpp): it lists the shelves at once
// and then makes the posters (BMPs the picture loader reads) in the background, rewriting index.tsv as it goes.
// Where each film and book was left is rgds-media's places.tsv, read again whenever it changes.
// Members of App in main.cpp; included once, right after the class, before music_screen.hpp.

bool MediaGlide::step(double dt) {
    if (target != aimed) from = value, aimed = target, elapsed = 0; // a new choice glides on from where it is
    if (value == target) return false;
    elapsed += std::min(dt, 64.0);
    const double t = std::min(1.0, elapsed / 160), eased = 1 - (1 - t) * (1 - t) * (1 - t);
    value = t >= 1 ? target : from + (target - from) * eased;
    return true;
}

bool App::media_sections() const { return media_available() && media_indexes() && !o.game_menu && !probe; }

// An rgds-media from before the shelves moved here has no `--index`: the Media tile then opens it the old way, so
// a launcher that arrives ahead of its rgds-media (a test bind, a part-applied update) still works. Looked up once.
bool App::media_indexes() const {
    if (media_index_found < 0) {
        std::ifstream in(media_binary(), std::ios::binary);
        const std::string needle = "--index";
        std::string carry, chunk(1 << 16, '\0');
        media_index_found = 0;
        while (in && !media_index_found) {
            in.read(chunk.data(), std::streamsize(chunk.size()));
            const std::string seen = carry + chunk.substr(0, size_t(in.gcount()));
            media_index_found = seen.find(needle) != std::string::npos;
            carry = seen.size() >= needle.size() ? seen.substr(seen.size() - needle.size() + 1) : seen;
        }
    }
    return media_index_found == 1;
}

// The sections shown, in order: Music, Videos, Books.
std::vector<int> App::media_section_list() const {
    std::vector<int> out;
    if (music_available()) out.push_back(0);
    if (media_sections()) {
        out.push_back(1);
        out.push_back(2);
    }
    return out;
}

bool App::media_shelf_open() const { return music_open && mu.section != 0 && mu.view == MusicUi::View::Browse; }

fs::path App::media_state_dir() const { return o.player / "media"; }

std::vector<fs::path> App::media_roots(bool books) const {
    if (const char* root = std::getenv("RGDS_MEDIA_ROOT"); root && *root)
        return {fs::path(root) / (books ? "books" : "videos")};
    // As runtime/media.py: beside the games on the data partition.
    std::error_code ec;
    const fs::path base = o.rgds_os || fs::is_directory("/data/rgds", ec) ? fs::path("/data") : fs::path("/mnt/mmc");
    return {base / (books ? "books" : "videos")};
}

fs::path App::media_binary() const {
    std::error_code ec;
    if (const char* named = std::getenv("RGDS_MEDIA_BINARY"); named && *named && fs::exists(named, ec)) return named;
    if (!o.app.empty() && fs::exists(o.app / "emulators/rgds-media/rgds-media", ec))
        return o.app / "emulators/rgds-media/rgds-media";
    return "/usr/bin/rgds-media";
}

void App::media_load_section() {
    if (media_section_read) return;
    media_section_read = true;
    std::ifstream in(media_state_dir() / "launcher.ini");
    std::string line;
    while (std::getline(in, line))
        if (line.rfind("section=", 0) == 0) mu.section = std::clamp(std::atoi(line.c_str() + 8), 0, 2);
}

void App::media_save_section() {
    std::error_code ec;
    fs::create_directories(media_state_dir(), ec);
    const fs::path file = media_state_dir() / "launcher.ini", tmp = media_state_dir() / "launcher.ini.tmp";
    {
        std::ofstream out(tmp);
        out << "section=" << mu.section << '\n';
        if (!out) return;
    }
    fs::rename(tmp, file, ec);
}

// The section to open on: the last one, if it is still there.
void App::media_fit_section() {
    const auto list = media_section_list();
    if (list.empty()) return;
    if (std::find(list.begin(), list.end(), mu.section) == list.end()) mu.section = list.front();
    mu.section_pill.snap(double(std::find(list.begin(), list.end(), mu.section) - list.begin()));
    mu.tab_pill.snap(mu.tab);
}

void App::media_set_section(int section) {
    const auto list = media_section_list();
    const auto at = std::find(list.begin(), list.end(), section);
    if (at == list.end() || (section == mu.section && mu.view == MusicUi::View::Browse)) return;
    // The pills stay where they are and the lit one glides; what is under them crossfades in place, as a
    // segmented control's page does, rather than the whole screen sliding sideways under the pill.
    view_route = ViewTravel::Fade;
    mu.section = section;
    mu.view = MusicUi::View::Browse;
    mu.motion.stop();
    mu.touch = MusicUi::Touch::None;
    mu.section_pill.target = double(at - list.begin());
    if (!preferences.animations) mu.section_pill.snap(mu.section_pill.target);
    if (section == 0) music_level();
    else {
        media_start_index(false);
        media_build(section == 2);
        media_place_focus(true);
    }
    media_save_section();
    emit_sound(MenuCue::Navigate);
    mu.bottom_changed = dirty = true;
}

void App::media_step_section(int delta) {
    const auto list = media_section_list();
    if (list.size() < 2) return;
    const auto at = std::find(list.begin(), list.end(), mu.section);
    const int index = at == list.end() ? 0 : int(at - list.begin());
    media_set_section(list[size_t((index + delta + int(list.size())) % int(list.size()))]);
}

// ---- What is on the card

// rgds-media --index, in the background and kindly (it decodes a frame of every film for its poster).
void App::media_start_index(bool again) {
    if (!media_sections()) return;
    if (media_indexer > 0) return;
    if (media_index_started && !again) return;
    media_index_started = true;
    const fs::path binary = media_binary();
    std::error_code ec;
    if (!fs::exists(binary, ec)) return;
    fs::create_directories(media_state_dir(), ec);
    std::vector<std::string> args = {binary.string(), "--index", "--state", media_state_dir().string()};
    for (const fs::path& d : media_roots(false)) args.insert(args.end(), {"--videos", d.string()});
    for (const fs::path& d : media_roots(true)) args.insert(args.end(), {"--books", d.string()});
    const pid_t child = fork();
    if (child == 0) {
        if (nice(10) == -1) { // a poster can wait; the menu can not
        }
        const int null = ::open("/dev/null", O_RDWR);
        if (null >= 0) {
            dup2(null, 0);
            dup2(null, 1);
        }
        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(a.data());
        argv.push_back(nullptr);
        execv(argv[0], argv.data());
        _exit(127);
    }
    if (child > 0) media_indexer = child;
}

static int64_t media_stamp(const fs::path& file) {
    std::error_code ec;
    const auto t = fs::last_write_time(file, ec);
    if (ec) return -1;
    return int64_t(t.time_since_epoch().count()) ^ int64_t(fs::file_size(file, ec));
}

// index.tsv and places.tsv read again when they changed. True when something shown did.
bool App::media_poll(Uint32 now, bool force) {
    if (!force && now - mu.index_checked < 300) return false;
    mu.index_checked = now;
    if (media_indexer > 0) {
        int status = 0;
        if (waitpid(media_indexer, &status, WNOHANG) == media_indexer) media_indexer = -1;
    }
    bool changed = false;
    const std::string kept = mu.section != 0 ? media_focused_path() : std::string();
    const int64_t index = media_stamp(media_state_dir() / "index.tsv");
    if (index != mu.index_seen) {
        mu.index_seen = index;
        std::vector<MediaEntry> entries;
        std::ifstream in(media_state_dir() / "index.tsv");
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::vector<std::string> f;
            size_t start = 0;
            for (size_t tab = line.find('\t'); tab != std::string::npos; tab = line.find('\t', start)) {
                f.push_back(line.substr(start, tab - start));
                start = tab + 1;
            }
            f.push_back(line.substr(start));
            if (f.size() < 12) continue;
            MediaEntry e;
            e.books = f[0] == "books";
            e.kind = f[1] == "folder" ? MediaEntry::Kind::Folder
                     : f[1] == "book" ? MediaEntry::Kind::Book
                                      : MediaEntry::Kind::Video;
            e.parent = f[2];
            e.path = f[3];
            e.name = f[4];
            e.key = f[5];
            e.length = std::atof(f[6].c_str());
            e.picture = f[7];
            e.blur = f[8];
            e.children = std::atoi(f[9].c_str());
            e.size = uint64_t(std::atoll(f[10].c_str()));
            e.ext = f[11];
            if (f.size() > 12) e.shade = std::clamp(std::atoi(f[12].c_str()), 60, 230);
            entries.push_back(std::move(e));
        }
        // A new poster has a path of its own, never asked for before: the picture cache needs no refresh.
        mu.entries = std::move(entries);
        changed = true;
    }
    const int64_t places = media_stamp(media_state_dir() / "places.tsv");
    if (places != mu.places_seen) {
        mu.places_seen = places;
        mu.places.clear();
        std::ifstream in(media_state_dir() / "places.tsv");
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream row(line);
            std::string key, position, length, when;
            if (!std::getline(row, key, '\t') || !std::getline(row, position, '\t') ||
                !std::getline(row, length, '\t') || !std::getline(row, when, '\t'))
                continue;
            MediaPlace p;
            p.position = std::atof(position.c_str());
            p.length = std::atof(length.c_str());
            p.when = std::atoll(when.c_str());
            mu.places[key] = p;
        }
        changed = true;
    }
    if (changed && mu.section != 0) {
        // The page keeps its focus on the same item while the shelf fills in.
        MediaShelf& shelf = media_shelf();
        media_build(mu.section == 2);
        for (size_t i = 0; i < shelf.items.size(); i++)
            if (mu.entries[size_t(shelf.items[i])].path == kept) shelf.focus = int(i);
        // Never under a finger or a flick: the shelf filling in must not move what is being scrolled.
        if (!pointer_down && !mu.motion.active()) media_place_focus(true);
        mu.bottom_changed = dirty = true;
    }
    return changed;
}

std::string App::media_focused_path() {
    const MediaShelf& shelf = media_shelf();
    if (shelf.focus < 0 || shelf.focus >= int(shelf.items.size())) return {};
    const int at = shelf.items[size_t(shelf.focus)];
    return at >= 0 && at < int(mu.entries.size()) ? mu.entries[size_t(at)].path : std::string();
}

MediaPlace App::media_place(const MediaEntry& e) const {
    const auto found = mu.places.find(e.key);
    return found == mu.places.end() ? MediaPlace{} : found->second;
}

MediaShelf& App::media_shelf() { return mu.shelves[mu.section == 2 ? 1 : 0]; }

// The page: the shelf's own folders together at its root, or the folder opened; what is being watched or read
// first, newest first, then folders and files in name order (as the index has them).
void App::media_build(bool books) {
    MediaShelf& shelf = mu.shelves[books ? 1 : 0];
    shelf.items.clear();
    std::set<std::string> roots;
    for (const fs::path& d : media_roots(books)) roots.insert(d.string());
    const std::string here = shelf.path.empty() ? std::string() : shelf.path.back();
    for (size_t i = 0; i < mu.entries.size(); i++) {
        const MediaEntry& e = mu.entries[i];
        if (e.books != books) continue;
        if (here.empty() ? !roots.count(e.parent) : e.parent != here) continue;
        shelf.items.push_back(int(i));
    }
    std::stable_sort(shelf.items.begin(), shelf.items.end(), [&](int a, int b) {
        const MediaPlace pa = media_place(mu.entries[size_t(a)]), pb = media_place(mu.entries[size_t(b)]);
        const bool ca = pa.started() && !pa.finished(), cb = pb.started() && !pb.finished();
        if (ca != cb) return ca;
        if (ca && cb) return pa.when > pb.when;
        return false;
    });
    shelf.focus = std::clamp(shelf.focus, 0, std::max(0, int(shelf.items.size()) - 1));
    shelf.scroll = std::clamp(shelf.scroll, 0, media_max_scroll());
}

// ---- The grid

MediaGrid App::media_grid() const { return mu.section == 2 ? media_layout::book_grid : media_layout::video_grid; }

SDL_Rect App::media_tile_box(int i) const {
    const MediaGrid g = media_grid();
    return {g.x0() + (i % g.cols) * (g.tile_w + g.gap), media_layout::grid_top + 10 + (i / g.cols) * (g.tile_h + g.gap),
            g.tile_w, g.tile_h};
}

int App::media_max_scroll() {
    const MediaShelf& shelf = media_shelf();
    if (shelf.items.empty()) return 0;
    const SDL_Rect last = media_tile_box(int(shelf.items.size()) - 1);
    return std::max(0, last.y + last.h + 4 - media_layout::grid_bottom);
}

void App::media_place_focus(bool snap) {
    MediaShelf& shelf = media_shelf();
    if (shelf.items.empty()) return;
    const SDL_Rect b = media_tile_box(shelf.focus);
    // The focused tile in view with a little air; the grid glides there.
    const int before = shelf.scroll;
    if (b.y - 10 - shelf.scroll < media_layout::grid_top) shelf.scroll = b.y - 10 - media_layout::grid_top;
    if (b.y + b.h + 8 - shelf.scroll > media_layout::grid_bottom)
        shelf.scroll = b.y + b.h + 8 - media_layout::grid_bottom;
    shelf.scroll = std::clamp(shelf.scroll, 0, media_max_scroll());
    if (shelf.scroll != before) mu.motion.glide(s.GetTicks(), before, shelf.scroll, preferences.animations && !snap);
    shelf.placed = true;
    const std::string key = media_focused_path();
    if (key != mu.hero_key) {
        mu.hero_prev = snap && mu.hero_key.empty() ? std::string() : mu.hero_key;
        mu.hero_key = key;
        mu.hero_at = s.GetTicks();
    }
    mu.bottom_changed = dirty = true;
}

void App::media_move(int dx, int dy) {
    MediaShelf& shelf = media_shelf();
    const int n = int(shelf.items.size());
    if (!n) return;
    const int cols = media_grid().cols;
    int f = shelf.focus;
    if (dx) f = std::clamp(f + dx, 0, n - 1);
    if (dy < 0 && f - cols >= 0) f -= cols;
    if (dy > 0) f = f + cols < n ? f + cols : (f / cols < (n - 1) / cols ? n - 1 : f);
    mu.focus_visible = true;
    if (f == shelf.focus) return;
    shelf.focus = f;
    media_place_focus(false);
    emit_sound(MenuCue::Navigate, dx + dy < 0 ? -1 : 1);
}

int App::media_hit(int x, int y) {
    const MediaShelf& shelf = media_shelf();
    if (y < media_layout::grid_top || y >= media_layout::grid_bottom) return -1;
    const int sy = y + shelf.scroll;
    for (int i = 0; i < int(shelf.items.size()); i++) {
        const SDL_Rect b = media_tile_box(i);
        if (x >= b.x && x < b.x + b.w && sy >= b.y && sy < b.y + b.h) return i;
    }
    return -1;
}

void App::media_activate(int index) {
    MediaShelf& shelf = media_shelf();
    if (index < 0 || index >= int(shelf.items.size())) return;
    shelf.focus = index;
    const MediaEntry e = mu.entries[size_t(shelf.items[size_t(index)])];
    if (e.kind == MediaEntry::Kind::Folder) {
        shelf.path.push_back(e.path);
        shelf.focus = shelf.scroll = 0;
        shelf.placed = false;
        mu.motion.stop();
        media_build(mu.section == 2);
        media_place_focus(true);
        view_route = ViewTravel::Forward;
        page_at = s.GetTicks();
        emit_sound(MenuCue::Open);
        mu.bottom_changed = dirty = true;
        return;
    }
    media_open_file(e.path);
}

void App::media_open_file(const std::string& path) {
    media_launch_file = path;
    launch_media();
}

void App::media_resume_latest() {
    MediaShelf& shelf = media_shelf();
    for (int i = 0; i < int(shelf.items.size()); i++) {
        const MediaEntry& e = mu.entries[size_t(shelf.items[size_t(i)])];
        const MediaPlace p = media_place(e);
        if (e.kind != MediaEntry::Kind::Folder && p.started() && !p.finished()) {
            shelf.focus = i;
            media_place_focus(false);
            media_activate(i);
            return;
        }
    }
}

// Where a film or book was left, changed by hand (Start over, Mark as finished): rgds-media's own file.
void App::media_set_place(const MediaEntry& e, double position) {
    const fs::path file = media_state_dir() / "places.tsv", tmp = media_state_dir() / "places.tsv.tmp";
    std::vector<std::string> lines;
    std::string line, extra;
    double length = e.length;
    {
        std::ifstream in(file);
        while (std::getline(in, line)) {
            if (line.rfind(e.key + '\t', 0) == 0) {
                std::istringstream row(line);
                std::string key, pos, len, when;
                std::getline(row, key, '\t');
                std::getline(row, pos, '\t');
                std::getline(row, len, '\t');
                std::getline(row, when, '\t');
                std::getline(row, extra);
                if (std::atof(len.c_str()) > 0) length = std::atof(len.c_str());
                continue;
            }
            lines.push_back(line);
        }
    }
    if (position < 0) position = length; // finished
    std::ostringstream row;
    row << e.key << '\t' << position << '\t' << length << '\t' << int64_t(std::time(nullptr)) << '\t' << extra;
    lines.push_back(row.str());
    {
        std::ofstream out(tmp);
        for (const auto& l : lines) out << l << '\n';
        if (!out) return;
    }
    std::error_code ec;
    fs::rename(tmp, file, ec);
    media_poll(s.GetTicks(), true);
}

void App::media_options(int index) {
    MediaShelf& shelf = media_shelf();
    if (index < 0 || index >= int(shelf.items.size())) return;
    shelf.focus = index;
    const MediaEntry e = mu.entries[size_t(shelf.items[size_t(index)])];
    if (e.kind == MediaEntry::Kind::Folder) return;
    const MediaPlace p = media_place(e);
    const bool books = e.books;
    std::vector<std::string> options;
    options.push_back(p.started() && !p.finished() ? (books ? "Read on" : "Resume") : books ? "Read" : "Play");
    if (p.started()) options.push_back(books ? "Start from the beginning" : "Play from the start");
    options.push_back(p.finished() ? "Mark as not finished" : "Mark as finished");
    show_picker(e.name, options, 0, [this, e, options](int choice) {
        const std::string& chosen = options[size_t(std::clamp(choice, 0, int(options.size()) - 1))];
        if (chosen == "Mark as finished") media_set_place(e, -1);
        else if (chosen == "Mark as not finished") media_set_place(e, 0);
        else {
            if (chosen == "Play from the start" || chosen == "Start from the beginning") media_set_place(e, 0);
            media_open_file(e.path);
        }
        mu.bottom_changed = dirty = true;
    });
}

bool App::media_back() {
    MediaShelf& shelf = media_shelf();
    if (shelf.path.empty()) return false;
    const std::string left = shelf.path.back();
    shelf.path.pop_back();
    shelf.focus = 0;
    mu.motion.stop();
    media_build(mu.section == 2);
    for (size_t i = 0; i < shelf.items.size(); i++)
        if (mu.entries[size_t(shelf.items[i])].path == left) shelf.focus = int(i);
    shelf.placed = false;
    media_place_focus(true);
    page_at = s.GetTicks();
    emit_sound(MenuCue::Back);
    mu.bottom_changed = dirty = true;
    return true;
}

// Every frame the Media level is open: the pills, the focus gliding, the shelves filling in.
bool App::media_tick(Uint32 now) {
    const double dt = mu.media_ticked ? double(now - mu.media_ticked) : 16;
    mu.media_ticked = now;
    bool moving = false;
    moving |= mu.section_pill.step(dt);
    moving |= mu.tab_pill.step(dt);
    if (mu.section == 0 && now - mu.music_hero_at < 160) moving = true; // Music's hero fading in
    if (mu.section != 0) {
        MediaShelf& shelf = media_shelf();
        if (now - mu.hero_at < 160) moving = true;
        if (media_poll(now, false)) moving = true;
        // Posters for what is in view: the loader reads them off the card on its own thread.
        if (!surfaces.empty()) {
            SDL_Renderer* lower = bottom_renderer();
            const int first = std::max(0, (shelf.scroll / std::max(1, media_grid().tile_h)) * media_grid().cols -
                                              media_grid().cols),
                      last = std::min(int(shelf.items.size()), first + media_grid().cols * 5);
            for (int i = first; i < last; i++) {
                const MediaEntry& e = mu.entries[size_t(shelf.items[size_t(i)])];
                if (!e.picture.empty()) media.request(lower, e.picture);
            }
        }
    }
    if (moving) mu.bottom_changed = true;
    return moving;
}

// Words broken into lines that fit; the last line takes the rest and ends in an ellipsis. Returns the lines drawn.
int App::media_wrapped(SDL_Renderer* r, const std::string& value, int x, int y, int pixels, int width, int rows,
                       Color ink) {
    std::vector<std::string> lines;
    std::string line;
    const auto measure = [&](const std::string& text) {
        return preferences.pixel ? float(pixel_cells(text) * std::max(2, (pixels + 6) / 10) * 6)
                                 : rounded.width(text, pixels);
    };
    for (const auto& piece : line_pieces(value)) {
        const std::string candidate = line.empty() ? piece.text : line + (piece.space_before ? " " : "") + piece.text;
        if (!line.empty() && measure(candidate) > width && int(lines.size()) + 1 < rows) {
            lines.push_back(line);
            line = piece.text;
        } else line = candidate;
    }
    if (!line.empty()) lines.push_back(line);
    for (size_t i = 0; i < lines.size(); i++)
        raw_label_colour(r, lines[i], x, y + int(i) * (pixels + 7), pixels, ink, width);
    return int(lines.size());
}

// ---- Drawing: the pills

// A row of pills: a sunken track for each, and a lit glass pill that slides between them.
void App::media_pill_row(SDL_Renderer* r, int left, int top, int width, int height, int gap,
                         const std::vector<std::string>& names, const std::vector<MusicGlyph>& glyphs, double lit,
                         int pressed, int size) {
    const int n = int(names.size());
    if (!n) return;
    const int w = (width - (n - 1) * gap) / n, radius = height / 2;
    for (int i = 0; i < n; i++) {
        const int x = left + i * (w + gap);
        fill_round(r, x, top, w, height, radius, theme.at("line"));
        fill_round(r, x + 1, top + 1, w - 2, height - 2, radius - 1, theme.at("slot"));
    }
    const double x = left + lit * (w + gap);
    alpha_round(r, int(std::lround(x)), top + 3, w, height, radius, shadow_colour(), preferences.dark ? 70 : 30);
    glossy_pill(r, int(std::lround(x)), top, w, height, radius, theme.at("accent"),
                pressed == int(std::lround(lit)) ? 1 : 0);
    const Color ink = theme.at("ink"), on = theme.at("on_accent");
    for (int i = 0; i < n; i++) {
        const double amount = 1 - std::min(1.0, std::abs(lit - i));
        const Color c = blend(ink, on, int(std::lround(amount * 100)));
        const std::string word = locale.text(names[size_t(i)]);
        const int glyph = glyphs.empty() ? 0 : size + 4;
        const int room = w - 20 - glyph, tw = std::min(room, label_width(word, size, false));
        const int x0 = left + i * (w + gap) + (w - tw - glyph) / 2, cy = top + height / 2 + (pressed == i ? 1 : 0);
        if (!glyphs.empty())
            music_glyph(r, glyphs[size_t(i)], x0 + size / 2, cy, preferences.rich_icons ? size : 12, c);
        raw_label_colour(r, word, x0 + glyph, centred_label_y(word, size, cy), size, c, room + 4);
    }
}

void App::media_section_pills(SDL_Renderer* r, int y) {
    using namespace media_layout;
    const auto list = media_section_list();
    std::vector<std::string> names;
    std::vector<MusicGlyph> glyphs;
    for (int section : list) {
        names.push_back(section_names[size_t(section)]);
        glyphs.push_back(section == 0 ? MusicGlyph::Note : section == 1 ? MusicGlyph::Film : MusicGlyph::Book);
    }
    const int pressed =
        pointer_down && mu.touch == MusicUi::Touch::Button && mu.touch_button >= 60 && mu.touch_button < 63
            ? mu.touch_button - 60
            : -1;
    media_pill_row(r, pills_left, y + pills_y, pills_right - pills_left, pills_h, pills_gap, names, glyphs,
                   mu.section_pill.value, pressed, 18);
}

void App::media_tab_pills(SDL_Renderer* r, int y) {
    using namespace media_layout;
    std::vector<std::string> names(music_layout::tab_names.begin(), music_layout::tab_names.end());
    const int pressed =
        pointer_down && mu.touch == MusicUi::Touch::Button && mu.touch_button >= 70 && mu.touch_button < 75
            ? mu.touch_button - 70
            : -1;
    media_pill_row(r, tabs_left, y + tabs_y, tabs_right - tabs_left, tabs_h, 4, names, {}, mu.tab_pill.value, pressed,
                   16);
}

// Back, the section pills and the rule under them.
void App::media_header(SDL_Renderer* r, int y, int rule) {
    const bool pressed =
        pointer_down && mu.touch == MusicUi::Touch::Button && mu.touch_button == 0 && contact_x < 84 && contact_y < 78;
    back_button(r, 28, y + 20, pressed);
    media_section_pills(r, y);
    rect(r, 24, y + rule, 592, 1, "line");
}

// The header of an opened folder: Back and the folder's name, as the music player's pages.
void App::media_folder_header(SDL_Renderer* r, int y) {
    const MediaShelf& shelf = media_shelf();
    music_header(r, y, fs::path(shelf.path.back()).filename().string(), false, 520);
}

// ---- Drawing: the grid

void App::media_tile(SDL_Renderer* r, SDL_Rect b, const MediaEntry& e, bool pressed) {
    const MediaGrid g = media_grid();
    const SDL_Rect pic{b.x, b.y, g.pic_w, g.pic_h};
    const int radius = 10;
    alpha_round(r, pic.x + 1, pic.y + 3, pic.w - 2, pic.h, radius, shadow_colour(), preferences.dark ? 90 : 34);
    SDL_Renderer* lower = r;
    bool drawn = false;
    if (!e.picture.empty()) {
        if (!media.ready(lower, e.picture)) media.request(lower, e.picture);
        drawn = media.draw(lower, e.picture, pic, false, 255, true, radius);
    }
    if (!drawn) {
        glossy_pill(r, pic.x, pic.y, pic.w, pic.h, radius,
                    e.kind == MediaEntry::Kind::Folder ? theme.at("focus") : theme.at("panel"), 0, false);
        music_glyph(r,
                    e.kind == MediaEntry::Kind::Folder ? MusicGlyph::Folder
                    : e.books                          ? MusicGlyph::Book
                                                       : MusicGlyph::Film,
                    pic.x + pic.w / 2, pic.y + pic.h / 2, preferences.rich_icons ? 36 : 36, theme.at("muted"));
    }
    const Color white{255, 255, 255};
    const MediaPlace place = media_place(e);
    if (e.kind == MediaEntry::Kind::Folder) {
        // A folder: its sign and how much is inside, in chips on the picture.
        alpha_round(r, pic.x + 8, pic.y + 8, 34, 24, 8, Color{0, 0, 0}, 150);
        music_glyph(r, MusicGlyph::Folder, pic.x + 25, pic.y + 20, preferences.rich_icons ? 16 : 12, white);
        const std::string n = std::to_string(e.children);
        const int cw = label_width(n, 14, false) + 14;
        alpha_round(r, pic.x + pic.w - cw - 8, pic.y + 8, cw, 24, 8, Color{0, 0, 0}, 150);
        raw_label_colour(r, n, pic.x + pic.w - cw - 1, centred_label_y(n, 14, pic.y + 20), 14, white, cw);
    } else {
        const double length = place.length > 0 ? place.length : e.length;
        const std::string chip = length <= 0 ? ""
                                 : e.books   ? std::to_string(int(length)) + " p"
                                             : music_time(int64_t(length * 1000));
        if (!chip.empty()) {
            const int cw = label_width(chip, 14, false) + 14;
            alpha_round(r, pic.x + pic.w - cw - 6, pic.y + pic.h - 30, cw, 24, 8, Color{0, 0, 0}, 160);
            raw_label_colour(r, chip, pic.x + pic.w - cw + 1, centred_label_y(chip, 14, pic.y + pic.h - 18), 14, white,
                             cw);
        }
        if (place.started()) {
            const double f = place.finished() ? 1.0 : place.fraction();
            alpha_round(r, pic.x + 10, pic.y + pic.h - 6, pic.w - 20, 4, 2, Color{0, 0, 0}, 140);
            fill_round(r, pic.x + 10, pic.y + pic.h - 6, std::max(4, int((pic.w - 20) * f)), 4, 2,
                       place.finished() ? theme.at("muted") : theme.at("accent"));
        }
    }
    if (pressed) alpha_round(r, pic.x, pic.y, pic.w, pic.h, radius, Color{0, 0, 0}, 50);
    // The title under the picture: two lines for a film, one for a book.
    if (e.books) raw_label(r, e.name, b.x + 2, b.y + g.pic_h + 8, 16, "ink", b.w - 4);
    else media_wrapped(r, e.name, b.x + 2, b.y + g.pic_h + 6, 16, b.w - 4, 2, theme.at("ink"));
}

// Four accent corners around the focused picture, gliding from tile to tile.
void App::media_corners(SDL_Renderer* r, double x, double y, double w, double h) {
    const Color accent = theme.at("accent");
    const int l = 20, t = 4, out = 5;
    const int x0 = int(std::lround(x)) - out, y0 = int(std::lround(y)) - out, x1 = int(std::lround(x + w)) + out - t,
              y1 = int(std::lround(y + h)) + out - t;
    for (const auto& [cx, cy] : {std::pair{x0, y0}, std::pair{x1, y0}, std::pair{x0, y1}, std::pair{x1, y1}}) {
        const bool right = cx == x1, bottom = cy == y1;
        fill_round(r, right ? cx + t - l : cx, cy, l, t, t / 2, accent);
        fill_round(r, cx, bottom ? cy + t - l : cy, t, l, t / 2, accent);
    }
}

void App::media_bottom(SDL_Renderer* r, int y) {
    using namespace media_layout;
    MediaShelf& shelf = media_shelf();
    const bool books = mu.section == 2;
    if (shelf.path.empty()) media_header(r, y, rule_y);
    else media_folder_header(r, y);
    if (shelf.items.empty()) {
        const bool working = media_indexer > 0 && mu.entries.empty();
        music_glyph(r, books ? MusicGlyph::Book : MusicGlyph::Film, 320, y + 180, preferences.rich_icons ? 64 : 60,
                    theme.at("muted"));
        if (working) centered(r, "Looking for videos and books…", y + 232, 2, "muted");
        else {
            centered(r, books ? "No books yet" : "No videos yet", y + 232, 2, "ink");
            centered_wrapped(r,
                             books ? "Copy PDF, EPUB, CBZ or folders of pages into the books folder."
                                   : "Copy films and series into the videos folder.",
                             y + 272, 16, 480, 3, "muted");
        }
    } else {
        const MediaGrid g = media_grid();
        const int shift = int(std::lround(mu.motion.stretch(shelf.scroll)));
        const int scroll = shelf.scroll + shift;
        SDL_Rect clip{0, y + grid_top, 640, grid_bottom - grid_top};
        s.RenderSetClipRect(r, &clip);
        const int row_h = g.tile_h + g.gap;
        const int first = std::max(0, (scroll - 10) / row_h * g.cols),
                  last = std::min(int(shelf.items.size()), first + g.cols * ((grid_bottom - grid_top) / row_h + 3));
        for (int i = first; i < last; i++) {
            SDL_Rect b = media_tile_box(i);
            b.y += y - scroll;
            if (b.y + b.h < y + grid_top || b.y > y + grid_bottom) continue;
            const bool pressed = pointer_down && mu.touch == MusicUi::Touch::List && !mu.swiped && mu.touch_row == i;
            media_tile(r, b, mu.entries[size_t(shelf.items[size_t(i)])], pressed);
        }
        // The corners stay on, as the media app had them: they say what A and the upper screen are about.
        if (!(pointer_down && mu.swiped)) {
            const SDL_Rect f = media_tile_box(shelf.focus);
            media_corners(r, f.x, f.y + y - scroll, f.w, g.pic_h);
        }
        s.RenderSetClipRect(r, nullptr);
        const int view = grid_bottom - grid_top, maximum = media_max_scroll();
        if (maximum > 0) {
            const int content = view + maximum, thumb = std::max(54, view * view / content);
            draw_scrollbar(r, 626, y + grid_top, view, thumb,
                           std::clamp(shelf.scroll, 0, maximum) * (view - thumb) / maximum,
                           mu.touch == MusicUi::Touch::Rail, false);
        }
    }
    music_mini(r, y);
}

// ---- Drawing: the upper screen

// The picked film or book, large, over a blur of its own picture. Drawn straight each frame: the blur is a
// 24-pixel picture the GPU scales up, the rest are the launcher's own textures.
void App::media_hero(SDL_Renderer* r, int y, const MediaEntry& e, int alpha) {
    const bool picture = !e.picture.empty() && media.ready(r, e.picture);
    if (!e.blur.empty()) {
        if (!media.ready(r, e.blur)) media.request(r, e.blur, false, true);
        media.draw(r, e.blur, {-48, y - 36, 736, 552}, false, Uint8(alpha), true);
    }
    s.SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    blend_rect(r, 0, y, 640, 480, Color{6, 8, 12}, e.shade * alpha / 255);
    if (!e.picture.empty() && !picture) media.request(r, e.picture, false, true);
    const bool tall = e.books;
    SDL_Rect box;
    if (tall) box = {40, y + 66, 222, 312};
    else box = {120, y + 58, 400, 225};
    if (picture) box = media.fitted_box(r, e.picture, box);
    rounded.opacity = alpha;
    alpha_round(r, box.x - 2, box.y + 8, box.w + 4, box.h + 2, 16, Color{0, 0, 0}, 110 * alpha / 255);
    if (!picture || !media.draw(r, e.picture, box, false, Uint8(alpha), true, 14)) {
        alpha_round(r, box.x, box.y, box.w, box.h, 14, theme.at("panel"), alpha);
        music_glyph(r,
                    e.kind == MediaEntry::Kind::Folder ? MusicGlyph::Folder
                    : e.books                          ? MusicGlyph::Book
                                                       : MusicGlyph::Film,
                    box.x + box.w / 2, box.y + box.h / 2, 64, theme.at("muted"), alpha);
    }
    const Color white{255, 255, 255}, soft{214, 222, 232};
    const int tx = tall ? box.x + box.w + 28 : 44, tw = 640 - tx - 44;
    int ty = tall ? y + 92 : box.y + box.h + 18;
    // A title that fits one line leaves no hole under it.
    ty += media_wrapped(r, e.name, tx, ty, 28, tw, tall ? 4 : 2, white) * 35 + 4;
    const MediaPlace place = media_place(e);
    std::string meta;
    const double length = place.length > 0 ? place.length : e.length;
    if (e.kind == MediaEntry::Kind::Folder) meta = music_count(size_t(e.children), " item", " items");
    else if (e.books) {
        if (length > 0) meta = std::to_string(int(length)) + " " + locale.text("pages");
        std::string ext = e.ext.empty() ? "" : e.ext.substr(1);
        for (auto& ch : ext) ch = char(std::toupper(static_cast<unsigned char>(ch)));
        if (!ext.empty()) meta += (meta.empty() ? "" : "  ·  ") + ext;
    } else {
        if (length > 0) meta = music_time(int64_t(length * 1000));
        if (e.size >= 1000000) {
            char size[32];
            if (e.size < 1000000000) std::snprintf(size, sizeof size, "%d MB", int(e.size / 1000000));
            else std::snprintf(size, sizeof size, "%.1f GB", double(e.size) / 1e9);
            meta += (meta.empty() ? "" : "  ·  ") + std::string(size);
        }
    }
    if (!meta.empty()) {
        raw_label_colour(r, meta, tx, ty, 18, soft, tw);
        ty += 30;
    }
    if (place.started() && e.kind != MediaEntry::Kind::Folder) {
        std::string where;
        if (place.finished()) where = locale.text("Finished");
        else if (e.books)
            where = locale.text("Page") + " " + std::to_string(int(place.position) + 1) +
                    (place.length > 0 ? " / " + std::to_string(int(place.length)) : "");
        else
            where = locale.text("Resume at") + " " + music_time(int64_t(place.position * 1000)) + "  ·  " +
                    music_time(int64_t(std::max(0.0, place.length - place.position) * 1000)) + " " +
                    locale.text("left");
        raw_label_colour(r, where, tx, ty, 18, white, tw);
        ty += 28;
        const int bw = std::min(tw, 360);
        alpha_round(r, tx, ty, bw, 6, 3, white, 60 * alpha / 255);
        alpha_round(r, tx, ty, std::max(6, int(bw * (place.finished() ? 1.0 : place.fraction()))), 6, 3,
                    theme.at("accent"), alpha);
        ty += 16;
    }
    rounded.opacity = 255;
}

// Key tips over the dark ground: light words, whatever the theme.
void App::media_hero_hints(SDL_Renderer* r, int y, const std::vector<KeyHint>& hints) {
    const Color ink = theme.at("ink"), muted = theme.at("muted");
    theme["ink"] = theme["muted"] = Color{226, 232, 240};
    key_hints(r, 320, y + 438, hints, 0, 16, 300);
    theme["ink"] = ink;
    theme["muted"] = muted;
}

void App::media_top(SDL_Renderer* r, int y) {
    rect(r, 0, y, 640, 480, "background");
    fill_round(r, 0, y, 640, 480, 0, Color{12, 14, 20});
    const MediaShelf& shelf = media_shelf();
    const auto entry = [&](const std::string& path) -> const MediaEntry* {
        for (const MediaEntry& e : mu.entries)
            if (e.path == path) return &e;
        return nullptr;
    };
    const Uint32 now = s.GetTicks();
    // A new film or book crossfades in quickly and in place, as the upper screen changes everywhere; while a
    // section change runs, the view's own crossfade is the only one.
    const double t =
        preferences.animations && !view_motion.running ? std::min(1.0, double(now - mu.hero_at) / 140.0) : 1.0;
    const double f = 1 - (1 - t) * (1 - t);
    const MediaEntry* before = mu.hero_prev.empty() || f >= 1 ? nullptr : entry(mu.hero_prev);
    const MediaEntry* current = shelf.items.empty() ? nullptr : entry(mu.hero_key);
    if (before) media_hero(r, y, *before, int(255 * (1 - f)));
    if (current) media_hero(r, y, *current, int(255 * f));
    if (!current && !before) {
        music_glyph(r, mu.section == 2 ? MusicGlyph::Book : MusicGlyph::Film, 320, y + 200, 96, Color{120, 130, 145});
    }
    // The status bar over a band of the theme, as on every page.
    rect(r, 0, y, 640, 48, "background");
    status_bar(r, y);
    media_section_shoulders(r, y);
    if (current && current->kind != MediaEntry::Kind::Folder) {
        const MediaPlace p = media_place(*current);
        const std::string a = p.started() && !p.finished() ? (current->books ? "Read on" : "Resume")
                              : current->books             ? "Read"
                                                           : "Play";
        media_hero_hints(r, y, {{"A", a}, {"X", "Options"}});
    } else if (current) media_hero_hints(r, y, {{"A", "Open"}});
}

// The shoulders name the sections either side.
void App::media_section_shoulders(SDL_Renderer* r, int y) {
    const auto list = media_section_list();
    if (list.size() < 2) return;
    const auto at = std::find(list.begin(), list.end(), mu.section);
    const int i = at == list.end() ? 0 : int(at - list.begin()), n = int(list.size());
    shoulder_tab(r, y, false, "L", locale.text(media_layout::section_names[size_t(list[size_t((i + n - 1) % n)])]));
    shoulder_tab(r, y, true, "R", locale.text(media_layout::section_names[size_t(list[size_t((i + 1) % n)])]));
}

// Music's library under Media: the focused row's cover large over a blur of itself (the engine's 44 px cover,
// scaled up smooth), its words beside it, as Videos and Books show a film or a book.
void App::music_hero(SDL_Renderer* r, int y) {
    using Kind = MusicRow::Kind;
    MusicLevel& level = music_level();
    const MusicRow& row = level.rows[size_t(std::clamp(level.focus, 0, int(level.rows.size()) - 1))];
    const bool covered = row.kind == Kind::Track || row.kind == Kind::Album || row.kind == Kind::Artist;
    const std::string art = covered ? row.art : std::string();
    // A new row cross-fades in, as the shelves' hero does.
    const std::string key = row.title + "\n" + row.note;
    const Uint32 now = s.GetTicks();
    if (key != mu.music_hero_key) {
        mu.music_hero_key = key;
        mu.music_hero_at = now;
    }
    const double t =
        preferences.animations && !view_motion.running ? std::min(1.0, double(now - mu.music_hero_at) / 140.0) : 1.0;
    const double f = 1 - (1 - t) * (1 - t);
    fill_round(r, 0, y, 640, 480, 0, Color{12, 14, 20});
    if (!art.empty()) {
        const fs::path small = music.folder / "art" / (art + "-44.bmp");
        if (!media.ready(r, small)) media.request(r, small, false, true);
        media.draw(r, small, {-48, y - 36, 736, 552}, false, Uint8(255 * f), true);
    }
    s.SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    blend_rect(r, 0, y, 640, 480, Color{6, 8, 12}, 150);
    const int alpha = int(255 * f);
    const SDL_Rect box{44, y + 70, 200, 200};
    alpha_round(r, box.x - 2, box.y + 8, box.w + 4, box.h + 2, 16, Color{0, 0, 0}, 110 * alpha / 255);
    const MusicGlyph stand_in = row.kind == Kind::Folder    ? MusicGlyph::Folder
                                : row.kind == Kind::Artist  ? MusicGlyph::Person
                                : row.kind == Kind::Album   ? MusicGlyph::Disc
                                : row.kind == Kind::Shuffle ? MusicGlyph::Shuffle
                                : row.kind == Kind::List    ? (row.id == 0 ? MusicGlyph::Heart : MusicGlyph::List)
                                                            : MusicGlyph::Note;
    if (art.empty()) {
        alpha_round(r, box.x, box.y, box.w, box.h, 16, theme.at("accent"), alpha);
        music_glyph(r, stand_in, box.x + box.w / 2, box.y + box.h / 2, preferences.rich_icons ? 96 : 96,
                    theme.at("on_accent"), alpha);
    } else {
        rounded.opacity = alpha;
        music_art(r, art, box, 16, stand_in);
    }
    rounded.opacity = alpha;
    const Color white{255, 255, 255}, soft{214, 222, 232};
    const int tx = 272, tw = 640 - tx - 40;
    int ty = y + 92;
    ty += media_wrapped(r, row.title, tx, ty, 28, tw, 3, white) * 35 + 4;
    raw_label_colour(r, row.note, tx, ty, 18, soft, tw);
    ty += 30;
    if (!row.side.empty()) raw_label_colour(r, row.side, tx, ty, 18, soft, tw);
    rounded.opacity = 255;
    // What there is, along the foot of the picture.
    const MusicLibrary& library = music.library;
    const std::string counts = music_count(library.tracks.size(), " song", " songs") + "  ·  " +
                               music_count(library.albums.size(), " album", " albums") + "  ·  " +
                               music_count(library.artists.size(), " artist", " artists");
    raw_label_colour(r, counts, 44, y + 300, 16, soft, 552);
    rect(r, 0, y, 640, 48, "background");
    status_bar(r, y);
    music_shoulders(r, y);
    media_hero_hints(r, y, {{"A", "Play"}, {"X", "Options"}});
}

// ---- Touch and keys

// The pills and Back in the header; -1 elsewhere. 60-62: a section, 70-74: a music tab.
int App::media_header_button(int x, int y) {
    using namespace media_layout;
    if (!media_sections()) return -1;
    const bool root = mu.section == 0 ? mu.levels.size() <= 1 : media_shelf().path.empty();
    if (!root || mu.view != MusicUi::View::Browse) return -1;
    if (y >= pills_y - 6 && y < pills_y + pills_h + 4 && x >= pills_left && x < pills_right) {
        const int n = int(media_section_list().size());
        return 60 + std::clamp((x - pills_left) * n / (pills_right - pills_left), 0, n - 1);
    }
    if (mu.section == 0 && y >= tabs_y - 2 && y < tabs_y + tabs_h + 4 && x >= tabs_left && x < tabs_right)
        return 70 +
               std::clamp((x - tabs_left) * music_layout::tabs / (tabs_right - tabs_left), 0, music_layout::tabs - 1);
    return -1;
}

// The keys of the Videos and Books pages; false leaves the key to the music player's own handling.
bool App::media_action(int code) {
    if (!media_shelf_open()) return false;
    switch (code) {
    case 8: media_move(0, -1); return true;
    case 9: media_move(0, 1); return true;
    case 0: media_move(-1, 0); return true;
    case 1: media_move(1, 0); return true;
    case 2:
        mu.focus_visible = true;
        media_activate(media_shelf().focus);
        return true;
    case 11:
        mu.focus_visible = true;
        media_options(media_shelf().focus);
        return true;
    case 4: media_resume_latest(); return true;
    case 13:
    case 14:
    case 15:
    case 16: media_step_section(code == 13 || code == 15 ? -1 : 1); return true;
    default: return false;
    }
}
