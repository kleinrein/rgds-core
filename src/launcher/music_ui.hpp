#pragma once
// What the music player's screens remember between frames: the list being browsed, what a finger is doing, the
// visualiser. The screens themselves are members of App (music_screen.hpp).
#include "launcher/kinetics.hpp"
#include "launcher/music_client.hpp"
#include "launcher/music_glyphs.hpp"
#include "launcher/music_viz.hpp"

namespace rgds {
// Videos and Books, the Media level's other two sections (media_shelf.hpp): one film, book or folder as
// rgds-media --index lists it.
struct MediaEntry {
    enum class Kind { Video, Book, Folder };
    Kind kind = Kind::Video;
    bool books = false; // on the Books shelf
    std::string parent, path, name, key, picture, blur, ext;
    double length = 0; // seconds, or pages
    int children = 0, shade = 150;
    uint64_t size = 0;
};

// Where a film or book was left (rgds-media's places.tsv).
struct MediaPlace {
    double position = 0, length = 0;
    int64_t when = 0;
    double fraction() const { return length > 0 ? std::clamp(position / length, 0.0, 1.0) : 0; }
    bool started() const { return position > .5; }
    bool finished() const { return length > 0 && position >= length * .97; }
};

// A value that follows its target like a soft spring: the pills and the focus corners.
// The lit pill gliding to a new choice: 160 ms ease-out, as a segmented control's (segment_position in main.cpp).
struct MediaGlide {
    double value = 0, target = 0, from = 0, aimed = 0, elapsed = 0;
    void snap(double v) { value = target = from = aimed = v, elapsed = 160; }
    bool step(double dt_ms);
};

struct MediaGrid {
    int cols, tile_w, tile_h, pic_w, pic_h, gap;
    int x0() const { return (640 - (cols * tile_w + (cols - 1) * gap)) / 2; }
};

// One shelf's page: the folders opened, what is on it, the focus and its corners.
struct MediaShelf {
    std::vector<std::string> path;
    std::vector<int> items; // into MusicUi::entries
    int focus = 0, scroll = 0;
    bool placed = false;
};

namespace media_layout {
// The lower screen's header: Back, then the section pills; Music's tabs as a second, smaller row.
constexpr int pills_y = 14, pills_h = 44, pills_left = 96, pills_right = 616, pills_gap = 6;
constexpr int tabs_y = 66, tabs_h = 34, tabs_left = 24, tabs_right = 616;
constexpr int rule_y = 70, music_rule_y = 108; // the header's rule: shelves, and Music with its tabs
constexpr int grid_top = 78, grid_bottom = 412;
// Films: three across, 16:9; books: four across, as tall as a paperback.
constexpr MediaGrid video_grid{3, 186, 150, 186, 105, 13}, book_grid{4, 134, 222, 134, 186, 14};
constexpr std::array<const char*, 3> section_names = {"Music", "Videos", "Books"};
} // namespace media_layout

struct MusicRow {
    enum class Kind { Track, Album, Artist, Folder, List, Shuffle };
    Kind kind = Kind::Track;
    int id = -1;    // the track, album or artist in the library; for a list, which one
    int number = 0; // a track on an album page shows its number where the cover would be
    std::string title, note, side, art, path;
};

// One page of the library: a tab, or what was opened from one.
struct MusicLevel {
    enum class Kind { Songs, Albums, Artists, Folders, Lists, Album, Artist, Folder, Favourites, Recent };
    Kind kind = Kind::Songs;
    std::string name, owner; // an album: its name and artist; an artist: the name; a folder: `path`
    std::string path, title;
    std::vector<MusicRow> rows;
    int focus = 0, scroll = 0;
    uint32_t revision = 0; // the library revision the rows were made from; 0: not made yet
};

struct MusicUi {
    enum class View { Browse, Player, Queue, Equaliser };
    View view = View::Browse;
    View came_from = View::Browse; // where Back from the queue or the equaliser returns to
    int tab = 0;
    std::vector<MusicLevel> levels; // levels[0] is the tab

    // The list on screen (the library or the queue) and its motion.
    ScrollMotion motion;
    int queue_focus = 0, queue_scroll = 0;
    uint32_t queue_place_seen = ~0u;

    // What the finger went down on.
    enum class Touch { None, List, Rail, Seek, Art, Band, Button };
    Touch touch = Touch::None;
    int touch_row = -1, touch_button = -1, scroll_at_press = 0, rail_grab = 0;
    bool swiped = false, held = false;

    // Now playing.
    bool seeking = false;
    int64_t seek_ms = 0;
    uint32_t seek_sent_at = 0;
    int art_shift = 0;
    uint32_t art_released_at = 0;
    int art_released_from = 0;

    // The equaliser page.
    int band = 0;

    bool focus_visible = false; // the last input was a button, so the focused row wears its ring
    bool bottom_changed = true; // the lower screen needs drawing (the visualiser alone only needs the upper one)

    // The visualiser: the picture, doubled and with rounded corners, as it is handed to the texture.
    Visualiser picture;
    VizInput heard;
    std::vector<uint32_t> frame;
    std::vector<uint8_t> mask;
    int frame_width = 0, frame_height = 0;
    uint32_t frame_at = 0, heard_serial = 0;

    std::string toast;
    uint32_t toast_until = 0;
    uint32_t error_seen = 0;
    int volume_sent = -1;
    bool was_alive = false, was_playing = false;
    // Playing under a closed lid: the panels are dark and input is ignored until it opens.
    bool lid_dark = false;
    uint32_t lid_quiet_at = 0;
    uint32_t polled_at = 0;

    // Media (media_shelf.hpp): 0 Music, 1 Videos, 2 Books, and what the two shelves show.
    int section = 0;
    MediaGlide section_pill, tab_pill; // the lit pill of each row, sliding
    std::array<MediaShelf, 2> shelves;
    std::vector<MediaEntry> entries;
    std::map<std::string, MediaPlace> places;
    int64_t index_seen = -2, places_seen = -2;
    uint32_t index_checked = 0, media_ticked = 0, hero_at = 0;
    std::string hero_key, hero_prev; // the item the upper screen shows, and the one fading out
    std::string music_hero_key;      // Music's first page: the row its upper screen shows
    uint32_t music_hero_at = 0;
};
} // namespace rgds
