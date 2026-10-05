#include "launcher/model.hpp"
#include "platform/sdl_dynamic.hpp"
#include "platform/system_controls.hpp"
#include "launcher/settings.hpp"
#include "launcher/font.hpp"
#include "launcher/carousel.hpp"
#include "launcher/media.hpp"
#include <array>
#include <csignal>
#include <ctime>
#include <iostream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace rgds;
static volatile sig_atomic_t stopping = 0;
static void on_signal(int) { stopping = 1; }

// Original, deliberately small probe font. UTF-8 paths are retained by the model;
// full Unicode text rendering is a later UI milestone.
static const std::map<char, std::array<unsigned char,7>> glyphs = {
 {'A',{14,17,17,31,17,17,17}}, {'B',{30,17,17,30,17,17,30}},
 {'C',{14,17,16,16,16,17,14}}, {'D',{30,17,17,17,17,17,30}},
 {'E',{31,16,16,30,16,16,31}}, {'F',{31,16,16,30,16,16,16}},
 {'G',{14,17,16,23,17,17,15}}, {'H',{17,17,17,31,17,17,17}},
 {'I',{14,4,4,4,4,4,14}}, {'J',{7,2,2,2,18,18,12}},
 {'K',{17,18,20,24,20,18,17}}, {'L',{16,16,16,16,16,16,31}},
 {'M',{17,27,21,21,17,17,17}}, {'N',{17,25,21,19,17,17,17}},
 {'O',{14,17,17,17,17,17,14}}, {'P',{30,17,17,30,16,16,16}},
 {'Q',{14,17,17,17,21,18,13}}, {'R',{30,17,17,30,20,18,17}},
 {'S',{15,16,16,14,1,1,30}}, {'T',{31,4,4,4,4,4,4}},
 {'U',{17,17,17,17,17,17,14}}, {'V',{17,17,17,17,17,10,4}},
 {'W',{17,17,17,21,21,21,10}}, {'X',{17,17,10,4,10,17,17}},
 {'Y',{17,17,10,4,4,4,4}}, {'Z',{31,1,2,4,8,16,31}},
 {'0',{14,17,19,21,25,17,14}}, {'1',{4,12,4,4,4,4,14}},
 {'2',{14,17,1,2,4,8,31}}, {'3',{30,1,1,14,1,1,30}},
 {'4',{2,6,10,18,31,2,2}}, {'5',{31,16,16,30,1,1,30}},
 {'6',{14,16,16,30,17,17,14}}, {'7',{31,1,2,4,8,8,8}},
 {'8',{14,17,17,14,17,17,14}}, {'9',{14,17,17,15,1,1,14}},
 {'.',{0,0,0,0,0,6,6}}, {':',{0,6,6,0,6,6,0}},
 {'-',{0,0,0,31,0,0,0}}, {'/',{1,2,2,4,8,8,16}},
 {'+',{0,4,4,31,4,4,0}}, {'?',{14,17,1,2,4,0,4}},
 {'!',{4,4,4,4,4,0,4}}, {'=',{0,31,0,31,0,0,0}},
 {'%',{25,25,2,4,8,19,19}},
 {'[',{14,8,8,8,8,8,14}}, {']',{14,2,2,2,2,2,14}},
 {'(',{2,4,8,8,8,4,2}}, {')',{8,4,2,2,2,4,8}},
 {'<',{1,2,4,8,4,2,1}}, {'>',{16,8,4,2,4,8,16}},
 {'_',{0,0,0,0,0,0,31}}, {' ',{0,0,0,0,0,0,0}}
 ,{'&',{12,18,20,8,21,18,13}}, {'\'',{4,4,8,0,0,0,0}}, {',',{0,0,0,0,0,4,8}}
};
struct Section { const char* title;const char* subtitle;const char* description; };
static const std::array<Section,6> sections = {{
    {"Theme & display","Brightness, colour and typeface","Make your home feel like your DS."},
    {"Power","Power mode, sleep and battery","More play. Less charging."},
    {"Library","Emulator, artwork and play history","Your favourites, always close by."},
    {"Sound","Volume and audio output","Set the sound of your next adventure."},
    {"Connectivity","Wi-Fi and connections","A little less setup. A lot more play."},
    {"System","Updates and device information","Keep your handheld feeling fresh."}
}};

struct Options {
    bool preview = false, probe = false, allow_launch = false, watch_assets = false, settings = false;
    int frames = 0, seconds = 0;
    std::string top = "DSI-1", bottom = "DSI-2";
    fs::path theme, themes, screenshot, app, data, state, preferences, system_preferences, input_script;
    std::string mode, font, section;
    std::vector<fs::path> roots;
};
struct Surface { SDL_Window* window = nullptr; SDL_Renderer* renderer = nullptr; Uint32 id = 0; };
class App {
    Sdl s;
    Options o;
    Model model;
    Carousel carousel;
    Theme theme;
    Theme light_theme=default_theme(),dark_theme=default_dark_theme();
    RoundedFont rounded;
    Media media;
    Preferences preferences;
    Settings settings;
    SystemControls system;
    SystemStatus hardware;
    SystemPreferences system_preferences;
    Uint32 last_activity=0,system_poll=0,lid_poll=0;
    std::optional<bool> lid_previous,lid_candidate;
    int lid_samples=0;
    bool font_ready=true;
    Uint32 selected_at=0,page_at=0,service_poll=0;
    pid_t worker=-1;
    std::string worker_action,username,password,edit_buffer,service_notice;
    int edit_field=0,key_row=0,key_col=0,key_layer=0;
    bool service_loaded=false;
    std::map<std::string,std::string> service;
    bool pointer_down=false;
    SDL_FingerID active_finger=0;
    int down_x=0,down_y=0;
    size_t pressed_game=0;
    void start_worker(const std::string& operation,const std::string& payload="") {
        if(worker>0)return;
        service_notice.clear();
        fs::create_directories(o.data/"scraper");
        std::vector<std::string> args={"python3","-B",(o.app.empty()?fs::path("runtime/scraper.py"):o.app/"runtime/scraper.py").string(),operation,"--data",o.data.string()};
        for(auto root:o.roots){args.push_back("--rom-dir");args.push_back(root.string());}
        int channel[2];if(pipe(channel)!=0){status="Could not start artwork service";return;}
        auto child=fork();
        if(child==0) {
            ::close(channel[1]);dup2(channel[0],STDIN_FILENO);::close(channel[0]);
            int log=::open((o.data/"scraper/worker.log").c_str(),O_WRONLY|O_CREAT|O_APPEND,0600);
            if(log>=0){dup2(log,STDOUT_FILENO);dup2(log,STDERR_FILENO);::close(log);}
            std::vector<char*> argv;for(auto& arg:args)argv.push_back(arg.data());argv.push_back(nullptr);execvp(argv[0],argv.data());_exit(127);
        }
        ::close(channel[0]);if(!payload.empty() && child>0){auto sent=write(channel[1],payload.data(),payload.size());(void)sent;}::close(channel[1]);
        if(child<0){status="Could not start artwork service";return;}
        worker=child;worker_action=operation;dirty=true;
    }
    void stop_worker() {
        if(worker<=0)return;
        if(waitpid(worker,nullptr,WNOHANG)==0) {
            kill(worker,SIGTERM);
            for(int i=0;i<30;i++){if(waitpid(worker,nullptr,WNOHANG)!=0){worker=-1;return;}usleep(10000);}
            kill(worker,SIGKILL);waitpid(worker,nullptr,0);
        }
        worker=-1;
    }
    void poll_service(Uint32 now) {
        if(worker>0 && waitpid(worker,nullptr,WNOHANG)!=0){worker=-1;dirty=true;}
        if(now-service_poll<250)return;service_poll=now;
        auto next=fields(o.data/"scraper/status.ini");if(next!=service){service=std::move(next);dirty=true;}
    }
    void select_game(size_t index,bool animate=true) {
        if(model.games.empty())return;
        model.selected=std::min(index,model.games.size()-1);selected_at=s.GetTicks();
        if(animate)carousel.select(model.selected,selected_at);else carousel.reset(model.selected);
        dirty=true;
    }
    void navigate(int delta) {
        if(model.games.empty())return;
        select_game(static_cast<size_t>(std::clamp(static_cast<long>(model.selected)+delta,0L,static_cast<long>(model.games.size())-1)));
    }
    void open_scraper() {
        settings.section=2;settings.page=Page::Scraper;settings.focus=0;
        if(worker<=0)start_worker("status");service_loaded=true;dirty=true;
    }
    void begin_edit(int field) {
        edit_field=field;edit_buffer=field==0?username:password;key_row=key_col=key_layer=0;
        settings.page=Page::Keyboard;s.StartTextInput();dirty=true;
    }
    void finish_edit(bool commit) {
        if(commit){if(edit_field==0)username=edit_buffer;else password=edit_buffer;}
        std::fill(edit_buffer.begin(),edit_buffer.end(),'\0');edit_buffer.clear();s.StopTextInput();settings.page=Page::Scraper;dirty=true;
    }
    void erase_character() {
        if(edit_buffer.empty())return;size_t at=edit_buffer.size()-1;
        while(at>0 && (static_cast<unsigned char>(edit_buffer[at])&0xc0)==0x80)at--;
        edit_buffer.resize(at);dirty=true;
    }
    std::vector<std::string> keyboard_rows() {
        if(key_layer==2)return {"!@#$%^&*()","_-+=[]{};","'\"\\|<>?",":,./~`1234"};
        return key_layer==1?std::vector<std::string>{"QWERTYUIOP","ASDFGHJKL","ZXCVBNM","1234567890"}:std::vector<std::string>{"qwertyuiop","asdfghjkl","zxcvbnm","1234567890"};
    }
    void keyboard_choose() {
        if(key_row==4) {
            if(key_col==0)key_layer=(key_layer+1)%3;
            else if(key_col==1 && edit_buffer.size()<256)edit_buffer+=' ';
            else if(key_col==2)erase_character();else if(key_col==3)finish_edit(true);
        }else if(edit_buffer.size()<256)edit_buffer+=keyboard_rows()[key_row][key_col];dirty=true;
    }
    void scraper_action() {
        if(settings.focus<2){begin_edit(settings.focus);return;}
        if(worker>0 && worker_action=="index")stop_worker();
        if(worker>0) {
            if(settings.focus==3){std::ofstream(o.data/"scraper/cancel").put('\n');status="Stopping after this request";}
            return;
        }
        if(settings.focus==2) {
            if(username.empty() || password.empty()){service_notice="Enter your username and password";dirty=true;return;}
            std::ostringstream json;json<<"{\"username\":"<<std::quoted(username)<<",\"password\":"<<std::quoted(password)<<"}";
            start_worker("login",json.str());std::fill(password.begin(),password.end(),'\0');password.clear();
        }else if(settings.focus==3)start_worker("scrape");else {start_worker("logout");username.clear();}
    }
    bool brightness_drag=false;
    struct Input { Uint32 time;SDL_Event event; };
    std::vector<Input> inputs;
    size_t next_input=0;
    std::vector<Surface> surfaces;
    std::vector<SDL_Joystick*> joysticks;
    bool quit = false, probe, dirty = true;
    std::string status = "READY", last_input = "WAITING FOR INPUT";
    std::string top_name, bottom_name;
    int touch_x = -1, touch_y = -1;
    Uint32 last_axis = 0;
    Uint32 theme_poll = 0, theme_changed = 0;
    bool theme_pending = false;
    std::map<fs::path,std::optional<std::string>> theme_source;
    std::map<fs::path,std::optional<std::string>> read_theme_source() {
        std::map<fs::path,std::optional<std::string>> result;
        for(auto path:{o.themes/"light.ini",o.themes/"dark.ini",o.theme}) {
            if(path.empty())continue;
            std::ifstream file(path,std::ios::binary);
            result[path]=file?std::optional<std::string>(std::string(std::istreambuf_iterator<char>(file),{})):std::nullopt;
        }
        return result;
    }
    void reload_themes() {
        auto light=fs::exists(o.themes/"light.ini")?load_theme(o.themes/"light.ini"):default_theme();
        auto dark=fs::exists(o.themes/"dark.ini")?load_theme(o.themes/"dark.ini",default_dark_theme()):default_dark_theme();
        if(!o.theme.empty()) {
            bool is_dark=dark_theme_file(o.theme);
            auto custom=load_theme(o.theme,is_dark?dark:light);
            if(is_dark)dark=std::move(custom);else light=std::move(custom);
        }
        light_theme=std::move(light);dark_theme=std::move(dark);theme=preferences.dark?dark_theme:light_theme;
    }
    void remember_appearance() {
        theme=preferences.dark?dark_theme:light_theme;
        try{save_preferences(o.preferences,preferences);status="APPEARANCE SAVED";}
        catch(const std::exception& e){status="COULD NOT SAVE SETTINGS";std::cerr<<e.what()<<std::endl;}
        dirty=true;
    }
    void choose_appearance(int choice) {
        if(settings.focus==0)preferences.dark=choice==1;
        else if(settings.focus==1) {
            if(choice==0 && !font_ready){status="ROUNDED FONT UNAVAILABLE";dirty=true;return;}
            preferences.pixel=choice==1;
        }else return;
        remember_appearance();
    }
    void refresh_system() {
        hardware=system.status();dirty=true;
    }
    bool apply_control(int focus,const SystemPreferences& value) {
        if(focus==0)return system.set("power",value.power);
        if(focus==2)return system.set("lid",value.lid);
        if(focus==4)return system.set("brightness",value.brightness);
        return true;
    }
    void change_system(int focus,int delta,bool cycle=false) {
        auto previous=system_preferences,next=previous;
        bool available=focus==0?hardware.power:focus==1?hardware.sleep:focus==2?hardware.lid:hardware.brightness;
        if(!available){status="Control unavailable on this device";dirty=true;return;}
        if(focus==0)next.power=cycle?(next.power+1)%4:std::clamp(next.power+delta,0,3);
        else if(focus==1)next.idle=cycle?(next.idle+1)%5:std::clamp(next.idle+delta,0,4);
        else if(focus==2)next.lid=cycle?!next.lid:delta>0;
        else if(focus==4)next.brightness=cycle?(next.brightness>=100?5:std::min(100,(next.brightness/5+1)*5)):std::clamp((next.brightness/5+delta)*5,5,100);
        if(next.power==previous.power && next.idle==previous.idle && next.lid==previous.lid && next.brightness==previous.brightness)return;
        if(!apply_control(focus,next)){status="Could not apply setting - see log";dirty=true;return;}
        try {
            save_system_preferences(o.system_preferences,next);system_preferences=next;
            status=o.preview?"Saved - desktop simulation":"Settings saved";
        }catch(const std::exception& e) {
            if(!apply_control(focus,previous))std::cerr<<"Could not restore previous hardware setting"<<std::endl;
            status="Could not save settings";std::cerr<<e.what()<<std::endl;
        }
        dirty=true;
    }
    void sleep_now() {
        if(!hardware.sleep){status="Sleep unavailable on this device";dirty=true;return;}
        save_state();status=o.preview?"Sleep simulated - press any control":"Sleeping - use Power to wake";dirty=true;draw();
        bool ok=system.sleep();
        status=ok?(o.preview?"Sleep simulated - settings retained":"Welcome back"):"Could not sleep - see log";
        // Clear queued presses from before suspend and reset idle timing after wake.
        SDL_Event pending{};while(s.PollEvent(&pending)){if(pending.type==SDL_QUIT)quit=true;}
        last_activity=s.GetTicks();dirty=true;
    }
    void watch_system(Uint32 now) {
        if(now-system_poll>=10000){system_poll=now;refresh_system();}
        if(o.preview || probe)return;
        if(now-lid_poll>=200) {
            lid_poll=now;auto closed=system.lid_closed();
            if(closed!=lid_candidate){lid_candidate=closed;lid_samples=1;}else lid_samples++;
            if(closed && lid_samples>=2 && closed!=lid_previous) {
                bool transition=lid_previous.has_value();lid_previous=closed;
                if(transition && *closed && system_preferences.lid && hardware.lid){sleep_now();return;}
            }
        }
        if(system_preferences.idle>0 && hardware.sleep && now-last_activity>=static_cast<Uint32>(idle_minutes[system_preferences.idle])*60000)sleep_now();
    }
    void watch_theme(Uint32 now) {
        if(!o.watch_assets || o.theme.empty() || now-theme_poll<100)return;
        theme_poll=now;
        auto source=read_theme_source();
        if(source!=theme_source) {
            theme_source=std::move(source);theme_changed=now;theme_pending=true;
        }
        // Editors may truncate then write, or replace the file via rename.
        if(theme_pending && now-theme_changed>=200) {
            theme_pending=false;action(6);
        }
    }
    void restore_state() {
        if(o.state.empty())return;
        std::ifstream file(o.state);
        unsigned schema=0,core=0;size_t selected=0;std::string path;
        if(!(file>>schema>>selected>>core>>std::quoted(path)) || schema!=1 || core>=3)return;
        model.core=core;
        if(!model.games.empty()) {
            model.selected=std::min(selected,model.games.size()-1);
            for(size_t i=0;i<model.games.size();i++)
                if(!path.empty() && model.games[i].path==fs::path(path)){model.selected=i;break;}
        }
        int page=0,section=0,focus=0;
        if(file>>page>>section>>focus && page>=0 && page<=7 && section>=0 && section<=5 && focus>=0 && focus<=4)
            settings={static_cast<Page>(page),section,focus};
        if(settings.page==Page::Power){settings.section=1;}
        if(settings.page==Page::Placeholder && settings.section==1)settings.page=Page::Power;
        if(settings.page==Page::Appearance || settings.page==Page::Display)settings.section=0;
        if(settings.page==Page::Display)settings.focus=0;
    }
    void save_state() {
        if(o.state.empty())return;
        try {
            if(!o.state.parent_path().empty())fs::create_directories(o.state.parent_path());
            auto pending=o.state.string()+".tmp";
            std::ofstream file(pending);
            file<<1<<' '<<model.selected<<' '<<model.core<<' '
                <<std::quoted(model.games.empty()?std::string():model.games[model.selected].path.string())<<'\n';
            if(settings.page!=Page::Library && settings.page!=Page::Keyboard)file<<static_cast<int>(settings.page)<<' '<<settings.section<<' '<<settings.focus<<'\n';
            file.close();if(!file)throw std::runtime_error("Cannot save launcher state");
            fs::rename(pending,o.state);
        }catch(const std::exception& e){std::cerr<<"State: "<<e.what()<<std::endl;}
    }
    void color(SDL_Renderer* r, const std::string& key) {
        auto c = theme.at(key); s.SetRenderDrawColor(r,c.r,c.g,c.b,255);
    }
    void rect(SDL_Renderer* r, int x,int y,int w,int h, const std::string& key, bool outline = false) {
        color(r,key); SDL_Rect box{x,y,w,h};
        if (outline) s.RenderDrawRect(r,&box); else s.RenderFillRect(r,&box);
    }
    int point_size(int size){return size==1?16:size==2?20:size==3?28:36;}
    void text(SDL_Renderer* r, std::string value, int x,int y,int size, const std::string& key, int max_chars = 80) {
        if(!preferences.pixel && font_ready){rounded.draw(r,value,x,y,point_size(size),theme.at(key),max_chars,std::max(1,612-x));return;}
        y+=size*3; // Match the rounded font's visible top, rather than its line-box top.
        color(r,key); int n=0;
        for (unsigned char c : value) {
            if ((c & 0xc0) == 0x80) continue;
            if (++n > max_chars) break;
            char ch = c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : static_cast<char>(c);
            auto it = glyphs.find(ch); if (it == glyphs.end()) it = glyphs.find('?');
            for (int row=0;row<7;row++) for (int col=0;col<5;col++) {
                if (it->second[row] & (1 << (4-col))) {
                    SDL_Rect p{x+col*size,y+row*size,size,size}; s.RenderFillRect(r,&p);
                }
            }
            x += size*6;
        }
    }
    void centered(SDL_Renderer* r, const std::string& value,int y,int size,const std::string& key,int limit=48) {
        auto count = std::min(static_cast<int>(value.size()),limit);
        if(!preferences.pixel && font_ready){auto width=std::min(rounded.width(value,point_size(size),limit),584.0f);text(r,value,static_cast<int>(320-width/2),y,size,key,limit);return;}
        count=std::min(count,584/(size*6));limit=count;
        text(r,value,320-count*size*3,y,size,key,limit);
    }
    void label(SDL_Renderer* r,const std::string& value,int x,int y,int pixels,const std::string& key,int width=560) {
        if(!preferences.pixel && font_ready){rounded.draw(r,value,x,y,pixels,theme.at(key),1000,width);return;}
        int scale=std::max(2,(pixels+6)/10);
        text(r,value,x,y,scale,key,std::min(80,width/(scale*6)));
    }
    void round_rect(SDL_Renderer* r,int x,int y,int w,int h,int radius,const std::string& key) {
        color(r,key);SDL_Rect center{x+radius,y,w-2*radius,h};s.RenderFillRect(r,&center);
        for(int dy=0;dy<radius;dy++) {
            int inset=radius-static_cast<int>(std::sqrt(radius*radius-(radius-dy-0.5)*(radius-dy-0.5)));
            SDL_Rect a{x+inset,y+dy,w-inset*2,1},b{x+inset,y+h-1-dy,w-inset*2,1};
            s.RenderFillRect(r,&a);s.RenderFillRect(r,&b);
        }
        rect(r,x,y+radius,w,h-radius*2,key);
    }
    void ring(SDL_Renderer* r,int x,int y,int radius,const std::string& key) {
        color(r,key);
        for(int i=0;i<48;i++){double a=i*6.2831853/48,b=(i+1)*6.2831853/48;s.RenderDrawLine(r,x+int(std::cos(a)*radius),y+int(std::sin(a)*radius),x+int(std::cos(b)*radius),y+int(std::sin(b)*radius));}
    }
    void icon(SDL_Renderer* r,int kind,int x,int y,const std::string& key) {
        color(r,key);
        if(kind==0){ring(r,x,y,11,key);ring(r,x-4,y-3,2,key);ring(r,x+4,y-3,2,key);ring(r,x,y+5,2,key);}
        else if(kind==1){ring(r,x,y+2,10,key);rect(r,x-3,y-12,6,15,"panel");color(r,key);s.RenderDrawLine(r,x,y-12,x,y+1);}
        else if(kind==2){rect(r,x-11,y-9,22,18,key,true);s.RenderDrawLine(r,x-4,y-9,x-4,y+9);s.RenderDrawLine(r,x+4,y-9,x+4,y+9);}
        else if(kind==3){rect(r,x-10,y-4,5,8,key);s.RenderDrawLine(r,x-5,y-4,x+1,y-9);s.RenderDrawLine(r,x+1,y-9,x+1,y+9);s.RenderDrawLine(r,x+1,y+9,x-5,y+4);s.RenderDrawLine(r,x+6,y-7,x+10,y);s.RenderDrawLine(r,x+10,y,x+6,y+7);}
        else if(kind==4){for(int i=0;i<3;i++){int d=5+i*5;s.RenderDrawLine(r,x-d,y-i*5,x,y-5-i*5);s.RenderDrawLine(r,x,y-5-i*5,x+d,y-i*5);}rect(r,x-1,y+5,3,3,key);}
        else {ring(r,x,y,10,key);s.RenderDrawLine(r,x,y-1,x,y+6);rect(r,x-1,y-6,2,2,key);}
    }
    void gear(SDL_Renderer* r,int x,int y) {
        ring(r,x,y,9,"accent");ring(r,x,y,3,"accent");color(r,"accent");
        for(int i=0;i<8;i++){double a=i*6.2831853/8;s.RenderDrawLine(r,x+int(std::cos(a)*9),y+int(std::sin(a)*9),x+int(std::cos(a)*13),y+int(std::sin(a)*13));}
    }
    void settings_header(SDL_Renderer* r,int y,const std::string& title) {
        round_rect(r,28,y+22,48,44,12,"panel");color(r,"ink");
        s.RenderDrawLine(r,57,y+35,47,y+44);s.RenderDrawLine(r,47,y+44,57,y+53);s.RenderDrawLine(r,47,y+44,63,y+44);
        label(r,title,94,y+24,28,"ink",480);rect(r,28,y+76,584,1,"line");
    }
    void choice(SDL_Renderer* r,int x,int y,const std::string& title,bool selected,bool focused) {
        round_rect(r,x,y,280,48,12,focused?"accent":"line");
        round_rect(r,x+2,y+2,276,44,10,selected?"accent":"panel");
        label(r,title,x+18,y+9,20,selected?"on_accent":"ink",222);
        if(selected){color(r,"on_accent");s.RenderDrawLine(r,x+248,y+24,x+253,y+29);s.RenderDrawLine(r,x+253,y+29,x+263,y+18);}
    }
    void settings_top(SDL_Renderer* r,int y) {
        int section=settings.section;
        const auto& info=sections[section];
        if(section==2) {
            label(r,settings.page==Page::Scraper || settings.page==Page::Keyboard?"ScreenScraper":"Library",30,y+95,16,"muted");
            label(r,settings.page==Page::Scraper || settings.page==Page::Keyboard?"Your account. Your artwork.":"Your library. Your way.",30,y+133,28,"ink");
            label(r,"Cartridge icons are available offline.",30,y+186,20,"muted");
            round_rect(r,28,y+235,584,128,6,"panel");
            label(r,settings.page==Page::Scraper || settings.page==Page::Keyboard?"ScreenScraper":"Default emulator",48,y+249,16,"muted");
            label(r,settings.page==Page::Scraper || settings.page==Page::Keyboard?(service["username"].empty()?"Sign in to fetch box art":service["username"]):core_names[preferences.core],48,y+279,24,"ink",520);
            label(r,service["state"]=="scraping"?service["completed"]+" / "+service["total"]+" games processed":"Box art stays available after you disconnect.",48,y+324,16,"muted",530);
            label(r,"Playtime is recorded when you play.",30,y+407,20,"ink");return;
        }
        if(settings.page==Page::Power || (settings.page==Page::Settings && section==1)) {
            label(r,"Power",30,y+95,16,"muted");label(r,"More play.",30,y+122,36,"ink");
            label(r,"Find your balance of speed and battery life.",30,y+174,20,"muted");
            round_rect(r,28,y+220,584,144,18,"panel");
            label(r,"Battery",50,y+236,16,"muted");
            label(r,hardware.battery<0?"Unavailable":std::to_string(hardware.battery)+"%",50,y+262,36,"ink",265);
            label(r,o.preview?"Preview battery":hardware.charging?"Charging / full":"On battery",346,y+249,20,"accent",240);
            label(r,power_names[system_preferences.power],346,y+289,20,"ink",240);
            round_rect(r,50,y+324,540,12,6,"slot");
            if(hardware.battery>0)round_rect(r,50,y+324,std::max(12,540*hardware.battery/100),12,6,"accent");
            label(r,"Sleep controls apply while you're in the menu.",30,y+389,20,"ink");
            label(r,o.preview?"Hardware changes are simulated here.":"Games keep their stock sleep handling.",30,y+429,16,"muted");return;
        }
        if(settings.page==Page::Display) {
            label(r,"Theme & display",30,y+95,16,"muted");label(r,"A comfortable glow.",30,y+122,36,"ink");
            label(r,"Set the brightness of both screens together.",30,y+174,20,"muted");
            round_rect(r,28,y+220,584,144,18,"panel");
            label(r,std::to_string(system_preferences.brightness)+"%",50,y+241,36,"ink");
            label(r,hardware.brightness?"Both screens":"Unavailable",300,y+251,24,"accent",280);
            label(r,"Choose a level that feels right for you.",50,y+314,20,"muted");
            label(r,o.preview?"Desktop preview - brightness is simulated.":"Your level is remembered for the next session.",30,y+401,20,"ink");return;
        }
        label(r,settings.page==Page::Settings?"Settings":info.title,30,y+95,16,"muted");
        label(r,section==0?"Make it yours.":info.title,30,y+122,36,"ink");
        label(r,info.description,30,y+170,20,"muted");
        if(section==0) {
            round_rect(r,74,y+220,492,154,18,"line");
            round_rect(r,76,y+218,488,152,16,"panel");
            round_rect(r,94,y+237,70,112,10,"slot");
            round_rect(r,101,y+244,56,43,5,"background");round_rect(r,101,y+299,56,43,5,"background");
            rect(r,119,y+255,20,20,"accent");rect(r,109,y+314,11,15,"accent");rect(r,126,y+314,11,15,"line");rect(r,143,y+314,7,15,"line");
            label(r,"Hello, player.",188,y+240,28,"ink",346);
            label(r,"A home for every cartridge.",188,y+280,20,"muted",346);
            label(r,preferences.dark?"Dark":"Light",188,y+321,16,"accent",100);
            label(r,preferences.pixel?"Pixel classic":"DS rounded",300,y+321,16,"muted",240);
            label(r,"Changes appear instantly.",30,y+399,20,"ink");
            label(r,"Your choices are remembered.",30,y+430,16,"muted");
        }else {
            round_rect(r,28,y+220,584,146,18,"panel");
            round_rect(r,50,y+244,52,52,14,"slot");icon(r,section,76,y+270,"accent");
            label(r,"Coming later",120,y+244,24,"ink");
            label(r,"This section is a preview of what's next.",120,y+280,16,"muted",465);
            label(r,"No setup needed here yet.",50,y+326,20,"muted");
            label(r,"Start with theme and display.",30,y+405,20,"ink");
        }
    }
    std::vector<std::pair<std::string,std::string>> placeholder_rows() {
        switch(settings.section) {
            case 1:return {{"Lid sleep","Pause your handheld when you close it"},{"Display brightness","A comfortable screen in every room"},{"Battery","Charge level and power preferences"}};
            case 2:return {{"Favourites","Keep your favourite cartridges together"},{"Recent games","Pick up where you left off"},{"Game preferences","Choose an emulator for each game"}};
            case 3:return {{"Volume","One simple control for your audio"},{"Audio output","Speakers and connected headphones"},{"Menu sounds","A little feedback with every tap"}};
            case 4:return {{"Wi-Fi","Find and join your network"},{"Connections","Manage paired accessories"}};
            default:return {{"Updates","New features for your handheld"},{"Device information","Your version and device details"},{"Recovery","A simple way back when you need it"}};
        }
    }
    void library_settings(SDL_Renderer* r,int y) {
        settings_header(r,y,"Library");label(r,"Default emulator",30,y+93,16,"muted");
        for(unsigned i=0;i<3;i++) {
            int x=28+int(i)*198;bool selected=preferences.core==i;
            round_rect(r,x,y+126,188,48,5,selected?"accent":settings.focus==0?"focus":"panel");
            label(r,core_names[i],x+14,y+136,20,selected?"on_accent":"ink",163);
        }
        label(r,"Used when you start a game.",30,y+191,16,"muted");
        round_rect(r,28,y+233,584,78,6,settings.focus==1?"focus":"panel");icon(r,2,57,y+269,"accent");
        label(r,"Artwork & ScreenScraper",86,y+243,24,"ink",492);
        label(r,"Account, box art and scraping",86,y+280,16,"muted",492);
        round_rect(r,28,y+335,584,58,6,settings.focus==2?"focus":"panel");
        label(r,"Favourites & recent games",46,y+344,20,"ink",445);label(r,"Later",530,y+350,16,"muted",70);
        label(r,"Left / Right Emulator     A Open     B Back",30,y+443,16,"muted");
    }
    void scraper_settings(SDL_Renderer* r,int y) {
        settings_header(r,y,"ScreenScraper");
        std::string state=service["state"],message=service["message"];
        label(r,!service_notice.empty()?service_notice:message.empty()?"Sign in with your ScreenScraper account":message,30,y+88,16,"muted",580);
        for(int field=0;field<2;field++) {
            int row=y+122+field*70;round_rect(r,28,row,584,60,5,settings.focus==field?"focus":"panel");
            label(r,field==0?"Username":"Password",44,row+2,16,"muted",160);
            std::string value=field==0?(username.empty()?service["username"]:username):std::string(password.size(),'*');
            label(r,value.empty()?"Tap to enter":value,44,row+26,20,"ink",548);
        }
        const bool busy=worker>0 && worker_action!="index";
        round_rect(r,28,y+272,584,43,5,settings.focus==2?"accent":"panel");
        label(r,busy && worker_action=="login"?"Signing in...":"Sign in",46,y+279,20,settings.focus==2?"on_accent":"ink",540);
        round_rect(r,28,y+327,584,43,5,settings.focus==3?"accent":"panel");
        label(r,busy && worker_action=="scrape"?"Stop scraping":"Fetch missing artwork",46,y+334,20,settings.focus==3?"on_accent":"ink",540);
        round_rect(r,28,y+382,584,43,5,settings.focus==4?"focus":"panel");label(r,"Sign out",46,y+389,20,"ink",520);
        label(r,"A Choose     B Back",30,y+447,16,"muted");
    }
    void keyboard_screen(SDL_Renderer* r,int y) {
        settings_header(r,y,edit_field==0?"Username":"Password");
        round_rect(r,28,y+91,584,58,5,"panel");
        std::string value=edit_field==0?edit_buffer:std::string(RoundedFont::codepoints(edit_buffer).size(),'*');
        if(value.size()>46)value=value.substr(value.size()-46);
        label(r,value.empty()?"Enter here":value,44,y+106,24,"ink",545);
        auto rows=keyboard_rows();
        for(int row=0;row<4;row++) {
            int left=(640-int(rows[row].size())*56+4)/2;
            for(size_t col=0;col<rows[row].size();col++) {
                bool chosen=key_row==row && key_col==static_cast<int>(col);int x=left+int(col)*56;
                round_rect(r,x,y+181+row*49,52,43,4,chosen?"accent":"panel");
                label(r,std::string(1,rows[row][col]),x+16,y+188+row*49,20,chosen?"on_accent":"ink",35);
            }
        }
        static const std::array<int,4> x={28,136,336,480},w={94,188,132,132};
        static const std::array<std::string,4> labels={"Shift","Space","Delete","Done"};
        for(int col=0;col<4;col++){bool active=key_row==4 && key_col==col;round_rect(r,x[col],y+391,w[col],45,4,active?"accent":"panel");label(r,labels[col],x[col]+12,y+402,16,active?"on_accent":"ink",w[col]-20);}
        label(r,"A Type     Start Done     B Cancel",30,y+447,16,"muted");
    }
    void settings_bottom(SDL_Renderer* r,int y) {
        rect(r,0,y,640,480,"background");
        if(settings.page==Page::LibraryOptions){library_settings(r,y);return;}
        if(settings.page==Page::Scraper){scraper_settings(r,y);return;}
        if(settings.page==Page::Keyboard){keyboard_screen(r,y);return;}
        if(settings.page==Page::Settings) {
            settings_header(r,y,"Settings");
            for(int i=0;i<6;i++) {
                int row=y+84+i*56;bool active=settings.section==i;
                round_rect(r,28,row,584,52,12,active?"accent":"panel");
                if(active)round_rect(r,30,row+2,580,48,10,"focus");
                icon(r,i,58,row+26,active?"accent":"muted");
                label(r,sections[i].title,84,row+1,24,"ink",370);
                label(r,sections[i].subtitle,84,row+30,16,"muted",380);
                round_rect(r,482,row+13,94,26,9,i<=1?"slot":"background");
                label(r,i==0?(preferences.dark?"Dark":"Light"):i==1?"Ready":"Later",496,row+14,16,i<=1?"accent":"muted",75);
                color(r,"muted");s.RenderDrawLine(r,592,row+21,597,row+26);s.RenderDrawLine(r,597,row+26,592,row+31);
            }
            label(r,"A Open     B Back",30,y+441,16,"muted");
        }else if(settings.page==Page::Appearance) {
            settings_header(r,y,"Theme & display");
            label(r,"Colour mode",30,y+88,16,"muted");
            choice(r,28,y+118,"Light",!preferences.dark,settings.focus==0);
            choice(r,332,y+118,"Dark",preferences.dark,settings.focus==0);
            label(r,"Typeface",30,y+189,16,"muted");
            choice(r,28,y+220,"DS rounded",!preferences.pixel,settings.focus==1);
            choice(r,332,y+220,"Pixel classic",preferences.pixel,settings.focus==1);
            label(r,"Soft and familiar, or crisp and classic.",30,y+284,16,"muted");
            round_rect(r,28,y+309,584,48,12,settings.focus==2?"focus":"panel");
            label(r,"Screen brightness",46,y+319,20,"ink",400);
            label(r,hardware.brightness?std::to_string(system_preferences.brightness)+"%":"N/A",532,y+320,16,"accent",70);
            round_rect(r,28,y+366,584,42,12,settings.focus==3?"focus":"panel");
            label(r,"Home layout & wallpaper",46,y+373,20,"ink",440);label(r,"Later",534,y+377,16,"muted",70);
            label(r,"Saved automatically",30,y+419,16,"accent");
            label(r,settings.focus==2?"A Open brightness     B Back":"Left / Right Choose     A Switch     B Back",30,y+449,16,"muted");
        }else if(settings.page==Page::Power) {
            settings_header(r,y,"Power");
            const std::array<std::string,4> titles={"Power mode","Menu idle sleep","Sleep on lid close","Sleep now"};
            const std::array<std::string,4> subtitles={"Speed and battery life","Since your last input","While in the menu","Power button to wake"};
            const std::array<bool,4> enabled={hardware.power,hardware.sleep,hardware.lid,hardware.sleep};
            const std::array<std::string,4> values={power_names[system_preferences.power],idle_name(system_preferences.idle),system_preferences.lid?"On":"Off",o.preview?"Simulate":"Sleep"};
            for(int i=0;i<4;i++) {
                int row=y+88+i*78;bool focused=settings.focus==i;
                round_rect(r,28,row,584,70,12,focused?"accent":"panel");
                if(focused)round_rect(r,30,row+2,580,66,10,"focus");
                label(r,titles[i],46,row+6,20,enabled[i]?"ink":"muted",270);
                label(r,subtitles[i],46,row+43,16,"muted",280);
                label(r,enabled[i]?values[i]:"Unavailable",350,row+21,20,enabled[i]?"accent":"muted",205);
                if(enabled[i]){label(r,i==3?">":"<",322,row+22,20,"muted",20);label(r,">",582,row+22,20,"muted",20);}
            }
            label(r,status=="READY"?"Saved automatically":status,30,y+416,16,"accent");
            label(r,"Left / Right Choose     A Switch     B Back",30,y+449,16,"muted");
        }else if(settings.page==Page::Display) {
            settings_header(r,y,"Screen brightness");
            label(r,"Both screens",30,y+105,24,"ink");
            label(r,hardware.brightness?std::to_string(system_preferences.brightness)+"%":"Unavailable",hardware.brightness?470:400,y+105,20,"accent",210);
            round_rect(r,28,y+163,584,122,16,"panel");
            rect(r,56,y+217,528,8,"slot");
            int position=56+(system_preferences.brightness-5)*528/95;
            rect(r,56,y+217,std::max(1,position-56),8,"accent");
            round_rect(r,std::clamp(position-12,44,572),y+205,24,32,8,hardware.brightness?"accent":"muted");
            label(r,"5%",56,y+251,16,"muted",100);label(r,"100%",533,y+251,16,"muted",70);
            label(r,"Tap the slider or use Left / Right.",30,y+318,20,"ink");
            label(r,"Each step changes brightness by 5%.",30,y+356,16,"muted");
            label(r,status=="READY"?"Saved automatically":status,30,y+416,16,"accent");
            label(r,"Left / Right Adjust     B Back",30,y+449,16,"muted");
        }else {
            settings_header(r,y,sections[settings.section].title);
            label(r,"Coming later",30,y+90,16,"accent");
            auto rows=placeholder_rows();
            for(size_t i=0;i<rows.size();i++) {
                int row=y+128+static_cast<int>(i)*82;
                round_rect(r,28,row,584,72,14,"panel");
                label(r,rows[i].first,46,row+7,24,"ink",445);
                label(r,rows[i].second,46,row+42,16,"muted",530);
                label(r,"Later",535,row+13,16,"muted",68);
            }
            label(r,"Nothing to configure yet.     B Back",30,y+443,16,"muted");
        }
    }
    void cart(SDL_Renderer* r,int x,int y,int width,int height,bool selected,int number) {
        rect(r,x+4,y+5,width,height,"line");
        rect(r,x,y,width,height,selected ? "accent" : "slot");
        rect(r,x+10,y+12,width-20,height-37,"panel");
        rect(r,x+width-27,y,27,14,"background");
        text(r,"DS",x+18,y+22,3,"ink");
        text(r,std::to_string(number),x+18,y+56,2,"muted");
        for(int i=0;i<5;i++) rect(r,x+13+i*(width-20)/5,y+height-18,6,10,"ink");
    }
    std::string clock() {
        auto now=std::time(nullptr); char buf[8];
        std::strftime(buf,sizeof(buf),"%H:%M",std::localtime(&now)); return buf;
    }
    fs::path item_folder(size_t index) {return o.data/"library"/game_key(model.games[index].path);}
    std::string game_title(size_t index) {
        auto& game=model.games[index];if(game.demo)return game.title;
        auto info=fields(item_folder(index)/"info.ini");if(!info["title"].empty())return info["title"];
        auto banner=fields(item_folder(index)/"banner.ini");if(!banner["title"].empty())return banner["title"];
        auto title=media.banner(game.path).title;return title.empty()?game.title:title;
    }
    void pattern(SDL_Renderer* r,int y,int height) {
        auto bg=theme.at("background"),fg=theme.at("line");
        s.SetRenderDrawColor(r,(bg.r*19+fg.r)/20,(bg.g*19+fg.g)/20,(bg.b*19+fg.b)/20,255);
        for(int row=y;row<y+height;row+=4)s.RenderDrawLine(r,0,row,639,row);
    }
    void game_icon(SDL_Renderer* r,size_t index,int x,int y,int size) {
        auto& game=model.games[index];SDL_Rect box{x,y,size,size};
        if(!game.demo && (media.draw(r,game.path,box,true) || media.draw(r,item_folder(index)/"icon.bmp",box)))return;
        if(!game.demo){round_rect(r,x+size/8,y+size/8,size*3/4,size*3/4,4,"slot");label(r,"DS",x+size/5,y+size/3,24,"muted",size*3/4);return;}
        // Original preview symbols, never Nintendo artwork.
        int q=std::max(2,size/8);round_rect(r,x,y,size,size,4,"focus");
        switch(index%5) {
            case 0:rect(r,x+q*2,y+q*2,q*4,q*4,"accent");rect(r,x+q*3,y+q,q*2,q*6,"ink");rect(r,x+q,y+q*3,q*6,q*2,"ink");rect(r,x+q*3,y+q*3,q*2,q*2,"on_accent");break;
            case 1:for(int i=0;i<4;i++)rect(r,x+q*(1+(i%2)*3),y+q*(1+(i/2)*3),q*2,q*2,i%2?"accent":"ink");break;
            case 2:rect(r,x+q*3,y+q,q*2,q*6,"ink");for(int i=0;i<3;i++)rect(r,x+q*3,y+q*(1+i*2),q*2,q,"on_accent");break;
            case 3:ring(r,x+size/2,y+size/2,size/3,"accent");rect(r,x+q*3,y+q*3,q*2,q*2,"ink");break;
            default:rect(r,x+q*2,y+q*2,q*4,q*4,"accent");rect(r,x+q*3,y+q*2,q,q*4,"on_accent");rect(r,x+q*2,y+q*3,q*4,q,"on_accent");break;
        }
    }
    void wrapped(SDL_Renderer* r,const std::string& value,int x,int y,int pixels,int width,int rows=3) {
        std::istringstream words(value);std::string word,line;int row=0;
        auto measure=[&](const std::string& str){return preferences.pixel?float(str.size()*std::max(2,(pixels+6)/10)*6):rounded.width(str,pixels);};
        while(words>>word) {
            std::string candidate=line.empty()?word:line+" "+word;
            if(!line.empty() && measure(candidate)>width){label(r,line,x,y+row*(pixels+7),pixels,"ink",width);if(++row>=rows)return;line=word;}else line=candidate;
        }
        if(!line.empty() && row<rows)label(r,line,x,y+row*(pixels+7),pixels,"ink",width);
    }
    void top(SDL_Renderer* r,int y) {
        rect(r,0,y,640,480,"background");pattern(r,y+64,416);
        label(r,clock(),28,y+13,24,"ink",180);
        auto now=std::time(nullptr);char date[40];std::strftime(date,sizeof(date),"%a, %d %b",std::localtime(&now));
        label(r,date,246,y+18,16,"muted",200);
        label(r,"RGDS",480,y+18,16,"muted",65);
        rect(r,559,y+18,41,20,"muted",true);rect(r,600,y+24,3,8,"muted");
        if(hardware.battery>=0)rect(r,562,y+21,35*hardware.battery/100,14,"accent");
        rect(r,0,y+61,640,1,"line");
        if(probe) {
            label(r,"Top display",30,y+96,36,"ink");label(r,top_name,30,y+145,20,"accent");
            for(int i=0;i<7;i++)rect(r,44+i*78,y+210,60,56,i%2?"accent":"ink");
            label(r,"Check panel order and controller input",30,y+340,20,"muted");label(r,"B / Escape returns to stock",30,y+410,20,"ink");return;
        }
        if(settings.page!=Page::Library){settings_top(r,y+int(std::max(0.0,1.0-(s.GetTicks()-page_at)/150.0)*5));return;}
        if(model.games.empty()) {
            label(r,"Your next adventure starts here.",30,y+160,28,"ink");label(r,"Add DS games to your library.",30,y+212,20,"muted");return;
        }
        auto item=item_folder(model.selected);auto info=fields(item/"info.ini");auto stats=fields(item/"stats.ini");
        round_rect(r,28,y+92,282,282,5,"line");round_rect(r,30,y+90,278,282,4,"panel");
        bool art=media.draw(r,item/"box.bmp",{40,y+100,258,258},false,static_cast<unsigned char>(std::min(255u,100u+(s.GetTicks()-selected_at)*2)));
        if(!art) {
            rect(r,40,y+100,258,35,"slot");label(r,"NINTENDO DS",55,y+106,16,"ink",230);
            game_icon(r,model.selected,120,y+175,100);
            label(r,"Box art will appear here",65,y+326,16,"muted",222);
        }
        label(r,"Nintendo DS",338,y+99,16,"muted",274);
        wrapped(r,game_title(model.selected),338,y+132,28,274,3);
        label(r,"Time played",338,y+274,16,"muted",270);
        uint64_t seconds=0;try{seconds=std::stoull(stats["seconds"]);}catch(...){}
        std::string time=seconds==0?"Not played yet":seconds<60?"Less than a minute":seconds<3600?std::to_string(seconds/60)+" min":std::to_string(seconds/3600)+"h "+std::to_string((seconds/60)%60)+"m";
        label(r,time,338,y+301,seconds==0 || seconds<60?20:28,"ink",274);
        if(!info["year"].empty())label(r,info["year"],338,y+344,16,"muted",270);
        rect(r,28,y+400,584,1,"line");
        label(r,status.find("ERROR")!=std::string::npos?status:info["developer"].empty()?"Ready for your next adventure":info["developer"],30,y+416,20,"ink",550);
        label(r,art?(info["provider"]=="ScreenScraper"?"Artwork from ScreenScraper":"Cached box art"):"Game icons from your cartridges",30,y+450,16,"muted",550);
    }
    void bottom(SDL_Renderer* r,int y) {
        rect(r,0,y,640,480,"background");
        if(!probe && settings.page!=Page::Library){settings_bottom(r,y+int(std::max(0.0,1.0-(s.GetTicks()-page_at)/150.0)*8));return;}
        pattern(r,y+85,305);
        label(r,probe?"Bottom display":"Nintendo DS",28,y+29,24,"ink",440);
        label(r,probe?bottom_name:std::to_string(model.games.size())+" games",28,y+65,16,"muted",480);
        round_rect(r,550,y+28,54,42,5,"panel");gear(r,577,y+49);
        if(probe) {
            for(int row=0;row<4;row++)for(int col=0;col<8;col++)rect(r,32+col*72,y+146+row*46,66,40,(col+row)%2?"slot":"panel");
            if(touch_x>=0){color(r,"accent");s.RenderDrawLine(r,touch_x-20,y+touch_y,touch_x+20,y+touch_y);s.RenderDrawLine(r,touch_x,y+touch_y-20,touch_x,y+touch_y+20);}
            label(r,last_input,30,y+355,20,"ink");label(r,"Home / F1 switches probe",30,y+410,16,"muted");return;
        }
        if(model.games.empty()){label(r,"Your cartridges go here",146,y+207,24,"muted");return;}
        rect(r,0,y+283,640,2,"line");
        SDL_Rect clip{0,y+113,640,186};s.RenderSetClipRect(r,&clip);
        int first=static_cast<int>(std::floor(carousel.position))-3;
        for(int offset=0;offset<7;offset++) {
            int index=first+offset;double distance=index-carousel.position;
            int x=static_cast<int>(std::round(320+distance*Carousel::spacing))-58;
            bool selected=index==static_cast<int>(model.selected);
            int lift=selected?static_cast<int>(std::max(0.0,1.0-(s.GetTicks()-selected_at)/190.0)*4):0;
            int row=y+151-lift;
            if(index<0 || index>=static_cast<int>(model.games.size())){round_rect(r,x,row,116,116,5,"line");round_rect(r,x+2,row+2,112,112,3,"background");continue;}
            round_rect(r,x,row+4,116,116,5,"line");round_rect(r,x,row,116,116,5,"line");round_rect(r,x+2,row+2,112,112,4,"panel");
            game_icon(r,static_cast<size_t>(index),x+26,row+26,64);
            if(selected) {
                color(r,"accent");
                for(int dx:{0,1}) {
                    s.RenderDrawLine(r,x-7-dx,row-7,x+12,row-7);s.RenderDrawLine(r,x-7-dx,row-7,x-7-dx,row+12);
                    s.RenderDrawLine(r,x+103,row-7-dx,x+123,row-7-dx);s.RenderDrawLine(r,x+123,row-7,x+123,row+12);
                    s.RenderDrawLine(r,x-7,row+103,x-7,row+123+dx);s.RenderDrawLine(r,x-7,row+123+dx,x+12,row+123+dx);
                    s.RenderDrawLine(r,x+103,row+123+dx,x+123,row+123+dx);s.RenderDrawLine(r,x+123,row+103,x+123,row+123+dx);
                }
            }
        }
        s.RenderSetClipRect(r,nullptr);
        centered(r,game_title(model.selected),y+301,2,"ink",65);
        // A map of the visible range, not a numeric pager or play-progress meter.
        round_rect(r,28,y+347,584,32,5,"panel");
        int ticks=std::min<int>(100,model.games.size());
        color(r,"line");for(int i=0;i<ticks;i++){int x=36+(ticks>1?i*568/(ticks-1):284);s.RenderDrawLine(r,x,y+357,x,y+369);}
        double fraction=model.games.size()>1?carousel.position/(model.games.size()-1):0.5;
        int width=std::clamp(568*5/static_cast<int>(model.games.size()),34,568);
        int left=std::clamp(static_cast<int>(36+fraction*568)-width/2,36,604-width);
        round_rect(r,left,y+351,width,24,4,"focus");rect(r,std::clamp(int(36+fraction*568)-1,36,601),y+355,3,16,"accent");
        round_rect(r,222,y+409,196,48,5,"accent");label(r,"Start",285,y+416,24,"on_accent",110);
        round_rect(r,28,y+409,96,48,5,"panel");label(r,"B Exit",42,y+422,16,"muted",85);
        round_rect(r,518,y+409,94,48,5,"panel");icon(r,2,542,y+432,"accent");label(r,"Art",562,y+422,16,"ink",46);
    }
    int display(const std::string& wanted,int fallback) {
        int count=s.GetNumVideoDisplays();
        for(int i=0;i<count;i++) {
            const char* name=s.GetDisplayName(i);
            if((name && std::string(name).find(wanted)!=std::string::npos) || wanted==std::to_string(i)) return i;
        }
        if(fallback>=count) throw std::runtime_error("Two SDL displays required. Use --top-display/--bottom-display after capturing inventory.");
        return fallback;
    }
    Surface create(const char* title,int index,int h,bool device) {
        Uint32 flags=device ? SDL_WINDOW_FULLSCREEN_DESKTOP : SDL_WINDOW_SHOWN;
        auto* w=s.CreateWindow(title,SDL_WINDOWPOS_CENTERED_DISPLAY(index),SDL_WINDOWPOS_CENTERED_DISPLAY(index),640,h,flags);
        if(!w) throw std::runtime_error(std::string("Window creation: ")+s.GetError());
        auto* r=s.CreateRenderer(w,-1,SDL_RENDERER_ACCELERATED|((!device || surfaces.empty())?SDL_RENDERER_PRESENTVSYNC:0));
        if(!r) r=s.CreateRenderer(w,-1,SDL_RENDERER_SOFTWARE);
        if(!r) {s.DestroyWindow(w);throw std::runtime_error(std::string("Renderer creation: ")+s.GetError());}
        s.RenderSetLogicalSize(r,640,h);
        return {w,r,s.GetWindowID(w)};
    }
    void open() {
        s.SetMainReady(); s.SetHint("SDL_TOUCH_MOUSE_EVENTS","0"); s.SetHint("SDL_MOUSE_TOUCH_EVENTS","0");
        if(s.Init(SDL_INIT_VIDEO|SDL_INIT_JOYSTICK|SDL_INIT_EVENTS)!=0) throw std::runtime_error(s.GetError());
        SDL_version version{}; s.GetVersion(&version);
        std::cout<<"SDL "<<int(version.major)<<'.'<<int(version.minor)<<'.'<<int(version.patch)
            <<" driver="<<(s.GetCurrentVideoDriver()?s.GetCurrentVideoDriver():"unknown")<<std::endl;
        for(int i=0;i<s.GetNumVideoDisplays();i++) {
            SDL_Rect bounds{}; s.GetDisplayBounds(i,&bounds);
            std::cout<<"display "<<i<<" "<<s.GetDisplayName(i)<<" "<<bounds.w<<"x"<<bounds.h<<std::endl;
        }
        if(o.preview) {surfaces.push_back(create("RGDS development preview",0,960,false));top_name="PREVIEW TOP";bottom_name="PREVIEW BOTTOM";}
        else {
            int t=display(o.top,0),b=display(o.bottom,1);
            if(t==b) throw std::runtime_error("Top and bottom resolved to the same display.");
            top_name=s.GetDisplayName(t);bottom_name=s.GetDisplayName(b);
            surfaces.push_back(create("RGDS top",t,480,true));surfaces.push_back(create("RGDS bottom",b,480,true));
        }
        for(int i=0;i<s.NumJoysticks();i++) {
            auto* j=s.JoystickOpen(i);
            if(j) {joysticks.push_back(j);std::cout<<"joystick "<<i<<" "<<s.JoystickName(j)<<std::endl;}
        }
    }
    void close() {
        rounded.clear();media.clear();
        for(auto* j:joysticks)s.JoystickClose(j);joysticks.clear();
        for(auto p:surfaces){s.DestroyRenderer(p.renderer);s.DestroyWindow(p.window);}surfaces.clear();s.Quit();
    }
    void save(SDL_Renderer* r,const fs::path& path) {
        int w=0,h=0;s.GetRendererOutputSize(r,&w,&h);
        auto* image=s.CreateRGBSurfaceWithFormat(0,w,h,32,SDL_PIXELFORMAT_ARGB8888);
        if(!image)throw std::runtime_error(s.GetError());
        if(s.RenderReadPixels(r,nullptr,image->format->format,image->pixels,image->pitch)!=0) {s.FreeSurface(image);throw std::runtime_error(s.GetError());}
        const auto pending=path.string()+".tmp";
        auto* file=s.RWFromFile(pending.c_str(),"wb");
        if(!file){s.FreeSurface(image);throw std::runtime_error("Cannot write screenshot");}
        const int rc=s.SaveBMP_RW(image,file,1);s.FreeSurface(image);
        if(rc!=0)throw std::runtime_error("Screenshot failed");fs::rename(pending,path);
    }
    void draw(bool screenshot=false) {
        if(o.preview) {
            top(surfaces[0].renderer,0);bottom(surfaces[0].renderer,480);
            if(screenshot)save(surfaces[0].renderer,o.screenshot);
            s.RenderPresent(surfaces[0].renderer);
        } else {
            top(surfaces[0].renderer,0);bottom(surfaces[1].renderer,0);
            if(screenshot) {
                save(surfaces[0].renderer,o.screenshot.string()+"-top.bmp");
                save(surfaces[1].renderer,o.screenshot.string()+"-bottom.bmp");
            }
            for(auto p:surfaces)s.RenderPresent(p.renderer);
        }
        dirty=false;
    }
    void launch() {
        if(model.games.empty() || probe) return;
        if(o.preview || model.games[model.selected].demo || !o.allow_launch) {status="PREVIEW: "+std::string(core_names[preferences.core])+" SELECTED";return;}
        if(o.app.empty() || o.data.empty()) {status="LAUNCH NEEDS APP AND DATA PATHS";return;}
        std::vector<std::string> args={"python3",(o.app/"runtime/emulators.py").string(),"launch","--app",o.app.string(),
            "--data",o.data.string(),"--core",core_ids[preferences.core],"--rom",model.games[model.selected].path.string()};
        stop_worker();save_state();
        close();
        pid_t child=fork();
        if(child==0) {
            std::vector<char*> argv;for(auto& a:args)argv.push_back(a.data());argv.push_back(nullptr);
            execvp(argv[0],argv.data());_exit(127);
        }
        int result=0;
        if(child<0)result=127;
        else {
            while(waitpid(child,&result,WNOHANG)==0) {
                if(stopping)kill(child,SIGTERM);
                usleep(50000);
            }
            result=WIFEXITED(result)?WEXITSTATUS(result):128+(WIFSIGNALED(result)?WTERMSIG(result):0);
        }
        if(stopping){quit=true;return;}
        open();last_activity=s.GetTicks();
        if(system_preferences.power>0 && hardware.power && !system.set("power",system_preferences.power))std::cerr<<"Could not reapply power profile after game"<<std::endl;
        if(hardware.brightness && !system.set("brightness",system_preferences.brightness))std::cerr<<"Could not reapply brightness after game"<<std::endl;
        status=result==0?"BACK HOME":"LAUNCH ERROR "+std::to_string(result)+" - SEE LOG";
    }
    void action(int code) {
        brightness_drag=false;
        last_activity=s.GetTicks();
        if(settings.page==Page::Keyboard) {
            if(code==3)finish_edit(false);else if(code==4)finish_edit(true);else if(code==2)keyboard_choose();
            else {
                if(code==8 || code==9)key_row=std::clamp(key_row+(code==8?-1:1),0,4);
                int columns=key_row==4?4:static_cast<int>(keyboard_rows()[key_row].size());
                key_col=std::clamp(key_col+(code==0?-1:code==1?1:0),0,columns-1);
            }dirty=true;return;
        }
        if(code==7 && !probe){if(settings.page==Page::Library)settings.open();else settings.page=Page::Library;dirty=true;return;}
        if(!probe && settings.page!=Page::Library && code!=5 && code!=6) {
            if(code==3)settings.back();
            else if(code==8 || code==9)settings.move(code==8?-1:1);
            else if(settings.page==Page::Appearance) {
                if(code==0 || code==1)choose_appearance(code);
                else if(code==2){if(settings.focus<2)choose_appearance(settings.focus==0?!preferences.dark:!preferences.pixel);else settings.enter();}
            }else if(settings.page==Page::Power) {
                if(settings.focus<3 && (code==0 || code==1 || code==2))change_system(settings.focus,code==0?-1:1,code==2);
                else if(settings.focus==3 && code==2)sleep_now();
            }else if(settings.page==Page::Display) {
                if(code==0 || code==1 || code==2)change_system(4,code==0?-1:1,code==2);
            }else if(settings.page==Page::LibraryOptions) {
                if(settings.focus==0 && (code==0 || code==1 || code==2)) {
                    preferences.core=code==2?(preferences.core+1)%3:static_cast<unsigned>(std::clamp(int(preferences.core)+(code==0?-1:1),0,2));remember_appearance();
                }else if(settings.focus==1 && code==2)open_scraper();
            }else if(settings.page==Page::Scraper) {
                if(code==2)scraper_action();
            }else if(code==2)settings.enter();
            dirty=true;return;
        }
        if(code==0)navigate(-1);
        else if(code==1)navigate(1);
        else if(code==2)launch();
        else if(code==3)quit=true;
        else if(code==4){settings.section=2;settings.page=Page::LibraryOptions;settings.focus=0;}
        else if(code==5)probe=!probe;
        else if(code==6) {
            try{reload_themes();status="THEME RELOADED";std::cout<<"Theme reloaded: "<<o.theme<<std::endl;}
            catch(const std::exception& e){status="THEME ERROR - RETAINED CURRENT";std::cerr<<e.what()<<std::endl;}
        }
        dirty=true;
    }
    void touch(int x,int y) {
        last_activity=s.GetTicks();
        brightness_drag=false;
        touch_x=x;touch_y=y;last_input="TOUCH "+std::to_string(x)+", "+std::to_string(y);dirty=true;
        if(probe)return;
        if(settings.page!=Page::Library) {
            y-=int(std::max(0.0,1.0-(s.GetTicks()-page_at)/150.0)*8);
            if(x>=28 && x<=76 && y>=22 && y<=66){action(3);return;}
            if(settings.page==Page::LibraryOptions) {
                if(y>=126 && y<=174 && x>=28 && x<=612){preferences.core=static_cast<unsigned>(std::clamp((x-28)/198,0,2));settings.focus=0;remember_appearance();}
                else if(y>=233 && y<=311 && x>=28 && x<=612)open_scraper();
                else if(y>=335 && y<=393)settings.focus=2;
                dirty=true;return;
            }
            if(settings.page==Page::Scraper) {
                if(x>=28 && x<=612){
                    if(y>=122 && y<=182){settings.focus=0;scraper_action();}
                    else if(y>=192 && y<=252){settings.focus=1;scraper_action();}
                    else if(y>=272 && y<=315){settings.focus=2;scraper_action();}
                    else if(y>=327 && y<=370){settings.focus=3;scraper_action();}
                    else if(y>=382 && y<=425){settings.focus=4;scraper_action();}
                }dirty=true;return;
            }
            if(settings.page==Page::Keyboard) {
                if(y>=181 && y<377){key_row=(y-181)/49;auto rows=keyboard_rows();int left=(640-int(rows[key_row].size())*56+4)/2;int col=(x-left)/56;
                    if(x>=left && col>=0 && col<int(rows[key_row].size()) && (x-left)%56<52 && (y-181)%49<43){key_col=col;keyboard_choose();}}
                else if(y>=391 && y<=436){const std::array<int,4> left={28,136,336,480},width={94,188,132,132};for(int i=0;i<4;i++)if(x>=left[i] && x<left[i]+width[i]){key_row=4;key_col=i;keyboard_choose();break;}}
                dirty=true;return;
            }
            if(settings.page==Page::Settings && x>=28 && x<=612 && y>=84 && y<420) {
                int section=(y-84)/56;if((y-84)%56<52){settings.section=section;settings.enter();}
            }else if(settings.page==Page::Appearance) {
                if(x>=28 && x<=612 && ((y>=118 && y<=166) || (y>=220 && y<=268))) {
                    if(x>308 && x<332)return;
                    settings.focus=y<180?0:1;choose_appearance(x>=332?1:0);
                }else if(x>=28 && x<=612 && y>=309 && y<=357){settings.focus=2;settings.enter();}
                else if(x>=28 && x<=612 && y>=366 && y<=408)settings.focus=3;
            }else if(settings.page==Page::Power && x>=28 && x<=612 && y>=88 && y<400) {
                int row=(y-88)/78;if((y-88)%78>=70)return;
                settings.focus=row;
                if(x>=320){if(row==3)sleep_now();else change_system(row,x<450?-1:1);}
            }else if(settings.page==Page::Display && x>=44 && x<=596 && y>=163 && y<=285) {
                brightness_drag=true;
                int value=std::clamp(5+static_cast<int>(std::round((x-56)*19.0/528))*5,5,100);
                change_system(4,value/5-system_preferences.brightness/5);
            }
            dirty=true;return;
        }
        if(x>=550 && x<=604 && y>=28 && y<=70){action(7);return;}
        if(y>=409 && y<=457){if(x>=222 && x<=418)action(2);else if(x>=28 && x<=124)action(3);else if(x>=518 && x<=612)open_scraper();}
    }
    void pointer_begin(int x,int y) {
        pointer_down=true;down_x=x;down_y=y;pressed_game=model.selected;
        if(!probe && settings.page==Page::Library && !model.games.empty() && ((y>=133 && y<=289) || (y>=340 && y<=386))) {
            carousel.down(x,y,y>=340);
            if(carousel.range){model.selected=carousel.drag(x,model.games.size());selected_at=s.GetTicks();dirty=true;}return;
        }
        if(!probe && settings.page==Page::Library && y>=404)return;
        touch(x,y);
    }
    void pointer_move(int x,int y) {
        if(!pointer_down)return;
        if(carousel.dragging) {
            if(std::abs(y-down_y)>12)carousel.moved=true;
            auto selected=carousel.drag(x,model.games.size());
            if(selected!=model.selected){model.selected=selected;selected_at=s.GetTicks();}
            dirty=true;
        }else if(brightness_drag)touch(std::clamp(x,44,596),std::clamp(y,163,285));
        else if(probe){touch_x=x;touch_y=y;last_input="Touch "+std::to_string(x)+", "+std::to_string(y);dirty=true;}
    }
    void pointer_end(int x,int y) {
        if(!pointer_down)return;pointer_down=false;brightness_drag=false;
        if(carousel.dragging) {
            carousel.dragging=false;
            if(carousel.moved || carousel.range){select_game(model.selected);return;}
            size_t index=carousel.tapped(x,model.games.size());
            bool launch_selected=index==pressed_game && std::abs(carousel.position-index)<0.08 && y>=133 && y<=289;
            select_game(index);if(launch_selected)launch();
        }else if(!probe && settings.page==Page::Library && down_y>=404 && std::abs(x-down_x)<8 && std::abs(y-down_y)<8)touch(x,y);
    }
    bool mouse_position(Uint32 window,int& x,int& y) {
        (void)x;
        if(o.preview){if(y<480 && !pointer_down)return false;y-=480;return true;}
        return surfaces.size()>1 && window==surfaces[1].id;
    }
    void event(const SDL_Event& e) {
        auto before=settings.page;handle_input(e);
        if(before!=settings.page){page_at=s.GetTicks();dirty=true;}
    }
    void handle_input(const SDL_Event& e) {
        if(e.type==SDL_KEYDOWN || e.type==SDL_TEXTINPUT || e.type==SDL_JOYBUTTONDOWN || e.type==SDL_JOYHATMOTION || e.type==SDL_MOUSEBUTTONDOWN || e.type==SDL_FINGERDOWN || e.type==SDL_FINGERMOTION)last_activity=s.GetTicks();
        if(e.type==SDL_QUIT){quit=true;return;}
        if(e.type==SDL_WINDOWEVENT){if(e.window.event==SDL_WINDOWEVENT_FOCUS_LOST){pointer_down=false;carousel.dragging=false;brightness_drag=false;carousel.select(model.selected,s.GetTicks());}dirty=true;return;}
        if(settings.page==Page::Keyboard && e.type==SDL_TEXTINPUT) {
            std::string value=e.text.text;
            if(edit_buffer.size()+value.size()<=256 && value.find_first_of("\r\n\t") == std::string::npos)edit_buffer+=value;
            dirty=true;return;
        }
        if(e.type==SDL_KEYDOWN && settings.page==Page::Keyboard) {
            switch(e.key.keysym.sym) {
                case SDLK_ESCAPE:finish_edit(false);break;case SDLK_RETURN:finish_edit(true);break;case SDLK_BACKSPACE:erase_character();break;
                case SDLK_LEFT:action(0);break;case SDLK_RIGHT:action(1);break;case SDLK_UP:action(8);break;case SDLK_DOWN:action(9);break;default:break;
            }return;
        }
        if(e.type==SDL_KEYDOWN && !e.key.repeat) {
            last_input="Key "+std::to_string(e.key.keysym.sym);dirty=true;
            switch(e.key.keysym.sym) {
                case SDLK_LEFT:action(0);break;case SDLK_RIGHT:action(1);break;case SDLK_UP:action(8);break;case SDLK_DOWN:action(9);break;
                case SDLK_RETURN:case SDLK_a:action(2);break;case SDLK_ESCAPE:case SDLK_b:action(3);break;
                case SDLK_TAB:case SDLK_c:action(4);break;case SDLK_F1:case SDLK_AC_HOME:action(5);break;
                case SDLK_r:action(6);break;case SDLK_s:action(7);break;default:break;
            }
        }else if(e.type==SDL_JOYBUTTONDOWN) {
            const char* a=std::getenv("RGDS_BUTTON_A");const char* b=std::getenv("RGDS_BUTTON_B");const char* select=std::getenv("RGDS_BUTTON_SETTINGS");
            if(e.jbutton.button==(a?std::atoi(a):0))action(2);else if(e.jbutton.button==(b?std::atoi(b):1))action(3);
            else if(e.jbutton.button==4)action(0);else if(e.jbutton.button==5)action(1);else if(e.jbutton.button==(select?std::atoi(select):6))action(7);
            else if(e.jbutton.button==7)action(4);else if(e.jbutton.button==8)action(5);
        }else if(e.type==SDL_JOYHATMOTION) {
            if(e.jhat.value&SDL_HAT_LEFT)action(0);else if(e.jhat.value&SDL_HAT_RIGHT)action(1);else if(e.jhat.value&SDL_HAT_UP)action(8);else if(e.jhat.value&SDL_HAT_DOWN)action(9);
        }else if(e.type==SDL_JOYAXISMOTION) {
            auto now=s.GetTicks();if(std::abs(static_cast<int>(e.jaxis.value))>16000 && now-last_axis>180){
                if(e.jaxis.axis==0)action(e.jaxis.value<0?0:1);else if(e.jaxis.axis==1)action(e.jaxis.value<0?8:9);last_axis=now;
            }
        }else if(e.type==SDL_MOUSEBUTTONDOWN && e.button.which!=SDL_TOUCH_MOUSEID && e.button.button==SDL_BUTTON_LEFT) {
            int x=e.button.x,y=e.button.y;if(mouse_position(e.button.windowID,x,y))pointer_begin(x,y);
        }else if(e.type==SDL_MOUSEMOTION && e.motion.which!=SDL_TOUCH_MOUSEID && pointer_down && (e.motion.state&SDL_BUTTON_LMASK)) {
            int x=e.motion.x,y=e.motion.y;if(mouse_position(e.motion.windowID,x,y))pointer_move(x,y);
        }else if(e.type==SDL_MOUSEBUTTONUP && e.button.which!=SDL_TOUCH_MOUSEID) {
            int x=e.button.x,y=e.button.y;
            if(e.button.windowID==0){x=carousel.last_x;y=down_y;}else if(!mouse_position(e.button.windowID,x,y))return;
            pointer_end(x,y);
        }else if(e.type==SDL_MOUSEWHEEL && settings.page==Page::Library && !probe)navigate(std::clamp(e.wheel.x?e.wheel.x:-e.wheel.y,-3,3));
        else if(e.type==SDL_FINGERDOWN || e.type==SDL_FINGERMOTION || e.type==SDL_FINGERUP) {
            int x=static_cast<int>(e.tfinger.x*640),y=static_cast<int>(e.tfinger.y*(o.preview?960:480));
            if(o.preview){if(y<480 && !pointer_down)return;y-=480;}
            else if(surfaces.size()<2 || (e.tfinger.windowID && e.tfinger.windowID!=surfaces[1].id))return;
            if(e.type==SDL_FINGERDOWN){if(pointer_down)return;active_finger=e.tfinger.fingerId;pointer_begin(x,y);}
            else if(e.tfinger.fingerId==active_finger){if(e.type==SDL_FINGERUP)pointer_end(x,y);else pointer_move(x,y);}
        }
        if(dirty && probe)std::cout<<last_input<<std::endl;
    }

public:
    App(Options options):o(std::move(options)),theme(default_theme()),
        rounded(s,o.app.empty()?fs::path("assets/fonts"):o.app/"assets/fonts"),media(s),system(o.preview,o.app,o.data),probe(o.probe) {
        if(o.themes.empty())o.themes=o.app.empty()?fs::path("themes/default"):o.app/"themes/default";
        if(o.preferences.empty())o.preferences=o.data.empty()?fs::path("build/dev/preferences.ini"):o.data/"preferences.ini";
        if(o.data.empty())o.data=o.preferences.parent_path();
        if(o.system_preferences.empty())o.system_preferences=o.preferences.parent_path()/"system.ini";
        refresh_system();system_preferences.brightness=hardware.brightness_percent;
        try {
            if(fs::exists(o.system_preferences)) {
                system_preferences=load_system_preferences(o.system_preferences);
                if(!probe) {
                    if(hardware.power && system_preferences.power>0 && !system.set("power",system_preferences.power))status="Could not apply saved power mode";
                    if(hardware.brightness && !system.set("brightness",system_preferences.brightness))status="Could not apply saved brightness";
                    if(hardware.lid && system_preferences.lid && !system.set("lid",1))status="Could not enable lid wake";
                }
            }
        }catch(const std::exception& e){std::cerr<<"System preferences fallback: "<<e.what()<<std::endl;}
        preferences.dark=dark_theme_file(o.theme);
        try{if(fs::exists(o.preferences))preferences=load_preferences(o.preferences);}
        catch(const std::exception& e){std::cerr<<"Preferences fallback: "<<e.what()<<std::endl;}
        if(!o.mode.empty())preferences.dark=o.mode=="dark";
        if(!o.font.empty())preferences.pixel=o.font=="pixel";
        try{rounded.check();}catch(const std::exception& e){font_ready=false;preferences.pixel=true;std::cerr<<"Font fallback: "<<e.what()<<std::endl;}
        try{reload_themes();}catch(const std::exception& e){std::cerr<<"Theme fallback: "<<e.what()<<std::endl;theme=preferences.dark?default_dark_theme():default_theme();status="DEFAULT THEME - CHECK LOG";}
        model.games=scan(o.roots);
        if(o.preview && model.games.empty()) {
            for(const char* name:{"A little adventure","Puzzle collection","Racing club","Garden stories","Pocket exploration"})
                model.games.push_back({name,{},true});
        }
        restore_state();carousel.reset(model.selected);theme_source=read_theme_source();
        if(o.settings) {
            settings.open();
            if(!o.section.empty()) {
                if(o.section=="scraper")open_scraper();
                else {
                    static const std::array<std::string,6> ids={"theme","power","library","sound","connectivity","system"};
                    auto it=std::find(ids.begin(),ids.end(),o.section=="brightness"?"theme":o.section);
                    if(it==ids.end())throw std::runtime_error("Unknown settings section");
                    settings.section=static_cast<int>(it-ids.begin());settings.enter();
                    if(o.section=="brightness")settings.page=Page::Display;
                }
            }
        }
    }
    ~App(){stop_worker();std::fill(password.begin(),password.end(),'\0');close();}
    int run() {
        open();const auto began=s.GetTicks();last_activity=began;Uint32 tick=0;int frames=0;
        if(!o.roots.empty())start_worker("index");
        if(!o.input_script.empty()) {
            std::ifstream file(o.input_script);if(!file)throw std::runtime_error("Cannot read input script");
            std::string line;
            while(std::getline(file,line)) {
                if(line.empty() || line[0]=='#')continue;
                std::istringstream record(line);Input input{};std::string type,key;
                if(!(record>>input.time>>type))throw std::runtime_error("Invalid preview input script");
                if(type=="key") {
                    if(!(record>>key))throw std::runtime_error("Input key missing");
                    input.event.type=SDL_KEYDOWN;input.event.key.keysym.sym=s.GetKeyFromName(key.c_str());
                    if(!input.event.key.keysym.sym)throw std::runtime_error("Unknown input key");
                }else if(type=="text") {
                    input.event.type=SDL_TEXTINPUT;std::string value;std::getline(record>>std::ws,value);if(value.size()>31)throw std::runtime_error("Input text too long");std::copy(value.begin(),value.end(),input.event.text.text);
                }else if(type=="tap" || type=="click") {
                    input.event.type=SDL_MOUSEBUTTONDOWN;input.event.button.button=SDL_BUTTON_LEFT;
                    if(!(record>>input.event.button.x>>input.event.button.y))throw std::runtime_error("Input coordinates missing");
                    input.event.button.y+=480;input.event.button.windowID=surfaces[0].id;
                }else if(type=="drag") {
                    input.event.type=SDL_MOUSEMOTION;input.event.motion.state=SDL_BUTTON_LMASK;
                    if(!(record>>input.event.motion.x>>input.event.motion.y))throw std::runtime_error("Input coordinates missing");
                    input.event.motion.y+=480;input.event.motion.windowID=surfaces[0].id;
                }else if(type=="release") {
                    input.event.type=SDL_MOUSEBUTTONUP;input.event.button.button=SDL_BUTTON_LEFT;
                }else throw std::runtime_error("Unknown preview input type");
                inputs.push_back(input);
                if(type=="click"){input.time++;input.event.type=SDL_MOUSEBUTTONUP;inputs.push_back(input);}
            }
            std::stable_sort(inputs.begin(),inputs.end(),[](const Input& a,const Input& b){return a.time<b.time;});
        }
        while(!quit && !stopping) {
            while(next_input<inputs.size() && s.GetTicks()-began>=inputs[next_input].time)s.PushEvent(&inputs[next_input++].event);
            auto now=s.GetTicks();
            poll_service(now);
            if(settings.page==Page::Scraper && !service_loaded && worker<=0){start_worker("status");service_loaded=true;}
            bool animated=carousel.update(now) || now-selected_at<190 || now-page_at<150;
            if(animated)dirty=true;
            watch_theme(now);
            if(!animated && !carousel.dragging)watch_system(now);
            if(o.seconds && s.GetTicks()-began>=static_cast<Uint32>(o.seconds)*1000)break;
            if(s.GetTicks()-tick>=1000){dirty=true;tick=s.GetTicks();}
            if(dirty || o.frames){draw(!o.screenshot.empty());frames++;}
            if(!o.preview && fs::exists(o.data/"session/capture-request")) {
                fs::remove(o.data/"session/capture-request");auto previous=o.screenshot;
                o.screenshot=o.data/"session/ui-snapshot";draw(true);o.screenshot=previous;
            }
            if(o.frames && frames>=o.frames)break;
            SDL_Event e{};
            if(s.WaitEventTimeout(&e,animated?1:50)){event(e);while(s.PollEvent(&e))event(e);}
        }
        save_state();
        return 0;
    }
};

static int self_test() {
    auto root=fs::temp_directory_path()/("rgds-model-"+std::to_string(getpid()));
    fs::create_directories(root/"sub");
    std::ofstream(root/"B.nds").put('x');std::ofstream(root/"sub/A.DSI").put('x');
    std::ofstream(root/"ignore.txt").put('x');std::ofstream(root/"C.zip").put('x');
    auto games=scan({root,root/"sub"});
    Model m; m.games=games;m.move(-1);
    bool ok=games.size()==3 && games[0].title=="A" && m.selected==2;
    m.move(1);ok=ok && m.selected==0;m.next_core();m.next_core();m.next_core();ok=ok && m.core==0;
    Model empty;empty.move(-99);ok=ok && empty.selected==0;
    fs::remove_all(root);
    std::cout<<(ok?"Library, deduplication, navigation and three-core model passed.":"Model test failed.")<<std::endl;
    return ok?0:1;
}
int main(int argc,char** argv) {
    try {
        Options o;
        for(int i=1;i<argc;i++) {
            std::string arg=argv[i];
            auto value=[&](){if(++i>=argc)throw std::runtime_error("Missing value for "+arg);return std::string(argv[i]);};
            if(arg=="--self-test")return self_test();
            if(arg=="--preview")o.preview=true;else if(arg=="--probe")o.probe=true;
            else if(arg=="--allow-launch")o.allow_launch=true;
            else if(arg=="--watch-assets")o.watch_assets=true;
            else if(arg=="--settings")o.settings=true;
            else if(arg=="--section")o.section=value();
            else if(arg=="--mode")o.mode=value();else if(arg=="--font")o.font=value();
            else if(arg=="--preferences")o.preferences=value();
            else if(arg=="--system-preferences")o.system_preferences=value();
            else if(arg=="--themes")o.themes=value();else if(arg=="--input-script")o.input_script=value();
            else if(arg=="--frames")o.frames=std::stoi(value());else if(arg=="--seconds")o.seconds=std::stoi(value());
            else if(arg=="--theme")o.theme=value();else if(arg=="--screenshot")o.screenshot=value();
            else if(arg=="--rom-dir")o.roots.emplace_back(value());
            else if(arg=="--app")o.app=value();else if(arg=="--data")o.data=value();
            else if(arg=="--state")o.state=value();
            else if(arg=="--top-display")o.top=value();else if(arg=="--bottom-display")o.bottom=value();
            else if(arg=="--help") {
                std::cout<<"RGDS: --preview | --probe [--seconds 90] | --allow-launch --app DIR --data DIR\n"
                    <<"--rom-dir DIR (repeatable), --theme FILE, --frames N, --screenshot FILE.bmp\n"
                    <<"--watch-assets (reload theme on save), --state FILE (remember selection/core)\n"
                    <<"--settings [--section theme|brightness|power|library|sound|connectivity|system]\n"
                    <<"--mode light|dark --font rounded|pixel --preferences FILE --themes DIR\n"
                    <<"--system-preferences FILE (power, brightness and menu sleep)\n"
                    <<"--input-script FILE (desktop-only timed key/tap events)\n"
                    <<"--top-display NAME/INDEX --bottom-display NAME/INDEX\n";return 0;
            } else throw std::runtime_error("Unknown option: "+arg);
        }
        if(o.probe && !o.seconds)o.seconds=90;
        if(!o.mode.empty() && o.mode!="light" && o.mode!="dark")throw std::runtime_error("Mode must be light or dark");
        if(!o.font.empty() && o.font!="rounded" && o.font!="pixel")throw std::runtime_error("Font must be rounded or pixel");
        if(!o.preview && !o.input_script.empty())throw std::runtime_error("Input scripts are only available in desktop preview");
        if(o.seconds<0 || o.frames<0)throw std::runtime_error("Frames and seconds must be nonnegative");
        std::signal(SIGTERM,on_signal);std::signal(SIGINT,on_signal);
        App app(std::move(o));return app.run();
    }catch(const std::exception& e){std::cerr<<"RGDS: "<<e.what()<<std::endl;return 1;}
}
