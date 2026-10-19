#pragma once
#include <SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace rgds {
// Newline and Stamp are PictoChat's: its Enter makes a second row of text, and a stamp key arms a stamp.
enum class KeyAction { Character, Shift, Caps, Tab, Backspace, Delete, Cancel, Left, Right, Space, Submit, Accents, Letters, Stamps, Stamp, Newline };
// What the keyboard is typing into. Chat adds stamps and Send in place of the cursor keys.
enum class KeyboardMode { Edit, Search, Chat };
struct KeyboardKey {SDL_Rect bounds;std::string label;KeyAction action;std::string alternate;};

// Upper case for the accented letters the keyboard offers; anything else is returned unchanged.
inline std::string keyboard_upper(const std::string& s){
    static const std::map<std::string,std::string> table={{"à","À"},{"á","Á"},{"â","Â"},{"ã","Ã"},{"ä","Ä"},{"å","Å"},{"æ","Æ"},{"ç","Ç"},{"è","È"},{"é","É"},
        {"ê","Ê"},{"ë","Ë"},{"ì","Ì"},{"í","Í"},{"î","Î"},{"ï","Ï"},{"ñ","Ñ"},{"ò","Ò"},{"ó","Ó"},{"ô","Ô"},{"õ","Õ"},{"ö","Ö"},{"ø","Ø"},
        {"ù","Ù"},{"ú","Ú"},{"û","Û"},{"ü","Ü"},{"ý","Ý"},{"þ","Þ"},{"ð","Ð"}};
    if(s.size()==1)return std::string(1,char(s[0]>='a' && s[0]<='z'?s[0]-32:s[0]));
    auto found=table.find(s);return found==table.end()?s:found->second;
}
inline std::vector<std::string> keyboard_split(const std::string& text){
    std::vector<std::string> out;
    for(size_t i=0;i<text.size();){size_t n=1;while(i+n<text.size() && (static_cast<unsigned char>(text[i+n])&0xc0)==0x80)n++;out.push_back(text.substr(i,n));i+=n;}
    return out;
}

// One keyboard for search, text fields and PictoChat: a laptop-style block that
// follows the original PictoChat's keys. Numbers and punctuation stay visible;
// drawing, touch and controller navigation all use these exact key bounds.
// `accents` swaps the letter keys for accented ones; `stamps` (chat only) shows
// the stamp palette instead. Both are one tap away and return to the letters.
// `top` is the first row's y: 186 under a text field, lower in a chat room.
// `scale_x` narrows the block (the chat room keeps Send and the arrows beside it).
inline void keyboard_scale(std::vector<std::vector<KeyboardKey>>& rows,float scale_x){
    if(scale_x==1)return;
    for(auto& row:rows)for(auto& key:row){key.bounds.x=int(std::lround(key.bounds.x*scale_x));key.bounds.w=int(std::lround(key.bounds.w*scale_x));}
}
inline std::vector<std::vector<KeyboardKey>> keyboard_layout(int layer,KeyboardMode mode,bool caps=false,bool accents=false,bool stamps=false,int top=186,float scale_x=1){
    const bool shift=layer==1,upper=shift!=caps,chat=mode==KeyboardMode::Chat;
    std::vector<std::vector<KeyboardKey>> rows;
    if(chat && stamps){
        rows.resize(3);
        for(int i=0;i<14;i++)rows[size_t(i/7)].push_back({{12+(i%7)*88,top+(i/7)*76,84,70},std::to_string(i),KeyAction::Stamp,""});
        rows[2]={{{12,top+156,124,46},"ABC",KeyAction::Letters,""},{{504,top+156,124,46},"Send",KeyAction::Submit,""}};
        keyboard_scale(rows,scale_x);return rows;
    }
    rows.resize(5);
    auto characters=[&](int row,int x,const std::vector<std::string>& plain,const std::vector<std::string>& shifted,bool letters){
        for(size_t i=0;i<plain.size();i++){
            // Letters follow Caps and Shift; punctuation and numbers follow Shift alone and show their other face above.
            const bool letter=letters && (plain[i].size()>1 || (plain[i][0]>='a' && plain[i][0]<='z'));
            const bool selected=letter?upper:shift;
            std::string label=selected?(letter?keyboard_upper(plain[i]):shifted[i]):plain[i];
            std::string alternate=letter?"":std::string(selected?plain[i]:shifted[i]);
            rows[size_t(row)].push_back({{x+int(i)*42,top+row*46,38,40},label,KeyAction::Character,alternate});
        }
    };
    auto simple=[&](const std::string& plain){return keyboard_split(plain);};
    characters(0,12,simple("`1234567890-="),simple("~!@#$%^&*()_+"),false);
    rows[0].push_back({{558,top,70,40},"Delete",KeyAction::Backspace,""});
    rows[1].push_back({{12,top+46,58,40},accents?"ABC":"àé",KeyAction::Accents,""});
    rows[2].push_back({{12,top+92,70,40},"Caps",KeyAction::Caps,""});
    if(accents){
        characters(1,74,simple("àáâãäåæçèéêëì"),simple("àáâãäåæçèéêëì"),true);
        characters(2,86,simple("íîïñòóôõöøß"),simple("íîïñòóôõöøß"),true);
    }else{
        characters(1,74,simple("qwertyuiop[]\\"),simple("QWERTYUIOP{}|"),true);
        characters(2,86,simple("asdfghjkl;'"),simple("ASDFGHJKL:\""),true);
    }
    rows[2].push_back({{550,top+92,78,40},"Enter",chat?KeyAction::Newline:KeyAction::Submit,""});
    rows[3].push_back({{12,top+138,88,40},"Shift",KeyAction::Shift,""});
    if(accents)characters(3,104,simple("ùúûüýÿþð¿¡"),simple("ùúûüýÿþð¿¡"),true);
    else characters(3,104,simple("zxcvbnm,./"),simple("ZXCVBNM<>?"),true);
    rows[3].push_back({{526,top+138,102,40},"Shift",KeyAction::Shift,""});
    const int last=top+184;
    if(chat)rows[4]={{{12,last,58,44},"",KeyAction::Stamps,""},
                     {{74,last,426,44},"Space",KeyAction::Space,""},
                     {{504,last,124,44},"Send",KeyAction::Submit,""}};
    else rows[4]={{{12,last,58,44},"Esc",KeyAction::Cancel,""},
                  {{74,last,58,44},"Del",KeyAction::Delete,""},
                  {{136,last,276,44},"Space",KeyAction::Space,""},
                  {{416,last,40,44},"",KeyAction::Left,""},
                  {{460,last,40,44},"",KeyAction::Right,""},
                  {{504,last,124,44},mode==KeyboardMode::Search?"Search":"Done",KeyAction::Submit,""}};
    keyboard_scale(rows,scale_x);return rows;
}
inline std::vector<std::vector<KeyboardKey>> keyboard_layout(int layer,bool search,bool caps=false){
    return keyboard_layout(layer,search?KeyboardMode::Search:KeyboardMode::Edit,caps);
}
inline int keyboard_nearest(const std::vector<KeyboardKey>& row,int x){
    int best=0,distance=10000;
    for(size_t i=0;i<row.size();i++){int d=std::abs(row[i].bounds.x+row[i].bounds.w/2-x);if(d<distance){best=int(i);distance=d;}}
    return best;
}
// Where the submit key sits, so Tab and the controller can jump straight to it.
inline std::pair<int,int> keyboard_submit(const std::vector<std::vector<KeyboardKey>>& rows){
    for(size_t r=rows.size();r-->0;)for(size_t c=0;c<rows[r].size();c++)if(rows[r][c].action==KeyAction::Submit)return {int(r),int(c)};
    return {0,0};
}
}
