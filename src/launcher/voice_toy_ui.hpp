#pragma once
// The sound toy's screen state (voice_toy_screen.hpp draws and drives it; the sound is voice_toy.hpp, played by
// MenuAudio). Kept apart from App so main.cpp only carries one member for it.
#include "launcher/kinetics.hpp"
#include "launcher/voice_toy.hpp"
#include "platform/sdl_dynamic.hpp"
#include <array>
#include <cstdint>
#include <map>
#include <tuple>
#include <string>
#include <sys/types.h>
#include <vector>

namespace rgds {

namespace voice_layout {
// The lower screen: the header (Back, Startup sound), the bend pad, six voices and three buttons.
constexpr SDL_Rect startup{392, 20, 224, 44};
constexpr SDL_Rect pad{24, 92, 592, 196};
constexpr int voices_y = 300, voice_w = 88, voice_h = 76;
inline int voice_x(int i) { return 26 + i * 100; }
// Under the voices: the page dots between two arrows (six voices a page).
constexpr int pages_y = 397;
constexpr SDL_Rect page_left{188, 380, 60, 34}, page_right{392, 380, 60, 34};
constexpr int buttons_y = 432, button_w = 196;
constexpr std::array<int, 3> button_x = {16, 222, 428}; // record, play, clear
// The upper screen: the talking face's card and the six sounds under it.
constexpr int slots_y = 326, slot_w = 88, slot_h = 84;
inline int slot_x(int i) { return 26 + i * 100; }
} // namespace voice_layout

struct VoiceToyUi {
    bool open = false, loaded = false;
    int slot = 0;
    std::array<std::vector<float>, voice_toy::slots> clips;    // prepared recordings, voice_toy::rate
    std::array<voice_toy::SlotStyle, voice_toy::slots> styles; // each one's voice and curve
    std::array<std::vector<float>, voice_toy::slots> outlines; // waveform columns for drawing
    int startup_slot = -1;                                     // whose sound plays when the handheld starts
    bool startup_played = false;
    // Recording: the microphone helper (runtime/microphone_test.py --record) streams raw PCM on a pipe.
    bool recording = false, streaming = false;
    pid_t worker = -1;
    int pipe = -1;
    std::string line; // the helper's first line ("rec", "busy", "unavailable")
    std::string odd;  // half a sample left over between reads
    std::vector<int16_t> taking;
    std::vector<float> demo; // preview: a made-up "hello" stands in for the microphone
    Uint32 record_at = 0;
    // Touch.
    enum class Touch { None, Pad, Button, Strip } touch = Touch::None;
    // The voice strip swipes between pages: it follows the finger (strip_dx), and after a turn or a short pull it
    // slides the rest of the way from strip_from to rest over strip_ms.
    int strip_x0 = 0;
    float strip_dx = 0, strip_from = 0;
    Uint32 strip_at = 0;
    static constexpr int strip_width = 600, strip_ms = 260;
    FingerSpeed strip_speed;
    int button = -1, last_column = -1;
    float last_value = 0;
    bool painted = false;
    // A short note over the lower screen.
    std::string toast;
    Uint32 toast_until = 0, drawn_at = 0;
    bool was_playing = false;
    int page = 0;                 // the page of voices shown
    Uint32 erase_armed_until = 0; // Clear, a second time within 3 s, erases the sound
    // The faces (voice_faces.hpp) baked per renderer, voice, size and mouth opening; dropped with the renderers.
    std::map<std::tuple<SDL_Renderer*, int, int, int>, SDL_Texture*> faces;
};

} // namespace rgds
