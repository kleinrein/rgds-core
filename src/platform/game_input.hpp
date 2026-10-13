#pragma once
#include <cstdint>

namespace rgds {
inline bool game_switcher_code(int code,bool anbernic){return anbernic?(code==312 || code==354):code==316;}
inline bool game_menu_code(int code,bool anbernic){return code==102 || code==139 || code==158 || code==172 || code==(anbernic?312:316) || (anbernic && code==354);}
// Packet fields match the native evdev framing reader. Passing unblocked touch
// data unchanged preserves the engine's own axis calibration/Type-A decoder.
template<class Packet>
bool filter_game_input(Packet& packet,bool blocked,bool anbernic,double resumed_monotonic,double resumed_realtime){
    bool menu=packet.type==1 && game_menu_code(packet.code,anbernic);
    double stamp=double(packet.micros)/1000000.0;
    bool stale=stamp>0 && stamp<(stamp>1000000000?resumed_realtime:resumed_monotonic);
    bool trigger=menu && packet.value==1 && !blocked && !stale;
    // A release queued during suspend must clear the engine/SDL's held state.
    // Only neutral transitions pass; stale presses and touch positions do not.
    bool release=(packet.type==1 && packet.value==0) || (packet.type==3 &&
        ((packet.code==57 && packet.value==-1) || ((packet.code==16 || packet.code==17) && packet.value==0)));
    if(!menu && stale && release)return false;
    if(!menu && blocked && !stale && packet.type==1)packet.value=0;
    else if(!menu && blocked && !stale && packet.type==3 && packet.code==57)packet.value=-1;
    else if(menu || ((blocked || stale) && (packet.type==1 || packet.type==3 || packet.type==2))){packet.type=0;packet.code=0;packet.value=0;}
    return trigger;
}
}
