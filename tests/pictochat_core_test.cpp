// Built with -fsanitize=address,undefined by tests/test_pictochat_core.py.
#include "launcher/pictochat_core.hpp"
#include <cassert>
#include <cstdio>
#include <random>

using namespace rgds::pictochat;

static std::mt19937 rng(12345);
static int pick(int lo,int hi){return std::uniform_int_distribution<int>(lo,hi)(rng);}

// Boxes 10 px wide and 16 high per ASCII character: enough to exercise clipping.
struct BoxGlyphs:GlyphSource {
    void draw(const std::string& t,int x,int y,const std::function<void(int,int,uint8_t)>& put)override{
        int pen=x;for(size_t i=0;i<t.size();i++){if((t[i]&0xc0)==0x80)continue;for(int dy=0;dy<16;dy++)for(int dx=0;dx<8;dx++)put(pen+dx,y+dy,200);pen+=10;}
    }
    int width(const std::string& t)override{int n=0;for(char c:t)if((c&0xc0)!=0x80)n++;return n*10;}
};

static Op random_stroke(){
    Op op;op.kind=Kind::Stroke;op.eraser=pick(0,3)==0;op.thick=pick(0,1);op.colour=op.eraser?0:uint8_t(pick(0,rainbow));
    int n=pick(1,60),x=pick(0,canvas_w-1),y=pick(0,canvas_h-1);
    for(int i=0;i<n;i++){x=std::clamp(x+pick(-20,20),-30,canvas_w+30);y=std::clamp(y+pick(-12,12),-30,canvas_h+30);op.points.push_back({int16_t(x),int16_t(y)});}
    return op;
}
static std::vector<Op> random_message(){
    std::vector<Op> ops;int n=pick(1,12);
    for(int i=0;i<n;i++){
        int kind=pick(0,5);
        if(kind<=3)ops.push_back(random_stroke());
        else if(kind==4){Op t;t.kind=Kind::Text;t.colour=uint8_t(pick(0,ink_count-1));t.text=pick(0,1)?"Hello PictoChat":"Blåbærsyltetøy ♥ 日本";t.points={{int16_t(pick(0,300)),int16_t(pick(0,60))}};ops.push_back(t);}
        else{Op s;s.kind=Kind::Stamp;s.stamp=uint8_t(pick(0,stamp_count-1));s.colour=uint8_t(pick(0,ink_count-1));s.points={{int16_t(pick(0,canvas_w)),int16_t(pick(0,canvas_h))}};ops.push_back(s);}
    }
    return ops;
}
static bool same(const std::vector<Op>& a,const std::vector<Op>& b){
    if(a.size()!=b.size())return false;
    for(size_t i=0;i<a.size();i++){
        const auto &x=a[i],&y=b[i];
        if(x.kind!=y.kind || x.eraser!=y.eraser || x.thick!=y.thick || x.colour!=y.colour || x.stamp!=y.stamp || x.text!=y.text || x.points!=y.points)return false;
    }
    return true;
}

int main(){
    // 1. Round trip.
    for(int i=0;i<500;i++){
        auto ops=random_message();auto bytes=encode(ops);
        if(bytes.empty())continue;
        std::vector<Op> back;assert(decode(bytes.data(),bytes.size(),back));assert(same(ops,back));
    }
    // 2. Every strict rejection.
    {
        Op s;s.points={{10,10},{20,20}};auto good=encode({s});assert(!good.empty());
        std::vector<Op> out;assert(decode(good.data(),good.size(),out));
        for(size_t n=0;n<good.size();n++)assert(!decode(good.data(),n,out));          // every truncation
        auto longer=good;longer.push_back(0);assert(!decode(longer.data(),longer.size(),out));   // trailing byte
        auto wrong=good;wrong[0]=2;assert(!decode(wrong.data(),wrong.size(),out));         // version
        assert(!decode(nullptr,0,out));
        std::vector<uint8_t> huge(max_payload+1,1);assert(!decode(huge.data(),huge.size(),out));
        std::vector<uint8_t> zero{1,0};assert(!decode(zero.data(),zero.size(),out));        // no ops
        std::vector<uint8_t> many{1,0xff,0xff,0x7f};assert(!decode(many.data(),many.size(),out));   // 2 million ops
        std::vector<uint8_t> overflow{1,0xff,0xff,0xff,0xff,0xff,0x01};assert(!decode(overflow.data(),overflow.size(),out));   // varint longer than 5 bytes
        std::vector<uint8_t> big_shift{1,0xff,0xff,0xff,0xff,0x1f};assert(!decode(big_shift.data(),big_shift.size(),out));
        std::vector<uint8_t> far_point{1,1,1,0,1,0xff,0xff,0x03,0};assert(!decode(far_point.data(),far_point.size(),out));   // x far outside the pad
        std::vector<uint8_t> eraser_ink{1,1,1,uint8_t(1|(3<<2)),1,0,0};assert(!decode(eraser_ink.data(),eraser_ink.size(),out));   // erasers have no colour
        std::vector<uint8_t> stamp_id{1,1,3,uint8_t(stamp_count),0,0,0};assert(!decode(stamp_id.data(),stamp_id.size(),out));
        std::vector<uint8_t> unknown{1,1,9,0,0,0};assert(!decode(unknown.data(),unknown.size(),out));
        for(std::string bad:{std::string("\xc0\x80"),std::string("\xed\xa0\x80"),std::string("\xf5\x80\x80\x80"),std::string("a\nb"),std::string("\x7f"),std::string("\xc2\x85"),std::string("\xe2\x82"),std::string("\xf4\x90\x80\x80")}){
            assert(!valid_text(bad));
            std::vector<uint8_t> t{1,1,2,0,0,0,uint8_t(bad.size())};t.insert(t.end(),bad.begin(),bad.end());assert(!decode(t.data(),t.size(),out));
        }
        assert(valid_text("Blåbær ♥ 日本 😀"));
        Op empty;empty.kind=Kind::Text;empty.points={{1,1}};assert(encode({empty}).empty());   // empty text
        Op long_text=empty;long_text.text=std::string(max_text+1,'a');assert(encode({long_text}).empty());
        Op no_points;assert(encode({no_points}).empty());
        std::vector<Op> too_many(max_ops+1,s);assert(encode(too_many).empty());
    }
    // 3. Fuzz: random bytes, and valid payloads with a few bytes changed.
    {
        long accepted=0;std::vector<Op> out;
        for(int i=0;i<150000;i++){
            std::vector<uint8_t> bytes(pick(0,64));for(auto& b:bytes)b=uint8_t(pick(0,255));
            if(!bytes.empty() && pick(0,1))bytes[0]=1;
            if(decode(bytes.data(),bytes.size(),out)){accepted++;auto again=encode(out);assert(!again.empty());std::vector<Op> back;assert(decode(again.data(),again.size(),back));assert(same(out,back));}
        }
        for(int i=0;i<60000;i++){
            auto bytes=encode(random_message());if(bytes.empty())continue;
            for(int k=pick(1,4);k>0;k--)bytes[size_t(pick(0,int(bytes.size())-1))]=uint8_t(pick(0,255));
            if(pick(0,5)==0)bytes.resize(size_t(pick(0,int(bytes.size()))));
            if(decode(bytes.data(),bytes.size(),out)){accepted++;auto again=encode(out);assert(!again.empty());}
        }
        std::printf("fuzz: %ld accepted payloads all re-encoded cleanly\n",accepted);
    }
    // 4. Drawing: incremental painting equals replay; the dirty rectangle covers every change.
    {
        BoxGlyphs glyphs;
        for(int i=0;i<200;i++){
            auto ops=random_message();Canvas live;Canvas replay;render(replay,ops,&glyphs);
            for(const auto& op:ops){
                Canvas before=live;Rect dirty;
                if(op.kind==Kind::Stroke){StrokePainter p(live,op);for(auto pt:op.points)dirty.add(p.add(pt));}else dirty=paint(live,op,&glyphs);
                for(int y=0;y<canvas_h;y++)for(int x=0;x<canvas_w;x++)if(before.at(x,y)!=live.at(x,y))assert(x>=dirty.x0 && x<dirty.x1 && y>=dirty.y0 && y<dirty.y1);
            }
            assert(live.px==replay.px);
        }
        // Extremes stay inside the buffer (ASan would trap otherwise).
        Canvas c;Op far;far.points={{-64,-64},{canvas_w+64,canvas_h+64},{-64,canvas_h+64},{canvas_w+64,-64}};far.thick=true;far.colour=rainbow;paint(c,far,&glyphs);
        Op text;text.kind=Kind::Text;text.text="edge of the pad";text.points={{canvas_w-30,canvas_h-5}};paint(c,text,&glyphs);
        Op stamp;stamp.kind=Kind::Stamp;stamp.stamp=0;stamp.points={{0,0}};paint(c,stamp,&glyphs);
        // Ruled paper: a blank pad has its two rules, and erasing puts them back instead of leaving a white gap.
        {Canvas ruled;assert(ruled.at(10,rule_row0)==rule && ruled.at(10,rule_row1)==rule && ruled.at(10,20)==paper);
         Op ink;ink.thick=true;ink.points={{100,rule_row0},{200,rule_row0}};Canvas c2;paint(c2,ink,nullptr);assert(c2.at(150,rule_row0)==inks[0]);
         Op wipe;wipe.eraser=true;wipe.thick=true;wipe.points={{90,rule_row0},{210,rule_row0}};paint(c2,wipe,nullptr);
         for(int x=60;x<250;x++)assert(c2.at(x,rule_row0)==rule && c2.at(x,rule_row0-3)==paper && c2.at(x,rule_row0+3)==paper);}
        // Erasing restores paper; a dot is drawn; rainbow changes along the path.
        Canvas d;Op dot;dot.points={{100,50}};dot.colour=5;paint(d,dot,nullptr);assert(d.at(100,50)==inks[5]);
        Op wipe;wipe.eraser=true;wipe.thick=true;wipe.points={{90,50},{110,50}};paint(d,wipe,nullptr);assert(d.at(100,50)==paper);
        Canvas r;Op line;line.colour=rainbow;for(int x=10;x<500;x+=2)line.points.push_back({int16_t(x),50});paint(r,line,nullptr);
        assert(r.at(20,50)!=r.at(200,50) && r.at(200,50)!=r.at(400,50));
        for(int id=0;id<stamp_count;id++){Canvas s;Op o;o.kind=Kind::Stamp;o.stamp=uint8_t(id);o.points={{100,50}};paint(s,o,nullptr);Canvas blank;long ink=0;for(size_t i=0;i<s.px.size();i++)ink+=s.px[i]!=blank.px[i];assert(ink>150 && ink<1700);}
    }
    // 5. Simplifying keeps the endpoints and stays near the original path.
    {
        std::vector<Point> path;for(int x=0;x<500;x++)path.push_back({int16_t(x),int16_t(50+int(10*std::sin(x*.05)))});
        auto simple=simplify(path,.8f);assert(simple.front()==path.front() && simple.back()==path.back() && simple.size()<path.size()/3);
        for(const auto& p:path){float best=1e9f;for(size_t i=0;i+1<simple.size();i++){
            float ax=simple[i].x,ay=simple[i].y,bx=simple[i+1].x,by=simple[i+1].y,ex=bx-ax,ey=by-ay,l2=ex*ex+ey*ey,t=l2?std::clamp(((p.x-ax)*ex+(p.y-ay)*ey)/l2,0.f,1.f):0.f;
            best=std::min(best,std::hypot(p.x-(ax+t*ex),p.y-(ay+t*ey)));}
            assert(best<=.85f);}
        assert(simplify({{1,1}},1).size()==1);
        // A busy two-line scribble still fits in one packet's budget once simplified.
        Op scribble;scribble.points.reserve(2400);
        for(int i=0;i<2400;i++)scribble.points.push_back({int16_t(20+int(250*(1+std::sin(i*.021)))),int16_t(56+int(48*std::sin(i*.13)))});
        scribble.points=simplify(scribble.points,.8f);auto bytes=encode({scribble});assert(!bytes.empty() && bytes.size()<max_payload);
        std::printf("a 2400-point scribble simplified to %zu points, %zu bytes\n",scribble.points.size(),bytes.size());
    }
    std::puts("pictochat core: ok");
}
