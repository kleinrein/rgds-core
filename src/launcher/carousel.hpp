#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
namespace rgds {
struct Carousel {
    double position=0,from=0,target=0;
    unsigned began=0;
    bool moving=false,dragging=false,moved=false,range=false;
    int start_x=0,last_x=0,start_y=0;
    double start_position=0;
    static constexpr double spacing=136;
    void reset(size_t selected){position=from=target=static_cast<double>(selected);moving=false;}
    void select(size_t selected,unsigned now){from=position;target=static_cast<double>(selected);began=now;moving=std::abs(from-target)>0.001;}
    bool update(unsigned now) {
        if(!moving)return false;
        double t=std::min(1.0,(now-began)/190.0);
        position=from+(target-from)*(1-std::pow(1-t,3));
        if(t>=1){position=target;moving=false;}return true;
    }
    void down(int x,int y,bool seek){dragging=true;moved=false;range=seek;start_x=last_x=x;start_y=y;start_position=position;moving=false;}
    size_t drag(int x,size_t count) {
        last_x=x;moved=moved || std::abs(x-start_x)>8;if(!count)return 0;
        position=range?std::clamp((x-28)/584.0,0.0,1.0)*(count-1):std::clamp(start_position+(start_x-x)/spacing,0.0,static_cast<double>(count-1));
        return static_cast<size_t>(std::llround(position));
    }
    size_t tapped(int x,size_t count) const {
        if(!count)return 0;
        auto index=static_cast<long>(std::llround(position+(x-320)/spacing));
        return static_cast<size_t>(std::clamp(index,0L,static_cast<long>(count)-1));
    }
};
}
