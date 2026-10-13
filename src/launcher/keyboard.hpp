#pragma once
#include <SDL.h>
#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

namespace rgds {
enum class KeyAction { Character, Shift, Caps, Tab, Backspace, Delete, Cancel, Left, Right, Space, Submit };
struct KeyboardKey {SDL_Rect bounds;std::string label;KeyAction action;std::string alternate;};
// A complete laptop-style typing block. Numbers and punctuation stay visible;
// drawing, touch and controller navigation all use these exact key bounds.
inline std::vector<std::vector<KeyboardKey>> keyboard_layout(int layer,bool search,bool caps=false){
    std::vector<std::vector<KeyboardKey>> rows(5);
    bool shift=layer==1,upper=shift!=caps;
    auto characters=[&](int row,int x,const std::string& plain,const std::string& shifted,bool letters=false){
        for(size_t i=0;i<plain.size();i++){
            bool letter=letters && plain[i]>='a' && plain[i]<='z';
            bool selected=letter?upper:shift;
            std::string alternate=letter?"":std::string(1,selected?plain[i]:shifted[i]);
            rows[row].push_back({{x+int(i)*42,186+row*46,38,40},std::string(1,selected?shifted[i]:plain[i]),KeyAction::Character,alternate});
        }
    };
    characters(0,12,"`1234567890-=","~!@#$%^&*()_+");
    rows[0].push_back({{558,186,70,40},"Delete",KeyAction::Backspace,""});
    rows[1].push_back({{12,232,58,40},"Tab",KeyAction::Tab,""});
    characters(1,74,"qwertyuiop[]\\","QWERTYUIOP{}|",true);
    rows[2].push_back({{12,278,70,40},"Caps",KeyAction::Caps,""});
    characters(2,86,"asdfghjkl;'","ASDFGHJKL:\"",true);
    rows[2].push_back({{550,278,78,40},"Enter",KeyAction::Submit,""});
    rows[3].push_back({{12,324,88,40},"Shift",KeyAction::Shift,""});
    characters(3,104,"zxcvbnm,./","ZXCVBNM<>?",true);
    rows[3].push_back({{526,324,102,40},"Shift",KeyAction::Shift,""});
    rows[4]={{{12,370,58,44},"Esc",KeyAction::Cancel,""},
             {{74,370,58,44},"Del",KeyAction::Delete,""},
             {{136,370,276,44},"Space",KeyAction::Space,""},
             {{416,370,40,44},"",KeyAction::Left,""},
             {{460,370,40,44},"",KeyAction::Right,""},
             {{504,370,124,44},search?"Search":"Done",KeyAction::Submit,""}};
    return rows;
}
inline int keyboard_nearest(const std::vector<KeyboardKey>& row,int x){
    int best=0,distance=10000;
    for(size_t i=0;i<row.size();i++){int d=std::abs(row[i].bounds.x+row[i].bounds.w/2-x);if(d<distance){best=int(i);distance=d;}}
    return best;
}
}
