// Builder-only SDL input driver. Never included in a device release.
#include <SDL.h>
#include <dlfcn.h>
#include <cstdlib>
#include <fstream>
#include <vector>
#include <string>
#include <thread>
#include <chrono>
struct Input {
    Uint32 at;
    std::string type;
    int value;
};
static SDL_JoystickID pad_id = -1;
__attribute__((constructor)) static void start_input_driver() {
    const char* path = std::getenv("RGDS_WAYLAND_TEST_INPUT");
    if (!path) return;
    void* library = dlopen("libSDL2-2.0.so.0", RTLD_NOW | RTLD_GLOBAL);
    if (!library) return;
    auto init = reinterpret_cast<decltype(&SDL_InitSubSystem)>(dlsym(library, "SDL_InitSubSystem"));
    auto push = reinterpret_cast<decltype(&SDL_PushEvent)>(dlsym(library, "SDL_PushEvent"));
    if (!init || !push || init(SDL_INIT_JOYSTICK | SDL_INIT_EVENTS)) return;
    std::ifstream file(path);
    Input input{};
    std::vector<Input> script;
    while (file >> input.at >> input.type >> input.value) script.push_back(input);
    SDL_VirtualJoystickDesc description{};
    description.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    description.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    description.nbuttons = 17;
    description.name = "gpio-keys-control";
    auto attach =
        reinterpret_cast<decltype(&SDL_JoystickAttachVirtualEx)>(dlsym(library, "SDL_JoystickAttachVirtualEx"));
    auto instance =
        reinterpret_cast<decltype(&SDL_JoystickGetDeviceInstanceID)>(dlsym(library, "SDL_JoystickGetDeviceInstanceID"));
    if (attach && instance) {
        int index = attach(&description);
        if (index >= 0) pad_id = instance(index);
    }
    std::thread([push, script] {
        auto start = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        for (auto input : script) {
            std::this_thread::sleep_until(start + std::chrono::milliseconds(input.at));
            SDL_Event event{};
            if (input.type == "reset") event.type = SDL_RENDER_DEVICE_RESET;
            else {
                event.type = input.type == "down" ? SDL_JOYBUTTONDOWN : SDL_JOYBUTTONUP;
                event.jbutton.which = pad_id;
                event.jbutton.button = Uint8(input.value);
                event.jbutton.state = input.type == "down" ? SDL_PRESSED : SDL_RELEASED;
            }
            push(&event);
        }
    }).detach();
}
