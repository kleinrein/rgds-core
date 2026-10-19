#pragma once
// RG DS Core's own chat pad: what one message is, how it travels, how it looks.
// No SDL, no I/O, nothing from Nintendo: the pen, text and stamp model is ours.
// A message is a short list of drawing operations, replayed identically on
// every handheld. Anything arriving from a network is untrusted: decode() is
// strict, bounded and never reads past its input.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace rgds::pictochat {

constexpr int canvas_w=480,canvas_h=96;           // one message: two lines of 48 px
constexpr size_t max_ops=400,max_points=6000,max_text=64,max_payload=8192;
constexpr int stamp_count=14,stamp_size=36;

struct Point {int16_t x=0,y=0;bool operator==(const Point& o)const{return x==o.x && y==o.y;}};
enum class Kind:uint8_t {Stroke=1,Text=2,Stamp=3};
// Colours 0..7 are inks; 8 is the rainbow pen, a hue that follows the stroke.
constexpr int ink_count=8,rainbow=8;
struct Op {
    Kind kind=Kind::Stroke;
    bool eraser=false,thick=false;                 // strokes
    uint8_t colour=0;                              // ink index, or rainbow
    uint8_t stamp=0;                               // stamps
    std::vector<Point> points;                     // strokes: the path; text and stamps: points[0] is the origin
    std::string text;                              // UTF-8, text only
};

constexpr uint32_t pack(unsigned r,unsigned g,unsigned b,unsigned a=255){return r|(g<<8)|(b<<16)|(a<<24);}
constexpr uint32_t paper=pack(255,255,255);
// Faint rules, like the lined paper of the original: one under each of the two rows. Erasing restores them.
constexpr uint32_t rule=pack(212,224,238);
constexpr int rule_row0=47,rule_row1=95;
constexpr std::array<uint32_t,ink_count> inks={pack(27,31,39),pack(222,52,52),pack(240,140,30),pack(226,190,20),
                                                pack(46,160,80),pack(35,110,210),pack(130,70,200),pack(224,80,160)};

// ---- encoding ----------------------------------------------------------------
namespace detail {
inline void put_varint(std::vector<uint8_t>& out,uint32_t v){while(v>=0x80){out.push_back(uint8_t(v|0x80));v>>=7;}out.push_back(uint8_t(v));}
inline uint32_t zig(int32_t v){return (uint32_t(v)<<1)^uint32_t(v>>31);}
inline int32_t unzig(uint32_t v){return int32_t(v>>1)^-int32_t(v&1);}
struct Reader {
    const uint8_t* data;size_t size,at=0;bool ok=true;
    uint8_t byte(){if(at>=size){ok=false;return 0;}return data[at++];}
    uint32_t varint(){
        uint32_t value=0;
        for(int shift=0;shift<=28;shift+=7){
            uint8_t b=byte();if(!ok)return 0;
            if(shift==28 && b>0x0f){ok=false;return 0;}
            value|=uint32_t(b&0x7f)<<shift;
            if(!(b&0x80))return value;
        }
        ok=false;return 0;
    }
    int32_t zigzag(){return unzig(varint());}
};
inline bool in_range(int32_t x,int32_t y){return x>=-64 && x<=canvas_w+64 && y>=-64 && y<=canvas_h+64;}
}

// Printable UTF-8 only: no overlong forms, surrogates, controls or values above U+10FFFF.
inline bool valid_text(std::string_view text){
    for(size_t i=0;i<text.size();){
        unsigned char c=text[i];uint32_t cp;int extra;
        if(c<0x80){cp=c;extra=0;}
        else if(c>=0xc2 && c<=0xdf){cp=c&0x1f;extra=1;}
        else if(c>=0xe0 && c<=0xef){cp=c&0x0f;extra=2;}
        else if(c>=0xf0 && c<=0xf4){cp=c&0x07;extra=3;}
        else return false;
        for(int k=1;k<=extra;k++){
            if(i+k>=text.size())return false;
            unsigned char d=text[i+k];if((d&0xc0)!=0x80)return false;cp=(cp<<6)|(d&0x3f);
        }
        if((extra==2 && cp<0x800) || (extra==3 && (cp<0x10000 || cp>0x10ffff)) || (cp>=0xd800 && cp<=0xdfff))return false;
        if(cp<0x20 || cp==0x7f || (cp>=0x80 && cp<0xa0))return false;
        i+=size_t(extra)+1;
    }
    return true;
}

// Canonical bytes for a message, or empty if it breaks a limit.
inline std::vector<uint8_t> encode(const std::vector<Op>& ops){
    std::vector<uint8_t> out;size_t points=0;
    if(ops.empty() || ops.size()>max_ops)return {};
    out.push_back(1);detail::put_varint(out,uint32_t(ops.size()));
    for(const auto& op:ops){
        if(op.points.empty() || (op.kind!=Kind::Stroke && op.points.size()!=1))return {};
        points+=op.points.size();if(points>max_points)return {};
        out.push_back(uint8_t(op.kind));
        if(op.kind==Kind::Stroke){
            if(op.colour>rainbow)return {};
            out.push_back(uint8_t((op.eraser?1:0)|(op.thick?2:0)|(op.colour<<2)));
            detail::put_varint(out,uint32_t(op.points.size()));
            int px=0,py=0;
            for(size_t i=0;i<op.points.size();i++){
                int dx=op.points[i].x-(i?px:0),dy=op.points[i].y-(i?py:0);
                detail::put_varint(out,detail::zig(dx));detail::put_varint(out,detail::zig(dy));
                px=op.points[i].x;py=op.points[i].y;
            }
        }else if(op.kind==Kind::Text){
            if(op.colour>=ink_count || op.text.empty() || op.text.size()>max_text || !valid_text(op.text))return {};
            out.push_back(op.colour);
            detail::put_varint(out,detail::zig(op.points[0].x));detail::put_varint(out,detail::zig(op.points[0].y));
            detail::put_varint(out,uint32_t(op.text.size()));out.insert(out.end(),op.text.begin(),op.text.end());
        }else{
            if(op.colour>=ink_count || op.stamp>=stamp_count)return {};
            out.push_back(op.stamp);out.push_back(op.colour);
            detail::put_varint(out,detail::zig(op.points[0].x));detail::put_varint(out,detail::zig(op.points[0].y));
        }
        if(out.size()>max_payload)return {};
    }
    return out;
}

// Strict inverse of encode(). False for anything malformed, oversize or out of range.
inline bool decode(const uint8_t* data,size_t size,std::vector<Op>& ops){
    ops.clear();
    if(!data || size==0 || size>max_payload)return false;
    detail::Reader in{data,size};
    if(in.byte()!=1)return false;
    uint32_t count=in.varint();
    if(!in.ok || count==0 || count>max_ops)return false;
    size_t points=0;
    for(uint32_t n=0;n<count;n++){
        Op op;uint8_t kind=in.byte();if(!in.ok)return false;
        if(kind==uint8_t(Kind::Stroke)){
            op.kind=Kind::Stroke;uint8_t style=in.byte();
            op.eraser=style&1;op.thick=style&2;op.colour=style>>2;
            if(!in.ok || op.colour>rainbow || (op.eraser && op.colour))return false;
            uint32_t length=in.varint();
            if(!in.ok || length==0 || length>max_points-points)return false;
            points+=length;int32_t x=0,y=0;
            op.points.reserve(length);
            for(uint32_t i=0;i<length;i++){
                int32_t dx=in.zigzag(),dy=in.zigzag();if(!in.ok)return false;
                if(i==0){x=dx;y=dy;}else{
                    if(dx<-(canvas_w+128) || dx>canvas_w+128 || dy<-(canvas_h+128) || dy>canvas_h+128)return false;
                    x+=dx;y+=dy;
                }
                if(!detail::in_range(x,y))return false;
                op.points.push_back({int16_t(x),int16_t(y)});
            }
        }else if(kind==uint8_t(Kind::Text)){
            op.kind=Kind::Text;op.colour=in.byte();int32_t x=in.zigzag(),y=in.zigzag();uint32_t length=in.varint();
            if(!in.ok || op.colour>=ink_count || !detail::in_range(x,y) || length==0 || length>max_text || length>size-in.at)return false;
            op.text.assign(reinterpret_cast<const char*>(data+in.at),length);in.at+=length;
            if(!valid_text(op.text))return false;
            points+=1;op.points.push_back({int16_t(x),int16_t(y)});
        }else if(kind==uint8_t(Kind::Stamp)){
            op.kind=Kind::Stamp;op.stamp=in.byte();op.colour=in.byte();int32_t x=in.zigzag(),y=in.zigzag();
            if(!in.ok || op.stamp>=stamp_count || op.colour>=ink_count || !detail::in_range(x,y))return false;
            points+=1;op.points.push_back({int16_t(x),int16_t(y)});
        }else return false;
        ops.push_back(std::move(op));
    }
    if(in.at!=size){ops.clear();return false;}
    return true;
}

// Ramer-Douglas-Peucker: keeps the look of a hand-drawn path with far fewer points.
inline std::vector<Point> simplify(const std::vector<Point>& path,float tolerance){
    if(path.size()<3)return path;
    std::vector<char> keep(path.size(),0);keep.front()=keep.back()=1;
    std::vector<std::pair<size_t,size_t>> work{{0,path.size()-1}};
    while(!work.empty()){
        auto [a,b]=work.back();work.pop_back();
        float ax=path[a].x,ay=path[a].y,bx=path[b].x,by=path[b].y,ex=bx-ax,ey=by-ay,length=std::sqrt(ex*ex+ey*ey);
        float far=-1;size_t at=a;
        for(size_t i=a+1;i<b;i++){
            float d=length<1e-6f?std::hypot(path[i].x-ax,path[i].y-ay):std::fabs(ex*(ay-path[i].y)-(ax-path[i].x)*ey)/length;
            if(d>far){far=d;at=i;}
        }
        if(far>tolerance){keep[at]=1;work.push_back({a,at});work.push_back({at,b});}
    }
    std::vector<Point> out;for(size_t i=0;i<path.size();i++)if(keep[i])out.push_back(path[i]);
    return out;
}

// ---- drawing -----------------------------------------------------------------
struct Rect {
    int x0=0,y0=0,x1=0,y1=0;
    bool empty()const{return x1<=x0 || y1<=y0;}
    void add(const Rect& o){if(o.empty())return;if(empty()){*this=o;return;}x0=std::min(x0,o.x0);y0=std::min(y0,o.y0);x1=std::max(x1,o.x1);y1=std::max(y1,o.y1);}
};

// Glyph supply for typed text. The launcher backs it with its bundled font; the
// callback receives an alpha for each pixel of the text, origin at (x,y).
struct GlyphSource {
    virtual ~GlyphSource()=default;
    virtual void draw(const std::string& utf8,int x,int y,const std::function<void(int,int,uint8_t)>& put)=0;
    virtual int width(const std::string& utf8)=0;
};

inline uint32_t hue_colour(float degrees){
    degrees=std::fmod(degrees,360.f);if(degrees<0)degrees+=360.f;
    float h=degrees/60.f,x=1-std::fabs(std::fmod(h,2.f)-1),r=0,g=0,b=0;
    if(h<1){r=1;g=x;}else if(h<2){r=x;g=1;}else if(h<3){g=1;b=x;}else if(h<4){g=x;b=1;}else if(h<5){r=x;b=1;}else{r=1;b=x;}
    auto level=[](float v){return unsigned(std::lround((.12f+.88f*v)*215.f));};   // a little deeper than pure hues, readable on white
    return pack(level(r),level(g),level(b));
}

namespace detail {
inline float sd_disc(float px,float py,float cx,float cy,float r){return std::hypot(px-cx,py-cy)-r;}
inline float sd_capsule(float px,float py,float ax,float ay,float bx,float by,float r){
    float ex=bx-ax,ey=by-ay,len2=ex*ex+ey*ey,t=len2<1e-9f?0.f:std::clamp(((px-ax)*ex+(py-ay)*ey)/len2,0.f,1.f);
    return std::hypot(px-(ax+t*ex),py-(ay+t*ey))-r;
}
inline float sd_poly(float px,float py,const std::vector<std::pair<float,float>>& v){
    float d=1e9f;bool inside=false;
    for(size_t i=0,j=v.size()-1;i<v.size();j=i++){
        d=std::min(d,sd_capsule(px,py,v[j].first,v[j].second,v[i].first,v[i].second,0));
        if(((v[i].second>py)!=(v[j].second>py)) && (px<(v[j].first-v[i].first)*(py-v[i].second)/(v[j].second-v[i].second)+v[i].first))inside=!inside;
    }
    return inside?-d:d;
}
}

// One shape per stamp, in a -1..1 box. Original drawings built from discs,
// capsules and polygons, so they stay clean at any size and need no bitmap.
inline float stamp_distance(int id,float x,float y){
    using namespace detail;
    auto uni=[](float a,float b){return std::min(a,b);};
    auto cut=[](float a,float b){return std::max(a,-b);};
    switch(id){
    case 0:return uni(uni(sd_disc(x,y,-.48f,-.32f,.52f),sd_disc(x,y,.48f,-.32f,.52f)),sd_poly(x,y,{{-.96f,-.12f},{.96f,-.12f},{0,.92f}}));   // heart
    case 1:{std::vector<std::pair<float,float>> s;for(int i=0;i<10;i++){float a=-1.5707963f+i*.62831853f,r=i%2?.42f:1.f;s.push_back({r*std::cos(a),r*std::sin(a)+.06f});}return sd_poly(x,y,s);}   // star
    case 2:return uni(uni(sd_disc(x,y,-.4f,.62f,.34f),sd_capsule(x,y,-.08f,.62f,-.08f,-.86f,.09f)),sd_capsule(x,y,-.08f,-.86f,.62f,-.5f,.1f));   // note
    case 3:case 4:{   // smile, frown
        float face=sd_disc(x,y,0,0,.94f),eyes=uni(sd_disc(x,y,-.32f,-.26f,.12f),sd_disc(x,y,.32f,-.26f,.12f)),mouth=1e9f;
        float sign=id==3?1.f:-1.f,cy=id==3?.2f:.62f;
        for(int i=0;i<8;i++){float a=-.9f+i*.2571f,b=a+.2571f;mouth=uni(mouth,sd_capsule(x,y,.5f*std::sin(a*1.5f),cy+sign*.28f*std::cos(a*1.5f)-sign*.0f,.5f*std::sin(b*1.5f),cy+sign*.28f*std::cos(b*1.5f),.07f));}
        return cut(cut(face,eyes),mouth);
    }
    case 5:{float d=sd_disc(x,y,0,0,.4f);for(int i=0;i<8;i++){float a=i*.78539816f;d=uni(d,sd_capsule(x,y,.62f*std::cos(a),.62f*std::sin(a),.92f*std::cos(a),.92f*std::sin(a),.07f));}return d;}   // sun
    case 6:return cut(sd_disc(x,y,0,0,.9f),sd_disc(x,y,.42f,-.28f,.78f));   // moon
    case 7:return uni(uni(sd_disc(x,y,-.42f,.18f,.4f),sd_disc(x,y,.1f,-.14f,.5f)),uni(sd_disc(x,y,.5f,.2f,.36f),sd_poly(x,y,{{-.42f,.58f},{.5f,.56f},{.5f,.2f},{-.42f,.2f}})));   // cloud
    case 8:{float d=sd_disc(x,y,0,0,.3f);for(int i=0;i<5;i++){float a=-1.5707963f+i*1.2566371f;d=uni(d,sd_disc(x,y,.58f*std::cos(a),.58f*std::sin(a),.34f));}return d;}   // flower
    case 9:return uni(sd_disc(x,y,0,.3f,.6f),sd_poly(x,y,{{0,-.98f},{-.5f,.04f},{.5f,.04f}}));   // drop
    case 10:return sd_poly(x,y,{{.3f,-.98f},{-.62f,.14f},{-.04f,.14f},{-.3f,.98f},{.62f,-.16f},{.04f,-.16f}});   // bolt
    case 11:return uni(sd_capsule(x,y,-.7f,.04f,-.24f,.56f,.14f),sd_capsule(x,y,-.24f,.56f,.74f,-.52f,.14f));   // tick
    case 12:return uni(sd_capsule(x,y,-.62f,-.62f,.62f,.62f,.14f),sd_capsule(x,y,.62f,-.62f,-.62f,.62f,.14f));   // cross
    default:return uni(sd_capsule(x,y,0,.9f,0,-.55f,.15f),sd_poly(x,y,{{0,-1.f},{-.62f,-.3f},{.62f,-.3f}}));   // up arrow
    }
}

// Interface icons, in the same -1..1 box: 0 pen, 1 eraser, 2 bin, 3 undo, 4 send, 5 up, 6 down,
// 7 person, 8 Wi-Fi bars, 9-12 the floor plans of rooms A to D, 13 the stamps key (a smile).
constexpr int ui_icon_count=14;
inline float icon_distance(int id,float x,float y){
    using namespace detail;
    auto uni=[](float a,float b){return std::min(a,b);};
    auto cut=[](float a,float b){return std::max(a,-b);};
    auto box=[](float px,float py,float cx,float cy,float hx,float hy){float qx=std::fabs(px-cx)-hx,qy=std::fabs(py-cy)-hy;return std::hypot(std::max(qx,0.f),std::max(qy,0.f))+std::min(std::max(qx,qy),0.f);};
    auto outline=[](float d){return std::fabs(d)-.095f;};
    const float k=.70710678f;
    const float along=(-x+y)*k,across=(x+y)*k;      // a frame turned 45 degrees: the tip points to the lower left
    switch(id){
    case 0:{   // pencil: body, rings where the ferrule is, a sharpened tip
        float body=uni(box(across,along,0,-.28f,.27f,.66f),sd_poly(across,along,{{-.27f,.38f},{.27f,.38f},{0,.98f}}));
        return cut(body,uni(box(across,along,0,-.66f,.4f,.05f),box(across,along,0,.4f,.4f,.035f)));
    }
    case 1:{   // eraser: a block with a band across it
        float block=box(across,along,0,0,.43f,.78f)-.1f;
        return cut(block,box(across,along,0,.12f,.6f,.045f));
    }
    case 2:{float tub=sd_poly(x,y,{{-.62f,-.3f},{.62f,-.3f},{.5f,.92f},{-.5f,.92f}});float slits=uni(sd_capsule(x,y,-.2f,-.05f,-.16f,.7f,.05f),sd_capsule(x,y,.2f,-.05f,.16f,.7f,.05f));
            return uni(uni(cut(tub,slits),sd_poly(x,y,{{-.86f,-.62f},{.86f,-.62f},{.86f,-.4f},{-.86f,-.4f}})),sd_capsule(x,y,-.22f,-.8f,.22f,-.8f,.08f));}
    case 3:{float d=1e9f;for(int i=0;i<10;i++){float a=3.1f-i*.37f,b=a-.37f;d=uni(d,sd_capsule(x,y,.6f*std::cos(a)+.05f,.6f*-std::sin(a)+.12f,.6f*std::cos(b)+.05f,.6f*-std::sin(b)+.12f,.13f));}
            return uni(d,sd_poly(x,y,{{-.98f,-.14f},{-.2f,-.14f},{-.6f,-.82f}}));}
    case 4:return uni(sd_poly(x,y,{{0,-.98f},{-.82f,.0f},{-.3f,.0f},{-.3f,.55f},{.3f,.55f},{.3f,.0f},{.82f,.0f}}),sd_capsule(x,y,-.7f,.9f,.7f,.9f,.1f));   // an arrow lifting out of a tray
    case 5:return sd_poly(x,y,{{0,-.7f},{-.85f,.6f},{.85f,.6f}})-.06f;       // solid triangles, like the original's scroll arrows
    case 6:return sd_poly(x,y,{{0,.7f},{-.85f,-.6f},{.85f,-.6f}})-.06f;
    case 7:return uni(sd_disc(x,y,0,-.4f,.38f),sd_poly(x,y,{{-.82f,.95f},{-.82f,.62f},{-.5f,.14f},{.5f,.14f},{.82f,.62f},{.82f,.95f}})-.04f);
    case 8:{float d=1e9f;for(int i=0;i<4;i++)d=uni(d,sd_capsule(x,y,-.75f+i*.5f,.82f,-.75f+i*.5f,.82f-(.4f+.45f*i),.17f));return d;}
    case 9:return cut(outline(box(x,y,0,0,.85f,.66f)),box(x,y,0,.66f,.24f,.22f));                       // one room with a door
    case 10:return uni(outline(box(x,y,-.46f,0,.46f,.66f)),outline(box(x,y,.46f,0,.46f,.66f)));          // two rooms side by side
    case 11:return outline(uni(box(x,y,0,0,.9f,.3f),box(x,y,0,0,.3f,.9f)));                              // a cross
    case 12:return outline(uni(box(x,y,-.3f,0,.5f,.82f),box(x,y,.4f,.4f,.5f,.42f)));                    // an L
    default:return stamp_distance(3,x,y);
    }
}

class Canvas {
public:
    std::vector<uint32_t> px;
    Canvas():px(size_t(canvas_w)*canvas_h,paper){clear();}
    static uint32_t background(int,int y){return y==rule_row0 || y==rule_row1?rule:paper;}
    void clear(){for(int y=0;y<canvas_h;y++)std::fill(px.begin()+size_t(y)*canvas_w,px.begin()+size_t(y+1)*canvas_w,background(0,y));}
    uint32_t at(int x,int y)const{return px[size_t(y)*canvas_w+x];}
    static uint32_t mix(uint32_t dst,uint32_t src,unsigned coverage){   // coverage 0..255
        if(coverage==0)return dst;if(coverage>=255)return src|0xff000000u;
        auto channel=[&](int shift){int d=(dst>>shift)&255,s=(src>>shift)&255;return uint32_t(d+((s-d)*int(coverage)+127)/255)&255;};
        return channel(0)|(channel(8)<<8)|(channel(16)<<16)|0xff000000u;
    }
    // A round-ended line of the given radius; the dirty rectangle comes back.
    // `erase` paints the paper back (its rules included) instead of a colour.
    Rect capsule(float ax,float ay,float bx,float by,float radius,uint32_t colour,bool erase=false){
        int x0=std::max(0,int(std::floor(std::min(ax,bx)-radius-1))),x1=std::min(canvas_w,int(std::ceil(std::max(ax,bx)+radius+1))+1);
        int y0=std::max(0,int(std::floor(std::min(ay,by)-radius-1))),y1=std::min(canvas_h,int(std::ceil(std::max(ay,by)+radius+1))+1);
        if(x1<=x0 || y1<=y0)return {};
        float ex=bx-ax,ey=by-ay,len2=ex*ex+ey*ey,inner=std::max(0.f,radius-.5f),outer=radius+.5f;
        for(int y=y0;y<y1;y++)for(int x=x0;x<x1;x++){
            float qx=x+.5f,qy=y+.5f,t=len2<1e-9f?0.f:std::clamp(((qx-ax)*ex+(qy-ay)*ey)/len2,0.f,1.f);
            float dx=qx-(ax+t*ex),dy=qy-(ay+t*ey),d2=dx*dx+dy*dy;
            if(d2>=outer*outer)continue;
            unsigned cover=d2<=inner*inner?255u:unsigned(std::clamp((outer-std::sqrt(d2))*255.f,0.f,255.f));
            auto& p=px[size_t(y)*canvas_w+x];p=mix(p,erase?background(x,y):colour,cover);
        }
        return {x0,y0,x1,y1};
    }
    Rect stamp(int id,float cx,float cy,uint32_t colour){
        const float half=stamp_size/2.f;
        int x0=std::max(0,int(cx-half-1)),x1=std::min(canvas_w,int(cx+half+2)),y0=std::max(0,int(cy-half-1)),y1=std::min(canvas_h,int(cy+half+2));
        if(x1<=x0 || y1<=y0)return {};
        for(int y=y0;y<y1;y++)for(int x=x0;x<x1;x++){
            float d=stamp_distance(id,(x+.5f-cx)/half,(y+.5f-cy)/half)*half;   // back to pixels
            unsigned cover=unsigned(std::clamp((.5f-d)*255.f,0.f,255.f));
            if(cover){auto& p=px[size_t(y)*canvas_w+x];p=mix(p,colour,cover);}
        }
        return {x0,y0,x1,y1};
    }
    Rect text(const std::string& utf8,int x,int y,uint32_t colour,GlyphSource& glyphs){
        Rect dirty;
        glyphs.draw(utf8,x,y,[&](int px_,int py_,uint8_t alpha){
            if(px_<0 || py_<0 || px_>=canvas_w || py_>=canvas_h || !alpha)return;
            auto& p=px[size_t(py_)*canvas_w+px_];p=mix(p,colour,alpha);
            dirty.add({px_,py_,px_+1,py_+1});
        });
        return dirty;
    }
};

// Paints a stroke point by point, so the pad can draw while the finger moves and
// a received message replays to exactly the same pixels.
class StrokePainter {
    Canvas& canvas;bool eraser,thick;uint8_t colour;float travelled=0;Point last{};bool started=false;
public:
    StrokePainter(Canvas& c,const Op& style):canvas(c),eraser(style.eraser),thick(style.thick),colour(style.colour){}
    static float radius(bool eraser,bool thick){return eraser?(thick?16.f:8.f):(thick?4.6f:2.1f);}
    Rect add(Point p){
        float r=radius(eraser,thick);
        auto colour_at=[&](float distance){return eraser?paper:colour==rainbow?hue_colour(distance*2.2f):inks[colour%ink_count];};
        Rect dirty;
        if(!started){started=true;dirty=canvas.capsule(p.x,p.y,p.x,p.y,r,colour_at(0),eraser);}
        else{
            float d=std::hypot(float(p.x-last.x),float(p.y-last.y));
            dirty=canvas.capsule(last.x,last.y,p.x,p.y,r,colour_at(travelled+d*.5f),eraser);travelled+=d;
        }
        last=p;return dirty;
    }
};

inline Rect paint(Canvas& canvas,const Op& op,GlyphSource* glyphs){
    Rect dirty;
    if(op.kind==Kind::Stroke){StrokePainter painter(canvas,op);for(const auto& p:op.points)dirty.add(painter.add(p));}
    else if(op.kind==Kind::Text && glyphs)dirty=canvas.text(op.text,op.points[0].x,op.points[0].y,inks[op.colour%ink_count],*glyphs);
    else if(op.kind==Kind::Stamp)dirty=canvas.stamp(op.stamp,op.points[0].x,op.points[0].y,inks[op.colour%ink_count]);
    return dirty;
}
inline void render(Canvas& canvas,const std::vector<Op>& ops,GlyphSource* glyphs){canvas.clear();for(const auto& op:ops)paint(canvas,op,glyphs);}

}
