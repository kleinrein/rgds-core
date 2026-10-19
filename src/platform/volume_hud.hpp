#pragma once
#include "platform/hud_style.hpp"
#include <algorithm>
#include <cstdint>
#include <string>

namespace rgds {
// Small CPU canvas shared by SDL and OpenGL presentation. Premultiplied RGBA
// lets the same HUD render over games without creating a compositor window.
inline void draw_volume_hud(unsigned char* pixels,int level) {
    level=std::clamp(level,0,100);
    if(hud::volume(pixels,level))return;
    auto pixel=[&](int x,int y,int r,int g,int b,int a=255){
        y+=56; // Below the RG DS performance counter.
        auto* p=pixels+(y*640+x)*4;int inverse=255-a;
        p[0]=(r*a+p[0]*inverse+127)/255;p[1]=(g*a+p[1]*inverse+127)/255;
        p[2]=(b*a+p[2]*inverse+127)/255;p[3]=a+(p[3]*inverse+127)/255;
    };
    auto box=[&](int x,int y,int w,int h,int r,int g,int b,int a=255){for(int yy=y;yy<y+h;yy++)for(int xx=x;xx<x+w;xx++)pixel(xx,yy,r,g,b,a);};
    // RG DS Core palette: matte navy, quiet border and blue accent.
    for(int y=48;y<130;y++)for(int x=108;x<532;x++){
        int dx=std::max({116-x,0,x-523}),dy=std::max({56-y,0,y-121});
        if(dx*dx+dy*dy>64)continue;
        bool border=x==108 || x==531 || y==48 || y==129 || dx*dx+dy*dy>49;
        int shade=23;
        pixel(x,y,border?45:shade,border?67:32,border?90:46,245);
    }
    auto glyph=[](char c)->uint64_t{
        switch(c){
            case '0':return 0x1f11111111111full;case '1':return 0x040c040404040eull;
            case '2':return 0x1e01011e10101full;case '3':return 0x1e01010e01011eull;
            case '4':return 0x1111111f010101ull;case '5':return 0x1f10101e01011eull;
            case '6':return 0x0f10101e11111full;case '7':return 0x1f010204080808ull;
            case '8':return 0x1f11111f11111full;case '9':return 0x1f11111f01011eull;
            case '%':return 0x19190204081313ull;case 'V':return 0x11111111110a04ull;
            case 'O':return 0x0e11111111110eull;case 'L':return 0x1010101010101full;
            case 'U':return 0x1111111111110eull;case 'M':return 0x111b1515111111ull;
            case 'E':return 0x1f10101e10101full;case 'T':return 0x1f040404040404ull;
            case 'D':return 0x1e11111111111eull;default:return 0;
        }
    };
    auto text=[&](const std::string& value,int x,int y,int scale){
        for(char c:value){uint64_t bits=glyph(c);for(int row=0;row<7;row++)for(int col=0;col<5;col++)
            if((bits>>((6-row)*8))&(1<<(4-col)))box(x+col*scale,y+row*scale,scale,scale,233,244,255);
            x+=6*scale;
        }
    };
    box(128,74,10,17,43,113,168);
    for(int x=138;x<151;x++){int spread=(x-138)*3/4;box(x,74-spread,1,17+spread*2,43,113,168);}
    if(level){for(int y=73;y<92;y++)for(int x=159;x<172;x++){
        int dx=x-151,dy=y-82,d=dx*dx+dy*dy;if((d>=110 && d<=155)||(d>=300 && d<=360))pixel(x,y,43,113,168);
    }}else{for(int i=0;i<24;i++){pixel(129+i,69+i,167,73,83);pixel(130+i,69+i,167,73,83);}}
    text(level?"VOLUME":"MUTED",190,68,3);
    std::string percent=std::to_string(level)+"%";text(percent,510-int(percent.size())*18,68,3);
    box(128,106,384,10,45,67,90);
    int fill=384*level/100;
    for(int y=106;y<116;y++)box(128,y,fill,1,78,187,255);
    if(fill)box(128,106,fill,2,135,213,255);
}
// The bridge owns FF status as well as volume, so the two backends of our
// emulator never expose a second native on-screen display.
inline void draw_fast_forward_hud(unsigned char* pixels) {
    if(hud::fast_forward(pixels))return;
    auto put=[&](int x,int y,int r,int g,int b,int a=245) {
        auto* p=pixels+(y*640+x)*4;
        p[0]=(r*a+127)/255;p[1]=(g*a+127)/255;p[2]=(b*a+127)/255;p[3]=a;
    };
    for(int y=420;y<458;++y)for(int x=24;x<142;++x){
        int dx=std::max({30-x,0,x-135}),dy=std::max({426-y,0,y-451});
        if(dx*dx+dy*dy>36)continue;
        bool edge=dx*dx+dy*dy>25 || x==24 || x==141 || y==420 || y==457;
        put(x,y,edge?45:23,edge?67:32,edge?90:46);
    }
    for(int n=0;n<2;++n)for(int x=0;x<12;++x)for(int y=-10+x;y<=10-x;++y)
        put(40+n*14+x,439+y,78,187,255,255);
    // Two F glyphs, matching the menu's pixel labels.
    for(int n=0;n<2;++n)for(int y=0;y<21;++y)for(int x=0;x<15;++x)
        if(x<3 || y<3 || (y>=9 && y<12 && x<12))put(87+n*21+x,429+y,233,244,255,255);
}
}
