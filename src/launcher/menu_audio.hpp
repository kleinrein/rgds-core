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
            error = "Menu audio is unavailable on this device.";
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
            for (size_t i = 0; i < frames; i++) {
                auto at = size_t(music_position);
                if (at >= pcm.size()) {
                    at = 0;
                    music_position = 0;
                }
                const size_t next = at + 1 < pcm.size() ? at + 1 : 0;
                const float fraction = float(music_position - at);
                const float sample = (pcm[at] + (pcm[next] - pcm[at]) * fraction) / 32768.f;
                music_gain += std::clamp(music_target - music_gain, -ramp, ramp);
                const float v = sample * music_gain * .7f;
                out[i * 2] += v;
                out[i * 2 + 1] += v;
                music_position += step;
                if (music_position >= pcm.size()) music_position -= pcm.size();
            }
            active = active || music_gain > .0005f || music_target > 0;
        }
        for (size_t i = 0; i < frames * 2; i++) out[i] = std::clamp(out[i], -.9f, .9f);
        sounding.store(active, std::memory_order_release);
    }
    bool open() {
        if (device) return true;
        if (failed || (!loaded && !music_on)) return false; // music needs no cue bank
        if (s.InitSubSystem(SDL_INIT_AUDIO) != 0) {
            failed = true;
            s.ClearError();
            return false;
        }
        initialized = true;
        // Match the 48 kHz dmix slave: a 32 kHz sink made ALSA resample every
        // block, including silence. Clips are resampled by each voice's step.
        SDL_AudioSpec want{};
        want.freq = 48000;
        want.format = AUDIO_F32SYS;
        want.channels = 2;
        want.samples = 512;
        want.callback = callback;
        want.userdata = this;
        device = s.OpenAudioDevice(nullptr, 0, &want, &spec, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
        if (!device) {
            failed = true;
            s.QuitSubSystem(SDL_INIT_AUDIO);
            initialized = false;
            s.ClearError();
            return false;
        }
        paused = true;
        quiet_since = s.GetTicks();
        opens++;
        if (opened) opened();
        return true;
    }
    // Close the sink but keep the decoded bank and settings for the next cue.
    void release() {
        if (device) {
            s.CloseAudioDevice(device);
            device = 0;
            releases++;
        }
        for (auto& v : voices) v = {};
        tail_left = tail_right = 0;
        music_gain = music_target = 0;
        music_started = false;
        sounding.store(false, std::memory_order_release);
        paused = true;
        if (initialized) {
            s.QuitSubSystem(SDL_INIT_AUDIO);
            initialized = false;
        }
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
            error = "Menu audio is unavailable on this device.";
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
        // The decode finished, or the sink was closed under the music: (re)start it.
        if (music_on && !music_muted && !music_started && track_ready.load(std::memory_order_acquire)) start_music();
        if (!device) return;
        if (!paused && !sounding.load(std::memory_order_acquire)) {
            s.PauseAudioDevice(device, 1);
            paused = true;
            quiet_since = now;
        }
        if (paused && idle_close_ms && now - quiet_since >= idle_close_ms) release();
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
