// MenuAudio with a fake audio device whose close is slow, like a dmix stall on the handheld:
// a cue pressed while the idle close runs waits and plays once the sink is open again.
#include "launcher/menu_audio.hpp"
#include <chrono>
#include <cstdio>
#include <vector>

namespace {
std::atomic<Uint32> clock_ms{0};
SDL_AudioCallback fill = nullptr;
void* fill_owner = nullptr;
int failures = 0;

void check(bool ok, const char* what) {
    if (ok) return;
    std::printf("FAIL: %s\n", what);
    failures++;
}

// Runs the mixer until every voice has finished, as the real device would.
void drain(rgds::MenuAudio& audio) {
    std::vector<float> block(4096 * 2);
    for (int i = 0; i < 200 && audio.active(); i++)
        fill(fill_owner, reinterpret_cast<Uint8*>(block.data()), int(block.size() * sizeof(float)));
}

// Polls with a fake clock (10 ms a step, really slept) until `done` or `steps` run out.
template <class Done> void poll_until(rgds::MenuAudio& audio, Done done, int steps = 200) {
    for (int i = 0; i < steps && !done(); i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        clock_ms += 10;
        audio.poll(clock_ms);
    }
}
} // namespace

int main() {
    Sdl s;
    s.InitSubSystem = [](Uint32) { return 0; };
    s.QuitSubSystem = [](Uint32) {};
    s.ClearError = [] {};
    s.GetTicks = [] { return clock_ms.load(); };
    s.LockAudioDevice = [](SDL_AudioDeviceID) {};
    s.UnlockAudioDevice = [](SDL_AudioDeviceID) {};
    s.PauseAudioDevice = [](SDL_AudioDeviceID, int) {};
    s.OpenAudioDevice = [](const char*, int, const SDL_AudioSpec* want, SDL_AudioSpec* got, int) -> SDL_AudioDeviceID {
        *got = *want;
        fill = want->callback;
        fill_owner = want->userdata;
        return 7;
    };
    s.CloseAudioDevice = [](SDL_AudioDeviceID) { std::this_thread::sleep_for(std::chrono::milliseconds(300)); };

    rgds::MenuAudio audio(s);
    audio.configure(true, 100, "assets/sounds");
    check(audio.error.empty(), "the packaged bank loads");

    // First cue: the sink opens on the helper, the cue waits and then plays.
    check(!audio.play(rgds::MenuCue::Navigate, clock_ms), "the first cue waits for the sink");
    poll_until(audio, [&] { return audio.plays == 1; });
    check(audio.plays == 1 && audio.opens == 1, "the waiting cue plays once the sink is up");

    // Quiet: the sink pauses, then the idle close starts on the helper (and takes 300 ms).
    drain(audio);
    audio.idle_close_ms = 50;
    poll_until(audio, [&] { return audio.releases == 1; });
    check(audio.releases == 1 && !audio.open_now(), "the quiet sink closes");

    // A cue during that close is kept, not reported as a dead device.
    check(!audio.play(rgds::MenuCue::Confirm, clock_ms), "a cue during the close waits");
    check(audio.error.empty(), "a cue during the close is not an error");
    poll_until(audio, [&] { return audio.plays == 2; });
    check(audio.plays == 2 && audio.opens == 2, "the cue pressed during the close plays after it");
    check(audio.counts[size_t(rgds::MenuCue::Confirm)] == 1, "it is the cue that was pressed");

    audio.close();
    if (failures) return 1;
    std::printf("menu audio: ok\n");
}
