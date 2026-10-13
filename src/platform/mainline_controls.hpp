#pragma once
#include <SDL.h>
#include <map>
namespace rgds {
// Linux gpio-keys-control uses standard BTN_* codes, unlike the stock driver.
// SDL enumerates those buttons by keycode, then adds KEY_HOME last.
struct MainlineControls {
    std::map<SDL_JoystickID, Uint8> directions;
    static int button(int raw) {
        constexpr int mapping[]={1,0,3,2,4,5,10,11,6,7,9,12,-1,-1,-1,-1,8};
        return raw>=0 && raw<17?mapping[raw]:-1;
    }
    // Physical control order used by ControlMapping: A/B/X/Y/L/R,
    // Start/Select, directions, triggers and stick clicks.
    static int raw_button(int source) {
        constexpr int mapping[]={1,0,2,3,4,5,9,8,12,13,14,15,6,7,10,11};
        return source>=0 && source<16?mapping[source]:-1;
    }
    bool normalize(SDL_Event& event) {
        if(event.type!=SDL_JOYBUTTONDOWN && event.type!=SDL_JOYBUTTONUP)return true;
        int raw=event.jbutton.button;
        if(raw>=12 && raw<=15){
            constexpr Uint8 bits[]={SDL_HAT_UP,SDL_HAT_DOWN,SDL_HAT_LEFT,SDL_HAT_RIGHT};
            auto which=event.jbutton.which;auto& value=directions[which];
            if(event.type==SDL_JOYBUTTONDOWN)value|=bits[raw-12];else value&=~bits[raw-12];
            event.type=SDL_JOYHATMOTION;event.jhat.which=which;event.jhat.hat=0;event.jhat.value=value;
            return true;
        }
        int mapped=button(raw);if(mapped<0)return false;
        event.jbutton.button=Uint8(mapped);return true;
    }
};
}
