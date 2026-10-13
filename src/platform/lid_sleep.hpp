#pragma once
#include <optional>

namespace rgds {
// Called at 50 ms intervals. Unknown readings never rearm a closed lid;
// readiness/setting changes must not lose an already debounced closure.
class LidSleep {
    std::optional<bool> candidate;
    int samples=0;
    bool handled=false;
    std::optional<unsigned long long> generation;
public:
    bool poll(std::optional<bool> closed,bool ready,std::optional<unsigned long long> sequence=std::nullopt) {
        // The owner may have been blocked in resume while an open/close pair
        // arrived. A new sensor generation preserves that otherwise lost edge.
        if(closed && sequence && generation!=sequence){generation=sequence;handled=false;candidate.reset();samples=0;}
        if(!closed){candidate.reset();samples=0;return false;}
        if(candidate!=closed){candidate=closed;samples=1;}else if(samples<2)++samples;
        if(samples<2)return false;
        if(!*closed){handled=false;return false;}
        if(!ready || handled)return false;
        handled=true;return true;
    }
};
}
