#pragma once
#include <algorithm>
#include <string>
#include <vector>
#ifdef __linux__
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace rgds {
// Private evdev readers used only to wake the idle UI. Every evdev client gets
// its own copy of each event, so draining these never takes input away from
// SDL's joystick, the raw touch decoder or the volume-key reader.
class InputWake {
    std::vector<int> fds;
public:
    InputWake()=default;
    InputWake(const InputWake&)=delete;
    InputWake& operator=(const InputWake&)=delete;
    ~InputWake(){close_all();}
    void open_all(const std::string& folder="/dev/input"){
        close_all();
#ifdef __linux__
        DIR* directory=opendir(folder.c_str());if(!directory)return;
        while(auto* entry=readdir(directory)){
            if(std::strncmp(entry->d_name,"event",5)!=0)continue;
            int fd=::open((folder+"/"+entry->d_name).c_str(),O_RDONLY|O_NONBLOCK|O_CLOEXEC);if(fd<0)continue;
            unsigned long types=0;
            if(ioctl(fd,EVIOCGBIT(0,sizeof(types)),&types)<0 || !(types&((1UL<<EV_KEY)|(1UL<<EV_ABS)))){::close(fd);continue;}
            fds.push_back(fd);
        }
        closedir(directory);
#else
        (void)folder;
#endif
    }
    void close_all(){
#ifdef __linux__
        for(int fd:fds)::close(fd);
#endif
        fds.clear();
    }
    bool available()const{return !fds.empty();}
    // Sleep until any input device reports an event or the timeout expires.
    void wait(int timeout){
#ifdef __linux__
        std::vector<pollfd> set;set.reserve(fds.size());
        for(int fd:fds)set.push_back({fd,POLLIN,0});
        if(::poll(set.data(),set.size(),std::max(timeout,0))<=0)return;
        input_event events[32];
        for(const auto& item:set)if(item.revents&POLLIN)while(::read(item.fd,events,sizeof(events))>0){}
#else
        (void)timeout;
#endif
    }
};

// SDL's event wait may poll an open joystick every millisecond. Without
// private wake descriptors, sleep once between explicit polls instead. Eight
// milliseconds also bounds raw evdev touch/volume latency; desktop
// keyboard-only windows keep their event wakeup. With wake descriptors the
// loop sleeps in poll(2) for the whole timeout and wakes on the first event.
// Inspired by ROCKNIXDS's documented idle-polling investigation; independently
// implemented for RGDS's existing SDL and evdev input loop.
template<class Api, class Event>
bool wait_ui_event(Api& api, Event* event, int timeout, bool polling_input, InputWake* wake=nullptr) {
    if (!polling_input) return api.WaitEventTimeout(event, timeout) != 0;
    if (api.PollEvent(event)) return true;
    if (wake && wake->available()) wake->wait(timeout);
    else api.Delay(std::clamp(timeout, 1, 8));
    return api.PollEvent(event) != 0;
}
}
