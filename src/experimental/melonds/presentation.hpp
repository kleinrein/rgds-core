// Original RGDS frontend timing; GPL-3.0-or-later.
#pragma once
#include <cstdint>

namespace rgds_melonds {
// Fast-forward still emulates every DS frame. Uploading/presenting both panels
// more often than they refresh wastes time without displaying more information.
// Normal mode deliberately does not use this limiter or skip any presentation.
struct PresentationClock {
    uint64_t next=0;
    void reset(){next=0;}
    bool due(bool fast,bool every_frame,uint64_t now){
        if(!fast || every_frame){next=0;return true;}
        if(next && now<next)return false;
        // Anchor to actual presentation, so a stall never causes catch-up bursts.
        next=now+16667;
        return true;
    }
};
}
