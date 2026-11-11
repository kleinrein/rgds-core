// The music player's rules, without a window or an engine: the equaliser's arithmetic, what the analyser hears, how
// the library groups its tracks, the queue and its shuffle, the symbols and the visualisers' pictures.
#include "launcher/music_client.hpp"
#include "launcher/music_glyphs.hpp"
#include "launcher/music_viz.hpp"
#include "music/eq.hpp"
#include "music/spectrum.hpp"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>

using namespace rgds;
namespace fs = std::filesystem;

#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            std::cerr << __FILE__ << ':' << __LINE__ << ": " #condition << '\n';                                       \
            std::exit(1);                                                                                              \
        }                                                                                                              \
    } while (0)

static float rms_of_tone(music::Equaliser& eq, float frequency) {
    std::vector<float> block(48000 * 2);
    for (size_t i = 0; i < 48000; i++)
        block[i * 2] = block[i * 2 + 1] = .1f * std::sin(2 * 3.14159265f * frequency * float(i) / 48000);
    eq.process(block.data(), 48000);
    double sum = 0;
    for (size_t i = 24000; i < 48000; i++) sum += double(block[i * 2]) * block[i * 2]; // past the filters' start
    return float(std::sqrt(sum / 24000));
}

static void equaliser() {
    using namespace music;
    std::array<float, eq_bands> flat{}, bass{};
    bass[1] = 6; // 62.5 Hz
    CHECK(std::fabs(eq_response(flat, 1000)) < 1e-4f);
    CHECK(std::fabs(eq_response(bass, 62.5f) - 6) < .1f);
    CHECK(std::fabs(eq_response(bass, 8000)) < .2f);
    // The headroom is the curve's own peak, so a boosted band cannot push a full-scale track past full scale.
    CHECK(std::fabs(eq_headroom(bass) + 6) < .2f);
    CHECK(eq_headroom(flat) == 0);
    std::array<float, eq_bands> cut{};
    cut[5] = -9;
    CHECK(eq_headroom(cut) == 0); // cuts alone need none
    for (const EqPreset& preset : eq_presets)
        for (float gain : preset.gain) CHECK(std::fabs(gain) <= 6);
    CHECK(std::string(eq_presets[0].name) == "Flat");

    Equaliser eq;
    eq.configure(48000);
    eq.set(flat, true);
    const float plain = rms_of_tone(eq, 62.5f);
    CHECK(std::fabs(plain - .1f / std::sqrt(2.f)) < .002f);
    // With the 6 dB boost the tone at the band comes out as loud as it went in (boost minus headroom), and a tone
    // far from it 6 dB quieter.
    eq.set(bass, true);
    rms_of_tone(eq, 62.5f); // let the headroom gain arrive
    CHECK(std::fabs(20 * std::log10(rms_of_tone(eq, 62.5f) / plain)) < .5f);
    CHECK(std::fabs(20 * std::log10(rms_of_tone(eq, 8000) / plain) + 6) < .5f);
    // Off is exactly off.
    eq.set(bass, false);
    rms_of_tone(eq, 1000);
    CHECK(std::fabs(rms_of_tone(eq, 62.5f) - plain) < .001f);
    // The limiter holds an over-hot signal under full scale.
    std::vector<float> hot(4800 * 2, 1.6f);
    eq.set(flat, false);
    eq.process(hot.data(), 4800);
    for (float v : hot) CHECK(std::fabs(v) <= 1.0f);
}

static void analyser() {
    using namespace music;
    Analyser analyser(48000);
    Viz viz;
    std::vector<float> tone(size_t(Analyser::size) * 2);
    const auto loudest_band = [&](float frequency) {
        for (int i = 0; i < Analyser::size; i++)
            tone[size_t(i) * 2] = tone[size_t(i) * 2 + 1] =
                .5f * std::sin(2 * 3.14159265f * frequency * float(i) / 48000);
        Analyser fresh(48000);
        fresh.run(tone.data(), .016f, viz);
        int best = 0;
        for (int b = 1; b < viz_bands; b++)
            if (viz.bands[b] > viz.bands[best]) best = b;
        return best;
    };
    // 40 Hz to 16 kHz in equal steps of pitch: 1 kHz sits a little past the middle, and higher is further right.
    const int low = loudest_band(100), high = loudest_band(8000), mid = loudest_band(1000); // 1 kHz last: read below
    CHECK(low < mid && mid < high);
    CHECK(std::abs(mid - int(std::log(1000.0 / 40) / std::log(400.0) * viz_bands)) <= 1);
    CHECK(viz.peak[0] > .45f && viz.peak[0] <= .51f);
    CHECK(std::fabs(viz.rms[0] - .5f / std::sqrt(2.f)) < .02f);
    // Silence lets the bars fall instead of dropping them.
    const float before = viz.bands[high];
    analyser.rest(viz, .016f);
    CHECK(viz.bands[high] <= before);
}

static music::Track track(const std::string& path, const std::string& title, const std::string& artist,
                          const std::string& album, int number, int64_t mtime) {
    music::Track t;
    t.path = path;
    t.mtime = mtime;
    t.info.title = title;
    t.info.artist = artist;
    t.info.album = album;
    t.info.track = number;
    t.info.duration_ms = 1000;
    return t;
}

static void library(const fs::path& folder) {
    // The file format survives a round trip, tabs in a tag included.
    std::vector<music::Track> tracks = {
        track("/m/Night Drive/02 b.flac", "Beta", "The Testers", "Night Drive", 2, 20),
        track("/m/Night Drive/01 a.mp3", "Alpha\twith a tab", "The Testers", "Night Drive", 1, 10),
        track("/m/Mix/x.mp3", "Xylo", "Someone", "Mix Tape", 1, 30),
        track("/m/Mix/y.mp3", "the Yonder", "Someone Else", "Mix Tape", 2, 40),
        track("/m/Loose/untagged.wav", "", "", "", 0, 50),
    };
    std::sort(tracks.begin(), tracks.end(), [](const auto& a, const auto& b) { return a.path < b.path; });
    CHECK(music::write_library(folder / "library.tsv", tracks));
    MusicLibrary library;
    CHECK(library.load(folder / "library.tsv"));
    CHECK(library.tracks.size() == 5);
    const int alpha = library.by_path.at("/m/Night Drive/01 a.mp3");
    CHECK(library.title(alpha) == "Alpha with a tab");
    // Songs by title, "The" aside; a file without a title goes by its name.
    std::vector<std::string> titles;
    for (int song : library.songs) titles.push_back(library.title(song));
    CHECK((titles == std::vector<std::string>{"Alpha with a tab", "Beta", "untagged", "Xylo", "the Yonder"}));
    // An album's tracks by number; one folder with two artists and no album artist is one album by nobody.
    CHECK(library.albums.size() == 3);
    for (const MusicAlbum& album : library.albums) {
        if (album.name == "Night Drive") {
            CHECK(album.tracks.size() == 2 && library.title(album.tracks[0]) == "Alpha with a tab");
            CHECK(album.artist == "The Testers");
        } else if (album.name == "Mix Tape") {
            CHECK(album.tracks.size() == 2 && album.artist.empty());
        } else CHECK(album.name == "Loose"); // no album tag: the folder
    }
    CHECK(library.recent.front() == library.by_path.at("/m/Loose/untagged.wav"));
    // Folders first, then files.
    const auto root = library.folder("/m");
    CHECK(root.size() == 3 && root[0].folder && root[0].name == "Loose" && root[2].count == 2);
    CHECK(library.under("/m/Mix").size() == 2);
    CHECK(library.folder("/m/Night Drive").size() == 2 && !library.folder("/m/Night Drive")[0].folder);
    // A search needs every word, in the title, the artist or the album.
    CHECK(library.search("night beta").size() == 1);
    CHECK(library.search("someone").size() == 2);
    CHECK(library.search("nothing like this").empty());
}

static std::vector<std::string> names(const MusicClient& client) {
    std::vector<std::string> out;
    for (const std::string& path : client.queue) out.push_back(MusicLibrary::stem(path));
    return out;
}

static void queue(const fs::path& folder) {
    // No engine: commands wait, the queue file is still written.
    MusicClient client;
    client.configure(folder / "run", folder, {folder / "music"}, {});
    CHECK(client.library.tracks.size() == 5);
    std::vector<int> all;
    for (size_t i = 0; i < client.library.tracks.size(); i++) all.push_back(int(i));
    client.play(all, 2, 0);
    CHECK(client.queue.size() == 5);
    const std::vector<std::string> original = names(client);
    {
        std::ifstream file(folder / music::queue_file);
        std::string line;
        CHECK(std::getline(file, line) && line == client.queue[0] + "\t0");
    }
    // Shuffle keeps what plays (the engine is not there, so the place is 0) in front, keeps every track, and can
    // be undone exactly.
    client.set_shuffle(true, 0);
    CHECK(client.queue[0] == client.library.tracks[0].path);
    std::vector<std::string> sorted_now = names(client), sorted_then = original;
    std::sort(sorted_now.begin(), sorted_now.end());
    std::sort(sorted_then.begin(), sorted_then.end());
    CHECK(sorted_now == sorted_then);
    client.set_shuffle(false, 0);
    CHECK(names(client) == original);
    // Removing keeps the order of the rest, also once shuffle is undone.
    CHECK(client.remove(3, 0));
    CHECK(client.queue.size() == 4 && client.queue_origin == (std::vector<int>{0, 1, 2, 3}));
    CHECK(!client.remove(9, 0));
    // A second client reads the queue back as it was left.
    MusicClient again;
    again.configure(folder / "run", folder, {folder / "music"}, {});
    CHECK(again.queue == client.queue && again.queue_origin == client.queue_origin);
    // Favourites persist.
    client.toggle_favourite(client.library.tracks[1].path);
    MusicClient third;
    third.configure(folder / "run", folder, {folder / "music"}, {});
    CHECK(third.loved(client.library.tracks[1].path) && third.favourite_tracks().size() == 1);
    third.toggle_favourite(client.library.tracks[1].path);
    CHECK(third.favourite_tracks().empty());
}

static void pictures() {
    // Every symbol has ink in both styles and stays inside its box's margin.
    for (int g = 0; g < int(MusicGlyph::Count); g++) {
        const std::string pixels = music_glyph_pixels(MusicGlyph(g));
        CHECK(pixels.size() == 144);
        const long ink = std::count(pixels.begin(), pixels.end(), '#');
        CHECK(ink >= 12 && ink <= 110);
        const std::vector<uint8_t> smooth = music_glyph_coverage(MusicGlyph(g), 24);
        long covered = 0;
        for (uint8_t value : smooth) covered += value > 127;
        CHECK(covered >= 40 && covered <= 420);
        for (int i = 0; i < 24; i++)
            CHECK(smooth[size_t(i)] == 0 && smooth[size_t(23 * 24 + i)] == 0); // top and bottom rows
    }
    // Every visualiser draws something from sound, in both sizes, and nothing reads outside its picture.
    VizInput heard;
    for (int b = 0; b < music::viz_bands; b++) heard.bands[size_t(b)] = .2f + .7f * float(b % 7) / 7;
    for (int i = 0; i < music::viz_wave; i++)
        heard.wave[0][size_t(i)] = heard.wave[1][size_t(i)] = std::sin(float(i) * .08f) * .6f;
    heard.rms[0] = heard.rms[1] = .3f;
    heard.peak[0] = heard.peak[1] = .7f;
    heard.bass = .6f;
    heard.beat = 1;
    for (const auto& size : {std::pair<int, int>{292, 136}, {292, 196}}) {
        Visualiser picture;
        picture.resize(size.first, size.second);
        picture.tint(Color{35, 110, 174});
        for (int mode = 0; mode < int(VizMode::Count); mode++) {
            const std::vector<uint32_t>* pixels = nullptr;
            for (int frame = 0; frame < 30; frame++) pixels = &picture.draw(VizMode(mode), heard, 1 / 60.f);
            CHECK(pixels->size() == size_t(size.first) * size_t(size.second));
            long lit = 0;
            for (uint32_t pixel : *pixels) lit += pixel != picture.background();
            CHECK(lit > 400);
        }
    }
}

int main(int argc, char** argv) {
    CHECK(argc == 2);
    const fs::path folder = argv[1];
    equaliser();
    analyser();
    library(folder);
    queue(folder);
    pictures();
    std::cout << "music player: ok\n";
    return 0;
}
