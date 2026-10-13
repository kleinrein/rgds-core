#pragma once
#include <algorithm>

namespace rgds {
// SDL's event wait may poll an open joystick every millisecond. Sleep once
// between explicit polls instead. Eight milliseconds also bounds raw evdev
// touch/volume latency; desktop keyboard-only windows keep their event wakeup.
// Inspired by ROCKNIXDS's documented idle-polling investigation; independently
// implemented for RGDS's existing SDL and evdev input loop.
template<class Api, class Event>
bool wait_ui_event(Api& api, Event* event, int timeout, bool polling_input) {
    if (!polling_input) return api.WaitEventTimeout(event, timeout) != 0;
    if (api.PollEvent(event)) return true;
    api.Delay(std::clamp(timeout, 1, 8));
    return api.PollEvent(event) != 0;
}
}
