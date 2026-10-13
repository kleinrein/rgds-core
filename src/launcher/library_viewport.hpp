#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>

namespace rgds {
// One coordinate system for list drawing, touch rows, swipes and the scrollbar.
struct LibraryViewport {
    static constexpr int top=147,height=316,stride=54,row_height=48;
    int scroll=0;
    int from=0,target=0;unsigned began=0;bool moving=false;
    int maximum(size_t count) const {return std::max(0,int(count)*stride-height);}
    void clamp(size_t count){scroll=std::clamp(scroll,0,maximum(count));}
    void reveal(size_t selected,size_t count){
        moving=false;
        int row=int(selected)*stride;
        if(row<scroll)scroll=row;
        if(row+stride>scroll+height)scroll=row+stride-height;
        clamp(count);
    }
    void select(size_t selected,size_t count,unsigned now,bool animate){
        int previous=scroll;reveal(selected,count);target=scroll;from=previous;began=now;
        moving=animate && from!=target;if(moving)scroll=from;
    }
    bool update(unsigned now){
        if(!moving)return false;
        double t=std::min(1.0,(now-began)/190.0);
        scroll=int(std::lround(from+(target-from)*(1-std::pow(1-t,3))));
        if(t>=1){scroll=target;moving=false;}return true;
    }
    int first() const{return scroll/stride;}
    int last(size_t count) const{return std::min(int(count),(scroll+height+stride-1)/stride);}
    int row_at(int y,size_t count) const {
        if(y<top || y>=top+height)return -1;
        int at=y-top+scroll,index=at/stride;
        return at%stride<row_height && index<int(count)?index:-1;
    }
    int thumb(size_t count) const{return std::max(54,height*height/std::max(height,int(count)*stride));}
    int thumb_top(size_t count) const{return top+(maximum(count)?scroll*(height-thumb(count))/maximum(count):0);}
    void seek(int y,int grab,size_t count){
        moving=false;
        scroll=std::clamp(int(std::lround((y-top-grab)*double(maximum(count))/std::max(1,height-thumb(count)))),0,maximum(count));
    }
};
}
