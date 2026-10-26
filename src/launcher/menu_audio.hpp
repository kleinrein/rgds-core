#pragma once
#include "platform/sdl_dynamic.hpp"
#include "launcher/menu_music.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace rgds {
enum class MenuCue {
    Navigate,
    Confirm,
    Open,
    Back,
    Enable,
    Disable,
    Favourite,
    Unfavourite,
    Launch,
    Resume,
    Save,
    Load,
    Error,
    Type,
    Adjust,
    LidClose,
    Unlock,
    Count
};
inline constexpr std::array<const char*, size_t(MenuCue::Count)> menu_cue_files = {
    "navigate", "confirm", "open", "back",  "enable", "disable", "favourite", "unfavourite", "launch",
    "resume",   "save",    "load", "error", "type",   "adjust",  "lid-close", "lid-open"};
inline constexpr std::array<const char*, size_t(MenuCue::Count)> menu_cue_labels = {
    "Browse",        "Select",           "Open a view",   "Go back",     "Switch on",     "Switch off",
    "Add favourite", "Remove favourite", "Start a game",  "Resume game", "Save complete", "Load complete",
    "Try again",     "Keyboard",         "Adjust volume", "Close lid",   "Open lid"};

// Cached PCM, four bounded voices, no allocation or file access in the callback.
// The sink opens on the first cue and closes after a few quiet seconds. SDL2
// keeps feeding silence while a device is merely paused, which kept the shared
// dmix stream, the RK817 DAC and a CPU core busy for as long as the menu was up.
class MenuAudio {
    struct Voice {
        const std::vector<int16_t>* clip = nullptr;
        double position = 0, step = 1;
        float left = 0, right = 0, last_left = 0, last_right = 0;
        unsigned attack = 0;
    };
    Sdl& s;
    SDL_AudioDeviceID device = 0;
    SDL_AudioSpec spec{};
    std::array<std::vector<int16_t>, size_t(MenuCue::Count)> clips;
    std::array<Voice, 4> voices{};
    std::atomic<bool> sounding{false};
    bool loaded = false, initialized = false, paused = true, failed = false, enabled = false;
    int volume = 55;
    Uint32 quiet_since = 0;
    float tail_left = 0, tail_right = 0;
    uint32_t random = 0x31415926, last_nav = 0;
    bool navigated = false;
    // Optional menu music: one looping track under the cues. The file is
    // decoded on a helper thread the first time it is wanted; the callback
    // reads it only once `track_ready` is set. Gain ramps over 0.6 s so the
    // track fades in, fades out for a launch and never clicks.
    MusicTrack track;
    std::thread music_loader;
    std::atomic<bool> track_ready{false}, track_loading{false}, track_failed{false};
    bool music_on = false, music_muted = false, music_started = false;
    int music_volume = 40;
    std::string music_file;
    double music_position = 0;
    float music_gain = 0, music_target = 0;
    // The volume buttons also scale everything this sink plays (see
    // set_master): the DAC alone left the cues and the track sounding the
    // same at every level above mute. Ramped so a button press never clicks.
    float master_gain = 1, master_target = 1;
    // Opening (and idling-out) the sink can stall for a long while inside ALSA's dmix; on the UI thread that froze
    // the menu and swallowed input. Both run on a helper while the loop keeps drawing; a cue asked for meanwhile
    // (opening or closing) waits in `pending` and plays once the sink is up, if that is within 600 ms. Handing the
    // sink on (close) still waits for the helper.
    std::thread audio_worker;
    std::atomic<bool> job_done{true};
    int job = 0; // 0 none, 1 opening, 2 closing
    SDL_AudioDeviceID new_device = 0;
    SDL_AudioSpec new_spec{};
    struct PendingCue {
        MenuCue cue = MenuCue::Navigate;
        int direction = 0;
        float pan = 0;
        Uint32 at = 0;
        bool set = false;
    } pending;
    void set_music_target(float target) {
        if (device) s.LockAudioDevice(device);
        music_target = target;
        if (device) s.UnlockAudioDevice(device);
    }
    void join_loader() {
        if (music_loader.joinable()) music_loader.join();
    }
    bool start_music() {
        if (!music_on || music_muted || !track_ready.load(std::memory_order_acquire)) return false;
        if (!device && !open()) {
            if (!job) error = "Menu audio is unavailable on this device.";
            return false;
        }
        s.LockAudioDevice(device);
        music_target = music_volume / 100.f;
        sounding.store(true, std::memory_order_release);
        s.UnlockAudioDevice(device);
        if (paused) {
            s.PauseAudioDevice(device, 0);
            paused = false;
        }
        music_started = true;
        return true;
    }
    float next_random() {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        return float(random & 65535) / 65535;
    }
    static void callback(void* self, Uint8* data, int bytes) {
        static_cast<MenuAudio*>(self)->mix(reinterpret_cast<float*>(data), size_t(bytes) / sizeof(float) / 2);
    }
    void mix(float* out, size_t frames) {
        bool active = false;
        for (size_t i = 0; i < frames; i++) {
            out[i * 2] = tail_left;
            out[i * 2 + 1] = tail_right;
            tail_left *= .95f;
            tail_right *= .95f;
        }
        if (std::abs(tail_left) < .00001f) tail_left = 0;
        if (std::abs(tail_right) < .00001f) tail_right = 0;
        for (auto& voice : voices) {
            if (!voice.clip) continue;
            const auto& pcm = *voice.clip;
            for (size_t i = 0; i < frames; i++) {
                auto at = size_t(voice.position);
                if (at + 1 >= pcm.size()) {
                    voice.clip = nullptr;
                    break;
                }
                float fraction = float(voice.position - at),
                      sample = (pcm[at] + (pcm[at + 1] - pcm[at]) * fraction) / 32768.f;
                float fade = std::min(1.f, float(++voice.attack) / std::max(1, spec.freq / 500));
                voice.last_left = sample * voice.left * fade;
                voice.last_right = sample * voice.right * fade;
                out[i * 2] += voice.last_left;
                out[i * 2 + 1] += voice.last_right;
                voice.position += voice.step;
            }
            active = active || voice.clip;
        }
        if (track_ready.load(std::memory_order_acquire) && (music_gain > .0005f || music_target > 0)) {
            const auto& pcm = track.samples;
            const double step = double(track.rate) / spec.freq;
            const float ramp = 1.f / (spec.freq * .6f);
            // The loop seam is a short crossfade of the tail into the head, so the wrap never steps or clicks.
            const size_t size = pcm.size(), seam = std::min(size / 4, size_t(track.rate / 20));
            for (size_t i = 0; i < frames; i++) {
                if (music_position >= size) music_position = music_position - size + seam;
                auto at = size_t(music_position);
                if (at + 1 >= size) at = size - 2;
                const float fraction = float(music_position - at);
                float sample = (pcm[at] + (pcm[at + 1] - pcm[at]) * fraction) / 32768.f;
                if (seam > 0 && at >= size - seam) {
                    const size_t head = at - (size - seam);
                    const float mix_in = float(music_position - double(size - seam)) / seam;
                    const float from_head = (pcm[head] + (pcm[head + 1] - pcm[head]) * fraction) / 32768.f;
                    sample += (from_head - sample) * std::clamp(mix_in, 0.f, 1.f);
                }
                music_gain += std::clamp(music_target - music_gain, -ramp, ramp);
                const float v = sample * music_gain * .7f;
                out[i * 2] += v;
                out[i * 2 + 1] += v;
                music_position += step;
            }
            active = active || music_gain > .0005f || music_target > 0;
        }
        // Down fast (a mute never lingers), up slowly (the amplifier is still settling after an unmute).
        const float master_down = 1.f / (spec.freq * .05f), master_up = 1.f / (spec.freq * .15f);
        for (size_t i = 0; i < frames; i++) {
            master_gain += std::clamp(master_target - master_gain, -master_down, master_up);
            out[i * 2] *= master_gain;
            out[i * 2 + 1] *= master_gain;
        }
        for (size_t i = 0; i < frames * 2; i++) out[i] = std::clamp(out[i], -.9f, .9f);
        sounding.store(active, std::memory_order_release);
    }
    void finish_open() {
        if (new_device) {
            device = new_device;
            spec = new_spec;
            new_device = 0;
            initialized = true;
            paused = true;
            quiet_since = s.GetTicks();
            master_gain = master_target;
            opens++;
            if (error == "Menu audio is unavailable on this device.") error.clear();
            if (opened) opened();
        } else failed = true;
    }
    // Collects a finished helper job; `block` waits for a running one.
    void reap(bool block) {
        if (!job || (!block && !job_done.load(std::memory_order_acquire))) return;
        audio_worker.join();
        if (job == 1) finish_open();
        job = 0;
    }
    bool opening() const { return job == 1; }
    // True when the sink is up. Otherwise it is being opened (see `opening`) or cannot be.
    bool open() {
        if (device) return true;
        reap(false);
        if (device) return true;
        if (failed || (!loaded && !music_on)) return false; // music needs no cue bank
        if (job) return false;
        job = 1;
        job_done.store(false, std::memory_order_release);
        audio_worker = std::thread([this] {
            if (s.InitSubSystem(SDL_INIT_AUDIO) != 0) s.ClearError();
            else {
                // Match the 48 kHz dmix slave: a 32 kHz sink made ALSA resample every
                // block, including silence. Clips are resampled by each voice's step.
                SDL_AudioSpec want{};
                want.freq = 48000;
                want.format = AUDIO_F32SYS;
                want.channels = 2;
                want.samples = 1024; // fewer wake-ups: a busy launcher frame no longer starves the sink
                want.callback = callback;
                want.userdata = this;
                new_device = s.OpenAudioDevice(nullptr, 0, &want, &new_spec, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
                if (!new_device) {
                    s.QuitSubSystem(SDL_INIT_AUDIO);
                    s.ClearError();
                }
            }
            job_done.store(true, std::memory_order_release);
        });
        return false;
    }
    // Close the sink but keep the decoded bank and settings for the next cue.
    void release(bool later = false) {
        reap(!later);
        pending.set = false;
        const auto closing = device;
        const bool quit = initialized;
        if (device) {
            device = 0;
            releases++;
        }
        for (auto& v : voices) v = {};
        tail_left = tail_right = 0;
        music_gain = music_target = 0;
        music_started = false;
        sounding.store(false, std::memory_order_release);
        paused = true;
        initialized = false;
        if (!closing && !quit) return;
        auto close_sink = [this, closing, quit] {
            if (closing) s.CloseAudioDevice(closing);
            if (quit) s.QuitSubSystem(SDL_INIT_AUDIO);
        };
        if (!later || job) return close_sink();
        job = 2;
        job_done.store(false, std::memory_order_release);
        audio_worker = std::thread([this, close_sink] {
            close_sink();
            job_done.store(true, std::memory_order_release);
        });
    }

public:
    uint64_t plays = 0, opens = 0, reads = 0, releases = 0, music_reads = 0;
    // Runs after every successful open, before the first sample is unpaused.
    std::function<void()> opened;
    Uint32 idle_close_ms = 3000;
    std::array<uint64_t, size_t(MenuCue::Count)> counts{};
    std::string error;
    explicit MenuAudio(Sdl& api) : s(api) {}
    ~MenuAudio() {
        close();
        join_loader();
    }
    MenuAudio(const MenuAudio&) = delete;
    // Menu music: `on` with a level above zero starts the loop (after the
    // one-time decode); off fades it out and the quiet sink closes as usual.
    // Cues keep working either way, and nothing is read while music is off.
    void configure_music(bool on, int level, const std::filesystem::path& file) {
        music_volume = std::clamp(level, 0, 100);
        music_on = on && music_volume > 0 && !track_failed.load();
        if (!music_on) {
            if (device) set_music_target(0);
            music_started = false;
            return;
        }
        if (!track_ready.load(std::memory_order_acquire) && !track_loading.exchange(true)) {
            join_loader();
            music_reads++;
            music_file = file.string();
            failed = false;
            music_loader = std::thread([this] {
                auto decoded = load_music(music_file);
                if (decoded.empty()) track_failed.store(true);
                else {
                    track = std::move(decoded);
                    track_ready.store(true, std::memory_order_release);
                }
                track_loading.store(false);
            });
        }
        if (device && music_started) set_music_target(music_volume / 100.f);
        else start_music();
    }
    // Lets the track fade out before the sink is handed on (a launch).
    void fade_music() {
        music_on = false;
        music_started = false;
        if (device) set_music_target(0);
    }
    // System mute silences the music without forgetting that it is on.
    void mute_music(bool muted) {
        if (muted == music_muted) return;
        music_muted = muted;
        if (muted) {
            if (device) set_music_target(0);
            music_started = false;
        }
    }
    // The system volume level (0-100) as a software gain over a 12 dB range:
    // full at 100, about half at 50, silent at 0.
    static float master_for_level(int level) {
        level = std::clamp(level, 0, 100);
        return level == 0 ? 0.f : std::pow(10.f, -(100 - level) / 100.f * 12.f / 20.f);
    }
    void set_master(int level) {
        const float target = master_for_level(level);
        if (target == master_target) return;
        if (device) s.LockAudioDevice(device);
        master_target = target;
        if (device) s.UnlockAudioDevice(device);
    }
    float master() const { return master_target; }
    bool music_playing() const { return music_started && device; }
    size_t music_samples() const { return track_ready.load(std::memory_order_acquire) ? track.samples.size() : 0; }
    void configure(bool on, int level, const std::filesystem::path& folder) {
        bool changed = enabled != on;
        enabled = on;
        int previous = volume;
        volume = std::clamp(level, 0, 100);
        if (device && previous && volume != previous) {
            s.LockAudioDevice(device);
            float ratio = float(volume) / previous;
            for (auto& v : voices) {
                v.left *= ratio;
                v.right *= ratio;
                v.last_left *= ratio;
                v.last_right *= ratio;
            }
            tail_left *= ratio;
            tail_right *= ratio;
            s.UnlockAudioDevice(device);
        }
        if (!on || !volume) {
            if (!music_on) close();
            else if (device) {
                s.LockAudioDevice(device);
                for (auto& v : voices) v = {};
                tail_left = tail_right = 0;
                s.UnlockAudioDevice(device);
            }
            return;
        }
        if (changed) failed = false;
        if (!loaded) {
            for (size_t i = 0; i < clips.size(); i++) {
                SDL_AudioSpec wav{};
                Uint8* data = nullptr;
                Uint32 bytes = 0;
                reads++;
                auto path = folder / (std::string(menu_cue_files[i]) + ".wav");
                auto* rw = s.RWFromFile(path.c_str(), "rb");
                if (!rw) continue;
                if (s.LoadWAV_RW(rw, 1, &wav, &data, &bytes)) {
                    // The packaged bank is mono PCM16/32kHz, at most 2 seconds.
                    if (wav.freq == 32000 && wav.format == AUDIO_S16LSB && wav.channels == 1 && bytes >= 4 &&
                        bytes <= 128000 && bytes % 2 == 0) {
                        clips[i].resize(bytes / 2);
                        for (size_t n = 0; n < clips[i].size(); n++)
                            clips[i][n] = int16_t(uint16_t(data[n * 2]) | uint16_t(data[n * 2 + 1]) << 8);
                    }
                    s.FreeWAV(data);
                }
            }
            loaded = true;
            s.ClearError();
        }
        error.clear();
        if (std::none_of(clips.begin(), clips.end(), [](const auto& clip) { return !clip.empty(); })) {
            error = "Menu sounds are unavailable.";
            return;
        }
        if (std::any_of(clips.begin(), clips.end(), [](const auto& clip) { return clip.empty(); }))
            error = "Some menu sounds are unavailable.";
        if (failed) error = "Menu audio is unavailable on this device.";
    }
    bool play(MenuCue cue, Uint32 now, int direction = 0, float pan = 0) {
        if (!enabled || !volume) return false;
        auto index = size_t(cue);
        if (index >= clips.size() || clips[index].empty()) return false;
        if (!device && !open()) {
            // Opening, or the idle close still running: the cue waits and plays once the sink is (re)opened.
            if (job) pending = {cue, direction, pan, now, true};
            else error = "Menu audio is unavailable on this device.";
            return false;
        }
        bool small = cue == MenuCue::Navigate || cue == MenuCue::Adjust || cue == MenuCue::Type;
        if (small && navigated && now - last_nav < 45) return false;
        float cadence = small && navigated && now - last_nav < 180 ? 1.025f : 1.f;
        if (small) {
            last_nav = now;
            navigated = true;
        }
        float pitch = small ? (.985f + next_random() * .03f) * cadence *
                                  (direction < 0   ? .8909f
                                   : direction > 0 ? 1.1225f
                                                   : 1.f)
                            : 1.f;
        float gain = volume / 100.f * (small ? .95f : 1.f) * (.96f + next_random() * .04f);
        pan = std::clamp(pan, -.3f, .3f);
        s.LockAudioDevice(device);
        // Navigation has a dedicated voice. It can never cut a launch/save cue.
        size_t slot = 0;
        if (!small) {
            slot = 1;
            for (size_t i = 1; i < voices.size(); i++)
                if (!voices[i].clip) {
                    slot = i;
                    break;
                } else if (voices[i].position > voices[slot].position) slot = i;
        }
        if (voices[slot].clip) {
            tail_left += voices[slot].last_left;
            tail_right += voices[slot].last_right;
        }
        voices[slot] = {&clips[index], 0, double(32000) / spec.freq * pitch, gain * (1 - pan) * .65f,
                        gain * (1 + pan) * .65f};
        sounding.store(true, std::memory_order_release);
        s.UnlockAudioDevice(device);
        if (paused) {
            s.PauseAudioDevice(device, 0);
            paused = false;
        }
        plays++;
        counts[index]++;
        return true;
    }
    void poll(Uint32 now) {
        reap(false);
        // A cue that waited out the idle close opens the sink again.
        if (pending.set && !device && !job) {
            open();
            if (!device && !job) pending.set = false; // the sink cannot open
        }
        if (pending.set && device) {
            const auto cue = pending;
            pending.set = false;
            if (now - cue.at < 600) play(cue.cue, now, cue.direction, cue.pan);
        }
        // The decode finished, or the sink was closed under the music: (re)start it.
        if (music_on && !music_muted && !music_started && track_ready.load(std::memory_order_acquire)) start_music();
        if (!device) return;
        if (!paused && !sounding.load(std::memory_order_acquire)) {
            s.PauseAudioDevice(device, 1);
            paused = true;
            quiet_since = now;
        }
        if (paused && idle_close_ms && now - quiet_since >= idle_close_ms) release(true);
    }
    bool active() const { return sounding.load(std::memory_order_acquire); }
    // Usable when a cue is requested; the sink itself may be closed while quiet.
    bool available() const { return (enabled && volume && loaded && !failed) || (music_on && !failed); }
    bool open_now() const { return device != 0; }
    size_t bytes() const {
        size_t result = 0;
        for (const auto& clip : clips) result += clip.size() * sizeof(int16_t);
        return result;
    }
    void stop() {
        if (device) {
            s.LockAudioDevice(device);
            for (auto& v : voices) v = {};
            tail_left = tail_right = 0;
            music_gain = music_target = 0;
            sounding.store(false, std::memory_order_release);
            s.UnlockAudioDevice(device);
            s.PauseAudioDevice(device, 1);
        }
        paused = true;
        music_started = false;
    }
    void close() {
        release();
        enabled = false;
    }
};
}
