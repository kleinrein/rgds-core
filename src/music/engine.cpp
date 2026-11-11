// rgds-music: the music player's engine. One small process that decodes (FFmpeg), shapes (equaliser, limiter) and
// plays (SDL audio, which is ALSA on the handheld and so follows /etc/asound.conf to a Bluetooth headset), keeps the
// library file current and tells the launcher what it is doing through a mapped block (music/shared.hpp). It is its
// own process so music carries on while the menu is busy or restarting, and so a damaged file can only take this
// down, never the menu.
//
//   rgds-music serve --run DIR --library DIR [--root DIR]...   the engine
//   rgds-music send --run DIR WORD...                          one command to a running engine
//   rgds-music status --run DIR                                what a running engine publishes, as text
//   rgds-music scan --library DIR [--root DIR]...              bring library.tsv up to date and leave
//   rgds-music probe FILE                                      what FFmpeg makes of one file
//
// Commands (one datagram of text each, on DIR/control):
//   load INDEX MS PLAY   read the queue file again, go to its track INDEX at MS; PLAY 1 starts, 0 waits paused
//   requeue INDEX        read the queue file again; the track now playing sits at INDEX and is not disturbed
//   play | pause | toggle | stop | next | prev | jump INDEX | seek MS | seekby MS
//   release              pause, and close the sound device as soon as the fade is done (a game is starting)
//   repeat 0|1|2         off, the whole queue, this track
//   eq ON PRESET G0..G9  the equaliser for the output in use now (PRESET -1: the listener's own curve)
//   volume PERCENT       the system volume level; the DAC follows it already, so only mute and the lowest steps act
//   rescan | quit
#include "music/eq.hpp"
#include "music/library.hpp"
#include "music/shared.hpp"
#include "music/spectrum.hpp"
#include <SDL.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <deque>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#ifdef __linux__
#include <linux/input.h>
#include <sys/ioctl.h>
#endif

namespace {
using namespace rgds::music;

uint64_t clock_ms() {
    return uint64_t(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

std::atomic<bool> stop_requested{false};
void on_signal(int) { stop_requested.store(true); }

struct Options {
    fs::path run, library;
    std::vector<fs::path> roots;
    // RG DS OS: bt-audio leaves the connected headset's address here while playback goes to it.
    fs::path bt_sink = "/run/rgds/bt-sink";
};

struct Chunk {
    std::vector<float> samples;
    size_t read = 0; // floats already played
    uint32_t token = 0;
    int64_t start_ms = 0;
};

struct Playing {
    int index = -1;
    std::string path;
    TrackInfo info;
};

struct EqSetting {
    bool on = false;
    int preset = 0;
    std::array<float, eq_bands> gain{};
};

class Engine {
    Options options;
    Shared* shared = nullptr;
    int control = -1, lock = -1;

    // ---- the queue: paths in play order, as the launcher wrote them
    std::mutex queue_mutex;
    std::vector<std::string> queue;

    // ---- decoded sound waiting for the output
    std::mutex pcm_mutex;
    std::deque<Chunk> chunks;
    size_t buffered = 0; // frames in `chunks` not yet played
    std::atomic<int> rate{48000};

    // ---- the decode thread and what it is asked to do
    std::thread decode_thread;
    std::mutex plan_mutex;
    std::condition_variable plan_wake;
    enum class Ask { None, Load, Seek, Drop };
    struct Request {
        Ask what = Ask::None;
        int index = 0;
        int64_t ms = 0;
    };
    std::deque<Request> asks;
    bool quit = false;
    Decoder decoder;
    int decode_index = -1; // the queue place being decoded; -1 with nothing open
    uint32_t decode_token = 0, next_token = 1;
    std::map<uint32_t, Playing> tokens;
    std::atomic<bool> ended{false};      // the queue ran out: once the buffer drains, playback stops
    std::atomic<bool> unplayable{false}; // the last load found nothing in the queue that would open
    std::atomic<Repeat> repeat{Repeat::Off};
    std::string pending_error; // set by the decode thread under plan_mutex
    uint32_t pending_error_serial = 0;

    // ---- the output
    SDL_AudioDeviceID device = 0;
    SDL_AudioSpec spec{};
    std::mutex dsp_mutex;
    Equaliser equaliser;
    std::atomic<float> gain_goal{0}, master{1};
    float gain = 0, tail[2] = {};
    std::atomic<bool> faded{true};
    std::atomic<uint32_t> audible_token{0};
    std::atomic<int64_t> audible_ms{0};
    std::atomic<bool> starved{false};
    // The last second and a bit of what went to the output, for the analyser.
    static constexpr size_t tap_size = 65536;
    std::vector<float> tap = std::vector<float>(tap_size * 2);
    std::atomic<uint64_t> tap_frames{0};

    // ---- what the main loop knows
    bool want_playing = false, device_paused = true, release_device = false;
    Playing current;
    uint32_t seen_token = 0;
    uint64_t paused_at = 0, open_retry_at = 0, open_since = 0, last_session = 0, last_route_check = 0, idle_since = 0,
             tap_seen_at = 0, tap_seen = 0;
    Route route = Route::Speaker;
    static constexpr float speaker_boost_db = 6;
    std::string bt_address;
    std::array<EqSetting, 3> eq{};
    Analyser analyser;
    std::vector<float> window = std::vector<float>(size_t(Analyser::size) * 2);
    uint64_t last_tick = 0;
    Now now;

    // ---- the library scan
    std::thread scan_thread;
    std::atomic<bool> scan_stop{false}, scan_running{false}, scan_again{false};
    std::atomic<uint32_t> scan_done{0}, scan_total{0}, library_serial{0};

    // ---- input devices: a headset's AVRCP keys, the headphone jack
    struct Input {
        int fd = -1;
        bool jack = false;
        std::string node;
    };
    std::vector<Input> inputs;
    uint64_t inputs_checked = 0;
    bool headphones = false;

    static void audio_callback(void* self, Uint8* data, int bytes) {
        static_cast<Engine*>(self)->render(reinterpret_cast<float*>(data), size_t(bytes) / sizeof(float) / 2);
    }

    // The audio thread. Takes what is buffered, shapes it, remembers it for the analyser, applies the fade and
    // the volume. No allocation: finished chunks are handed to `spent` and freed by the main loop.
    std::vector<Chunk> spent, spare; // `spent` under pcm_mutex
    void render(float* out, size_t frames) {
        size_t filled = 0;
        {
            std::lock_guard<std::mutex> hold(pcm_mutex);
            while (filled < frames && !chunks.empty()) {
                Chunk& c = chunks.front();
                const size_t available = (c.samples.size() - c.read) / 2, take = std::min(available, frames - filled);
                std::memcpy(out + filled * 2, c.samples.data() + c.read, take * 2 * sizeof(float));
                c.read += take * 2;
                filled += take;
                buffered -= take;
                audible_token.store(c.token, std::memory_order_relaxed);
                audible_ms.store(c.start_ms + int64_t(c.read / 2) * 1000 / rate.load(std::memory_order_relaxed),
                                 std::memory_order_relaxed);
                if (c.read >= c.samples.size()) {
                    if (spent.size() < spent.capacity()) spent.push_back(std::move(c));
                    chunks.pop_front();
                }
            }
            starved.store(chunks.empty(), std::memory_order_relaxed);
        }
        if (filled) {
            std::lock_guard<std::mutex> hold(dsp_mutex);
            equaliser.process(out, filled);
        }
        // Nothing to play (the decoder is behind, or a seek emptied the buffer): the last sample dies away over a
        // few milliseconds instead of stopping dead, which would click.
        if (filled) {
            tail[0] = out[(filled - 1) * 2];
            tail[1] = out[(filled - 1) * 2 + 1];
        }
        for (size_t i = filled; i < frames; i++) {
            tail[0] *= .97f;
            tail[1] *= .97f;
            out[i * 2] = tail[0];
            out[i * 2 + 1] = tail[1];
        }
        uint64_t at = tap_frames.load(std::memory_order_relaxed);
        for (size_t i = 0; i < frames; i++) {
            const size_t slot = size_t((at + i) % tap_size) * 2;
            tap[slot] = out[i * 2];
            tap[slot + 1] = out[i * 2 + 1];
        }
        tap_frames.store(at + frames, std::memory_order_release);
        const float goal = gain_goal.load(std::memory_order_relaxed), level = master.load(std::memory_order_relaxed);
        for (size_t i = 0; i < frames; i++) {
            gain += (goal - gain) * .0015f; // about 30 ms
            out[i * 2] *= gain * level;
            out[i * 2 + 1] *= gain * level;
        }
        if (std::fabs(goal - gain) < .002f) gain = goal;
        faded.store(gain == 0, std::memory_order_relaxed);
    }

    void set_error(const std::string& text) { // decode thread, plan_mutex held
        pending_error = text;
        pending_error_serial++;
    }

    std::string queue_at(int index, size_t* count = nullptr) {
        std::lock_guard<std::mutex> hold(queue_mutex);
        if (count) *count = queue.size();
        return index >= 0 && size_t(index) < queue.size() ? queue[size_t(index)] : std::string();
    }

    // Opens the first track that will open, starting at `index` and walking forward (round again when the whole
    // queue repeats). Returns its place, or -1 when nothing would.
    int open_from(int index) {
        size_t count = 0;
        queue_at(0, &count);
        for (size_t tried = 0; tried < count; tried++) {
            if (index >= int(count)) {
                if (repeat.load() != Repeat::All) return -1;
                index = 0;
            }
            const std::string path = queue_at(index);
            if (decoder.open(path, rate.load())) {
                Playing p;
                p.index = index;
                p.path = path;
                p.info = decoder.info;
                std::lock_guard<std::mutex> hold(plan_mutex);
                decode_token = next_token++;
                tokens[decode_token] = std::move(p);
                decode_index = index;
                return index;
            }
            {
                std::lock_guard<std::mutex> hold(plan_mutex);
                set_error(fs::path(path).filename().string() + ": " + decoder.error);
            }
            index++;
        }
        return -1;
    }

    void flush() {
        std::lock_guard<std::mutex> hold(pcm_mutex);
        chunks.clear();
        buffered = 0;
    }

    void decode_loop() {
        std::vector<float> block;
        for (;;) {
            Ask what;
            int index;
            int64_t ms;
            {
                std::unique_lock<std::mutex> hold(plan_mutex);
                plan_wake.wait_for(hold, std::chrono::milliseconds(20), [&] {
                    if (quit || !asks.empty()) return true;
                    if (!decoder.is_open()) return false;
                    std::lock_guard<std::mutex> pcm(pcm_mutex);
                    return buffered < size_t(rate.load());
                });
                if (quit) return;
                const Request next = asks.empty() ? Request{} : asks.front();
                if (!asks.empty()) asks.pop_front();
                what = next.what;
                index = next.index;
                ms = next.ms;
            }
            if (what == Ask::Load) {
                decoder.close();
                flush();
                ended.store(false);
                const int opened = open_from(index);
                if (opened < 0) {
                    std::lock_guard<std::mutex> hold(plan_mutex);
                    decode_index = -1;
                    unplayable.store(true);
                    ended.store(true);
                    continue;
                }
                if (ms > 0 && opened == index) decoder.seek(ms);
                // Nothing of the old track is left to play: what is heard next is this one.
                audible_token.store(decode_token);
                audible_ms.store(opened == index ? ms : 0);
                continue;
            }
            if (what == Ask::Seek) {
                // The track being heard may no longer be the one being decoded (the next one is read ahead for a
                // gapless change): go back to it first.
                const uint32_t heard = audible_token.load();
                int heard_index = -1;
                {
                    std::lock_guard<std::mutex> hold(plan_mutex);
                    const auto found = tokens.find(heard);
                    if (found != tokens.end()) heard_index = found->second.index;
                }
                if (heard_index < 0) continue;
                if (heard != decode_token || !decoder.is_open()) {
                    decoder.close();
                    flush();
                    if (open_from(heard_index) != heard_index) continue;
                    audible_token.store(decode_token);
                }
                if (decoder.seek(ms)) {
                    flush();
                    ended.store(false);
                    audible_ms.store(std::max<int64_t>(0, ms));
                }
                continue;
            }
            if (what == Ask::Drop) {
                // The queue changed around the playing track: whatever was read ahead of it belongs to the old
                // queue. `index` is where the playing track sits now.
                const uint32_t heard = audible_token.load();
                bool ahead;
                {
                    std::lock_guard<std::mutex> hold(plan_mutex);
                    for (auto& entry : tokens)
                        if (entry.first == heard) entry.second.index = index;
                    ahead = decode_token != heard;
                    if (ahead) {
                        tokens.erase(decode_token);
                        decode_token = heard;
                    }
                    decode_index = index;
                }
                if (ahead) {
                    decoder.close();
                    std::lock_guard<std::mutex> pcm(pcm_mutex);
                    while (!chunks.empty() && chunks.back().token != heard) {
                        buffered -= (chunks.back().samples.size() - chunks.back().read) / 2;
                        chunks.pop_back();
                    }
                }
                ended.store(false);
                if (!decoder.is_open()) follow(index);
                continue;
            }
            if (!decoder.is_open()) continue;
            block.clear();
            const int64_t start = decoder.position_ms();
            const bool more = decoder.read(block);
            if (!block.empty()) {
                Chunk c;
                c.token = decode_token;
                c.start_ms = start;
                c.samples = std::move(block);
                block = {};
                std::lock_guard<std::mutex> hold(pcm_mutex);
                buffered += c.samples.size() / 2;
                chunks.push_back(std::move(c));
            }
            if (!more) follow(decode_index);
        }
    }

    // The track at `index` has been read to its end: open what comes after it, so the change is gapless.
    void follow(int index) {
        decoder.close();
        const Repeat mode = repeat.load();
        size_t count = 0;
        queue_at(0, &count);
        int next = mode == Repeat::One ? index : index + 1;
        if (next >= int(count) && mode != Repeat::All) next = -1;
        if (next < 0 || open_from(next) < 0) {
            std::lock_guard<std::mutex> hold(plan_mutex);
            decode_index = -1;
            ended.store(true);
        }
    }

    void request(Ask what, int index = 0, int64_t ms = 0) {
        {
            std::lock_guard<std::mutex> hold(plan_mutex);
            // A load makes everything asked before it moot; a scrub's seeks collapse into the last one.
            if (what == Ask::Load) asks.clear();
            else if (what == Ask::Seek && !asks.empty() && asks.back().what == Ask::Seek) asks.pop_back();
            asks.push_back({what, index, ms});
        }
        if (what == Ask::Load) unplayable.store(false);
        plan_wake.notify_all();
    }

    // ---- the output device
    void close_device() {
        if (!device) return;
        SDL_CloseAudioDevice(device);
        device = 0;
        device_paused = true;
        faded.store(true);
        gain = 0;
    }
    bool open_device() {
        SDL_AudioSpec want{};
        want.freq = 48000;
        want.format = AUDIO_F32SYS;
        want.channels = 2;
        want.samples = 1024;
        want.callback = audio_callback;
        want.userdata = this;
        device = SDL_OpenAudioDevice(nullptr, 0, &want, &spec, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
        if (!device) return false;
        device_paused = true;
        gain = 0;
        tail[0] = tail[1] = 0;
        if (spec.freq != rate.load()) {
            // The output wants another rate than the buffer holds: decode again from where we are, at its rate.
            rate.store(spec.freq);
            {
                std::lock_guard<std::mutex> hold(dsp_mutex);
                equaliser.configure(float(spec.freq));
            }
            analyser.configure(float(spec.freq));
            if (current.index >= 0) request(Ask::Load, current.index, audible_ms.load());
        }
        return true;
    }

    void apply_eq() {
        const EqSetting& e = eq[size_t(route)];
        std::lock_guard<std::mutex> hold(dsp_mutex);
        equaliser.set(e.gain, e.on);
        // The internal speakers are quiet next to a game's sound: music gets make-up gain there (the limiter holds
        // the peaks). Headphones and Bluetooth play the file as it is.
        equaliser.set_boost(route == Route::Speaker ? speaker_boost_db : 0);
    }
    void load_eq() {
        std::ifstream in(options.library / "eq.ini");
        std::string line;
        static const char* names[3] = {"speaker", "bluetooth", "headphones"};
        while (std::getline(in, line)) {
            const size_t equals = line.find('=');
            if (equals == std::string::npos) continue;
            for (int r = 0; r < 3; r++) {
                if (line.compare(0, equals, names[r]) != 0) continue;
                std::istringstream cells(line.substr(equals + 1));
                int on = 0;
                EqSetting e;
                cells >> on >> e.preset;
                for (float& g : e.gain) cells >> g;
                if (!cells.fail()) {
                    e.on = on != 0;
                    for (float& g : e.gain) g = std::clamp(g, -eq_range, eq_range);
                    eq[size_t(r)] = e;
                }
            }
        }
    }
    void save_eq() {
        static const char* names[3] = {"speaker", "bluetooth", "headphones"};
        std::ofstream out(options.library / "eq.ini", std::ios::trunc);
        for (int r = 0; r < 3; r++) {
            out << names[r] << '=' << int(eq[size_t(r)].on) << ' ' << eq[size_t(r)].preset;
            for (float g : eq[size_t(r)].gain) out << ' ' << g;
            out << '\n';
        }
    }

    // ---- the session: where the listener was, for the next start
    void save_session() { save_session(std::max(0, current.index), published_position(), current.path); }
    void save_session(int index, int64_t position, const std::string& path) {
        last_session = clock_ms();
        const fs::path file = options.library / session_file, temporary = file.string() + ".part";
        {
            std::ofstream out(temporary, std::ios::trunc);
            out << "index=" << index << "\nposition_ms=" << position << "\nrepeat=" << int(repeat.load())
                << "\npath=" << path << '\n';
        }
        std::error_code ec;
        fs::rename(temporary, file, ec);
    }
    bool read_queue() {
        std::vector<std::string> paths;
        std::ifstream in(options.library / queue_file);
        std::string line;
        while (std::getline(in, line)) {
            const size_t tab = line.find('\t');
            if (tab != std::string::npos) line.resize(tab);
            if (!line.empty()) paths.push_back(line);
        }
        std::lock_guard<std::mutex> hold(queue_mutex);
        queue = std::move(paths);
        return !queue.empty();
    }
    void restore_session() {
        if (!read_queue()) return;
        std::ifstream in(options.library / session_file);
        std::string line, path;
        int index = 0, mode = 0;
        int64_t position = 0;
        while (std::getline(in, line)) {
            if (line.rfind("index=", 0) == 0) index = std::atoi(line.c_str() + 6);
            else if (line.rfind("position_ms=", 0) == 0) position = std::atoll(line.c_str() + 12);
            else if (line.rfind("repeat=", 0) == 0) mode = std::atoi(line.c_str() + 7);
            else if (line.rfind("path=", 0) == 0) path = line.substr(5);
        }
        repeat.store(Repeat(std::clamp(mode, 0, 2)));
        if (queue_at(index) != path) index = 0, position = 0; // the queue file is newer than the session
        request(Ask::Load, index, position);
        now.queue_serial++;
    }

    int64_t published_position() const { return std::max<int64_t>(0, audible_ms.load()); }

    // ---- commands
    void play() {
        size_t count = 0;
        queue_at(0, &count);
        if (!count) return;
        if (ended.load()) {
            bool empty;
            {
                std::lock_guard<std::mutex> hold(pcm_mutex);
                empty = chunks.empty();
            }
            if (empty) request(Ask::Load, std::max(0, current.index), 0);
        }
        want_playing = true;
        release_device = false;
        open_retry_at = 0;
        open_since = clock_ms();
    }
    void pause() {
        if (!want_playing) return;
        want_playing = false;
        save_session();
    }
    void jump(int index, int64_t ms = 0) {
        size_t count = 0;
        queue_at(0, &count);
        if (!count) return;
        request(Ask::Load, std::clamp(index, 0, int(count) - 1), ms);
    }
    void step(int direction) {
        size_t count = 0;
        queue_at(0, &count);
        if (!count) return;
        int index = std::max(0, current.index);
        if (direction < 0 && published_position() > 3000) return request(Ask::Seek, 0, 0);
        index += direction;
        if (index >= int(count)) {
            if (repeat.load() != Repeat::All) return;
            index = 0;
        }
        if (index < 0) index = repeat.load() == Repeat::All ? int(count) - 1 : 0;
        request(Ask::Load, index, 0);
        now.queue_serial++;
    }
    void command(const std::string& line) {
        std::istringstream words(line);
        std::string verb;
        words >> verb;
        idle_since = clock_ms();
        if (verb == "load") {
            int index = 0, start = 1;
            int64_t ms = 0;
            words >> index >> ms >> start;
            if (!read_queue()) {
                stop();
                return;
            }
            jump(index, ms);
            now.queue_serial++;
            if (start) play();
        } else if (verb == "requeue") {
            int index = 0;
            words >> index;
            if (!read_queue()) {
                stop();
                return;
            }
            request(Ask::Drop, index);
            current.index = index;
            now.queue_serial++;
        } else if (verb == "play") play();
        else if (verb == "pause") pause();
        else if (verb == "release") {
            // Pause, and give the sound device up as soon as the fade is done: a game is about to want it.
            pause();
            release_device = true;
        } else if (verb == "toggle") want_playing ? pause() : play();
        else if (verb == "stop") stop();
        else if (verb == "next") step(1);
        else if (verb == "prev") step(-1);
        else if (verb == "jump") {
            int index = 0;
            words >> index;
            jump(index);
        } else if (verb == "seek" || verb == "seekby") {
            int64_t ms = 0;
            words >> ms;
            if (verb == "seekby") ms += published_position();
            if (current.info.duration_ms > 0) ms = std::min(ms, std::max<int64_t>(0, current.info.duration_ms - 500));
            request(Ask::Seek, 0, std::max<int64_t>(0, ms));
        } else if (verb == "repeat") {
            int mode = 0;
            words >> mode;
            const Repeat before = repeat.exchange(Repeat(std::clamp(mode, 0, 2)));
            // What was read ahead followed the old rule (the same track again, or nothing after the last).
            if (before != repeat.load() && current.index >= 0) request(Ask::Drop, current.index);
        } else if (verb == "eq") {
            int on = 0;
            EqSetting e;
            words >> on >> e.preset;
            for (float& g : e.gain) words >> g;
            if (words.fail()) return;
            e.on = on != 0;
            for (float& g : e.gain) g = std::clamp(g, -eq_range, eq_range);
            eq[size_t(route)] = e;
            apply_eq();
            save_eq();
        } else if (verb == "volume") {
            int level = 100;
            words >> level;
            level = std::clamp(level, 0, 100);
            // The system volume already sets the DAC (runtime/audio.py), so the music plays at full scale on top
            // of it. A second curve here, as the menu's sounds have, left songs far quieter than games. Only the
            // lowest steps taper a little further, down to -9 dB at 1%, where the DAC's floor is still loud.
            master.store(level == 0 ? 0.f
                                    : level >= 25 ? 1.f
                                                  : std::pow(10.f, -float(25 - level) / 24 * 9 / 20));
        } else if (verb == "rescan") start_scan();
        else if (verb == "quit") stop_requested.store(true);
    }
    void stop() {
        want_playing = false;
        request(Ask::Load, 0, 0);
        save_session();
    }

    // ---- the library
    void start_scan() {
        if (scan_running.load()) {
            scan_again.store(true);
            return;
        }
        if (scan_thread.joinable()) scan_thread.join();
        scan_stop.store(false);
        scan_running.store(true);
        scan_thread = std::thread([this] {
            do {
                scan_again.store(false);
                scan_done.store(0);
                scan_total.store(0);
                Scanner scanner(options.library);
                const fs::path file = options.library / library_file;
                const std::vector<Track> known = read_library(file);
                bool complete = true;
                const std::vector<Track> tracks = scanner.scan(
                    options.roots, known,
                    [&](size_t done, size_t total) {
                        scan_done.store(uint32_t(done));
                        scan_total.store(uint32_t(total));
                        if (scan_stop.load()) complete = false;
                        return complete;
                    },
                    [&](const std::vector<Track>& partial) {
                        // Only a first scan shows its catch as it goes: later ones would make known tracks vanish
                        // until the walk reaches them again.
                        if (known.empty() && write_library(file, partial)) library_serial.fetch_add(1);
                    });
                if (!complete) break;
                bool changed = tracks.size() != known.size();
                for (size_t i = 0; !changed && i < tracks.size(); i++)
                    changed = tracks[i].path != known[i].path || tracks[i].mtime != known[i].mtime ||
                              tracks[i].size != known[i].size;
                std::error_code ec;
                if ((changed || !fs::exists(file, ec)) && write_library(file, tracks)) library_serial.fetch_add(1);
                scan_done.store(uint32_t(tracks.size()));
            } while (scan_again.load() && !scan_stop.load());
            scan_running.store(false);
        });
    }

    // ---- input devices (Linux): media keys from a headset, the headphone jack
    void scan_inputs() {
#ifdef __linux__
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator("/dev/input", ec)) {
            const std::string node = entry.path().string();
            if (entry.path().filename().string().rfind("event", 0) != 0) continue;
            bool known = false;
            for (const Input& i : inputs) known = known || i.node == node;
            if (known) continue;
            const int fd = ::open(node.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) continue;
            unsigned long keys[(KEY_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = {};
            unsigned long switches[(SW_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = {};
            const auto has = [](const unsigned long* bits, int code) {
                return (bits[size_t(code) / (8 * sizeof(long))] >> (size_t(code) % (8 * sizeof(long)))) & 1;
            };
            ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys);
            ioctl(fd, EVIOCGBIT(EV_SW, sizeof switches), switches);
            Input input;
            input.fd = fd;
            input.node = node;
            if (has(switches, SW_HEADPHONE_INSERT)) {
                input.jack = true;
                unsigned long state[(SW_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = {};
                if (ioctl(fd, EVIOCGSW(sizeof state), state) >= 0) headphones = has(state, SW_HEADPHONE_INSERT);
            } else if (!has(keys, KEY_NEXTSONG) && !has(keys, KEY_PLAYPAUSE) && !has(keys, KEY_PLAYCD)) {
                ::close(fd);
                input.fd = -1; // remembered, so it is not opened again every two seconds
            }
            inputs.push_back(input);
        }
#endif
    }
    void read_input(Input& input) {
#ifdef __linux__
        struct input_event events[16];
        for (;;) {
            const ssize_t got = ::read(input.fd, events, sizeof events);
            if (got < ssize_t(sizeof events[0])) {
                if (got < 0 && errno == EAGAIN) return;
                ::close(input.fd); // unplugged, or the headset left
                input.fd = -1;
                input.node.clear();
                return;
            }
            for (size_t i = 0; i < size_t(got) / sizeof events[0]; i++) {
                const auto& e = events[i];
                if (e.type == EV_SW && e.code == SW_HEADPHONE_INSERT && input.jack) {
                    const bool in = e.value != 0;
                    if (headphones && !in && route == Route::Headphones) pause(); // pulled out: never out loud
                    headphones = in;
                    last_route_check = 0;
                } else if (e.type == EV_KEY && e.value == 1) {
                    idle_since = clock_ms();
                    switch (e.code) {
                    case KEY_PLAYPAUSE: want_playing ? pause() : play(); break;
                    case KEY_PLAYCD: play(); break;
                    case KEY_PAUSECD:
                    case KEY_STOPCD: pause(); break;
                    case KEY_NEXTSONG: step(1); break;
                    case KEY_PREVIOUSSONG: step(-1); break;
                    case KEY_FASTFORWARD: command("seekby 10000"); break;
                    case KEY_REWIND: command("seekby -10000"); break;
                    default: break;
                    }
                }
            }
        }
#else
        (void)input;
#endif
    }

    // Where the sound goes: a headset bt-audio switched playback to, the jack, or the speakers.
    void check_route() {
        last_route_check = clock_ms();
        std::string address;
        {
            std::ifstream in(options.bt_sink);
            std::getline(in, address);
        }
        const Route before = route;
        const std::string before_address = bt_address;
        bt_address = address;
        route = !address.empty() ? Route::Bluetooth : headphones ? Route::Headphones : Route::Speaker;
        if (route == before && address == before_address) return;
        if (before == Route::Bluetooth && route != Route::Bluetooth) {
            // The headset went (switched off, out of range): stop here instead of carrying on out of the speakers.
            pause();
            close_device();
        } else if (route == Route::Bluetooth) {
            // ALSA resolves where "default" goes when a stream opens: this one still points at the speakers.
            close_device();
            open_since = clock_ms();
        }
        apply_eq();
    }

    void tick() {
        const uint64_t at = clock_ms();
        const float seconds = last_tick ? std::min(.1f, float(at - last_tick) / 1000) : .016f;
        last_tick = at;
        {
            std::lock_guard<std::mutex> hold(pcm_mutex);
            spent.swap(spare);
        }
        spare.clear(); // frees the played chunks here, off the audio thread
        spare.reserve(64);
        if (at - last_route_check >= 500) check_route();
        if (at - inputs_checked >= 2000) {
            inputs_checked = at;
            inputs.erase(std::remove_if(inputs.begin(), inputs.end(), [](const Input& i) { return i.node.empty(); }),
                         inputs.end());
            scan_inputs();
        }
        // What is heard now.
        const uint32_t token = audible_token.load();
        if (token != seen_token) {
            std::lock_guard<std::mutex> hold(plan_mutex);
            const auto found = tokens.find(token);
            if (found != tokens.end()) {
                current = found->second;
                seen_token = token;
                now.track_serial++;
                for (auto it = tokens.begin(); it != tokens.end();)
                    it = it->first < token && it->first != decode_token ? tokens.erase(it) : std::next(it);
            }
        }
        bool empty;
        {
            std::lock_guard<std::mutex> hold(pcm_mutex);
            empty = chunks.empty();
        }
        if (unplayable.load()) {
            // Nothing in the queue would open (the card was pulled, the files are gone): say so by stopping.
            want_playing = false;
            current = {};
        } else if (ended.load() && empty && want_playing && seen_token == token) {
            // The queue played out: back to its first track, waiting.
            want_playing = false;
            if (device && !device_paused) {
                // Silence is all that is coming out: stop the stream now, so none of the first track is heard.
                SDL_PauseAudioDevice(device, 1);
                device_paused = true;
                paused_at = at;
                SDL_LockAudioDevice(device);
                gain = 0;
                SDL_UnlockAudioDevice(device);
                faded.store(true);
                gain_goal.store(0);
            }
            request(Ask::Load, 0, 0);
            now.queue_serial++;
            save_session(0, 0, queue_at(0));
        }
        // The device follows what is wanted.
        if (want_playing) {
            if (!device && at >= open_retry_at) {
                if (!open_device()) {
                    // A headset's stream can take a moment to exist after it connects, and the menu may still be
                    // letting go of its own sink.
                    open_retry_at = at + 300;
                    if (at - open_since > 6000) {
                        std::lock_guard<std::mutex> hold(plan_mutex);
                        set_error(std::string("No sound output: ") + SDL_GetError());
                        want_playing = false;
                    }
                }
            }
            if (device) {
                gain_goal.store(1);
                if (device_paused) {
                    SDL_PauseAudioDevice(device, 0);
                    device_paused = false;
                    tap_seen = tap_frames.load();
                    tap_seen_at = at;
                }
                // A stream that stopped taking sound (the headset vanished before bt-audio noticed) is reopened.
                const uint64_t frames = tap_frames.load();
                if (frames != tap_seen) {
                    tap_seen = frames;
                    tap_seen_at = at;
                } else if (at - tap_seen_at > 3000) {
                    close_device();
                    open_since = at;
                }
            }
            paused_at = 0;
        } else if (device) {
            gain_goal.store(0);
            if (!device_paused && faded.load()) {
                SDL_PauseAudioDevice(device, 1);
                device_paused = true;
                paused_at = at;
            }
            // A paused stream still holds the DAC and the amplifiers on (and a headset awake): let go after a while.
            if (device_paused && (release_device || (paused_at && at - paused_at > 3000))) close_device();
        }
        // The analyser reads what is coming out of the speakers now, not what was just handed to the device: a
        // Bluetooth link holds about a fifth of a second.
        const bool sounding = device && !device_paused;
        if (sounding) {
            const uint64_t frames = tap_frames.load(std::memory_order_acquire);
            const uint64_t delay =
                uint64_t(spec.samples) * 2 + (route == Route::Bluetooth ? uint64_t(rate.load()) / 5 : 0);
            if (frames >= delay + uint64_t(Analyser::size)) {
                const uint64_t from = frames - delay - uint64_t(Analyser::size);
                for (int i = 0; i < Analyser::size; i++) {
                    const size_t slot = size_t((from + uint64_t(i)) % tap_size) * 2;
                    window[size_t(i) * 2] = tap[slot];
                    window[size_t(i) * 2 + 1] = tap[slot + 1];
                }
                analyser.run(window.data(), seconds, shared->viz);
            }
        } else analyser.rest(shared->viz, seconds);
        if (want_playing && at - last_session > 15000) save_session();
        publish(at);
    }

    void publish(uint64_t at) {
        size_t count = 0;
        queue_at(0, &count);
        now.status = want_playing ? Status::Playing : current.index >= 0 ? Status::Paused : Status::Stopped;
        now.route = route;
        now.repeat = repeat.load();
        now.queue_index = uint32_t(std::max(0, current.index));
        now.queue_count = uint32_t(count);
        now.position_ms = published_position();
        now.duration_ms = current.info.duration_ms;
        if (now.duration_ms > 0) now.position_ms = std::min(now.position_ms, now.duration_ms);
        now.sample_rate = uint32_t(current.info.sample_rate);
        now.channels = uint32_t(current.info.channels);
        now.bitrate = uint32_t(current.info.bitrate);
        now.bits = uint32_t(current.info.bits);
        std::snprintf(now.codec, sizeof now.codec, "%s", current.info.codec.c_str());
        std::snprintf(now.path, sizeof now.path, "%s", current.path.c_str());
        const EqSetting& e = eq[size_t(route)];
        now.eq_on = e.on;
        now.eq_preset = e.preset;
        for (int i = 0; i < eq_bands; i++) now.eq_gain[i] = e.gain[size_t(i)];
        {
            std::lock_guard<std::mutex> hold(dsp_mutex);
            now.eq_preamp = equaliser.headroom_db();
        }
        now.library_serial = library_serial.load();
        now.scanning = scan_running.load();
        now.scan_done = scan_done.load();
        now.scan_total = scan_total.load();
        {
            std::lock_guard<std::mutex> hold(plan_mutex);
            if (pending_error_serial != now.error_serial) {
                now.error_serial = pending_error_serial;
                std::snprintf(now.error, sizeof now.error, "%s", pending_error.c_str());
            }
        }
        now.heartbeat = at;
        shared->sequence.fetch_add(1, std::memory_order_acq_rel);
        std::memcpy(static_cast<void*>(&shared->now), &now, sizeof now);
        shared->sequence.fetch_add(1, std::memory_order_acq_rel);
    }

public:
    explicit Engine(Options o) : options(std::move(o)) {}

    int serve() {
        std::error_code ec;
        fs::create_directories(options.run, ec);
        fs::create_directories(options.library / "art", ec);
        // One engine per run folder.
        lock = ::open((options.run / "lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
        if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) != 0) {
            std::fprintf(stderr, "rgds-music: already running in %s\n", options.run.c_str());
            return 0;
        }
        const fs::path block = options.run / shared_file;
        const int fd = ::open(block.c_str(), O_CREAT | O_RDWR | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0 || ftruncate(fd, sizeof(Shared)) != 0) {
            std::perror("rgds-music: state");
            return 1;
        }
        void* memory = mmap(nullptr, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ::close(fd);
        if (memory == MAP_FAILED) {
            std::perror("rgds-music: mmap");
            return 1;
        }
        shared = new (memory) Shared();

        const fs::path address = options.run / control_socket;
        sockaddr_un name{};
        name.sun_family = AF_UNIX;
        if (address.string().size() >= sizeof name.sun_path) {
            std::fprintf(stderr, "rgds-music: %s is too long for a socket\n", address.c_str());
            return 1;
        }
        std::snprintf(name.sun_path, sizeof name.sun_path, "%s", address.c_str());
        ::unlink(address.c_str());
        control = socket(AF_UNIX, SOCK_DGRAM, 0);
        if (control < 0 || bind(control, reinterpret_cast<sockaddr*>(&name), sizeof name) != 0) {
            std::perror("rgds-music: control socket");
            return 1;
        }
        fcntl(control, F_SETFL, O_NONBLOCK);
        fcntl(control, F_SETFD, FD_CLOEXEC);

        if (SDL_Init(SDL_INIT_AUDIO) != 0) {
            std::fprintf(stderr, "rgds-music: %s\n", SDL_GetError());
            return 1;
        }
        av_log_set_level(AV_LOG_FATAL);
        load_eq();
        scan_inputs();
        check_route();
        std::error_code exists;
        if (fs::exists(options.library / library_file, exists)) library_serial.store(1);
        decode_thread = std::thread([this] { decode_loop(); });
        restore_session();
        start_scan();
        idle_since = clock_ms();

        char text[4096];
        std::vector<pollfd> fds;
        while (!stop_requested.load()) {
            fds.clear();
            fds.push_back({control, POLLIN, 0});
            for (const Input& i : inputs)
                if (i.fd >= 0) fds.push_back({i.fd, POLLIN, 0});
            // Sixty times a second while there is something to show; a slow heartbeat otherwise.
            poll(fds.data(), nfds_t(fds.size()), device && !device_paused ? 16 : 100);
            for (;;) {
                const ssize_t got = recv(control, text, sizeof text - 1, 0);
                if (got <= 0) break;
                text[got] = 0;
                command(text);
            }
            for (Input& i : inputs)
                if (i.fd >= 0) read_input(i);
            tick();
            // Nothing playing and nobody asking for half an hour: leave. The session is saved; the launcher
            // starts a new engine when it is wanted.
            if (want_playing || scan_running.load()) idle_since = clock_ms();
            else if (clock_ms() - idle_since > 30 * 60 * 1000) break;
        }
        want_playing = false;
        save_session();
        // Fade out before the device goes.
        if (device && !device_paused) {
            gain_goal.store(0);
            for (int i = 0; i < 20 && !faded.load(); i++) SDL_Delay(5);
        }
        close_device();
        {
            std::lock_guard<std::mutex> hold(plan_mutex);
            quit = true;
        }
        plan_wake.notify_all();
        decode_thread.join();
        scan_stop.store(true);
        if (scan_thread.joinable()) scan_thread.join();
        now.status = Status::Stopped;
        now.heartbeat = 0;
        publish(0);
        ::unlink(address.c_str());
        SDL_Quit();
        return 0;
    }
};

int send(const fs::path& run, const std::string& text) {
    sockaddr_un name{};
    name.sun_family = AF_UNIX;
    std::snprintf(name.sun_path, sizeof name.sun_path, "%s", (run / control_socket).c_str());
    const int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) return 1;
    const bool ok = sendto(fd, text.data(), text.size(), 0, reinterpret_cast<sockaddr*>(&name), sizeof name) ==
                    ssize_t(text.size());
    ::close(fd);
    if (!ok) std::perror("rgds-music: send");
    return ok ? 0 : 1;
}

int usage() {
    std::fprintf(stderr, "usage: rgds-music serve --run DIR --library DIR [--root DIR]... [--bt-sink FILE]\n"
                         "       rgds-music send --run DIR WORD...\n"
                         "       rgds-music status --run DIR\n"
                         "       rgds-music scan --library DIR [--root DIR]...\n"
                         "       rgds-music probe FILE\n");
    return 2;
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    const std::string mode = argv[1];
    Options options;
    std::vector<std::string> rest;
    for (int i = 2; i < argc; i++) {
        const std::string arg = argv[i];
        const auto value = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (arg == "--run") options.run = value();
        else if (arg == "--library") options.library = value();
        else if (arg == "--root") options.roots.emplace_back(value());
        else if (arg == "--bt-sink") options.bt_sink = value();
        else rest.push_back(arg);
    }
    if (mode == "probe") {
        if (rest.empty()) return usage();
        av_log_set_level(AV_LOG_FATAL);
        Decoder file;
        if (!file.open(rest[0], 48000)) {
            std::fprintf(stderr, "%s: %s\n", rest[0].c_str(), file.error.c_str());
            return 1;
        }
        const TrackInfo& i = file.info;
        std::printf("title=%s\nartist=%s\nalbum=%s\nalbum_artist=%s\ngenre=%s\ntrack=%d\ndisc=%d\nyear=%d\n"
                    "duration_ms=%lld\ncodec=%s\nsample_rate=%d\nchannels=%d\nbitrate=%d\nbits=%d\npicture=%d\n",
                    i.title.c_str(), i.artist.c_str(), i.album.c_str(), i.album_artist.c_str(), i.genre.c_str(),
                    i.track, i.disc, i.year, static_cast<long long>(i.duration_ms), i.codec.c_str(), i.sample_rate,
                    i.channels, i.bitrate, i.bits, file.picture() ? 1 : 0);
        // Decoded through to the end, so a test can tell the whole file plays.
        std::vector<float> block;
        size_t frames = 0;
        double energy = 0;
        bool more = true;
        while (more) {
            block.clear();
            more = file.read(block);
            frames += block.size() / 2;
            for (float v : block) energy += double(v) * v;
        }
        std::printf("decoded_ms=%lld\nrms=%.5f\n", static_cast<long long>(frames * 1000 / 48000),
                    frames ? std::sqrt(energy / double(frames * 2)) : 0.0);
        return 0;
    }
    if (mode == "status") {
        // What a running engine publishes, as text: for a shell on the handheld, and for the tests.
        if (options.run.empty()) return usage();
        const int fd = ::open((options.run / shared_file).c_str(), O_RDONLY);
        void* memory = fd < 0 ? MAP_FAILED : mmap(nullptr, sizeof(Shared), PROT_READ, MAP_SHARED, fd, 0);
        if (memory == MAP_FAILED) {
            std::fprintf(stderr, "rgds-music: no engine state in %s\n", options.run.c_str());
            return 1;
        }
        const Shared* shared = static_cast<const Shared*>(memory);
        Now now;
        if (shared->magic != shared_magic || shared->version != shared_version || !read_now(*shared, now)) return 1;
        static const char* states[] = {"stopped", "playing", "paused"};
        static const char* routes[] = {"speaker", "bluetooth", "headphones"};
        float loudest = 0;
        for (float band : shared->viz.bands) loudest = std::max(loudest, band);
        std::printf("status=%s\nroute=%s\nrepeat=%u\nindex=%u\ncount=%u\ntrack_serial=%u\nqueue_serial=%u\n"
                    "position_ms=%lld\nduration_ms=%lld\ncodec=%s\nsample_rate=%u\nbitrate=%u\nbits=%u\npath=%s\n"
                    "eq_on=%u\neq_preset=%d\neq_preamp=%.2f\nlibrary_serial=%u\nscanning=%u\nscan=%u/%u\n"
                    "error=%s\nheartbeat=%llu\nviz_serial=%u\nviz_peak=%.3f\nviz_band=%.3f\n",
                    states[size_t(now.status) % 3], routes[size_t(now.route) % 3], unsigned(now.repeat),
                    now.queue_index, now.queue_count, now.track_serial, now.queue_serial,
                    static_cast<long long>(now.position_ms), static_cast<long long>(now.duration_ms), now.codec,
                    now.sample_rate, now.bitrate, now.bits, now.path, now.eq_on, now.eq_preset, double(now.eq_preamp),
                    now.library_serial, now.scanning, now.scan_done, now.scan_total, now.error,
                    static_cast<unsigned long long>(now.heartbeat), shared->viz.serial.load(),
                    double(std::max(shared->viz.peak[0], shared->viz.peak[1])), double(loudest));
        return 0;
    }
    if (mode == "send") {
        if (options.run.empty() || rest.empty()) return usage();
        std::string text;
        for (const std::string& word : rest) text += (text.empty() ? "" : " ") + word;
        return send(options.run, text);
    }
    if (mode == "scan") {
        if (options.library.empty()) return usage();
        av_log_set_level(AV_LOG_FATAL);
        Scanner scanner(options.library);
        const fs::path file = options.library / library_file;
        const std::vector<Track> tracks = scanner.scan(
            options.roots, read_library(file), [](size_t, size_t) { return true; }, [](const std::vector<Track>&) {});
        if (!write_library(file, tracks)) return 1;
        std::printf("%zu tracks\n", tracks.size());
        return 0;
    }
    if (mode != "serve" || options.run.empty() || options.library.empty()) return usage();
    std::signal(SIGTERM, on_signal);
    std::signal(SIGINT, on_signal);
    std::signal(SIGPIPE, SIG_IGN);
    return Engine(std::move(options)).serve();
}
