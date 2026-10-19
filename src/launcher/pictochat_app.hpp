#pragma once
// The chat pad's behaviour without any drawing: what is on the pad, who is in the
// room, what the keys do, and the helper process that talks to the network.
// pictochat_core.hpp defines a message; the launcher paints all of this.
#include "launcher/pictochat_core.hpp"
#include <cerrno>
#include <csignal>
#include <deque>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>

namespace rgds::pictochat {
namespace fs=std::filesystem;

struct Box {
    int x=0,y=0,w=0,h=0;
    bool has(int px,int py)const{return px>=x && px<x+w && py>=y && py<y+h;}
};

// ---- where everything is on the lower screen (640 x 480) ----------------------
// The keyboard is the launcher's shared one (keyboard.hpp); it starts at y 186.
namespace layout {
// In a room there is no footer: the keyboard sits at the bottom (first row at
// `keyboard_top`), the pad is drawn larger than the 480 x 96 message it
// holds (`pad_scale`; touches are mapped back), tools stand on the left and
// Send with the scroll arrows on the right. Who else is in the room runs
// along the very top; inks, undo and clear sit under the pad.
// Like the DS: a column of tools down the left, the pad with the names of the
// others under it, the inks, then the keyboard (narrowed to 90 %) with Send,
// the scroll arrows and Leave in a column on its right.
constexpr Box pad{58,50,540,108};
constexpr float pad_scale=float(pad.w)/canvas_w;
constexpr int keyboard_top=240;
constexpr float keyboard_scale=.9f;
inline Box tool(int i){return {6,30+i*52,44,44};}             // pen, eraser, thin, thick
constexpr Box names{58,162,540,22};                           // who else is in the room
inline Box swatch(int i){return {58+i*36,190,32,36};}         // 0..7 inks, 8 rainbow
constexpr Box undo{394,190,46,36};
constexpr Box clear{446,190,46,36};
constexpr Box send{572,240,62,62};
constexpr Box scroll_up{572,308,62,36};
constexpr Box scroll_down{572,348,62,36};
constexpr Box leave{572,418,62,50};                           // back to the lobby (B does the same)
// The lobby: the four rooms as rows, then your profile.
inline Box room(int i){return {20,16+i*84,600,76};}
constexpr Box profile{20,356,600,56};
// Choosing a name and colour.
constexpr Box field{20,30,600,56};
inline Box chip(int i){return {14+i*36,116,32,32};}           // 0 follows the launcher's accent, 1..16 are the DS colours
}

// ---- the helper process ---------------------------------------------------------
class Helper {
    pid_t pid=-1;int in=-1,out=-1;std::string buffer;bool reported_exit=false;
public:
    Helper()=default;
    Helper(const Helper&)=delete;Helper& operator=(const Helper&)=delete;
    ~Helper(){stop();}
    bool running()const{return pid>0;}
    bool start(const fs::path& script,const std::vector<std::string>& extra){
        if(pid>0)return true;
        std::error_code ec;if(!fs::is_regular_file(script,ec))return false;
        std::signal(SIGPIPE,SIG_IGN);   // a helper that died must not take the launcher with it
        int to[2],from[2];
        if(pipe(to)!=0)return false;
        if(pipe(from)!=0){::close(to[0]);::close(to[1]);return false;}
        std::vector<std::string> args={"python3","-B",script.string()};args.insert(args.end(),extra.begin(),extra.end());
        pid=fork();
        if(pid==0){
            if(dup2(to[0],0)<0 || dup2(from[1],1)<0)_exit(127);
            for(int fd=3;fd<256;fd++)::close(fd);
            std::vector<char*> argv;for(auto& a:args)argv.push_back(a.data());argv.push_back(nullptr);
            execvp(argv[0],argv.data());_exit(127);
        }
        ::close(to[0]);::close(from[1]);
        if(pid<0){::close(to[1]);::close(from[0]);return false;}
        in=to[1];out=from[0];reported_exit=false;buffer.clear();
        for(int fd:{in,out}){fcntl(fd,F_SETFD,FD_CLOEXEC);fcntl(fd,F_SETFL,fcntl(fd,F_GETFL)|O_NONBLOCK);}
        return true;
    }
    void write_line(const std::string& line){
        if(in<0)return;
        std::string text=line+"\n";const char* p=text.data();size_t left=text.size();
        for(int tries=0;left && tries<200;){
            ssize_t n=::write(in,p,left);
            if(n>0){p+=n;left-=size_t(n);continue;}
            if(n<0 && (errno==EAGAIN || errno==EINTR)){tries++;usleep(1000);continue;}
            break;   // a closed pipe: the exit shows up in poll()
        }
    }
    // Complete lines the helper has printed; "EXIT" once if it has ended.
    std::vector<std::string> poll(){
        std::vector<std::string> lines;
        if(out>=0){
            char chunk[4096];ssize_t n;
            while((n=::read(out,chunk,sizeof(chunk)))>0){buffer.append(chunk,size_t(n));if(buffer.size()>1<<20){buffer.clear();break;}}
            for(size_t at;(at=buffer.find('\n'))!=std::string::npos;){
                if(at<=20000)lines.push_back(buffer.substr(0,at));
                buffer.erase(0,at+1);
            }
        }
        if(pid>0){
            int status=0;pid_t done=waitpid(pid,&status,WNOHANG);
            if(done==pid || (done<0 && errno!=EINTR)){pid=-1;if(!reported_exit){reported_exit=true;lines.push_back("EXIT");}}
        }
        return lines;
    }
    void stop(){
        if(pid>0){
            write_line("QUIT");
            for(int i=0;i<40 && waitpid(pid,nullptr,WNOHANG)==0;i++)usleep(10000);
            if(waitpid(pid,nullptr,WNOHANG)==0){kill(pid,SIGKILL);waitpid(pid,nullptr,0);}
            pid=-1;
        }
        if(in>=0)::close(in);if(out>=0)::close(out);in=out=-1;buffer.clear();
    }
};

// ---- the app --------------------------------------------------------------------
struct User {std::string name;int room=0,colour=0;};
struct Entry {
    uint32_t serial=0;std::string sender,name,notice;int colour=0;bool mine=false;std::vector<Op> ops;
};
enum class View {Lobby,Room,Naming};

inline int utf8_length(const std::string& s){int n=0;for(unsigned char c:s)if((c&0xc0)!=0x80)n++;return n;}
inline void drop_last_character(std::string& s){
    if(s.empty())return;size_t at=s.size()-1;while(at>0 && (static_cast<unsigned char>(s[at])&0xc0)==0x80)at--;s.erase(at);
}

class Chat {
public:
    // who we are: `colour` is the DS colour others see; automatic follows the launcher's accent
    std::string name="RG DS";int colour=11;bool colour_auto=true;
    // where we are
    View view=View::Lobby;int room=0;
    // the network
    bool helper_up=false,online=false;std::string address,self_id;
    std::map<std::string,User> users;
    std::deque<Entry> log;int scrolled=0;bool unread=false;
    Helper helper;
    // the pad
    Canvas pad;std::vector<Op> ops;Op live;std::unique_ptr<StrokePainter> painter;bool drawing=false;
    bool eraser=false,thick=false,stamp_armed=false;int stamp=0,ink=0;
    int caret_line=0;int caret_x=6;
    Rect pad_dirty;bool pad_replayed=true;
    std::string status;uint32_t serial=0;double last_sent=-10;
    GlyphSource* glyphs=nullptr;

    static int text_y(int line){return line*48+10;}
    uint8_t plain_ink()const{return uint8_t(ink==rainbow?0:ink);}
    size_t points_used()const{size_t n=0;for(const auto& op:ops)n+=op.points.size();return n;}

    // -- profile ---------------------------------------------------------------------
    // `accent` is the launcher's DS accent colour (0-15), or -1 for the classic blue; with no saved choice the pad follows it.
    void load_profile(const fs::path& file,int accent){
        colour_auto=true;colour=accent>=0 && accent<16?accent:11;
        std::ifstream in(file);std::string line;
        while(std::getline(in,line)){
            if(line.rfind("name=",0)==0){auto n=line.substr(5);while(utf8_length(n)>16)drop_last_character(n);if(!n.empty())name=n;}
            else if(line.rfind("colour=",0)==0){try{int c=std::stoi(line.substr(7));if(c>=0 && c<16){colour=c;colour_auto=false;}}catch(const std::exception&){}}
        }
    }
    void follow_accent(int accent){if(colour_auto)colour=accent>=0 && accent<16?accent:11;}
    void save_profile(const fs::path& file)const{
        std::error_code ec;fs::create_directories(file.parent_path(),ec);
        auto pending=fs::path(file.string()+".pending");
        {std::ofstream out(pending);out<<"name="<<name<<"\ncolour="<<(colour_auto?std::string("auto"):std::to_string(colour))<<"\n";if(!out)return;}
        fs::rename(pending,file,ec);
    }

    // -- network ---------------------------------------------------------------------
    int people(int r)const{int n=0;for(const auto& u:users)if(u.second.room==r)n++;return n+(view==View::Room && room==r?1:0);}
    std::vector<int> colours_in(int r)const{std::vector<int> out;if(view==View::Room && room==r)out.push_back(colour);for(const auto& u:users)if(u.second.room==r && out.size()<16)out.push_back(u.second.colour);return out;}
    void hello(){if(view==View::Room)helper.write_line("HELLO "+std::to_string(room)+" "+std::to_string(colour)+" "+name);}
    void notice(const std::string& text){Entry e;e.serial=++serial;e.notice=text;push(std::move(e));}
    void push(Entry e){
        const bool at_end=scrolled==0;
        log.push_back(std::move(e));while(log.size()>60)log.pop_front();
        // `scrolled` counts messages newer than the one in view: a new arrival adds one, an eviction at the far end changes nothing.
        if(!at_end){scrolled=std::min<int>(scrolled+1,int(log.size())-1);unread=true;}
    }
    void handle(const std::string& line){
        std::istringstream words(line);std::string word;words>>word;
        if(word=="READY"){words>>self_id;helper_up=true;}
        else if(word=="ONLINE"){online=true;words>>address;}
        else if(word=="OFFLINE"){online=false;address.clear();}
        else if(word=="EXIT"){helper_up=false;online=false;address.clear();users.clear();}
        else if(word=="FULL"){if(view==View::Room){leave();status="That room is full";}}
        else if(word=="USER"){
            std::string id,r,c;words>>id>>r>>c;std::string nm;std::getline(words>>std::ws,nm);
            try{
                User u;u.room=std::stoi(r);u.colour=std::stoi(c);u.name=nm;
                if(id.size()!=16 || u.room<0 || u.room>3 || u.colour<0 || u.colour>15 || users.size()>=64)return;
                const bool fresh=!users.count(id) || users[id].room!=u.room;
                users[id]=u;
                if(fresh && view==View::Room && u.room==room)notice(u.name+" joined");
            }catch(const std::exception&){}
        }else if(word=="GONE"){
            std::string id;words>>id;auto found=users.find(id);
            if(found!=users.end()){if(view==View::Room && found->second.room==room)notice(found->second.name+" left");users.erase(found);}
        }else if(word=="MSG"){
            std::string id,hex;words>>id>>hex;auto found=users.find(id);
            if(found==users.end() || view!=View::Room || found->second.room!=room || hex.size()%2 || hex.size()>2*max_payload)return;
            std::vector<uint8_t> bytes;bytes.reserve(hex.size()/2);
            auto nibble=[](char c){return c>='0' && c<='9'?c-'0':c>='a' && c<='f'?c-'a'+10:-1;};
            for(size_t i=0;i<hex.size();i+=2){int a=nibble(hex[i]),b=nibble(hex[i+1]);if(a<0 || b<0)return;bytes.push_back(uint8_t(a*16+b));}
            Entry e;e.serial=++serial;e.sender=id;e.name=found->second.name;e.colour=found->second.colour;
            if(decode(bytes.data(),bytes.size(),e.ops))push(std::move(e));
        }
    }
    void poll(){for(const auto& line:helper.poll())handle(line);}

    // -- rooms -----------------------------------------------------------------------
    void join(int r){
        view=View::Room;room=std::clamp(r,0,3);log.clear();scrolled=0;unread=false;
        wipe();eraser=false;stamp_armed=false;status.clear();
        notice(std::string("You are in Room ")+char('A'+room));
        hello();
    }
    void leave(){helper.write_line("LEAVE");view=View::Lobby;log.clear();scrolled=0;drawing=false;painter.reset();}
    void close(){helper.stop();users.clear();log.clear();view=View::Lobby;helper_up=online=false;address.clear();drawing=false;painter.reset();}

    // -- the pad ---------------------------------------------------------------------
    void replay(){if(glyphs)render(pad,ops,glyphs);else{pad.clear();for(const auto& op:ops)paint(pad,op,nullptr);}pad_replayed=true;pad_dirty={0,0,canvas_w,canvas_h};recompute_caret();}
    void wipe(){ops.clear();live=Op();painter.reset();drawing=false;caret_line=0;caret_x=6;pad.clear();pad_replayed=true;pad_dirty={0,0,canvas_w,canvas_h};}
    void recompute_caret(){
        caret_line=0;caret_x=6;
        for(auto it=ops.rbegin();it!=ops.rend();++it){
            if(it->kind!=Kind::Text)continue;
            caret_line=it->points[0].y>=48?1:0;caret_x=it->points[0].x+(glyphs?glyphs->width(it->text):0);break;
        }
    }
    bool inked()const{return !ops.empty();}
    static Point clamp_point(int x,int y){return {int16_t(std::clamp(x-layout::pad.x,0,canvas_w-1)),int16_t(std::clamp(y-layout::pad.y,0,canvas_h-1))};}

    void begin_stroke(int x,int y){
        auto p=clamp_point(x,y);
        if(ops.size()>=max_ops-1 || points_used()>=max_points-8){status="The pad is full: send it or clear it";return;}
        if(stamp_armed){
            Op op;op.kind=Kind::Stamp;op.stamp=uint8_t(stamp);op.colour=plain_ink();op.points={p};
            pad_dirty.add(paint(pad,op,nullptr));ops.push_back(std::move(op));status.clear();return;
        }
        live=Op();live.kind=Kind::Stroke;live.eraser=eraser;live.thick=thick;live.colour=eraser?0:uint8_t(ink);
        painter=std::make_unique<StrokePainter>(pad,live);drawing=true;status.clear();
        live.points.push_back(p);pad_dirty.add(painter->add(p));
    }
    void extend_stroke(int x,int y){
        if(!drawing)return;auto p=clamp_point(x,y);const auto& last=live.points.back();
        if(std::abs(p.x-last.x)<2 && std::abs(p.y-last.y)<2)return;
        if(live.points.size()>=1500 || points_used()+live.points.size()>=max_points-8){end_stroke();begin_stroke(x,y);return;}
        live.points.push_back(p);pad_dirty.add(painter->add(p));
    }
    void end_stroke(){
        if(!drawing)return;drawing=false;painter.reset();
        if(!live.points.empty())ops.push_back(std::move(live));
        live=Op();
    }
    void undo(){end_stroke();if(!ops.empty()){ops.pop_back();replay();}status.clear();}
    void clear_pad(){end_stroke();wipe();status.clear();}

    void type(const std::string& piece){
        if(piece.empty() || ops.size()>=max_ops-1 || !glyphs)return;
        end_stroke();
        if(piece=="\n"){if(caret_line==0){caret_line=1;caret_x=6;}return;}
        const int width=glyphs->width(piece);
        if(caret_x+width>canvas_w-6){if(caret_line==0){caret_line=1;caret_x=6;}else return;}
        Op* last=ops.empty()?nullptr:&ops.back();
        if(last && last->kind==Kind::Text && last->colour==plain_ink() && last->points[0].y==text_y(caret_line) && last->text.size()+piece.size()<=max_text
           && last->points[0].x+glyphs->width(last->text)==caret_x){last->text+=piece;}
        else{
            Op op;op.kind=Kind::Text;op.colour=plain_ink();op.text=piece;op.points={{int16_t(caret_x),int16_t(text_y(caret_line))}};
            if(points_used()>=max_points-8)return;
            ops.push_back(std::move(op));
        }
        status.clear();replay();   // also puts the caret after what was just written
    }
    void backspace(){
        end_stroke();
        if(!ops.empty() && ops.back().kind==Kind::Text){drop_last_character(ops.back().text);if(ops.back().text.empty())ops.pop_back();replay();}
        else undo();
        status.clear();
    }

    // -- sending ---------------------------------------------------------------------
    // The message as every handheld will see it: simplified, encoded, decoded again.
    bool compose(std::vector<Op>& canonical,std::string& why)const{
        std::vector<Op> message=ops;
        for(auto& op:message)if(op.kind==Kind::Stroke)op.points=simplify(op.points,.7f);
        auto bytes=encode(message);
        if(bytes.empty()){why="Too much on the pad: erase some of it";return false;}
        if(!decode(bytes.data(),bytes.size(),canonical)){why="That message could not be sent";return false;}
        return true;
    }
    bool send(double now){
        end_stroke();
        if(view!=View::Room || ops.empty())return false;
        if(now-last_sent<.4)return false;
        std::vector<Op> canonical;std::string why;
        if(!compose(canonical,why)){status=why;return false;}
        auto bytes=encode(canonical);
        static const char* digits="0123456789abcdef";std::string hex;hex.reserve(bytes.size()*2);
        for(uint8_t b:bytes){hex+=digits[b>>4];hex+=digits[b&15];}
        helper.write_line("SEND "+hex);
        Entry e;e.serial=++serial;e.sender=self_id;e.name=name;e.colour=colour;e.mine=true;e.ops=std::move(canonical);
        scrolled=0;unread=false;push(std::move(e));
        last_sent=now;wipe();status.clear();
        return true;
    }
};

}
