// SPDX-License-Identifier: GPL-3.0-or-later
// A film on the handheld: FFmpeg reads, decodes and keeps time; the caller draws.
//
// Three threads of its own: one reads the file into packet queues, one decodes pictures, one decodes sound into
// the ring the SDL audio callback empties. The sound is the clock (a film without sound runs on the wall clock),
// and the draw thread asks for the picture that belongs to now: it never waits on the file or a decoder.
//
// Pictures come out as YUV planes (4:2:0, or NV12 from a hardware decoder) for an SDL YUV texture, so the colour
// conversion and the scaling are the GPU's. Anything else is converted once on the decode thread.
//
// Seeking is by request: the reader seeks, both decoders are flushed, and with `exact` the pictures and sound before
// the target are decoded and dropped, so the film starts on the frame asked for, not on the key frame before it.
#pragma once
#include "music/eq.hpp"
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct AVFormatContext;
struct AVCodecContext;
struct AVPacket;
struct AVFrame;
struct SwsContext;
struct SwrContext;
struct AVFilterGraph;
struct AVFilterContext;
struct ass_library;
struct ass_renderer;
struct ass_track;

namespace media {

struct Track {
    int stream = -1;
    std::string title, language, codec;
    int channels = 0;    // sound
    bool bitmap = false; // subtitles drawn as pictures (Blu-ray, DVD)
    bool external = false;
};

struct Chapter {
    double start = 0, end = 0;
    std::string title;
};

// One picture: planes as the texture takes them.
struct Picture {
    enum Format { None, I420, NV12 } format = None;
    int w = 0, h = 0;
    double pts = 0;
    uint64_t serial = 0;
    const uint8_t* plane[3] = {};
    int pitch[3] = {};
    AVFrame* frame = nullptr; // owns the planes
};

// A subtitle line as text, for the lower screen ("Subtitles below").
struct Line {
    double start = 0, end = 0;
    std::string text;
};

// A subtitle picture over the film, in the film's own pixel grid (RGBA, premultiplied: no).
struct Overlay {
    int w = 0, h = 0;             // the canvas the film is drawn on
    std::vector<uint32_t> pixels; // bytes R, G, B, A
    bool changed = false;
    bool empty = true;
};

struct Info {
    std::string title, container, video_codec, error;
    int w = 0, h = 0;
    double duration = 0, fps = 0;
    bool hardware = false;
    std::vector<Track> audio, subtitles;
    std::vector<Chapter> chapters;
};

class Player {
public:
    Player();
    ~Player();
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    // Opens the file and starts its threads, paused at `start` seconds. `audio_rate`: the sound device's rate.
    bool open(const std::string& path, double start, std::string& error);
    void close();
    const Info& info() const { return info_; }

    // The sound: the SDL device is opened by the player itself (null driver: no sound, the wall clock).
    void play();
    void pause();
    bool paused() const { return paused_; }
    void toggle() { paused_ ? play() : pause(); }
    bool ended() const;
    // 0.5 .. 2.0, sound kept at its pitch.
    void set_speed(double speed);
    double speed() const { return speed_; }
    // Shifts the picture later than the sound (a Bluetooth headset's delay), in seconds.
    void set_picture_delay(double seconds) { picture_delay_ = seconds; }

    // Where the film is now, in seconds (the clock).
    double position() const;
    double duration() const { return info_.duration; }
    void seek(double seconds, bool exact = true);
    void seek_by(double seconds) { seek(target_or_position() + seconds, true); }
    bool seeking() const { return seek_pending_ || seek_settling_; }

    // Sound and subtitle tracks, by index into info().audio / subtitles (-1: off).
    void select_audio(int index);
    void select_subtitles(int index);
    int audio_track() const { return audio_index_; }
    int subtitle_track() const { return subtitle_index_; }
    // An .srt/.ass beside the film, read whole into the subtitle renderer.
    // `select`: shown at once (no subtitles of the film's own are on).
    bool add_subtitle_file(const std::string& path, bool select = true);

    // The picture to show now: the newest decoded one whose time has come. `fresh` is true when it differs from the
    // last one returned. The picture stays valid until the next call.
    const Picture* picture(bool& fresh);

    // The subtitles at the clock, drawn into a w x h overlay (the film's display rect). Cheap when nothing changed.
    void subtitles(Overlay& overlay, int w, int h, int font_scale_percent);
    // The text line on screen now (empty: none), for the lower screen.
    std::string subtitle_text();

    // The music player's equaliser on the film's sound (the curve for the output in use), with make-up gain in dB
    // for the speakers; its limiter keeps boosts from clipping.
    void set_eq(const std::array<float, rgds::music::eq_bands>& gain, bool on, float boost_db);

    // Volume the player applies itself, 0..1 (the DAC is the system's).
    void set_volume(double v) { volume_ = v; }
    double volume() const { return volume_; }

    // Measured, for the status page and tests.
    struct Stats {
        uint64_t decoded = 0, shown = 0, dropped = 0;
        double decode_ms = 0; // the last picture's decode
    };
    Stats stats() const;

    // Called from SDL's audio thread.
    void fill_audio(uint8_t* out, int bytes);

private:
    struct PacketQueue {
        std::deque<AVPacket*> items;
        size_t bytes = 0;
        bool eof = false;
    };
    struct Chunk {
        std::vector<float> samples; // stereo, interleaved
        size_t read = 0;            // frames consumed
        double pts = 0;
        uint64_t serial = 0;
    };

    void read_loop();
    void video_loop();
    void audio_loop();
    bool open_decoder(int stream, AVCodecContext*& ctx, bool video);
    void open_audio_device();
    bool build_tempo(AVCodecContext* a);
    void flush_queue(PacketQueue& q);
    double target_or_position() const;
    double wall() const;
    void decode_subtitle(AVPacket* packet, double time_base);
    void subtitle_loop(int stream);
    void stop_subtitle_reader();
    bool parse_subtitle_file(const std::string& path, ass_track*& track, std::vector<Line>& lines);

    Info info_;
    std::string path_;
    AVFormatContext* format_ = nullptr;
    AVCodecContext* video_ = nullptr;
    AVCodecContext* audio_ = nullptr;
    AVCodecContext* subtitle_ = nullptr;
    int video_stream_ = -1, audio_stream_ = -1, subtitle_stream_ = -1;
    int audio_index_ = -1, subtitle_index_ = -1;
    SwsContext* sws_ = nullptr;
    SwrContext* swr_ = nullptr;
    AVFilterGraph* tempo_graph_ = nullptr;
    AVFilterContext *tempo_in_ = nullptr, *tempo_out_ = nullptr;
    double tempo_built_ = 1.0;
    ass_library* ass_ = nullptr;
    ass_renderer* ass_renderer_ = nullptr;
    ass_track* ass_track_ = nullptr;
    int ass_w_ = 0, ass_h_ = 0, ass_scale_ = 0;
    std::vector<Line> lines_;
    std::vector<std::string> external_paths_; // subtitle files beside the film, by -1 - Track::stream
    struct BitmapSub {
        double start = 0, end = 0;
        int w = 0, h = 0; // the picture the rects belong to
        struct Rect {
            int x, y, w, h;
            std::vector<uint32_t> px;
        };
        std::vector<Rect> rects;
    };
    std::deque<BitmapSub> bitmaps_;
    const BitmapSub* bitmap_shown_ = nullptr;
    double bitmap_shown_start_ = -1;
    std::mutex sub_mutex_;

    std::thread reader_, video_thread_, audio_thread_, sub_thread_;
    std::atomic<bool> sub_stop_{false};
    std::atomic<uint64_t> sub_gen_{1};
    std::atomic<double> sub_seek_to_{0};
    std::atomic<bool> stop_{false};
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    PacketQueue vq_, aq_;
    std::deque<Picture> pictures_; // decoded, oldest first
    Picture shown_;
    uint64_t shown_serial_ = ~0ull;
    double shown_pts_ = -1;

    // Seeking
    std::atomic<uint64_t> serial_{1};
    bool seek_pending_ = false, seek_exact_ = false;
    std::atomic<bool> seek_settling_{false};
    double seek_target_ = 0;
    std::atomic<double> drop_before_{-1};

    // Sound and the clock
    uint32_t device_ = 0;
    int device_rate_ = 48000, device_samples_ = 1024;
    std::deque<Chunk> chunks_;
    size_t queued_frames_ = 0;
    double clock_pts_ = 0;  // the film's time at the last callback's end of output
    uint64_t clock_at_ = 0; // SDL performance counter then
    bool clock_valid_ = false;
    double wall_base_ = 0; // no sound: the film time at wall_at_
    uint64_t wall_at_ = 0;
    std::atomic<bool> paused_{true};
    double speed_ = 1.0;
    double picture_delay_ = 0;
    double volume_ = 1.0;
    rgds::music::Equaliser eq_;
    std::vector<float> mix_;
    std::atomic<bool> read_eof_{false};
    Stats stats_;
};

// Pictures of a film at even steps, for the scrub strip and library posters, on a thread of its own with its own
// decoder (it never touches the Player's).
class Thumbnails {
public:
    ~Thumbnails();
    // `count` pictures of `w` x `h` (RGBA, the film's shape inside, black around it).
    void start(const std::string& path, int count, int w, int h);
    void stop();
    int count() const { return count_; }
    int w() const { return w_; }
    int h() const { return h_; }
    double time_of(int i) const;
    // Pictures done since the last call (indexes), with their pixels.
    std::vector<int> take_new();
    const uint32_t* pixels(int i) const;

private:
    void run();
    std::string path_;
    int count_ = 0, w_ = 0, h_ = 0;
    double duration_ = 0;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    mutable std::mutex mutex_;
    std::vector<std::vector<uint32_t>> done_;
    std::vector<int> fresh_;
};

// One picture from a film at `fraction` of its length (skipping black frames), for a poster. RGBA, at most max_w wide.
bool poster(const std::string& path, double fraction, int max_w, std::vector<uint32_t>& px, int& w, int& h,
            double& duration, std::string& error);

// A length as 1:02:03 or 2:03.
std::string clock_text(double seconds);

} // namespace media
