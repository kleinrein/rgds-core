#pragma once
#include "launcher/model.hpp"
#include <algorithm>
#include <array>
#include <cmath>

namespace rgds {
// The sixteen "favourite colours" of the original DS system settings, in the
// firmware's order. Each can recolour the menus' accent; -1 keeps the theme's.
struct AccentColour { const char* name; Color rgb; };
inline constexpr std::array<AccentColour,16> ds_colours={{
    {"Grey",{97,130,154}},{"Brown",{186,73,0}},{"Red",{251,0,24}},{"Pink",{251,138,251}},
    {"Orange",{251,146,0}},{"Yellow",{243,227,0}},{"Lime",{170,251,0}},{"Green",{0,251,0}},
    {"Dark green",{0,162,56}},{"Sea green",{73,219,138}},{"Turquoise",{48,186,243}},{"Blue",{0,89,243}},
    {"Dark blue",{0,0,146}},{"Purple",{138,0,211}},{"Violet",{211,0,235}},{"Magenta",{251,0,146}}}};

inline double relative_luminance(Color c){
    auto linear=[](unsigned char value){double x=value/255.0;return x<=0.04045?x/12.92:std::pow((x+0.055)/1.055,2.4);};
    return 0.2126*linear(c.r)+0.7152*linear(c.g)+0.0722*linear(c.b);
}
inline double contrast_ratio(Color a,Color b){
    double x=relative_luminance(a)+0.05,y=relative_luminance(b)+0.05;return x>y?x/y:y/x;
}
inline Color mix_colour(Color a,Color b,double amount){
    auto channel=[&](unsigned char from,unsigned char to){return static_cast<unsigned char>(std::lround(from+(to-from)*amount));};
    return {channel(a.r,b.r),channel(a.g,b.g),channel(a.b,b.b)};
}
// The accent also colours text (values, links, labels). Darken (light mode)
// or lighten (dark mode) the hue only as far as needed for `ratio` against the
// background, cards and the focused-row tint: 4.5:1 is WCAG AA for text.
inline Color readable_accent(Color base,const Theme& theme,bool dark,double ratio=4.5){
    const Color toward=dark?Color{255,255,255}:Color{0,0,0};
    auto enough=[&](Color colour){
        return contrast_ratio(colour,theme.at("background"))>=ratio && contrast_ratio(colour,theme.at("panel"))>=ratio;
    };
    Color accent=base;
    for(int step=0;step<40 && !enough(accent);step++)accent=mix_colour(accent,toward,0.05);
    return accent;
}
// Focus tint and on-accent ink that keep both the accent and body text readable.
inline void settle_accent(Theme& theme,bool dark,double ratio){
    const Color white{255,255,255},ink{18,24,30};
    const Color accent=theme.at("accent");
    theme["on_accent"]=contrast_ratio(white,accent)>=contrast_ratio(ink,accent)?white:ink;
    double tint=dark?0.24:0.14;Color focus=mix_colour(theme.at("background"),accent,tint);
    while(tint>0.02 && (contrast_ratio(theme.at("ink"),focus)<ratio || contrast_ratio(accent,focus)<ratio)){tint-=0.02;focus=mix_colour(theme.at("background"),accent,tint);}
    theme["focus"]=focus;
}
inline void apply_accent(Theme& theme,int index,bool dark){
    if(index<0 || index>=int(ds_colours.size()))return;
    theme["accent"]=readable_accent(ds_colours[size_t(index)].rgb,theme,dark);
    settle_accent(theme,dark,4.5);
}
// High contrast: near-black/white text, strong component edges (3:1+) and
// accents at 7:1 (WCAG AAA) on every surface.
inline void apply_high_contrast(Theme& theme,bool dark){
    if(dark){theme["background"]={0,0,0};theme["panel"]={16,20,26};theme["slot"]={30,36,44};theme["ink"]={255,255,255};theme["muted"]={214,224,234};theme["line"]={150,166,182};}
    else{theme["background"]={255,255,255};theme["panel"]={255,255,255};theme["slot"]={214,220,226};theme["ink"]={0,0,0};theme["muted"]={38,48,58};theme["line"]={84,98,112};}
    theme["accent"]=readable_accent(theme.at("accent"),theme,dark,7.0);
    settle_accent(theme,dark,7.0);
}
}
