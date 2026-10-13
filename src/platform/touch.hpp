#pragma once
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/input.h>
#include <sys/ioctl.h>
#endif
namespace rgds {
struct TouchPoint { int kind,x,y; }; // down, move, up, cancel
struct TouchDecoder {
    struct Contact {int id=-1,x=0,y=0;bool active=false,x_known=false,y_known=false,tracking_seen=false,frame_x=false,frame_y=false;};
    int min_x=0,max_x=640,min_y=0,max_y=480,slot=0;
    bool type_a=false,uses_slots=false;
    Contact current,single;
    std::array<Contact,16> slots{};
    std::vector<Contact> contacts;
    bool mt_seen=false,button_seen=false,button_down=false,reported=false,dropped=false,ignore_remaining=false;
    int primary=-1,last_x=0,last_y=0;
    int coordinate(int value,int minimum,int maximum,int extent)const{return std::clamp(int((int64_t(value)-minimum)*extent/std::max<int64_t>(1,int64_t(maximum)-minimum)),0,extent-1);}
    std::vector<TouchPoint> cancel(){bool was=reported;reported=false;ignore_remaining=false;current=single=Contact{};slots={};contacts.clear();button_seen=button_down=false;return was?std::vector<TouchPoint>{{3,last_x,last_y}}:std::vector<TouchPoint>{};}
    void discard_contact(){bool active=reported || ignore_remaining;cancel();ignore_remaining=active;}
    void finish_contact(){if(!current.tracking_seen)current.active=button_seen?button_down:(current.frame_x && current.frame_y);if(contacts.size()<16 && current.active && current.x_known && current.y_known)contacts.push_back(current);current=Contact{};}
    std::vector<TouchPoint> feed(int type,int code,int value) {
        if(type==0 && code==3){auto result=cancel();ignore_remaining=true;dropped=true;return result;}
        if(dropped){if(type==0 && code==0)dropped=false;return {};}
        if(type==1 && code==330){button_seen=true;button_down=value!=0;single.active=button_down;if(!button_down){current.active=false;for(auto& contact:slots)contact.active=false;}}
        else if(type==3){
            if(code==47){uses_slots=true;slot=value>=0 && value<int(slots.size())?value:-1;}
            else if(code==0 || code==1){if(code==0){single.x=value;single.x_known=true;}else{single.y=value;single.y_known=true;}}
            else if(code==24){single.active=value>0;}
            else if(code==53 || code==54 || code==57){
                mt_seen=true;Contact* contact=uses_slots?(slot>=0?&slots[size_t(slot)]:nullptr):&current;
                if(contact){
                    if(code==53){contact->x=value;contact->x_known=contact->frame_x=true;}
                    else if(code==54){contact->y=value;contact->y_known=contact->frame_y=true;}
                    else{contact->id=value;contact->tracking_seen=true;contact->active=value>=0;}
                }
            }
        }
        if(type==0 && code==2 && !uses_slots){type_a=true;finish_contact();return {};}
        if(type!=0 || code!=0)return {};
        if(type_a && !uses_slots)finish_contact();
        std::vector<std::pair<int,Contact>> active;
        auto add=[&](const Contact& contact,int fallback){if(contact.active && contact.x_known && contact.y_known && (!button_seen || button_down))active.emplace_back(contact.id>=0?contact.id:fallback,contact);};
        if(mt_seen){if(uses_slots){for(size_t i=0;i<slots.size();i++)add(slots[i],-int(i)-2);}else if(type_a){for(size_t i=0;i<contacts.size();i++)add(contacts[i],-int(i)-2);}else add(current,-2);}
        else add(single,-1);
        std::vector<TouchPoint> result;
        if(reported){auto found=std::find_if(active.begin(),active.end(),[&](const auto& point){return point.first==primary;});
            if(found==active.end()){result.push_back({2,last_x,last_y});reported=false;ignore_remaining=!active.empty();}
            else{int x=coordinate(found->second.x,min_x,max_x,640),y=coordinate(found->second.y,min_y,max_y,480);if(x!=last_x || y!=last_y){last_x=x;last_y=y;result.push_back({1,x,y});}}
        }else if(!ignore_remaining && !active.empty()){primary=active.front().first;last_x=coordinate(active.front().second.x,min_x,max_x,640);last_y=coordinate(active.front().second.y,min_y,max_y,480);reported=true;result.push_back({0,last_x,last_y});}
        if(active.empty())ignore_remaining=false;
        if(type_a)contacts.clear();
        for(auto& contact:slots)contact.frame_x=contact.frame_y=false;
        current.frame_x=current.frame_y=false;
        return result;
    }
};
// Linux AArch64 input_event: two 64-bit times followed by type/code/value.
// The same framing reader permits host replay of actual captured input bytes.
struct TouchPacket {uint64_t micros=0;int type=0,code=0,value=0;};
class TouchPackets {
    std::vector<unsigned char> pending;
public:
    static uint64_t number(const unsigned char* p,int bytes){uint64_t value=0;for(int i=0;i<bytes;i++)value|=uint64_t(p[i])<<(i*8);return value;}
    std::vector<TouchPacket> feed(const unsigned char* bytes,size_t count){pending.insert(pending.end(),bytes,bytes+count);std::vector<TouchPacket> result;size_t used=0;
        while(pending.size()-used>=24){auto* event=pending.data()+used;result.push_back({number(event,8)*1000000+number(event+8,8),int(number(event+16,2)),int(number(event+18,2)),int(int32_t(number(event+20,4)))});used+=24;}
        pending.erase(pending.begin(),pending.begin()+static_cast<std::ptrdiff_t>(used));return result;
    }
};
class TouchDevice {
    int fd=-1;
    TouchDecoder decoder;
    TouchPackets packets;
    std::string selected_path;
    std::vector<TouchPacket> replay;
    size_t replay_at=0;
    bool replaying=false;
    bool initial_down=false;
    uint64_t replay_begin=0;
public:
    bool open_device(const std::string& path="") {
        close_device();
#ifdef __linux__
        std::vector<std::string> paths;if(!path.empty())paths.push_back(path);else{std::error_code error;for(auto& entry:std::filesystem::directory_iterator("/dev/input",error))if(entry.path().filename().string().rfind("event",0)==0)paths.push_back(entry.path().string());std::sort(paths.begin(),paths.end());}
        int best=-1;
        for(const auto& candidate:paths){int next=::open(candidate.c_str(),O_RDONLY|O_NONBLOCK|O_CLOEXEC);if(next<0)continue;
            unsigned char abs[16]{},keys[96]{},props[8]{};char name[256]{};
            ioctl(next,EVIOCGBIT(EV_ABS,sizeof(abs)),abs);ioctl(next,EVIOCGBIT(EV_KEY,sizeof(keys)),keys);ioctl(next,EVIOCGPROP(sizeof(props)),props);ioctl(next,EVIOCGNAME(sizeof(name)),name);name[sizeof(name)-1]=0;
            auto bit=[](const unsigned char* bits,int number){return (bits[number/8]&(1u<<(number%8)))!=0;};
            bool mt=bit(abs,ABS_MT_POSITION_X) && bit(abs,ABS_MT_POSITION_Y),single=bit(abs,ABS_X) && bit(abs,ABS_Y),direct=bit(props,INPUT_PROP_DIRECT);
            bool touch=bit(keys,BTN_TOUCH) || (mt && (direct || bit(abs,ABS_MT_TRACKING_ID)));
            input_absinfo x{},y{};int score=direct?100:0;std::string label=name;if(label.find("gt9")!=std::string::npos || label.find("touch")!=std::string::npos || label.find("Goodix")!=std::string::npos)score+=50;
            if((!mt && !single) || !touch || (!direct && bit(keys,BTN_TOOL_FINGER)) || ioctl(next,EVIOCGABS(mt?ABS_MT_POSITION_X:ABS_X),&x)<0 || ioctl(next,EVIOCGABS(mt?ABS_MT_POSITION_Y:ABS_Y),&y)<0 || x.maximum<=x.minimum || y.maximum<=y.minimum || score<=best){::close(next);continue;}
            if(fd>=0)::close(fd);fd=next;best=score;selected_path=candidate;unsigned char down[96]{};initial_down=ioctl(fd,EVIOCGKEY(sizeof(down)),down)>=0 && bit(down,BTN_TOUCH);decoder=TouchDecoder{};decoder.min_x=x.minimum;decoder.max_x=x.maximum;decoder.min_y=y.minimum;decoder.max_y=y.maximum;decoder.uses_slots=mt && bit(abs,ABS_MT_SLOT);decoder.type_a=mt && !decoder.uses_slots;
        }
        return fd>=0;
#else
        (void)path;return false;
#endif
    }
    bool open_replay(const std::string& path){close_device();std::ifstream file(path,std::ios::binary);if(!file)return false;std::array<unsigned char,768> bytes{};while(file){file.read(reinterpret_cast<char*>(bytes.data()),bytes.size());auto decoded=packets.feed(bytes.data(),size_t(file.gcount()));replay.insert(replay.end(),decoded.begin(),decoded.end());if(replay.size()>65536){close_device();return false;}}if(replay.empty())return false;replay_begin=replay.front().micros;replaying=true;selected_path=path;return true;}
    void close_device(){if(fd>=0)::close(fd);fd=-1;decoder=TouchDecoder{};packets=TouchPackets{};replay.clear();replay_at=0;replaying=false;initial_down=false;selected_path.clear();}
    ~TouchDevice(){close_device();}
    bool available()const{return fd>=0 || replaying;}
    bool initially_pressed()const{return initial_down;}
    const std::string& path()const{return selected_path;}
    void discard_until_release(){poll();decoder.discard_contact();}
    std::vector<TouchPoint> poll(uint64_t elapsed_ms=0) {
        std::vector<TouchPoint> result;
        auto consume=[&](const TouchPacket& event){auto points=decoder.feed(event.type,event.code,event.value);result.insert(result.end(),points.begin(),points.end());};
        if(replaying){while(replay_at<replay.size() && replay[replay_at].micros-replay_begin<=elapsed_ms*1000)consume(replay[replay_at++]);return result;}
#ifdef __linux__
        std::array<unsigned char,24*32> bytes{};while(fd>=0){ssize_t count=::read(fd,bytes.data(),bytes.size());if(count>0){for(auto event:packets.feed(bytes.data(),size_t(count)))consume(event);}else if(count<0 && errno==EINTR)continue;else if(count<0 && (errno==EAGAIN || errno==EWOULDBLOCK))break;else{auto cancel=decoder.cancel();result.insert(result.end(),cancel.begin(),cancel.end());close_device();break;}}
#endif
        return result;
    }
};
}
