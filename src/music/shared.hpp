#pragma once
// What the music engine (rgds-music, src/music/engine.cpp) and the launcher share: the block of memory the engine
// publishes what it is doing in, and the names of the files beside it. The launcher only reads the block; everything
// it wants goes through the control socket as one line of text (see engine.cpp "Commands").
//
// The block is a file in the engine's run folder that both sides map. `now` is written under a sequence lock: the
// engine makes `sequence` odd, writes, makes it even; a reader copies and retries while the number moved or is odd.
// `viz` is written sixty times a second without a lock: a torn frame of a visualiser is one frame of a visualiser.
#include <atomic>
#include <cstdint>
#include <cstring>

namespace rgds::music {
inline constexpr uint32_t shared_magic = 0x554d4752; // "RGMU"
inline constexpr uint32_t shared_version = 3;
inline constexpr int eq_bands = 10;
inline constexpr int viz_bands = 64;
inline constexpr int viz_wave = 512;

enum class Status : uint32_t { Stopped, Playing, Paused };
enum class Route : uint32_t { Speaker, Bluetooth, Headphones };
enum class Repeat : uint32_t { Off, All, One };

struct Now {
    Status status = Status::Stopped;
    Route route = Route::Speaker;
    Repeat repeat = Repeat::Off;
    uint32_t queue_index = 0, queue_count = 0;
    // Counts every time the playing track changes (also to the same file again), so a reader knows to look.
    uint32_t track_serial = 0;
    // Counts every time the engine itself changed the queue's place or content (a headset's Next key, a track
    // that would not open), and every time it loaded a queue file.
    uint32_t queue_serial = 0;
    int64_t position_ms = 0, duration_ms = 0;
    uint32_t sample_rate = 0, channels = 0, bitrate = 0, bits = 0;
    char codec[24] = {};
    char path[1024] = {};
    // The equaliser as the engine applies it: per output, so the speakers and a headset each keep their own.
    uint32_t eq_on = 0;
    int32_t eq_preset = 0; // an index into eq.hpp's presets, -1 for the listener's own curve
    float eq_gain[eq_bands] = {};
    float eq_preamp = 0; // dB the engine takes off so the curve cannot clip
    // The library scan: `library_serial` moves when library.tsv has been rewritten.
    uint32_t library_serial = 0, scanning = 0, scan_done = 0, scan_total = 0;
    uint32_t error_serial = 0;
    char error[160] = {};
    // Milliseconds on the engine's own steady clock at the last write: a reader that sees it stand still for two
    // seconds knows the engine is gone.
    uint64_t heartbeat = 0;
};

struct Viz {
    std::atomic<uint32_t> serial{0};
    float bands[viz_bands] = {};     // 0-1, 40 Hz to 16 kHz in equal steps of pitch, already as loud as it sounds
    float wave[2][viz_wave] = {};    // the last 21 ms, left and right, -1 to 1
    float peak[2] = {}, rms[2] = {}; // 0-1 of full scale, over the same window
    float bass = 0;                  // 0-1, the lowest bands together
    float beat = 0;                  // 1 on a kick, falling to 0 over about 200 ms
};

struct Shared {
    uint32_t magic = shared_magic, version = shared_version;
    std::atomic<uint32_t> sequence{0};
    Now now;
    Viz viz;
};

// A consistent copy of `now`; false when the engine was writing through all of the tries.
inline bool read_now(const Shared& shared, Now& out) {
    for (int attempt = 0; attempt < 8; attempt++) {
        const uint32_t before = shared.sequence.load(std::memory_order_acquire);
        if (before & 1) continue;
        std::memcpy(static_cast<void*>(&out), static_cast<const void*>(&shared.now), sizeof(Now));
        if (shared.sequence.load(std::memory_order_acquire) == before) return true;
    }
    return false;
}

// Files in the run folder (--run): the mapped block, the socket commands arrive on, the queue the launcher wrote.
inline constexpr const char* shared_file = "state";
inline constexpr const char* control_socket = "control";
inline constexpr const char* queue_file = "queue.tsv";
// Files in the library folder (--library): the tracks, the pictures (art/<key>-<size>.bmp), what was playing, and
// the queue (kept here, not in the run folder, so it is still there after a restart).
inline constexpr const char* library_file = "library.tsv";
inline constexpr const char* session_file = "session.ini";
// A cover is kept at the three sizes the screens draw it, so none is ever scaled: a list row, the upper screen, the
// Now playing page.
inline constexpr int art_small = 44, art_medium = 88, art_large = 200;
} // namespace rgds::music
