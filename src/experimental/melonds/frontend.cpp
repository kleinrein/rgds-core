// Original RGDS NDS frontend, distributed under GPL-3.0-or-later with melonDS.
#include "NDS.h"
#include "SPI_Firmware.h"
#include "microphone.hpp"
#include "Args.h"
#include "Savestate.h"
#include "frontend_platform.hpp"
#include "test_cartridge.hpp"
#include "presentation.hpp"
#include <SDL.h>
#include <array>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

using namespace melonDS;
namespace fs = std::filesystem;
namespace {
volatile sig_atomic_t interrupted = 0;
bool running = true, fast = false, threaded = true, battery_dirty = false, state_loaded = false;
std::vector<u8> battery;
std::unique_ptr<NDS> nds;
fs::path battery_path, state_path;
u64 last_flush = 0;
u32 completed_frames = 0;
SDL_AudioDeviceID microphone = 0;
CaptureMic alsa_microphone;
bool mic_requested = false, mic_attempted = false;
u64 blow_until = 0, lid_until = 0;
uint32_t mic_noise = 0x1957;
void require(bool value, const std::string& why) {
    if (!value) throw std::runtime_error(why);
}
void signal_stop(int) { interrupted = 1; }
std::string environment(const char* name, const std::string& fallback = {}) {
    auto* v = std::getenv(name);
    return v ? v : fallback;
}
u64 micros() { return Platform::GetUSCount(); }
struct FrameProfile {
    bool enabled = environment("RGDS_MELONDS_PROFILE") == "1";
    bool speed = false, renderer = true;
    u64 began = 0, frames = 0, presented = 0, core = 0, audio = 0, draw = 0, pacing = 0;
    void reset(u64 now) {
        began = now;
        frames = presented = core = audio = draw = pacing = 0;
        speed = fast;
        renderer = threaded;
    }
    void begin(u64 now, bool discontinuity) {
        if (enabled && (!began || discontinuity || speed != fast || renderer != threaded)) reset(now);
    }
    void report(u64 now, bool force = false) {
        if (!enabled || !frames || (!force && now - began < 1000000)) return;
        std::fprintf(
            stderr,
            "{\"type\":\"melonds-profile\",\"fast_forward\":%s,\"threaded_3d\":%s,\"frames\":%llu,\"presented\":%llu,\"game_frames_total\":%u,\"wall_seconds\":%.6f,\"core_us_per_frame\":%.3f,\"audio_us_per_frame\":%.3f,\"draw_us_per_frame\":%.3f,\"pacing_us_per_frame\":%.3f}\n",
            speed ? "true" : "false", renderer ? "true" : "false", (unsigned long long)frames,
            (unsigned long long)presented, completed_frames, double(now - began) / 1000000, double(core) / frames,
            double(audio) / frames, double(draw) / frames, double(pacing) / frames);
        reset(now);
    }
};
void atomic_write(const fs::path& path, const void* data, size_t length) {
    require(!path.empty(), "Save path missing");
    fs::create_directories(path.parent_path());
    auto pending = path.string() + ".pending";
    int fd = ::open(pending.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    require(fd >= 0, "Could not stage save: " + path.string());
    const auto* bytes = static_cast<const u8*>(data);
    size_t at = 0;
    bool okay = true;
    while (at < length) {
        auto n = ::write(fd, bytes + at, length - at);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            okay = false;
            break;
        }
        at += size_t(n);
    }
    if (okay) okay = ::fsync(fd) == 0;
    if (::close(fd) != 0) okay = false;
    if (!okay) {
        ::unlink(pending.c_str());
        throw std::runtime_error("Could not flush save: " + path.string());
    }
    fs::rename(pending, path);
    fd = ::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    require(fd >= 0, "Could not open save directory");
    okay = ::fsync(fd) == 0;
    ::close(fd);
    require(okay, "Could not flush save directory");
}
std::vector<u8> read_bytes(const fs::path& path, size_t maximum) {
    auto length = fs::file_size(path);
    require(length > 0 && length <= maximum, "Invalid file size: " + path.string());
    std::vector<u8> bytes(length);
    std::ifstream stream(path, std::ios::binary);
    require(bool(stream.read(reinterpret_cast<char*>(bytes.data()), bytes.size())), "Could not read " + path.string());
    return bytes;
}
void flush_battery() {
    if (battery_dirty) {
        atomic_write(battery_path, battery.data(), battery.size());
        battery_dirty = false;
        last_flush = micros();
    }
}
void save_state() {
    Savestate state;
    require(nds->DoSavestate(&state) && !state.Error, "Checkpoint save failed");
    state.Finish();
    require(!state.Error, "Checkpoint finalization failed");
    flush_battery();
    atomic_write(state_path, state.Buffer(), state.Length());
}
void load_state() {
    auto bytes = read_bytes(state_path, 64 * 1024 * 1024);
    Savestate state(bytes.data(), bytes.size(), false);
    require(!state.Error, "Invalid checkpoint header");
    Savestate backup;
    require(nds->DoSavestate(&backup) && !backup.Error, "Could not protect current state");
    backup.Finish();
    if (!nds->DoSavestate(&state) || state.Error) {
        Savestate restore(backup.Buffer(), backup.Length(), false);
        require(nds->DoSavestate(&restore) && !restore.Error, "Could not restore previous state");
        throw std::runtime_error("Checkpoint rejected; previous state restored");
    }
    nds->SetKeyMask(0xfff);
    nds->ReleaseScreen();
    state_loaded = true;
}
void threading(bool on) {
    RendererSettings settings{};
    settings.ScaleFactor = 1;
    settings.Threaded = on;
    nds->GetRenderer().SetRenderSettings(settings);
    threaded = on;
}
struct Video {
    SDL_Window* windows[2]{};
    SDL_Renderer* renderers[2]{};
    SDL_Texture* textures[2]{};
    u64 audio_samples = 0;
    SDL_AudioDeviceID audio = 0;
    SDL_GameController* pad = nullptr;
    ~Video() {
        if (microphone) {
            SDL_CloseAudioDevice(microphone);
            microphone = 0;
        }
        if (audio) SDL_CloseAudioDevice(audio);
        if (pad) SDL_GameControllerClose(pad);
        for (int i = 0; i < 2; i++) {
            if (textures[i]) SDL_DestroyTexture(textures[i]);
            if (renderers[i]) SDL_DestroyRenderer(renderers[i]);
            if (windows[i]) SDL_DestroyWindow(windows[i]);
        }
        SDL_Quit();
    }
    void open(bool test) {
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
        SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
        require(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) == 0,
                SDL_GetError());
        int displays = SDL_GetNumVideoDisplays();
        require(test || displays >= 2, "melonDS needs two displays");
        for (int i = 0; i < 2; i++) {
            int display = i;
            auto selection = environment(i ? "RGDS_BOTTOM_DISPLAY" : "RGDS_TOP_DISPLAY");
            if (!selection.empty()) {
                bool found = false;
                for (int n = 0; n < displays; n++)
                    if (selection == std::to_string(n) ||
                        (SDL_GetDisplayName(n) && selection == SDL_GetDisplayName(n))) {
                        display = n;
                        found = true;
                        break;
                    }
                require(found, "Configured screen is unavailable");
            }
            if (test) display = 0;
            SDL_Rect bounds{0, 0, 640, 480};
            if (!test)
                require(SDL_GetDisplayBounds(display, &bounds) == 0 && bounds.w > 0 && bounds.h > 0,
                        "Cannot determine native panel size");
            windows[i] =
                SDL_CreateWindow(i ? "melonDS Bottom" : "melonDS Top", SDL_WINDOWPOS_UNDEFINED_DISPLAY(display),
                                 SDL_WINDOWPOS_UNDEFINED_DISPLAY(display), bounds.w, bounds.h,
                                 test ? SDL_WINDOW_HIDDEN : SDL_WINDOW_FULLSCREEN_DESKTOP);
            require(windows[i], SDL_GetError());
            // The DS clock paces both panels once. Per-window vsync could block
            // twice and halve the dual-screen frame rate.
            renderers[i] = SDL_CreateRenderer(windows[i], -1, test ? SDL_RENDERER_SOFTWARE : SDL_RENDERER_ACCELERATED);
            require(renderers[i], SDL_GetError());
            SDL_RenderSetLogicalSize(renderers[i], 256, 192);
            // The upstream software renderer returns BGRA bytes on every host.
            textures[i] =
                SDL_CreateTexture(renderers[i], SDL_PIXELFORMAT_BGRA32, SDL_TEXTUREACCESS_STREAMING, 256, 192);
            require(textures[i], SDL_GetError());
        }
        SDL_AudioSpec desired{};
        desired.freq = 48000;
        desired.format = AUDIO_S16SYS;
        desired.channels = 2;
        desired.samples = 512;
        audio = SDL_OpenAudioDevice(nullptr, 0, &desired, nullptr, 0);
        require(audio != 0, SDL_GetError());
        SDL_PauseAudioDevice(audio, 0);
        for (int i = 0; i < SDL_NumJoysticks(); i++)
            if (SDL_IsGameController(i)) {
                pad = SDL_GameControllerOpen(i);
                break;
            }
    }
    void draw() {
        void* frames[2]{};
        require(nds->GPU.GetFramebuffers(&frames[0], &frames[1]), "No DS framebuffers");
        for (int i = 0; i < 2; i++) {
            require(SDL_UpdateTexture(textures[i], nullptr, frames[i], 256 * 4) == 0, SDL_GetError());
            SDL_RenderClear(renderers[i]);
            require(SDL_RenderCopy(renderers[i], textures[i], nullptr, nullptr) == 0, SDL_GetError());
            SDL_RenderPresent(renderers[i]);
        }
    }
    void sound() {
        std::array<s16, 8192> pcm{};
        int count = nds->SPU.ReadOutput(pcm.data(), pcm.size() / 2);
        if (fast) {
            SDL_ClearQueuedAudio(audio);
            return;
        }
        if (SDL_GetQueuedAudioSize(audio) > 48000 * 4 / 10) SDL_ClearQueuedAudio(audio);
        if (count > 0) {
            require(SDL_QueueAudio(audio, pcm.data(), count * 4) == 0, SDL_GetError());
            audio_samples += count;
        }
    }
};
}
namespace rgds_melonds {
void mic_start() {
    mic_requested = true;
    if (environment("RGDS_MICROPHONE", "1") == "0" || !SDL_WasInit(SDL_INIT_AUDIO)) return;
    if (!mic_attempted) {
        mic_attempted = true;
        if (environment("SDL_AUDIODRIVER") != "dummy" && alsa_microphone.open(47605)) return;
        SDL_AudioSpec desired{};
        desired.freq = 47605;
        desired.format = AUDIO_S16SYS;
        desired.channels = 1;
        desired.samples = 512;
        if (!microphone) microphone = SDL_OpenAudioDevice(nullptr, 1, &desired, nullptr, 0);
        if (!microphone) std::fprintf(stderr, "Microphone unavailable: %s\n", SDL_GetError());
    }
    if (microphone) {
        SDL_ClearQueuedAudio(microphone);
        SDL_PauseAudioDevice(microphone, 0);
    }
}
void mic_stop() {
    mic_requested = false;
    alsa_microphone.close();
    mic_attempted = false;
    if (microphone) {
        SDL_PauseAudioDevice(microphone, 1);
        SDL_ClearQueuedAudio(microphone);
    }
}
int mic_read(s16* data, int maximum) {
    if (!mic_requested || !data || maximum <= 0) return 0;
    if (micros() < blow_until) {
        for (int i = 0; i < maximum; i++) {
            mic_noise = mic_noise * 1664525 + 1013904223;
            data[i] = s16(int(mic_noise >> 16) - 32768);
        }
        return maximum;
    }
    if (alsa_microphone.active()) return alsa_microphone.read(data, maximum);
    if (!microphone) return 0;
    if (SDL_GetQueuedAudioSize(microphone) > 47605 * 2 / 4) SDL_ClearQueuedAudio(microphone);
    return int(SDL_DequeueAudio(microphone, data, Uint32(maximum) * 2) / 2);
}
void stop(int reason) {
    running = false;
    if (reason != int(Platform::External)) std::fprintf(stderr, "melonDS stopped: %d\n", reason);
}
void battery_write(const u8* data, u32 length, u32 offset, u32 count) {
    if (!data || !length || length > 16 * 1024 * 1024) return;
    if (battery.size() != length) battery.assign(data, data + length);
    else {
        offset %= length;
        count = std::min(count, length);
        u32 first = std::min(count, length - offset);
        std::memcpy(battery.data() + offset, data + offset, first);
        if (count > first) std::memcpy(battery.data(), data, count - first);
    }
    battery_dirty = true;
}
}
// Called by the RGDS bridge at SDL event polling, between emulated frames.
extern "C" uint32_t rgds_emulated_frames() { return completed_frames; }
extern "C" int rgds_ds_control(int action) {
    if (!nds || (action != 0 && action != 1)) return 0;
    if (action == 0) blow_until = micros() + 2000000;
    else {
        nds->SetLidClosed(true);
        lid_until = micros() + 2000000;
    }
    return 1;
}
extern "C" int rgds_set_threaded_3d(int value) {
    if (!nds || (value != 0 && value != 1)) return 0;
    threading(value != 0);
    return 1;
}
extern "C" int rgds_load_checkpoint() {
    try {
        if (!nds) return 0;
        load_state();
        return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Load failed: %s\n", e.what());
        return 0;
    }
}
int main(int argc, char** argv) {
    Video video;
    bool test = false;
    int frame_limit = 0;
    fs::path rom;
    try {
        for (int i = 1; i < argc; i++) {
            std::string arg = argv[i];
            auto value = [&]() {
                require(i + 1 < argc, "Missing option value");
                return std::string(argv[++i]);
            };
            if (arg == "--rom") rom = value();
            else if (arg == "--save") battery_path = value();
            else if (arg == "--state") state_path = value();
            else if (arg == "--frames") frame_limit = std::stoi(value());
            else if (arg == "--self-test") test = true;
            else if (arg == "--self-test-fast") {
                test = true;
                fast = true;
            } else throw std::runtime_error("Unknown option: " + arg);
        }
        require(!battery_path.empty() && !state_path.empty(), "Explicit save and checkpoint paths required");
        require(frame_limit >= 0 && frame_limit <= 10000000, "Invalid frame limit");
        std::signal(SIGINT, signal_stop);
        std::signal(SIGTERM, signal_stop);
        NDSArgs args;
        args.OutputSampleRate = 48000;
        nds = std::make_unique<NDS>(std::move(args));
        std::unique_ptr<NDSCart::CartCommon> cart;
        if (test) {
            auto data = cartridge();
            cart = NDSCart::ParseROM(data.data(), data.size());
        } else {
            require(fs::is_regular_file(rom), "ROM missing");
            auto size = fs::file_size(rom);
            require(size >= 512 && size <= 512 * 1024 * 1024, "Invalid NDS ROM size");
            auto bytes = std::make_unique<u8[]>(size);
            std::ifstream input(rom, std::ios::binary);
            require(bool(input.read(reinterpret_cast<char*>(bytes.get()), size)), "Could not read ROM");
            cart = NDSCart::ParseROM(std::move(bytes), size);
        }
        require(bool(cart), "Cartridge is unsupported");
        nds->SetNDSCart(std::move(cart));
        if (fs::exists(battery_path)) {
            auto saved = read_bytes(battery_path, 16 * 1024 * 1024);
            nds->SetNDSSave(saved.data(), saved.size());
        }
        int language = std::clamp(std::atoi(environment("RGDS_DS_LANGUAGE", "1").c_str()), 0, 5);
        for (auto& user : nds->GetFirmware().GetUserData()) {
            user.Settings = (user.Settings & ~7) | language;
            user.UpdateChecksum();
        }
        nds->Reset();
        nds->SetupDirectBoot(test ? "RGDS-Probe.nds" : rom.filename().string());
        std::time_t clock = std::time(nullptr);
        std::tm local{};
        if (localtime_r(&clock, &local))
            nds->RTC.SetDateTime(std::clamp(local.tm_year + 1900, 2000, 2099), local.tm_mon + 1, local.tm_mday,
                                 local.tm_hour, local.tm_min, local.tm_sec);
        threading(environment("RGDS_THREADED_3D", "1") != "0");
        nds->Start();
        // The runner copies only this game's verified automatic checkpoint.
        if (environment("RGDS_NATIVE_AUTOLOAD") == "1" && fs::exists(state_path)) load_state();
        if (test) {
            nds->ARM9Write32(0x04000000, 0x10000);
            nds->ARM9Write32(0x04001000, 0x10000);
            nds->ARM9Write16(0x05000000, 0x001f);
            nds->ARM9Write16(0x05000400, 0x03e0);
            if (!frame_limit) frame_limit = 90;
        }
        video.open(test);
        if (mic_requested) rgds_melonds::mic_start();
        u64 began = micros(), deadline = began;
        unsigned frames = 0, presented = 0;
        bool pen = false;
        int pen_x = 0, pen_y = 0;
        u64 wake = began;
        FrameProfile profile;
        rgds_melonds::PresentationClock presentation;
        const bool every_frame = environment("RGDS_MELONDS_PRESENT_EVERY_FRAME") == "1";
        const std::array<SDL_Scancode, 12> keys = {SDL_SCANCODE_X,      SDL_SCANCODE_Z,     SDL_SCANCODE_RSHIFT,
                                                   SDL_SCANCODE_RETURN, SDL_SCANCODE_RIGHT, SDL_SCANCODE_LEFT,
                                                   SDL_SCANCODE_UP,     SDL_SCANCODE_DOWN,  SDL_SCANCODE_W,
                                                   SDL_SCANCODE_Q,      SDL_SCANCODE_S,     SDL_SCANCODE_A};
        const std::array<SDL_GameControllerButton, 12> buttons = {SDL_CONTROLLER_BUTTON_A,
                                                                  SDL_CONTROLLER_BUTTON_B,
                                                                  SDL_CONTROLLER_BUTTON_BACK,
                                                                  SDL_CONTROLLER_BUTTON_START,
                                                                  SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
                                                                  SDL_CONTROLLER_BUTTON_DPAD_LEFT,
                                                                  SDL_CONTROLLER_BUTTON_DPAD_UP,
                                                                  SDL_CONTROLLER_BUTTON_DPAD_DOWN,
                                                                  SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
                                                                  SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
                                                                  SDL_CONTROLLER_BUTTON_X,
                                                                  SDL_CONTROLLER_BUTTON_Y};
        while (running && !interrupted && (!frame_limit || frames < unsigned(frame_limit))) {
            auto time = micros();
            if (lid_until && time >= lid_until) {
                nds->SetLidClosed(false);
                lid_until = 0;
            }
            const bool resumed = time - wake > 250000;
            if (resumed) {
                SDL_ClearQueuedAudio(video.audio);
                if (microphone) SDL_ClearQueuedAudio(microphone);
                pen = false;
                nds->ReleaseScreen();
                deadline = time;
                presentation.reset();
            }
            wake = time;
            SDL_Event event{};
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_QUIT) running = false;
                if (event.type == SDL_KEYDOWN && !event.key.repeat) {
                    if (event.key.keysym.sym == SDLK_F5) {
                        try {
                            save_state();
                        } catch (const std::exception& e) {
                            std::fprintf(stderr, "Save failed: %s\n", e.what());
                        }
                    }
                    if (event.key.keysym.sym == SDLK_F7) {
                        try {
                            load_state();
                            pen = false;
                            SDL_ClearQueuedAudio(video.audio);
                        } catch (const std::exception& e) {
                            std::fprintf(stderr, "Load failed: %s\n", e.what());
                        }
                    }
                    if (event.key.keysym.sym == SDLK_TAB) {
                        fast = !fast;
                        deadline = micros();
                        presentation.reset();
                        SDL_ClearQueuedAudio(video.audio);
                    }
                }
                if (event.type == SDL_FINGERDOWN || event.type == SDL_FINGERMOTION) {
                    pen = true;
                    pen_x = std::clamp(int(event.tfinger.x * 256), 0, 255);
                    pen_y = std::clamp(int(event.tfinger.y * 192), 0, 191);
                }
                if (event.type == SDL_FINGERUP) pen = false;
                if (event.type == SDL_MOUSEBUTTONDOWN && event.button.windowID == SDL_GetWindowID(video.windows[1])) {
                    pen = true;
                    pen_x = std::clamp(event.button.x, 0, 255);
                    pen_y = std::clamp(event.button.y, 0, 191);
                }
                if (event.type == SDL_MOUSEMOTION && pen &&
                    event.motion.windowID == SDL_GetWindowID(video.windows[1])) {
                    pen_x = std::clamp(event.motion.x, 0, 255);
                    pen_y = std::clamp(event.motion.y, 0, 191);
                }
                if (event.type == SDL_MOUSEBUTTONUP) pen = false;
                if (event.type == SDL_CONTROLLERDEVICEADDED && !video.pad)
                    video.pad = SDL_GameControllerOpen(event.cdevice.which);
                if (event.type == SDL_CONTROLLERDEVICEREMOVED && video.pad &&
                    !SDL_GameControllerGetAttached(video.pad)) {
                    SDL_GameControllerClose(video.pad);
                    video.pad = nullptr;
                }
            }
            if (!running) break;
            const bool restored = state_loaded;
            if (state_loaded) {
                state_loaded = false;
                pen = false;
                SDL_ClearQueuedAudio(video.audio);
                if (microphone) SDL_ClearQueuedAudio(microphone);
                deadline = micros();
                presentation.reset();
            }
            if (profile.enabled) {
                auto now = micros();
                profile.begin(now, resumed || restored || now - time > 250000);
            }
            const u8* keyboard = SDL_GetKeyboardState(nullptr);
            u32 mask = 0xfff;
            for (int i = 0; i < 12; i++)
                if (keyboard[keys[i]] || (video.pad && SDL_GameControllerGetButton(video.pad, buttons[i])))
                    mask &= ~(1u << i);
            if (test) {
                mask = frames == 3 ? 0xffe : 0xfff;
                pen = frames == 3;
                pen_x = 128;
                pen_y = 96;
            }
            nds->SetKeyMask(mask);
            if (pen) nds->TouchScreen(pen_x, pen_y);
            else nds->ReleaseScreen();
            if (test) require(((nds->ARM7Read16(0x04000136) & 0x40) == 0) == pen, "Pen state mismatch");
            auto core_began = profile.enabled ? micros() : 0;
            nds->RunFrame();
            completed_frames++;
            auto audio_began = profile.enabled ? micros() : 0;
            video.sound();
            auto draw_began = profile.enabled ? micros() : 0;
            // Self-test frame 5 checks readback pixels and therefore always draws.
            bool drawn = (test && frames == 4) || presentation.due(fast, every_frame, fast ? micros() : 0);
            if (drawn) {
                video.draw();
                presented++;
            }
            auto draw_end = profile.enabled ? micros() : 0;
            frames++;
            if (test) {
                u32 sampled;
                std::memcpy(&sampled, nds->MainRAM + 0x1004, 4);
                require((sampled & 0x3ff) == (mask & 0x3ff), "Game did not sample buttons");
            }
            if (battery_dirty && micros() - last_flush >= 500000) flush_battery();
            if (test && frames == 5) {
                void* screens[2]{};
                require(nds->GPU.GetFramebuffers(&screens[0], &screens[1]), "Screens missing");
                require(std::memcmp(screens[0], screens[1], 256 * 192 * 4) != 0, "Screen outputs identical");
                for (int panel = 0; panel < 2; panel++) {
                    // melonDS returns BGRA bytes; read SDL in an independent RGBA
                    // format so matching the same wrong packed format cannot pass.
                    std::vector<u8> pixels(640 * 480 * 4);
                    require(SDL_RenderReadPixels(video.renderers[panel], nullptr, SDL_PIXELFORMAT_RGBA32, pixels.data(),
                                                 640 * 4) == 0,
                            "SDL readback failed");
                    const auto* expected = static_cast<const u8*>(screens[panel]) + (96 * 256 + 128) * 4;
                    const auto* actual = pixels.data() + (240 * 640 + 320) * 4;
                    require(actual[0] == expected[2] && actual[1] == expected[1] && actual[2] == expected[0],
                            "SDL RGB channels differ from emulated screen");
                }
                u32 saved;
                std::memcpy(&saved, nds->MainRAM + 0x1000, 4);
                save_state();
                nds->ARM9Write32(0x02001000, 0);
                load_state();
                u32 restored;
                std::memcpy(&restored, nds->MainRAM + 0x1000, 4);
                require(restored == saved, "Checkpoint RAM did not restore");
                for (int toggle = 0; toggle < 20; toggle++) {
                    require(rgds_set_threaded_3d(toggle % 2) == 1 && threaded == bool(toggle % 2),
                            "Live threading failed");
                    u32 current;
                    std::memcpy(&current, nds->MainRAM + 0x1000, 4);
                    require(current == saved, "Threading changed game progress");
                }
                auto good = read_bytes(state_path, 64 * 1024 * 1024);
                auto broken = good;
                broken[0] ^= 0xff;
                atomic_write(state_path, broken.data(), broken.size());
                require(rgds_load_checkpoint() == 0, "Corrupt checkpoint accepted");
                atomic_write(state_path, good.data(), good.size());
                require(rgds_load_checkpoint() == 1, "Valid checkpoint rejected");
                rgds_melonds::mic_start();
                require(microphone != 0, "Dummy microphone did not open");
                SDL_Delay(40);
                std::array<s16, 4096> captured{};
                require(rgds_melonds::mic_read(captured.data(), captured.size()) > 0, "Microphone queue empty");
                rgds_melonds::mic_stop();
                require(rgds_melonds::mic_read(captured.data(), captured.size()) == 0,
                        "Stopped microphone still active");
                std::array<u8, 512> nvram{};
                for (size_t i = 0; i < nvram.size(); i++) nvram[i] = u8(i);
                rgds_melonds::battery_write(nvram.data(), nvram.size(), 0, nvram.size());
                flush_battery();
                nvram[510] = 17;
                nvram[511] = 18;
                nvram[0] = 19;
                nvram[1] = 20;
                rgds_melonds::battery_write(nvram.data(), nvram.size(), 510, 4);
                flush_battery();
                require(read_bytes(battery_path, 1024) == std::vector<u8>(nvram.begin(), nvram.end()),
                        "Battery save readback differs");
            }
            auto pacing_began = profile.enabled ? micros() : 0;
            if (!fast) {
                deadline += 16715;
                auto now = micros();
                if (deadline > now) Platform::Sleep(deadline - now);
                else if (now - deadline > 50000) deadline = now;
            }
            if (profile.enabled) {
                auto now = micros();
                profile.frames++;
                profile.presented += drawn;
                profile.core += audio_began - core_began;
                profile.audio += draw_began - audio_began;
                profile.draw += draw_end - draw_began;
                profile.pacing += now - pacing_began;
                profile.report(now);
            }
        }
        profile.report(micros(), true);
        flush_battery();
        if (test) {
            if (!fast) require(video.audio_samples > 1000, "No audio samples queued");
            require(!nds->JIT.JitBlocks9.empty() && !nds->JIT.JitBlocks7.empty(), "JIT inactive");
            require(fs::file_size(state_path) > 1000, "Checkpoint missing");
            u32 progress;
            std::memcpy(&progress, nds->MainRAM + 0x1000, 4);
            std::printf(
                "{\"frames\":%u,\"presented\":%u,\"progress\":%u,\"seconds\":%.3f,\"fast_forward\":%s,\"jit\":true,\"two_screens\":true,\"audio\":%s,\"live_threading\":true,\"checkpoint\":true}\n",
                frames, presented, progress, (micros() - began) / 1000000.0, fast ? "true" : "false",
                fast ? "false" : "true");
        }
        nds->Stop();
        nds.reset();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "melonDS: %s\n", error.what());
        if (nds) {
            nds->Stop();
            nds.reset();
        }
        return 1;
    }
}
