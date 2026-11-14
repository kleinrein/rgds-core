#pragma once
// What the music player's screens remember between frames: the list being browsed, what a finger is doing, the
// visualiser. The screens themselves are members of App (music_screen.hpp).
#include "launcher/kinetics.hpp"
#include "launcher/music_client.hpp"
#include "launcher/music_glyphs.hpp"
#include "launcher/music_viz.hpp"

namespace rgds {
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
};
} // namespace rgds
