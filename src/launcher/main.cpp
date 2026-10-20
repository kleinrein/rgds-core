#include "launcher/model.hpp"
#include "platform/sdl_dynamic.hpp"
#include "platform/event_wait.hpp"
#include "platform/wayland_role.hpp"
#include "platform/ui_trace.hpp"
#include "platform/system_controls.hpp"
#include "platform/lid_sleep.hpp"
#include "launcher/settings.hpp"
#include "launcher/i18n.hpp"
#include "launcher/collection_filter.hpp"
#include "launcher/keyboard.hpp"
#include "launcher/library_summary.hpp"
#include "launcher/settings_visual.hpp"
#include "launcher/pixel_icons.hpp"
#include "launcher/font.hpp"
#include "launcher/pixel_text.hpp"
#include "launcher/carousel.hpp"
#include "launcher/view_motion.hpp"
#include "launcher/menu_audio.hpp"
#include "launcher/accent.hpp"
#include "platform/alsa_mixer.hpp"
#include "platform/net_probe.hpp"
#include "launcher/library_viewport.hpp"
#include "launcher/media.hpp"
#include "launcher/game_options.hpp"
#include "launcher/shader_preview.hpp"
#include "launcher/library_cache.hpp"
#include "launcher/pictochat_app.hpp"
#include "platform/touch.hpp"
#include "platform/volume_keys.hpp"
#include "launcher/repeat.hpp"
#include "platform/overlay_frame.hpp"
#include "platform/control_mapping.hpp"
#include "platform/mainline_controls.hpp"
#include <array>
#include <csignal>
#include <ctime>
#include <iostream>
#include <iomanip>
#include <optional>
#include <functional>
#include <sstream>
#include <set>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/statvfs.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <poll.h>
#include <unistd.h>

using namespace rgds;
static volatile sig_atomic_t stopping = 0;
static void on_signal(int) { stopping = 1; }

// Opt-in startup evidence. Buffer milestones and write once after presentation;
// logging must not add disk IO between SDL/window/font initialization stages.
struct LaunchProfile {
    struct Mark {const char* stage;uint64_t us;int panel;};
    std::array<Mark,32> marks{};size_t count=0;
    bool enabled=false,finished=false;uint64_t began=0;
    static uint64_t clock(){timespec now{};clock_gettime(CLOCK_MONOTONIC,&now);return uint64_t(now.tv_sec)*1000000+now.tv_nsec/1000;}
    void start(){const char* option=std::getenv("RGDS_LAUNCH_PROFILE");enabled=option && std::string(option)=="1";if(enabled)began=clock();}
    void mark(const char* stage,int panel=-1){if(enabled && !finished && count<marks.size())marks[count++]={stage,clock()-began,panel};}
    void finish(bool menu){
        if(!enabled || finished)return;mark("first-present");finished=true;
        std::ostringstream output;output<<"{\"type\":\"rgds-launch-profile\",\"pid\":"<<getpid()<<",\"game_menu\":"<<(menu?"true":"false")<<",\"milestones\":[";
        for(size_t i=0;i<count;i++){if(i)output<<',';output<<"{\"stage\":\""<<marks[i].stage<<"\",\"elapsed_us\":"<<marks[i].us<<",\"panel\":"<<marks[i].panel<<'}';}
        output<<"]}\n";std::cerr<<output.str();
    }
};
static LaunchProfile launch_profile;

// Sleep until a child exits instead of waking the CPU every 10 ms while an
// emulator owns the screens. Signals still interrupt poll(2).
static void wait_child_exit(pid_t pid,int timeout_ms){
#if defined(__linux__) && defined(SYS_pidfd_open)
    if(pid>0){
        int fd=int(syscall(SYS_pidfd_open,pid,0));
        if(fd>=0){pollfd item{fd,POLLIN,0};poll(&item,1,timeout_ms);::close(fd);return;}
    }
#endif
    (void)pid;(void)timeout_ms;usleep(10000);
}

// UI reads may materialize an empty optional field. Missing and empty fields
// render identically, so that must not turn an unchanged status file into a repaint.
static bool same_fields(const std::map<std::string,std::string>& a,const std::map<std::string,std::string>& b) {
    for(const auto& pair:a)if(!pair.second.empty()){auto found=b.find(pair.first);if(found==b.end() || found->second!=pair.second)return false;}
    for(const auto& pair:b)if(!pair.second.empty()){auto found=a.find(pair.first);if(found==a.end() || found->second!=pair.second)return false;}
    return true;
}
static bool same_hardware(const SystemStatus& a,const SystemStatus& b) {
    return a.brightness==b.brightness && a.power==b.power && a.sleep==b.sleep && a.lid==b.lid && a.charging==b.charging && a.external_power==b.external_power && a.brightness_percent==b.brightness_percent && a.battery==b.battery && a.volume==b.volume && a.volume_percent==b.volume_percent && a.poweroff==b.poweroff;
}

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
 ,{char(0xdf),{14,17,17,22,17,17,22}}, {char(0xc6),{15,20,20,30,20,20,23}}, {char(0xd8),{15,19,21,21,25,17,30}}, {char(0xc5),{4,10,4,14,17,31,17}}
 ,{'&',{12,18,20,8,21,18,13}}, {'\'',{4,4,8,0,0,0,0}}, {',',{0,0,0,0,0,4,8}}
};
struct Section { const char* title; };
static const std::array<Section,9> sections = {{
    {"Theme & display"},
    {"Power"},
    {"Library"},
    {"Sound"},
    {"Connectivity"},
    {"System"},
    {"Controls"},
    {"Statistics"},
    {"Power off"}
}};

struct Options {
    bool independent=false,mainline=false;
    bool preview = false, probe = false, allow_launch = false, watch_assets = false, settings = false,game_menu=false,game_switcher=false,home=false,library=false,boot_warning=false,boot_startup=false,card_repair=false;
    int frames = 0, seconds = 0, boot_ready_fd = -1;
    std::string top = "DSI-1", bottom = "DSI-2";
    fs::path theme, themes, screenshot, app, data, state, preferences, system_preferences, input_script,cache_report,touch_recording;
    std::string mode, font, section;
    std::vector<fs::path> roots;
    fs::path game_rom;
    std::string game_core="dsperate";
    fs::path executable;
};
struct Surface { SDL_Window* window = nullptr; SDL_Renderer* renderer = nullptr; Uint32 id = 0;SDL_Surface* bitmap=nullptr; };
class App {
    Sdl s;
    MenuAudio sounds{s};
    Options o;
    Model model;
    bool session_initialized=false,library_ready=false,boot_displayed=false,boot_touch_candidate=false,boot_touch_wait_release=false,boot_hardware_ready=false;
    Uint32 boot_displayed_at=0,boot_scene_at=0;
    std::set<SDL_Keycode> boot_blocked_keys;
    std::set<std::pair<SDL_JoystickID,Uint8>> boot_blocked_buttons;
    pid_t library_child=-1;
    int library_pipe=-1;
    bool library_eof=false,library_scan_started=false;
    std::string library_bytes;
    std::vector<Game> all_games;
    GameOptions game_preferences;
    std::vector<Shader> shaders;
    std::vector<int> shader_choices;
    bool shader_per_game=false;
    int shader_core=0;
    Page shader_return=Page::LibraryOptions;
    int shader_preview_scene=0;
    ShaderPreviewMode shader_preview_mode=ShaderPreviewMode::Split;
    float shader_preview_split=.5f;
    // The picker reuses the Home carousel and rail, with its own position.
    Carousel shader_carousel;
    bool shader_drag=false,shader_thumbs_pending=false;
    ControlMapping edited_controls;
    Page controls_return=Page::Settings;
    bool controls_per_game=false,controls_inherit=false,control_capture=false;
    int capture_logical=-1;
    Uint32 capture_started=0,capture_clock=0;
    std::string controls_notice;
    Uint32 pointer_at=0,release_at=0,launch_at=0,feedback_frame_at=0;
    int contact_x=0,contact_y=0;
    bool touch_tail=false,options_ready=false,launching=false,launch_preview=false;
    unsigned launching_core=0;
    int launch_drawn_head=-1;
    enum class LaunchPhase {None,Waiting,Preparing,Exiting,Playing};
    LaunchPhase launch_phase=LaunchPhase::None;
    pid_t launch_child=-1;
    int launch_channel=-1,launch_result=0;
    bool launch_cancelled=false,launch_handoff=false;
    Uint32 launch_exit_at=0,launch_cancel_at=0;
    double launch_exit_opacity=1;
    Carousel carousel;
    ViewMotion view_motion;
    int view_touch_dx=0,view_touch_dy=0;
    MotionHits motion_hits;
    ViewTravel view_route=ViewTravel::Forward;
    bool motion_touch=false,drawing_elements=false;
    int element_origin_y=0;
    Uint32 view_drawn_at=0;
    uint64_t element_moves=0,back_transitions=0;
    // Animations page: four 72 px cards, and three 104 px choices in a card.
    static int motion_row(int index){return 100+index*82;}
    static int motion_segment_x(int index){return 270+index*112;}
    bool view_transitions_enabled()const{return preferences.animations && preferences.view_animations && !probe && !launching && settings.page!=Page::BootWarning;}
    void motion_input(int code){
        view_route=(code==3 || (code==7 && settings.page!=Page::Library) || (code==2 && settings.page==Page::Display) || (code==4 && settings.page==Page::Keyboard))?ViewTravel::Back:code==0?ViewTravel::Left:code==1?ViewTravel::Right:ViewTravel::Forward;
        if(view_transitions_enabled() && preferences.view_style==ViewStyle::Spring)view_motion.interact(s.GetTicks(),motion_touch?contact_x:320,motion_touch?contact_y:240);
    }
    void move_element(uint32_t id,int& x,int& y,int width,int height,bool focused,int clip_top=0,int clip_bottom=480,int clip_left=0,int clip_right=640){
        if(!drawing_elements || !view_transitions_enabled() || layered(preferences.view_style))return;
        MotionRect bounds{x,y-element_origin_y,width,height};
        auto offset=view_motion.element(id,bounds,focused,view_drawn_at);
        if(pointer_down && bounds.contains(contact_x,contact_y))offset.y+=3;
        motion_hits.add(bounds,offset.x,offset.y,clip_top,clip_bottom,clip_left,clip_right);
        x+=offset.x;y+=offset.y;if(offset.x || offset.y)element_moves++;
    }
    SDL_Texture* view_canvas=nullptr;
    SDL_Texture *view_previous=nullptr,*view_presented=nullptr;
    bool view_snapshot_ready=false,view_previous_ready=false;
    ViewStyle last_view_style=ViewStyle::Spring;
    std::vector<std::pair<int,bool>> view_levels;
    uint64_t push_animation_frames=0;
    void clear_view_canvases(){
        for(auto** texture:{&view_canvas,&view_previous,&view_presented})if(*texture){s.DestroyTexture(*texture);*texture=nullptr;}
        view_snapshot_ready=view_previous_ready=false;
    }
    void start_view_motion(Uint32 now,ViewTravel route=ViewTravel::Forward){
        // Push composites the view captured just before the input that
        // changed it; nothing captured (no input) means no transition.
        if(layered(preferences.view_style) && !view_previous_ready){view_motion.stop();return;}
        view_motion.start(now,route,preferences.view_style);
    }
    SDL_Renderer* bottom_renderer(){return surfaces.size()>1?surfaces[1].renderer:surfaces[0].renderer;}
    uint64_t view_capture_frame=~uint64_t(0);
    // The outgoing image for a Push transition, captured once per displayed
    // frame when an input arrives. Idle frames never render offscreen.
    void capture_view_for_input(const SDL_Event& e){
        if(!view_transitions_enabled() || !layered(preferences.view_style) || view_canvas_failed || view_motion.running || surfaces.empty())return;
        const bool trigger=e.type==SDL_KEYDOWN || e.type==SDL_JOYBUTTONDOWN || (e.type==SDL_JOYHATMOTION && e.jhat.value) ||
                           e.type==SDL_MOUSEBUTTONUP || e.type==SDL_FINGERUP;
        if(!trigger || view_capture_frame==rendered_frames)return;
        SDL_Renderer* renderer=bottom_renderer();
        if(!view_previous)view_previous=s.CreateTexture(renderer,SDL_PIXELFORMAT_RGBA8888,SDL_TEXTUREACCESS_TARGET,640,480);
        if(!view_previous || s.SetRenderTarget(renderer,view_previous)!=0){view_canvas_failed=true;s.ClearError();return;}
        s.RenderSetLogicalSize(renderer,640,480);s.RenderSetClipRect(renderer,nullptr);
        s.SetRenderDrawBlendMode(renderer,SDL_BLENDMODE_NONE);s.SetRenderDrawColor(renderer,0,0,0,0);s.RenderClear(renderer);
        paint_bottom(renderer,0);
        if(s.SetRenderTarget(renderer,nullptr)!=0)throw std::runtime_error("Could not restore capture canvas");
        s.RenderSetLogicalSize(renderer,640,o.preview?960:480);s.RenderSetClipRect(renderer,nullptr);
        view_previous_ready=true;view_capture_frame=rendered_frames;
    }
    bool view_canvas_failed=false,view_seen=false;
    std::tuple<int,bool,int,int> last_view;
    uint64_t view_transitions=0,view_animation_frames=0;
    void sync_view_motion(Uint32 now){
        auto key=std::make_tuple(int(settings.page),home_menu,preferences.view,preferences.layout);
        bool enabled=view_transitions_enabled();
        view_motion.pace=view_pace(preferences.view_speed);
        if(last_view_style!=preferences.view_style){clear_view_canvases();view_motion.stop();view_motion.style=preferences.view_style;view_canvas_failed=false;last_view_style=preferences.view_style;}
        auto level=std::make_pair(int(settings.page),home_menu);
        if(view_seen && level!=std::make_pair(std::get<0>(last_view),std::get<1>(last_view))){
            auto parent=std::find(view_levels.begin(),view_levels.end(),level);
            if(parent!=view_levels.end()){view_route=ViewTravel::Back;view_levels.erase(parent+1,view_levels.end());}
            else if(view_route==ViewTravel::Back)view_levels={level};
            else {view_route=ViewTravel::Forward;view_levels.push_back(level);}
        }else if(!view_seen)view_levels={level};
        if(view_seen && key!=last_view && enabled){
            // A layout or collection change stays in place: a quick crossfade,
            // never a sideways slide of the whole screen.
            if(settings.page==Page::Library && std::get<0>(last_view)==int(Page::Library) && home_menu==std::get<1>(last_view))
                view_route=ViewTravel::Fade;
            start_view_motion(now,view_route);if(view_route==ViewTravel::Back)back_transitions++;view_route=ViewTravel::Forward;view_transitions++;
        }
        last_view=key;view_seen=true;
        if(!enabled){if(view_motion.active())dirty=true;view_motion.stop();motion_hits.clear();clear_view_canvases();view_canvas_failed=false;}
    }
    struct SoundState {
        Page page;bool home,picker;int view,layout,focus,pick,row,col,volume,brightness;size_t game,text;unsigned toggles;bool favourite,launch;
    };
    unsigned sound_toggles()const{return unsigned(preferences.dark)|(unsigned(preferences.pixel)<<1)|(unsigned(preferences.animations)<<2)|(unsigned(preferences.view_animations)<<3)|(unsigned(preferences.touch_indicator)<<4)|(unsigned(preferences.auto_load)<<5)|(unsigned(preferences.auto_save)<<6)|(unsigned(preferences.menu_sounds)<<7)|(unsigned(system_preferences.lid)<<8);}
    SoundState sound_state()const{return {settings.page,home_menu,picker_open,preferences.view,preferences.layout,settings.page==Page::Settings?settings.section:settings.focus,picker_focus,key_row,key_col,hardware.volume_percent,system_preferences.brightness,navigation_selected(),edit_buffer.size(),sound_toggles(),game_preferences.favourite,launching};}
    unsigned sound_depth=0;int sound_code=2;SoundState sound_before{};
    std::optional<MenuCue> explicit_sound;
    std::string menu_sound_action,departure_action;
    Uint32 departure_at=0;bool departure_dispatch=false,sleep_audio_parked=false;
    void configure_sounds(){
        if(!sounds.opened)sounds.opened=[this]{restore_dac_level();};
        sounds.configure(preferences.menu_sounds && !probe && !sleep_audio_parked && settings.page!=Page::Microphone,preferences.sound_volume,asset_path("sounds"));
        // Music belongs to the home, library and settings; not to the in-game
        // menu, the microphone test, sleep or a launch in progress.
        sounds.configure_music(preferences.menu_music && !probe && !o.game_menu && !sleep_audio_parked && settings.page!=Page::Microphone && !launching,preferences.music_volume,asset_path("music")/"menu.wav");
    }
    // Every new sink can reset the RK817 DAC, including the boot screen. The
    // menu sink stays paused until the saved level is back; a direct control
    // write avoids a helper process on each reopen. Status may not exist yet.
    void restore_dac_level(){
        if(o.preview)return;
        int level=hardware.volume?hardware.volume_percent:-1;
        if(level<0){auto saved=fields(o.data/"volume.ini");try{if(saved["schema"]=="1")level=std::clamp(std::stoi(saved["volume"]),0,100);}catch(const std::exception&){level=-1;}}
        if(level>=0 && write_mixer_percent(dac_percent_for_level(level)))return;
        if(!system.sync_volume()){sounds.close();sounds.error="Menu audio is unavailable on this device.";}
    }
    bool emit_sound(MenuCue cue,int direction=0){
        if(probe || settings.page==Page::Microphone || (hardware.volume && hardware.volume_percent==0))return false;
        return sounds.play(cue,s.GetTicks(),direction,motion_touch?(contact_x/640.f-.5f)*.5f:direction*.12f);
    }
    void sound_intent(MenuCue cue){if(sound_depth)explicit_sound=cue;else emit_sound(cue);}
    void finish_sound_action(){
        if(--sound_depth)return;
        if(!departure_action.empty()){explicit_sound.reset();return;}
        const auto after=sound_state();auto before=sound_before;int direction=sound_code==0 || sound_code==8?-1:1;
        if(explicit_sound){emit_sound(*explicit_sound);explicit_sound.reset();return;}
        if(before.launch || after.launch)return;
        if(before.page==Page::GameOptions && after.page==Page::GameOptions && before.favourite!=after.favourite){emit_sound(after.favourite?MenuCue::Favourite:MenuCue::Unfavourite);return;}
        if(before.toggles!=after.toggles){emit_sound((after.toggles&~before.toggles)?MenuCue::Enable:MenuCue::Disable);return;}
        if(before.page!=after.page || before.home!=after.home || before.picker!=after.picker){
            bool back=sound_code==3 || view_route==ViewTravel::Back;
            emit_sound(back?MenuCue::Back:before.picker && !after.picker?MenuCue::Confirm:MenuCue::Open);return;
        }
        if(before.view!=after.view || before.layout!=after.layout){emit_sound(MenuCue::Open,direction);return;}
        if(before.volume!=after.volume || before.brightness!=after.brightness){emit_sound(MenuCue::Adjust,direction);return;}
        if(before.text!=after.text){emit_sound(MenuCue::Type);return;}
        if(before.game!=after.game || before.focus!=after.focus || before.pick!=after.pick || before.row!=after.row || before.col!=after.col)emit_sound(MenuCue::Navigate,direction);
    }
    struct SoundAction {
        App& app;
        SoundAction(App& value,int code):app(value){if(!app.sound_depth++){app.sound_before=app.sound_state();app.sound_code=code;app.explicit_sound.reset();}else if(code==3)app.sound_code=3;}
        ~SoundAction(){app.finish_sound_action();}
    };
    void menu_sounds_action(int code){
        if(code==3){settings.back();dirty=true;return;}
        if(code==8 || code==9){settings.focus=std::clamp(settings.focus+(code==8?-1:1),0,4);dirty=true;return;}
        if(code!=0 && code!=1 && code!=2 && code!=4)return;
        if(settings.focus==0){preferences.menu_sounds=(code==2 || code==4)?!preferences.menu_sounds:code==1;remember_appearance();}
        else if(settings.focus==1){
            if(code==0 || code==1){preferences.sound_volume=std::clamp(preferences.sound_volume+(code==0?-5:5),0,100);remember_appearance();sound_intent(MenuCue::Adjust);}
            else show_picker("Effects volume",{"Mute","25%","50%","75%","100%"},(preferences.sound_volume+12)/25,[this](int n){preferences.sound_volume=n*25;remember_appearance();sound_intent(MenuCue::Adjust);});
        }else if(settings.focus==2){preferences.menu_music=(code==2 || code==4)?!preferences.menu_music:code==1;remember_appearance();sound_intent(preferences.menu_music?MenuCue::Enable:MenuCue::Disable);}
        else if(settings.focus==3){
            if(code==0 || code==1){preferences.music_volume=std::clamp(preferences.music_volume+(code==0?-5:5),0,100);remember_appearance();}
            else show_picker("Music volume",{"20%","40%","60%","80%","100%"},std::clamp((preferences.music_volume+10)/20-1,0,4),[this](int n){preferences.music_volume=(n+1)*20;remember_appearance();});
        }else {
            std::vector<std::string> names(menu_cue_labels.begin(),menu_cue_labels.end());
            show_picker("Preview sound",names,0,[this](int n){sound_intent(static_cast<MenuCue>(n));});
        }dirty=true;
    }
    TouchDevice touchscreen;
    VolumeKeys volume_keys;
    HoldRepeat hold;
    Theme theme;
    Theme light_theme=default_theme(),dark_theme=default_dark_theme();
    RoundedFont rounded;
    Media media;
    ShaderPreview shader_preview;
    LibraryCache library_cache;
    Uint32 library_poll=0;
    uint64_t rendered_frames=0,render_file_reads=0,render_stamp_checks=0;
    UiTrace ui_trace;
    CollectionFilter collection_filter,search_preview;
    std::vector<Game> search_matches;
    bool search_preview_waiting=false,search_preview_loading=false;
    Uint32 search_preview_at=0;
    LibrarySummary library_summary;LibrarySummary::Result summary;bool summary_ready=false;
    bool collection_loading=false;
    std::string collection_selection;
    Preferences preferences;
    Locale locale;
    Settings settings;
    SystemControls system;
    SystemStatus hardware;
    OverlayFrame overlay;
    bool embedded_menu=false;
    bool home_menu=true,scroll_drag=false,scroll_hover=false,list_touch=false,list_swipe=false,tap_pending=false;
    int gesture_scroll=0,scroll_grab=0;
    Uint32 scroll_cue_at=0;
    bool picker_open=false,picker_detail=false,detail_drag=false,detail_rail=false;
    std::vector<std::string> detail_lines;
    int detail_scroll=0;
    std::string picker_title;
    std::vector<std::string> picker_options;
    int picker_focus=0,picker_selected=0,picker_core_offset=-1;std::vector<int> picker_cores;   // emulator picker rows -> core indices
    int picker_scroll=0,picker_grab=0,picker_scroll_start=0;
    bool picker_rail=false,picker_swipe=false;
    Uint32 picker_at=0,picker_closed_at=0;bool picker_was_open=false;
    // The sheet leaves with a short slide; input already belongs to the page.
    bool picker_closing()const{return preferences.animations && !picker_open && picker_closed_at && s.GetTicks()-picker_closed_at<140;}
    std::function<void(int)> picker_apply;
    struct ToggleMotion {bool value=false;float from=0;Uint32 at=0;};
    std::map<std::pair<int,std::string>,ToggleMotion> toggle_motion;
    Uint32 micro_until=0;
    enum class LibraryPress {None,Back,View,Options,Search,Clear,LeftArrow,RightArrow,Horizontal,Vertical,Row};
    LibraryPress library_press=LibraryPress::None;
    std::array<RailPress,3> rail_press; // Left, right, captured thumb.
    bool rail_feedback_active=false;
    int library_press_row=-1;
    Uint32 library_press_at=0;
    size_t home_selected=1;
    std::vector<Game> switch_games;
    int list_scroll=0,list_focus=-1;
    Page list_page=Page::Library;
    static constexpr int list_top=96,list_height=316,list_stride=82;
    // Sound and brightness: the slider card's vertical span (touch and drawing agree).
    std::pair<int,int> slider_band()const{return settings.page==Page::Sound?std::make_pair(96,186):std::make_pair(163,285);}
    static int sound_row(int index){return 198+index*74;}
    int list_count() const {
        switch(settings.page){
        case Page::Settings:return 9;
        case Page::MenuSounds:return 5;
        case Page::GameSwitcher:return int(switch_games.size())+1;
        case Page::Graphics:return 8;
        case Page::Power:return 6;
        case Page::LibraryOptions:return 10;
        case Page::Appearance:return 11;
        case Page::Scraper:return 3;
        case Page::DeviceInfo:return 5;case Page::System:return 8;
        case Page::GameOptions:if(experimental_game())return 8;return (game_preferences.core<0?int(preferences.core):game_preferences.core)==0?(superdrastic_game()?10:9):7;
        case Page::GameArtwork:return 3;
        case Page::Maintenance:return 7;case Page::Network:return 5;case Page::Startup:return 4;
        case Page::SaveGallery:return 9;case Page::Discover:return 3;case Page::RomSetup:return 3;case Page::DSControls:return 5;
        case Page::EmulatorOptions:return o.game_core=="drastic"?12:10;
        case Page::ShaderPicker:return int(shader_choices.size());
        case Page::Controls:return 19;
        case Page::Wifi:return std::max(0,std::atoi(device_status.count("network_count")?device_status.at("network_count").c_str():"0"));
        case Page::Timezone:return int(timezones.size());
        default:return 0;
        }
    }
    int list_selected() const {return settings.page==Page::Settings?settings.section:settings.focus;}
    // Back: the row you left `target` from, kept within its current list.
    static constexpr SDL_Rect rail_left_bounds(){return {0,409,RailGeometry::left,71};}
    static constexpr SDL_Rect rail_right_bounds(){return {RailGeometry::left+RailGeometry::width,409,640-RailGeometry::left-RailGeometry::width,71};}
    void back_to(Page target){settings.page=target;int row=settings.recall(target);int count=list_count();settings.focus=count>0?std::clamp(row,0,count-1):row;}
    int list_max_scroll() const {return std::max(0,list_count()*list_stride-16-list_height);}
    void sync_list() {
        if(settings.page!=list_page){list_page=settings.page;list_scroll=0;list_focus=-1;}
        int focus=list_selected();
        if(focus!=list_focus){
            list_focus=focus;int top=focus*list_stride,bottom=top+66;
            if(top<list_scroll)list_scroll=top;
            if(bottom>list_scroll+list_height)list_scroll=bottom-list_height;
        }
        list_scroll=std::clamp(list_scroll,0,list_max_scroll());
    }
    void scroll_to(int y){
        int count=list_count(),content=count*list_stride-16;
        int thumb=content>0?std::max(54,list_height*list_height/std::max(list_height,content)):list_height;
        list_scroll=std::clamp(int(std::round((y-list_top-scroll_grab)*double(list_max_scroll())/std::max(1,list_height-thumb))),0,list_max_scroll());dirty=true;
    }
    // Home tiles: Settings, Games, Favourites, Recents, then the console's PictoChat.
    static constexpr size_t home_count=6,pictochat_tile=4,last_played_tile=5;
    // The carousel shows five tiles; the DSi column adds the last played game.
    size_t home_tiles()const{return preferences.home_layout==1?home_count:home_count-1;}
    size_t navigation_count() const {return home_menu?home_tiles():model.games.size();}
    void home_layout_changed(){home_selected=std::min(home_selected,home_tiles()-1);carousel.reset(navigation_selected());cancel_navigation();}
    size_t navigation_selected() const {return home_menu?home_selected:model.selected;}
    void select_navigation(size_t index,bool animate=true){
        if(!home_menu){select_game(index,animate);return;}
        home_selected=std::min<size_t>(index,home_tiles()-1);selected_at=s.GetTicks();
        if(animate && preferences.animations)carousel.select(home_selected,selected_at);else carousel.reset(home_selected);dirty=true;
    }
    // Pages where a held Left/Right (keys, d-pad, stick, L/R) repeats: the
    // Home rail, sliders and the shader carousel.
    bool horizontal_hold_page()const{return settings.page==Page::Library || settings.page==Page::Display || settings.page==Page::Sound || settings.page==Page::ShaderPicker;}
    void cancel_navigation(){dirty=true;touch_tail=false;options_ready=false;pointer_down=false;pointer_source=PointerSource::None;carousel.dragging=false;browser_list.moving=false;scroll_drag=false;picker_rail=picker_swipe=false;detail_drag=detail_rail=false;list_touch=list_swipe=tap_pending=false;browser_list_touch=browser_rail=false;shader_drag=false;shader_carousel.dragging=false;rail_press={};hold.clear();}
    void load_library(std::vector<Game> games){
        model.games=std::move(games);
        if(o.preview && model.games.empty())for(const char* name:{"A little adventure","Puzzle collection","Racing club","Garden stories","Pocket exploration"})model.games.push_back({name,{},true});
        all_games=model.games;library_ready=true;if(!o.game_menu){summary_ready=false;library_summary.request(o.data,all_games);}device_status=fields(o.data/"device/status.ini");
        if(!o.game_menu && !o.boot_startup)library_view(preferences.view);
        if(o.game_menu){
            if(std::find(std::begin(core_ids),std::end(core_ids),o.game_core)==std::end(core_ids))throw std::runtime_error("Unknown game engine");
            if(!o.game_rom.empty())o.game_rom=fs::weakly_canonical(o.game_rom);
            model.games={{o.game_rom.empty()?"Preview game":o.game_rom.stem().string(),o.game_rom,o.game_rom.empty()}};
            model.selected=0;home_menu=false;settings.page=Page::QuickMenu;menu_status={{"state","paused"},{"checkpoint",o.preview?"1":"0"}};
        }
        if(o.game_switcher){load_switcher();settings.page=Page::GameSwitcher;}
        restore_state();if(o.library || o.game_menu)home_menu=false;else if(o.home || o.boot_startup){home_menu=true;settings.page=Page::Library;}home_selected=std::min(home_selected,home_tiles()-1);carousel.reset(navigation_selected());browser_list.reveal(model.selected,model.games.size());theme_source=read_theme_source();
        if(o.settings){settings.open();if(!o.section.empty()){
            if(o.section=="scraper")open_scraper();else{static const std::array<std::string,8> ids={"theme","power","library","sound","connectivity","system","controls","statistics"};auto found=std::find(ids.begin(),ids.end(),o.section=="brightness" || o.section=="motion"?"theme":o.section);if(found==ids.end())throw std::runtime_error("Unknown settings section");settings.section=int(found-ids.begin());settings.enter();if(o.section=="brightness")settings.page=Page::Display;else if(o.section=="motion")settings.page=Page::Motion;else if(o.section=="controls")open_controls(false);}
        }}
    }
    void stop_library_scan(){if(library_child>0){kill(library_child,SIGTERM);for(int i=0;i<30;i++){if(waitpid(library_child,nullptr,WNOHANG)!=0){library_child=-1;break;}usleep(10000);}if(library_child>0){kill(library_child,SIGKILL);waitpid(library_child,nullptr,0);}}library_child=-1;if(library_pipe>=0)::close(library_pipe);library_pipe=-1;}
    void begin_library_scan(){
        library_scan_started=true;
        if(o.roots.empty()){load_library({});return;}
        int channel[2];if(pipe(channel)!=0){status="Could not start library scan";library_ready=true;return;}
        if(fcntl(channel[0],F_SETFL,O_NONBLOCK)<0 || fcntl(channel[0],F_SETFD,FD_CLOEXEC)<0 || fcntl(channel[1],F_SETFD,FD_CLOEXEC)<0){::close(channel[0]);::close(channel[1]);status="Could not start library scan";library_ready=true;return;}
        std::vector<std::string> arguments={o.executable.string(),"--scan-library"};for(const auto& root:o.roots){arguments.push_back("--rom-dir");arguments.push_back(root.string());}
        std::vector<char*> argv;for(auto& argument:arguments)argv.push_back(argument.data());argv.push_back(nullptr);
        library_child=fork();if(library_child==0){::close(channel[0]);if(dup2(channel[1],STDOUT_FILENO)<0)_exit(127);::close(channel[1]);execv(argv[0],argv.data());_exit(127);}
        ::close(channel[1]);if(library_child<0){::close(channel[0]);status="Could not start library scan";library_ready=true;return;}library_pipe=channel[0];
    }
    void poll_library_scan(){
        if(library_pipe<0)return;char bytes[4096];ssize_t count;
        while((count=read(library_pipe,bytes,sizeof(bytes)))>0){library_bytes.append(bytes,size_t(count));if(library_bytes.size()>64*1024*1024){stop_library_scan();status="Library scan exceeded its limit";library_ready=true;return;}}
        if(count==0)library_eof=true;else if(count<0 && errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR){stop_library_scan();status="Could not read library scan";library_ready=true;return;}
        if(library_child>0){int result=0;auto done=waitpid(library_child,&result,WNOHANG);if(done==0)return;if(done<0 || !WIFEXITED(result) || WEXITSTATUS(result)!=0){stop_library_scan();status="Library scan failed";library_ready=true;return;}library_child=0;}
        if(!library_eof)return;::close(library_pipe);library_pipe=-1;library_child=-1;
        std::vector<Game> games;size_t at=0;while(at<library_bytes.size()){auto path_end=library_bytes.find('\0',at);if(path_end==std::string::npos)break;auto title_end=library_bytes.find('\0',path_end+1);if(title_end==std::string::npos)break;games.push_back({library_bytes.substr(path_end+1,title_end-path_end-1),fs::path(library_bytes.substr(at,path_end-at)),false});at=title_end+1;}
        library_bytes.clear();auto previous=settings;bool previous_home=home_menu;auto previous_category=home_selected;load_library(std::move(games));if(o.boot_startup){settings=previous;home_menu=previous_home;home_selected=previous_category;if(!home_menu)library_view(preferences.view);carousel.reset(navigation_selected());}
        else if(scan_stay){if(!previous_home)library_view(preferences.view);settings=previous;home_menu=previous_home;home_selected=previous_category;carousel.reset(navigation_selected());}
        scan_stay=false;if(!o.roots.empty())start_worker("index");dirty=true;
    }
    void start_boot_session(){
        if(session_initialized)return;
        load_system_configuration();
        if(!preferences.pixel)check_fonts();
        shaders=load_shaders(o.app.empty()?fs::path("assets/shaders/catalog.tsv"):o.app/"assets/shaders/catalog.tsv");
        cancel_navigation();boot_touch_candidate=false;session_initialized=true;settings.page=Page::Library;home_menu=!o.library;selected_at=page_at=s.GetTicks();
        if(o.preview){refresh_system();boot_hardware_ready=true;}else system.request_status();
        begin_library_scan();dirty=true;
    }
    void continue_boot(){if(settings.page==Page::BootWarning && boot_displayed)start_boot_session();}
    void snapshot_boot_input(){
        if(!o.boot_startup || session_initialized)return;
        auto pump=reinterpret_cast<decltype(&SDL_PumpEvents)>(dlsym(s.handle,"SDL_PumpEvents"));if(pump)pump();
        auto keys=reinterpret_cast<decltype(&SDL_GetKeyboardState)>(dlsym(s.handle,"SDL_GetKeyboardState"));
        if(keys){int length=0;auto state=keys(&length);for(auto item:{std::make_pair(SDLK_a,SDL_SCANCODE_A),std::make_pair(SDLK_RETURN,SDL_SCANCODE_RETURN)})if(state && item.second<length && state[item.second])boot_blocked_keys.insert(item.first);}
        auto mouse=reinterpret_cast<decltype(&SDL_GetMouseState)>(dlsym(s.handle,"SDL_GetMouseState"));if(mouse && (mouse(nullptr,nullptr)&SDL_BUTTON_LMASK))boot_touch_wait_release=true;
        auto touch_count=reinterpret_cast<decltype(&SDL_GetNumTouchDevices)>(dlsym(s.handle,"SDL_GetNumTouchDevices"));auto touch_id=reinterpret_cast<decltype(&SDL_GetTouchDevice)>(dlsym(s.handle,"SDL_GetTouchDevice"));auto fingers=reinterpret_cast<decltype(&SDL_GetNumTouchFingers)>(dlsym(s.handle,"SDL_GetNumTouchFingers"));if(touch_count && touch_id && fingers)for(int i=0;i<touch_count();i++)if(fingers(touch_id(i))>0)boot_touch_wait_release=true;
        boot_touch_wait_release=boot_touch_wait_release || touchscreen.initially_pressed();
        auto update=reinterpret_cast<decltype(&SDL_JoystickUpdate)>(dlsym(s.handle,"SDL_JoystickUpdate"));auto button=reinterpret_cast<decltype(&SDL_JoystickGetButton)>(dlsym(s.handle,"SDL_JoystickGetButton"));auto instance=reinterpret_cast<decltype(&SDL_JoystickInstanceID)>(dlsym(s.handle,"SDL_JoystickInstanceID"));if(update)update();const char* a=std::getenv("RGDS_BUTTON_A");int accept=a?std::atoi(a):0;if(button && instance)for(auto* joystick:joysticks)for(int index:{accept,7}){bool mainline=o.mainline && mainline_pads.count(instance(joystick));int raw=mainline?(index==7?9:1):index;if(button(joystick,raw))boot_blocked_buttons.insert({instance(joystick),Uint8(index)});}
    }
    std::set<SDL_JoystickID> menu_held_hats;
    // A fresh pause menu would read the game's held controls as new presses:
    // SDL posts every joystick button already down on its first update, and a
    // stylus resting on the panel decodes as a new contact. A held A (accelerate),
    // B, Start, Select or Menu, or a stylus over Resume, would close the menu at
    // once, so each control held at open is ignored until it is released.
    void snapshot_menu_input(){
        if(!o.game_menu)return;
        std::string held;if(touchscreen.initially_pressed()){touchscreen.hold_initial_contact();held=" touch";}
        auto update=reinterpret_cast<decltype(&SDL_JoystickUpdate)>(dlsym(s.handle,"SDL_JoystickUpdate"));auto count=reinterpret_cast<decltype(&SDL_JoystickNumButtons)>(dlsym(s.handle,"SDL_JoystickNumButtons"));auto button=reinterpret_cast<decltype(&SDL_JoystickGetButton)>(dlsym(s.handle,"SDL_JoystickGetButton"));auto hat=reinterpret_cast<decltype(&SDL_JoystickGetHat)>(dlsym(s.handle,"SDL_JoystickGetHat"));auto instance=reinterpret_cast<decltype(&SDL_JoystickInstanceID)>(dlsym(s.handle,"SDL_JoystickInstanceID"));
        if(update && count && button && hat && instance){update();for(auto* joystick:joysticks){auto id=instance(joystick);bool mainline=o.mainline && mainline_pads.count(id);
            for(int raw=0;raw<count(joystick);raw++)if(button(joystick,raw)){int index=mainline?MainlineControls::button(raw):raw;if(index>=0){boot_blocked_buttons.insert({id,Uint8(index)});held+=" button "+std::to_string(index);}}
            if(hat(joystick,0)!=SDL_HAT_CENTERED){menu_held_hats.insert(id);held+=" hat";}}}
        if(!held.empty())std::cout<<"RGDS menu helper: held at open:"<<held<<"; ignored until released"<<std::endl;
    }
    // The column's top tile: straight back into the last game.
    void open_last_played(){
        const auto& last=summary.groups[0].last_game;
        if(!summary_ready || !last){status=summary_ready?"Nothing played yet":"Preparing…";dirty=true;return;}
        home_menu=false;library_view(0);remember_appearance();
        if(collection_loading)return;   // a very large card is still being listed; Games opens instead
        for(size_t i=0;i<model.games.size();i++)if(model.games[i].path==last->path){select_game(i,false);launch();return;}
    }
    void open_category(){
        cancel_navigation();
        if(home_selected==last_played_tile)open_last_played();
        else if(home_selected==pictochat_tile)open_pictochat();
        else if(home_selected==0)settings.open();
        else{home_menu=false;library_view(int(home_selected)-1);remember_appearance();}dirty=true;
    }
    void return_home(){view_route=ViewTravel::Back;cancel_navigation();home_menu=true;home_selected=size_t(preferences.view)+1;carousel.reset(home_selected);selected_at=s.GetTicks();dirty=true;}
    SystemPreferences system_preferences;
    bool system_configuration_loaded=false;
    Uint32 last_activity=0,system_poll=0,system_retry=0,lid_poll=0,brightness_changed=0,connection_poll=0;
    int brightness_applied=60;bool brightness_unsaved=false;
    int volume_applied=25;bool volume_unsaved=false;
    pid_t return_control_child=-1;
    std::optional<int> return_volume,return_brightness,return_power,return_lid,active_return_volume,active_return_brightness;
    bool return_status_needed=false,return_audio_failed=false;
    bool return_writes_pending()const{return return_control_child>0 || return_volume || return_brightness || return_power || return_lid;}
    void begin_return_controls(){
        if(return_control_child>0 || system.volume_pending() || system.brightness_pending())return;
        if(!return_volume && !return_brightness && !return_power && !return_lid)return;
        std::vector<std::string> arguments={o.executable.string(),"--restore-controls",o.app.string(),o.data.string(),std::to_string(return_volume.value_or(-1)),std::to_string(return_brightness.value_or(-1)),std::to_string(return_power.value_or(-1)),std::to_string(return_lid.value_or(-1))};
        std::vector<char*> argv;for(auto& argument:arguments)argv.push_back(argument.data());argv.push_back(nullptr);
        active_return_volume=return_volume;active_return_brightness=return_brightness;return_volume.reset();return_brightness.reset();return_power.reset();return_lid.reset();system.discard_status();
        return_control_child=fork();if(return_control_child==0){execv(argv[0],argv.data());_exit(127);}if(return_control_child<0){return_audio_failed=active_return_volume.has_value();status="ERROR: Could not restore settings";}
    }
    void poll_return_controls(bool wait=false){
        if(return_control_child>0){int result=0;pid_t completed;do{completed=waitpid(return_control_child,&result,wait?0:WNOHANG);}while(completed<0 && errno==EINTR);
            if(completed==0)return;return_control_child=-1;int failure=completed>0 && WIFEXITED(result)?WEXITSTATUS(result):127;
            if(active_return_volume){return_audio_failed=(failure&1)!=0;if(!return_audio_failed)volume_applied=*active_return_volume;}
            if(active_return_brightness && !(failure&2))brightness_applied=*active_return_brightness;
            if(failure)status="ERROR: Could not restore settings - see log";
            if(!return_volume && active_return_volume){if(return_audio_failed)hardware.volume_percent=confirmed_volume();volume_unsaved=false;}
            if(!return_brightness && active_return_brightness){if(failure&2){system_preferences.brightness=brightness_applied;hardware.brightness_percent=brightness_applied;}if(brightness_unsaved){try{save_system_preferences(o.system_preferences,system_preferences);}catch(const std::exception& error){status=error.what();}}brightness_unsaved=false;}
            active_return_volume.reset();active_return_brightness.reset();dirty=true;
        }
        begin_return_controls();
        if(!wait && !return_writes_pending() && return_status_needed && system.request_status()){return_status_needed=false;system_poll=s.GetTicks();}
    }
    void flush_return_controls(){while(return_writes_pending()){poll_return_controls(true);if(return_control_child<=0 && (system.volume_pending() || system.brightness_pending())){system.flush_volume();system.flush_brightness();}}}
    LidSleep lid_sleep;
    bool font_ready=true,font_checked=false;
    void check_fonts(bool first_frame=false){
        if(!font_ready || font_checked)return;
        try{
            if(first_frame){if(o.boot_warning)rounded.check({16,20,28,36});else rounded.check({16,20,28});}
            else{rounded.check();font_checked=true;}
        }catch(const std::exception& e){font_ready=false;preferences.pixel=true;std::cerr<<"Font fallback: "<<e.what()<<std::endl;}
    }
    Uint32 selected_at=0,page_at=0,service_poll=0,clock_poll=0;
    Uint32 cover_motion_at=0; // Only a changed selection animates the top-screen cover.
    std::time_t drawn_second=0;   // the column's clock ticks once a second
    std::string drawn_status_stamp;
    int drawn_charge_phase=-1;
    pid_t worker=-1;
    bool worker_ok=false;
    Uint32 input_guard_until=0;
    std::string worker_action,username,password,edit_buffer,service_notice;
    int edit_field=0,key_row=0,key_col=0,key_layer=0;
    bool key_caps=false;size_t edit_cursor=0;
    bool service_loaded=false;
    std::map<std::string,std::string> service;
    bool pointer_down=false;
    enum class EdgeGesture {None,Bottom,Left};
    EdgeGesture edge_gesture=EdgeGesture::None;
    void gesture_intro(){
        remember_appearance();
        show_detail("Touch gestures",locale.text("Swipe up from the bottom edge to go Home from anywhere.")+"\n"+locale.text("Swipe in from the left edge to go Back.")+"\n"+locale.text("A small handle at the bottom of the lower screen shows gestures are on; it rises with your finger, and a ring marks the spot when a gesture fires."));
    }
    Uint32 gesture_flash_at=0;int gesture_flash_kind=0;
    // The handle at the bottom edge, the Home pill that rises with the finger,
    // the Back disc that slides in from the left, and a ring where one fired.
    void gesture_cues(SDL_Renderer* r,int y){
        if(!gestures_available())return;
        const Uint32 now=s.GetTicks();
        const bool bottom=pointer_down && edge_gesture==EdgeGesture::Bottom,left=pointer_down && edge_gesture==EdgeGesture::Left;
        const double pb=bottom?std::clamp((down_y-contact_y)/72.0,0.0,1.0):0,pl=left?std::clamp((contact_x-down_x)/72.0,0.0,1.0):0;
        {const int w=72+int(48*pb),lift=int(26*pb);alpha_round(r,320-w/2,y+471-lift,w,5,2,theme.at(pb>0?"accent":"ink"),int((preferences.dark?90:110)+140*pb));
         if(pb>.2){const std::string text=locale.text("Home");const int pw=label_width(text,16,false)+52;alpha_round(r,320-pw/2,y+428-lift,pw,34,17,theme.at("accent"),int(250*std::min(1.0,(pb-.2)*2)));
            if(pb>.5){ui_icon(r,SettingsIcon::Home,320-pw/2+20,y+445-lift,18,"on_accent");raw_label_colour(r,text,320-pw/2+36,centred_label_y(text,16,y+445-lift),16,theme.at("on_accent"),pw);}}}
        if(pl>0){const int cx=-44+int(80*pl);alpha_round(r,cx-40,y+200,80,80,40,theme.at("accent"),int(250*pl));if(pl>.4)ui_icon(r,SettingsIcon::Back,cx+14,y+240,24,"on_accent");}
        if(preferences.animations && gesture_flash_at && now-gesture_flash_at<260){const double t=(now-gesture_flash_at)/260.0;feedback_arc(r,gesture_flash_kind?24:320,y+(gesture_flash_kind?240:468),int(18+56*t),0,1,theme.at("accent"),int(220*(1-t)),4);}
    }
    bool gestures_available()const{return preferences.gestures && !probe && !(chat_open && chat.view==pictochat::View::Room) && !launching && settings.page!=Page::BootWarning && settings.page!=Page::Controls && !o.game_menu;}
    // A swipe that started on an edge and travelled far enough: act once, then swallow the rest of the touch.
    bool edge_gesture_move(int x,int y){
        if(edge_gesture==EdgeGesture::None)return false;
        const bool home=edge_gesture==EdgeGesture::Bottom && down_y-y>=72 && std::abs(x-down_x)<120,back=edge_gesture==EdgeGesture::Left && x-down_x>=72 && std::abs(y-down_y)<120;
        if(!home && !back)return false;
        edge_gesture=EdgeGesture::None;cancel_navigation();brightness_drag=volume_drag=false;
        gesture_flash_at=s.GetTicks();gesture_flash_kind=home?0:1;micro_until=gesture_flash_at+270;
        if(home){const bool chatting=chat_open;close_pictochat();picker_open=false;if(settings.page!=Page::Library){settings.page=Page::Library;settings.focus=0;}if(!home_menu)return_home();if(!chatting)sound_intent(MenuCue::Back);}
        else action(3);
        dirty=true;return true;
    }
    enum class PointerSource {None,Mouse,SDL,Raw};
    PointerSource pointer_source=PointerSource::None;
    Uint32 raw_touch_at=0,sdl_touch_at=0;
    bool raw_touch_seen=false,sdl_touch_seen=false;
    SDL_FingerID active_finger=0;
    int down_x=0,down_y=0;
    size_t pressed_game=0;
    std::map<std::string,std::string> menu_status;
    std::map<std::string,std::string> device_status,update_status;
    bool update_health_sent=false;uint64_t update_health_at=0,update_health_started=0;
    bool device_loaded=false;
    std::string network_signature;int probed_signal=-1;Uint32 device_info_at=0;
    // Spawn the full status helper only for a changed link, an open network
    // page or a five-minute safety refresh. Quality comes from the kernel.
    void refresh_network(Uint32 now){
        connection_poll=now;
        auto probe=o.preview?NetworkProbe{}:probe_network();
        bool details=settings.page==Page::Network || settings.page==Page::Wifi || settings.page==Page::DeviceInfo || settings.page==Page::Startup || settings.page==Page::Maintenance;
        if(!device_loaded || o.preview || details || probe.signature!=network_signature || now-device_info_at>=300000){
            start_worker("device.info");device_loaded=true;device_info_at=now;network_signature=probe.signature;probed_signal=-1;return;
        }
        int level=probe.signal>=67?3:probe.signal>=34?2:probe.signal>0?1:0,previous=probed_signal>=67?3:probed_signal>=34?2:probed_signal>0?1:0;
        if(probed_signal<0 || level!=previous)dirty=true;
        probed_signal=probe.signal;
    }
    std::string network_id,confirmation;
    std::string startup_mode="stock";bool startup_warning=true;
    Page confirm_return=Page::System;
    Uint32 menu_poll=0;
    unsigned menu_sequence=0;
    std::string menu_pending_token;
    Uint32 menu_request_at=0;
    void confirm(const std::string& operation,Page parent){confirmation=operation;confirm_return=parent;settings.page=Page::Confirm;settings.focus=0;dirty=true;}
    void save_startup(){std::ostringstream payload;payload<<"{\"mode\":\""<<startup_mode<<"\",\"warning\":"<<(startup_warning?"true":"false")<<'}';start_worker("device.startup",payload.str());}
    void change_startup(int direction,bool toggle){
        if(settings.focus==3){save_experience("quick_resume",experience["quick_resume"]!="1");return;}
        if(o.independent){
            if(settings.focus==2)confirm("reboot",Page::Startup);
            else if(settings.focus==1){startup_mode="rgds";startup_warning=toggle?device_status["boot_warning"]!="1":direction>0;save_startup();}
            return;
        }
        if(worker>0)return;startup_mode=device_status["boot_mode"]=="rgds"?"rgds":"stock";startup_warning=device_status["boot_warning"]=="1";
        if(settings.focus==2){confirm("reboot",Page::Startup);return;}
        if(settings.focus==1){startup_warning=toggle?!startup_warning:direction>0;save_startup();return;}
        auto previous=startup_mode;startup_mode=toggle?(startup_mode=="stock"?"rgds":"stock"):direction>0?"rgds":"stock";
        if(startup_mode==previous)return;if(startup_mode=="rgds")confirm("startup-rgds",Page::Startup);else save_startup();
    }
    void wifi_choose() {
        if(worker>0)return;auto prefix="network_"+std::to_string(settings.focus)+"_";
        network_id=device_status[prefix+"id"];if(network_id.empty())return;
        if(device_status[prefix+"connected"]=="1"){settings.page=Page::Network;service_notice="Already connected.";dirty=true;return;}
        if(device_status[prefix+"saved"]=="1"){
            auto selected_network=network_id;
            show_picker("Saved network",{"Connect","Enter password"},0,[this,selected_network](int selected){
                network_id=selected_network;
                if(selected==1){begin_edit(2);return;}
                std::ostringstream payload;payload<<"{\"network\":"<<std::quoted(network_id)<<",\"use_saved\":true}";
                start_worker("device.connect",payload.str());settings.page=Page::Network;settings.focus=1;
            });return;
        }
        if(device_status[prefix+"security"].find("802.1X")!=std::string::npos){service_notice="This network requires an enterprise profile.";dirty=true;return;}
        if(device_status[prefix+"security"].empty()){std::ostringstream payload;payload<<"{\"network\":"<<std::quoted(network_id)<<",\"password\":\"\"}";start_worker("device.connect",payload.str());settings.page=Page::Network;}
        else begin_edit(2);dirty=true;
    }
    std::string menu_input="no input";
    void note_menu_input(const SDL_Event& e){
        if(e.type==SDL_JOYBUTTONDOWN)menu_input="joystick button "+std::to_string(e.jbutton.button);
        else if(e.type==SDL_JOYHATMOTION && e.jhat.value)menu_input="joystick hat "+std::to_string(e.jhat.value);
        else if(e.type==SDL_KEYDOWN)menu_input="key "+std::to_string(e.key.keysym.sym);
        else if(e.type==SDL_FINGERDOWN || e.type==SDL_MOUSEBUTTONDOWN)menu_input="SDL pointer";
    }
    void menu_request(const std::string& action) {
        if(menu_status["state"]=="busy" || !menu_pending_token.empty()){dirty=true;return;}
        if(o.preview && action.rfind("switch:",0)==0){std::cout<<"Game switch (preview): "<<action<<std::endl;menu_status["message"]="Saving before switching games…";dirty=true;return;}
        if(action=="resume" && !departure_dispatch && preferences.menu_sounds){
            if(emit_sound(MenuCue::Resume)){explicit_sound.reset();departure_action=action;departure_at=s.GetTicks()+180;return;}
        }
        menu_sound_action=action;
        if(action=="save" || action=="load" || action.rfind("slot-",0)==0)sound_intent(MenuCue::Confirm);
        // The supervisor logs <pid>-<n> tokens; this names the input behind each one.
        if(o.game_menu)std::cout<<"RGDS menu helper: "<<getpid()<<'-'<<menu_sequence+1<<' '<<action<<" after "<<menu_input<<" at "<<s.GetTicks()<<" ms"<<std::endl;
        if(o.preview){if(action=="save" || action.rfind("slot-save:",0)==0)sound_intent(MenuCue::Save);else if(action=="load" || action.rfind("slot-load:",0)==0)sound_intent(MenuCue::Load);if(action=="threaded:0" || action=="threaded:1")menu_status["threaded"]=action.substr(9);if(action=="fast-forward")menu_status["fast_forward"]=menu_status["fast_forward"]=="1"?"0":"1";menu_status["message"]=action=="save"?"Checkpoint saved (preview)":action=="load"?"Checkpoint loaded (preview)":action.rfind("shader:",0)==0?"Shader change requested (preview)":"";if(action.rfind("shader:",0)==0)std::cout<<"Shader request (preview): "<<action<<std::endl;if(action=="resume" || action=="quit" || action=="force-quit" || action=="restart")quit=true;dirty=true;return;}
        auto path=o.data/"session/game-menu.request",pending=fs::path(path.string()+".pending");
        fs::create_directories(path.parent_path());std::ofstream file(pending);
        auto request_token=std::to_string(getpid())+'-'+std::to_string(++menu_sequence);
        file<<"token="<<request_token<<"\naction="<<action<<'\n';file.close();
        if(!file){status="Could not send game action";dirty=true;return;}fs::rename(pending,path);
        if(action=="resume")quit=true;
        else{
            menu_pending_token=request_token;menu_request_at=s.GetTicks();menu_status["state"]="busy";
            menu_status["message"]=action=="save"?"Saving checkpoint…":action=="load"?"Loading checkpoint…":(action.rfind("graphics:",0)==0 || action.rfind("filter2d:",0)==0 || action.rfind("adaptive:",0)==0)?"Applying graphics…":action.rfind("shader:",0)==0?"Applying shader…":action=="restart"?"Saving before restarting…":action=="quit"?"Returning home…":"Working…";
        }dirty=true;
    }
    std::map<std::string,std::string> slots,experience;
    Page gallery_return=Page::GameOptions,ds_return=Page::GameOptions,discover_return=Page::Library;
    int gallery_core=1,discovery_filter=0,resume_core=-1;
    std::string search_query;Page search_return=Page::Discover;Uint32 automatic_update_at=0;
    bool resume_checked=false,setup_checked=false,resume_launch=false;
    fs::path gallery_root(){return item_folder(model.selected)/"slots"/core_ids[gallery_core];}
    void save_experience(const std::string& key,int value){
        std::ostringstream body;body<<"{\"action\":\"settings\",\""<<key<<"\":"<<value<<'}';
        start_worker("device.library",body.str());experience[key]=std::to_string(value);dirty=true;
    }
    void open_gallery(){
        if(model.games.empty())return;gallery_return=settings.page;
        auto local=library_cache.options(model.games[model.selected]);gallery_core=o.game_menu?active_engine():local.core<0?int(preferences.core):local.core;
        settings.page=Page::SaveGallery;settings.focus=0;slots=fields(gallery_root()/"index.ini");dirty=true;
        if(!o.game_menu)slot_worker("slots",0);
    }
    void slot_worker(const std::string& action,int slot,const std::string& file="",bool overwrite=false){
        std::ostringstream body;body<<"{\"action\":"<<std::quoted(action)<<",\"rom\":"<<std::quoted(model.games[model.selected].path.string())<<",\"core\":"<<std::quoted(core_ids[gallery_core])<<",\"slot\":"<<slot<<",\"file\":"<<std::quoted(file)<<",\"overwrite\":"<<(overwrite?"true":"false")<<'}';
        start_worker("device.library",body.str());
    }
    void slot_actions(){
        int slot=settings.focus;bool occupied=!slots["slot_"+std::to_string(slot)].empty();
        std::vector<std::string> actions={o.game_menu?"Save to slot":"Import slot",o.game_menu?"Load slot":"Export slot","Undo overwrite",o.game_menu?"Undo load":"Import help"};if(o.game_menu)actions.push_back("Export slot");show_picker("Save slot",actions,0,[this,slot,occupied](int choice){
            if(choice==0 && o.game_menu){auto save=[this,slot](int accepted){if(accepted)menu_request("slot-save:"+std::to_string(slot)+":yes");};if(occupied)show_picker("Replace this save?",{"Cancel","Replace"},0,save);else menu_request("slot-save:"+std::to_string(slot));}
            else if(choice==1 && o.game_menu){if(occupied)show_picker("Load this save?",{"Cancel","Load slot"},0,[this,slot](int n){if(n)menu_request("slot-load:"+std::to_string(slot));});}
            else if(choice==2){if(o.game_menu)menu_request("slot-undo");else slot_worker("undo",slot);}
            else if(choice==3){if(o.game_menu)menu_request("slot-undo-load");else show_detail("Import help",locale.text("Copy .rgdssave.zip files into userdata/imports. Imports must match this game and emulator."));}
            else if(choice==4 || (choice==1 && !o.game_menu)){if(occupied){if(o.game_menu)menu_request("slot-export:"+std::to_string(slot));else slot_worker("export",slot);}}
            else if(choice==0){
                std::vector<std::string> names;std::error_code ec;auto folder=o.data/"imports";
                if(fs::exists(folder))for(const auto& file:fs::directory_iterator(folder,ec)){auto name=file.path().filename().string();if(file.is_regular_file() && name.size()>12 && name.substr(name.size()-12)==".rgdssave.zip" && names.size()<100)names.push_back(name);}
                std::sort(names.begin(),names.end());if(names.empty()){show_detail("Import slot",locale.text("Copy .rgdssave.zip files into userdata/imports. Imports must match this game and emulator."));return;}
                show_picker("Import slot",names,0,[this,names,slot,occupied](int n){auto name=names.at(n);if(occupied)show_picker("Replace this save?",{"Cancel","Replace"},0,[this,name,slot](int yes){if(yes)slot_worker("import",slot,name,true);});else slot_worker("import",slot,name);});
            }
        });
    }
    pid_t microphone_worker=-1;int microphone_pipe=-1,microphone_level=0,microphone_peak=0,microphone_db=-60;
    std::string microphone_bytes,microphone_state;Uint32 microphone_update=0;
    void stop_microphone(){
        if(microphone_worker>0){kill(-microphone_worker,SIGTERM);kill(microphone_worker,SIGTERM);for(int i=0;i<30;i++){if(waitpid(microphone_worker,nullptr,WNOHANG)!=0){microphone_worker=-1;break;}usleep(10000);}if(microphone_worker>0){kill(microphone_worker,SIGKILL);waitpid(microphone_worker,nullptr,0);microphone_worker=-1;}}
        if(microphone_pipe>=0){::close(microphone_pipe);microphone_pipe=-1;}microphone_bytes.clear();
    }
    void open_microphone(){
        if(o.game_menu){show_detail("Test microphone",locale.text("Return home before testing the physical microphone."));return;}
        stop_microphone();microphone_level=microphone_peak=0;microphone_db=-60;microphone_state=o.preview?"Test the microphone on your handheld.":"Listening…";
        settings.page=Page::Microphone;settings.focus=0;configure_sounds();dirty=true;
        if(o.preview)return;
        int channel[2];if(pipe(channel)!=0){microphone_state="Microphone unavailable";return;}
        auto helper=(o.app/"runtime/microphone_test.py").string(),data=o.data.string();
        auto child=fork();if(child==0){setpgid(0,0);::close(channel[0]);dup2(channel[1],STDOUT_FILENO);::close(channel[1]);execlp("python3","python3","-B",helper.c_str(),"--data",data.c_str(),nullptr);_exit(127);}
        ::close(channel[1]);if(child<0){::close(channel[0]);microphone_state="Microphone unavailable";return;}
        setpgid(child,child);microphone_worker=child;microphone_pipe=channel[0];fcntl(microphone_pipe,F_SETFL,fcntl(microphone_pipe,F_GETFL)|O_NONBLOCK);fcntl(microphone_pipe,F_SETFD,FD_CLOEXEC);microphone_update=s.GetTicks();
    }
    void poll_microphone(Uint32 now){
        if(settings.page!=Page::Microphone){if(microphone_worker>0 || microphone_pipe>=0)stop_microphone();return;}
        if(microphone_pipe<0)return;
        char buffer[512];ssize_t size;while((size=read(microphone_pipe,buffer,sizeof(buffer)))>0)microphone_bytes.append(buffer,size);
        size_t newline;while((newline=microphone_bytes.find('\n'))!=std::string::npos){auto line=microphone_bytes.substr(0,newline);microphone_bytes.erase(0,newline+1);std::istringstream values(line);int level,peak,db;
            if(values>>level>>peak>>db){microphone_level=std::clamp(level,0,100);microphone_peak=std::clamp(peak,0,100);microphone_db=std::clamp(db,-60,0);microphone_state="Listening…";microphone_update=now;}
            else{microphone_state=line=="busy"?"Return home before testing the physical microphone.":"Microphone unavailable";microphone_level=microphone_peak=0;}dirty=true;
        }
        if(microphone_bytes.size()>4096)microphone_bytes.clear();
        if(microphone_worker>0 && waitpid(microphone_worker,nullptr,WNOHANG)!=0){microphone_worker=-1;::close(microphone_pipe);microphone_pipe=-1;if(microphone_state=="Listening…")microphone_state="Microphone unavailable";microphone_level=microphone_peak=0;dirty=true;}
        if(now-microphone_update>3000 && microphone_state=="Listening…"){microphone_state="Microphone unavailable";microphone_level=microphone_peak=0;dirty=true;}
    }
    void microphone_screen(SDL_Renderer* r,int y){
        settings_header(r,y,"Test microphone");
        round_rect(r,28,y+105,584,250,12,"panel");ui_icon(r,SettingsIcon::Volume,70,y+145,30,"accent");label(r,"Live input",100,y+133,24,"ink",280);
        raw_label(r,std::to_string(microphone_db)+" dBFS",424,y+136,22,"accent",160);
        for(int i=0;i<24;i++)round_rect(r,52+i*22,y+207,17,52,4,i*100/24<microphone_level?"accent":"slot");
        int peak=52+std::clamp(microphone_peak*523/100,0,523);rect(r,peak,y+198,3,70,"ink");
        label(r,"Quiet",52,y+275,16,"muted",200);label(r,"Loud",526,y+275,16,"muted",70);
        wrapped(r,microphone_state,52,y+312,18,536,2);
        label(r,"Audio is not recorded or saved.",40,y+381,18,"muted",560);
    }
    void open_discover(){discover_return=settings.page;settings.page=Page::Discover;settings.focus=0;status.clear();dirty=true;}
    void launcher_search_shortcut(){
        if(o.game_menu || !session_initialized || settings.page==Page::BootWarning)return;
        cancel_navigation();picker_open=false;picker_apply={};picker_detail=false;probe=false;
        if(settings.page==Page::Keyboard)finish_edit(false);
        if(settings.page==Page::Microphone){stop_microphone();settings.page=Page::Sound;configure_sounds();}
        if(settings.page==Page::Confirm)settings.page=confirm_return;
        control_capture=false;capture_logical=-1;
        if(settings.page!=Page::Discover)open_discover();else{settings.focus=0;dirty=true;}
    }
    void save_switcher_library(){
        auto path=o.data/"session/switcher-library.ini",pending=fs::path(path.string()+".pending");
        fs::create_directories(path.parent_path());std::ofstream out(pending);
        std::set<fs::path> seen;int count=0;
        auto add=[&](const Game& game){
            auto rom=game.path.string();if(game.demo || count>=9 || rom.find_first_of("\r\n")!=std::string::npos || !seen.insert(game.path).second)return;
            auto title=library_title(game.title);std::replace(title.begin(),title.end(),'\n',' ');std::replace(title.begin(),title.end(),'\r',' ');
            out<<"game_"<<count<<"_rom="<<rom<<"\ngame_"<<count<<"_title="<<title<<'\n';count++;
        };
        if(!model.games.empty())add(model.games[model.selected]);
        for(const auto& game:summary.recent_games)add(game);
        // A fresh library can still switch games before it has play history.
        for(const auto& game:model.games){if(count>=9)break;add(game);}
        out<<"count="<<count<<'\n';out.close();if(out)fs::rename(pending,path);
    }
    void load_switcher(){
        switch_games.clear();auto values=fields(o.data/"session/game-switcher.ini");
        for(int i=0;i<std::clamp(std::atoi(values["count"].c_str()),0,9);i++){
            auto prefix="game_"+std::to_string(i);fs::path path=values[prefix+"_rom"];
            if(!path.empty()){std::error_code ec;auto canonical=fs::weakly_canonical(path,ec);if(!ec)path=canonical;}
            if(!path.empty())switch_games.push_back({values[prefix+"_title"].empty()?path.stem().string():values[prefix+"_title"],path,false});
        }
        if(switch_games.empty() && !model.games.empty())switch_games.push_back(model.games[0]);
    }
    bool discovery_active()const{return !search_query.empty() || discovery_filter!=0;}
    std::string discovery_filter_name()const{
        static const std::array<std::string,4> names={"All games","Unplayed","Played","Missing artwork"};
        return names.at(discovery_filter);
    }
    std::string discovery_description()const{
        std::string value=search_query;
        if(discovery_filter){if(!value.empty())value+="  ·  ";value+=locale.text(discovery_filter_name());}
        return value;
    }
    void apply_discovery(){
        home_menu=false;settings.page=Page::Library;settings.focus=0;
        library_view(preferences.view);dirty=true;
    }
    void clear_discovery(){search_query.clear();discovery_filter=0;apply_discovery();}
    void quick_search(){
        picker_open=false;picker_apply={};discover_return=Page::Library;
        begin_edit(4);
    }
    static std::string trimmed_query(const std::string& value){
        auto first=value.find_first_not_of(" \t\r\n"),last=value.find_last_not_of(" \t\r\n");
        return first==std::string::npos?"":value.substr(first,last-first+1);
    }
    void search_changed(){
        if(edit_field!=4)return;
        search_preview.cancel();search_matches.clear();search_preview_waiting=true;search_preview_loading=true;
        search_preview_at=s.GetTicks();dirty=true;
    }
    void poll_search_preview(Uint32 now){
        if(settings.page!=Page::Keyboard || edit_field!=4)return;
        try{
            if(search_preview_waiting && now-search_preview_at>=150 && library_ready){
                search_preview_waiting=false;search_preview.request(o.data,all_games,preferences.view,trimmed_query(edit_buffer),discovery_filter);
            }
            if(auto result=search_preview.poll()){search_matches=std::move(result->games);search_preview_loading=false;dirty=true;}
        }catch(const std::exception&){search_preview_loading=false;dirty=true;}
    }
    void open_ds_controls(){ds_settings=fields(item_folder(model.selected)/"ds-input.ini");ds_return=settings.page;settings.page=Page::DSControls;settings.focus=0;dirty=true;}
    std::map<std::string,std::string> ds_settings;
    bool ds_supported(){int core=o.game_menu?active_engine():game_preferences.core<0?int(preferences.core):game_preferences.core;return core==1 || core==3 || core==4;}
    void ds_controls_row(SDL_Renderer* r,int y,int i){const std::array<std::string,5> names={"Blow into microphone","Close and open DS lid","DS language","Real microphone","DS nickname"};const std::array<std::string,6> languages={"Japanese","English","French","German","Italian","Spanish"};int language=ds_settings.count("language")?std::clamp(std::atoi(ds_settings["language"].c_str()),0,5):1;
        list_row(r,y,i,names[i],!ds_supported()?"Engine managed":i==4?(ds_settings.count("nickname")?ds_settings["nickname"]:"RG DS Core"):i==2?languages[language]:i==3?ds_settings["microphone"]=="0"?"Off":"On":o.game_menu?"2 seconds":"In game",ds_supported() && (i>=2 || o.game_menu));}
    void ds_controls_action(){
        if(!ds_supported())return;
        if(settings.focus<2){if(o.game_menu)menu_request(settings.focus==0?"ds:blow":"ds:lid");return;}
        if(settings.focus==2)show_picker("DS language",{"Japanese","English","French","German","Italian","Spanish"},ds_settings.count("language")?std::atoi(ds_settings["language"].c_str()):1,[this](int n){save_ds_setting("language",n);});
        else if(settings.focus==4)begin_edit(5);
        else save_ds_setting("microphone",ds_settings["microphone"]=="0");
    }
    void save_ds_setting(const std::string& key,int value){save_ds_setting(key,std::to_string(value));}
    void save_ds_setting(const std::string& key,const std::string& value){
        try{ds_settings[key]=value;auto folder=item_folder(model.selected);fs::create_directories(folder);auto path=folder/"ds-input.ini",pending=fs::path(path.string()+".pending");std::ofstream out(pending);for(auto& pair:ds_settings)out<<pair.first<<'='<<pair.second<<'\n';out.close();if(out){fs::rename(pending,path);status="Restart the game to apply DS settings.";}}catch(const std::exception&){status="Could not save DS settings.";}dirty=true;
    }
    bool scan_stay=false;   // a scan from Add your games keeps that page open
    void rescan_games(bool stay=false){stop_library_scan();library_bytes.clear();library_eof=false;library_ready=false;scan_stay=stay;if(!stay){home_menu=true;settings.page=Page::Library;}begin_library_scan();dirty=true;}
    // Add your games: opened by itself on a first start (Done then goes to the
    // library) or from Library settings (Done goes back there). A scan from the
    // page stays on it and reports what it found.
    bool setup_first_run=false,setup_scanned=false;
    bool backup_requested=false;   // Back up all saves was chosen; its outcome shows on the Library page
    void confirm_reset_state(){
        show_picker("Restart without loading a state?",{"Cancel","Reset state"},0,[this](int n){if(n)menu_request("reset-state");});
    }
    void quick_action() {
        if(menu_status["state"]=="busy")return;

        if(settings.focus==0)menu_request("resume");
        else if(settings.focus==1 || settings.focus==2)open_gallery();
        else if(settings.focus==3){game_preferences=library_cache.reload_options(model.games[0]);if(experimental_game()){settings.page=Page::Graphics;settings.focus=0;status.clear();}else{open_shader_picker(true);shader_return=Page::QuickMenu;}}
        else if(settings.focus==4)open_emulator_options();
        else if(settings.focus==5){settings.section=3;settings.page=Page::Sound;}
        else if(settings.focus==6){settings.section=0;settings.page=Page::Display;}
        else if(settings.focus==7)menu_request("restart");
        else if(settings.focus==9)open_controls(true,Page::QuickMenu);
        else if(settings.focus==10){load_switcher();settings.page=Page::GameSwitcher;settings.focus=0;}
        else if(menu_status["state"]=="error"){settings.page=Page::LeaveGame;settings.focus=0;}
        else menu_request("quit");dirty=true;
    }
    int active_engine()const{return int(std::find(std::begin(core_ids),std::end(core_ids),o.game_core)-std::begin(core_ids));}
    void open_emulator_options(){game_preferences=library_cache.reload_options(model.games[0]);status.clear();settings.page=Page::EmulatorOptions;settings.focus=0;dirty=true;}
    std::string active_graphics_name()const{
        auto found=menu_status.find("active_scale");
        if(found==menu_status.end())return "Waiting for renderer";
        int scale=std::clamp(std::atoi(found->second.c_str()),1,5);
        return scale==1?"Native / software":std::to_string(scale)+"x Vulkan / "+std::to_string(scale*256)+" x "+std::to_string(scale*192);
    }
    void apply_experimental_graphics(const GameOptions& next){
        if(!o.game_menu || o.preview){
            apply_game_preferences(next,o.game_menu?"Graphics applied (preview).":"Graphics saved.");
            if(o.game_menu){menu_status["active_scale"]=std::to_string(next.rgds_scale);menu_status["active_smooth"]=std::to_string(next.rgds_smooth);menu_status["active_filter2d"]=std::to_string(next.rgds_2d);}
            return;
        }
        menu_request("graphics:"+std::to_string(next.rgds_scale)+":"+std::to_string(next.rgds_smooth)+":"+std::to_string(std::max(0,next.frameskip)));
    }
    static std::string filter2d_name(int mode){return mode==1?"Sharp":mode==2?"Smooth":mode==3?"Crisp":"Native pixels";}
    void apply_filter2d(int mode){
        auto next=game_preferences;next.rgds_2d=mode;
        if(!o.game_menu || o.preview){
            apply_game_preferences(next,o.game_menu?"2D scaling applied (preview).":"2D scaling saved.");
            if(o.game_menu)menu_status["active_filter2d"]=std::to_string(mode);
            return;
        }
        menu_request("filter2d:"+std::to_string(mode));
    }
    void apply_adaptive(bool on){
        // In a game the engine switches live (adaptive:N); the runtime saves the
        // choice once it is applied. Outside, or in a preview, only save it.
        if(!o.game_menu || o.preview){
            auto next=game_preferences;next.rgds_adaptive=on;
            apply_game_preferences(next,o.game_menu?"Heavy 3D scenes applied (preview).":"Heavy 3D scenes saved.");
            return;
        }
        menu_request(std::string("adaptive:")+(on?"1":"0"));
    }
    void graphics_action(){
        if(settings.focus==0)show_experimental_resolution_picker();
        else if(settings.focus==1)show_picker("3D edge smoothing",{"Sharp","Smooth (3x and above)"},game_preferences.rgds_smooth,[this](int n){auto next=game_preferences;next.rgds_smooth=n;apply_experimental_graphics(next);});
        else if(settings.focus==2)show_picker("Heavy 3D scenes (2x and above)",{"Adaptive (holds 60 fps)","Always sharp"},game_preferences.rgds_adaptive?0:1,[this](int n){apply_adaptive(n==0);});
        else if(settings.focus==3)show_picker("2D scaling (2x and above)",{"Native pixels","Sharp","Smooth","Crisp"},game_preferences.rgds_2d,[this](int n){apply_filter2d(n);});
        else if(settings.focus==4){open_shader_picker(true);shader_return=Page::Graphics;}
        else if(settings.focus==5)show_picker("Frameskip",{"Off (smooth)","1 frame","2 frames","3 frames"},std::max(0,game_preferences.frameskip),[this](int n){auto next=game_preferences;next.frameskip=n;apply_experimental_graphics(next);});
        else if(settings.focus==7)menu_request("restart");
        dirty=true;
    }
    bool emulator_option_enabled(int row){
        if(row>=(active_engine()==0?10:8) && row<list_count())return true;
        if(superdrastic_game() && (row==0 || row==9))return false;
        if(row==2)return hardware.power;
        if(row==3)return active_engine()==0 || active_engine()==3;
        if(row==4)return active_engine()==0 || active_engine()==2 || active_engine()==4;
        if(row==5)return active_engine()==0 || active_engine()==1 || active_engine()==4;
        return row>=0 && (row<8 || ((row==8 || row==9) && active_engine()==0));
    }
    void change_emulator_option(int direction){
        if(settings.focus==list_count()-1){confirm_reset_state();return;}
        if(settings.focus==list_count()-2){open_ds_controls();return;}
        if(!emulator_option_enabled(settings.focus))return;
        int row=settings.focus-1,engine=active_engine();
        if(row<0){
            if(o.preview){menu_status["fps"]=menu_status["fps"]=="1"?"0":"1";dirty=true;}
            else menu_request(menu_status["fps"]=="1"?"fps:0":"fps:1");
            return;
        }
        if(row==0){menu_request("fast-forward");return;}
        if(row==1){change_system(0,direction);return;}
        if(row==2 && engine==3){menu_request(menu_status["threaded"]=="0"?"threaded:1":"threaded:0");return;}
        auto next=game_preferences;
        if(row==2 && engine==0)next.threaded=next.threaded==0?1:0;
        else if(row==3)next.hires=next.hires==1?0:1;
        else if(row==4){if(!direction)return;next.frameskip=std::clamp((next.frameskip<0?0:next.frameskip)+direction,0,3);}
        else if(row==6){menu_request("restart");return;}else return;
        if(experimental_game())apply_experimental_graphics(next);else apply_game_preferences(next,"Saved. Save & restart applies this change.");
    }
    void watch_menu(Uint32 now) {
        // Pending actions should acknowledge promptly; idle menus keep their
        // slower poll. The first frame starts with the supervisor's real state.
        if(!o.game_menu || o.preview || (rendered_frames && now-menu_poll<(menu_pending_token.empty() && menu_status["state"]!="busy"?100u:20u)))return;menu_poll=now;
        auto value=fields(o.data/"session/game-menu.status");
        if(value["view_request"]!=menu_status["view_request"] && std::atoi(value["view_request"].c_str())>0){
            picker_open=false;cancel_navigation();
            if(value["view"]=="switcher"){load_switcher();settings.page=Page::GameSwitcher;}
            else settings.page=Page::QuickMenu;
            settings.focus=0;dirty=true;
        }
        // The old status can still be on disk after a tap. Keep the local
        // busy state until the supervisor acknowledges this exact request.
        if(menu_pending_token.empty() || value["request_token"]==menu_pending_token){
            if(!menu_pending_token.empty() && value["state"]!="busy"){
                if(value["state"]=="error")emit_sound(MenuCue::Error);
                // The direct present has no GL pass: the filter is saved and needs the renderer, so offer the restart.
                else if(menu_sound_action.rfind("shader:",0)==0 && value["message"].rfind("Shader saved",0)==0){settings.page=Page::ApplyShader;settings.focus=0;}
                else if(menu_sound_action=="save" || menu_sound_action.rfind("slot-save:",0)==0)emit_sound(MenuCue::Save);
                else if(menu_sound_action=="load" || menu_sound_action.rfind("slot-load:",0)==0)emit_sound(MenuCue::Load);
                // The supervisor saves live graphics choices after the engine accepts them; show what it stored.
                if((menu_sound_action.rfind("graphics:",0)==0 || menu_sound_action.rfind("filter2d:",0)==0 || menu_sound_action.rfind("adaptive:",0)==0) && value["state"]!="error")game_preferences=library_cache.reload_options(model.games[0]);
                menu_sound_action.clear();
            }
            if(value["state"]!="busy")menu_pending_token.clear();if(!same_fields(value,menu_status)){menu_status=std::move(value);dirty=true;}
        }else if(now-menu_request_at>(menu_sound_action.rfind("graphics:",0)==0?50000u:15000u)){
            menu_pending_token.clear();emit_sound(MenuCue::Error);menu_status["state"]="error";
            menu_status["message"]="The game menu did not respond. Resume or return without saving.";dirty=true;
        }
        auto volume=fields(o.data/"volume.ini");if(volume.count("volume")){
            int level=std::clamp(std::atoi(volume["volume"].c_str()),0,100);
            volume_applied=level;
            if(!volume_unsaved && !system.volume_pending() && level!=hardware.volume_percent){hardware.volume_percent=level;if(level>0)unmuted_volume=level;volume_toast=now+1800;dirty=true;}
        }
        if(menu_status["state"]=="home")quit=true;
    }
    void start_worker(const std::string& operation,const std::string& payload="") {
        if(worker>0 && (worker_action=="device.info" || worker_action=="device.update-auto") && operation!="device.info")stop_worker();
        if(worker>0 && worker_action=="index" && operation.rfind("device.",0)==0)stop_worker();
        if(worker>0)return;
        bool busy_visible=operation!="device.info" || settings.page==Page::Startup || settings.page==Page::Wifi || !service_notice.empty();
        service_notice.clear();
        bool device=operation.rfind("device.",0)==0;auto folder=o.data/(device?"device":"scraper");fs::create_directories(folder);
        fs::path script=fs::path("runtime")/(device?"device_services.py":"scraper.py");
        std::vector<std::string> args={"python3","-B",(o.app.empty()?script:o.app/script).string(),device?operation.substr(7):operation,"--data",o.data.string()};
        if(device){args.push_back("--app");args.push_back((o.app.empty()?fs::current_path():o.app).string());if(o.preview)args.push_back("--preview");}
        else {static const std::array<const char*,3> providers={"auto","gametdb","libretro"};args.push_back("--provider");args.push_back(operation=="scrape" && settings.page==Page::GameArtwork?"auto":providers.at(preferences.artwork_provider));
            if(operation=="scrape" && settings.page==Page::GameArtwork && !model.games.empty()){args.push_back("--rom");args.push_back(model.games[model.selected].path.string());args.push_back("--force");}
            for(auto root:o.roots){args.push_back("--rom-dir");args.push_back(root.string());}}
        int channel[2];if(pipe(channel)!=0){status="Could not start artwork service";return;}
        auto child=fork();
        if(child==0) {
            ::close(channel[1]);dup2(channel[0],STDIN_FILENO);::close(channel[0]);
            int log=::open((folder/"worker.log").c_str(),O_WRONLY|O_CREAT|O_APPEND,0600);
            if(log>=0){dup2(log,STDOUT_FILENO);dup2(log,STDERR_FILENO);::close(log);}
            std::vector<char*> argv;for(auto& arg:args)argv.push_back(arg.data());argv.push_back(nullptr);execvp(argv[0],argv.data());_exit(127);
        }
        ::close(channel[0]);if(!payload.empty() && child>0){auto sent=write(channel[1],payload.data(),payload.size());(void)sent;}::close(channel[1]);
        if(child<0){status="Could not start artwork service";return;}
        worker=child;worker_action=operation;worker_ok=false;if(busy_visible)dirty=true;
    }
    void stop_worker() {
        if(worker<=0)return;
        if(waitpid(worker,nullptr,WNOHANG)==0) {
            kill(worker,SIGTERM);
            for(int i=0;i<100;i++){if(waitpid(worker,nullptr,WNOHANG)!=0){worker=-1;return;}usleep(10000);}
            kill(worker,SIGKILL);waitpid(worker,nullptr,0);
        }
        worker=-1;
    }
    Uint32 worker_finished_at=0;
    void poll_service(Uint32 now) {
        bool artwork_finished=false;
        if(worker>0){int result=0;auto finished=waitpid(worker,&result,WNOHANG);if(finished>0){worker_ok=WIFEXITED(result) && WEXITSTATUS(result)==0;worker=-1;worker_finished_at=now;service_poll=0;artwork_finished=worker_action=="index" || worker_action=="scrape";if(worker_action!="device.info" || settings.page==Page::Startup || settings.page==Page::Wifi)dirty=true;}}
        if(artwork_finished){if(settings.page==Page::GameArtwork && !model.games.empty())artwork_result=fields(item_folder(model.selected)/"artwork-status.ini");library_cache.invalidate_metadata();if(media.refresh())library_cache.invalidate_titles();}
        // Status files change only through workers; poll quickly while one runs.
        bool quick=worker>0 || now-worker_finished_at<1000 || settings.page==Page::SaveGallery || !menu_pending_token.empty();
        if(now-service_poll<(quick?250u:2000u))return;service_poll=now;
        auto next=fields(o.data/"scraper/status.ini");if(!same_fields(next,service)){service=std::move(next);library_cache.invalidate_metadata();if(media.refresh())library_cache.invalidate_titles();dirty=true;}
        auto device=fields(o.data/"device/status.ini");if(!same_fields(device,device_status)){device_status=std::move(device);dirty=true;}
        experience=fields(o.data/"experience.ini");if(!experience.count("auto_update"))experience["auto_update"]="1";
        if(settings.page==Page::SaveGallery && !model.games.empty()){auto next=fields(gallery_root()/"index.ini");if(next!=slots){slots=std::move(next);media.refresh();dirty=true;}}
        auto update=fields(o.data/"update/status.ini");if(!same_fields(update,update_status)){update_status=std::move(update);dirty=true;}
        if(worker<=0 && worker_ok && worker_action=="device.update-apply" && update_status["state"]=="restarting")quit=true;
        if(shutting_down() && worker<=0 && !worker_ok){shutdown_at=0;shutdown_kind.clear();dirty=true;}
        if(worker<=0 && !worker_ok && worker_action.rfind("device.",0)==0 && device_status["state"]!="error"){device_status["state"]="error";device_status["message"]="This request did not finish. Please try again.";dirty=true;}
        if(worker<=0 && worker_ok && (worker_action=="device.poweroff" || worker_action=="device.reboot") && device_status["state"]=="ready"){system_exit_ack=!o.preview;quit=true;}
    }
    void select_game(size_t index,bool animate=true) {
        if(model.games.empty())return;
        model.selected=std::min(index,model.games.size()-1);selected_at=s.GetTicks();if(animate)cover_motion_at=selected_at;browser_list.select(model.selected,model.games.size(),selected_at,animate && preferences.animations);
        if(animate && preferences.animations)carousel.select(model.selected,selected_at);else carousel.reset(model.selected);
        dirty=true;
    }
    void navigate(int delta) {
        SoundAction sound_action(*this,delta<0?0:1);
        motion_input(delta<0?0:1);
        if(!navigation_count())return;
        long count=static_cast<long>(navigation_count()),previous=static_cast<long>(navigation_selected());
        size_t next=static_cast<size_t>((previous+delta%count+count)%count);if(next!=size_t(previous))sound_intent(MenuCue::Navigate);select_navigation(next,std::abs(previous-static_cast<long>(next))<=3);
    }
    void library_view(int view) {
        cancel_navigation();
        std::string selected=model.games.empty()?"":model.games[model.selected].demo?"demo:"+model.games[model.selected].title:model.games[model.selected].path.string();
        preferences.view=view;collection_filter.cancel();collection_loading=false;
        if((view!=0 && all_games.size()>256) || !search_query.empty() || discovery_filter){
            collection_selection=selected;collection_loading=true;collection_filter.request(o.data,all_games,view,search_query,discovery_filter);
            model.games.clear();model.selected=0;carousel.reset(0);browser_list.scroll=0;dirty=true;return;
        }
        model.games=all_games;
        if(view==1)model.games.erase(std::remove_if(model.games.begin(),model.games.end(),[&](const Game& game){return !library_cache.options(game,true).favourite;}),model.games.end());
        if(view==2) {
            // Snapshot each date once. Sorting a large collection never performs
            // filesystem reads or churns the bounded detail cache in a comparator.
            std::vector<std::pair<Game,std::string>> recent;recent.reserve(model.games.size());
            for(const auto& game:model.games){auto date=library_cache.stats(game,true).last_played;if(!date.empty())recent.emplace_back(game,std::move(date));}
            std::stable_sort(recent.begin(),recent.end(),[](const auto& a,const auto& b){return a.second>b.second;});
            model.games.clear();model.games.reserve(recent.size());for(auto& item:recent)model.games.push_back(std::move(item.first));
        }
        model.selected=0;if(!selected.empty())for(size_t i=0;i<model.games.size();i++)if((model.games[i].demo?"demo:"+model.games[i].title:model.games[i].path.string())==selected)model.selected=i;
        carousel.reset(model.selected);browser_list.scroll=0;browser_list.reveal(model.selected,model.games.size());dirty=true;
    }
    void poll_collection(){
        try{auto result=collection_filter.poll();if(!result || !collection_loading)return;
            model.games=std::move(result->games);collection_loading=false;model.selected=0;
            for(size_t i=0;i<model.games.size();i++)if(model.games[i].path.string()==collection_selection){model.selected=i;break;}
            carousel.reset(model.selected);browser_list.reveal(model.selected,model.games.size());dirty=true;
        }catch(const std::exception&){collection_loading=false;status="Could not read this collection. Try again.";dirty=true;}
    }
    void open_game_options() {
        if(model.games.empty())return;
        game_preferences=library_cache.options(model.games[model.selected],true);status.clear();settings.page=Page::GameOptions;settings.focus=0;settings.section=2;dirty=true;
    }
    bool apply_game_preferences(const GameOptions& next,const char* message){
        try{save_game_options(item_folder(model.selected)/"game.ini",next);game_preferences=next;library_cache.saved_options(model.games[model.selected],next);if(!o.game_menu)library_summary.request(o.data,all_games);status=message;}
        catch(const std::exception& e){status="Could not save game settings. Previous values kept.";std::cerr<<e.what()<<std::endl;dirty=true;return false;}dirty=true;return true;
    }
    void choose_game_emulator(int core){
        if(core==game_preferences.core)return;
        auto next=game_preferences;next.core=core;
        apply_game_preferences(next,o.game_menu?"Saved. Save & restart changes the emulator.":"Game emulator saved.");
    }
    void change_game_option(int delta,bool toggle=false) {
        auto next=game_preferences;
        if(settings.focus==0)next.favourite=toggle?!next.favourite:delta>0;
        else if(settings.focus==1){int& value=next.core;const int step=toggle || delta>0?1:-1;do value=toggle?(value>=core_count-1?-1:value+1):std::clamp(value+step,-1,core_count-1);while(value==retired_core);}
        else {open_shader_picker(true);return;}
        apply_game_preferences(next,"Game options saved");
    }
    std::string shader_name(int value){return value<0?"Default":shaders.at(std::clamp(value,0,shader_count-1)).name;}
    bool experimental_game()const{
        if(o.game_menu)return o.game_core=="rgds-experimental";
        return (game_preferences.core<0?int(preferences.core):game_preferences.core)==4;
    }
    std::string experimental_resolution_name()const{return game_preferences.rgds_scale==1?"Native":std::to_string(game_preferences.rgds_scale)+"x";}
    void show_experimental_resolution_picker(){
        show_picker("DSCore 3D resolution",{"Native (fastest)","2x sharper","3x sharper","4x (experimental)","5x (experimental)"},game_preferences.rgds_scale-1,[this](int n){
            auto next=game_preferences;next.rgds_scale=n+1;
            apply_experimental_graphics(next);
        });
    }
    bool superdrastic_game()const{
        int core=game_preferences.core<0?int(preferences.core):game_preferences.core;
        int backend=game_preferences.drastic_backend<0?preferences.drastic_backend:game_preferences.drastic_backend;
        if(o.game_menu)return o.game_core=="drastic" && std::getenv("RGDS_SD_SHARED_MENU");
        return core==0 && backend==1;
    }
    int superdrastic_resolution()const{return game_preferences.hires==1?(game_preferences.gengis_scale>=2?game_preferences.gengis_scale:1):0;}
    const char* superdrastic_resolution_name()const{
        static const char* names[]={"Native","2x","2x Gengis","3x Gengis"};return names[superdrastic_resolution()];
    }
    void show_superdrastic_resolution_picker(){
        show_picker("3D resolution",{"Native","2x (DraStic)","2x Gengis (experimental)","3x Gengis (experimental)"},superdrastic_resolution(),[this](int n){
            auto next=game_preferences;next.hires=n>0;next.gengis_scale=n>=2?n:0;
            apply_game_preferences(next,"Saved. Applies on your next SuperDrastic launch.");
        });
    }
    void open_shader_picker(bool per_game) {
        if(superdrastic_game()){status="SuperDrastic currently uses native output. Your standard shader choice is kept.";dirty=true;return;}
        shader_per_game=per_game;shader_return=per_game?Page::GameOptions:Page::LibraryOptions;
        shader_core=o.game_menu?active_engine():per_game && game_preferences.core>=0?game_preferences.core:int(preferences.core);
        shader_choices.clear();if(per_game)shader_choices.push_back(-1);
        for(auto place:shader_order)if(shader_supported(place.id,shader_core))shader_choices.push_back(place.id);
        int selected=per_game?game_preferences.filter:preferences.filter;
        auto found=std::find(shader_choices.begin(),shader_choices.end(),selected);
        settings.page=Page::ShaderPicker;settings.focus=found==shader_choices.end()?0:int(found-shader_choices.begin());
        shader_carousel.reset(size_t(settings.focus));shader_drag=false;dirty=true;
    }
    void close_shader_picker(){settings.page=shader_return;settings.focus=shader_return==Page::QuickMenu?3:shader_return==Page::Graphics?4:shader_per_game?2:4;dirty=true;}
    fs::path shader_preview_folder()const{return o.app.empty()?fs::path("assets/shader-previews"):o.app/"assets/shader-previews";}
    int preview_shader()const{int selected=shader_choices.empty()?0:shader_choices.at(std::clamp(settings.focus,0,int(shader_choices.size())-1));return selected<0?preferences.filter:selected;}
    void change_shader_preview(int code){
        if(code==11)shader_preview_scene=(shader_preview_scene+1)%ShaderPreview::scene_count;
        else if(code==12)shader_preview_mode=ShaderPreviewMode((int(shader_preview_mode)+1)%3);
        else{shader_preview_mode=ShaderPreviewMode::Split;shader_preview_split=std::clamp(shader_preview_split+(code==13?-.1f:.1f),.1f,.9f);}
        dirty=true;
    }
    int shader_choice_family(int index)const{
        if(shader_choices.empty())return 0;int value=shader_choices.at(std::clamp(index,0,int(shader_choices.size())-1));return value<0?0:shader_family(value);
    }
    void focus_shader(int index){
        if(shader_choices.empty())return;index=std::clamp(index,0,int(shader_choices.size())-1);
        if(index!=settings.focus){settings.focus=index;selected_at=s.GetTicks();}
        if(preferences.animations)shader_carousel.select(size_t(index),s.GetTicks());else shader_carousel.reset(size_t(index));dirty=true;
    }
    // Up/Down, L/R and the group chips jump to the first preset of a group.
    void shader_family_jump(int family){for(int i=0;i<int(shader_choices.size());i++)if(shader_choice_family(i)==family){focus_shader(i);return;}}
    void shader_family_step(int direction){
        const int family=shader_choice_family(settings.focus);
        for(int step=1;step<=shader_family_count;step++){
            const int wanted=((family+direction*step)%shader_family_count+shader_family_count)%shader_family_count;
            for(int i=0;i<int(shader_choices.size());i++)if(shader_choice_family(i)==wanted){focus_shader(i);return;}
        }
    }
    static SDL_Rect shader_chip(int family){return {24+family*99,86,95,32};}
    // Touch on the carousel or its rail. Taps elsewhere use the shared tap path.
    bool shader_pointer_down(int x,int y){
        const size_t count=shader_choices.size();if(!count)return false;
        if(y>=409 && y<480){
            if(x<RailGeometry::left){focus_shader(settings.focus-1);return true;}
            if(x>=RailGeometry::left+RailGeometry::width){focus_shader(settings.focus+1);return true;}
            shader_carousel.seek_down(x,y,count,s.GetTicks());shader_drag=true;
            int index=int(shader_carousel.drag(x,count));if(index!=settings.focus){settings.focus=index;selected_at=s.GetTicks();}dirty=true;return true;
        }
        if(y>=128 && y<330){shader_carousel.down(x,y,false,s.GetTicks());shader_drag=true;dirty=true;return true;}
        return false;
    }
    fs::path controls_file(bool per_game){return per_game && !model.games.empty()?item_folder(model.selected)/"controls.ini":o.data/"controls.ini";}
    static std::string control_title(int source){
        static const std::array<const char*,17> labels={"A","B","X","Y","L","R","Start","Select","Up","Down","Left","Right","L2","R2","Left stick","Right stick","Unassigned"};
        return source>=0 && source<int(labels.size())?labels[source]:"Unassigned";
    }
    void load_controls_editor(){
        auto global=controls_file(false),local=controls_per_game?controls_file(true):fs::path{};
        edited_controls.load(global.string(),local.string());
        auto saved=local.empty()?std::map<std::string,std::string>{}:fields(local);
        controls_inherit=controls_per_game && (saved.empty() || saved["inherit"]=="1");
    }
    void open_controls(bool per_game,Page parent=Page::Settings){
        controls_return=parent;controls_per_game=per_game && !model.games.empty();control_capture=false;capture_logical=-1;controls_notice.clear();
        settings.page=Page::Controls;settings.section=6;settings.focus=0;load_controls_editor();dirty=true;
    }
    void close_controls(){
        control_capture=false;capture_logical=-1;settings.page=controls_return;
        settings.focus=controls_return==Page::QuickMenu?9:controls_return==Page::GameOptions?3:0;
        if(controls_return==Page::GameOptions)settings.section=2;
        if(controls_return==Page::Settings)settings.section=6;dirty=true;
    }
    bool save_controls_editor(){
        try{
            auto path=controls_file(controls_per_game);fs::create_directories(path.parent_path());auto pending=path.string()+".pending";
            std::ofstream file(pending);file<<"schema=1\npreset="<<edited_controls.preset<<"\nshortcut="<<edited_controls.shortcut<<"\nshortcut_action="<<edited_controls.shortcut_action<<'\n';
            file<<"fast_forward_button="<<physical_names[edited_controls.fast_forward_source]<<"\nfast_forward_mode="<<edited_controls.fast_forward_mode<<'\n';
            if(controls_per_game)file<<"inherit="<<controls_inherit<<'\n';
            for(int logical=0;logical<12;logical++)file<<"bind_"<<control_names[logical]<<'='<<physical_names[edited_controls.sources[logical]]<<'\n';
            file.close();if(!file)throw std::runtime_error("Could not write controls");fs::rename(pending,path);
            controls_notice=controls_inherit?"Following your global controls.":controls_per_game?"Controls saved for this game.":"Global controls saved.";
            dirty=true;return true;
        }catch(const std::exception& error){std::cerr<<"Control settings: "<<error.what()<<'\n';controls_notice="Could not save controls. Check writable storage.";dirty=true;return false;}
    }
    void capture_control(int source){
        if(!control_capture || capture_logical<0 || source<0 || source>control_none)return;
        auto previous=edited_controls;bool previous_inherit=controls_inherit;
        int old=capture_logical==12?edited_controls.fast_forward_source:edited_controls.sources[capture_logical];
        if(source!=control_none)for(int logical=0;logical<12;logical++)if(logical!=capture_logical && edited_controls.sources[logical]==source)edited_controls.sources[logical]=old;
        if(capture_logical==12)edited_controls.fast_forward_source=source;
        else{if(source!=control_none && edited_controls.fast_forward_source==source)edited_controls.fast_forward_source=old;edited_controls.sources[capture_logical]=source;}
        edited_controls.preset="custom";controls_inherit=false;
        if(!save_controls_editor()){edited_controls=previous;controls_inherit=previous_inherit;}
        control_capture=false;capture_logical=-1;dirty=true;
    }
    std::string controls_row_name(int row){
        if(row==0)return "Profile";if(row==1)return "Button layout";if(row==2)return "Home shortcut";if(row==3)return "Shortcut action";if(row==16)return controls_per_game?"Use global controls":"Restore defaults";
        if(row==17)return "Fast-forward button";if(row==18)return "Fast-forward style";
        return "Button "+control_title(row-4);
    }
    std::string controls_row_value(int row){
        if(row==0)return controls_per_game?"This game":"Global";
        if(row==1)return controls_inherit?"Inherited":edited_controls.preset=="default"?"Default":edited_controls.preset=="ab-swap"?"A / B swapped":edited_controls.preset=="left-handed"?"Left-handed":"Custom";
        if(row==2)return edited_controls.shortcut=="start+menu"?"Start + Anbernic":edited_controls.shortcut=="startthenmenu"?"Start, then Anbernic":"Anbernic";
        if(row==3)return edited_controls.shortcut_action=="home"?"Return home":"Open menu";
        if(row==16)return controls_per_game?"Inherit":"Reset";
        if(row==17)return control_capture && capture_logical==12?"Press a button…":control_title(edited_controls.fast_forward_source);
        if(row==18)return edited_controls.fast_forward_mode=="hold"?"Hold":"Toggle";
        return control_capture && row==capture_logical+4?"Press a button…":control_title(edited_controls.sources[row-4]);
    }
    void controls_action(int code){
        if(control_capture){if(code==3 || code==7){control_capture=false;capture_logical=-1;controls_notice="Binding unchanged.";}dirty=true;return;}
        if(code==3 || code==7){close_controls();return;}
        if(code==8 || code==9){settings.focus=std::clamp(settings.focus+(code==8?-1:1),0,18);dirty=true;return;}
        if(code!=0 && code!=1 && code!=2 && code!=4)return;
        int row=settings.focus,direction=code==0?-1:1;auto previous=edited_controls;bool old_inherit=controls_inherit;
        if(row==0){if(!model.games.empty()){controls_per_game=!controls_per_game;load_controls_editor();controls_notice.clear();}dirty=true;return;}
        if((row>=4 && row<=15) || row==17){
            int target=row==17?12:row-4;
            if(code==2 || code==4){control_capture=true;capture_logical=target;capture_started=capture_clock=s.GetTicks();controls_notice="Press a button or direction. Anbernic / Escape cancels.";}
            else{int selected=target==12?edited_controls.fast_forward_source:edited_controls.sources[target];int next=(selected+direction+17)%17;control_capture=true;capture_logical=target;capture_control(next);}dirty=true;return;
        }
        if(row==1){const std::array<std::string,3> names={"default","ab-swap","left-handed"};auto found=std::find(names.begin(),names.end(),edited_controls.preset);int index=found==names.end()?-1:int(found-names.begin());edited_controls.apply({{"preset",names[(index+direction+3)%3]}});controls_inherit=false;}
        else if(row==2){const std::array<std::string,3> names={"start+menu","anbernic","startthenmenu"};int index=int(std::find(names.begin(),names.end(),edited_controls.shortcut)-names.begin());edited_controls.shortcut=names[(index+direction+3)%3];controls_inherit=false;}
        else if(row==3){edited_controls.shortcut_action=edited_controls.shortcut_action=="home"?"menu":"home";controls_inherit=false;}
        else if(row==16){if(controls_per_game){controls_inherit=true;edited_controls.load(controls_file(false).string());}else edited_controls=ControlMapping{};}
        else if(row==18){edited_controls.fast_forward_mode=edited_controls.fast_forward_mode=="hold"?"toggle":"hold";controls_inherit=false;}
        if(!save_controls_editor()){edited_controls=previous;controls_inherit=old_inherit;}dirty=true;
    }
    void render_shader_preview(SDL_Renderer* r,int y){
        int selected=preview_shader();
        const int focused=shader_choices.empty()?0:shader_choices.at(settings.focus);
        const bool in_use=(shader_per_game?game_preferences.filter:preferences.filter)==focused;
        const std::string group=locale.text("Shader studio")+"  ·  "+locale.text(shader_family_names[shader_choice_family(settings.focus)]);
        raw_label(r,group,30,y+65,16,"muted",in_use?440:580);
        label(r,shader_name(focused),30,y+90,28,"ink",in_use?440:570);
        if(in_use){const std::string text=locale.text("IN USE");const int w=label_width(text,14,false)+28;raised_pill(r,616-w,y+94,w,30,15,theme.at("accent"));raw_label(r,text,616-w+14,centred_label_y(text,14,y+109),14,"on_accent",w-20);}
        round_rect(r,24,y+137,378,282,14,"line");round_rect(r,26,y+139,374,278,12,"panel");
        SDL_Rect frame{30,y+143,366,274};
        bool ready=shader_preview.draw(r,shader_preview_folder(),shader_preview_scene,selected,frame,shader_preview_mode,shader_preview_split);
        if(!ready){wrapped(r,"Preview unavailable",58,y+251,24,314,2);label(r,"Your shader choice is still available.",42,y+302,16,"muted",340);}
        round_rect(r,416,y+137,200,160,12,"line");round_rect(r,418,y+139,196,156,10,"panel");
        SDL_Rect detail{422,y+145,188,141};shader_preview.detail(r,shader_preview_folder(),shader_preview_scene,selected,detail,shader_preview_mode,shader_preview_split);
        label(r,"Magnified detail",420,y+308,16,"muted",188);
        label(r,ShaderPreview::scene_name(shader_preview_scene),420,y+334,20,"ink",188);
        label(r,shader_preview_mode==ShaderPreviewMode::Split?"Before / after":shader_preview_mode==ShaderPreviewMode::Original?"Original pixels":"With shader",420,y+365,16,"accent",188);
        label(r,"X Scene   Y Compare",420,y+394,16,"muted",188);
        if(ready && shader_preview_mode==ShaderPreviewMode::Split){int x=frame.x+int(frame.w*shader_preview_split);rect(r,x,y+143,2,274,"on_accent");round_rect(r,frame.x+7,y+150,68,24,5,"panel");label(r,"Before",frame.x+14,y+152,16,"ink",60);round_rect(r,frame.x+frame.w-73,y+150,66,24,5,"panel");label(r,"After",frame.x+frame.w-64,y+152,16,"ink",59);}
        std::string note=status.rfind("Could not save",0)==0?status:o.game_menu && !menu_status["message"].empty()?menu_status["message"]:"Up / Down or L / R: change group";
        wrapped(r,note,30,y+433,16,580,2);
    }
    void render_shader_carousel(SDL_Renderer* r,int y){
        settings_header(r,y,"Shaders");
        secondary_button(r,368,y+24,120,42,12,pointer_down && contact_x>=368 && contact_x<=488 && contact_y>=24 && contact_y<=66);ui_icon(r,SettingsIcon::Art,386,y+45,18,"accent");label(r,"Scene",403,y+35,17,"ink",74);
        secondary_button(r,496,y+24,128,42,12,pointer_down && contact_x>=496 && contact_y>=24 && contact_y<=66);ui_icon(r,SettingsIcon::Shader,514,y+45,18,"accent");label(r,"Compare",530,y+35,16,"ink",85);
        const int count=int(shader_choices.size()),selection=std::clamp(settings.focus,0,std::max(0,count-1)),family=shader_choice_family(selection);
        for(int f=0;f<shader_family_count;f++){
            const auto chip=shader_chip(f);const bool on=f==family,pressed=pointer_down && !shader_drag && point_in(chip,down_x,down_y) && point_in(chip,contact_x,contact_y);
            if(on)raised_pill(r,chip.x,y+chip.y,chip.w,chip.h,16,theme.at("accent"),pressed?1:0);else secondary_button(r,chip.x,y+chip.y,chip.w,chip.h,16,pressed);
            const std::string text=locale.text(shader_family_names[f]);const int w=std::min(chip.w-14,label_width(text,14,false)),sink=pressed?2:0;
            raw_label(r,text,chip.x+(chip.w-w)/2,centred_label_y(text,14,y+chip.y+chip.h/2+sink),14,on?"on_accent":"ink",chip.w-14);
        }
        // The Home tile, frame and action label; each tile shows its effect at 1:1.
        SDL_Rect clip{0,y+124,640,206};s.RenderSetClipRect(r,&clip);
        const int tile=128;int budget=2;bool pending=false;
        const int first=int(std::floor(shader_carousel.position))-3;
        const int current=shader_per_game?game_preferences.filter:preferences.filter;
        for(int pass=0;pass<2;pass++)for(int offset=0;offset<8;offset++){
            const int index=first+offset;if(index<0 || index>=count || (index==selection)!=(pass==1))continue;
            const double distance=index-shader_carousel.position;const int x=int(std::lround(320+distance*Carousel::spacing))-tile/2;
            if(x>=640 || x+tile<=0)continue;
            const bool selected=index==selection;
            const bool pressed=selected && pointer_down && shader_drag && !shader_carousel.moved && !shader_carousel.range && int(shader_carousel.tapped(down_x,size_t(count)))==selection;
            const int lift=pressed?-2:selected && preferences.animations?int(std::max(0.0,1.0-(s.GetTicks()-selected_at)/190.0)*4):0,row=y+144-lift;
            const int value=shader_choices[size_t(index)],shown=value<0?preferences.filter:value;
            if(selected){
                if(preferences.dark){round_rect(r,x-15,row-14,tile+30,tile+52,10,"line");round_rect(r,x-13,row-12,tile+26,tile+48,8,"focus");}
                round_rect(r,x-9,row-9,tile+18,tile+44,7,"accent");
                round_rect(r,x-3,row-3,tile+6,tile+8,6,"line");centered_tile_label(r,value==current?"IN USE":"APPLY",x-8,row+tile+9,tile+16);
            }
            round_rect(r,x,row+4,tile,tile,5,"line");round_rect(r,x,row,tile,tile,5,"line");round_rect(r,x+2,row+2,tile-4,tile-4,4,"panel");
            if(!shader_preview.thumb(r,shader_preview_folder(),shader_preview_scene,shown,{x+4,row+4,tile-8,tile-8},budget)){
                ui_icon(r,SettingsIcon::Shader,x+tile/2,row+tile/2,44,"muted");
                if(!shader_preview.thumb_known(r,shader_preview_folder(),shader_preview_scene,shown))pending=true;
            }
            if(value==current && !selected){fill_round(r,x+tile-24,row+8,16,16,8,theme.at("panel"));fill_round(r,x+tile-22,row+10,12,12,6,theme.at("accent"));}
        }
        s.RenderSetClipRect(r,nullptr);shader_thumbs_pending=pending;
        if(!count)return;
        const int value=shader_choices[size_t(selection)];
        const std::string name=locale.text(shader_name(value));const int name_w=std::min(592,label_width(name,22,false));
        raw_label(r,name,320-name_w/2,y+330,22,"ink",592);
        const std::string note=locale.text(value<0?"Follows your library shader.":value==0?"Off keeps the original picture.":shaders.at(value).description);
        const int note_w=label_width(note,16,false);
        if(note_w<=592)raw_label(r,note,320-note_w/2,y+364,16,"muted",592);else wrapped(r,note,24,y+358,16,592,2,false);
        const bool held=pointer_down && !shader_drag;
        draw_rail(r,y,size_t(count),shader_carousel.position,{held && down_y>=409 && down_x<RailGeometry::left && contact_x<RailGeometry::left?1.0:0.0,held && down_y>=409 && down_x>=RailGeometry::left+RailGeometry::width && contact_x>=RailGeometry::left+RailGeometry::width?1.0:0.0,shader_drag && shader_carousel.range?1.0:0.0},false);
    }
    void choose_shader() {
        if(o.game_menu && menu_status["state"]=="busy"){dirty=true;return;}
        int selected=shader_choices.at(settings.focus);
        if(shader_per_game) {
            auto next=game_preferences;next.filter=selected;
            if(!apply_game_preferences(next,"Shader saved for next launch"))return;
        }else{preferences.filter=selected;remember_appearance();}
        if(o.game_menu){int effective=selected<0?preferences.filter:selected;if(!shader_supported(effective,shader_core))effective=0;menu_request("shader:"+std::to_string(effective));dirty=true;}else close_shader_picker();
    }
    Page artwork_return=Page::GameOptions;
    std::string artwork_match;
    std::map<std::string,std::string> artwork_result;
    void open_game_artwork(Page back=Page::GameOptions){
        if(model.games.empty())return;
        artwork_return=back;artwork_match=fields(item_folder(model.selected)/"artwork.ini")["match_title"];
        artwork_result=fields(item_folder(model.selected)/"artwork-status.ini");
        service_notice.clear();settings.page=Page::GameArtwork;settings.focus=0;dirty=true;
    }
    bool save_artwork_match(const std::string& title){
        try{
            auto path=item_folder(model.selected)/"artwork.ini";fs::create_directories(path.parent_path());
            auto pending=path.string()+".pending";std::ofstream file(pending);
            std::string clean=title;std::replace(clean.begin(),clean.end(),'\n',' ');std::replace(clean.begin(),clean.end(),'\r',' ');
            file<<"schema=1\nmatch_title="<<clean<<'\n';file.close();if(!file)throw std::runtime_error("Could not save matching title.");fs::rename(pending,path);artwork_match=clean;return true;
        }catch(const std::exception&){service_notice="Could not save matching title.";return false;}
    }
    void review_artwork(){
        std::vector<std::string> names;std::vector<size_t> targets;
        for(size_t i=0;i<model.games.size();i++){
            auto result=fields(item_folder(i)/"artwork-status.ini");std::error_code ec;
            if(!fs::exists(item_folder(i)/"box.bmp",ec) || result["state"]=="error" || result["state"]=="missing"){
                names.push_back(game_title(i));targets.push_back(i);
            }
        }
        if(names.empty()){service_notice=model.games.empty()?"No games in this collection.":"Every game in this collection has artwork.";dirty=true;return;}
        show_picker("Unmatched artwork",names,0,[this,targets](int n){select_game(targets.at(n),false);open_game_artwork(Page::Scraper);});
    }
    void game_artwork_action(){
        if(worker>0 && worker_action=="scrape"){
            if(settings.focus==1){std::ofstream(o.data/"scraper/cancel").put('\n');service_notice="Stopping after this request.";}
            else service_notice="Stop fetching before editing the match.";
            dirty=true;return;
        }
        if(settings.focus==0){begin_edit(3);return;}
        if(settings.focus==2){if(save_artwork_match(""))service_notice="Using the ROM's game code and filename.";dirty=true;return;}
        if(worker>0 && (worker_action=="index" || worker_action=="device.info" || worker_action=="status"))stop_worker();
        if(model.games[model.selected].demo){service_notice="Add your DS games to fetch their artwork.";dirty=true;return;}
        start_worker("scrape");
    }
    void open_scraper() {
        settings.section=2;settings.page=Page::Scraper;settings.focus=0;
        if(worker<=0)start_worker("status");service_loaded=true;dirty=true;
    }
    void begin_edit(int field) {
        if(field==4)search_return=settings.page;
        edit_field=field;edit_buffer=field==5?(ds_settings.count("nickname")?ds_settings["nickname"]:"RG DS Core"):field==4?search_query:field==0?username:field==1?password:field==3?artwork_match:"";key_row=1;key_col=1;key_layer=0;key_caps=false;edit_cursor=edit_buffer.size();
        settings.page=Page::Keyboard;s.StartTextInput();search_changed();dirty=true;
    }
    void finish_edit(bool commit) {
        if(edit_field==5){if(commit)save_ds_setting("nickname",edit_buffer.empty()?std::string("RG DS Core"):edit_buffer);edit_buffer.clear();s.StopTextInput();settings.page=Page::DSControls;settings.focus=4;dirty=true;return;}
        if(edit_field==4){
            search_preview.cancel();search_preview_waiting=search_preview_loading=false;search_matches.clear();
            if(commit){search_query=trimmed_query(edit_buffer);apply_discovery();}
            else{settings.page=search_return;settings.focus=0;}
            edit_buffer.clear();s.StopTextInput();dirty=true;return;
        }
        if(edit_field==3){
            if(commit && save_artwork_match(edit_buffer))service_notice="Matching title saved. Choose Fetch artwork.";
            edit_buffer.clear();s.StopTextInput();settings.page=Page::GameArtwork;settings.focus=0;dirty=true;return;
        }
        if(edit_field==2) {
            if(commit){std::ostringstream payload;payload<<"{\"network\":"<<std::quoted(network_id)<<",\"password\":"<<std::quoted(edit_buffer)<<'}';start_worker("device.connect",payload.str());}
            std::fill(edit_buffer.begin(),edit_buffer.end(),'\0');edit_buffer.clear();s.StopTextInput();settings.page=Page::Network;settings.focus=1;dirty=true;return;
        }
        if(commit){if(edit_field==0)username=edit_buffer;else password=edit_buffer;}
        std::fill(edit_buffer.begin(),edit_buffer.end(),'\0');edit_buffer.clear();s.StopTextInput();settings.page=Page::Scraper;dirty=true;
    }
    void move_edit_cursor(int direction){
        if(direction<0 && edit_cursor){edit_cursor--;while(edit_cursor && (static_cast<unsigned char>(edit_buffer[edit_cursor])&0xc0)==0x80)edit_cursor--;}
        else if(direction>0 && edit_cursor<edit_buffer.size()){edit_cursor++;while(edit_cursor<edit_buffer.size() && (static_cast<unsigned char>(edit_buffer[edit_cursor])&0xc0)==0x80)edit_cursor++;}
        dirty=true;
    }
    void insert_text(const std::string& value){
        if(edit_field==6 && RoundedFont::codepoints(edit_buffer+value).size()>16)return;
        if(edit_field==5){auto text=edit_buffer;text.insert(edit_cursor,value);auto points=RoundedFont::codepoints(text);size_t units=0;for(auto c:points)units+=c>0xFFFF?2:1;if(units>10)return;}
        if(edit_buffer.size()+value.size()<=256 && value.find_first_of("\r\n\t")==std::string::npos){edit_buffer.insert(edit_cursor,value);edit_cursor+=value.size();search_changed();}
        dirty=true;
    }
    void erase_character() {
        if(!edit_cursor)return;size_t end=edit_cursor;move_edit_cursor(-1);
        edit_buffer.erase(edit_cursor,end-edit_cursor);search_changed();dirty=true;
    }
    void delete_character(){
        if(edit_cursor>=edit_buffer.size())return;size_t begin=edit_cursor;move_edit_cursor(1);
        edit_buffer.erase(begin,edit_cursor-begin);edit_cursor=begin;search_changed();dirty=true;
    }
    void scraper_action() {
        if(settings.focus==2){review_artwork();return;}
        if(settings.focus==0){open_setting_picker();return;}
        if(worker>0 && (worker_action=="index" || worker_action=="device.info" || worker_action=="status"))stop_worker();
        if(worker>0){std::ofstream(o.data/"scraper/cancel").put('\n');service_notice="Stopping after this request";return;}
        start_worker("scrape");
    }
    bool brightness_drag=false,volume_drag=false;
    int unmuted_volume=25;
    Uint32 volume_toast=0;
    struct Input { Uint32 time;SDL_Event event; };
    std::vector<Input> inputs;
    size_t next_input=0;
    std::vector<Surface> surfaces;
    std::vector<SDL_Joystick*> joysticks;
    InputWake input_wake;
    // An idle menu sleeps until input arrives; only held controls, pending
    // hardware writes and timed UI keep a short cadence.
    int idle_wait_ms(Uint32 now){
        if(!input_wake.available())return touchscreen.available() || volume_keys.available()?8:50;
        if(hold.active() || pointer_down || carousel.dragging || volume_drag || brightness_drag || touch_tail)return 8;
        int wait=250;
        if(!library_ready || settings.page==Page::Microphone)wait=16;
        if(system.volume_pending() || system.brightness_pending() || volume_unsaved || brightness_unsaved)wait=std::min(wait,20);
        if(worker>0 || launching)wait=std::min(wait,50);
        if(artwork_busy() && preferences.animations)wait=std::min(wait,55);
        if(volume_toast)wait=std::min(wait,std::max(1,int(volume_toast-now)));
        if(hardware.charging)wait=std::min(wait,int(420-now%420)+1);
        return wait;
    }
    bool wait_event(SDL_Event* event,int timeout) {
        return wait_ui_event(s,event,timeout,!joysticks.empty() || touchscreen.available() || volume_keys.available(),&input_wake);
    }
    bool quit = false, probe, dirty = true, system_exit_ack = false;
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
    // The selected light/dark palette with the optional DS favourite colour.
    void select_theme(){theme=preferences.dark?dark_theme:light_theme;apply_accent(theme,preferences.accent,preferences.dark);if(preferences.high_contrast)apply_high_contrast(theme,preferences.dark);}
    void reload_themes() {
        auto light=fs::exists(o.themes/"light.ini")?load_theme(o.themes/"light.ini"):default_theme();
        auto dark=fs::exists(o.themes/"dark.ini")?load_theme(o.themes/"dark.ini",default_dark_theme()):default_dark_theme();
        if(!o.theme.empty()) {
            bool is_dark=dark_theme_file(o.theme);
            auto custom=load_theme(o.theme,is_dark?dark:light);
            if(is_dark)dark=std::move(custom);else light=std::move(custom);
        }
        light_theme=std::move(light);dark_theme=std::move(dark);select_theme();
    }
    void remember_appearance() {
        select_theme();
        try{save_preferences(o.preferences,preferences);status="Settings saved";}
        catch(const std::exception& e){
            status="Could not save all settings. Check storage.";std::cerr<<e.what()<<std::endl;
            // Base and additive UI preferences have separate atomic files.
            // A partial failure must display what actually reached storage.
            try{preferences=load_preferences(o.preferences);select_theme();}catch(const std::exception&){}
        }
        locale.load(asset_path("locales"),preferences.language);configure_sounds();dirty=true;
    }
    // Theme & display uses the shared settings list, four roomy rows per
    // screen: 0 colour mode, 1 accent colour, 2 high contrast, 3 typeface,
    // 4 icon style, 5 screen brightness, 6 animations, 7 touch indicator.
    // The two-choice rows show both options in place (tap a side, Left/Right,
    // or A to swap).
    using SegmentNames=std::array<const char*,2>;
    static constexpr SegmentNames mode_names{"Light","Dark"},typeface_names{"DS rounded","Pixel classic"};
    SDL_Rect segment_bounds(int index,int option){
        const auto& names=index==0?mode_names:typeface_names;
        int width=std::clamp(std::max(label_width(names[0],18),label_width(names[1],18))+34,100,150),left=574-2*width-8;
        return {left+4+option*width,list_top+index*list_stride-list_scroll+17,width,32};
    }
    void segment_row(SDL_Renderer* r,int y,int index,const std::string& title,bool second){
        int row=y+list_top+index*list_stride-list_scroll;bool focused=index==list_selected();
        int dx=24;move_element(uint32_t(index),dx,row,566,66,focused,list_top,list_top+list_height,20,595);dx-=24;
        settings_card(r,24+dx,row,566,66,focused);
        round_rect(r,38+dx,row+11,44,44,12,focused?"accent":"slot");
        ui_icon(r,index==0?(second?SettingsIcon::Moon:SettingsIcon::Sun):SettingsIcon::Type,60+dx,row+33,25,focused?"on_accent":"accent");
        const auto& names=index==0?mode_names:typeface_names;
        auto first=segment_bounds(index,0);first.x+=dx;first.y+=y;
        label(r,title,98+dx,row+19,24,"ink",first.x-16-(98+dx));
        sunken_track(r,first.x-4,first.y-4,first.w*2+8,first.h+8,13);
        for(int option=0;option<2;option++){
            SDL_Rect box{first.x+option*first.w,first.y,first.w,first.h};bool on=(option==1)==second;
            if(on)raised_pill(r,box.x,box.y,box.w,box.h,10,theme.at("accent"),0,false);
            int width=std::min(box.w-12,label_width(names[size_t(option)],18));
            label(r,names[size_t(option)],box.x+(box.w-width)/2,centred_label_y(locale.text(names[size_t(option)]),18,box.y+box.h/2),18,on?"on_accent":"ink",box.w-12);
        }
    }
    void appearance_row(SDL_Renderer* r,int y,int i){
        switch(i){
        case 0:segment_row(r,y,i,"Colour mode",preferences.dark);break;
        case 1:{Color swatch=theme.at("accent");list_row(r,y,i,"Accent colour",accent_name(preferences.accent),true,&swatch);break;}
        case 2:list_row(r,y,i,"High contrast",preferences.high_contrast?"On":"Off");break;
        case 3:segment_row(r,y,i,"Typeface",preferences.pixel);break;
        case 4:list_row(r,y,i,"Icon style",preferences.rich_icons?"Rich":"Simple pixel");break;
        case 5:list_row(r,y,i,"Screen brightness",std::to_string(system_preferences.brightness)+"%");break;
        // Animations opens its own page, so even "Off" is a value, not a switch.
        case 6:list_row(r,y,i,"Animations",!preferences.animations?"Off":preferences.view_animations?(preferences.view_style==ViewStyle::Push?"Push":preferences.view_style==ViewStyle::Zoom?"Zoom":"Spring"):"Subtle",true,nullptr,false);break;
        case 7:list_row(r,y,i,"Touch indicator",preferences.touch_indicator?"On":"Off");break;
        case 8:list_row(r,y,i,"Home layout",preferences.home_layout==1?"Column":"Carousel");break;
        case 9:list_row(r,y,i,"Background",preferences.background==1?"Grid":"Plain");break;
        default:list_row(r,y,i,"Touch gestures",preferences.gestures?"On":"Off");break;
        }
    }
    static std::string accent_name(int index){return index>=0 && index<int(ds_colours.size())?ds_colours[size_t(index)].name:"Classic";}
    void accent_swatch(SDL_Renderer* r,int cx,int cy,int radius,Color fill){
        for(int dy=-radius;dy<=radius;dy++){int half=int(std::sqrt(double(radius*radius-dy*dy)));tint_rect(r,cx-half,cy+dy,half*2+1,1,fill);}
        ring(r,cx,cy,radius,"line");
    }
    // Off, an open share that appears in Finder/Explorer like a NAS, or the
    // previous one-time password login. Both shares are advertised by name.
    void show_sharing_picker(){
        int current=device_status["sharing"]!="1"?0:device_status["sharing_mode"]=="open"?1:2;
        show_picker("Network file sharing",{"Off","Open on this Wi-Fi","Password protected"},current,[this,current](int n){
            if(n==current)return;
            start_worker(n==0?"device.sharing-off":n==1?"device.sharing-open":"device.sharing-on");
        });
    }
    // Theme & display > Accent colour: the DSi profile's 4x4 favourite-colour
    // grid. Tiles show the original DS colour and its name (never colour
    // alone); the interface uses the contrast-adjusted variant. Focus 0-15 are
    // tiles, 16 Classic blue, 17 High contrast.
    static constexpr int colour_grid_x=28,colour_grid_y=102,colour_tile_w=137,colour_tile_h=56,colour_gap=12,colour_row_pitch=66;
    void open_colours(){settings.page=Page::Colours;settings.focus=preferences.accent>=0?preferences.accent:16;page_at=s.GetTicks();dirty=true;}
    SDL_Rect colour_tile(int index)const{return {colour_grid_x+(index%4)*(colour_tile_w+colour_gap),colour_grid_y+(index/4)*colour_row_pitch,colour_tile_w,colour_tile_h};}
    SDL_Rect colour_option(int index)const{return {index==16?28:332,colour_grid_y+4*colour_row_pitch+4,280,48};}
    void choose_colour(int index){
        if(index==17){preferences.high_contrast=!preferences.high_contrast;emit_sound(preferences.high_contrast?MenuCue::Enable:MenuCue::Disable);}
        else{int next=index==16?-1:index;if(next==preferences.accent){dirty=true;return;}preferences.accent=next;emit_sound(MenuCue::Confirm);}
        remember_appearance();
    }
    void colours_action(int code){
        int& focus=settings.focus;
        if(code==3){settings.back();dirty=true;return;}
        if(code==2 || code==4){choose_colour(focus);dirty=true;return;}
        if(focus<16){
            if(code==0)focus=focus%4==0?focus+3:focus-1;
            else if(code==1)focus=focus%4==3?focus-3:focus+1;
            else if(code==8)focus=focus<4?(focus<2?16:17):focus-4;
            else if(code==9)focus=focus>=12?(focus%4<2?16:17):focus+4;
        }else{
            if(code==0 || code==1)focus=focus==16?17:16;
            else if(code==8)focus=focus==16?12:14;
            else if(code==9)focus=focus==16?0:2;
        }
        emit_sound(MenuCue::Navigate,code==0 || code==8?-1:1);dirty=true;
    }
    void colours_touch(int x,int y){
        for(int i=0;i<16;i++)if(point_in(colour_tile(i),x,y)){settings.focus=i;choose_colour(i);dirty=true;return;}
        for(int i:{16,17})if(point_in(colour_option(i),x,y)){settings.focus=i;choose_colour(i);dirty=true;return;}
    }
    // Pick whichever of white or near-black reads better on a raw DS colour.
    static Color tile_ink(Color fill){const Color white{255,255,255},ink{18,24,30};return contrast_ratio(white,fill)>=contrast_ratio(ink,fill)?white:ink;}
    void colours_screen(SDL_Renderer* r,int y){
        settings_header(r,y,"Accent colour");
        label(r,"FAVOURITE COLOUR",30,y+84,13,"muted");
        for(int i=0;i<16;i++){
            auto tile=colour_tile(i);tile.y+=y;bool focused=settings.focus==i,selected=preferences.accent==i;
            Color fill=ds_colours[size_t(i)].rgb;
            if(focused){round_rect(r,tile.x-5,tile.y-5,tile.w+10,tile.h+10,16,"ink");round_rect(r,tile.x-2,tile.y-2,tile.w+4,tile.h+4,13,"background");}
            round_rect(r,tile.x,tile.y+3,tile.w,tile.h,12,"slot");
            fill_round(r,tile.x,tile.y,tile.w,tile.h,12,fill);
            // The DSi's glossy colour squares: a soft light band on the top half.
            fill_round(r,tile.x+3,tile.y+3,tile.w-6,tile.h/2-3,10,mix_colour(fill,Color{255,255,255},.22));
            Color ink=tile_ink(fill);
            int width=label_width(ds_colours[size_t(i)].name,16);
            label_colour(r,ds_colours[size_t(i)].name,tile.x+(tile.w-width)/2,tile.y+tile.h/2-10,16,ink,tile.w-12);
            if(selected){int cx=tile.x+tile.w-15,cy=tile.y+15;fill_round(r,cx-10,cy-10,20,20,10,ink);check_mark(r,cx,cy,fill);}
        }
        for(int i:{16,17}){
            auto box=colour_option(i);box.y+=y;bool focused=settings.focus==i;
            settings_card(r,box.x,box.y,box.w,box.h,focused);
            if(i==16){
                Theme classic=preferences.dark?dark_theme:light_theme;if(preferences.high_contrast)apply_high_contrast(classic,preferences.dark);
                accent_swatch(r,box.x+28,box.y+24,11,classic.at("accent"));label(r,"Classic blue",box.x+50,box.y+12,20,"ink",170);
                if(preferences.accent<0)ui_icon(r,SettingsIcon::Check,box.x+box.w-26,box.y+24,20,"accent");
            }else{
                ui_icon(r,SettingsIcon::Sun,box.x+26,box.y+24,22,"accent");label(r,"High contrast",box.x+48,box.y+13,18,"ink",130);
                draw_toggle(r,box.x+box.w-56,box.y+10,"High contrast",preferences.high_contrast);
            }
        }
    }
    void colours_top(SDL_Renderer* r,int y){
        // Live preview of real interface parts in the current colours.
        const std::string name=settings.focus<16?ds_colours[size_t(settings.focus)].name:settings.focus==16?"Classic blue":"High contrast";
        settings_card(r,24,y+70,592,330);
        label(r,"PREVIEW",48,y+92,13,"muted");
        label(r,accent_name(preferences.accent),48,y+112,28,"accent",300);
        if(preferences.high_contrast)label(r,"High contrast",360,y+120,20,"ink",230);
        raised_pill(r,48,y+170,252,48,12,theme.at("accent"));label(r,"Play",72,y+182,22,"on_accent",200);
        raised_pill(r,316,y+170,276,48,12,theme.at("focus"));label(r,"Selected row",340,y+182,20,"ink",230);
        label(r,"Switches and values",48,y+243,20,"ink",300);label(r,"60%",420,y+243,20,"accent",60);draw_toggle(r,548,y+238,"Preview",true);
        round_rect(r,48,y+294,544,10,5,"slot");round_rect(r,48,y+294,360,10,5,"accent");
        label(r,name,48,y+330,18,"muted",300);label(r,"A  Apply",440,y+330,18,"muted",150);
    }
    // Left/Right (choice 0/1) set a row's value directly; A (choice 2)
    // swaps two-choice rows, flips switches and opens the rest.
    void choose_appearance(int choice) {
        const int row=settings.focus;const bool step=choice!=2;
        switch(row){
        case 0:preferences.dark=step?choice==1:!preferences.dark;break;
        case 1:
            if(!step){open_colours();return;}
            preferences.accent=std::clamp(preferences.accent+(choice==1?1:-1),-1,int(ds_colours.size())-1);break;
        case 2:preferences.high_contrast=step?choice==1:!preferences.high_contrast;emit_sound(preferences.high_contrast?MenuCue::Enable:MenuCue::Disable);break;
        case 3:{
            bool pixel=step?choice==1:!preferences.pixel;
            if(!pixel)check_fonts();
            if(!pixel && !font_ready){status="ROUNDED FONT UNAVAILABLE";dirty=true;return;}
            preferences.pixel=pixel;break;}
        case 4:
            if(!step){show_picker("Icon style",{"Simple pixel","Rich"},preferences.rich_icons?1:0,[this](int n){preferences.rich_icons=n==1;remember_appearance();});return;}
            preferences.rich_icons=choice==1;break;
        case 5:if(step){change_system(4,choice==0?-1:1);return;}settings.enter();dirty=true;return;
        case 6:settings.page=Page::Motion;settings.focus=0;dirty=true;return;
        case 7:preferences.touch_indicator=step?choice==1:!preferences.touch_indicator;break;
        case 8:
            if(!step){show_picker("Home layout",{"Carousel","Column"},preferences.home_layout,[this](int n){preferences.home_layout=n;home_layout_changed();remember_appearance();});return;}
            preferences.home_layout=choice;home_layout_changed();break;
        case 9:
            if(!step){show_picker("Background",{"Plain","Grid"},preferences.background,[this](int n){preferences.background=n;remember_appearance();});return;}
            preferences.background=choice;break;
        case 10:preferences.gestures=step?choice==1:!preferences.gestures;if(preferences.gestures)gesture_intro();break;
        default:return;
        }
        remember_appearance();
    }
    void refresh_system() {
        hardware=system.status();dirty=true;
        if(o.preview) {
            std::ifstream file(o.data/"volume.ini");std::string line;
            while(std::getline(file,line))if(line.rfind("volume=",0)==0)hardware.volume_percent=std::clamp(std::atoi(line.c_str()+7),0,100);
        }
        if(hardware.volume_percent>0)unmuted_volume=hardware.volume_percent;
        volume_applied=hardware.volume_percent;
    }
    void save_preview_volume(int value){if(!o.preview)return;fs::create_directories(o.data);auto path=o.data/"volume.ini",pending=fs::path(path.string()+".tmp");std::ofstream file(pending);file<<"schema=1\nvolume="<<value<<'\n';file.close();if(!file)throw std::runtime_error("Could not save volume");fs::rename(pending,path);}
    int confirmed_volume(){if(!o.preview){auto fields=rgds::fields(o.data/"volume.ini");if(fields.count("volume")){try{size_t end=0;int value=std::stoi(fields["volume"],&end);if(end==fields["volume"].size() && value>=0 && value<=100)return value;}catch(...){}}}return system.cached().volume_percent;}
    void poll_volume(){if(auto update=system.poll_volume()){
        if(update->success){volume_applied=update->value;return_audio_failed=false;}
        if(update->settled && update->value==hardware.volume_percent){
            if(!update->success){hardware.volume_percent=volume_applied=confirmed_volume();status="Could not set volume - see log";}
            try{save_preview_volume(volume_applied);}catch(const std::exception& error){status=error.what();}
            volume_unsaved=false;dirty=true;
        }
    }}
    void flush_volume(){if(!volume_unsaved && !system.volume_pending())return;bool ok=system.flush_volume();volume_applied=ok?system.cached().volume_percent:confirmed_volume();if(!ok){hardware.volume_percent=volume_applied;status="Could not set volume - see log";}try{save_preview_volume(volume_applied);}catch(const std::exception& error){status=error.what();}volume_unsaved=false;poll_volume();}
    void toggle_mute(){hold.clear();volume_drag=false;change_volume(hardware.volume_percent==0?unmuted_volume:0);}
    void change_volume(int value) {
        if(!hardware.volume){status="Volume control unavailable";dirty=true;return;}
        value=std::clamp(value,0,100);
        if(value==hardware.volume_percent && !return_audio_failed){volume_toast=s.GetTicks()+1800;last_activity=s.GetTicks();dirty=true;return;}
        if(return_writes_pending())return_volume=value;else system.request_volume(value);volume_unsaved=true;
        hardware.volume_percent=value;if(value>0)unmuted_volume=value;
        volume_toast=s.GetTicks()+1800;last_activity=s.GetTicks();dirty=true;
    }
    void apply_timezone() {setenv("TZ",system_preferences.timezone.c_str(),1);tzset();dirty=true;}
    void load_system_configuration(){
        if(system_configuration_loaded)return;
        system_configuration_loaded=true;
        try{
            if(fs::exists(o.system_preferences)){
                system_preferences=load_system_preferences(o.system_preferences);
                if(!probe && !o.game_menu){
                    if(hardware.power && system_preferences.power>0 && !system.set("power",system_preferences.power))status="Could not apply saved power mode";
                    if(hardware.brightness){if(system.set("brightness",system_preferences.brightness)){brightness_applied=system_preferences.brightness;hardware.brightness_percent=brightness_applied;}else{system_preferences.brightness=brightness_applied;status="Could not apply saved brightness";}}
                }
            }
        }catch(const std::exception& e){std::cerr<<"System preferences fallback: "<<e.what()<<std::endl;}
        if(!probe && !o.game_menu && hardware.lid && system_preferences.lid && !system.set("lid",1))status="Could not enable lid wake";
        apply_timezone();
    }
    void save_date_format(int value) {
        auto previous=system_preferences.date_format;system_preferences.date_format=value;
        try{save_system_preferences(o.system_preferences,system_preferences);dirty=true;}
        catch(const std::exception& e){system_preferences.date_format=previous;status="Could not save date format";std::cerr<<e.what()<<std::endl;dirty=true;}
    }
    void save_time_format(int value) {
        auto previous=system_preferences.time_format;system_preferences.time_format=value;
        try{save_system_preferences(o.system_preferences,system_preferences);dirty=true;}
        catch(const std::exception& e){system_preferences.time_format=previous;status="Could not save time format";std::cerr<<e.what()<<std::endl;dirty=true;}
    }
    void save_timezone() {
        auto previous=system_preferences.timezone;system_preferences.timezone=timezones.at(settings.focus);
        try{save_system_preferences(o.system_preferences,system_preferences);apply_timezone();}
        catch(const std::exception& e){system_preferences.timezone=previous;status="Could not save timezone";std::cerr<<e.what()<<std::endl;dirty=true;}
    }
    bool apply_control(int focus,const SystemPreferences& value) {
        if(return_writes_pending()){if(focus==0)return_power=value.power;else if(focus==2)return_lid=value.lid;else if(focus==4)return_brightness=value.brightness;return true;}
        if(focus==0)return system.set("power",value.power);
        if(focus==2)return system.set("lid",value.lid);
        if(focus==4)return system.set("brightness",value.brightness);
        return true;
    }
    void poll_brightness() {
        if(auto result=system.poll_brightness()){
            if(result->success){brightness_applied=result->value;hardware.brightness_percent=result->value;}
            else if(result->settled && result->value==system_preferences.brightness){system_preferences.brightness=brightness_applied;status="Could not apply brightness";dirty=true;}
        }
        if(brightness_unsaved && !return_writes_pending() && !system.brightness_pending() && !brightness_drag && s.GetTicks()-brightness_changed>=180){
            try{save_system_preferences(o.system_preferences,system_preferences);brightness_unsaved=false;}
            catch(const std::exception& e){status="Could not save brightness";brightness_unsaved=false;std::cerr<<e.what()<<std::endl;dirty=true;}
        }
    }
    void flush_brightness() {
        bool success=system.flush_brightness();
        if(auto result=system.poll_brightness()){if(result->success)brightness_applied=result->value;}
        if(!success){system_preferences.brightness=brightness_applied;status="Could not apply brightness";}
        if(brightness_unsaved){try{save_system_preferences(o.system_preferences,system_preferences);brightness_unsaved=false;}catch(const std::exception& e){std::cerr<<e.what()<<std::endl;}}
    }
    void change_system(int focus,int delta,bool cycle=false) {
        auto previous=system_preferences,next=previous;
        bool available=focus==0?hardware.power:focus==1?hardware.sleep:focus==2?hardware.lid:focus==3?hardware.battery>=0:hardware.brightness;
        if(!available){status="Control unavailable on this device";dirty=true;return;}
        if(focus==0)next.power=cycle?(next.power+1)%4:std::clamp(next.power+delta,0,3);
        else if(focus==1)next.idle=cycle?(next.idle+1)%5:std::clamp(next.idle+delta,0,4);
        else if(focus==2)next.lid=cycle?!next.lid:delta>0;
        else if(focus==3)next.battery_percent=cycle?!next.battery_percent:delta>0;
        else if(focus==4)next.brightness=cycle?(next.brightness>=100?5:std::min(100,(next.brightness/5+1)*5)):std::clamp((next.brightness/5+delta)*5,5,100);
        if(next.power==previous.power && next.idle==previous.idle && next.lid==previous.lid && next.brightness==previous.brightness && next.battery_percent==previous.battery_percent)return;
        if(focus==4){
            system_preferences.brightness=next.brightness;if(return_writes_pending())return_brightness=next.brightness;else system.request_brightness(next.brightness);
            brightness_changed=s.GetTicks();brightness_unsaved=true;dirty=true;return;
        }
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
    void reset_after_sleep() {
        top_seen.clear();
        // Discard pre-suspend SDL/raw presses, including a held browser gesture.
        SDL_Event pending{};while(s.PollEvent(&pending)){if(pending.type==SDL_QUIT)quit=true;}
        touchscreen.discard_until_release();cancel_navigation();brightness_drag=volume_drag=false;
        carousel.reset(navigation_selected());last_activity=s.GetTicks();dirty=true;
    }
    void restore_sleep_audio() {configure_sounds();}
    void sleep_now(bool lid=false) {
        if(!hardware.sleep){status="Sleep unavailable on this device";dirty=true;return;}
        flush_return_controls();
        flush_volume();
        flush_brightness();save_state();status=o.preview?"Sleep simulated - press any control":"Sleeping - use Power to wake";dirty=true;draw();
        sounds.stop();
        // The private shared PCM can survive suspend just like an engine's
        // PCM. Keep it warm: reopening the codec can swallow a short cue and
        // delays the wake sound. Exclusive/custom configurations still release it.
        const char* shared_audio=std::getenv("RGDS_SHARED_AUDIO");
        bool warm_audio=!o.preview && shared_audio && std::string(shared_audio)=="1" && sounds.available();
        bool close_played=false;
        if(warm_audio || o.preview)close_played=emit_sound(MenuCue::LidClose);
        if(o.preview && close_played){
            auto began=s.GetTicks();while(sounds.active() && s.GetTicks()-began<400)s.Delay(5);
        }
        if(!warm_audio)sounds.close();
        bool ok=system.sleep(lid,close_played);
        status=ok?(o.preview?"Sleep simulated - settings retained":"Welcome back"):"Could not sleep - see log";
        reset_after_sleep();
        // Stock resume hooks may reset the mixer even with the sink retained.
        if(warm_audio && !system.sync_volume())sounds.close();
        restore_sleep_audio();if(ok)emit_sound(MenuCue::Unlock);
    }
    void watch_system(Uint32 now) {
        if(!return_writes_pending() && now-system_poll>=10000 && now-system_retry>=500){system_retry=now;if(system.request_status())system_poll=now;}
        // The game runner owns lid sleep while playing, including its overlay.
        if(o.preview || probe || o.game_menu)return;
        if(now-lid_poll>=50) {
            lid_poll=now;auto closed=system.lid_closed();
            if(lid_sleep.poll(closed,system_preferences.lid && hardware.lid,system.lid_generation())){sleep_now(true);return;}
        }
        if(!pointer_down && system_preferences.idle>0 && hardware.sleep && now-last_activity>=static_cast<Uint32>(idle_minutes[system_preferences.idle])*60000)sleep_now();
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
        std::ifstream navigation(o.state);std::string navigation_line;bool saved_home=false;
        while(std::getline(navigation,navigation_line)){std::istringstream row(navigation_line);std::string marker;int category=1;if(row>>marker>>category && marker=="home" && category>=0 && category<int(home_count)){saved_home=true;home_menu=true;home_selected=size_t(category);}}
        std::ifstream file(o.state);
        unsigned schema=0,core=0;size_t selected=0;std::string path;
        if(!(file>>schema>>selected>>core>>std::quoted(path)) || schema!=1 || core>=core_count)return;
        if(!saved_home)home_menu=false;
        model.core=core;
        if(!model.games.empty()) {
            model.selected=std::min(selected,model.games.size()-1);
            for(size_t i=0;i<model.games.size();i++)
                if(!path.empty() && model.games[i].path==fs::path(path)){model.selected=i;break;}
        }
        int page=0,section=0,focus=0;
        if(file>>page>>section>>focus && page==int(Page::ShaderPicker) && focus>=0 && focus<=shader_count) {
            std::string marker;int per_game=0;
            if(file>>marker>>per_game && marker=="shader" && (per_game==0 || per_game==1) && (!per_game || !model.games.empty())) {
                if(per_game)game_preferences=library_cache.options(model.games[model.selected]);
                open_shader_picker(per_game);if(o.game_menu)shader_return=Page::QuickMenu;settings.focus=std::min(focus,int(shader_choices.size())-1);shader_carousel.reset(size_t(std::max(0,settings.focus)));
                int scene=0,mode=2;float split=.5f;if(file>>marker>>scene>>mode>>split && marker=="shader-preview" && scene>=0 && scene<ShaderPreview::scene_count && mode>=0 && mode<=2 && split>=.1f && split<=.9f){shader_preview_scene=scene;shader_preview_mode=ShaderPreviewMode(mode);shader_preview_split=split;}return;
            }
        }
        if(file && page==int(Page::Controls) && focus>=0 && focus<=18){std::string marker,tail;int per_game=0,parent=int(o.game_menu?Page::QuickMenu:Page::Settings),saved_parent=parent;file>>marker>>per_game;std::getline(file,tail);std::istringstream extra(tail);if(extra>>saved_parent && (saved_parent==int(Page::Settings) || saved_parent==int(Page::QuickMenu) || saved_parent==int(Page::GameOptions)))parent=saved_parent;open_controls(marker=="controls" && per_game==1,static_cast<Page>(parent));settings.focus=focus;return;}
        if(file && page>=0 && (page<=int(Page::GameOptions) || page==int(Page::Startup) || page==int(Page::Statistics) || page==int(Page::RomSetup) || page==int(Page::Discover) || page==int(Page::Motion) || page==int(Page::Colours) || page==int(Page::MenuSounds)) && page!=int(Page::Keyboard) && page!=int(Page::QuickMenu) && page!=int(Page::LeaveGame) && page!=int(Page::Confirm) && section>=0 && section<=8 && focus>=0 && focus<=(page==int(Page::Motion)?3:page==int(Page::Timezone)?int(timezones.size())-1:page==int(Page::Maintenance)?6:page==int(Page::LibraryOptions)?9:page==int(Page::Appearance)?10:page==int(Page::System)?7:page==int(Page::Wifi)?std::max(0,std::atoi(device_status["network_count"].c_str())-1):4))
            settings={static_cast<Page>(page),section,focus};
        if(settings.page==Page::Power){settings.section=1;}
        if(settings.page==Page::Placeholder && settings.section==1)settings.page=Page::Power;
        if(settings.page==Page::Placeholder && settings.section==0)settings.page=Page::Appearance;
        if(settings.page==Page::Placeholder && settings.section>=2)settings.page=settings.section==2?Page::LibraryOptions:settings.section==3?Page::Sound:settings.section==4?Page::Network:Page::System;
        if(settings.page==Page::Appearance || settings.page==Page::Display || settings.page==Page::Motion || settings.page==Page::Colours)settings.section=0;
        if(settings.page==Page::Display)settings.focus=0;
        if(settings.page==Page::Sound || settings.page==Page::MenuSounds){settings.section=3;settings.focus=0;}
        if(settings.page==Page::System || settings.page==Page::Timezone)settings.section=5;
        if(settings.page==Page::System)settings.focus=std::min(settings.focus,7);
        if(settings.page==Page::Startup){settings.section=5;settings.focus=std::min(settings.focus,3);start_worker("device.startup-info");}
        if(settings.page==Page::Network)settings.focus=std::min(settings.focus,4);
        if(settings.page==Page::Maintenance)settings.focus=std::min(settings.focus,6);
        if(settings.page==Page::Statistics)settings.section=7;
        if(settings.page==Page::GameOptions){if(model.games.empty())settings.page=Page::Library;else game_preferences=library_cache.options(model.games[model.selected]);settings.focus=std::min(settings.focus,5);}
    }
    void save_state() {
        if(o.state.empty() || !session_initialized || !library_ready || settings.page==Page::BootWarning)return;
        try {
            if(!o.state.parent_path().empty())fs::create_directories(o.state.parent_path());
            auto pending=o.state.string()+".tmp";
            std::ofstream file(pending);
            file<<1<<' '<<model.selected<<' '<<model.core<<' '
                <<std::quoted(model.games.empty()?std::string():model.games[model.selected].path.string())<<'\n';
            if(settings.page!=Page::Library && settings.page!=Page::Keyboard)file<<static_cast<int>(settings.page)<<' '<<settings.section<<' '<<settings.focus<<'\n';
            if(settings.page==Page::ShaderPicker)file<<"shader "<<shader_per_game<<'\n';
            if(settings.page==Page::ShaderPicker)file<<"shader-preview "<<shader_preview_scene<<' '<<int(shader_preview_mode)<<' '<<shader_preview_split<<'\n';
            if(settings.page==Page::Controls)file<<"controls "<<controls_per_game<<' '<<int(controls_return)<<'\n';
            if(home_menu && !o.game_menu)file<<"home "<<home_selected<<'\n';
            file.close();if(!file)throw std::runtime_error("Cannot save launcher state");
            fs::rename(pending,o.state);
        }catch(const std::exception& e){std::cerr<<"State: "<<e.what()<<std::endl;}
    }
    void color(SDL_Renderer* r, const std::string& key) {
        auto c = theme.at(key); s.SetRenderDrawColor(r,c.r,c.g,c.b,o.game_menu && key=="background"?56:255);
    }
    void rect(SDL_Renderer* r, int x,int y,int w,int h, const std::string& key, bool outline = false) {
        color(r,key); SDL_Rect box{x,y,w,h};
        if (outline) s.RenderDrawRect(r,&box); else s.RenderFillRect(r,&box);
    }
    Color blend(Color a,Color b,int amount){return {static_cast<unsigned char>((a.r*(100-amount)+b.r*amount)/100),static_cast<unsigned char>((a.g*(100-amount)+b.g*amount)/100),static_cast<unsigned char>((a.b*(100-amount)+b.b*amount)/100)};}
    void tint_rect(SDL_Renderer* r,int x,int y,int w,int h,Color value){s.SetRenderDrawColor(r,value.r,value.g,value.b,255);SDL_Rect box{x,y,w,h};s.RenderFillRect(r,&box);}
    void sheen(SDL_Renderer* r,int x,int y,int w,int h,const std::string& key,int strength=12) {
        auto base=theme.at(key);for(int i=0;i<12;i++){int top=y+h*i/12,bottom=y+h*(i+1)/12;auto shade=blend(base,i<6?theme.at("panel"):theme.at("background"),strength*std::abs(6-i)/6);tint_rect(r,x,top,w,bottom-top,shade);}
    }
    void horizontal_sheen(SDL_Renderer* r,int x,int y,int w,int h,const std::string& key,int strength=12) {
        auto base=theme.at(key),highlight=theme.at("panel");
        int span=std::max(1,w-1);
        for(int column=0;column<w;column++)tint_rect(r,x+column,y,1,h,blend(base,highlight,strength*(w-1-column)/span));
    }
    void triangle(SDL_Renderer* r,int x,int y,int half,int height,const std::string& key,bool down=true) {
        color(r,key);for(int i=0;i<height;i++){int width=half*(height-i)/height;int row=down?y+i:y+height-1-i;s.RenderDrawLine(r,x-width,row,x+width,row);}
    }
    void battery_icon(SDL_Renderer* r,int y) {
        const int x=576,top=y+14;
        round_rect(r,x,top,38,20,3,"muted");round_rect(r,x+2,top+2,34,16,2,"slot");rect(r,x+38,top+6,3,8,"muted");
        int bars=hardware.battery<0?0:hardware.battery>75?4:hardware.battery>45?3:hardware.battery>15?2:hardware.battery>0?1:0;
        drawn_charge_phase=hardware.charging?int((s.GetTicks()/420)%4):-1;
        if(hardware.charging)bars=1+drawn_charge_phase;
        auto level=hardware.battery>=0 && hardware.battery<=15 && !hardware.charging?Color{239,129,75}:theme.at("accent");
        for(int i=0;i<4;i++)tint_rect(r,x+5+i*7,top+5,5,10,i<bars?level:blend(theme.at("slot"),theme.at("muted"),16));
        if(hardware.charging){color(r,"accent");s.RenderDrawLine(r,x-10,top+1,x-16,top+10);s.RenderDrawLine(r,x-16,top+10,x-9,top+10);s.RenderDrawLine(r,x-9,top+10,x-15,top+19);}
        else if(hardware.external_power){color(r,"accent");s.RenderDrawLine(r,x-15,top+3,x-15,top+8);s.RenderDrawLine(r,x-10,top+3,x-10,top+8);s.RenderDrawLine(r,x-17,top+8,x-8,top+8);s.RenderDrawLine(r,x-17,top+8,x-17,top+12);s.RenderDrawLine(r,x-8,top+8,x-8,top+12);s.RenderDrawLine(r,x-17,top+12,x-8,top+12);s.RenderDrawLine(r,x-13,top+12,x-13,top+18);}
    }
    fs::path status_asset(const std::string& name){return asset_path(std::string("status/")+(preferences.dark?"dark-":"light-")+name+".bmp");}
    std::string wifi_asset(){
        if(device_status["wifi"]!="1")return "wifi-off";
        int signal=-1;if(!device_status["wifi_signal"].empty())signal=std::atoi(device_status["wifi_signal"].c_str());
        if(probed_signal>=0 && device_status["wifi_connected"]=="1")signal=probed_signal;
        int level=signal>=67?3:signal>=34?2:signal>0?1:0;return "wifi-"+std::to_string(level);
    }
    std::string sound_asset(){return hardware.volume && hardware.volume_percent==0?"muted":"sound";}
    // Covers are being fetched: a small spinner and the count beside the
    // brand, on every screen, until the artwork worker reports it is done.
    bool artwork_busy() const {auto found=service.find("state");return found!=service.end() && found->second=="scraping";}
    int loader_phase(Uint32 now) const {return preferences.animations?int((now/110)%8):0;}
    void artwork_loader(SDL_Renderer* r,int y){
        static const int dx[8]={0,5,7,5,0,-5,-7,-5},dy[8]={-7,-5,0,5,7,5,0,-5};
        const int head=drawn_loader_phase=loader_phase(s.GetTicks()),cx=232,cy=y+24;
        const Color ink=theme.at("accent");
        s.SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
        for(int i=0;i<8;i++){
            static const Uint8 tail[8]={255,200,150,105,70,50,40,40};
            s.SetRenderDrawColor(r,ink.r,ink.g,ink.b,tail[(head-i+8)%8]);
            SDL_Rect dot{cx+dx[i]-2,cy+dy[i]-2,4,4};s.RenderFillRect(r,&dot);
        }
        s.SetRenderDrawBlendMode(r,SDL_BLENDMODE_NONE);
        auto count=[&](const char* key){auto found=service.find(key);return found==service.end()?std::string("0"):found->second;};
        const std::string progress=count("completed")+"/"+count("total");
        label(r,progress,246,status_label_y(progress,16,y),16,"muted",110,false);
    }
    int drawn_loader_phase=-1;
    static int pixel_scale(int size){return std::max(1,(size+4)/10);}
    int label_width(const std::string& source,int size,bool translate=true){auto value=translate?locale.text(source):source;return preferences.pixel?int(RoundedFont::codepoints(value).size())*pixel_scale(size)*6:int(std::ceil(rounded.width(value,size)));}
    std::string status_stamp() {
        auto now=std::time(nullptr);char stamp[32];const auto& local=*std::localtime(&now);std::strftime(stamp,sizeof(stamp),date_format_patterns.at(system_preferences.date_format),&local);return std::string(stamp)+"  "+time_text(local);
    }
    // The label y that puts this text's ink (capitals and digits) on `cy`.
    int centred_label_y(const std::string& value,int size,int cy) {
        auto bounds=preferences.pixel?std::pair<int,int>{pixel_scale(size)*3,pixel_scale(size)*10}:rounded.ink_bounds(value,size);
        return cy-(bounds.first+bounds.second+1)/2;
    }
    int status_label_y(const std::string& value,int size,int y) {return centred_label_y(value,size,y+24);}
    void status_bar(SDL_Renderer* r,int y) {
        sheen(r,0,y,640,48,"background",24);
        if(!preferences.rich_icons){
            // The pixel style's own status symbols, at the same 24 px as its icons.
            draw_pixel_glyph(s,r,pixel_status_glyph(sound_asset()),24,y+24,24,theme.at("ink"));
            draw_pixel_glyph(s,r,pixel_status_glyph(wifi_asset()),58,y+24,24,theme.at("ink"));
        }else{
            media.draw(r,status_asset(sound_asset()),{10,y+10,28,28},false);
            media.draw(r,status_asset(wifi_asset()),{44,y+10,28,28},false);
        }
        // Rendered at its drawn size; its capitals match the clock's (14 px),
        // centred on the bar's text line (y + 24) like the labels.
        media.draw(r,asset_path(preferences.dark?"brand/status-dark.bmp":"brand/status-light.bmp"),{84,y+13,129,22},false);
        if(artwork_busy())artwork_loader(r,y);
        drawn_status_stamp=status_stamp();const auto& stamp=drawn_status_stamp;
        int right=hardware.battery>=0 && system_preferences.battery_percent?494:550;
        label(r,stamp,right-label_width(stamp,20),status_label_y(stamp,20,y),20,"ink",right-180);
        battery_icon(r,y);
        if(system_preferences.battery_percent && hardware.battery>=0){auto percent=std::to_string(hardware.battery)+"%";label(r,percent,554-label_width(percent,20),status_label_y(percent,20,y),20,"muted",65);}
        rect(r,0,y+47,640,1,"line");
    }
    int point_size(int size){return size==1?16:size==2?20:size==3?28:36;}
    void text(SDL_Renderer* r, std::string value, int x,int y,int size, const std::string& key, int max_chars = 80,bool translate=true) {
        if(translate)value=locale.text(value);
        if(!preferences.pixel && font_ready){rounded.draw(r,value,x,y,point_size(size),theme.at(key),max_chars,std::max(1,612-x));return;}
        y+=size*3; // Match the rounded font's visible top, rather than its line-box top.
        color(r,key); int n=0;
        for (unsigned c : RoundedFont::codepoints(value)) {
            if (++n > max_chars) break;
            unsigned accent=0;
            auto vietnamese=vietnamese_pixel_glyph(c);if(vietnamese.base)c=vietnamese.base;
            if(c>=0xe0 && c<=0xfe && c!=0xf7)c-=32;
            switch(c){
            case 0xc0:case 0xc1:case 0xc2:case 0xc3:case 0xc4:accent=c-0xc0+1;c='A';break;
            case 0xc8:case 0xc9:case 0xca:case 0xcb:accent=c==0xcb?5:c-0xc8+1;c='E';break;
            case 0xcc:case 0xcd:case 0xce:case 0xcf:accent=c==0xcf?5:c-0xcc+1;c='I';break;
            case 0xd2:case 0xd3:case 0xd4:case 0xd5:case 0xd6:accent=c-0xd2+1;c='O';break;
            case 0xd9:case 0xda:case 0xdb:case 0xdc:accent=c==0xdc?5:c-0xd9+1;c='U';break;
            case 0xd1:accent=4;c='N';break;case 0xc7:accent=6;c='C';break;
            case 0xdd:accent=2;c='Y';break;case 0xa1:c='!';break;case 0xbf:c='?';break;
            default:break;}
            if(accent){
                auto dot=[&](int col,int row){SDL_Rect p{x+col*size,y+row*size,size,size};s.RenderFillRect(r,&p);};
                if(accent==1){dot(1,-2);dot(2,-1);}else if(accent==2){dot(3,-2);dot(2,-1);}
                else if(accent==3){dot(2,-2);dot(1,-1);dot(3,-1);}else if(accent==4){dot(1,-2);dot(2,-2);dot(2,-1);dot(3,-1);}
                else if(accent==5){dot(1,-2);dot(3,-2);}else if(accent==6){dot(2,7);dot(1,8);}
            }
            if(vietnamese.base){
                auto dot=[&](int col,int row){SDL_Rect p{x+col*size,y+row*size,size,size};s.RenderFillRect(r,&p);};
                if(vietnamese.shape==1){dot(2,-2);dot(1,-1);dot(3,-1);}
                else if(vietnamese.shape==2){dot(1,-2);dot(2,-1);dot(3,-2);}
                else if(vietnamese.shape==3){dot(4,-1);dot(5,-2);}
                else if(vietnamese.shape==4){for(int col=0;col<4;col++)dot(col,3);}
                int lift=vietnamese.shape && vietnamese.shape!=4?-2:0;
                if(vietnamese.tone==1){dot(1,-2+lift);dot(2,-1+lift);}
                else if(vietnamese.tone==2){dot(3,-2+lift);dot(2,-1+lift);}
                else if(vietnamese.tone==3){dot(2,-3+lift);dot(3,-2+lift);dot(2,-1+lift);}
                else if(vietnamese.tone==4){dot(1,-2+lift);dot(2,-2+lift);dot(2,-1+lift);dot(3,-1+lift);}
                else if(vietnamese.tone==5)dot(2,8);
            }
            if(c==0xe6)c=0xc6;else if(c==0xf8)c=0xd8;else if(c==0xe5)c=0xc5;else if(c==0xe9)c='E';else if(c==0x2019)c='\'';else if(c==0x2026)c='.';else if(c==0x2013 || c==0x2014)c='-';
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
    void centered(SDL_Renderer* r, const std::string& source,int y,int size,const std::string& key,int limit=48) {
        auto value=locale.text(source);
        auto count = std::min(static_cast<int>(RoundedFont::codepoints(value).size()),limit);
        if(!preferences.pixel && font_ready){auto width=std::min(rounded.width(value,point_size(size),limit),584.0f);text(r,value,static_cast<int>(320-width/2),y,size,key,limit);return;}
        count=std::min(count,584/(size*6));limit=count;
        text(r,value,320-count*size*3,y,size,key,limit);
    }
    void label(SDL_Renderer* r,const std::string& source,int x,int y,int pixels,const std::string& key,int width=560,bool translate=true) {
        auto value=translate?locale.text(source):source;
        if(!preferences.pixel && font_ready){rounded.draw(r,value,x,y,pixels,theme.at(key),1000,width);return;}
        int scale=pixel_scale(pixels);
        text(r,value,x,y,scale,key,std::min(80,width/(scale*6)),false);
    }
    void raw_label(SDL_Renderer* r,const std::string& value,int x,int y,int pixels,const std::string& key,int width=560){label(r,value,x,y,pixels,key,width,false);}
    void raw_wrapped(SDL_Renderer* r,const std::string& value,int x,int y,int pixels,int width,int rows=3){wrapped(r,value,x,y,pixels,width,rows,false);}
    void centered_tile_label(SDL_Renderer* r,const std::string& value,int x,int y,int width) {
        int measured=label_width(value,20);
        label(r,value,x+(width-measured)/2,y,20,"on_accent",width);
    }
    std::string category_title(size_t index){static const std::array<std::string,home_count> titles={"Settings","Games","Favourites","Recents","PictoChat","Last played"};return titles.at(index);}
    void category_icon(SDL_Renderer* r,size_t index,int x,int y,int size){
        static const std::array<SettingsIcon,home_count> symbols={SettingsIcon::Sliders,SettingsIcon::Cartridge,SettingsIcon::Heart,SettingsIcon::Clock,SettingsIcon::Chat,SettingsIcon::Play};
        ui_icon(r,symbols.at(index),x+size/2,y+size/2,size,"accent");
    }

    void settings_footer(SDL_Renderer* r,int y) {
        if(settings.page==Page::ShaderPicker)return;
        rect(r,0,y+424,640,56,"background");rect(r,24,y+424,592,1,"line");
        const bool pressed=footer_pressed(true),a_pressed=footer_pressed(false);
        footer_button(r,12,y+432,160,"Back","B",false,pressed);
        if(settings.page==Page::Keyboard){
            label(r,edit_field==4?"X Delete / Start Search":"X Delete / Start Done",184,y+446,14,"muted",278);
            footer_button(r,468,y+432,160,edit_field==4?"Search":"Done","",true,a_pressed);return;
        }
        if(settings.page==Page::Statistics || settings.page==Page::Microphone)return;
        if(settings.page==Page::GameSwitcher)label(r,"X  Game menu",212,y+443,16,"muted",240);
        if(settings.page==Page::ShaderPicker)label(r,"X Scene / Y Compare",212,y+443,14,"muted",244);
        if((settings.page==Page::EmulatorOptions && !emulator_option_enabled(settings.focus)) || (settings.page==Page::GameOptions && settings.focus==2 && superdrastic_game()))return;
        footer_button(r,468,y+432,160,settings.page==Page::ShaderPicker?"Apply":settings.page==Page::Settings?"Open":settings.page==Page::DeviceInfo?"View":settings.page==Page::Sound?(settings.focus==2?"Open":settings.focus==1?"Test":hardware.volume_percent==0?"Unmute":"Mute"):settings.page==Page::Display?"Done":"Select","A",true,a_pressed);
    }
    static const char* icon_asset(SettingsIcon symbol){
        switch(symbol){case SettingsIcon::Play:return "play";case SettingsIcon::Save:return "save";case SettingsIcon::Load:return "load";case SettingsIcon::Shader:return "shaders";case SettingsIcon::Sun:return "brightness";case SettingsIcon::Restart:return "restart";case SettingsIcon::Home:return "home";case SettingsIcon::Art:return "artwork";case SettingsIcon::Library:return "library";case SettingsIcon::Sliders:return "settings";case SettingsIcon::Cartridge:return "games";case SettingsIcon::Heart:return "favorites";case SettingsIcon::Clock:return "recent";case SettingsIcon::Display:return "display";case SettingsIcon::Power:return "power";case SettingsIcon::Volume:return "sound";case SettingsIcon::Wifi:return "wifi";case SettingsIcon::System:return "system";case SettingsIcon::Controller:return "controls";case SettingsIcon::Shutdown:return "shutdown";case SettingsIcon::Chat:return "pictochat";default:return nullptr;}
    }
    void ui_icon(SDL_Renderer* r,SettingsIcon symbol,int x,int y,int size,const std::string& key){
        if(drawing_elements && view_transitions_enabled() && view_motion.active()){
            double pop=view_motion.accent(uint32_t(symbol),view_drawn_at);
            size=int(std::round(size*(1+.14*pop)));y-=int(std::round(3*pop));
        }
        if(!preferences.rich_icons && draw_pixel_icon(s,r,symbol,x,y,size,theme.at(key)))return;
        auto name=icon_asset(symbol);
        if(preferences.rich_icons && name && size>=20 && media.draw(r,asset_path(std::string("icons/")+name+".bmp"),{x-size/2,y-size/2,size,size},false))return;
        draw_settings_icon(s,r,symbol,x,y,size,theme.at(key));
    }
    void settings_card(SDL_Renderer* r,int x,int y,int w,int h,bool focused=false){
        round_rect(r,x,y+3,w,h,14,"slot");round_rect(r,x,y,w,h,14,focused?"accent":"panel");
        if(focused)round_rect(r,x+2,y+2,w-4,h-4,12,"focus");
    }
    void list_row(SDL_Renderer* r,int y,int index,const std::string& title,const std::string& value="",bool enabled=true,const Color* swatch=nullptr,bool switches=true) {
        int row=y+list_top+index*list_stride-list_scroll;bool focused=index==list_selected();
        int dx=24;move_element(uint32_t(index),dx,row,566,66,focused,list_top,list_top+list_height,20,595);dx-=24;
        settings_card(r,24+dx,row,566,66,focused);
        round_rect(r,38+dx,row+11,44,44,12,focused && enabled?"accent":"slot");ui_icon(r,settings.page==Page::SaveGallery?SettingsIcon::Save:settings.page==Page::ShaderPicker?SettingsIcon::Shader:settings_icon_for(title),60+dx,row+33,25,enabled?(focused?"on_accent":"accent"):"muted");
        const bool toggle=switches && (value=="On" || value=="Off");
        const int dot=swatch?24:0;
        const int value_width=value.empty()?0:toggle?82:std::min(224,label_width(value,18)+(enabled?42:20)+dot);
        const int text_width=464-(value_width?value_width+16:28);
        label(r,title,98+dx,row+19,24,enabled?"ink":"muted",text_width,settings.page!=Page::Wifi);
        if(toggle){
            draw_toggle(r,524+dx,row+19,title,value=="On",enabled);
        }else if(!value.empty()){
            int x=574-value_width+dx;round_rect(r,x,row+18,value_width,30,10,focused?"panel":"slot");if(swatch)accent_swatch(r,x+18,row+33,8,*swatch);label(r,value,x+10+dot,row+23,18,enabled?"accent":"muted",value_width-(enabled?34:20)-dot);if(enabled)ui_icon(r,SettingsIcon::Chevron,562+dx,row+33,12,"accent");
        }else if(enabled)ui_icon(r,SettingsIcon::Chevron,570+dx,row+33,14,focused?"accent":"muted");
    }
    void draw_toggle(SDL_Renderer* r,int x,int y,const std::string& name,bool value,bool enabled=true){
        auto key=std::make_pair(int(settings.page),name);auto now=s.GetTicks();auto found=toggle_motion.find(key);
        if(found==toggle_motion.end())found=toggle_motion.emplace(key,ToggleMotion{value,float(value),now-100}).first;
        auto& motion=found->second;
        auto position=[&](){float t=preferences.animations?std::min(1.f,(now-motion.at)/90.f):1.f;t=1-(1-t)*(1-t);return motion.from+(float(motion.value)-motion.from)*t;};
        if(motion.value!=value){motion.from=position();motion.value=value;motion.at=now;micro_until=now+100;}
        sunken_track(r,x,y,44,28,14);
        if(enabled && value)raised_pill(r,x+1,y+1,42,26,13,theme.at("accent"),0,false);
        const int knob=x+5+int(14*position());
        alpha_round(r,knob,y+7,18,18,9,shadow_colour(),preferences.dark?90:40);
        round_rect(r,knob,y+5,18,18,9,"panel");alpha_round(r,knob+4,y+7,10,5,2,Color{255,255,255},preferences.dark?40:150);
        label(r,value?"On":"Off",x-42,centred_label_y(locale.text(value?"On":"Off"),16,y+14),16,enabled?"ink":"muted",36);
    }
    void list_scrollbar(SDL_Renderer* r,int y) {
        if(list_max_scroll()<=0)return;
        int thumb=std::max(54,list_height*list_height/(list_count()*list_stride-16));
        draw_scrollbar(r,596,y+list_top,list_height,thumb,list_scroll*(list_height-thumb)/list_max_scroll(),scroll_drag,scroll_hover);
    }
    // The vertical rail after the horizontal one: a recessed groove with a
    // pip per page, a glass window under the thumb, and a glossy thumb with
    // an engraved grip that widens while it is held.
    // The vertical rail: a quiet slot with a recessed groove down its middle
    // and a glossy thumb with an engraved grip. Nothing changes on hover;
    // dragging only adds a soft accent glow, so the thumb never jumps.
    void draw_scrollbar(SDL_Renderer* r,int x,int y,int height,int thumb,int offset,bool dragging,bool hover,int width=36) {
        (void)hover;
        const int cx=x+width/2;
        round_rect(r,x,y,width,height,width/2,"slot");
        round_rect(r,cx-4,y+10,8,height-20,4,"line");round_rect(r,cx-3,y+11,6,height-22,3,"background");
        const int top=y+offset,tw=width-10,left=cx-tw/2;
        alpha_round(r,left,top+3,tw,thumb,tw/2,shadow_colour(),preferences.dark?70:30);
        if(dragging)alpha_round(r,left-3,top-3,tw+6,thumb+6,tw/2+3,theme.at("accent"),preferences.dark?70:50);
        glossy_pill(r,left,top,tw,thumb,tw/2,theme.at("accent"),dragging?.5:0);
        for(int line=-1;line<=1;line++){tint_rect(r,cx-6,top+thumb/2+line*6+1,12,2,blend(theme.at("accent"),Color{0,0,0},28));round_rect(r,cx-6,top+thumb/2+line*6,12,2,1,"on_accent");}
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
    // Explicit colours (DS colour tiles) reuse the themed drawing paths.
    void fill_round(SDL_Renderer* r,int x,int y,int w,int h,int radius,Color fill){theme["swatch"]=fill;round_rect(r,x,y,w,h,radius,"swatch");}
    void label_colour(SDL_Renderer* r,const std::string& value,int x,int y,int pixels,Color ink,int width){theme["swatch_ink"]=ink;label(r,value,x,y,pixels,"swatch_ink",width);}
    void raw_label_colour(SDL_Renderer* r,const std::string& value,int x,int y,int pixels,Color ink,int width){theme["swatch_ink"]=ink;raw_label(r,value,x,y,pixels,"swatch_ink",width);}
    void check_mark(SDL_Renderer* r,int cx,int cy,Color ink){
        s.SetRenderDrawColor(r,ink.r,ink.g,ink.b,255);
        for(int w=0;w<2;w++){s.RenderDrawLine(r,cx-5,cy+w,cx-1,cy+4+w);s.RenderDrawLine(r,cx-1,cy+4+w,cx+6,cy-4+w);}
    }
    void ring(SDL_Renderer* r,int x,int y,int radius,const std::string& key) {
        color(r,key);
        for(int i=0;i<48;i++){double a=i*6.2831853/48,b=(i+1)*6.2831853/48;s.RenderDrawLine(r,x+int(std::cos(a)*radius),y+int(std::sin(a)*radius),x+int(std::cos(b)*radius),y+int(std::sin(b)*radius));}
    }
    void icon(SDL_Renderer* r,int kind,int x,int y,const std::string& key) {
        color(r,key);
        if(kind==0){ring(r,x,y,11,key);ring(r,x-4,y-3,2,key);ring(r,x+4,y-3,2,key);ring(r,x,y+5,2,key);}
        else if(kind==1){ring(r,x,y+2,10,key);rect(r,x-3,y-12,6,15,"panel");color(r,key);s.RenderDrawLine(r,x,y-12,x,y+1);}
        else if(kind==2){rect(r,x-11,y-9,24,18,key,true);s.RenderDrawLine(r,x-4,y-9,x-4,y+9);s.RenderDrawLine(r,x+4,y-9,x+4,y+9);}
        else if(kind==3){rect(r,x-10,y-4,5,8,key);s.RenderDrawLine(r,x-5,y-4,x+1,y-9);s.RenderDrawLine(r,x+1,y-9,x+1,y+9);s.RenderDrawLine(r,x+1,y+9,x-5,y+4);s.RenderDrawLine(r,x+6,y-7,x+10,y);s.RenderDrawLine(r,x+10,y,x+6,y+7);}
        else if(kind==4){for(int i=0;i<3;i++){int d=5+i*5;s.RenderDrawLine(r,x-d,y-i*5,x,y-5-i*5);s.RenderDrawLine(r,x,y-5-i*5,x+d,y-i*5);}rect(r,x-1,y+5,3,3,key);}
        else {ring(r,x,y,10,key);s.RenderDrawLine(r,x,y-1,x,y+6);rect(r,x-1,y-6,2,2,key);}
    }
    void gear(SDL_Renderer* r,int x,int y) {
        ring(r,x,y,9,"accent");ring(r,x,y,3,"accent");color(r,"accent");
        for(int i=0;i<8;i++){double a=i*6.2831853/8;s.RenderDrawLine(r,x+int(std::cos(a)*9),y+int(std::sin(a)*9),x+int(std::cos(a)*13),y+int(std::sin(a)*13));}
    }
    void choose_browser_layout(){show_picker("Browser layout",{"Cartridges","Covers","List"},preferences.layout,[this](int n){preferences.layout=n;cancel_navigation();remember_appearance();});}
    void choose_library_view(){
        show_picker("Library view",{"All games","Favourites","Recents","Cartridges","Covers","List","Jump to letter","Search and filters","Clear search and filters"},preferences.view,[this](int n){
            if(n<3){library_view(n);remember_appearance();}
            else if(n<6){preferences.layout=n-3;cancel_navigation();remember_appearance();}
            else if(n==6)jump_to_letter();
            else if(n==7)open_discover();
            else clear_discovery();
        });
    }
    static SDL_Rect library_view_choice(int index){
        if(index<6)return {40+(index%3)*188,120+(index/3)*100,184,52};
        if(index<8)return {40+(index-6)*284,296,276,48};
        return {40,354,560,40};
    }
    void library_choice_icon(SDL_Renderer* r,int index,int x,int y,int size,const std::string& ink){
        if(index==5){for(int i=-1;i<=1;i++){round_rect(r,x-size/2,y+i*size/3-1,3,3,1,ink);round_rect(r,x-size/2+7,y+i*size/3-1,size-7,3,1,ink);}return;}
        if(index==7){search_icon(r,x,y,size,ink);return;}
        static constexpr std::array<SettingsIcon,9> icons={SettingsIcon::Cartridge,SettingsIcon::Heart,SettingsIcon::Clock,SettingsIcon::Cartridge,SettingsIcon::Art,SettingsIcon::Library,SettingsIcon::Type,SettingsIcon::Library,SettingsIcon::Refresh};
        ui_icon(r,icons.at(index),x,y,size,ink);
    }
    static std::string initial_letter(const std::string& title){
        auto first=title.find_first_not_of(" ");if(first==std::string::npos)return "#";unsigned char c=title[first];if(c>='a' && c<='z')c-=32;return c>='A' && c<='Z'?std::string(1,char(c)):"#";
    }
    // The displayed title when it is already known; otherwise the file name's.
    // Opening the sheet never reads metadata for every cartridge on the card.
    std::string jump_title(size_t index){
        const auto& game=model.games[index];if(game.demo)return game.title;
        if(const auto* entry=library_cache.peek(game);entry && entry->title_ready)return entry->title;
        return library_title(game.title);
    }
    void jump_to_letter(){
        std::map<std::string,size_t> first;for(size_t i=0;i<model.games.size();i++)first.emplace(initial_letter(jump_title(i)),i);
        std::vector<std::string> letters;std::vector<size_t> targets;
        int selected=0;auto current=model.games.empty()?"":initial_letter(jump_title(model.selected));
        for(const auto& entry:first){if(entry.first==current)selected=int(letters.size());letters.push_back(entry.first);targets.push_back(entry.second);}
        show_picker("Jump to letter",letters,selected,[this,targets](int n){if(n>=0 && n<int(targets.size()))select_game(targets[n],false);});
    }
    void show_picker(std::string title,std::vector<std::string> options,int selected,std::function<void(int)> apply){
        picker_title=std::move(title);picker_options=std::move(options);picker_selected=picker_focus=std::clamp(selected,0,std::max(0,int(picker_options.size())-1));
        picker_apply=std::move(apply);picker_core_offset=-1;picker_detail=false;picker_open=true;picker_scroll=0;picker_rail=picker_swipe=scroll_hover=false;reveal_picker_focus();picker_at=s.GetTicks();hold.clear();dirty=true;
    }
    void show_emulator_picker(bool per_game){
        std::vector<std::string> choices;if(per_game)choices.push_back("Use default ("+std::string(core_names[preferences.core])+")");
        std::vector<int> cores;for(int i=0;i<core_count;i++)if(core_offered(i)){cores.push_back(i);choices.push_back(std::string(core_names[i])+(i==4?" (recommended)":i==3?" (experimental)":""));}
        const int current=per_game?game_preferences.core:int(preferences.core);
        const int row=int(std::find(cores.begin(),cores.end(),current)-cores.begin());
        show_picker(per_game?"Game emulator":"Default emulator",choices,per_game?(current<0?0:row+1):row,[this,per_game,cores](int n){
            const int core=per_game?(n==0?-1:cores.at(n-1)):cores.at(n);
            if(per_game)choose_game_emulator(core);
            else if(int(preferences.core)!=core){preferences.core=unsigned(core);remember_appearance();}
        });picker_core_offset=per_game?1:0;picker_cores=cores;
    }
    const char* drastic_backend_name(int value)const{return value<0?"Default":value==1?"SuperDrastic":"Standard";}
    void show_drastic_backend_picker(bool per_game){
        std::vector<std::string> choices;if(per_game)choices.push_back("Use default ("+std::string(drastic_backend_name(preferences.drastic_backend))+")");
        choices.push_back("Standard");choices.push_back("SuperDrastic (experimental)");
        show_picker("DraStic backend",choices,per_game?game_preferences.drastic_backend+1:preferences.drastic_backend,[this,per_game](int n){
            if(per_game){if(game_preferences.drastic_backend==n-1)return;auto next=game_preferences;next.drastic_backend=n-1;apply_game_preferences(next,"Saved. Applies on your next DraStic launch.");}
            else if(preferences.drastic_backend!=n){preferences.drastic_backend=n;remember_appearance();}
        });
    }
    void show_drastic_screen_picker(){
        show_picker("Standard screen order",{"Normal","Swapped"},game_preferences.drastic_screen_swap==1,[this](int n){
            if(n==(game_preferences.drastic_screen_swap==1))return;
            auto next=game_preferences;next.drastic_screen_swap=n;
            apply_game_preferences(next,o.game_menu?"Saved. Save & restart applies screen order.":"Saved for the next Standard DraStic launch.");
        });
    }
    std::string emulator_choice_hint()const{
        const int row=picker_focus-picker_core_offset;
        if(row<0 || row>=int(picker_cores.size()))return "Follows your Library default. Each emulator keeps its own saves.";
        const int core=picker_cores[row];
        static const std::array<const char*,core_count> hints={
            "DraStic: an alternative for games that need it. Saves stay separate.",
            "DSperate: the original upstream engine. DSCore adds fixes and speed.",
            "NNDDSS: a compatibility option. Try DSperate first for speed.",
            "melonDS: experimental. RG DS game testing is still pending.",
            "DSCore: recommended. Fastest, with lid, stylus and 3D fixes and optional 2x–5x 3D."};
        return hints.at(core);
    }
    bool open_setting_picker(){
        if((settings.page==Page::GameOptions && settings.focus>=list_count()-2) || (settings.page==Page::EmulatorOptions && settings.focus>=list_count()-2))return false;
        int row=settings.focus;
        if(settings.page==Page::System && row==0){std::vector<std::string> names;int selected=0;for(auto& item:locale.languages()){if(item.first==preferences.language)selected=int(names.size());names.push_back(item.second);}show_picker("Language",names,selected,[this](int n){preferences.language=locale.languages().at(n).first;locale.load(asset_path("locales"),preferences.language);remember_appearance();});return true;}
        if(settings.page==Page::System && row==2){show_picker("Date format",{date_format_names[0],date_format_names[1],date_format_names[2],date_format_names[3],date_format_names[4]},system_preferences.date_format,[this](int n){save_date_format(n);});return true;}
        if(settings.page==Page::System && row==3){show_picker("Time format",{time_format_names[0],time_format_names[1]},system_preferences.time_format,[this](int n){save_time_format(n);});return true;}
        if(settings.page==Page::EmulatorOptions && !emulator_option_enabled(row))return false;
        if(settings.page==Page::LibraryOptions && row==0){
            show_emulator_picker(false);
        }else if(settings.page==Page::LibraryOptions && row==5){choose_browser_layout();
        }else if(settings.page==Page::LibraryOptions && row==6){show_drastic_backend_picker(false);
        }else if(settings.page==Page::GameOptions && row==5 && experimental_game()){
            show_experimental_resolution_picker();
        }else if((settings.page==Page::GameOptions && row==5) || (settings.page==Page::EmulatorOptions && row==8)){show_drastic_backend_picker(true);
        }else if((settings.page==Page::GameOptions && row==6) || (settings.page==Page::EmulatorOptions && row==9)){
            if(settings.page==Page::GameOptions && superdrastic_game())return true;
            show_drastic_screen_picker();
        }else if(settings.page==Page::GameOptions && row==7 && superdrastic_game()){
            show_superdrastic_resolution_picker();
        }else if(settings.page==Page::Scraper && row==0){
            if(worker>0 && worker_action=="scrape"){service_notice="Stop scraping before changing source";return true;}
            show_picker("Artwork source",{"Automatic","GameTDB","Libretro"},preferences.artwork_provider,[this](int n){stop_worker();preferences.artwork_provider=n;remember_appearance();start_worker("status");});
        }else if(settings.page==Page::GameOptions && row==1){
            show_emulator_picker(true);
        }else if((settings.page==Page::Power && row==0) || (settings.page==Page::EmulatorOptions && row==2)){
            if(!hardware.power)return false;
            show_picker("Power mode",{power_names[0],power_names[1],power_names[2],power_names[3]},system_preferences.power,[this](int n){change_system(0,n-system_preferences.power);});
        }else if(settings.page==Page::Power && row==1){
            if(!hardware.sleep)return false;
            show_picker("Menu idle sleep",{idle_name(0),idle_name(1),idle_name(2),idle_name(3),idle_name(4)},system_preferences.idle,[this](int n){change_system(1,n-system_preferences.idle);});
        }else if(settings.page==Page::Startup && row==0){
            if(o.independent)return false;
            show_picker("Boot into",{"Anbernic","RG DS Core"},device_status["boot_mode"]=="rgds",[this](int n){change_startup(n?1:-1,false);});
        }else if(settings.page==Page::Controls && !control_capture && row==0 && !model.games.empty()){
            show_picker("Profile",{"Global","This game"},controls_per_game,[this](int n){controls_per_game=n;load_controls_editor();controls_notice.clear();});
        }else if(settings.page==Page::Controls && !control_capture && row>=1 && row<=3){
            if(row==1){int selected=edited_controls.preset=="ab-swap"?1:edited_controls.preset=="left-handed"?2:0;
                show_picker("Button layout",{"Default","A / B swapped","Left-handed"},selected,[this](int n){auto previous=edited_controls;bool inherit=controls_inherit;const char* values[]={"default","ab-swap","left-handed"};edited_controls.apply({{"preset",values[n]}});controls_inherit=false;if(!save_controls_editor()){edited_controls=previous;controls_inherit=inherit;}});
            }else if(row==2){int selected=edited_controls.shortcut=="anbernic"?1:edited_controls.shortcut=="startthenmenu"?2:0;
                show_picker("Home shortcut",{"Start + Anbernic","Anbernic","Start, then Anbernic"},selected,[this](int n){auto previous=edited_controls;bool inherit=controls_inherit;const char* values[]={"start+menu","anbernic","startthenmenu"};edited_controls.shortcut=values[n];controls_inherit=false;if(!save_controls_editor()){edited_controls=previous;controls_inherit=inherit;}});
            }else show_picker("Shortcut action",{"Open menu","Return home"},edited_controls.shortcut_action=="home",[this](int n){auto previous=edited_controls;bool inherit=controls_inherit;edited_controls.shortcut_action=n?"home":"menu";controls_inherit=false;if(!save_controls_editor()){edited_controls=previous;controls_inherit=inherit;}});
        }else if(settings.page==Page::EmulatorOptions && row==6){
            show_emulator_picker(true);
        }else if(settings.page==Page::EmulatorOptions && row==5){
            show_picker("Frameskip",{"Off","1 frame","2 frames","3 frames"},std::max(0,game_preferences.frameskip),[this](int n){change_emulator_option(n-std::max(0,game_preferences.frameskip));});
        }else if(settings.page==Page::EmulatorOptions && row==4){
            if(experimental_game()){show_experimental_resolution_picker();return true;}
            if(superdrastic_game()){show_superdrastic_resolution_picker();return true;}
            show_picker("3D resolution",{"Native","2x"},game_preferences.hires==1,[this](int n){if(n!=(game_preferences.hires==1))change_emulator_option(1);});
        }else return false;
        return true;
    }
    std::string device_info_name(int row)const{
        static const std::array<const char*,5> names={"RGDS build","Stock firmware","Kernel","Free space","Device address"};return names.at(row);
    }
    std::string device_info_value(int row){
        static const std::array<const char*,5> keys={"build","firmware","kernel","free_gb","ip"};
        auto value=device_status[keys.at(row)];return value.empty()?"Not available":value+(row==3?" GB":"");
    }
    void open_device_detail(){show_detail(device_info_name(settings.focus),device_info_value(settings.focus));}
    void show_detail(const std::string& title,const std::string& value){
        picker_core_offset=-1;
        picker_title=title;std::string line;
        detail_lines.clear();detail_scroll=0;
        // Break even unspaced build/kernel identifiers, without cutting UTF-8.
        for(size_t at=0;at<value.size();){
            size_t end=at+1;while(end<value.size() && (static_cast<unsigned char>(value[end])&0xc0)==0x80)end++;
            auto letter=value.substr(at,end-at);at=end;
            if(letter=="\n"){detail_lines.push_back(line);line.clear();continue;}
            if(letter=="\r")continue;if(letter=="\t")letter=" ";
            if(!line.empty() && label_width(line+letter,20)>516){
                auto space=line.find_last_of(' ');
                if(space!=std::string::npos){detail_lines.push_back(line.substr(0,space));line.erase(0,space+1);}
                else{detail_lines.push_back(line);line.clear();}
            }
            line+=letter;
        }
        if(!line.empty())detail_lines.push_back(line);
        picker_options.clear();picker_apply={};picker_detail=picker_open=true;picker_at=s.GetTicks();hold.clear();dirty=true;
    }
    int detail_max_scroll()const{return std::max(0,int(detail_lines.size())-8);}
    void scroll_detail_rail(int y){detail_scroll=std::clamp((y-164)*detail_max_scroll()/224,0,detail_max_scroll());dirty=true;}
    int picker_visible()const{return std::min(8,int(picker_options.size()));}
    int picker_stride()const{return std::min(52,322/std::max(1,picker_visible()));}
    bool picker_scrollable()const{return !picker_detail && picker_title=="Language";}
    int picker_height()const{return picker_visible()*picker_stride();}
    int picker_max_scroll()const{return std::max(0,int(picker_options.size())*picker_stride()-picker_height());}
    int picker_thumb()const{return std::max(54,picker_height()*picker_height()/std::max(1,int(picker_options.size())*picker_stride()));}
    int picker_thumb_offset()const{return picker_max_scroll()?picker_scroll*(picker_height()-picker_thumb())/picker_max_scroll():0;}
    void reveal_picker_focus(){
        if(!picker_scrollable())return;
        int row=picker_focus*picker_stride();
        if(row<picker_scroll)picker_scroll=row;
        if(row+picker_stride()>picker_scroll+picker_height())picker_scroll=row+picker_stride()-picker_height();
        picker_scroll=std::clamp(picker_scroll,0,picker_max_scroll());
    }
    void scroll_picker(int position){
        picker_scroll=std::clamp(position,0,picker_max_scroll());
        int first=(picker_scroll+picker_stride()-1)/picker_stride();
        int last=(picker_scroll+picker_height())/picker_stride()-1;
        picker_focus=std::clamp(picker_focus,first,std::min(last,int(picker_options.size())-1));dirty=true;
    }
    void seek_picker(int y){
        scroll_picker(int(std::lround((y-picker_top()-64-picker_grab)*double(picker_max_scroll())/std::max(1,picker_height()-picker_thumb()))));
    }
    int picker_first()const{if(picker_scrollable())return picker_scroll/picker_stride();return std::clamp(picker_focus-picker_visible()+1,0,std::max(0,int(picker_options.size())-picker_visible()));}
    bool picker_paged()const{return !picker_detail && !picker_scrollable() && picker_title!="Library view" && picker_title!="Jump to letter" && int(picker_options.size())>picker_visible();}
    int picker_top()const{return (picker_title=="Jump to letter" || picker_title=="Library view")?32:picker_detail?96:410-picker_visible()*picker_stride()-64;}
    void picker_action(int code){
        motion_input(code);
        if(picker_detail){
            if(code==2 || code==3 || code==4 || code==7){picker_open=false;detail_drag=detail_rail=false;}
            else if(code==0 || code==8)detail_scroll=std::max(0,detail_scroll-1);
            else if(code==1 || code==9)detail_scroll=std::min(detail_max_scroll(),detail_scroll+1);
            dirty=true;return;
        }
        if(picker_title=="Library view"){
            if(code==2 || code==4){
                if(picker_focus<6){if(picker_apply)picker_apply(picker_focus);dirty=true;return;}
            }else if(code==8 || code==9){
                static constexpr std::array<int,9> above={0,1,2,0,1,2,3,5,6},below={3,4,5,6,6,7,8,8,8};
                picker_focus=(code==8?above:below)[picker_focus];
                dirty=true;return;
            }
        }
        if(picker_title=="Jump to letter"){
            if(code==12 || (picker_options.empty() && (code==2 || code==4))){quick_search();return;}
            if(code==11){picker_open=false;picker_apply={};open_discover();return;}
            if(picker_options.empty() && code!=3 && code!=7)return;
        }
        if(picker_title=="Jump to letter" && (code==8 || code==9)){picker_focus=std::clamp(picker_focus+(code==8?-7:7),0,int(picker_options.size())-1);dirty=true;return;}
        if(code==3 || code==7){picker_open=false;picker_apply={};}
        else if(code==0 || code==8)picker_focus=std::max(0,picker_focus-1);
        else if(code==1 || code==9)picker_focus=std::min(int(picker_options.size())-1,picker_focus+1);
        else if(code==2 || code==4){auto apply=std::move(picker_apply);int selected=picker_focus;picker_open=false;if(apply)apply(selected);}
        if(code==0 || code==1 || code==8 || code==9)reveal_picker_focus();
        dirty=true;
    }
    void picker_touch(int x,int y){
        if(y>=424){if(x<200)picker_action(3);else if(x>=440)picker_action(picker_title=="Library view"?3:2);else if(picker_title=="Jump to letter")picker_action(12);return;}
        if(y<picker_top()){picker_action(3);return;}
        if(picker_detail)return;
        if(picker_paged() && y>=picker_top()+16 && y<picker_top()+56){
            if(x>=480 && x<528)picker_focus=std::max(0,picker_first()-1);
            else if(x>=544 && x<592)picker_focus=std::min(int(picker_options.size())-1,picker_first()+2*picker_visible()-1);
            dirty=true;return;
        }
        if(picker_title=="Library view"){
            for(int i=0;i<int(picker_options.size());i++)if(point_in(library_view_choice(i),x,y)){picker_focus=i;picker_action(2);break;}
            return;
        }
        if(picker_title=="Jump to letter"){
            if(point_in({40,96,364,52},x,y)){picker_action(12);return;}
            if(point_in({416,96,184,52},x,y)){picker_action(11);return;}
            if(x>=40 && x<600 && y>=164 && y<404){int index=(y-164)/60*7+(x-40)/80;if(index<int(picker_options.size()) && (x-40)%80<72 && (y-164)%60<52){picker_focus=index;picker_action(2);}}return;
        }
        if(picker_scrollable()){
            int local=y-picker_top()-64;
            if(picker_max_scroll()>0 && x>=572 && x<604 && local>=0 && local<picker_height()){
                int offset=picker_thumb_offset();picker_grab=local>=offset && local<offset+picker_thumb()?local-offset:picker_thumb()/2;
                seek_picker(y);return;
            }
            int at=local+picker_scroll,row=at/picker_stride();
            if(x>=40 && x<560 && local>=0 && local<picker_height() && at%picker_stride()<picker_stride()-6 && row<int(picker_options.size())){picker_focus=row;picker_action(2);}return;
        }
        int row=(y-picker_top()-64)/picker_stride()+picker_first();
        if(x>=40 && x<=600 && y>=picker_top()+64 && row>=picker_first() && row<picker_first()+picker_visible() && row<int(picker_options.size())){picker_focus=row;picker_action(2);}
    }
    void render_picker(SDL_Renderer* r,int y){render_picker_body(r,y);s.RenderSetClipRect(r,nullptr);}
    void render_picker_body(SDL_Renderer* r,int y){
        const bool closing=!picker_open;
        if(drawing_elements && !closing)motion_hits.clear(); // The modal owns input, including its empty spaces.
        // A sheet: it rises in (160 ms) and slides away when closed (140 ms).
        // Input and persistence never wait for either.
        const Uint32 now=s.GetTicks();
        float progress=!preferences.animations?1.f:closing?1.f-std::min(1.f,(now-picker_closed_at)/140.f):std::min(1.f,(now-picker_at)/160.f);
        const float ease=1-(1-progress)*(1-progress)*(1-progress);
        int top=y+picker_top()+int(std::lround((closing?420:28)*(1-ease)));
        s.SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);s.SetRenderDrawColor(r,0,0,0,Uint8(std::lround(92*ease)));SDL_Rect shade{0,y,640,424};s.RenderFillRect(r,&shade);s.SetRenderDrawBlendMode(r,SDL_BLENDMODE_NONE);
        if(closing){SDL_Rect clip{0,y,640,424};s.RenderSetClipRect(r,&clip);}
        settings_card(r,24,top,592,414-(top-y));
        round_rect(r,294,top+8,52,4,2,"line");
        bool library_modal=picker_title=="Library view" || picker_title=="Browser layout" || picker_title=="Jump to letter";
        if(library_modal)library_choice_icon(r,picker_title=="Jump to letter"?6:0,62,top+38,28,"accent");
        label(r,picker_title,library_modal?88:48,top+24,24,"ink",picker_paged()?416:library_modal?500:540);
        if(picker_paged()){
            const bool before=picker_first()>0,after=picker_first()+picker_visible()<int(picker_options.size());
            secondary_button(r,480,top+16,48,40,10,false,before);
            secondary_button(r,544,top+16,48,40,10,false,after);
            triangle(r,504,top+30,8,12,before?"accent":"line",false);
            triangle(r,568,top+30,8,12,after?"accent":"line");
        }
        if(picker_detail){
            for(int i=detail_scroll;i<std::min(int(detail_lines.size()),detail_scroll+8);i++)label(r,detail_lines[i],48,top+68+(i-detail_scroll)*28,20,"ink",516);
            if(detail_max_scroll()>0){
                int thumb=std::max(36,224*8/int(detail_lines.size()));
                draw_scrollbar(r,582,top+68,224,thumb,detail_scroll*(224-thumb)/detail_max_scroll(),detail_rail,false,30);
            }
            rect(r,0,y+424,640,56,"background");rect(r,24,y+424,592,1,"line");
            footer_button(r,12,y+432,160,"Close","B",false,footer_pressed(true));footer_button(r,468,y+432,160,"Done","A",true,footer_pressed(false));return;
        }
        if(picker_title=="Library view"){
            int offset=top-32;
            label(r,"Collection",48,offset+94,16,"muted",520);
            label(r,"Browser layout",48,offset+194,16,"muted",520);
            for(int i=0;i<int(picker_options.size());i++){
                auto box=library_view_choice(i);box.y+=offset;
                bool selected=i<3?i==preferences.view:i<6?i-3==preferences.layout:false;
                bool focused=i==picker_focus;
                move_element(uint32_t(i+100),box.x,box.y,box.w,box.h,focused,0,424);
                // A tile: tinted and outlined in the accent when it is the current choice,
                // with a filled check; the focused one wears an accent ring so it reads on both palettes.
                alpha_round(r,box.x,box.y+3,box.w,box.h,10,shadow_colour(),preferences.dark?70:22);
                if(focused)alpha_round(r,box.x-3,box.y-3,box.w+6,box.h+6,13,theme.at("accent"),preferences.dark?120:70);
                round_rect(r,box.x,box.y,box.w,box.h,10,selected || focused?"accent":"line");
                if(selected)fill_round(r,box.x+2,box.y+2,box.w-4,box.h-4,8,mix_colour(theme.at("panel"),theme.at("accent"),preferences.dark?.32:.14));
                else glossy_pill(r,box.x+2,box.y+2,box.w-4,box.h-4,8,theme.at("panel"));
                const int chip=std::min(34,box.h-14);fill_round(r,box.x+8,box.y+(box.h-chip)/2,chip,chip,9,selected?theme.at("accent"):theme.at("slot"));
                library_choice_icon(r,i,box.x+8+chip/2,box.y+box.h/2,chip-12,selected?"on_accent":"accent");
                int width=box.w-chip-(selected?56:30),font=label_width(picker_options[i],18)>width?15:18;
                label(r,picker_options[i],box.x+chip+18,centred_label_y(locale.text(picker_options[i]),font,box.y+box.h/2),font,"ink",width);
                if(selected){fill_round(r,box.x+box.w-30,box.y+box.h/2-10,20,20,10,theme.at("accent"));check_mark(r,box.x+box.w-20,box.y+box.h/2,theme.at("on_accent"));}
            }
            rect(r,0,y+424,640,56,"background");rect(r,24,y+424,592,1,"line");
            footer_button(r,12,y+432,160,"Close","B",false,footer_pressed(true));
            label(r,"A  Select",220,y+445,16,"muted",220);
            footer_button(r,468,y+432,160,"Done","",true,footer_pressed(false));return;
        }
        if(picker_title=="Jump to letter"){
            raised_pill(r,40,y+96,364,52,10,theme.at("accent"));search_icon(r,65,y+121,22,"on_accent");
            label(r,"Y  Quick search",90,y+111,22,"on_accent",296);
            secondary_button(r,416,y+96,184,52,10,false,true);ui_icon(r,SettingsIcon::Sliders,439,y+122,24,"accent");label(r,"X  Filters",459,y+111,20,"accent",132);
            if(picker_options.empty())wrapped(r,"No letters to jump to. Search for another game.",56,y+196,22,528,3);
            for(int i=0;i<int(picker_options.size());i++){int x=40+(i%7)*80,row=y+164+(i/7)*60;if(i==picker_focus)raised_pill(r,x,row,72,52,8,theme.at("accent"));else round_rect(r,x,row,72,52,8,"focus");label(r,picker_options[i],x+(72-label_width(picker_options[i],24))/2,centred_label_y(picker_options[i],24,row+26),24,i==picker_focus?"on_accent":"ink",64);}
        }else {
          SDL_Rect clip{40,top+64,564,picker_height()};if(picker_scrollable())s.RenderSetClipRect(r,&clip);
          int end=std::min(int(picker_options.size()),picker_first()+picker_visible()+(picker_scrollable()?1:0));
          for(int i=picker_first();i<end;i++){
            int stride=picker_stride(),row=top+64+(picker_scrollable()?i*stride-picker_scroll:(i-picker_first())*stride),dx=40,width=picker_scrollable()?520:560;bool focused=i==picker_focus;
            if(!picker_scrollable())move_element(uint32_t(i+100),dx,row,width,stride-6,focused,0,424);dx-=40;
            if(focused)alpha_round(r,40+dx,row+3,width,stride-6,10,shadow_colour(),preferences.dark?70:24);
            round_rect(r,40+dx,row,width,stride-6,10,focused?"focus":"panel");
            if(focused)round_rect(r,40+dx,row+(stride-34)/2,4,28,2,"accent");
            bool layout=picker_title=="Browser layout",swatch=picker_title=="Accent colour";
            if(layout)library_choice_icon(r,i+3,70+dx,row+(stride-6)/2,28,"accent");
            if(swatch){Theme preview=preferences.dark?dark_theme:light_theme;apply_accent(preview,i-1,preferences.dark);accent_swatch(r,74+dx,row+(stride-6)/2,12,preview.at("accent"));}
            label(r,picker_options[i],(layout || swatch?98:58)+dx,row+(stride-30)/2,22,"ink",width-(layout || swatch?134:94));
            if(i==picker_selected)ui_icon(r,SettingsIcon::Check,40+width-29+dx,row+(stride-6)/2,22,"accent");
            else ring(r,40+width-29+dx,row+(stride-6)/2,8,focused?"accent":"line");
          }
          if(picker_scrollable()){
            s.RenderSetClipRect(r,nullptr);
            if(picker_max_scroll()>0)draw_scrollbar(r,572,top+64,picker_height(),picker_thumb(),picker_thumb_offset(),picker_rail,scroll_hover);
          }
        }
        rect(r,0,y+424,640,56,"background");rect(r,24,y+424,592,1,"line");
        footer_button(r,12,y+432,160,"Cancel","B",false,footer_pressed(true));
        label(r,picker_title=="Jump to letter"?"Y Search / X Filters":"Choose an option",202,y+445,16,"muted",252);
        footer_button(r,468,y+432,160,picker_title=="Jump to letter"?(picker_options.empty()?"Search":"Jump"):"Apply","A",true,footer_pressed(false));
    }
    // One back control everywhere: Settings, the in-game menu and the library
    // header share its size, rim, shadow, arrow and pressed colours.
    static constexpr int back_size=44;
    void back_button(SDL_Renderer* r,int x,int y,bool pressed){
        round_rect(r,x,y+3,back_size,back_size,12,"slot");
        int top=y+(pressed?2:0);
        round_rect(r,x,top,back_size,back_size,12,pressed?"accent":"line");
        round_rect(r,x+1,top+1,back_size-2,back_size-2,11,pressed?"accent":"panel");
        ui_icon(r,SettingsIcon::Back,x+back_size/2,top+back_size/2,24,pressed?"on_accent":"accent");
    }
    void settings_header(SDL_Renderer* r,int y,const std::string& title) {
        bool pressed=pointer_down && contact_x<80 && contact_y<78;
        back_button(r,28,y+20,pressed);
        label(r,title,88,y+25,28,"ink",475);
        rect(r,24,y+77,592,1,"line");rect(r,24,y+76,44,2,"accent");
    }
    void choice(SDL_Renderer* r,int x,int y,const std::string& title,bool selected,bool focused) {
        move_element(uint32_t(x+y),x,y,280,48,focused && selected);
        alpha_round(r,x,y+3,280,48,12,shadow_colour(),preferences.dark?70:26);
        round_rect(r,x,y,280,48,12,focused?"accent":"line");
        if(selected)raised_pill(r,x+2,y+2,276,44,10,theme.at("accent"),0,false);else round_rect(r,x+2,y+2,276,44,10,"panel");
        ui_icon(r,title=="Cancel"?SettingsIcon::Back:title=="Continue"?SettingsIcon::Check:settings_icon_for(title),x+28,y+24,21,selected?"on_accent":"muted");
        label(r,title,x+50,y+9,20,selected?"on_accent":"ink",182);
        if(selected){color(r,"on_accent");s.RenderDrawLine(r,x+248,y+24,x+253,y+29);s.RenderDrawLine(r,x+253,y+29,x+263,y+18);}
    }
    void settings_top(SDL_Renderer* r,int y) {
        if(settings.page==Page::ShaderPicker){render_shader_preview(r,y);return;}
        // Navigation belongs on the lower panel. Keep this identity independent
        // of row focus and setting values so the upper panel stays quiet.
        std::string title="Settings";
        SettingsIcon symbol=SettingsIcon::System;
        Page page=settings.page==Page::Confirm?confirm_return:settings.page;
        switch(page){
        case Page::MenuSounds:title="Sounds & music";symbol=SettingsIcon::Volume;break;
        case Page::Motion:title="Animations";symbol=SettingsIcon::Spark;break;
        case Page::Colours:colours_top(r,y);return;
        case Page::Appearance:title="Theme & display";symbol=SettingsIcon::Display;break;
        case Page::Display:title="Screen brightness";symbol=SettingsIcon::Sun;break;
        case Page::Power:title="Power";symbol=SettingsIcon::Power;break;
        case Page::LibraryOptions:title="Library";symbol=SettingsIcon::Library;break;
        case Page::Scraper:case Page::GameArtwork:title="Artwork";symbol=SettingsIcon::Art;break;
        case Page::Keyboard:title=edit_field==5?"DS nickname":edit_field==2?"Connectivity":edit_field==3?"Artwork":edit_field==4?"Search":"ScreenScraper";symbol=edit_field==2?SettingsIcon::Wifi:SettingsIcon::Art;break;
        case Page::Microphone:title="Test microphone";symbol=SettingsIcon::Volume;break;
        case Page::Sound:title="Sound";symbol=SettingsIcon::Volume;break;
        case Page::Network:case Page::Wifi:title="Connectivity";symbol=SettingsIcon::Wifi;break;
        case Page::System:case Page::DeviceInfo:case Page::Timezone:case Page::Maintenance:title="System";break;
        case Page::Startup:title="Startup";symbol=SettingsIcon::Home;break;
        case Page::SaveGallery:title="Save gallery";symbol=SettingsIcon::Save;break;
        case Page::Discover:title="Search and filters";symbol=SettingsIcon::Library;break;
        case Page::RomSetup:title="Add your games";symbol=SettingsIcon::Cartridge;break;
        case Page::DSControls:title="DS controls";symbol=SettingsIcon::Controller;break;
        case Page::Statistics:title="Statistics";symbol=SettingsIcon::Clock;break;
        case Page::Controls:title="Controls";symbol=SettingsIcon::Controller;break;
        case Page::GameOptions:title="Game options";symbol=SettingsIcon::Cartridge;break;
        case Page::Graphics:title="Graphics";symbol=SettingsIcon::Display;break;
        case Page::EmulatorOptions:title=std::string(core_names[active_engine()])+" settings";symbol=SettingsIcon::Cartridge;break;
        case Page::QuickMenu:case Page::ApplyShader:title="Game menu";symbol=SettingsIcon::Controller;break;
        default:break;
        }
        if(page==Page::GameSwitcher && !switch_games.empty()){
            settings_card(r,24,y+82,592,222);
            const auto& game=switch_games[std::min(settings.focus,int(switch_games.size())-1)];
            if(!cover_art(r,folder_for_game(game)/"box.bmp",{46,y+104,174,174},true))game_icon(r,game,89,y+147,88);
            label(r,settings.focus==0?"Resume":"Switch game",246,y+103,18,"accent",344);
            raw_wrapped(r,library_title(game.title),246,y+139,26,344,2);
            wrapped(r,"Your current game is saved before switching.",246,y+222,16,344,2);
            if(!menu_status["message"].empty())wrapped(r,menu_status["message"],32,y+408,18,576,2);
            return;
        }
        if(page==Page::MenuSounds){
            settings_card(r,28,y+106,584,264);ui_icon(r,settings.focus>=2 && settings.focus<=3?SettingsIcon::Spark:SettingsIcon::Volume,320,y+160,48,"accent");centered(r,"A little handheld magic",y+205,2,"ink");
            std::string note=settings.focus>=2 && settings.focus<=3?(preferences.menu_music?(sounds.error.empty()?"A soft tune plays while you browse. It fades out when a game starts.":sounds.error):"Turn on menu music for a soft tune while you browse."):preferences.menu_sounds?(sounds.error.empty()?"Playful sounds for browsing, choosing and starting games.":sounds.error):"Turn on menu sounds to hear the previews.";
            wrapped(r,note,52,y+258,20,536,3);return;
        }
        if(page==Page::SaveGallery){
            round_rect(r,24,y+80,592,326,12,"panel");auto key=std::to_string(settings.focus);
            if(!media.draw(r,slots["image_"+key],{40,y+104,320,240},false))ui_icon(r,SettingsIcon::Save,200,y+220,70,"muted");
            label(r,locale.text("Slot")+" "+std::to_string(settings.focus+1),388,y+122,28,"ink",208);raw_label(r,core_names[gallery_core],388,y+173,20,"accent",208);
            raw_wrapped(r,slots["slot_"+key].empty()?locale.text("Empty slot"):slots["slot_"+key],388,y+219,20,208,3);
            wrapped(r,o.game_menu?menu_status["message"]:device_status["message"],36,y+418,16,568,2);return;
        }
        if(page==Page::Microphone){round_rect(r,24,y+82,592,320,12,"panel");ui_icon(r,SettingsIcon::Volume,320,y+145,52,"accent");centered(r,"Test microphone",y+202,2,"ink");wrapped(r,"Blow or speak into the microphone. Watch the level rise, then fall when you stop.",54,y+255,22,532,4);return;}
        if(page==Page::DSControls){round_rect(r,24,y+82,592,320,12,"panel");ui_icon(r,SettingsIcon::Controller,320,y+126,48,"accent");centered(r,"DS controls",y+181,2,"ink");wrapped(r,!ds_supported()?"Use the engine's native controls for microphone and DS settings.":status.empty()?"Real microphone is enabled by default. Blow and lid actions return to the game for two seconds. Language changes apply after restarting.":status,54,y+242,22,532,5);return;}
        if(page==Page::RomSetup){round_rect(r,24,y+82,592,320,12,"panel");ui_icon(r,SettingsIcon::Cartridge,320,y+128,48,"accent");centered(r,!library_ready?"Looking for games…":setup_scanned?"Scan complete":"Your DS library starts here",y+177,2,"ink");
            const size_t found=model.games.size();
            if(!library_ready)wrapped(r,"Reading Roms/NDS on your games card. This takes a moment.",54,y+242,24,532,4);
            else if(setup_scanned)wrapped(r,found?locale.text(locale.text(std::to_string(found)+(found==1?" game":" games"))+" found. Choose Done to go to your library."):locale.text("No games found. Copy .nds or DS ZIP files to Roms/NDS on your games card and scan again."),54,y+242,24,532,4);
            else wrapped(r,"Copy .nds or DS ZIP files to Roms/NDS on your games card, then choose Scan for games.",54,y+242,24,532,4);return;}
        if(page==Page::Statistics){
            const auto& group=summary.groups[0];
            // A ring of your collection: the played share fills it, the hours sit inside.
            settings_card(r,24,y+66,592,344);
            const double share=group.games?double(group.played)/double(group.games):0;
            const int rx=150,ry=y+190;
            feedback_arc(r,rx,ry,78,0,1,theme.at("slot"),255,12);
            if(summary_ready && share>0)feedback_arc(r,rx,ry,78,0,std::max(.02,share),theme.at("accent"),255,12);
            const std::string hours=summary_ready?std::to_string(group.seconds/3600)+"h":"…",minutes=summary_ready?std::to_string((group.seconds/60)%60)+"m":"";
            raw_label(r,hours,rx-label_width(hours,36,false)/2,ry-34,36,"ink",160);
            raw_label(r,minutes,rx-label_width(minutes,20,false)/2,ry+10,20,"muted",160);
            {const std::string caption=locale.text("Time played");raw_label(r,caption,rx-label_width(caption,14,false)/2,ry+94,14,"muted",200);}
            // Three tiles on the right.
            const std::array<std::string,3> values={summary_ready?std::to_string(group.games):"…",summary_ready?std::to_string(group.played)+(group.games?"  ·  "+std::to_string(int(std::lround(share*100)))+"%":""):"…",summary_ready?std::to_string(group.sessions):"…"};
            const std::array<const char*,3> titles={"Games","Games played","Times played"};
            for(int i=0;i<3;i++){const int row=y+92+i*72;round_rect(r,276,row,320,60,12,"slot");round_rect(r,277,row+1,318,58,11,"background");
                label(r,titles[i],296,row+8,14,"muted",280);raw_label(r,values[size_t(i)],296,row+27,24,"ink",280);}
            // The favourite: the most played game, with its icon.
            round_rect(r,276,y+310,320,76,12,"focus");
            if(summary_ready && !summary.most_played.empty()){const auto& top=summary.most_played[0];game_icon(r,top.game,292,y+324,48,0);label(r,"Most played",352,y+320,14,"muted",232);raw_label(r,top.title,352,y+341,20,"ink",232);}
            else{ui_icon(r,SettingsIcon::Heart,316,y+348,28,"muted");label(r,summary_ready?"Nothing played yet":"Preparing…",352,y+338,16,"muted",232);}
            return;
        }
        if(page==Page::Maintenance){
            ui_icon(r,SettingsIcon::System,320,y+112,40,"accent");centered(r,"Updates & recovery",y+157,2,"ink");
            round_rect(r,32,y+211,576,177,12,"panel");
            label(r,"Installed version",52,y+226,16,"muted",530);raw_label(r,o.app.empty()?"Development":o.app.filename().string(),52,y+251,20,"ink",530);
            std::string message=update_status["message"].empty()?"Check for updates when you are ready.":update_status["message"];
            if(!service_notice.empty())message=service_notice;
            else if(worker_action.rfind("device.update-",0)!=0 && !device_status["message"].empty())message=device_status["message"];
            wrapped(r,message,52,y+288,20,530,3);
            int progress=std::clamp(std::atoi(update_status["progress"].c_str()),0,100);
            bool busy=worker>0 && worker_action.rfind("device.update-",0)==0;
            if(busy){round_rect(r,52,y+367,494,6,3,"slot");round_rect(r,52,y+367,std::max(3,494*progress/100),6,3,"accent");raw_label(r,std::to_string(progress)+"%",552,y+354,16,"muted",48);}
            if(!update_status["version"].empty())raw_label(r,update_status["version"],52,y+408,20,"accent",530);return;
        }
        ui_icon(r,symbol,320,y+218,36,"muted");
        int width=label_width(title,24);
        label(r,title,320-width/2,y+254,24,"muted",584);
        if((page==Page::GameArtwork || (page==Page::Keyboard && edit_field==3)) && !model.games.empty()){
            auto name=game_title(model.selected);int name_width=std::min(576,label_width(name,20));
            label(r,name,320-name_width/2,y+318,20,"ink",576);
        }
        if(page==Page::Network || page==Page::Wifi)network_status_card(r,y);
        if(settings.page==Page::Controls && control_capture){
            const std::string prompt="Press a button for "+(capture_logical==12?std::string("fast-forward"):control_title(capture_logical));
            label(r,prompt,320-label_width(prompt,20)/2,y+320,20,"ink",584);
            label(r,"Anbernic / Escape cancels",320-label_width("Anbernic / Escape cancels",16)/2,y+354,16,"muted",584);
        }
        // Operational feedback remains visible, without permanent advice cards.
        std::string message;
        if(o.game_menu && (menu_status["state"]=="busy" || menu_status["state"]=="error"))message=menu_status["message"];
        else if(picker_open && picker_core_offset>=0)message=emulator_choice_hint();
        else if(picker_open && picker_title=="Standard screen order")message="Standard DraStic only. Applies on the next launch; game saves are kept.";
        else if(picker_open && picker_title=="DraStic backend")message="SuperDrastic uses its own pause menu and display path. Experimental; RG DS testing is pending.";
        else if(status.rfind("Could not save",0)==0)message=status;
        else if(settings.page==Page::GameOptions && settings.focus==2 && superdrastic_game())message="SuperDrastic currently uses native output. Your standard shader choice is kept.";
        else if(settings.page==Page::GameOptions && settings.focus==7 && superdrastic_game())message="Gengis is experimental. 3x smooths 3D into a 2x picture and costs more CPU; some effects may differ.";
        else if(settings.page==Page::Graphics){
            static const char* hints[]={"True 3D resolution. 3x balances quality and speed; 5x is experimental.","Smooth combines 3D samples at the panel resolution. 2D artwork stays crisp.","Adaptive renders heavy 3D scenes at native for a few seconds to hold 60 fps. Applies during play.","Resamples 2D art, text and menus on the 3D screen. Sharp keeps pixels crisp; Smooth blends them; Crisp smooths pixel-art stairs.","Screen filters change the final picture, including 2D artwork.","Keep off for smooth motion. Skipping display frames can help demanding games.","The renderer currently running. Graphics changes apply during play.","Save a checkpoint and restart with your chosen graphics settings."};
            message=status.empty()?hints[std::clamp(settings.focus,0,7)]:status;
        }
        else if(settings.page==Page::EmulatorOptions){
            int engine=active_engine(),next=game_preferences.core<0?int(preferences.core):game_preferences.core;
            if(settings.focus==list_count()-1)message="Restart from the title screen. In-game saves and save slots are kept.";
            else if(settings.focus==list_count()-2)message="Microphone, simulated blowing, lid and DS language.";
            else if(settings.focus==0)message=engine!=0?"Counts emulated game frames. Changes immediately.":"Counts display updates, not emulation speed. Changes immediately.";
            else if(settings.focus==1)message="Changes immediately. Map a button in Controls for quick access.";
            else if(settings.focus==2)message="Applies immediately. Higher clocks use more battery.";
            else if(settings.focus==3)message=engine==1?"3D rendering automatically uses worker threads.":engine==2?"This engine does not expose a threading switch.":engine==3?"Applies immediately. Your game keeps its progress.":"Save & restart applies Threaded 3D.";
            else if(settings.focus==4 && experimental_game())message="Higher resolution costs GPU time. Changes apply during play; Native uses software rendering.";
            else if(settings.focus==4)message=emulator_option_enabled(4)?"Native is faster; 2x uses more GPU time. Save & restart applies it.":"This emulator uses native resolution. No resolution switch is available.";
            else if(settings.focus==5)message=emulator_option_enabled(5)?"Off gives smooth motion. Skipping frames can help speed. Restart to apply.":"This emulator does not expose a frameskip setting.";
            else if(settings.focus==9)message="Sets Standard DraStic screen order for this game. Save & restart applies it.";
            else if(settings.focus==8)message="Choose the display backend for the next DraStic launch. DSperate stays your default.";
            else if(settings.focus==6 || next!=engine)message="Restart uses "+std::string(core_names[next])+". Saves and checkpoints stay separate per emulator.";
            else message="Save & restart keeps your checkpoint and applies pending settings.";
        }
        else if(settings.page==Page::Controls && controls_notice.rfind("Could not",0)==0)message=controls_notice;
        else if(settings.page==Page::LibraryOptions){
            if(backup_requested && worker_action=="device.library"){
                if(worker>0)message="Backing up your saves…";
                else if(device_status["state"]=="error")message=device_status["message"].empty()?"The backup did not finish. Please try again.":device_status["message"];
                else if(!device_status["message"].empty())message=device_status["message"];
            }
            if(message.empty()){if(settings.focus==9)message="Copies every save and checkpoint into one zip in userdata/exports.";else if(settings.focus==8)message="Where games go on the card, and a fresh scan.";else if(settings.focus==1)message="Where covers and titles come from.";}
        }
        else if(page==Page::Startup || page==Page::Network || page==Page::Wifi || page==Page::Maintenance){
            if(device_status["state"]=="error")message=device_status["message"];
        }else if(page==Page::Scraper || page==Page::GameArtwork){
            if(service["state"]=="scraping")message=service["completed"]+" / "+service["total"]+" games";
            else if(!service_notice.empty())message=service_notice;
            else message=page==Page::GameArtwork?artwork_result["message"]:service["message"];
        }
        if(!message.empty())wrapped(r,message,32,y+422,16,576,2);
    }
    // What the radio is doing right now, as a phone would show it: a green
    // card while connected, a spinner while connecting or scanning, a plain
    // note while off or idle, and the service's message when it failed.
    bool network_busy()const{return worker>0 && (worker_action=="device.connect" || worker_action=="device.scan" || worker_action=="device.wifi-on" || worker_action=="device.wifi-off");}
    void network_status_card(SDL_Renderer* r,int y){
        const bool connected=device_status["wifi_connected"]=="1",radio=device_status["wifi"]=="1",busy=network_busy(),failed=!busy && device_status["state"]=="error";
        const int left=40,top=y+296,width=560,height=116;
        const Color green=preferences.dark?Color{36,128,76}:Color{44,160,92},white{255,255,255};
        const Color tone=busy?theme.at("accent"):green;const bool lit=busy || connected;
        alpha_round(r,left,top+4,width,height,16,shadow_colour(),preferences.dark?60:22);
        if(lit)glossy_pill(r,left,top,width,height,16,tone);
        else{round_rect(r,left,top,width,height,16,"line");round_rect(r,left+1,top+1,width-2,height-2,15,failed?"focus":"panel");}
        const Color ink=lit?white:theme.at("ink"),soft=lit?blend(white,tone,22):theme.at("muted");
        // The badge on the left: a spinner, signal bars, or the radio glyph.
        const int bx=left+54,by=top+height/2;
        fill_round(r,bx-30,by-30,60,60,30,lit?blend(tone,white,18):theme.at("slot"));
        if(busy)feedback_arc(r,bx,by,19,preferences.animations?(s.GetTicks()%1400)/1400.0:0,.7,white,255,3);
        else if(connected){
            int signal=device_status["wifi_signal"].empty()?-1:std::atoi(device_status["wifi_signal"].c_str());if(probed_signal>=0)signal=probed_signal;
            const int level=signal>=67?3:signal>=34?2:signal>0?1:0;
            for(int i=0;i<4;i++){const int h=8+i*6;fill_round(r,bx-16+i*9,by+12-h,6,h,2,i<=level?white:blend(green,white,45));}
        }else{
            draw_settings_icon(s,r,SettingsIcon::Wifi,bx,by,30,theme.at(radio?"accent":"muted"));
            if(!radio){color(r,"muted");for(int w=0;w<3;w++)s.RenderDrawLine(r,bx-16+w,by-16,bx+14+w,by+16);}
        }
        const int tx=left+104;
        std::string headline,detail,extra;bool translate_headline=true;
        if(busy){headline=worker_action=="device.scan"?"Looking for networks…":worker_action=="device.wifi-on"?"Turning Wi-Fi on…":worker_action=="device.wifi-off"?"Turning Wi-Fi off…":locale.text("Connecting to")+" "+(network_id.empty()?device_status["connection"]:network_id)+"…";translate_headline=worker_action!="device.connect";detail=locale.text("This can take a few seconds");}
        else if(connected){headline=device_status["connection"];translate_headline=false;detail=device_status["ip"].empty()?locale.text("Waiting for an address…"):locale.text("IP address")+"  "+device_status["ip"];
            int signal=device_status["wifi_signal"].empty()?-1:std::atoi(device_status["wifi_signal"].c_str());if(probed_signal>=0)signal=probed_signal;if(signal>=0)extra=locale.text("Signal")+"  "+std::to_string(signal)+"%";}
        else if(failed){headline="Could not connect";detail=device_status["message"];}
        else if(!radio){headline="Wi-Fi is off";detail=device_status["wifi_supported"]=="0"?device_status["wifi_reason"]:locale.text("Turn it on to find networks");}
        else{headline="Not connected";detail=locale.text("Choose a network from the list");}
        raw_label_colour(r,translate_headline?locale.text(headline):headline,tx,top+22,26,ink,width-120);
        raw_label_colour(r,detail,tx,top+60,18,soft,width-120);
        if(!extra.empty())raw_label_colour(r,extra,tx,top+86,16,soft,width-120);
    }
    void search_icon(SDL_Renderer* r,int x,int y,int size,const std::string& ink){
        ring(r,x-2,y-2,size/3,ink);color(r,ink);
        for(int w=0;w<2;w++)s.RenderDrawLine(r,x+size/6,y+size/6+w,x+size/2,y+size/2+w);
    }
    void keyboard_screen(SDL_Renderer* r,int y) {
        settings_header(r,y,edit_field==5?"DS nickname":edit_field==4?"Search games":edit_field==3?"Matching title":edit_field==0?"Username":"Password");
        round_rect(r,20,y+91,600,62,12,"accent");round_rect(r,22,y+93,596,58,10,"panel");
        std::string value=(edit_field==0 || edit_field==3 || edit_field==4 || edit_field==5)?edit_buffer:std::string(RoundedFont::codepoints(edit_buffer).size(),'*');
        bool hidden=edit_field==1 || edit_field==2;
        size_t cursor=hidden?RoundedFont::codepoints(edit_buffer.substr(0,edit_cursor)).size():edit_cursor;
        while(cursor && label_width(value.substr(0,cursor),24,false)>480){size_t next=1;while(next<value.size() && (static_cast<unsigned char>(value[next])&0xc0)==0x80)next++;value.erase(0,next);cursor-=next;}
        int x=edit_field==4?64:38;
        if(edit_field==4)search_icon(r,43,y+121,22,"accent");
        raw_label(r,value.empty()?locale.text(edit_field==4?"Type a game title…":"Enter here"):value,x,y+109,24,value.empty()?"muted":"ink",490);
        if(!edit_buffer.empty())rect(r,x+label_width(value.substr(0,cursor),24,false)+2,y+109,2,28,"accent");
        if(!edit_buffer.empty()){round_rect(r,568,y+103,40,38,10,"slot");label(r,"X",582,y+112,18,"muted",24);}
        label(r,edit_field==4?(search_preview_loading?"Finding games…":locale.text(std::to_string(search_matches.size())+(search_matches.size()==1?" match":" matches"))):"Tap to type, or use the D-pad and A",24,y+160,14,"muted",580);
        draw_keyboard(r,y,keyboard_rows(),true);
    }
    void search_keyboard_top(SDL_Renderer* r,int y){
        label(r,"Search in",28,y+78,16,"muted",580);
        static const std::array<std::string,3> views={"All games","Favourites","Recently played"};
        label(r,views[preferences.view],28,y+102,30,"ink",580);
        if(discovery_filter)label(r,discovery_filter_name(),28,y+146,18,"accent",580);
        std::string message=search_preview_loading?"Finding games…":locale.text(std::to_string(search_matches.size())+(search_matches.size()==1?" matching game":" matching games"));
        label(r,message,28,y+185,22,"accent",580);
        if(!search_preview_loading && search_matches.empty())wrapped(r,"Try a shorter title or clear your filter.",28,y+236,22,570,2);
        for(size_t i=0;i<std::min(size_t(4),search_matches.size());i++){
            round_rect(r,24,y+228+int(i)*49,592,42,9,"panel");
            raw_label(r,library_title(search_matches[i].title),42,y+237+int(i)*49,20,"ink",554);
        }
        label(r,"A Key / Start Search / B Cancel",28,y+442,16,"muted",584);
    }
    void settings_bottom(SDL_Renderer* r,int y) {
        rect(r,0,y,640,480,"background");pattern(r,y,480);
        if(o.game_menu && menu_status["state"]=="busy"){
            feedback_arc(r,320,y+195,30,preferences.animations?(s.GetTicks()%1600)/1600.0:0,.72,theme.at("accent"),255,3);
            ui_icon(r,SettingsIcon::Save,320,y+195,25,"accent");
            wrapped(r,menu_status["message"].empty()?"Saving your progress…":menu_status["message"],72,y+257,26,496,3);
            centered(r,"Please keep your handheld on",y+371,1,"muted");return;
        }
        if(settings.page==Page::Statistics){
            settings_header(r,y,"Most played");
            if(!summary_ready)wrapped(r,"Preparing your statistics…",32,y+126,24,560,2);
            else if(summary.most_played.empty())wrapped(r,"Play a game to start your statistics.",32,y+126,24,560,3);
            else for(size_t i=0;i<summary.most_played.size();i++){auto& entry=summary.most_played[i];int row=y+100+int(i)*64;
                alpha_round(r,28,row+3,584,60,10,shadow_colour(),preferences.dark?50:16);round_rect(r,28,row,584,60,10,"panel");
                // Gold, silver and bronze for the top three; a plain number after that.
                static const std::array<Color,3> medals={Color{236,178,48},Color{168,180,192},Color{196,128,78}};
                if(i<3){fill_round(r,40,row+16,28,28,14,medals[i]);alpha_round(r,45,row+19,18,8,4,Color{255,255,255},90);const std::string n=std::to_string(i+1);raw_label_colour(r,n,54-label_width(n,16,false)/2,centred_label_y(n,16,row+30),16,Color{255,255,255},24);}
                else raw_label(r,std::to_string(i+1),42,row+19,20,"muted",28);
                game_icon(r,entry.game,76,row+6,48,i);raw_label(r,entry.title,140,row+9,20,"ink",286);
                auto time=std::to_string(entry.seconds/3600)+"h "+std::to_string((entry.seconds/60)%60)+"m";raw_label(r,time,450,row+10,18,"accent",146);
                round_rect(r,140,row+43,286,4,2,"slot");round_rect(r,140,row+43,std::max(3,int(286*entry.seconds/std::max<uint64_t>(1,summary.most_played[0].seconds))),4,2,"accent");
                raw_label(r,std::to_string(entry.sessions)+" ×",450,row+34,14,"muted",145);
            }return;
        }
        if(settings.page==Page::GameSwitcher){
            sync_list();settings_header(r,y,"Game switcher");
            SDL_Rect clip{20,y+list_top,575,list_height};s.RenderSetClipRect(r,&clip);
            for(int i=0;i<list_count();i++){
                int row=y+list_top+i*list_stride-list_scroll;if(row+66<y+list_top || row>y+list_top+list_height)continue;
                if(i==int(switch_games.size())){list_row(r,y,i,"Game menu");continue;}
                int x=24;bool selected=settings.focus==i;
                move_element(uint32_t(i),x,row,566,66,selected,list_top,list_top+list_height,24,590);
                round_rect(r,x,row,566,66,10,selected?"focus":"panel");
                if(selected)round_rect(r,x,row+16,4,34,2,"accent");
                game_icon(r,switch_games[i],x+14,row+9,48);
                raw_label(r,library_title(switch_games[i].title),x+76,row+6,22,"ink",464);
                label(r,i==0?"Resume":"Switch game",x+76,row+38,14,selected?"accent":"muted",464);
            }
            s.RenderSetClipRect(r,nullptr);list_scrollbar(r,y);return;
        }
        if(settings.page==Page::ShaderPicker){render_shader_carousel(r,y);return;}
        sync_list();int count=list_count();
        if(count>0 || settings.page==Page::Wifi){
            std::string title=settings.page==Page::Graphics?"Graphics":settings.page==Page::MenuSounds?"Sounds & music":settings.page==Page::SaveGallery?"Save gallery":settings.page==Page::Discover?"Search and filters":settings.page==Page::RomSetup?"Add your games":settings.page==Page::DSControls?"DS controls":settings.page==Page::Settings?"Settings":settings.page==Page::Appearance?"Theme & display":settings.page==Page::Power?"Power":settings.page==Page::LibraryOptions?"Library":settings.page==Page::GameOptions?"Game options":settings.page==Page::System?"System":settings.page==Page::Startup?"Startup":settings.page==Page::Network?"Connectivity":settings.page==Page::Wifi?"Available networks":settings.page==Page::Timezone?"Timezone":settings.page==Page::ShaderPicker?"Shaders":settings.page==Page::EmulatorOptions?"Emulator settings":(settings.page==Page::Scraper || settings.page==Page::GameArtwork)?"Artwork":settings.page==Page::DeviceInfo?"Device information":settings.page==Page::Controls?"Controls":"Updates & recovery";
            settings_header(r,y,title);if(settings.page==Page::Wifi){secondary_button(r,476,y+24,150,42,12,pointer_down && contact_x>=476 && contact_y>=24 && contact_y<=66);ui_icon(r,SettingsIcon::Refresh,498,y+45,18,"accent");label(r,"Scan",518,y+34,20,"accent",91);}
            if(settings.page==Page::ShaderPicker){secondary_button(r,368,y+24,120,42,12,pointer_down && contact_x>=368 && contact_x<=488 && contact_y>=24 && contact_y<=66);ui_icon(r,SettingsIcon::Art,386,y+45,18,"accent");label(r,"Scene",403,y+35,17,"ink",74);secondary_button(r,496,y+24,128,42,12,pointer_down && contact_x>=496 && contact_y>=24 && contact_y<=66);ui_icon(r,SettingsIcon::Shader,514,y+45,18,"accent");label(r,"Compare",530,y+35,16,"ink",85);}
            SDL_Rect clip{20,y+list_top,575,list_height};s.RenderSetClipRect(r,&clip);
            for(int i=0;i<count;i++){
                if(i*list_stride-list_scroll+66<0 || i*list_stride-list_scroll>list_height)continue;
                if(settings.page==Page::Settings)list_row(r,y,i,sections[i].title);
                else if(settings.page==Page::Appearance)appearance_row(r,y,i);
                else if(settings.page==Page::ShaderPicker){int value=shader_choices[i];list_row(r,y,i,shader_name(value),(shader_per_game?game_preferences.filter:preferences.filter)==value?"Selected":"");}
                else if(settings.page==Page::Power){
                    const std::array<std::string,6> names={"Power mode","Menu idle sleep","Sleep on lid close","Battery percentage","Sleep now","Power off"};
                    const std::array<std::string,6> values={power_names[system_preferences.power],idle_name(system_preferences.idle),system_preferences.lid?"On":"Off",system_preferences.battery_percent?"On":"Off","Sleep","Power off"};
                    const std::array<bool,6> enabled={hardware.power,hardware.sleep,hardware.lid,hardware.battery>=0,hardware.sleep,hardware.poweroff};list_row(r,y,i,names[i],enabled[i]?values[i]:"Unavailable",enabled[i]);
                }else if(settings.page==Page::LibraryOptions){const std::array<std::string,10> names={"Default emulator","Artwork sources","Auto-load state","Auto-save on exit","Default shader","Browser layout","DraStic backend","Search and filters","Add your games","Back up all saves"};const std::array<std::string,10> values={core_names[preferences.core],"",preferences.auto_load?"On":"Off",preferences.auto_save?"On":"Off",shader_name(preferences.filter),preferences.layout==0?"Cartridges":preferences.layout==1?"Covers":"List",drastic_backend_name(preferences.drastic_backend),"","",""};list_row(r,y,i,names[i],values[i]);}
                else if(settings.page==Page::GameOptions && i>=count-2)list_row(r,y,i,i==count-2?"Save gallery":"DS controls");
                else if(settings.page==Page::GameOptions && i==5 && experimental_game())list_row(r,y,i,"3D resolution",experimental_resolution_name());
                else if(settings.page==Page::GameOptions){const std::array<std::string,8> names={"Favourite","Emulator","Shader","Controls","Artwork","DraStic backend","Screen order","3D resolution"},values={game_preferences.favourite?"On":"Off",game_preferences.core<0?"Default":core_names[game_preferences.core],superdrastic_game()?"Native output":shader_name(game_preferences.filter),"","",drastic_backend_name(game_preferences.drastic_backend),game_preferences.drastic_screen_swap==1?"Swapped":"Normal",superdrastic_resolution_name()};list_row(r,y,i,names[i],values[i],(i!=2 && i!=6) || !superdrastic_game());}
                else if(settings.page==Page::Controls)list_row(r,y,i,controls_row_name(i),controls_row_value(i));
                else if(settings.page==Page::MenuSounds){const std::array<std::string,5> names={"Menu sounds","Effects volume","Menu music","Music volume","Preview sound"};list_row(r,y,i,names[i],i==0?(preferences.menu_sounds?"On":"Off"):i==1?std::to_string(preferences.sound_volume)+"%":i==2?(preferences.menu_music?"On":"Off"):i==3?std::to_string(preferences.music_volume)+"%":"");}
                else if(settings.page==Page::System){const std::array<std::string,8> names={"Language","Timezone","Date format","Time format","Device information","Updates & recovery",o.independent?"Restart menu":"Return to stock","Startup"};list_row(r,y,i,names[i],i==0?locale.name():i==1?system_preferences.timezone:i==2?date_format_names.at(system_preferences.date_format):i==3?time_format_names.at(system_preferences.time_format):"");}
                else if(settings.page==Page::Startup){const std::array<std::string,4> names={"Boot into","Touch to continue","Restart","Resume after power off"};const std::array<std::string,4> values={o.independent?"RG DS Core":device_status["boot_mode"]=="rgds"?"RG DS Core":"Anbernic",device_status["boot_warning"]=="1"?"On":"Off","",experience["quick_resume"]=="1"?"On":"Off"};list_row(r,y,i,names[i],values[i],!o.independent || i!=0);}
                else if(settings.page==Page::SaveGallery)list_row(r,y,i,locale.text("Slot")+" "+std::to_string(i+1),slots["slot_"+std::to_string(i)].empty()?"Empty slot":slots["slot_"+std::to_string(i)]);
                else if(settings.page==Page::Discover){const std::array<std::string,3> names={"Search games","Filter","Clear search and filters"};const std::array<std::string,4> filters={"No filter","Unplayed","Played","Missing artwork"};list_row(r,y,i,names[i],i==0?search_query:i==1?filters[discovery_filter]:"");}
                else if(settings.page==Page::RomSetup){const std::array<std::string,3> names={"Where to put games","Scan for games","Done"};const size_t found=model.games.size();list_row(r,y,i,names[i],i==1?(!library_ready?locale.text("Scanning…"):setup_scanned?locale.text(std::to_string(found)+(found==1?" game":" games")):std::string()):std::string());}
                else if(settings.page==Page::DSControls)ds_controls_row(r,y,i);
                else if(settings.page==Page::Network){const std::array<std::string,5> names={"Wi-Fi","Available networks","Refresh connection","Network file sharing","Sharing details"};
                    std::string value=i==0?(device_status["wifi_supported"]=="0"?"Unavailable":device_status["wifi"]=="1"?"On":"Off"):i==1?(network_busy()?"Working…":device_status["wifi_connected"]=="1"?device_status["connection"]:""):i==3?(device_status["sharing"]!="1"?"Off":device_status["sharing_mode"]=="open"?"Open":"Password"):i==4?"SMB":"";
                    list_row(r,y,i,names[i],value,i!=4 || device_status["sharing"]=="1");}
                else if(settings.page==Page::Wifi){auto prefix="network_"+std::to_string(i)+"_";const bool joining=worker>0 && worker_action=="device.connect" && device_status[prefix+"id"]==network_id;list_row(r,y,i,device_status[prefix+"ssid"],joining?"Connecting…":device_status[prefix+"connected"]=="1"?"Connected":(device_status[prefix+"saved"]=="1"?"Saved · ":"")+device_status[prefix+"signal"]+"%");}
                else if(settings.page==Page::Timezone){auto name=std::string(timezones[i]);std::replace(name.begin(),name.end(),'_',' ');list_row(r,y,i,name,system_preferences.timezone==timezones[i]?"Selected":"");}
                else if(settings.page==Page::Maintenance){bool busy=worker>0 && worker_action.rfind("device.update-",0)==0;
                    const std::array<std::string,7> names={"Check for updates",update_status["prepared"]=="1"?"Restart & install":"Download update",busy?"Pause download":"Release notes","Use previous build","Clear temporary cache",update_status["prepared"]=="1"?"Discard update":"Save device report","Automatic updates"};
                    bool enabled=!busy || (i==2 && worker_action=="device.update-download");if(i==1)enabled=enabled && (update_status["state"]=="available" || update_status["prepared"]=="1" || update_status["state"]=="cancelled" || update_status["state"]=="error");
                    list_row(r,y,i,names[i],i==0?(update_status["channel"]=="testing"?"Testing":"Stable"):i==1?update_status["version"]:i==6?experience["auto_update"]=="0"?"Off":experience["auto_update"]=="2"?"Download automatically":"Check automatically":"",enabled);}
                else if(settings.page==Page::GameArtwork){const std::array<std::string,3> names={"Matching title",worker>0 && worker_action=="scrape"?"Stop fetching":"Fetch artwork","Use ROM identity"};list_row(r,y,i,names[i],i==0?(artwork_match.empty()?"Automatic":artwork_match):"");}
                else if(settings.page==Page::Scraper){const std::array<std::string,3> providers={"Automatic","GameTDB","Libretro"};const std::array<std::string,3> names={"Artwork source",worker>0 && worker_action=="scrape"?"Stop scraping":"Fetch missing artwork","Review unmatched"};list_row(r,y,i,names[i],i==0?providers.at(preferences.artwork_provider):"");}
                else if(settings.page==Page::DeviceInfo)list_row(r,y,i,device_info_name(i),device_info_value(i));
                else if(settings.page==Page::Graphics){const std::array<std::string,8> names={"3D resolution","3D edge smoothing","Heavy 3D scenes","2D scaling","Screen filter","Frameskip","Running now","Save & restart"};const std::array<std::string,8> values={experimental_resolution_name(),game_preferences.rgds_smooth?"Smooth":"Sharp",game_preferences.rgds_adaptive?"Adaptive":"Always sharp",filter2d_name(game_preferences.rgds_2d),shader_name(game_preferences.filter),game_preferences.frameskip>0?std::to_string(game_preferences.frameskip)+" frames":"Off (smooth)",active_graphics_name(),""};list_row(r,y,i,names[i],values[i],i!=6);}
                else if(settings.page==Page::EmulatorOptions && i>=count-2)list_row(r,y,i,i==count-2?"DS controls":"Reset state");
                else if(settings.page==Page::EmulatorOptions){int engine=active_engine();const std::array<std::string,10> names={engine!=0?"Game FPS":"Display FPS","Fast-forward","Power profile","Threaded 3D","3D resolution","Frameskip","Next emulator","Save & restart","DraStic backend","Screen order"};bool enabled=emulator_option_enabled(i);const std::array<std::string,10> values={menu_status["fps"]=="1"?"On":"Off",menu_status["fast_forward"]=="1"?"On":"Off",power_names[system_preferences.power],(engine==1 || engine==4)?"Automatic":engine==2?"Engine managed":engine==3?(menu_status["threaded"]=="0"?"Off":"On"):game_preferences.threaded==0?"Off":"On",experimental_game()?experimental_resolution_name():superdrastic_game()?superdrastic_resolution_name():game_preferences.hires==1?"2x":"Native",game_preferences.frameskip>0?std::to_string(game_preferences.frameskip)+" frames":"Off (smooth)",game_preferences.core<0?"Default":core_names[game_preferences.core],"",drastic_backend_name(game_preferences.drastic_backend),game_preferences.drastic_screen_swap==1?"Swapped":"Normal"};list_row(r,y,i,names[i],enabled || i==3?values[i]:"Unavailable",enabled);}
            }
            s.RenderSetClipRect(r,nullptr);list_scrollbar(r,y);
            if(settings.page==Page::Wifi && !count)wrapped(r,worker>0?"Looking for networks…":!device_status["message"].empty()?device_status["message"]:!device_status["wifi_reason"].empty()?device_status["wifi_reason"]:"No networks found",30,y+145,24,560,4);
            return;
        }
        if(settings.page==Page::QuickMenu) {
            settings_header(r,y,"Game menu");bool busy=menu_status["state"]=="busy";
            const std::array<std::string,11> names={"Resume","Save slots","Load slots",experimental_game()?"Graphics":"Shaders","Emulator","Sound","Brightness","Save & restart","Return home","Controls","Game switcher"};
            const std::array<SettingsIcon,11> symbols={SettingsIcon::Play,SettingsIcon::Save,SettingsIcon::Load,SettingsIcon::Shader,SettingsIcon::Sliders,SettingsIcon::Volume,SettingsIcon::Sun,SettingsIcon::Restart,SettingsIcon::Home,SettingsIcon::Controller,SettingsIcon::Library};
            for(int i=0;i<11;i++){
                bool top=i==0 || i==10;
                int x=top?(i==0?28:328):28+((i-1)%3)*200,width=top?284:184,row=y+(top?96:166+((i-1)/3)*82),height=top?56:72;
                bool enabled=!busy && true,focused=settings.focus==i;
                move_element(uint32_t(i),x,row,width,height,focused);
                settings_card(r,x,row,width,height,focused);
                if(top){round_rect(r,x+14,row+10,36,36,11,"panel");ui_icon(r,symbols[i],x+32,row+28,28,"accent");label(r,names[i],x+62,row+16,20,enabled?"ink":"muted",width-76);}
                else{ui_icon(r,symbols[i],x+width/2,row+23,32,enabled?"accent":"muted");int measured=std::min(width-24,label_width(names[i],16));label(r,names[i],x+(width-measured)/2,row+44,16,enabled?"ink":"muted",width-24);}
            }return;
        }
        if(settings.page==Page::ApplyShader){
            settings_header(r,y,"Shader saved");label(r,shader_name(game_preferences.filter),30,y+106,24,"accent",560);
            const std::array<std::string,2> names={"Save & restart","Keep playing"};
            for(int i=0;i<2;i++){int row=y+174+i*86;settings_card(r,28,row,584,66,settings.focus==i);ui_icon(r,i==0?SettingsIcon::Refresh:SettingsIcon::Play,62,row+33,25,"accent");label(r,names[i],94,row+21,24,"ink",490);}return;
        }
        if(settings.page==Page::LeaveGame){settings_header(r,y,"Return home");wrapped(r,"The checkpoint did not finish saving.",30,y+105,28,580,2);const std::array<const char*,3> names={"Try saving again","Resume game","Return without saving"};const std::array<SettingsIcon,3> symbols={SettingsIcon::Save,SettingsIcon::Play,SettingsIcon::Home};for(int i=0;i<3;i++){int row=y+210+i*70;settings_card(r,28,row,584,58,settings.focus==i);ui_icon(r,symbols[i],60,row+29,24,"accent");label(r,names[i],94,row+17,20,"ink",490);}return;}
        if(settings.page==Page::Keyboard){keyboard_screen(r,y);return;}
        if(settings.page==Page::Colours){colours_screen(r,y);return;}
        if(settings.page==Page::Motion){
            settings_header(r,y,"Animations");
            const std::array<std::string,2> names={"Interface animations","View transitions"};
            const std::array<std::string,2> hints={"Small touches, scrolling and startup motion","Animate pages with the selected style"};
            for(int i=0;i<2;i++){int row=y+motion_row(i),dx=28;move_element(uint32_t(i),dx,row,584,72,settings.focus==i);dx-=28;settings_card(r,28+dx,row,584,72,settings.focus==i);ui_icon(r,SettingsIcon::Spark,56+dx,row+22,22,"accent");label(r,names[i],83+dx,row+10,22,"ink",410);label(r,hints[i],48+dx,row+44,16,"muted",480);draw_toggle(r,552+dx,row+8,names[i],i==0?preferences.animations:preferences.view_animations);}
            // Three-way choices: transition style and speed.
            auto segments=[&](int index,const char* title,const std::array<const char*,3>& options,int selected,const char* hint){
                int row=y+motion_row(index),dx=28;move_element(uint32_t(index),dx,row,584,72,settings.focus==index);dx-=28;
                settings_card(r,28+dx,row,584,72,settings.focus==index);label(r,title,48+dx,row+12,22,"ink",220);
                sunken_track(r,motion_segment_x(0)+dx-4,row+4,336,42,13);
                for(int i=0;i<3;i++){int x=motion_segment_x(i)+dx;if(selected==i)raised_pill(r,x,row+8,104,34,10,theme.at("accent"),0,false);label(r,options[i],x+52-label_width(options[i],17)/2,centred_label_y(locale.text(options[i]),17,row+25),17,selected==i?"on_accent":"ink",96);}
                label(r,hint,48+dx,row+47,15,"muted",540);
            };
            const int style=preferences.view_style==ViewStyle::Push?1:preferences.view_style==ViewStyle::Zoom?2:0;
            segments(2,"Transition style",{"Spring","Push","Zoom"},style,style==1?"Slide into pages; slide back to return":style==2?"Pages zoom and fade in, like the DSi menu":"Springy menu changes with a colourful wobble");
            segments(3,"Speed",{"Snappy","Normal","Relaxed"},preferences.view_speed,preferences.view_speed==0?"Quickest transitions, about 0.1 s":preferences.view_speed==2?"Slower, calmer transitions":"Balanced for everyday use");
            return;
        }
        if(settings.page==Page::Microphone){microphone_screen(r,y);return;}
        if(settings.page==Page::Sound || settings.page==Page::Display){
            bool sound=settings.page==Page::Sound,available=sound?hardware.volume:hardware.brightness;int value=sound?hardware.volume_percent:system_preferences.brightness;
            settings_header(r,y,sound?"Sound":"Screen brightness");
            const auto band=slider_band();const int card_top=band.first,card_height=band.second-band.first,track=card_top+(sound?50:52);
            // Title and value inside the card, the slider below them.
            settings_card(r,28,y+card_top,584,card_height);
            ui_icon(r,sound?SettingsIcon::Volume:SettingsIcon::Sun,58,y+card_top+22,22,"accent");label(r,sound?"Volume":"Both screens",82,y+card_top+8,22,"ink",300);
            {const std::string shown=available?std::to_string(value)+"%":locale.text("Unavailable");raw_label(r,shown,584-label_width(shown,available?26:18,false),y+card_top+(available?5:10),available?26:18,"accent",200);}
            sunken_track(r,54,y+track-2,532,14,7);
            int position=56+(sound?value*528/100:(value-5)*528/95);
            if(position-56>8)glossy_pill(r,56,y+track,std::max(10,position-56),10,5,theme.at("accent"));
            raised_pill(r,std::clamp(position-17,39,567),y+track-13,34,36,12,theme.at(available?"accent":"muted"),(volume_drag || brightness_drag)?.6:0);
            ui_icon(r,SettingsIcon::Sliders,std::clamp(position,56,584),y+track+5+((volume_drag || brightness_drag)?1:0),16,"on_accent");
            label(r,sound?"Mute":"5%",56,y+track+22,14,"muted",100);label(r,"100%",545,y+track+22,14,"muted",60);
            if(sound){
                for(int i=0;i<3;i++){
                    const int row=y+sound_row(i);const bool focused=settings.focus==i,pressed=pointer_down && contact_y>=sound_row(i) && contact_y<sound_row(i)+66 && contact_x>=28 && contact_x<612;
                    settings_card(r,28,row+(pressed?2:0),584,66,focused);
                    round_rect(r,42,row+11+(pressed?2:0),44,44,12,focused?"accent":"slot");
                    ui_icon(r,i==2?SettingsIcon::Spark:SettingsIcon::Volume,64,row+33+(pressed?2:0),25,focused?"on_accent":"accent");
                    label(r,i==2?"Sounds & music":i==1?"Test microphone":value==0?"Unmute":"Mute",102,row+19+(pressed?2:0),24,"ink",380);
                    if(i==2){const std::string state=preferences.menu_sounds || preferences.menu_music?"On":"Off";const int w=label_width(state,18)+42;round_rect(r,578-w,row+18,w,30,10,focused?"panel":"slot");label(r,state,588-w,row+23,18,"accent",w-34);}
                    ui_icon(r,SettingsIcon::Chevron,574,row+33,14,focused?"accent":"muted");
                }
            }return;
        }
        if(settings.page==Page::Confirm){settings_header(r,y,"Confirm");std::string prompt=confirmation=="startup-rgds"?"Start RG DS automatically on future boots?":confirmation=="reboot"?"Restart the device?":confirmation=="return-stock"?(o.independent?"Restart the RGDS menu?":"Return to the stock menu?"):confirmation=="wifi-off"?"Turn Wi-Fi off?":confirmation=="poweroff"?"Power off your handheld?":confirmation=="update-apply"?"Restart and install the update?":confirmation=="rollback"?"Use the previous RGDS build?":"Clear temporary game files?";settings_card(r,28,y+105,584,209);ui_icon(r,SettingsIcon::Info,58,y+136,24,"accent");label(r,"REVIEW YOUR CHOICE",88,y+125,13,"muted",495);wrapped(r,prompt,52,y+170,28,536,3);choice(r,28,y+365,"Cancel",settings.focus==0,true);choice(r,332,y+365,"Continue",settings.focus==1,true);return;}
        settings_header(r,y,settings.page==Page::MenuSounds?"Sounds & music":sections[settings.section].title);
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
    // The clock as the user chose it: 24-hour, or 12-hour with AM/PM.
    std::string time_text(const std::tm& t)const{
        char buf[16];
        if(system_preferences.time_format==1){std::snprintf(buf,sizeof(buf),"%d:%02d %s",t.tm_hour%12==0?12:t.tm_hour%12,t.tm_min,t.tm_hour<12?"AM":"PM");return buf;}
        std::snprintf(buf,sizeof(buf),"%02d:%02d",t.tm_hour,t.tm_min);return buf;
    }
    std::string clock() {auto now=std::time(nullptr);return time_text(*std::localtime(&now));}
    fs::path folder_for_game(const Game& game){return library_cache.folder(game);}
    fs::path item_folder(size_t index) {return folder_for_game(model.games[index]);}
    std::string game_title(size_t index) {
        auto& game=model.games[index];if(game.demo)return o.game_menu?locale.text(game.title):game.title;
        auto& entry=library_cache.metadata(game);
        if(!entry.title_ready){entry.title=entry.info["title"];if(entry.title.empty())entry.title=entry.info["artwork_title"];if(entry.title.empty())entry.title=library_title(game.title);if(entry.title.empty())entry.title=entry.banner["title"];if(entry.title.empty())entry.title=media.banner(game.path).title;entry.title_ready=true;}
        return entry.title;
    }
    void warm_icon(SDL_Renderer* r,const Game& game){if(!game.demo && !media.warm(r,game.path,true))media.warm(r,folder_for_game(game)/"icon.bmp");}
    void warm_icon(SDL_Renderer* r,size_t index){warm_icon(r,model.games[index]);}
    // The same icon, read and decoded by the loader thread; it appears on the
    // frame after it arrives, so browsing never waits on the card.
    void request_icon(SDL_Renderer* r,const Game& game){if(game.demo)return;media.request(r,game.path,true);media.request(r,folder_for_game(game)/"icon.bmp");}
    void prepare_render_assets() {
        if(settings.page==Page::GameSwitcher)for(auto& panel:surfaces){for(auto& game:switch_games)warm_icon(panel.renderer,game);if(settings.focus<int(switch_games.size()))media.warm(panel.renderer,folder_for_game(switch_games[settings.focus])/"box.bmp");}
        // The focused cover is ready for the very first Games frame. Keep it
        // warm while on Home too; neighbours are prepared after presentation.
        if(!o.game_menu && settings.page==Page::Library && !model.games.empty())for(auto& panel:surfaces){
            media.warm(panel.renderer,item_folder(model.selected)/"box.bmp");warm_icon(panel.renderer,model.selected);
        }
        if(home_menu && home_selected>0 && home_selected<pictochat_tile){const auto& last=summary.groups[home_selected>1?home_selected-1:0].last_game;if(last)for(auto& panel:surfaces)warm_icon(panel.renderer,*last);}
        if(home_menu && preferences.home_layout==1 && summary.groups[0].last_game)for(auto& panel:surfaces)warm_icon(panel.renderer,*summary.groups[0].last_game);
        if(settings.page==Page::SaveGallery){auto path=slots["image_"+std::to_string(settings.focus)];if(!path.empty())for(auto& panel:surfaces)media.warm(panel.renderer,path);}
        for(auto& panel:surfaces){for(const char* name:{"brand/mark-48.bmp","brand/mark-120.bmp"})media.warm(panel.renderer,asset_path(name));media.warm(panel.renderer,asset_path(preferences.dark?"brand/status-dark.bmp":"brand/status-light.bmp"));media.warm(panel.renderer,asset_path(preferences.dark?"brand/wordmark-dark.bmp":"brand/wordmark-light.bmp"));}
        for(auto& panel:surfaces){media.warm(panel.renderer,status_asset(sound_asset()));media.warm(panel.renderer,status_asset(wifi_asset()));}
        if(preferences.rich_icons && settings.page!=Page::BootWarning)for(auto& panel:surfaces)for(const char* name:{"settings","games","favorites","recent","pictochat","display","power","library","sound","wifi","system","controls","shutdown","play","save","load","shaders","brightness","restart","home","artwork"}){
            if(settings.page==Page::GameSwitcher && std::string(name)!="settings" && std::string(name)!="games" && std::string(name)!="recent")continue;
            if(settings.page==Page::Library && std::string(name)!="settings" && std::string(name)!="games" && std::string(name)!="favorites" && std::string(name)!="recent" && std::string(name)!="pictochat" && !(picker_open && (picker_title=="Library view" || picker_title=="Browser layout") && std::string(name)=="artwork"))continue;
            media.warm(panel.renderer,asset_path(std::string("icons/")+name+".bmp"));
        }
        // Statistics uses game identities from the summary, independent of
        // the current library filter. Prepare only the bottom-screen icons.
        if(settings.page==Page::Statistics && summary_ready && !surfaces.empty())for(const auto& entry:summary.most_played)warm_icon(surfaces.back().renderer,entry.game);
        carousel.item_spacing=!home_menu && preferences.layout==1?158:Carousel::spacing;
        if(settings.page==Page::ShaderPicker && !surfaces.empty())shader_preview.warm(surfaces.front().renderer,shader_preview_folder(),shader_preview_scene,preview_shader(),true);
        if(probe || model.games.empty() || home_menu)return;
        // QuickMenu displays its own identity and controls. Defer ROM/banner
        // metadata until a page actually needs the game title.
        if(o.game_menu){game_title(0);for(auto& panel:surfaces)media.warm(panel.renderer,o.data/"session/menu-background.bmp");return;}
        game_title(model.selected);
        if(o.game_menu || settings.page!=Page::Library)return;
        library_cache.stats(model.games[model.selected]);
        browser_list.clamp(model.games.size());
        int first=preferences.layout==2?browser_list.first():int(std::floor(carousel.position))-3;
        int end=preferences.layout==2?browser_list.last(model.games.size()):first+7;
        // List metadata and icon fallbacks are ready before either draw pass.
        if(preferences.layout==2)for(int index=std::max(0,first);index<std::min(end,int(model.games.size()));index++)game_title(size_t(index));
        // Favourite hearts: the visible cartridges' options are read here, never while drawing.
        for(int index=std::max(0,first);index<std::min(end,int(model.games.size()));index++)if(!model.games[size_t(index)].demo)library_cache.options(model.games[size_t(index)]);
        if(!model.games[model.selected].demo)library_cache.options(model.games[model.selected]);
        for(size_t panel=0;panel<surfaces.size();panel++){
            auto* renderer=surfaces[panel].renderer;
            // The selected cartridge is prepared in place so its cover and
            // title never pop in; the visible neighbours and a wide window
            // beyond them load on the loader thread, nearest first.
            media.warm(renderer,item_folder(model.selected)/"box.bmp");warm_icon(renderer,model.selected);
            if(!o.preview && panel==0)continue;
            (void)first;(void)end;
            const int reach=preferences.layout==2?14:10,centre=int(model.selected),count=int(model.games.size());
            for(int distance=1;distance<=reach;distance++)for(int direction:{1,-1}){int index=centre+direction*distance;if(index>=0 && index<count)request_icon(renderer,model.games[size_t(index)]);}
        }
    }
    // Covers near the selection load in the background, nearest first; cached
    // misses are remembered too, so missing artwork is never asked for twice.
    void prepare_next_cover(){
        if(o.game_menu || home_menu || settings.page!=Page::Library || preferences.layout!=1 || model.games.empty())return;
        int center=int(std::lround(carousel.position));
        for(int distance=1;distance<=5;distance++)for(int direction:{-1,1}){
            int index=center+direction*distance;if(index<0 || index>=int(model.games.size()))continue;
            media.request(surfaces.back().renderer,item_folder(size_t(index))/"box.bmp");
        }
    }
    void watch_library_assets(Uint32 now) {
        if(now-library_poll<1000)return;library_poll=now;
        bool changed=!home_menu && !model.games.empty() && library_cache.refresh(model.games[model.selected]);
        if(media.refresh()){library_cache.invalidate_titles();changed=true;}
        if(changed)dirty=true;
    }
    void pattern(SDL_Renderer* r,int y,int height) {
        auto bg=theme.at("background"),fg=theme.at("line");
        if(preferences.background==1){
            // The DSi's graph paper: faint rules every 4 px, a light grid every
            // 16 px and a firmer one every 64 px, each major line with a pale
            // edge under it so the paper reads as embossed rather than drawn on.
            auto shade=[&](int parts){return Color{(unsigned char)((bg.r*(parts-1)+fg.r)/parts),(unsigned char)((bg.g*(parts-1)+fg.g)/parts),(unsigned char)((bg.b*(parts-1)+fg.b)/parts)};};
            const Color fine=shade(22),minor=shade(9),major=shade(4),edge=blend(bg,preferences.dark?Color{255,255,255}:Color{255,255,255},preferences.dark?5:60);
            tint_colour(r,fine);for(int row=y;row<y+height;row+=4)if(row%16)s.RenderDrawLine(r,0,row,639,row);
            tint_colour(r,minor);for(int row=y+(16-y%16)%16;row<y+height;row+=16)if(row%64)s.RenderDrawLine(r,0,row,639,row);
            for(int column=16;column<640;column+=16)if(column%64)s.RenderDrawLine(r,column,y,column,y+height-1);
            for(int row=y+(64-y%64)%64;row<y+height;row+=64){tint_colour(r,major);s.RenderDrawLine(r,0,row,639,row);if(row+1<y+height){tint_colour(r,edge);s.RenderDrawLine(r,0,row+1,639,row+1);}}
            for(int column=0;column<640;column+=64){tint_colour(r,major);s.RenderDrawLine(r,column,y,column,y+height-1);tint_colour(r,edge);s.RenderDrawLine(r,column+1,y,column+1,y+height-1);}
            return;
        }
        s.SetRenderDrawColor(r,(bg.r*19+fg.r)/20,(bg.g*19+fg.g)/20,(bg.b*19+fg.b)/20,255);
        for(int row=y;row<y+height;row+=4)s.RenderDrawLine(r,0,row,639,row);
    }
    void game_icon(SDL_Renderer* r,const Game& game,int x,int y,int size,size_t index=0) {
        SDL_Rect box{x,y,size,size};
        if(!game.demo && (media.draw(r,game.path,box,true) || media.draw(r,folder_for_game(game)/"icon.bmp",box)))return;
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
    void game_icon(SDL_Renderer* r,size_t index,int x,int y,int size){game_icon(r,model.games[index],x,y,size,index);}
    bool cover_art(SDL_Renderer* r,const fs::path& path,SDL_Rect box,bool selected){
        box=media.fitted_box(r,path,box);
        round_rect(r,box.x,box.y+3,box.w+6,box.h+6,10,"slot");
        round_rect(r,box.x-3,box.y-3,box.w+6,box.h+6,10,selected?"accent":"line");
        round_rect(r,box.x,box.y,box.w,box.h,7,"panel");
        return media.draw(r,path,box,false,255,false,7);
    }
    void wrapped(SDL_Renderer* r,const std::string& source,int x,int y,int pixels,int width,int rows=3,bool translate=true) {
        auto value=translate?locale.text(source):source;
        std::istringstream words(value);std::string word,line;int row=0;
        auto measure=[&](const std::string& str){return preferences.pixel?float(RoundedFont::codepoints(str).size()*std::max(2,(pixels+6)/10)*6):rounded.width(str,pixels);};
        while(words>>word) {
            std::string candidate=line.empty()?word:line+" "+word;
            if(!line.empty() && measure(candidate)>width){label(r,line,x,y+row*(pixels+7),pixels,"ink",width,false);if(++row>=rows)return;line=word;}else line=candidate;
        }
        if(!line.empty() && row<rows)label(r,line,x,y+row*(pixels+7),pixels,"ink",width,false);
    }
    fs::path asset_path(const std::string& name)const{return (o.app.empty()?fs::path("."):o.app)/"assets"/name;}
    // The tile is drawn from the bitmap rendered at the nearest size above.
    static const char* mark_asset(int size){return size<=24?"brand/mark-24.bmp":size<=48?"brand/mark-48.bmp":size<=120?"brand/mark-120.bmp":"brand/mark-144.bmp";}
    // Powering off or restarting: both screens fade to the background, the
    // mark sits on the upper one inside a turning ring, and nothing accepts
    // input until the system takes over. A failed request lifts the veil.
    Uint32 shutdown_at=0;std::string shutdown_kind;
    bool shutting_down()const{return shutdown_at!=0;}
    void begin_shutdown(const std::string& kind){shutdown_at=s.GetTicks();shutdown_kind=kind;cancel_navigation();picker_open=false;micro_until=shutdown_at+600;dirty=true;}
    void shutdown_overlay(SDL_Renderer* r,int y,bool upper){
        if(!shutting_down())return;
        const Uint32 now=s.GetTicks();const double t=preferences.animations?std::min(1.0,(now-shutdown_at)/420.0):1.0,ease=1-(1-t)*(1-t);
        const Color bg=theme.at("background");
        s.SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);s.SetRenderDrawColor(r,bg.r,bg.g,bg.b,Uint8(std::lround(255*ease)));SDL_Rect veil{0,y,640,480};s.RenderFillRect(r,&veil);s.SetRenderDrawBlendMode(r,SDL_BLENDMODE_NONE);
        if(ease<.6)return;
        const int alpha=int(std::lround(255*std::min(1.0,(ease-.6)/.4)));
        if(upper){
            const int cx=320,cy=y+206;
            alpha_round(r,cx-70,cy-66,140,140,34,shadow_colour(),int((preferences.dark?60:18)*alpha/255));
            round_rect(r,cx-70,cy-70,140,140,34,"panel");brand(r,cx-48,cy-48,96);
            const double spin=preferences.animations?(now%1400)/1400.0:0;
            feedback_arc(r,cx,cy,86,0,1,theme.at("slot"),alpha,5);feedback_arc(r,cx,cy,86,spin,.28,theme.at("accent"),alpha,5);
            const std::string text=locale.text(shutdown_kind=="reboot"?"Restarting…":"Powering off…");
            raw_label(r,text,320-label_width(text,26,false)/2,y+318,26,"ink",400);
            const std::string note=locale.text("Your saves are safe. See you soon!");
            raw_label(r,note,320-label_width(note,16,false)/2,y+358,16,"muted",500);
        }else{
            const double spin=preferences.animations?(now%1400)/1400.0:0;
            feedback_arc(r,320,y+240,26,spin,.3,theme.at("accent"),alpha,4);
        }
    }
    void brand(SDL_Renderer* r,int x,int y,int size){if(!media.draw(r,asset_path(mark_asset(size)),{x,y,size,size},false))ui_icon(r,SettingsIcon::Display,x+size/2,y+size/2,size,"accent");}
    bool boot_animating(Uint32)const{return o.boot_startup && preferences.animations && (settings.page==Page::BootWarning || !library_ready);}
    double boot_phase()const{return preferences.animations?((s.GetTicks()-boot_scene_at)%2400)/2400.0:0;}
    void boot_caption(SDL_Renderer* r,const std::string& value,int y,int pixels,const std::string& key){
        const int width=std::min(576,label_width(value,pixels));label(r,value,320-width/2,y,pixels,key,576);
    }
    void boot_emblem(SDL_Renderer* r,int y){
        // The identity tile settles in once (a short rise and fade), then
        // rests; a soft floor shadow breathes slowly while the library loads.
        const double t=preferences.animations?std::min(1.0,(s.GetTicks()-boot_scene_at)/420.0):1.0;
        const double ease=1-std::pow(1-t,3);
        const int lift=int(std::lround(6*(1-ease)));
        const double breath=preferences.animations?(1+std::cos(boot_phase()*6.283185307))*.5:.5;
        // A soft floor shadow (always darker than the page, in both modes).
        const Color shade{0,0,0};
        for(int i=0;i<6;i++){
            int half=44-i*6+int(4*breath);
            tint_rect(r,320-half,y+128+i,half*2,1,blend(theme.at("background"),shade,(preferences.dark?5:7)-i));
        }
        // Visible from frame one (the identity never pops in from nothing).
        if(!media.draw(r,asset_path("brand/mark-120.bmp"),{260,y+lift,120,120},false,static_cast<unsigned char>(std::lround(255*(.55+.45*ease)))))
            ui_icon(r,SettingsIcon::Display,320,y+60,96,"accent");
    }
    void boot_wordmark(SDL_Renderer* r,int y,int){
        // Drawn at its rendered size (289 x 48): no scaling, crisp edges.
        if(!media.draw(r,asset_path(preferences.dark?"brand/wordmark-dark.bmp":"brand/wordmark-light.bmp"),{176,y,289,48},false))
            boot_caption(r,"RG DS Core",y+8,28,"ink");
    }
    void boot_scene(SDL_Renderer* r,int y){
        rect(r,0,y,640,480,"background");
        boot_emblem(r,y+112);boot_wordmark(r,y+268,356);
        // Five soft round dots, a slow wave of light passing through them.
        for(int i=0;i<5;i++){
            double wave=preferences.animations?(1+std::cos(boot_phase()*6.283185307-i*.65))*.5:.4;
            const Color c=blend(theme.at("background"),theme.at("accent"),int(22+68*wave));
            const int cx=288+i*16,cy=y+368;
            for(int dy=-4;dy<=4;dy++){int half=int(std::sqrt(std::max(0.0,16.5-dy*dy)));tint_rect(r,cx-half,cy+dy,half*2+1,1,c);}
        }
        boot_caption(r,"Preparing your library…",y+403,16,"muted");
    }
    void boot_top(SDL_Renderer* r,int y){
        rect(r,0,y,640,480,"background");
        // Small, deliberate safety mark and an open text layout, like the
        // original handheld welcome screen rather than a settings dialog.
        Color amber{239,178,65};s.SetRenderDrawColor(r,amber.r,amber.g,amber.b,255);
        for(int row=0;row<30;row++){int half=row*18/30;s.RenderDrawLine(r,320-half,y+88+row,320+half,y+88+row);}
        rect(r,319,y+98,3,10,"background");rect(r,319,y+111,3,3,"background");
        boot_caption(r,"Before you play",y+150,28,"ink");
        rect(r,288,y+207,64,1,"line");
        boot_caption(r,"Play at a comfortable volume.",y+241,20,"ink");
        boot_caption(r,"Give your eyes and hands a break.",y+277,20,"ink");
        boot_caption(r,"Pause when you need to.",y+313,20,"ink");
        boot_caption(r,"RG DS Core",y+415,16,"muted");
    }
    void boot_stylus(SDL_Renderer* r,int x,int y){
        double phase=boot_phase();
        int lift=preferences.animations?int(3*(1-std::cos(phase*6.283185307))):0;
        if(preferences.animations){
            feedback_arc(r,x,y+10,9+int(phase*11),0,1,theme.at("accent"),int(100*(1-phase)),1);
        }else feedback_arc(r,x,y+10,12,0,1,theme.at("line"),255,1);
        // Closed pen silhouette, a pointed nib and an inset highlight.
        color(r,"ink");
        for(int i=0;i<7;i++)s.RenderDrawLine(r,x+4+i,y+3-lift,x+29+i,y-22-lift);
        for(int i=0;i<6;i++)s.RenderDrawLine(r,x+i,y+9-i-lift,x+4+i,y+3-lift);
        color(r,"background");s.RenderDrawLine(r,x+9,y-lift,x+31,y-22-lift);
    }
    void boot_bottom(SDL_Renderer* r,int y){
        rect(r,0,y,640,480,"background");
        boot_emblem(r,y+70);boot_wordmark(r,y+222,332);
        boot_stylus(r,309,y+309);
        boot_caption(r,"Touch to continue",y+344,28,"ink");
        boot_caption(r,"A / Start",y+419,16,"muted");
    }
    // A read-only games card breaks saves, volume and launch at once. Explain
    // the automatic check plainly instead of letting every action fail.
    int card_repair=0;bool card_repair_dismissed=false;Uint32 card_check_at=0,card_repair_at=0;
    void check_card(Uint32 now,bool force=false){
        if(card_repair || card_repair_dismissed || o.preview || o.game_menu || o.data.empty())return;
        if(!force && now-card_check_at<5000)return;
        card_check_at=now;struct statvfs st{};
        if(statvfs(o.data.c_str(),&st)!=0 || !(st.f_flag&ST_RDONLY))return;
        start_card_repair(now);
    }
    void start_card_repair(Uint32 now){
        auto script=(o.app.empty()?fs::current_path():o.app)/"runtime/card_repair.py";
        auto child=fork();
        if(child==0){int null=::open("/dev/null",O_RDWR);if(null>=0){dup2(null,STDIN_FILENO);dup2(null,STDOUT_FILENO);}execlp("python3","python3","-B",script.c_str(),"start","--data",o.data.c_str(),nullptr);_exit(127);}
        int code=-1;if(child>0){int raw=0;if(waitpid(child,&raw,0)==child && WIFEXITED(raw))code=WEXITSTATUS(raw);}
        if(code==0 || code==3){card_repair=code==0?1:2;card_repair_at=now;stop_worker();picker_open=false;hold.clear();dirty=true;}
    }
    void card_top(SDL_Renderer* r,int y){
        rect(r,0,y,640,480,"background");
        brand(r,296,y+96,48);
        boot_caption(r,card_repair==1?"Checking the memory card":"The memory card needs attention",y+178,28,"ink");
        rect(r,288,y+229,64,1,"line");
        if(card_repair==1){
            boot_caption(r,"The card stopped accepting changes, so",y+262,20,"ink");
            boot_caption(r,"RG DS is checking and fixing it now.",y+296,20,"ink");
            boot_caption(r,"Your games and saves stay on the card.",y+330,20,"muted");
        }else{
            boot_caption(r,"The automatic check did not fix it.",y+262,20,"ink");
            boot_caption(r,"Copy your saves to a computer and",y+296,20,"ink");
            boot_caption(r,"check the card there.",y+330,20,"ink");
        }
    }
    void card_bottom(SDL_Renderer* r,int y){
        rect(r,0,y,640,480,"background");
        if(card_repair==1){
            // Calm progress: eight soft dots, one brighter, no spinner churn.
            const double phase=preferences.animations?((s.GetTicks()-card_repair_at)%1600)/1600.0:0;
            for(int i=0;i<8;i++){
                double angle=i*0.785398163-1.570796327;int cx=320+int(std::cos(angle)*30),cy=y+190+int(std::sin(angle)*30);
                int lit=std::abs(i-int(phase*8))%8;int strength=lit==0?100:lit==1 || lit==7?55:22;
                for(int dy=-5;dy<=5;dy++)for(int dx=-5;dx<=5;dx++)if(dx*dx+dy*dy<=25)tint_rect(r,cx+dx,cy+dy,1,1,blend(theme.at("background"),theme.at("accent"),strength));
            }
            boot_caption(r,"RG DS restarts by itself when it is done.",y+268,20,"ink");
            boot_caption(r,"This usually takes about a minute.",y+302,20,"muted");
        }else{
            boot_caption(r,"Games cannot start or save right now.",y+190,20,"ink");
            boot_caption(r,"Press any button to continue",y+404,16,"muted");
        }
    }
    void top(SDL_Renderer* r,int y) {
        if(card_repair){card_top(r,y);return;}
        if(settings.page==Page::BootWarning){boot_top(r,y);return;}
        if(o.boot_startup && !library_ready){boot_scene(r,y);return;}
        if(chat_open){chat_top(r,y);return;}
        if(o.game_menu){
            rect(r,0,y,640,480,"background");
            media.draw(r,o.data/"session/menu-background.bmp",{0,y+48,640,432},false,70);status_bar(r,y);
            if(settings.page!=Page::LeaveGame){settings_top(r,y);if(settings.page!=Page::ShaderPicker && !model.games.empty()){round_rect(r,24,y+320,592,76,14,"panel");raw_wrapped(r,game_title(0),44,y+336,24,552,2);}return;}
            round_rect(r,24,y+366,596,94,10,"panel");
            raw_label(r,model.games.empty()?locale.text("Game menu"):game_title(0),40,y+377,24,"ink",560);
            std::string message=menu_status["message"];
            if(message.empty())message=std::string(core_names[active_engine()])+"  ·  "+(menu_status["live"]=="1"?"Live background":"Paused");
            wrapped(r,message,40,y+416,16,560,2);return;
        }
        rect(r,0,y,640,480,"background");pattern(r,y+48,432);status_bar(r,y);
        if(probe) {
            label(r,"Top display",30,y+96,36,"ink");label(r,top_name,30,y+145,20,"accent");
            for(int i=0;i<7;i++)rect(r,44+i*78,y+210,60,56,i%2?"accent":"ink");
            label(r,"Check panel order and controller input",30,y+340,20,"muted");label(r,"B / Escape returns to stock",30,y+410,20,"ink");return;
        }
        if(settings.page==Page::Keyboard && edit_field==4){search_keyboard_top(r,y);return;}
        if(settings.page!=Page::Library){settings_top(r,y);return;}
        if(home_menu && preferences.home_layout==1){home_column_top(r,y);return;}
        if(home_menu){
            round_rect(r,22,y+82,596,320,10,"line");round_rect(r,24,y+84,592,316,8,"panel");
            if(home_selected==0){category_icon(r,0,282,y+162,76);centered(r,"Settings",y+261,2,"ink");return;}
            if(home_selected==pictochat_tile){pictochat_top(r,y);return;}
            category_icon(r,home_selected,288,y+105,64);centered(r,category_title(home_selected),y+185,2,"ink");
            const auto& stats=summary.groups[home_selected>1?home_selected-1:0];
            std::array<std::string,3> values={summary_ready?std::to_string(stats.games):"…",summary_ready?std::to_string(stats.sessions):"…",summary_ready?std::to_string(stats.seconds/3600)+"h "+std::to_string((stats.seconds/60)%60)+"m":"…"};
            const std::array<std::string,3> captions={"Games","Times played","Time played"};
            for(int i=0;i<3;i++){int x=52+i*182;raw_label(r,values[i],x+(172-label_width(values[i],24))/2,y+239,24,"ink",172);label(r,captions[i],x+(172-label_width(captions[i],16))/2,y+273,16,"muted",172);}
            round_rect(r,70,y+318,500,64,8,"slot");
            if(stats.last_game)game_icon(r,*stats.last_game,84,y+326,48);
            int last_x=stats.last_game?146:88,last_width=552-last_x;
            label(r,"Last played",last_x,y+326,14,"muted",last_width);
            raw_label(r,stats.last_title.empty()?locale.text("Not played yet"):stats.last_title,last_x,y+348,20,"ink",last_width);
            if(!library_ready)centered(r,"Preparing your cartridges…",y+423,1,"muted");else if(return_writes_pending())centered(r,"Restoring settings…",y+423,1,"muted");else if(update_status["state"]=="available" || update_status["prepared"]=="1")centered(r,"Update available in Maintenance",y+423,1,"muted");return;

        }
        if(model.games.empty()) {
            label(r,library_ready && !collection_loading?(discovery_active()?"No matching games":"No games"):"Preparing the library…",30,y+160,28,"ink");return;
        }
        auto item=item_folder(model.selected);auto& info=library_cache.metadata(model.games[model.selected]).info;auto seconds=library_cache.stats(model.games[model.selected]).seconds;
        stage_favourite=game_loved(model.selected);
        if(!cover_stage(r,y,item/"box.bmp"))icon_stage(r,y,info["developer"]);
        cover_caption(r,y,seconds);
        library_hints(r,y);
    }
    // Word-wrapped text centred on the screen, at most `rows` lines.
    void centered_wrapped(SDL_Renderer* r,const std::string& value,int y,int pixels,int width,int rows,const std::string& key){
        std::istringstream words(value);std::string word,line;std::vector<std::string> lines;
        while(words>>word){std::string next=line.empty()?word:line+" "+word;if(!line.empty() && label_width(next,pixels,false)>width){lines.push_back(line);line=word;}else line=next;}
        if(!line.empty())lines.push_back(line);
        for(int i=0;i<std::min<int>(rows,int(lines.size()));i++){int w=std::min(width,label_width(lines[size_t(i)],pixels,false));raw_label(r,lines[size_t(i)],320-w/2,y+i*(pixels+8),pixels,key,width);}
    }
    // PictoChat's page on Home's top screen: what it is and whether Wi-Fi is up.
    void pictochat_top(SDL_Renderer* r,int y){
        category_icon(r,pictochat_tile,272,y+104,96);
        centered(r,"PictoChat",y+216,2,"ink");
        centered(r,"Draw and chat with nearby handhelds",y+262,1,"muted",72);
        const bool on=device_status["wifi_connected"]=="1";
        const std::string chip=on?"Wi-Fi connected":"No Wi-Fi: you can still draw";
        const int width=label_width(chip,18)+48,left=320-width/2;
        round_rect(r,left,y+302,width,38,19,on?"accent":"slot");
        label_colour(r,chip,left+24,y+310,18,on?theme.at("on_accent"):theme.at("muted"),width-30);
        centered(r,"Press A to open",y+423,1,"muted",72);
    }
    // A heart badge for favourites, on the cover stage and on the tiles below.
    bool stage_favourite=false;
    bool game_loved(size_t index){const auto& game=model.games[index];return !game.demo && library_cache.options(game).favourite;}
    void favourite_badge(SDL_Renderer* r,int cx,int cy,int size){
        alpha_round(r,cx-size/2,cy-size/2+2,size,size,size/2,shadow_colour(),preferences.dark?80:36);
        round_rect(r,cx-size/2-2,cy-size/2-2,size+4,size+4,size/2+2,"panel");
        glossy_pill(r,cx-size/2,cy-size/2,size,size,size/2,Color{232,72,96});
        draw_settings_icon(s,r,SettingsIcon::Heart,cx,cy+1,size*11/20,Color{255,255,255});
    }
    // Without a cover: the cartridge's own DS icon, large and crisp on the
    // same card as covers, with its title (the only place it is not repeated).
    void icon_stage(SDL_Renderer* r,int y,const std::string& developer){
        const int size=128,box=size+40,left=320-box/2,top=y+118;
        const Color shade=preferences.dark?Color{0,0,0}:Color{24,40,60};
        for(int layer=4;layer>=1;layer--)alpha_round(r,left-layer*3,top-layer*3+8,box+layer*6,box+layer*6,20+layer*3,shade,preferences.dark?34:16);
        round_rect(r,left-1,top-1,box+2,box+2,21,"line");round_rect(r,left,top,box,box,20,"panel");
        game_icon(r,model.selected,320-size/2,top+20,size);
        if(stage_favourite)favourite_badge(r,left+box-6,top+6,36);
        centered_wrapped(r,game_title(model.selected),y+322,26,560,2,"ink");
        if(!developer.empty()){int w=std::min(560,label_width(developer,18,false));raw_label(r,developer,320-w/2,y+394,18,"muted",560);}
    }
    // Translucent rounded fill without overlapping strips (blending stays even).
    void alpha_round(SDL_Renderer* r,int x,int y,int w,int h,int radius,Color fill,int alpha){
        radius=std::clamp(radius,0,std::min(w,h)/2);
        s.SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);s.SetRenderDrawColor(r,fill.r,fill.g,fill.b,Uint8(std::clamp(alpha,0,255)));
        for(int dy=0;dy<radius;dy++){
            int inset=radius-int(std::sqrt(radius*radius-(radius-dy-0.5)*(radius-dy-0.5)));
            SDL_Rect a{x+inset,y+dy,w-inset*2,1},b{x+inset,y+h-1-dy,w-inset*2,1};s.RenderFillRect(r,&a);s.RenderFillRect(r,&b);
        }
        SDL_Rect middle{x,y+radius,w,h-radius*2};s.RenderFillRect(r,&middle);
    }
    // DSi / GammaOS-style cover stage: the box art alone, large and centred on
    // a soft shadow and a white card edge. The title already sits in the lower
    // screen's speech bubble. A new selection settles in over 190 ms.
    bool cover_stage(SDL_Renderer* r,int y,const fs::path& path){
        if(!media.ready(r,path))return false;
        SDL_Rect box=media.fitted_box(r,path,{100,y+70,440,344});
        // A new cover only settles in with a brief, still fade (no zoom or
        // lift): browsing quickly stays calm on the eyes.
        double t=preferences.animations && cover_motion_at?std::min(1.0,(s.GetTicks()-cover_motion_at)/110.0):1,ease=1-std::pow(1-t,2);
        const Color shade=preferences.dark?Color{0,0,0}:Color{24,40,60};
        for(int layer=4;layer>=1;layer--)alpha_round(r,box.x-6-layer*3,box.y-6-layer*3+8,box.w+12+layer*6,box.h+12+layer*6,14+layer*3,shade,preferences.dark?34:16);
        round_rect(r,box.x-7,box.y-7,box.w+14,box.h+14,15,"line");
        round_rect(r,box.x-6,box.y-6,box.w+12,box.h+12,14,"panel");
        bool drawn=media.draw(r,path,box,false,Uint8(std::lround(255*(.8+.2*ease))),false,8);
        if(stage_favourite)favourite_badge(r,box.x+box.w-4,box.y,36);
        // A faint glass sheen fading down from the top edge, like the DSi's shelves.
        s.SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
        const int sheen=std::max(8,box.h*2/5);
        for(int row=0;row<sheen;row++){
            int inset=row<8?8-int(std::sqrt(64-(8-row-.5)*(8-row-.5))):0;double fade=1-double(row)/sheen;
            s.SetRenderDrawColor(r,255,255,255,Uint8(std::lround(34*fade*fade)));SDL_Rect line{box.x+inset,box.y+row,box.w-2*inset,1};s.RenderFillRect(r,&line);
        }
        return drawn;
    }
    // Under the cover: status first, then play count and time as icon +
    // number (language-neutral). An unplayed game shows nothing.
    // The DSi keeps its bottom screen for touch: View and Options live as
    // quiet button hints along the top screen's lower edge.
    void library_hint(SDL_Renderer* r,int x,int y,const std::string& button,const std::string& title,bool right,bool enabled=true){
        const std::string text=locale.text(title);
        const int h=32,w=10+22+8+label_width(text,16,false)+14,left=right?x-w:x,cy=y+h/2;
        const Color shade=preferences.dark?Color{0,0,0}:Color{24,40,60};
        alpha_round(r,left,y+2,w,h,16,shade,preferences.dark?44:18);
        alpha_round(r,left,y,w,h,16,theme.at("panel"),preferences.dark?218:232);
        // The key badge sits on the outer end, next to the screen's corner.
        const int badge=right?left+w-27:left+5,tx=right?left+12:left+35;
        round_rect(r,badge,y+5,22,22,11,enabled?"accent":"slot");
        if(enabled)alpha_round(r,badge+4,y+7,14,7,3,Color{255,255,255},56);   // the glass highlight of a DSi key
        label(r,button,badge+(22-label_width(button,14,false))/2,centred_label_y(button,14,cy),14,enabled?"on_accent":"muted",22,false);
        raw_label(r,text,tx,centred_label_y(text,16,cy),16,enabled?"ink":"muted",w-40);
    }
    // Y and X in the lower corners, clear of the cover and its caption.
    void library_hints(SDL_Renderer* r,int y){
        if(home_menu || settings.page!=Page::Library || picker_open || launching)return;
        library_hint(r,10,y+439,"Y","View",false);
        library_hint(r,630,y+439,"X","Options",true,!model.games.empty());
    }
    void cover_caption(SDL_Renderer* r,int y,uint64_t seconds){
        std::string notice;
        if(return_writes_pending())notice="Restoring settings…";
        else if(o.boot_startup && (!boot_hardware_ready || system.volume_pending()))notice="Preparing controls…";
        else if(status.find("ERROR")!=std::string::npos)notice=status;
        if(!notice.empty()){int width=std::min(400,label_width(notice,16));label(r,notice,320-width/2,centred_label_y(locale.text(notice),16,y+455),16,"muted",400);return;}
        uint64_t sessions=library_cache.stats(model.games[model.selected]).sessions;
        if(!sessions && !seconds)return;
        const std::string plays=std::to_string(sessions),time=std::to_string(seconds/3600)+"h "+std::to_string((seconds/60)%60)+"m";
        int a=label_width(plays,16,false),b=label_width(time,16,false),total=24+a+28+24+b+28,x=320-total/2;
        const int cy=y+455;
        round_rect(r,x,cy-16,total,32,16,"panel");
        ui_icon(r,SettingsIcon::Play,x+20,cy,14,"accent");raw_label(r,plays,x+34,centred_label_y(plays,16,cy),16,"ink",a+4);
        x+=34+a+18;ui_icon(r,SettingsIcon::Clock,x+10,cy,14,"accent");raw_label(r,time,x+24,centred_label_y(time,16,cy),16,"ink",b+4);
    }
    // Fixed geometry, no textures or per-frame allocations for feedback rings.
    void feedback_arc(SDL_Renderer* r,int x,int y,int radius,double start,double amount,Color ink,int alpha=255,int weight=2) {
        static const auto points=[](){std::array<std::pair<double,double>,64> result{};for(size_t i=0;i<result.size();i++){double a=(double(i)/64-.25)*6.283185307;result[i]={std::cos(a),std::sin(a)};}return result;}();
        s.SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);s.SetRenderDrawColor(r,ink.r,ink.g,ink.b,Uint8(std::clamp(alpha,0,255)));
        int first=int(start*64),count=std::clamp(int(std::ceil(amount*64)),0,64);
        for(int i=0;i<count;i++){auto a=points[(first+i)%64],b=points[(first+i+1)%64];for(int w=0;w<weight;w++)s.RenderDrawLine(r,x+int(std::round(a.first*(radius+w))),y+int(std::round(a.second*(radius+w))),x+int(std::round(b.first*(radius+w))),y+int(std::round(b.second*(radius+w))));}
    }
    bool options_hold() const {
        return pointer_down && !probe && !home_menu && settings.page==Page::Library && carousel.dragging && !carousel.moved && !carousel.range && !model.games.empty() && pressed_game==model.selected && carousel.tapped(down_x,model.games.size())==pressed_game && std::abs(carousel.position-pressed_game)<.08;
    }
    bool touch_animating(Uint32 now) const {
        return preferences.touch_indicator && preferences.animations && ((pointer_down && (now-pointer_at<100 || (options_hold() && now-pointer_at<600))) || (touch_tail && now-release_at<140));
    }
    void touch_feedback(SDL_Renderer* r,int y) {
        gesture_cues(r,y);
        if(!preferences.touch_indicator || probe || launching || chat_open || (!pointer_down && !touch_tail))return;
        auto now=s.GetTicks();double release=pointer_down?0:std::min(1.0,(now-release_at)/140.0);
        if(release>=1)return;
        int radius=preferences.animations?(pointer_down?18+int(4*std::max(0.0,1.0-(now-pointer_at)/100.0)):18+int(release*6)):18;
        int alpha=int(220*(1-release));SDL_Rect clip{0,y,640,480};s.RenderSetClipRect(r,&clip);
        feedback_arc(r,contact_x,y+contact_y,radius+2,0,1,theme.at("panel"),alpha,2);
        feedback_arc(r,contact_x,y+contact_y,radius,0,1,theme.at("accent"),alpha,2);
        if(options_hold()){
            double progress=std::min(1.0,(now-pointer_at)/600.0);
            feedback_arc(r,contact_x,y+contact_y,radius+5,0,preferences.animations?progress:progress>=1?1:0,theme.at("accent"),255,3);
            if(progress>=1)centered(r,"Release for options",y+361,1,"accent");
        }
        s.RenderSetClipRect(r,nullptr);
    }
    double launch_opacity(Uint32 now) const {
        if(!preferences.animations)return launch_phase==LaunchPhase::Exiting?0:1;
        if(launch_phase==LaunchPhase::Exiting){double t=std::min(1.0,(now-launch_exit_at)/80.0);return launch_exit_opacity*(1-t*t);}
        double t=std::min(1.0,(now-launch_at)/110.0);return 1-std::pow(1-t,3);
    }
    void loading_overlay(SDL_Renderer* r,int y,bool lower) {
        if(!launching)return;
        auto now=s.GetTicks();double opacity=launch_opacity(now);
        SDL_Rect panel{0,y,640,480};s.SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
        auto background=theme.at("background");
        double wash=launch_phase==LaunchPhase::Exiting && launch_handoff?launch_exit_opacity:opacity;
        s.SetRenderDrawColor(r,background.r,background.g,background.b,Uint8((lower?224:172)*wash));s.RenderFillRect(r,&panel);
        if(lower && opacity>0){
            // Eight small DS-style blocks: no card, labels, textures or artwork.
            static const std::array<std::pair<int,int>,8> offsets={{{0,-23},{16,-16},{23,0},{16,16},{0,23},{-16,16},{-23,0},{-16,-16}}};
            int head=preferences.animations?int((now-launch_at)/75)%8:0;
            auto accent=theme.at("accent");
            for(int i=0;i<8;i++){
                int age=(head-i+8)%8,alpha=preferences.animations?255-age*27:180;
                s.SetRenderDrawColor(r,accent.r,accent.g,accent.b,Uint8(alpha*opacity));
                SDL_Rect block{317+offsets[i].first,y+237+offsets[i].second,6,6};s.RenderFillRect(r,&block);
            }
        }
        if(launch_phase==LaunchPhase::Exiting && launch_handoff){
            // Clear the library into the theme's neutral colour for handoff.
            double t=preferences.animations?std::min(1.0,(now-launch_exit_at)/80.0):1;
            s.SetRenderDrawColor(r,background.r,background.g,background.b,Uint8(255*t*t));s.RenderFillRect(r,&panel);
        }
    }
    struct LibraryHeader {
        SDL_Rect card,back,view,options,title,search,clear;
        int category_x,category_width;
    };
    LibraryHeader library_header_layout() {
        constexpr int margin=8,padding=20,gap=8,height=44;
        SDL_Rect card{margin,12,640-margin*2,118};
        const int left=card.x+padding,right=card.x+card.w-padding,top=card.y+12;
        // View and Options are button hints on the top screen (Y / X), as on
        // the DSi, and search lives on R2 (Q / E on a keyboard). For touch:
        // the first row (collection, count, search glyph) opens search, the
        // title row opens the view sheet, and a long press on the selected
        // game opens its options.
        SDL_Rect options{0,0,0,0};
        // Touch target is a little wider than the shared 44 px back button.
        SDL_Rect back{left,top,back_size+8,height};
        int category_x=back.x+back.w+gap;
        const std::array<std::string,3> views={"Games","Favourites","Recents"};
        int category_width=std::clamp(label_width(views[preferences.view],16),64,124);
        SDL_Rect clear{right-44,top,44,height};
        SDL_Rect search{category_x,top,(discovery_active()?clear.x:right)-category_x,height};
        SDL_Rect title{left,84,right-left,30};
        SDL_Rect view=title;
        return {card,back,view,options,title,search,clear,category_x,category_width};
    }
    static bool point_in(SDL_Rect bounds,int x,int y){return x>=bounds.x && x<bounds.x+bounds.w && y>=bounds.y && y<bounds.y+bounds.h;}
    bool library_control_active(LibraryPress control,SDL_Rect bounds) const {
        if(library_press!=control)return false;
        if(pointer_down)return point_in(bounds,down_x,down_y) && point_in(bounds,contact_x,contact_y);
        return preferences.animations && s.GetTicks()-library_press_at<120;
    }
    bool update_rail_feedback(Uint32 now) {
        const bool available=!probe && !picker_open && !launching && settings.page==Page::Library && (home_menu || preferences.layout!=2) && !(home_menu && preferences.home_layout==1) && navigation_count()>1;
        const std::array<bool,3> held={
            available && pointer_down && library_press==LibraryPress::LeftArrow && point_in(rail_left_bounds(),contact_x,contact_y),
            available && pointer_down && library_press==LibraryPress::RightArrow && point_in({556,409,84,71},contact_x,contact_y),
            available && pointer_down && carousel.dragging && carousel.range};
        bool active=false;
        for(size_t i=0;i<rail_press.size();i++){
            if(rail_press[i].set(held[i],now,preferences.animations))dirty=true;
            active=rail_press[i].animating(now,preferences.animations) || active;
        }
        // Paint the exact resting state before the event loop goes idle.
        if(active!=rail_feedback_active){rail_feedback_active=active;dirty=true;}
        return active;
    }
    // DSi-style glass button: a bright band over the upper half with a crisp
    // lower edge, plain accent below (labels keep their contrast), a little
    // reflected light along the bottom. Pressing dims the gloss.
    void glossy_rail_control(SDL_Renderer* r,int x,int y,int w,int h,int radius,double press,bool enabled) {
        if(!enabled){round_rect(r,x,y,w,h,radius,"line");return;}
        glossy_pill(r,x,y,w,h,radius,theme.at("accent"),press);
    }
    // ---- depth, shared by every control ----
    // The same glass over any colour: a bright band fading down the upper
    // half with a crisp top line, reflected light along the bottom and a
    // darker last row. `press` dims the gloss. No shadow: callers add one.
    void glossy_pill(SDL_Renderer* r,int x,int y,int w,int h,int radius,Color base,double press=0){
        radius=std::clamp(radius,0,std::min(w,h)/2);fill_round(r,x,y,w,h,radius,base);
        const Color white{255,255,255},black{0,0,0};
        auto inset_at=[&](int row){double dy=row<radius?radius-row-.5:row>=h-radius?row-(h-radius)+.5:-1;return dy<0?0:radius-int(std::sqrt(std::max(0.0,radius*radius-dy*dy)));};
        const int band=std::max(3,h*9/20);
        for(int row=1;row<band;row++){int inset=inset_at(row)+2;if(w-2*inset<=0)continue;double fade=1.0-double(row)/band;tint_rect(r,x+inset,y+row,w-2*inset,1,blend(base,white,int((14+30*fade)*(1-.55*press))));}
        {int inset=inset_at(1)+3;if(w-2*inset>0)tint_rect(r,x+inset,y+1,w-2*inset,1,blend(base,white,int(62*(1-.5*press))));}
        for(int row=std::max(1,h-4);row<h-2;row++){int inset=inset_at(row)+2;if(w-2*inset>0)tint_rect(r,x+inset,y+row,w-2*inset,1,blend(base,white,int((row-(h-5))*4*(1-press))));}
        if(w-2*radius>0 && h>3)tint_rect(r,x+radius,y+h-2,w-2*radius,1,blend(base,black,int(16+12*press)));
    }
    Color shadow_colour()const{return preferences.dark?Color{0,0,0}:Color{24,40,60};}
    // A raised control: soft shadow, then the glass body, sunk 2 px when pressed.
    void raised_pill(SDL_Renderer* r,int x,int y,int w,int h,int radius,Color fill,double press=0,bool shadow=true){
        const int sink=int(std::lround(2*press));
        if(shadow)alpha_round(r,x,y+3,w,h,radius,shadow_colour(),int((preferences.dark?70:28)*(1-.5*press)));
        glossy_pill(r,x,y+sink,w,h,radius,fill,press);
    }
    // A footer or sheet button: a raised glass pill (accent when primary) with an
    // optional round key badge ("B", "A") and a label that sinks while pressed.
    void footer_button(SDL_Renderer* r,int x,int y,int w,const std::string& text,const std::string& badge,bool primary,bool pressed,Color* tint=nullptr){
        const Color fill=primary?(tint?*tint:theme.at("accent")):theme.at(pressed?"focus":"panel"),ink=primary?(tint?tile_ink(*tint):theme.at("on_accent")):theme.at("ink");
        raised_pill(r,x,y,w,40,12,fill,pressed?1:0);const int sink=pressed?2:0;
        int tx=x+18;
        if(!badge.empty()){
            if(primary){fill_round(r,x+w-36,y+9+sink,22,22,11,ink);raw_label_colour(r,badge,x+w-36+(22-label_width(badge,14,false))/2,centred_label_y(badge,14,y+20+sink),14,fill,22);}
            else{round_rect(r,x+13,y+9+sink,22,22,11,"slot");raw_label(r,badge,x+13+(22-label_width(badge,14,false))/2,centred_label_y(badge,14,y+20+sink),14,"ink",22);tx=x+48;}
        }
        const std::string value=locale.text(text);
        raw_label_colour(r,value,tx,centred_label_y(value,20,y+20+sink),20,ink,w-(badge.empty()?36:primary?58:60));
    }
    bool footer_pressed(bool left)const{return pointer_down && contact_y>=424 && (left?contact_x<200:contact_x>=440);}
    // A secondary button: a raised panel pill, pressed in while held.
    void secondary_button(SDL_Renderer* r,int x,int y,int w,int h,int radius,bool pressed,bool lit=false){
        raised_pill(r,x,y,w,h,radius,theme.at(lit?"focus":pressed?"focus":"panel"),pressed?1:0);
    }
    // A recessed track: a rim, the background inside and a shadow along its top.
    void sunken_track(SDL_Renderer* r,int x,int y,int w,int h,int radius){
        round_rect(r,x,y,w,h,radius,"line");round_rect(r,x+1,y+1,w-2,h-2,std::max(0,radius-1),"background");
        alpha_round(r,x+2,y+2,w-4,std::min(4,std::max(1,h-4)),std::max(0,radius-2),shadow_colour(),preferences.dark?50:14);
    }
    void tint_colour(SDL_Renderer* r,Color value){s.SetRenderDrawColor(r,value.r,value.g,value.b,255);}
    // The rail's own glass shelf: a soft vertical sheen over the slot colour.
    Color rail_shelf(int row){return row>=26?theme.at("slot"):blend(theme.at("panel"),theme.at("slot"),std::max(0,row)*100/26);}
    // The Home rail after the DSi's scroll bar: a square arrow button at each
    // end with a plain white triangle, a recessed track with one small tick
    // per item, and a white glass grabber that shows the ticks through it with
    // the current one dark. Everything shares one height.
    void draw_rail(SDL_Renderer* r,int y,size_t count,double position,std::array<double,3> press_state,bool bubble){
            rail_bar(r,y);
            constexpr int control=RailGeometry::control,top_edge=RailGeometry::top,height=RailGeometry::height;
            constexpr int rail_left=RailGeometry::left,rail_width=RailGeometry::width;
            const bool dark=preferences.dark,enabled=count>1;
            for(int x:{rail_left-control-16,rail_left+rail_width+16}){
                const bool right=x>320;
                const double press=press_state[right?1:0];
                const int inset=int(std::lround(2*press)),sink=int(std::lround(2*press));
                alpha_round(r,x,y+top_edge+3,control,height,8,shadow_colour(),int((dark?60:26)*(1-.5*press)));
                round_rect(r,x,y+top_edge,control,height,8,"line");
                glossy_rail_control(r,x+inset,y+top_edge+sink+1,control-2*inset,height-2-sink,7,press,enabled);
                // The DSi's solid triangle, pointing outward.
                const int direction=right?1:-1,cx=x+control/2+direction*int(std::lround(2*press)),cy=y+top_edge+height/2+sink;
                for(int pass=0;pass<2;pass++){
                    if(pass==0){if(!enabled)continue;tint_colour(r,blend(theme.at("accent"),Color{0,0,0},30));}else color(r,enabled?"on_accent":"muted");
                    const int shift=1-pass;
                    for(int i=0;i<=12;i++){const int half=i*7/12;s.RenderDrawLine(r,cx+direction*(7-i),cy-half+shift,cx+direction*(7-i),cy+half+shift);}
                }
            }
            const auto rail=RailGeometry::at(count,position);
            const int track_top=y+top_edge+height/2-9;
            sunken_track(r,rail_left,track_top,rail_width,18,6);
            // Ticks: one per item while they fit, else a fixed texture. Those
            // behind the current position are dark, the rest quiet.
            const int travel=rail_width-rail.thumb_width-4,first=rail_left+2+rail.thumb_width/2;
            const double pitch=enabled?double(travel)/double(count-1):0;
            const Color quiet=blend(theme.at("background"),theme.at("muted"),dark?55:45),done=blend(theme.at("muted"),theme.at("ink"),35);
            auto ticks=[&](int from,int to,bool inside){
                SDL_Rect clip{from,track_top,to-from,18};s.RenderSetClipRect(r,&clip);
                if(pitch>=7){for(size_t i=0;i<count;i++){const int tx=first+int(std::lround(i*pitch));tint_rect(r,tx-2,track_top+7,4,4,i<size_t(std::lround(position))?done:quiet);}}
                else for(int tx=first;tx<=first+travel;tx+=8)tint_rect(r,tx-2,track_top+7,4,4,tx<rail.thumb_left+rail.thumb_width/2?done:quiet);
                if(inside && enabled){const int cx=rail.thumb_left+rail.thumb_width/2;tint_rect(r,cx-3,track_top+6,6,6,theme.at("accent"));}
                s.RenderSetClipRect(r,nullptr);
            };
            ticks(rail_left,rail_left+rail_width,false);
            const double press=press_state[2];
            const int lift=int(std::lround(2*press)),grow=int(std::lround(2*press)),top=y+top_edge-lift,grab_height=height+2*lift;
            const int thumb_left=rail.thumb_left-grow,thumb_width=rail.thumb_width+2*grow;
            alpha_round(r,thumb_left,top+3+int(std::lround(2*press)),thumb_width,grab_height,8,shadow_colour(),int(dark?60:26+26*press));
            if(press>0)alpha_round(r,thumb_left-4,top-4,thumb_width+8,grab_height+8,11,theme.at("accent"),int(60*press));
            round_rect(r,thumb_left,top,thumb_width,grab_height,8,enabled?"accent":"line");
            glossy_pill(r,thumb_left+1,top+1,thumb_width-2,grab_height-2,7,dark?theme.at("focus"):Color{255,255,255},press*.6);
            // The track shows through the glass: its rim and the ticks, with the current one marked.
            tint_rect(r,thumb_left+4,track_top,thumb_width-8,1,blend(theme.at("line"),theme.at("panel"),40));
            tint_rect(r,thumb_left+4,track_top+17,thumb_width-8,1,blend(theme.at("line"),theme.at("panel"),40));
            ticks(thumb_left+4,thumb_left+thumb_width-4,true);
            if(press>.05 && bubble && enabled)rail_bubble(r,y,rail.thumb_left+rail.thumb_width/2,press);
    }
    void rail_bar(SDL_Renderer* r,int y){
        rect(r,0,y+407,640,73,"slot");
        const Color top=theme.at("panel");
        for(int row=0;row<26;row++)tint_rect(r,0,y+408+row,640,1,rail_shelf(row));
        rect(r,0,y+407,640,1,"line");tint_rect(r,0,y+408,640,1,blend(top,Color{255,255,255},preferences.dark?6:60));
    }
    // Recessed groove with one pip per item while they stay distinct, drawn
    // only between `from` and `to` (the glass window redraws its part).
    void rail_groove(SDL_Renderer* r,int y,int from,int to,size_t count,const RailGeometry& rail){
        constexpr int rail_left=RailGeometry::left,rail_width=RailGeometry::width;
        SDL_Rect clip{from,y+430,to-from,30};s.RenderSetClipRect(r,&clip);
        round_rect(r,rail_left+4,y+436,rail_width-8,15,7,"line");
        round_rect(r,rail_left+5,y+438,rail_width-10,12,6,"background");
        const int pip_span=rail_width-rail.thumb_width-4;
        const int pitch=count>1?pip_span/int(count-1):0,pip=pitch>=9?6:4;
        if(count>1 && pitch>=5){
            const Color quiet=blend(theme.at("background"),theme.at("muted"),60);
            for(size_t i=0;i<count;i++){
                int cx=rail_left+2+rail.thumb_width/2+int(std::lround(double(i)*pip_span/(count-1)));
                bool inside=cx>=rail.window_left+4 && cx<rail.window_left+rail.window_width-4;
                fill_round(r,cx-pip/2,y+441,pip,6,1,inside?theme.at("muted"):quiet);
            }
        }
        s.RenderSetClipRect(r,nullptr);
    }
    // While the thumb is held: "12 / 62" floats above it, so a long scrub
    // always shows where it will land. Numbers only, no translation needed.
    void rail_bubble(SDL_Renderer* r,int y,int cx,double press){
        const std::string text=std::to_string(model.selected+1)+" / "+std::to_string(model.games.size());
        const int width=label_width(text,18,false)+28,height=34,left=std::clamp(cx-width/2,8,632-width);
        const int top=y+372+int(std::lround((1-press)*8));
        const Color shade=preferences.dark?Color{0,0,0}:Color{24,40,60};
        alpha_round(r,left+1,top+4,width,height,12,shade,int(40*press));
        round_rect(r,left-1,top-1,width+2,height+2,13,"line");round_rect(r,left,top,width,height,12,"panel");
        for(int row=0;row<7;row++)tint_rect(r,cx-row,top+height+row,row*2+1,1,theme.at(row==6?"line":"panel"));
        raw_label(r,text,left+14,top+7,18,"accent",width-20);
    }
    void mark_library_press(int x,int y) {
        library_press=LibraryPress::None;library_press_row=-1;
        if(settings.page!=Page::Library || picker_open)return;
        auto header=library_header_layout();
        if(!home_menu && point_in(header.back,x,y))library_press=LibraryPress::Back;
        else if(!home_menu && point_in(header.view,x,y))library_press=LibraryPress::View;
        else if(point_in(header.options,x,y))library_press=LibraryPress::Options;
        else if(!home_menu && discovery_active() && point_in(header.clear,x,y))library_press=LibraryPress::Clear;
        else if(!home_menu && point_in(header.search,x,y))library_press=LibraryPress::Search;
        else if(preferences.layout==2 && y>=LibraryViewport::top && y<LibraryViewport::top+LibraryViewport::height){
            if(x>=592 && x<628)library_press=LibraryPress::Vertical;
            else if(x>=24 && x<592){library_press_row=browser_list.row_at(y,navigation_count());if(library_press_row>=0)library_press=LibraryPress::Row;}
        }else if((home_menu || preferences.layout!=2) && y>=409 && y<480){
            if(x>=0 && x<RailGeometry::left)library_press=LibraryPress::LeftArrow;
            else if(x>=RailGeometry::left+RailGeometry::width && x<640)library_press=LibraryPress::RightArrow;
            else library_press=LibraryPress::Horizontal;
        }
        if(library_press!=LibraryPress::None){
            library_press_at=s.GetTicks();micro_until=library_press_at+120;
            // Seed on input so a complete tap queued between frames still has
            // release feedback, independent of the optional touch indicator.
            if(!probe && navigation_count()>1){
                int index=library_press==LibraryPress::LeftArrow?0:library_press==LibraryPress::RightArrow?1:library_press==LibraryPress::Horizontal?2:-1;
                if(index>=0)rail_press[size_t(index)].set(true,library_press_at,preferences.animations);
            }
        }
    }
    void library_header_action(SDL_Renderer* r,int y,SDL_Rect bounds,const std::string& title,const std::string& button,bool enabled=true) {
        auto hit=bounds;
        bounds.y+=y+4;bounds.h-=8;
        bool pressed=library_control_active(title=="View"?LibraryPress::View:LibraryPress::Options,hit);
        int offset=pressed?1:0;
        round_rect(r,bounds.x,bounds.y+1,bounds.w,bounds.h,10,"slot");
        round_rect(r,bounds.x+offset,bounds.y+offset,bounds.w,bounds.h,10,pressed?"focus":"panel");
        const int badge_x=bounds.x+10,badge_y=bounds.y+(bounds.h-20)/2;
        round_rect(r,badge_x+offset,badge_y+offset,20,20,10,enabled?(pressed?"accent":"slot"):"slot");
        label(r,button,badge_x+offset+(20-label_width(button,14))/2,badge_y+offset-(preferences.pixel?3:0),14,enabled?(pressed?"on_accent":"accent"):"muted",20);
        label(r,title,badge_x+28+offset,bounds.y+offset+(bounds.h-14)/2-(preferences.pixel?6:3),14,enabled?"ink":"muted",label_width(title,14));
    }
    // ---- the DSi column: the last game on top, the categories as wide tiles ----
    struct HomeTile {SDL_Rect box;size_t index;};
    static std::array<HomeTile,6> home_column_tiles(){
        return {{{{24,20,592,88},last_played_tile},{{24,122,288,92},1},{{328,122,288,92},2},{{24,228,288,92},3},{{328,228,288,92},pictochat_tile},{{24,334,592,64},0}}};
    }
    static constexpr SDL_Rect home_bar_left{12,418,80,54},home_bar_right{548,418,80,54};
    // Up and down walk the rows, left and right the two tiles of a row.
    int home_column_memory=0;   // the column (Games/Recents or Favourites/PictoChat) you were last in
    bool home_column_move(int code){
        static constexpr std::array<std::array<int,2>,4> rows={{{5,5},{1,2},{3,4},{0,0}}};
        int row=0,column=home_column_memory;
        for(int i=0;i<4;i++){if(rows[size_t(i)][0]==int(home_selected) || rows[size_t(i)][1]==int(home_selected)){row=i;if(rows[size_t(i)][0]!=rows[size_t(i)][1])column=rows[size_t(i)][1]==int(home_selected)?1:0;break;}}
        if(code==8 || code==9)row=std::clamp(row+(code==8?-1:1),0,3);
        else if(code==0 || code==1)column=code==0?0:1;
        else return false;
        if(rows[size_t(row)][0]!=rows[size_t(row)][1])home_column_memory=column;
        select_navigation(size_t(rows[size_t(row)][size_t(column)]),false);return true;
    }
    void home_column_touch(int x,int y){
        for(const auto& tile:home_column_tiles())if(point_in(tile.box,x,y)){select_navigation(tile.index,false);open_category();return;}
        if(point_in(home_bar_left,x,y)){settings.section=0;settings.page=Page::Display;settings.focus=0;dirty=true;return;}
        if(point_in(home_bar_right,x,y)){settings.section=7;settings.page=Page::Statistics;settings.focus=0;dirty=true;return;}
    }
    void home_column_bottom(SDL_Renderer* r,int y){
        for(const auto& tile:home_column_tiles()){
            const SDL_Rect b=tile.box;const bool selected=home_selected==tile.index;
            int bx=b.x,by=y+b.y;move_element(uint32_t(tile.index),bx,by,b.w,b.h,selected);
            const bool pressed=pointer_down && point_in(b,contact_x,contact_y) && point_in(b,down_x,down_y);const int sink=pressed?2:0;
            alpha_round(r,bx,by+3,b.w,b.h,12,shadow_colour(),preferences.dark?60:20);
            round_rect(r,bx,by+sink,b.w,b.h,12,"line");
            if(selected)fill_round(r,bx+1,by+1+sink,b.w-2,b.h-2,11,mix_colour(theme.at("panel"),theme.at("accent"),.12));else round_rect(r,bx+1,by+1+sink,b.w-2,b.h-2,11,"panel");
            alpha_round(r,bx+8,by+2+sink,b.w-16,std::min(10,b.h/5),5,Color{255,255,255},preferences.dark?12:120);
            if(tile.index==last_played_tile){
                const auto& group=summary.groups[0];
                if(summary_ready && group.last_game){game_icon(r,*group.last_game,bx+20,by+12+sink,64);label(r,"Last played",bx+104,by+16+sink,14,"muted",460);raw_label(r,group.last_title,bx+104,by+38+sink,24,"ink",460);}
                else{round_rect(r,bx+20,by+12+sink,64,64,8,"slot");ui_icon(r,SettingsIcon::Cartridge,bx+52,by+44+sink,32,"muted");label(r,summary_ready?"Nothing played yet":"Preparing…",bx+104,by+22+sink,22,"muted",460);if(summary_ready)label(r,"Your last game will appear here",bx+104,by+52+sink,14,"muted",460);}
            }else{
                const int icon=tile.index==0?36:48;category_icon(r,tile.index,bx+(tile.index==0?24:18),by+(b.h-icon)/2+sink,icon);
                const std::string title=locale.text(category_title(tile.index));const bool note_row=tile.index!=0;
                raw_label(r,title,bx+(tile.index==0?76:84),centred_label_y(title,22,by+b.h/2+sink-(note_row?9:0)),22,"ink",b.w-100);
                std::string note;
                if(tile.index>=1 && tile.index<=3){const auto& g=summary.groups[tile.index-1];note=summary_ready?locale.text(std::to_string(g.games)+(g.games==1?" game":" games")):"…";}
                else if(tile.index==pictochat_tile)note=locale.text(device_status["wifi_connected"]=="1"?"Wi-Fi connected":"Draw and chat");
                if(!note.empty())raw_label(r,note,bx+84,by+b.h/2+8+sink,14,"muted",b.w-100);
            }
            if(selected)chat_brackets(r,bx,by+sink,b.w,b.h,theme.at("accent"),4);
        }
        // The DSi's bottom bar: brightness on the left, the mark in the middle, the clock (statistics) on the right.
        rail_bar(r,y);
        for(int i=0;i<2;i++){const SDL_Rect b=i?home_bar_right:home_bar_left;const bool pressed=pointer_down && point_in(b,contact_x,contact_y) && point_in(b,down_x,down_y);
            raised_pill(r,b.x,y+b.y,b.w,b.h,12,theme.at(pressed?"focus":"panel"),pressed?1:0);ui_icon(r,i?SettingsIcon::Clock:SettingsIcon::Sun,b.x+b.w/2,y+b.y+b.h/2+(pressed?2:0),26,"accent");}
        brand(r,296,y+421,48);
    }
    // The upper screen of the column: the DSi's clock and calendar.
    void home_column_top(SDL_Renderer* r,int y){
        const auto now=std::time(nullptr);const std::tm t=*std::localtime(&now);
        const int cx=168,cy=y+241,radius=135;   // the card spans y 96..386, like the calendar
        alpha_round(r,cx-radius-10,cy-radius-6,2*radius+20,2*radius+20,22,shadow_colour(),preferences.dark?60:18);
        round_rect(r,cx-radius-10,cy-radius-10,2*radius+20,2*radius+20,22,"line");round_rect(r,cx-radius-9,cy-radius-9,2*radius+18,2*radius+18,21,"panel");
        for(int i=0;i<12;i++){
            const double a=i*3.14159265358979/6;const int mx=cx+int(std::lround(std::sin(a)*(radius-24))),my=cy-int(std::lround(std::cos(a)*(radius-24)));
            if(i%3==0){const std::string n=i==0?"12":std::to_string(i);raw_label(r,n,mx-label_width(n,34,false)/2,centred_label_y(n,34,my),34,"muted",48);}
            else rect(r,mx-3,my-3,6,6,"line");   // the DSi's small square marks
        }
        auto hand=[&](double angle,int length,int weight,Color c){
            s.SetRenderDrawColor(r,c.r,c.g,c.b,255);const int ex=cx+int(std::lround(std::sin(angle)*length)),ey=cy-int(std::lround(std::cos(angle)*length));
            for(int w=-weight/2;w<=weight/2;w++)for(int v=-weight/2;v<=weight/2;v++)s.RenderDrawLine(r,cx+w,cy+v,ex+w,ey+v);
        };
        hand((t.tm_hour%12+t.tm_min/60.0)*3.14159265358979/6,int(radius*.5),4,theme.at("ink"));
        hand((t.tm_min+t.tm_sec/60.0)*3.14159265358979/30,int(radius*.78),3,theme.at("ink"));
        hand(t.tm_sec*3.14159265358979/30,int(radius*.84),2,theme.at("accent"));
        rect(r,cx-4,cy-4,8,8,"ink");rect(r,cx-2,cy-2,4,4,"accent");
        // The calendar: month and year, weekday row, one cell per day, today lit.
        const int left=330,top=y+96,cell=40;
        alpha_round(r,left,top+3,282,290,16,shadow_colour(),preferences.dark?60:18);round_rect(r,left,top,282,290,16,"line");round_rect(r,left+1,top+1,280,288,15,"panel");
        {char month[16];std::snprintf(month,sizeof(month),"%02d/%04d",t.tm_mon+1,t.tm_year+1900);raw_label(r,month,left+141-label_width(month,22,false)/2,top+12,22,"ink",200);}
        static const std::array<const char*,7> days={"Su","Mo","Tu","We","Th","Fr","Sa"};
        const Color sunday=preferences.dark?Color{235,110,110}:Color{200,56,56},saturday=preferences.dark?Color{120,160,240}:Color{56,96,200};
        for(int i=0;i<7;i++){const int x=left+1+i*cell;tint_rect(r,x,top+46,cell,26,i==0?sunday:i==6?saturday:theme.at("slot"));const std::string d=locale.text(days[size_t(i)]);raw_label_colour(r,d,x+cell/2-label_width(d,14,false)/2,centred_label_y(d,14,top+59),14,i==0 || i==6?Color{255,255,255}:theme.at("ink"),cell);}
        std::tm first=t;first.tm_mday=1;first.tm_hour=12;std::mktime(&first);
        std::tm next=t;next.tm_mon+=1;next.tm_mday=0;next.tm_hour=12;std::mktime(&next);
        const int start=first.tm_wday,days_in_month=next.tm_mday,rows=(start+days_in_month+6)/7;
        // Like the DSi: the Sunday column washed pink, Saturday pale blue, a
        // fine grid between the days and today in a dotted frame.
        const Color grid=blend(theme.at("panel"),theme.at("line"),preferences.dark?70:55);
        tint_rect(r,left+1,top+72,cell,rows*36+2,blend(theme.at("panel"),sunday,preferences.dark?14:12));
        tint_rect(r,left+1+6*cell,top+72,cell,rows*36+2,blend(theme.at("panel"),saturday,preferences.dark?14:12));
        for(int i=1;i<7;i++)tint_rect(r,left+1+i*cell,top+46,1,rows*36+28,grid);
        for(int row=0;row<=rows;row++)tint_rect(r,left+1,top+72+row*36,7*cell,1,grid);
        for(int day=1;day<=days_in_month;day++){
            const int slot=start+day-1,col=slot%7,row=slot/7;const int x=left+1+col*cell,ry=top+72+row*36;
            const bool today=day==t.tm_mday;
            if(today){
                fill_round(r,x+3,ry+3,cell-5,31,3,theme.at("panel"));
                const Color mark=theme.at("accent");
                for(int d=0;d<cell-6;d+=3){tint_rect(r,x+3+d,ry+3,2,2,mark);tint_rect(r,x+3+d,ry+32,2,2,mark);}
                for(int d=0;d<31;d+=3){tint_rect(r,x+3,ry+3+d,2,2,mark);tint_rect(r,x+cell-4,ry+3+d,2,2,mark);}
            }
            const std::string n=std::to_string(day);
            raw_label_colour(r,n,x+cell/2-label_width(n,16,false)/2,centred_label_y(n,16,ry+18),16,today?theme.at("accent"):col==0?sunday:col==6?saturday:theme.at("ink"),cell);
        }
        // What the selected tile holds.
        std::string line;
        if(home_selected==last_played_tile){const auto& g=summary.groups[0];line=summary_ready && g.last_game?locale.text("Last played")+"  ·  "+g.last_title:locale.text("Nothing played yet");}
        else if(home_selected>=1 && home_selected<=3){const auto& g=summary.groups[home_selected-1];line=locale.text(category_title(home_selected))+(summary_ready?"  ·  "+locale.text(std::to_string(g.games)+(g.games==1?" game":" games"))+"  ·  "+std::to_string(g.seconds/3600)+"h "+std::to_string((g.seconds/60)%60)+"m":"");}
        else line=locale.text(home_selected==pictochat_tile?"Draw and chat with nearby handhelds":"Settings");
        if(!library_ready)line=locale.text("Preparing your cartridges…");else if(update_status["state"]=="available" || update_status["prepared"]=="1")line=locale.text("Update available in Maintenance");
        {const int w=std::min(600,label_width(line,16,false));raw_label(r,line,320-w/2,y+432,16,"muted",600);}
    }
    void bottom(SDL_Renderer* r,int y) {
        if(card_repair){card_bottom(r,y);return;}
        if(settings.page==Page::BootWarning){boot_bottom(r,y);return;}
        if(o.boot_startup && !library_ready){boot_scene(r,y);return;}
        rect(r,0,y,640,480,"background");
        if(!probe && settings.page!=Page::Library){settings_bottom(r,y);if(!(o.game_menu && menu_status["state"]=="busy"))settings_footer(r,y);if(picker_open || picker_closing())render_picker(r,y);return;}
        pattern(r,y,480);

        if(probe) {
            label(r,"Bottom display",28,y+29,24,"ink",440);label(r,bottom_name,28,y+85,16,"muted",480);
            for(int row=0;row<4;row++)for(int col=0;col<8;col++)rect(r,32+col*72,y+146+row*46,66,40,(col+row)%2?"slot":"panel");
            if(touch_x>=0){color(r,"accent");s.RenderDrawLine(r,touch_x-20,y+touch_y,touch_x+20,y+touch_y);s.RenderDrawLine(r,touch_x,y+touch_y-20,touch_x,y+touch_y+20);}
            label(r,last_input,30,y+355,20,"ink");label(r,"Home / F1 switches probe",30,y+410,16,"muted");return;
        }
        if(home_menu && preferences.home_layout==1){home_column_bottom(r,y);if(picker_open || picker_closing())render_picker(r,y);return;}
        auto header=library_header_layout();auto card=header.card;
        round_rect(r,card.x,y+card.y+3,card.w,card.h,14,"slot");
        round_rect(r,card.x,y+card.y,card.w,card.h,14,"line");
        round_rect(r,card.x+1,y+card.y+1,card.w-2,card.h-2,13,"panel");
        triangle(r,320,y+128,17,19,"line");triangle(r,320,y+126,15,19,"panel");
        if(home_menu)centered(r,category_title(home_selected),y+56,3,"ink");
        else{
            const std::array<std::string,3> views={"Games","Favourites","Recents"};
            auto back=header.back;
            bool back_pressed=library_control_active(LibraryPress::Back,back);
            back_button(r,back.x,y+back.y,back_pressed);
            // One quiet row: the collection (or the active search), the
            // count and a search glyph. The row is the touch target for search.
            auto search=header.search;bool pressed=library_control_active(LibraryPress::Search,search);
            if(pressed)round_rect(r,search.x-6,y+search.y+4,search.w+12,search.h-8,10,"focus");
            const int row_cy=y+back.y+back.h/2;
            const std::string collection=discovery_active()?discovery_description():locale.text(views[preferences.view]);
            const std::string count=collection_loading?locale.text("Searching…"):locale.text(std::to_string(model.games.size())+(discovery_active()?(model.games.size()==1?" match":" matches"):(model.games.size()==1?" game":" games")));
            const int right_edge=discovery_active()?header.clear.x-6:search.x+search.w-40,count_width=label_width(count,14,false);
            raw_label(r,count,right_edge-count_width,centred_label_y(count,14,row_cy),14,"muted",count_width+4);
            raw_label(r,collection,header.category_x,centred_label_y(collection,16,row_cy),16,discovery_active()?"accent":"ink",std::max(40,right_edge-count_width-12-header.category_x));
            if(discovery_active()){
                auto clear=header.clear;bool active=library_control_active(LibraryPress::Clear,clear);
                round_rect(r,clear.x+4,y+clear.y+6,clear.w-8,clear.h-12,10,active?"accent":"slot");
                int cx=clear.x+clear.w/2,cy=y+clear.y+clear.h/2;
                color(r,active?"on_accent":"accent");for(int w=0;w<2;w++){s.RenderDrawLine(r,cx-4,cy-4+w,cx+4,cy+4+w);s.RenderDrawLine(r,cx+4,cy-4+w,cx-4,cy+4+w);}
            }else search_icon(r,search.x+search.w-18,row_cy,18,"accent");
            rect(r,card.x+20,y+73,card.w-40,1,"slot");
            auto title=header.view;bool title_pressed=library_control_active(LibraryPress::View,title);
            if(title_pressed)round_rect(r,title.x-8,y+title.y-5,title.w+16,title.h+10,8,"focus");
            if(!model.games.empty())raw_label(r,game_title(model.selected),header.title.x,y+header.title.y,20,"ink",header.title.w);
        }

        if(!home_menu && model.games.empty() && library_ready && !collection_loading && discovery_active()){
            search_icon(r,320,y+200,36,"muted");centered(r,"No matching games",y+243,2,"ink");
            centered(r,"Try another title or clear your filters",y+282,1,"muted");
            raised_pill(r,80,y+326,232,48,12,theme.at("accent"));label(r,"Edit search",124,y+338,20,"on_accent",200);
            raised_pill(r,328,y+326,232,48,12,theme.at("panel"));label(r,"Clear filters",368,y+338,20,"accent",200);
        }
        size_t count=navigation_count(),selection=navigation_selected();
        if(count && !home_menu && preferences.layout==2){
            SDL_Rect clip{24,y+LibraryViewport::top,568,LibraryViewport::height};s.RenderSetClipRect(r,&clip);
            for(int index=browser_list.first();index<browser_list.last(count);index++){
                int row=y+LibraryViewport::top+index*LibraryViewport::stride-browser_list.scroll;bool selected=size_t(index)==model.selected;
                SDL_Rect bounds{24,row-y,568,48};bool pressed=library_press_row==index && library_control_active(LibraryPress::Row,bounds);
                int dx=24;move_element(uint32_t(index),dx,row,568,48,selected,LibraryViewport::top,LibraryViewport::top+LibraryViewport::height,24,592);dx-=24;
                int inset=pressed?1:0;round_rect(r,24+dx+inset,row+inset,568-2*inset,48-2*inset,7,selected?"focus":pressed?"line":"panel");if(selected || pressed)rect(r,24+dx,row+8,4,32,"accent");
                game_icon(r,size_t(index),38+dx,row+6,36);raw_label(r,game_title(size_t(index)),88+dx,row+10,20,"ink",game_loved(size_t(index))?456:488);
                if(game_loved(size_t(index)))favourite_badge(r,568+dx,row+24,20);
            }
            s.RenderSetClipRect(r,nullptr);
            if(browser_list.maximum(count)>0){
                bool pressed=library_control_active(LibraryPress::Vertical,{596,LibraryViewport::top,32,LibraryViewport::height});
                draw_scrollbar(r,594,y+LibraryViewport::top,LibraryViewport::height,browser_list.thumb(count),browser_list.thumb_top(count)-LibraryViewport::top,browser_rail || pressed,browser_hover,36);
            }
        }else if(count && !home_menu && preferences.layout==1){
            SDL_Rect clip{0,y+LibraryViewport::top,640,LibraryViewport::height};s.RenderSetClipRect(r,&clip);
            std::vector<int> visible;int first=int(std::floor(carousel.position))-3;
            for(int index=std::max(0,first);index<std::min(int(count),first+7);index++)visible.push_back(index);
            // Draw distant covers first so the focused box owns every overlapping pixel.
            std::stable_sort(visible.begin(),visible.end(),[&](int a,int b){return std::abs(a-carousel.position)>std::abs(b-carousel.position);});
            for(int index:visible){
                double distance=index-carousel.position,depth=std::min(2.0,std::abs(distance));
                int size=int(std::lround(224-depth*34)),x=int(std::lround(320+distance*carousel.item_spacing))-size/2,row=y+151+int(depth*18);
                bool selected=index==int(selection);
                move_element(uint32_t(index),x,row,size,size,selected,LibraryViewport::top,LibraryViewport::top+LibraryViewport::height);
                if(auto path=item_folder(size_t(index))/"box.bmp";!cover_art(r,path,{x,row,size,size},selected)){
                    game_icon(r,size_t(index),x+(size-64)/2,row+20,64);
                    if(!media.pending(path))label(r,"No cover yet",x+10,row+size-43,16,"muted",size-20);
                }
                if(game_loved(size_t(index)))favourite_badge(r,x+size-14,row+14,selected?28:22);
            }
            // A stable action badge sits below the artwork, clear of its frame.
            int play_width=std::max(92,label_width("Play",16)+48),play_x=320-play_width/2;
            raised_pill(r,play_x,y+383,play_width,24,12,theme.at("accent"));
            ring(r,play_x+14,y+395,8,"on_accent");
            raw_label(r,"A",play_x+14-label_width("A",12)/2,y+386,12,"on_accent",18);
            label(r,"Play",play_x+30,y+383,16,"on_accent",play_width-36);
            s.RenderSetClipRect(r,nullptr);
        }else if(count){
            SDL_Rect clip{0,y+LibraryViewport::top,640,LibraryViewport::height};s.RenderSetClipRect(r,&clip);
            int first=int(std::floor(carousel.position))-3;
            for(int tile_pass=0;tile_pass<2;tile_pass++)for(int offset=0;offset<7;offset++){
                const int tile_size=128,icon_size=72;
                int index=first+offset;if((index==int(selection))!=(tile_pass==1))continue;double distance=index-carousel.position;int x=int(std::round(320+distance*carousel.item_spacing))-tile_size/2;bool selected=index==int(selection);
                bool pressed=selected && pointer_down && carousel.dragging && !carousel.moved && !carousel.range && carousel.tapped(down_x,count)==selection;
                int lift=pressed?-3:selected && preferences.animations?int(std::max(0.0,1.0-(s.GetTicks()-selected_at)/190.0)*4):0,row=y+183-lift;
                if(index<0 || index>=int(count)){round_rect(r,x,row,tile_size,tile_size,5,"line");round_rect(r,x+2,row+2,tile_size-4,tile_size-4,3,"background");continue;}
                move_element(uint32_t(index),x,row,tile_size,tile_size,selected,LibraryViewport::top,LibraryViewport::top+LibraryViewport::height);
                if(selected){
                    if(preferences.dark){round_rect(r,x-15,row-14,tile_size+30,tile_size+52,10,"line");round_rect(r,x-13,row-12,tile_size+26,tile_size+48,8,"focus");}
                    raised_pill(r,x-9,row-9,tile_size+18,tile_size+44,7,theme.at("accent"));
                    round_rect(r,x-3,row-3,tile_size+6,tile_size+8,6,"line");round_rect(r,x-1,row-1,tile_size+2,tile_size+2,4,"panel");centered_tile_label(r,home_menu?"OPEN":"PLAY",x-8,row+tile_size+9,tile_size+16);
                }
                round_rect(r,x,row+4,tile_size,tile_size,5,"line");round_rect(r,x,row,tile_size,tile_size,5,"line");round_rect(r,x+2,row+2,tile_size-4,tile_size-4,4,"panel");rect(r,x+9,row+7,tile_size-18,1,"background");rect(r,x+10,row+tile_size-6,tile_size-20,1,"slot");
                if(home_menu)category_icon(r,size_t(index),x+(tile_size-icon_size)/2,row+(tile_size-icon_size)/2,icon_size);
                else{game_icon(r,size_t(index),x+(tile_size-icon_size)/2,row+(tile_size-icon_size)/2,icon_size);if(game_loved(size_t(index)))favourite_badge(r,x+tile_size-14,row+14,22);}
            }
            s.RenderSetClipRect(r,nullptr);
        }
        // The list uses this space for rows; paging controls belong to the
        // layouts that actually page horizontally.
        if(home_menu || preferences.layout!=2){
            // One edge-to-edge rail for Home, cartridges and covers, after the
            // DSi menu: glass end buttons, a pip per item in a recessed groove,
            // a glass window over the visible range and a glossy thumb that
            // lifts while held, with the position floating above it.
            // Shared hit bounds.
            const auto now_ticks=s.GetTicks();
            draw_rail(r,y,count,carousel.position,{rail_press[0].value(now_ticks,preferences.animations),rail_press[1].value(now_ticks,preferences.animations),rail_press[2].value(now_ticks,preferences.animations)},!home_menu);
        }

        if(picker_open || picker_closing())render_picker(r,y);
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
        launch_profile.mark("window-start",int(surfaces.size()));
        Uint32 flags=device ? SDL_WINDOW_FULLSCREEN_DESKTOP : SDL_WINDOW_SHOWN;
        if(o.game_menu && !o.preview)flags|=SDL_WINDOW_ALWAYS_ON_TOP;
        int width=640,height=h;
        if(device){
            SDL_Rect bounds{};
            if(s.GetDisplayBounds(index,&bounds)!=0 || bounds.w<=0 || bounds.h<=0 || bounds.w>4096 || bounds.h>4096)
                throw std::runtime_error("Cannot determine native panel size");
            width=bounds.w;height=bounds.h;
            if(width>640)s.SetHint("SDL_RENDER_SCALE_QUALITY","1");
        }
        auto* w=s.CreateWindow(title,SDL_WINDOWPOS_CENTERED_DISPLAY(index),SDL_WINDOWPOS_CENTERED_DISPLAY(index),width,height,flags);
        if(!w) throw std::runtime_error(std::string("Window creation: ")+s.GetError());
        launch_profile.mark("window-created",int(surfaces.size()));
        auto* r=s.CreateRenderer(w,-1,SDL_RENDERER_ACCELERATED|((!device || surfaces.empty())?SDL_RENDERER_PRESENTVSYNC:0));
        if(!r) r=s.CreateRenderer(w,-1,SDL_RENDERER_SOFTWARE);
        if(!r) {s.DestroyWindow(w);throw std::runtime_error(std::string("Renderer creation: ")+s.GetError());}
        launch_profile.mark("renderer-created",int(surfaces.size()));
        SDL_RendererInfo info{};
        if(s.GetRendererInfo(r,&info)==0)std::cout<<title<<" renderer="<<(info.name?info.name:"unknown")<<" flags="<<info.flags<<std::endl;
        if(o.independent && device && std::string(s.GetCurrentVideoDriver())=="wayland") {
            const char* role=std::string(title)=="RG DS Core top"?"org.rgds.top":"org.rgds.bottom";
            if(!rgds::wayland_role(w,role,s.handle)) {
                s.DestroyRenderer(r);s.DestroyWindow(w);
                throw std::runtime_error("Wayland panel assignment unavailable");
            }
        }
        s.RenderSetLogicalSize(r,640,h);
        launch_profile.mark("renderer-ready",int(surfaces.size()));
        return {w,r,s.GetWindowID(w)};
    }
    void open() {
        if(o.independent && !o.game_menu)ui_trace.open(std::getenv("RGDS_UI_TRACE"));
        s.SetMainReady(); s.SetHint("SDL_TOUCH_MOUSE_EVENTS","0"); s.SetHint("SDL_MOUSE_TOUCH_EVENTS","0");
        embedded_menu=o.game_menu && !o.preview && std::getenv("RGDS_OVERLAY_FILE");
        s.SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS","1");
        launch_profile.mark("sdl-init-start");
        if(s.Init((embedded_menu?0:SDL_INIT_VIDEO)|SDL_INIT_JOYSTICK|SDL_INIT_EVENTS)!=0) throw std::runtime_error(s.GetError());
        launch_profile.mark("sdl-init-done");
        if(!o.preview) {
            if(!std::getenv("RGDS_VOLUME_EXTERNAL"))volume_keys.open_devices();
            const char* input=std::getenv("RGDS_TOUCH_DEV");
            if(touchscreen.open_device(input?input:""))std::cout<<"Touch: "<<touchscreen.path()<<" (logical 640x480; calibrated evdev range)"<<std::endl;
            else std::cerr<<"Direct touch unavailable; using SDL events"<<std::endl;
            input_wake.open_all();
        }
        SDL_version version{}; s.GetVersion(&version);
        std::cout<<"SDL "<<int(version.major)<<'.'<<int(version.minor)<<'.'<<int(version.patch)
            <<" driver="<<(s.GetCurrentVideoDriver()?s.GetCurrentVideoDriver():"unknown")<<std::endl;
        for(int i=0;i<s.GetNumVideoDisplays();i++) {
            SDL_Rect bounds{}; s.GetDisplayBounds(i,&bounds);
            std::cout<<"display "<<i<<" "<<s.GetDisplayName(i)<<" "<<bounds.w<<"x"<<bounds.h<<std::endl;
        }
        if(embedded_menu){
            if(!overlay.open(std::getenv("RGDS_OVERLAY_FILE"),true))throw std::runtime_error("Could not open shared menu frame");
            for(int i=0;i<2;i++){
                auto* bitmap=s.CreateRGBSurfaceWithFormat(0,640,480,32,SDL_PIXELFORMAT_RGBA32);
                auto* renderer=bitmap?s.CreateSoftwareRenderer(bitmap):nullptr;
                if(!renderer){if(bitmap)s.FreeSurface(bitmap);throw std::runtime_error("Could not create menu canvas");}
                surfaces.push_back({nullptr,renderer,0,bitmap});
            }
        }else if(o.preview) {surfaces.push_back(create("RG DS Core preview",0,960,false));top_name="PREVIEW TOP";bottom_name="PREVIEW BOTTOM";if(!o.touch_recording.empty() && !touchscreen.open_replay(o.touch_recording.string()))throw std::runtime_error("Cannot read Linux touch recording");}
        else {
            int t=display(o.top,0),b=display(o.bottom,1);
            if(t==b) throw std::runtime_error("Top and bottom resolved to the same display.");
            top_name=s.GetDisplayName(t);bottom_name=s.GetDisplayName(b);
            surfaces.push_back(create("RG DS Core top",t,480,true));surfaces.push_back(create("RG DS Core bottom",b,480,true));
        }
        // Preview tests: a virtual pad whose listed buttons are already down
        // when the UI opens, like a player still holding A as the menu starts.
        const char* preview_held=o.preview?std::getenv("RGDS_PREVIEW_HELD_BUTTONS"):nullptr;int preview_pad=-1;
        if(preview_held)if(auto attach=reinterpret_cast<decltype(&SDL_JoystickAttachVirtual)>(dlsym(s.handle,"SDL_JoystickAttachVirtual")))preview_pad=attach(SDL_JOYSTICK_TYPE_GAMECONTROLLER,0,17,1);
        for(int i=0;i<s.NumJoysticks();i++) {
            auto* j=s.JoystickOpen(i);
            if(j) {joysticks.push_back(j);const char* name=s.JoystickName(j);std::cout<<"joystick "<<i<<" "<<(name?name:"")<<std::endl;
                if(o.mainline && name && std::string(name)=="gpio-keys-control"){
                    auto instance=reinterpret_cast<decltype(&SDL_JoystickInstanceID)>(dlsym(s.handle,"SDL_JoystickInstanceID"));
                    if(instance)mainline_pads.insert(instance(j));
                }}
        }
        if(preview_pad>=0){
            auto id=reinterpret_cast<decltype(&SDL_JoystickGetDeviceInstanceID)>(dlsym(s.handle,"SDL_JoystickGetDeviceInstanceID"));auto pad=reinterpret_cast<decltype(&SDL_JoystickFromInstanceID)>(dlsym(s.handle,"SDL_JoystickFromInstanceID"));auto press=reinterpret_cast<decltype(&SDL_JoystickSetVirtualButton)>(dlsym(s.handle,"SDL_JoystickSetVirtualButton"));
            std::istringstream held(preview_held);if(id && pad && press)if(auto* joystick=pad(id(preview_pad)))for(int button;held>>button;held.ignore(1))press(joystick,button,SDL_PRESSED);
        }
        snapshot_boot_input();snapshot_menu_input();
        launch_profile.mark("input-ready");
    }
    void close() {
        media.stop();sounds.close();
        flush_return_controls();system.discard_status();
        flush_volume();
        flush_brightness();
        touchscreen.close_device();volume_keys.close_devices();input_wake.close_all();hold.clear();pointer_down=false;pointer_source=PointerSource::None;raw_touch_seen=sdl_touch_seen=false;carousel.dragging=false;volume_drag=false;
        clear_view_canvases();view_canvas_failed=false;view_seen=false;view_levels.clear();view_motion.stop();
        rounded.clear();media.clear();shader_preview.clear();
        for(auto* j:joysticks)s.JoystickClose(j);joysticks.clear();
        mainline_pads.clear();mainline_controls.directions.clear();overlay.close();for(auto p:surfaces){s.DestroyRenderer(p.renderer);if(p.window)s.DestroyWindow(p.window);if(p.bitmap)s.FreeSurface(p.bitmap);}surfaces.clear();s.Quit();
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
    void update_health(bool presented=false){
        if(o.game_menu || !library_ready || !session_initialized || settings.page==Page::BootWarning)return;
        if(!update_health_sent && !presented)return;
        // SDL shuts down while an emulator owns the screens. Keep the startup
        // pulse on an independent clock so immediate play cannot mimic a hang.
        auto now=uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        if(update_health_sent && (now-update_health_at<1000 || now-update_health_started>30000))return;
        const char* token=std::getenv("RGDS_UPDATE_TOKEN");if(!token)return;
        auto path=o.data/"session/update-health.json",pending=fs::path(path.string()+".pending");
        std::ofstream file(pending);file<<"{\"build\":"<<std::quoted(o.app.filename().string())<<",\"token\":"<<std::quoted(token)<<",\"pulse\":"<<now<<"}\n";file.close();
        if(file){std::error_code ec;fs::rename(pending,path,ec);if(!ec){if(!update_health_sent)update_health_started=now;update_health_sent=true;update_health_at=now;}}
    }
    void paint_bottom(SDL_Renderer* renderer,int y){
        drawing_elements=true;element_origin_y=y;view_drawn_at=s.GetTicks();motion_hits.clear();
        bottom(renderer,y);drawing_elements=false;
    }
    void animated_bottom(SDL_Renderer* renderer,int y){
        if(chat_open){chat_bottom(renderer,y);return;}
        if(view_transitions_enabled() && layered(preferences.view_style) && !view_canvas_failed){push_bottom(renderer,y);return;}
        if(!view_motion.running || view_canvas_failed){paint_bottom(renderer,y);return;}
        if(!view_canvas)view_canvas=s.CreateTexture(renderer,SDL_PIXELFORMAT_RGBA8888,SDL_TEXTUREACCESS_TARGET,640,480);
        if(!view_canvas || s.SetRenderTarget(renderer,view_canvas)!=0){view_canvas_failed=true;view_motion.stop();s.ClearError();paint_bottom(renderer,y);return;}
        // SDL restores the window viewport/logical size when leaving a target.
        s.RenderSetLogicalSize(renderer,640,480);
        s.RenderSetClipRect(renderer,nullptr);
        paint_bottom(renderer,0);
        s.RenderSetClipRect(renderer,nullptr);
        if(s.SetRenderTarget(renderer,nullptr)!=0)throw std::runtime_error("Could not restore menu canvas");
        s.RenderSetLogicalSize(renderer,640,o.preview?960:480);
        rect(renderer,0,y,640,480,"background");
        SDL_Rect clip{0,y,640,480};s.RenderSetClipRect(renderer,&clip);
        const auto pose=view_motion.pose(view_drawn_at);
        SDL_Rect destination{int(std::round(320-320*pose.sx+pose.x)),y+int(std::round(240-240*pose.sy+pose.y)),int(std::round(640*pose.sx)),int(std::round(480*pose.sy))};
        s.SetTextureBlendMode(view_canvas,SDL_BLENDMODE_BLEND);
        // One stable surface keeps text readable throughout the small spring.
        s.SetTextureColorMod(view_canvas,255,255,255);s.SetTextureAlphaMod(view_canvas,255);
        s.RenderCopyEx(renderer,view_canvas,nullptr,&destination,pose.tilt,nullptr,SDL_FLIP_NONE);
        s.RenderSetClipRect(renderer,nullptr);view_animation_frames++;
    }
    void push_bottom(SDL_Renderer* renderer,int y){
        // Idle: straight to the panel. The capture that a transition needs
        // was taken when the input arrived (capture_view_for_input).
        if(!view_motion.running || !view_previous_ready){view_previous_ready=false;paint_bottom(renderer,y);return;}
        if(!view_canvas)view_canvas=s.CreateTexture(renderer,SDL_PIXELFORMAT_RGBA8888,SDL_TEXTUREACCESS_TARGET,640,480);
        auto fallback=[&](){s.ClearError();clear_view_canvases();view_canvas_failed=true;view_motion.stop();paint_bottom(renderer,y);};
        if(!view_canvas || s.SetRenderTarget(renderer,view_canvas)!=0){fallback();return;}
        s.RenderSetLogicalSize(renderer,640,480);s.RenderSetClipRect(renderer,nullptr);
        s.SetRenderDrawBlendMode(renderer,SDL_BLENDMODE_NONE);s.SetRenderDrawColor(renderer,0,0,0,0);s.RenderClear(renderer);
        s.SetRenderDrawBlendMode(renderer,o.game_menu?SDL_BLENDMODE_BLEND:SDL_BLENDMODE_NONE);
        paint_bottom(renderer,0);
        if(s.SetRenderTarget(renderer,nullptr)!=0)throw std::runtime_error("Could not restore push canvas");
        s.RenderSetLogicalSize(renderer,640,o.preview?960:480);
        SDL_Rect clip{0,y,640,480};s.RenderSetClipRect(renderer,&clip);
        rect(renderer,0,y,640,480,"background");
        const auto layers=view_motion.push(view_drawn_at);
        auto layer=[&](SDL_Texture* texture,double x,double shade=0,double alpha=1){
            int light=int(std::round(255*(1-shade)));s.SetTextureColorMod(texture,light,light,light);
            s.SetTextureAlphaMod(texture,Uint8(std::lround(255*alpha)));s.SetTextureBlendMode(texture,alpha<1?SDL_BLENDMODE_BLEND:SDL_BLENDMODE_NONE);
            SDL_Rect destination{int(std::round(x)),y,640,480};s.RenderCopy(renderer,texture,nullptr,&destination);
            s.SetTextureColorMod(texture,255,255,255);s.SetTextureAlphaMod(texture,255);
        };
        if(view_motion.style==ViewStyle::Zoom){
            // Scaled about the panel centre; the outgoing view underneath.
            const auto zoom=view_motion.zoom(view_drawn_at);
            auto scaled=[&](SDL_Texture* texture,double scale,double alpha,double dx){
                if(alpha<=.004)return;
                s.SetTextureAlphaMod(texture,Uint8(std::lround(255*std::clamp(alpha,0.0,1.0))));s.SetTextureBlendMode(texture,SDL_BLENDMODE_BLEND);
                const int w=int(std::lround(640*scale)),h=int(std::lround(480*scale));
                SDL_Rect destination{320-w/2+int(std::lround(dx)),y+240-h/2,w,h};s.RenderCopy(renderer,texture,nullptr,&destination);
                s.SetTextureAlphaMod(texture,255);
            };
            scaled(view_previous,zoom.out_scale,zoom.out_alpha,zoom.out_dx);
            scaled(view_canvas,zoom.in_scale,zoom.in_alpha,zoom.in_dx);
        }
        else if(layers.incoming_above){layer(view_previous,layers.outgoing,layers.shade);layer(view_canvas,layers.incoming,0,layers.alpha);}
        else{layer(view_canvas,layers.incoming,layers.shade);layer(view_previous,layers.outgoing);}
        s.RenderSetClipRect(renderer,nullptr);
        view_animation_frames++;push_animation_frames++;
    }
    void draw(bool screenshot=false) {
        if(picker_was_open && !picker_open)picker_closed_at=s.GetTicks();
        picker_was_open=picker_open;
        sync_view_motion(s.GetTicks());
        const auto reads=library_cache.counters.file_reads+media.counters.file_reads+shader_preview.counters.file_reads,checks=library_cache.counters.stamp_checks+media.counters.stamp_checks+shader_preview.counters.stamp_checks;
        auto begin_panel=[&](SDL_Renderer* renderer){
            // A swapped back buffer has undefined contents. Clear the complete
            // target, independently of the previous page's clip/blend state.
            if(s.RenderSetClipRect(renderer,nullptr)!=0 || s.SetRenderDrawBlendMode(renderer,SDL_BLENDMODE_NONE)!=0)
                throw std::runtime_error(std::string("Panel state: ")+s.GetError());
            const auto background=theme.at("background");
            s.SetRenderDrawColor(renderer,background.r,background.g,background.b,o.game_menu?0:255);
            if(s.RenderClear(renderer)!=0)throw std::runtime_error(std::string("Panel clear: ")+s.GetError());
            s.SetRenderDrawBlendMode(renderer,o.game_menu?SDL_BLENDMODE_BLEND:SDL_BLENDMODE_NONE);
        };
        auto flush_panel=[&](SDL_Renderer* renderer){
            if(s.RenderFlush(renderer)!=0)throw std::runtime_error(std::string("Panel draw: ")+s.GetError());
        };
        auto present_panel=[&](SDL_Renderer* renderer){
            s.ClearError();s.RenderPresent(renderer);
            if(const char* error=s.GetError();error && *error)throw std::runtime_error(std::string("Panel presentation: ")+error);
        };
        if(o.preview) {
            begin_panel(surfaces[0].renderer);
            top(surfaces[0].renderer,0);animated_bottom(surfaces[0].renderer,480);touch_feedback(surfaces[0].renderer,480);
            loading_overlay(surfaces[0].renderer,0,false);loading_overlay(surfaces[0].renderer,480,true);shutdown_overlay(surfaces[0].renderer,0,true);shutdown_overlay(surfaces[0].renderer,480,false);
            if(!embedded_menu)volume_feedback(surfaces[0].renderer,0);
            flush_panel(surfaces[0].renderer);
            if(screenshot)save(surfaces[0].renderer,o.screenshot);
            present_panel(surfaces[0].renderer);
        } else {
            // SDL GLES2 readback/present relies on the current GL context when
            // batching is disabled. Finish each panel before drawing the next
            // so the upper window is never read or swapped with the lower
            // renderer's context still current.
            auto* pixels=embedded_menu?overlay.begin():nullptr;
            for(int i=0;i<2;i++){
                auto* renderer=surfaces[i].renderer;
                if(i==0 && chat_open && (chat.drawing || chat.view==pictochat::View::Lobby) && chat_top_signature()==chat_top_seen)continue;
                if(i==0 && !screenshot){const auto signature=top_signature(s.GetTicks());if(!signature.empty() && signature==top_seen){top_skips++;continue;}top_seen=signature;top_drawn_at=s.GetTicks();}
                ui_trace.mark(i==0?UiTrace::TopDraw:UiTrace::BottomDraw,rendered_frames,uint32_t(settings.page));
                begin_panel(renderer);
                if(i==0)top(renderer,0);else{animated_bottom(renderer,0);touch_feedback(renderer,0);}
                loading_overlay(renderer,0,i==1);shutdown_overlay(renderer,0,i==0);
                if(i==0 && !embedded_menu)volume_feedback(renderer,0);
                ui_trace.mark(i==0?UiTrace::TopFlush:UiTrace::BottomFlush,rendered_frames,uint32_t(settings.page));
                flush_panel(renderer);
                if(screenshot)save(renderer,o.screenshot.string()+(i==0?"-top.bmp":"-bottom.bmp"));
                if(embedded_menu){if(s.RenderReadPixels(renderer,nullptr,SDL_PIXELFORMAT_RGBA32,pixels+i*overlay_panel_bytes,640*4)!=0)throw std::runtime_error("Menu frame capture failed");}
                else {
                    ui_trace.mark(i==0?UiTrace::TopPresent:UiTrace::BottomPresent,rendered_frames,uint32_t(settings.page));
                    present_panel(renderer);
                }
            }
            if(embedded_menu)overlay.publish();
        }
        rendered_frames++;render_file_reads+=library_cache.counters.file_reads+media.counters.file_reads+shader_preview.counters.file_reads-reads;render_stamp_checks+=library_cache.counters.stamp_checks+media.counters.stamp_checks+shader_preview.counters.stamp_checks-checks;
        ui_trace.mark(UiTrace::FrameDone,rendered_frames,uint32_t(settings.page));
        update_health(true);
        if(rendered_frames==1)launch_profile.finish(o.game_menu);
        if(rendered_frames==1 && o.game_menu && !embedded_menu && !o.preview){
            if(const char* ready=std::getenv("RGDS_MENU_READY_FILE")){
                auto pending=std::string(ready)+".pending";
                std::ofstream file(pending);file<<getpid()<<'\n';file.close();
                if(file){std::error_code ec;fs::rename(pending,ready,ec);}
            }
        }
        if(o.boot_ready_fd>=0){
            const char ready='1';int flags=0;
#ifdef MSG_NOSIGNAL
            flags=MSG_NOSIGNAL;
#elif defined(SO_NOSIGPIPE)
            const int suppress=1;setsockopt(o.boot_ready_fd,SOL_SOCKET,SO_NOSIGPIPE,&suppress,sizeof(suppress));
#endif
            // The guardian may have exited. A failed notification never interrupts
            // presentation, and the nonblocking socket cannot delay the first frame.
            send(o.boot_ready_fd,&ready,1,flags);::close(o.boot_ready_fd);o.boot_ready_fd=-1;
        }
        if(settings.page==Page::BootWarning && !boot_displayed){boot_displayed=true;boot_displayed_at=s.GetTicks();}
        dirty=false;
    }
    void volume_feedback(SDL_Renderer* r,int y) {
        if(!volume_toast || s.GetTicks()>=volume_toast)return;
        round_rect(r,130,y+60,380,62,10,"focus");label(r,hardware.volume_percent==0?"Muted":"Volume",151,y+68,20,"ink",175);
        label(r,std::to_string(hardware.volume_percent)+"%",421,y+70,20,"accent",80);
        round_rect(r,151,y+106,338,5,2,"slot");rect(r,151,y+106,std::max(1,hardware.volume_percent*338/100),5,"accent");
    }
    void launch() {
        if(launching)return;
        if(worker>0 && (worker_action=="device.reboot" || worker_action=="device.poweroff" || worker_action=="device.update-apply")){status="Finishing the system request";dirty=true;return;}
        if(probe)return;
        if(home_menu){open_category();return;}
        if(return_writes_pending()){status="Restoring settings";dirty=true;return;}
        // A previous failure must not latch every core off. The runner checks
        // writable storage and reapplies saved volume before opening any audio.
        return_audio_failed=false;
        if(o.boot_startup && (!library_ready || (!o.preview && (!boot_hardware_ready || system.volume_pending())))){status="Preparing the launcher";dirty=true;return;}
        if(model.games.empty())return;
        launch_preview=o.preview || model.games[model.selected].demo || !o.allow_launch;
        if(!launch_preview && (o.app.empty() || o.data.empty())) {status="ERROR: Launch needs app and data paths";dirty=true;return;}
        auto local=library_cache.options(model.games[model.selected]);launching_core=unsigned(offered_core(local.core<0?int(preferences.core):local.core));
        if(resume_launch)launching_core=unsigned(resume_core);
        sound_intent(MenuCue::Launch);
        launching=true;launch_at=s.GetTicks();feedback_frame_at=launch_at;launch_phase=LaunchPhase::Waiting;sounds.fade_music();
        launch_cancelled=launch_handoff=false;launch_result=0;launch_drawn_head=-1;cancel_navigation();carousel.reset(model.selected);dirty=true;
        if(!launch_preview && worker>0){kill(worker,SIGTERM);worker_action.clear();}
    }
    // ---- PictoChat: our own chat pad. Its rules and state live in launcher/pictochat_app.hpp;
    // this is its drawing and its hooks into the launcher's input. It uses the launcher's one keyboard,
    // and it needs no BIOS, firmware or file from Nintendo.
    struct ChatGlyphs:pictochat::GlyphSource {
        RoundedFont& font;explicit ChatGlyphs(RoundedFont& f):font(f){}
        // A missing font file must not take the launcher down: the text is simply not drawn.
        void draw(const std::string& t,int x,int y,const std::function<void(int,int,uint8_t)>& put)override{try{font.rasterize(t,24,x,y,put);}catch(const std::exception&){}}
        int width(const std::string& t)override{try{return int(std::lround(font.width(t,24)));}catch(const std::exception&){return int(RoundedFont::codepoints(t).size())*12;}}
    };
    pictochat::Chat chat;ChatGlyphs chat_glyphs{rounded};bool chat_open=false;
    int chat_focus=0;                                   // the lobby row a controller would open (4 is the profile)
    int chat_shoulder=0;Uint32 chat_shoulder_at=0;   // L (+1, older) or R (-1, newer) held on a controller
    double chat_scroll=0;Uint32 chat_scroll_at=0,chat_repeat_at=0,chat_anim_at=0;   // the log's scroll in pixels, eased towards its goal
    uint32_t chat_seen_serial=0,chat_slid_serial=0;size_t chat_seen_users=0;bool chat_seen_online=false,chat_seen_up=false;
    std::map<std::pair<SDL_Renderer*,uint32_t>,SDL_Texture*> chat_cards;
    std::map<SDL_Renderer*,SDL_Texture*> chat_pads,chat_sheets;
    bool key_accents=false,key_stamps=false,key_focus_visible=true;
    // Icon sizes in the sheet: interface icons, then the four room plans, then the stamps.
    static constexpr int chat_cells[pictochat::ui_icon_count]={26,26,24,24,28,22,22,20,22,52,52,52,52,30};
    static constexpr int chat_stamp_cell=56,chat_tail=16;
    static Color chat_ink(int index){uint32_t c=pictochat::inks[size_t(index)%pictochat::ink_count];return {uint8_t(c&255),uint8_t((c>>8)&255),uint8_t((c>>16)&255)};}
    static Color chat_hue(float degrees){uint32_t c=pictochat::hue_colour(degrees);return {uint8_t(c&255),uint8_t((c>>8)&255),uint8_t((c>>16)&255)};}
    // The colour that dresses the pad: yours, or the launcher's accent while you have not chosen one.
    Color chat_tint()const{return chat.colour_auto?theme.at("accent"):ds_colours[size_t(chat.colour)].rgb;}
    int chat_accent_index()const{return preferences.accent>=0 && preferences.accent<16?preferences.accent:11;}
    fs::path chat_helper_script()const{
        if(!o.app.empty())return o.app/"runtime/pictochat_net.py";
        std::error_code ec;return fs::weakly_canonical(o.executable.parent_path()/"../../runtime/pictochat_net.py",ec);   // a development build beside the sources
    }
    void chat_save_profile(){if(!o.data.empty())chat.save_profile(o.data/"pictochat.ini");}
    void open_pictochat(){
        if(chat_open || launching || probe)return;
        cancel_navigation();chat_open=true;chat.glyphs=&chat_glyphs;
        chat.load_profile(o.data.empty()?fs::path():o.data/"pictochat.ini",preferences.accent);
        chat.view=pictochat::View::Lobby;chat_focus=0;chat_seen_serial=chat_slid_serial=chat.serial;chat_scroll=0;
        key_layer=0;key_caps=false;key_accents=key_stamps=false;key_focus_visible=false;
        std::vector<std::string> extra;
        if(const char* more=std::getenv("RGDS_PICTOCHAT_ARGS")){std::istringstream words(more);std::string word;while(words>>word)extra.push_back(word);}
        chat.helper.start(chat_helper_script(),extra);   // without it the pad still works, on this handheld alone
        s.StartTextInput();emit_sound(MenuCue::Open);dirty=true;
    }
    void close_pictochat(){
        if(!chat_open)return;
        chat.close();chat_open=false;chat_shoulder=0;s.StopTextInput();edit_buffer.clear();
        for(auto& entry:chat_cards)if(entry.second)s.DestroyTexture(entry.second);chat_cards.clear();
        for(auto& entry:chat_pads)if(entry.second)s.DestroyTexture(entry.second);chat_pads.clear();
        for(auto& entry:chat_sheets)if(entry.second)s.DestroyTexture(entry.second);chat_sheets.clear();
        pointer_down=false;key_focus_visible=true;emit_sound(MenuCue::Back);dirty=true;
    }
    // ---- textures: messages, the live pad and one sheet of icons ----
    SDL_Texture* chat_upload(SDL_Renderer* r,const std::vector<uint32_t>& pixels,int w,int h,bool blend){
        auto* texture=s.CreateTexture(r,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,w,h);if(!texture)return nullptr;
        if(s.UpdateTexture(texture,nullptr,pixels.data(),w*4)!=0){s.DestroyTexture(texture);return nullptr;}
        s.SetTextureBlendMode(texture,blend?SDL_BLENDMODE_BLEND:SDL_BLENDMODE_NONE);return texture;
    }
    SDL_Texture* chat_card(SDL_Renderer* r,const pictochat::Entry& entry){
        auto key=std::make_pair(r,entry.serial);auto found=chat_cards.find(key);if(found!=chat_cards.end())return found->second;
        pictochat::Canvas canvas;pictochat::render(canvas,entry.ops,&chat_glyphs);
        return chat_cards[key]=chat_upload(r,canvas.px,pictochat::canvas_w,pictochat::canvas_h,false);
    }
    SDL_Texture* chat_pad_texture(SDL_Renderer* r){
        auto& texture=chat_pads[r];
        if(!texture){texture=chat_upload(r,chat.pad.px,pictochat::canvas_w,pictochat::canvas_h,false);chat.pad_dirty={};return texture;}
        if(!chat.pad_dirty.empty()){
            auto d=chat.pad_dirty;d.x0=std::max(0,d.x0);d.y0=std::max(0,d.y0);d.x1=std::min(pictochat::canvas_w,d.x1);d.y1=std::min(pictochat::canvas_h,d.y1);
            if(d.x1>d.x0 && d.y1>d.y0){SDL_Rect area{d.x0,d.y0,d.x1-d.x0,d.y1-d.y0};s.UpdateTexture(texture,&area,&chat.pad.px[size_t(d.y0)*pictochat::canvas_w+d.x0],pictochat::canvas_w*4);}
            chat.pad_dirty={};
        }
        return texture;
    }
    static int chat_cell(int id){return id<pictochat::ui_icon_count?chat_cells[id]:chat_stamp_cell;}
    static int chat_sheet_left(int id){int x=0;for(int i=0;i<id;i++)x+=chat_cell(i);return x;}
    // The icons and stamps are drawn once from their outlines as white shapes that take any colour.
    SDL_Texture* chat_sheet(SDL_Renderer* r){
        auto& texture=chat_sheets[r];if(texture)return texture;
        const int total=pictochat::ui_icon_count+pictochat::stamp_count,width=chat_sheet_left(total);
        std::vector<uint32_t> pixels(size_t(width)*chat_stamp_cell,0);
        for(int id=0;id<total;id++){
            const int cell=chat_cell(id),left=chat_sheet_left(id);
            for(int py=0;py<cell;py++)for(int px=0;px<cell;px++){
                const float u=(px+.5f-cell/2.f)/(cell/2.f),v=(py+.5f-cell/2.f)/(cell/2.f);
                const float d=(id<pictochat::ui_icon_count?pictochat::icon_distance(id,u,v):pictochat::stamp_distance(id-pictochat::ui_icon_count,u,v))*(cell/2.f);
                pixels[size_t(py)*width+left+px]=pictochat::pack(255,255,255,unsigned(std::clamp((.5f-d)*255.f,0.f,255.f)));
            }
        }
        return texture=chat_upload(r,pixels,width,chat_stamp_cell,true);
    }
    // id 0-12: interface icons; stamps are drawn with id = ui_icon_count + stamp.
    void chat_icon(SDL_Renderer* r,int id,int cx,int cy,Color ink){
        auto* texture=chat_sheet(r);if(!texture)return;
        const int cell=chat_cell(id);
        s.SetTextureColorMod(texture,ink.r,ink.g,ink.b);SDL_Rect source{chat_sheet_left(id),0,cell,cell},target{cx-cell/2,cy-cell/2,cell,cell};s.RenderCopy(r,texture,&source,&target);
    }
    // ---- drawing helpers ----
    bool chat_down(const pictochat::Box& b)const{return pointer_down && b.has(contact_x,contact_y) && b.has(down_x,down_y);}
    void chat_brackets(SDL_Renderer* r,int x,int y,int w,int h,Color c,int grow){
        const int t=3,l=16;x-=grow;y-=grow;w+=grow*2;h+=grow*2;
        for(int i=0;i<2;i++)for(int j=0;j<2;j++){
            const int px=i?x+w-l:x,py=j?y+h-t:y,qx=i?x+w-t:x,qy=j?y+h-l:y;
            tint_rect(r,px,py,l,t,c);tint_rect(r,qx,qy,t,l,c);
        }
    }
    // A raised key or button; `lit` fills it with the pad's colour.
    void chat_key_face(SDL_Renderer* r,const SDL_Rect& b,int radius,bool lit,bool down,bool enabled=true){
        const int sink=down?2:0;
        round_rect(r,b.x,b.y+3,b.w,b.h,radius,"slot");
        if(lit)fill_round(r,b.x,b.y+sink,b.w,b.h,radius,chat_tint());
        else round_rect(r,b.x,b.y+sink,b.w,b.h,radius,down?"focus":enabled?"panel":"slot");
    }
    void chat_button(SDL_Renderer* r,int y,const pictochat::Box& box,bool lit,bool enabled,int icon,const std::string& text="",Color* ink_out=nullptr){
        const bool down=enabled && chat_down(box);SDL_Rect b{box.x,y+box.y,box.w,box.h};
        chat_key_face(r,b,10,lit && enabled,down,enabled);
        const Color ink=!enabled?theme.at("line"):lit?tile_ink(chat_tint()):theme.at("ink");const int sink=down?2:0;
        if(ink_out)*ink_out=ink;
        if(icon>=0)chat_icon(r,icon,b.x+b.w/2,b.y+(text.empty()?b.h/2:b.h/2-9)+sink,ink);
        if(!text.empty())label_colour(r,text,b.x+(b.w-label_width(text,14))/2,b.y+b.h-22+sink,14,ink,b.w-2);
    }
    void chat_footer(SDL_Renderer* r,int y,const std::string& left,const std::string& right,const std::string& middle){
        rect(r,0,y+424,640,56,"background");rect(r,24,y+424,592,1,"line");
        footer_button(r,12,y+432,160,left,"B",false,footer_pressed(true));
        if(!middle.empty())label(r,middle,184,y+446,14,"muted",276);
        if(!right.empty()){Color tint=chat_tint();footer_button(r,468,y+432,160,right,"A",true,footer_pressed(false),&tint);}
    }
    // ---- the keyboard, shared with search and every text field ----
    std::vector<std::vector<KeyboardKey>> keyboard_rows()const{
        const bool room=chat_open && chat.view==pictochat::View::Room;
        return keyboard_layout(key_layer,room?KeyboardMode::Chat:edit_field==4 && !chat_open?KeyboardMode::Search:KeyboardMode::Edit,key_caps,key_accents,room && key_stamps,room?pictochat::layout::keyboard_top:186,room?pictochat::layout::keyboard_scale:1.f);
    }
    void keyboard_clamp(){auto rows=keyboard_rows();key_row=std::clamp(key_row,0,int(rows.size())-1);key_col=std::clamp(key_col,0,int(rows[size_t(key_row)].size())-1);}
    bool key_down(const SDL_Rect& b)const{return pointer_down && point_in(b,contact_x,contact_y) && point_in(b,down_x,down_y);}
    void draw_keyboard(SDL_Renderer* r,int y,const std::vector<std::vector<KeyboardKey>>& rows,bool show_focus){
        const bool room=chat_open && chat.view==pictochat::View::Room;
        const Color tint=room?chat_tint():theme.at("accent"),on_tint=room?tile_ink(tint):theme.at("on_accent");
        int bottom=0,top=480,left=640,right=0;for(const auto& row:rows)for(const auto& key:row){bottom=std::max(bottom,key.bounds.y+key.bounds.h);top=std::min(top,key.bounds.y);left=std::min(left,key.bounds.x);right=std::max(right,key.bounds.x+key.bounds.w);}
        round_rect(r,left-6,y+top-6,right-left+12,bottom-top+12,10,"slot");
        for(size_t row=0;row<rows.size();row++)for(size_t col=0;col<rows[row].size();col++){
            auto key=rows[row][col];auto b=key.bounds;b.y+=y;
            const bool chosen=show_focus && key_row==int(row) && key_col==int(col),submit=key.action==KeyAction::Submit;
            const bool armed=key.action==KeyAction::Stamp && chat.stamp_armed && std::to_string(chat.stamp)==key.label;
            const bool lit=(key.action==KeyAction::Shift && key_layer==1) || (key.action==KeyAction::Caps && key_caps) || (key.action==KeyAction::Accents && key_accents) || (key.action==KeyAction::Stamps && key_stamps) || armed;
            const bool down=key_down(b),filled=chosen || submit || lit;const int sink=down?2:0;
            round_rect(r,b.x,b.y+2,b.w,b.h,7,"line");
            if(filled)fill_round(r,b.x,b.y+sink,b.w,b.h,7,tint);
            else round_rect(r,b.x,b.y+sink,b.w,b.h,7,down?"focus":key.action==KeyAction::Character || key.action==KeyAction::Stamp?"panel":"focus");
            const Color ink=filled?on_tint:theme.at("ink");const std::string ink_key=filled?"on_accent":"ink";
            const int cx=b.x+b.w/2,cy=b.y+sink+b.h/2;
            if(key.action==KeyAction::Stamp){chat_icon(r,pictochat::ui_icon_count+std::stoi(key.label),cx,cy,armed?on_tint:chat_ink(chat.ink==pictochat::rainbow?0:chat.ink));continue;}
            if(key.action==KeyAction::Stamps){chat_icon(r,13,cx,cy,ink);continue;}
            color(r,ink_key);
            if(key.action==KeyAction::Shift){
                s.RenderDrawLine(r,cx,cy-10,cx-11,cy+1);s.RenderDrawLine(r,cx,cy-10,cx+11,cy+1);
                s.RenderDrawLine(r,cx-11,cy+1,cx-5,cy+1);s.RenderDrawLine(r,cx+11,cy+1,cx+5,cy+1);
                s.RenderDrawLine(r,cx-5,cy+1,cx-5,cy+11);s.RenderDrawLine(r,cx+5,cy+1,cx+5,cy+11);s.RenderDrawLine(r,cx-5,cy+11,cx+5,cy+11);
            }else if(key.action==KeyAction::Backspace){
                s.RenderDrawLine(r,cx-18,cy,cx-8,cy-11);s.RenderDrawLine(r,cx-8,cy-11,cx+17,cy-11);
                s.RenderDrawLine(r,cx+17,cy-11,cx+17,cy+11);s.RenderDrawLine(r,cx+17,cy+11,cx-8,cy+11);s.RenderDrawLine(r,cx-8,cy+11,cx-18,cy);
                s.RenderDrawLine(r,cx-3,cy-5,cx+7,cy+5);s.RenderDrawLine(r,cx-3,cy+5,cx+7,cy-5);
            }else if(key.action==KeyAction::Left || key.action==KeyAction::Right){
                const int sign=key.action==KeyAction::Left?-1:1;
                s.RenderDrawLine(r,cx-8*sign,cy,cx+8*sign,cy);s.RenderDrawLine(r,cx+8*sign,cy,cx+2*sign,cy-6);s.RenderDrawLine(r,cx+8*sign,cy,cx+2*sign,cy+6);
            }else if(key.action==KeyAction::Space && room){
                tint_rect(r,cx-70,cy+4,140,3,filled?on_tint:theme.at("muted"));
            }else if(key.action==KeyAction::Newline){
                // The return arrow of the original's Enter key.
                s.RenderDrawLine(r,cx+12,cy-9,cx+12,cy+3);s.RenderDrawLine(r,cx+12,cy+3,cx-10,cy+3);s.RenderDrawLine(r,cx-10,cy+3,cx-4,cy-3);s.RenderDrawLine(r,cx-10,cy+3,cx-4,cy+9);
            }else{
                const int size=key.action==KeyAction::Character?22:16;
                const bool legend=key.action==KeyAction::Character || key.action==KeyAction::Caps || key.action==KeyAction::Cancel || key.action==KeyAction::Delete || key.action==KeyAction::Accents || key.label=="Enter";
                const auto value=legend?key.label:locale.text(key.label);
                const int width=label_width(value,size,false);
                raw_label(r,value,b.x+(b.w-width)/2,b.y+sink+(b.h-size)/2+(key.alternate.empty()?-2:4),size,ink_key,b.w-4);
                if(!key.alternate.empty())raw_label(r,key.alternate,b.x+4,b.y+sink+2,11,filled?"on_accent":"muted",b.w-8);
            }
        }
    }
    void keyboard_apply(const KeyboardKey& key){
        const bool room=chat_open && chat.view==pictochat::View::Room,naming=chat_open && chat.view==pictochat::View::Naming;
        switch(key.action){
        case KeyAction::Shift:key_layer=key_layer==0?1:0;break;
        case KeyAction::Caps:key_caps=!key_caps;break;
        case KeyAction::Tab:{auto at=keyboard_submit(keyboard_rows());key_row=at.first;key_col=at.second;break;}
        case KeyAction::Accents:key_accents=!key_accents;keyboard_clamp();break;
        case KeyAction::Letters:key_stamps=false;key_row=1;key_col=1;keyboard_clamp();break;
        case KeyAction::Stamps:key_stamps=true;key_row=0;key_col=0;break;
        case KeyAction::Stamp:chat.stamp=std::stoi(key.label);chat.stamp_armed=true;chat.eraser=false;break;
        case KeyAction::Backspace:if(room)chat.backspace();else erase_character();break;
        case KeyAction::Delete:delete_character();break;
        case KeyAction::Cancel:if(naming)chat_finish_name(false);else finish_edit(false);return;
        case KeyAction::Left:move_edit_cursor(-1);break;
        case KeyAction::Right:move_edit_cursor(1);break;
        case KeyAction::Submit:if(naming)chat_finish_name(true);else if(room)chat_send();else finish_edit(true);return;
        case KeyAction::Space:if(room)chat.type(" ");else insert_text(" ");break;
        case KeyAction::Newline:chat.type("\n");break;
        case KeyAction::Character:if(room)chat.type(key.label);else insert_text(key.label);key_layer=0;break;
        }
        dirty=true;
    }
    void keyboard_choose(){keyboard_clamp();auto rows=keyboard_rows();keyboard_apply(rows.at(size_t(key_row)).at(size_t(key_col)));}
    // Controller: move over the keys, as in search.
    void keyboard_move(int code){
        auto rows=keyboard_rows();keyboard_clamp();
        if(code==8 || code==9){int x=rows[size_t(key_row)][size_t(key_col)].bounds.x+rows[size_t(key_row)][size_t(key_col)].bounds.w/2;key_row=std::clamp(key_row+(code==8?-1:1),0,int(rows.size())-1);key_col=keyboard_nearest(rows[size_t(key_row)],x);}
        else key_col=std::clamp(key_col+(code==0?-1:code==1?1:0),0,int(rows[size_t(key_row)].size())-1);
        key_focus_visible=true;
    }
    bool keyboard_touch(int x,int y){
        auto rows=keyboard_rows();
        for(size_t row=0;row<rows.size();row++)for(size_t col=0;col<rows[row].size();col++)if(point_in(rows[row][col].bounds,x,y)){key_row=int(row);key_col=int(col);key_focus_visible=false;emit_sound(MenuCue::Type);keyboard_choose();return true;}
        return false;
    }
    // ---- the lobby ----
    void chat_enter_naming(){
        edit_field=6;edit_buffer=chat.name;edit_cursor=edit_buffer.size();key_layer=0;key_caps=false;key_accents=false;key_stamps=false;key_row=1;key_col=1;key_focus_visible=false;
        chat.view=pictochat::View::Naming;emit_sound(MenuCue::Open);dirty=true;
    }
    void chat_finish_name(bool keep){
        if(keep){std::string text=edit_buffer;while(!text.empty() && text.back()==' ')text.pop_back();if(!text.empty()){chat.name=text;chat_save_profile();}}
        edit_buffer.clear();edit_field=0;chat.view=pictochat::View::Lobby;dirty=true;
    }
    void chat_set_colour(int chip){   // chip 0 follows the accent, 1-16 are the DS colours
        if(chip==0){chat.colour_auto=true;chat.colour=chat_accent_index();}else{chat.colour_auto=false;chat.colour=chip-1;}
        chat_save_profile();emit_sound(MenuCue::Navigate);dirty=true;
    }
    void chat_join(int room){chat.join(room);chat_scroll=0;chat_slid_serial=chat.serial;key_layer=0;key_caps=false;key_accents=key_stamps=false;key_focus_visible=false;emit_sound(MenuCue::Open);dirty=true;}
    void chat_leave_room(){chat.leave();emit_sound(MenuCue::Back);dirty=true;}
    void chat_lobby_bottom(SDL_Renderer* r,int y){
        using namespace pictochat;
        const Color tint=chat_tint();
        const Uint32 now=s.GetTicks();
        for(int i=0;i<4;i++){
            const auto box=layout::room(i);const bool focus=pointer_down?chat_down(box):chat_focus==i;
            const int n=chat.people(i),cy=y+box.y+box.h/2;
            alpha_round(r,box.x,y+box.y+3,box.w,box.h,14,shadow_colour(),preferences.dark?70:22);
            if(focus)fill_round(r,box.x,y+box.y,box.w,box.h,14,mix_colour(theme.at("panel"),tint,.16));else round_rect(r,box.x,y+box.y,box.w,box.h,14,"panel");
            // The floor plan on its own tile, centred; the name beside it.
            round_rect(r,box.x+12,y+box.y+10,56,56,12,focus?"panel":"slot");
            chat_icon(r,9+i,box.x+40,cy,focus?tint:theme.at("muted"));
            const auto colours=chat.colours_in(i);const std::string title=locale.text("Chat Room")+" "+std::string(1,char('A'+i));
            raw_label(r,title,box.x+84,colours.empty()?centred_label_y(title,24,cy):y+box.y+9,24,"ink",300);
            int dot=0;for(int colour:colours){fill_round(r,box.x+84+dot*18,y+box.y+47,13,13,6,ds_colours[size_t(colour)].rgb);if(++dot>=14)break;}
            // The lighter block: how many are in, and whether the room is reachable.
            {const int bx=box.x+box.w-200,bw=188;round_rect(r,bx,y+box.y+12,bw,52,12,focus?"panel":"focus");
             chat_icon(r,7,bx+30,cy,theme.at("muted"));const std::string heads=std::to_string(n)+"/16";raw_label(r,heads,bx+52,centred_label_y(heads,24,cy),24,n>=16?"muted":"ink",80);
             chat_icon(r,8,bx+bw-30,cy,chat.online?tint:theme.at("line"));}
            if(focus){const int grow=preferences.animations?2+int(std::lround(2*std::sin(now*.006))):3;chat_brackets(r,box.x,y+box.y,box.w,box.h,tint,grow);}
        }
        // Your profile.
        const auto f=layout::profile;const bool pressed=chat_down(f),focus=!pointer_down && chat_focus==4;
        alpha_round(r,f.x,y+f.y+3,f.w,f.h,14,shadow_colour(),preferences.dark?70:22);
        if(focus)fill_round(r,f.x,y+f.y,f.w,f.h,14,mix_colour(theme.at("panel"),tint,.16));else round_rect(r,f.x,y+f.y+(pressed?2:0),f.w,f.h,14,pressed?"focus":"panel");
        fill_round(r,f.x+18,y+f.y+15,26,26,13,tint);
        raw_label(r,chat.name,f.x+58,y+f.y+14,26,"ink",360);
        label(r,"Edit",f.x+f.w-96,y+f.y+18,18,"muted",60);ui_icon(r,SettingsIcon::Chevron,f.x+f.w-26,y+f.y+f.h/2,18,"muted");
        if(focus)chat_brackets(r,f.x,y+f.y,f.w,f.h,tint,preferences.animations?2+int(std::lround(2*std::sin(now*.006))):3);
        chat_footer(r,y,"Leave","Join","");
    }
    void chat_naming_bottom(SDL_Renderer* r,int y){
        using namespace pictochat;
        const Color tint=chat_tint();
        label(r,"Your name",24,y+8,16,"muted",300);
        const auto f=layout::field;
        fill_round(r,f.x,y+f.y,f.w,f.h,12,tint);round_rect(r,f.x+2,y+f.y+2,f.w-4,f.h-4,10,"panel");
        const auto text=edit_buffer;size_t cursor=edit_cursor;
        raw_label(r,text.empty()?locale.text("Type your name"):text,38,y+f.y+13,26,text.empty()?"muted":"ink",520);
        if(!text.empty())tint_rect(r,38+label_width(text.substr(0,cursor),26,false)+2,y+f.y+13,2,30,tint);
        if(!text.empty()){round_rect(r,568,y+f.y+9,40,38,10,"slot");label(r,"X",582,y+f.y+18,18,"muted",24);}
        label(r,"Your colour",24,y+94,16,"muted",260);
        label(r,chat.colour_auto?"Matches your accent":ds_colours[size_t(chat.colour)].name,200,y+94,16,"muted",300);
        for(int i=0;i<17;i++){
            const auto b=layout::chip(i);const bool picked=i==0?chat.colour_auto:!chat.colour_auto && chat.colour==i-1;
            if(picked)round_rect(r,b.x-3,y+b.y-3,b.w+6,b.h+6,11,"ink");
            round_rect(r,b.x-1,y+b.y-1,b.w+2,b.h+2,9,"line");
            if(i==0){fill_round(r,b.x,y+b.y,b.w,b.h,8,theme.at("accent"));ui_icon(r,SettingsIcon::Spark,b.x+b.w/2,y+b.y+b.h/2,18,"on_accent");}
            else fill_round(r,b.x,y+b.y,b.w,b.h,8,ds_colours[size_t(i-1)].rgb);
        }
        draw_keyboard(r,y,keyboard_rows(),key_focus_visible);
        chat_footer(r,y,"Cancel","Done","");
    }
    void chat_lobby_top(SDL_Renderer* r,int y){
        using namespace pictochat;
        const Color tint=chat_tint();
        category_icon(r,pictochat_tile,272,y+66,96);
        centered(r,"PictoChat",y+174,2,"ink");
        centered(r,!chat.status.empty()?chat.status:chat.view==View::Naming?"Type your name":"Choose a chat room to join",y+218,1,chat.status.empty()?"muted":"accent",72);
        round_rect(r,100,y+258,440,76,16,"panel");
        fill_round(r,122,y+282,28,28,14,tint);
        raw_label(r,chat.view==View::Naming?edit_buffer:chat.name,164,y+266,26,"ink",360);
        label(r,"You appear like this to others",164,y+302,14,"muted",360);
        const bool up=chat.helper_up,on=chat.online;
        const std::string state=up?(on?"Wi-Fi connected":"No Wi-Fi: you can still draw"):chat.helper.running()?"Starting…":"Chat is unavailable: you can still draw";
        const int w=label_width(state,18)+60,left=320-w/2;
        round_rect(r,left,y+356,w,38,19,on?"accent":"slot");chat_icon(r,8,left+26,y+375,on?theme.at("on_accent"):theme.at("muted"));label_colour(r,state,left+46,y+364,18,on?theme.at("on_accent"):theme.at("muted"),w-54);
        if(on){const int n=int(chat.users.size());centered(r,n==0?"Nobody else is nearby yet":n==1?"1 person nearby":std::to_string(n)+" people nearby",y+414,1,"muted",72);}
    }
    // ---- the room: lower screen ----
    // Screen coordinates on the (enlarged) pad, mapped back to the message canvas.
    std::pair<int,int> chat_pad_point(int x,int y)const{
        const auto p=pictochat::layout::pad;
        return {p.x+int(std::lround((x-p.x)/pictochat::layout::pad_scale)),p.y+int(std::lround((y-p.y)/pictochat::layout::pad_scale))};
    }
    void chat_room_bottom(SDL_Renderer* r,int y){
        using namespace pictochat;
        const Color tint=chat_tint(),on_tint=tile_ink(tint);const auto p=layout::pad;
        // Your name tab on the frame, then the ruled paper (scaled up from the message canvas).
        const std::string you=chat.name;const int tab=std::clamp(label_width(you,16,false)+32,80,320);
        alpha_round(r,p.x-5,y+p.y-1,p.w+10,p.h+10,12,shadow_colour(),preferences.dark?70:24);
        fill_round(r,p.x-5,y+p.y-5,p.w+10,p.h+10,12,tint);
        if(auto* paper=chat_pad_texture(r)){SDL_Rect target{p.x,y+p.y,p.w,p.h};s.RenderCopy(r,paper,nullptr,&target);}
        fill_round(r,p.x-5,y+p.y-25,tab,26,8,tint);raw_label_colour(r,you,p.x+9,centred_label_y(you,16,y+p.y-13),16,on_tint,tab-22);
        if(!chat.drawing){const int cx=p.x+int(chat.caret_x*layout::pad_scale),top=p.y+int((Chat::text_y(chat.caret_line)-2)*layout::pad_scale);tint_rect(r,cx,y+top,2,int(28*layout::pad_scale),tint);}
        // Under the pad, as on the DS: who else is here as coloured name chips, or the pad's own notice.
        {const auto n=layout::names;const int cy=y+n.y+n.h/2;
         if(!chat.status.empty())raw_label(r,chat.status,n.x,centred_label_y(chat.status,14,cy),14,"accent",n.w);
         else{
            const int count=chat.people(chat.room);const std::string heads=std::to_string(count)+"/16";const int heads_w=label_width(heads,16,false)+34;int x=n.x;
            for(const auto& u:chat.users){
                if(u.second.room!=chat.room)continue;const Color c=ds_colours[size_t(u.second.colour)].rgb;const int w=std::min(150,label_width(u.second.name,14,false)+22);
                if(x+w>n.x+n.w-heads_w-8)break;
                fill_round(r,x,y+n.y,w,n.h,11,c);raw_label_colour(r,u.second.name,x+11,centred_label_y(u.second.name,14,cy),14,tile_ink(c),w-20);x+=w+6;
            }
            if(x==n.x){const std::string alone=locale.text(chat.online?"Nobody else here yet":"Drawing on your own");raw_label(r,alone,n.x,centred_label_y(alone,14,cy),14,"muted",400);}
            chat_icon(r,7,n.x+n.w-heads_w+10,cy,theme.at("muted"));raw_label(r,heads,n.x+n.w-heads_w+26,centred_label_y(heads,16,cy),16,"ink",heads_w);
         }}
        // The tool column on the left: pen, eraser and the two sizes; undo and clear end the ink row.
        chat_button(r,y,layout::tool(0),!chat.eraser && !chat.stamp_armed,true,0);
        chat_button(r,y,layout::tool(1),chat.eraser,true,1);
        for(int i=0;i<2;i++){
            const auto box=layout::tool(2+i);const bool on=chat.thick==(i==1);
            chat_button(r,y,box,on,true,-1);
            const int size=chat.eraser?(i?18:10):(i?13:6);const Color ink=on?on_tint:theme.at("ink");
            fill_round(r,box.x+box.w/2-size/2,y+box.y+box.h/2-size/2,size,size,size/2,ink);
        }
        chat_button(r,y,layout::undo,false,chat.inked(),3);chat_button(r,y,layout::clear,false,chat.inked(),2);
        // Inks.
        for(int i=0;i<=rainbow;i++){
            const auto b=layout::swatch(i);const bool picked=chat.ink==i && !chat.eraser;
            if(picked)round_rect(r,b.x-3,y+b.y-3,b.w+6,b.h+6,11,"ink");
            round_rect(r,b.x-1,y+b.y-1,b.w+2,b.h+2,9,"line");   // keeps the black ink visible on dark themes
            if(i<rainbow)fill_round(r,b.x,y+b.y,b.w,b.h,8,chat_ink(i));
            else{for(int x=0;x<b.w;x++)tint_rect(r,b.x+x,y+b.y+3,1,b.h-6,chat_hue(x*11.f));round_rect(r,b.x,y+b.y,b.w,3,2,"panel");round_rect(r,b.x,y+b.y+b.h-3,b.w,3,2,"panel");}
        }
        // Beside the keyboard, as the DS keeps them: Send, the scroll arrows and Leave.
        chat_button(r,y,layout::send,true,chat.inked(),4,"Send");
        const bool older=chat.scrolled+1<int(chat.log.size()),newer=chat.scrolled>0;
        chat_button(r,y,layout::scroll_up,false,older,5);chat_button(r,y,layout::scroll_down,false,newer,6);
        if(chat.unread)fill_round(r,layout::scroll_down.x+layout::scroll_down.w-12,y+layout::scroll_down.y+4,8,8,4,tint);
        {const auto b=layout::leave;const bool down=chat_down(b);chat_key_face(r,{b.x,y+b.y,b.w,b.h},10,false,down);ui_icon(r,SettingsIcon::Back,b.x+b.w/2,y+b.y+17+(down?2:0),20,"accent");
         const std::string text=locale.text("Leave");raw_label(r,text,b.x+(b.w-label_width(text,12,false))/2,y+b.y+b.h-18+(down?2:0),12,"muted",b.w);}
        draw_keyboard(r,y,keyboard_rows(),key_focus_visible);
    }
    void chat_bottom(SDL_Renderer* r,int y){
        rect(r,0,y,640,480,"background");pattern(r,y,480);
        if(chat.view==pictochat::View::Lobby){chat_lobby_bottom(r,y);return;}
        if(chat.view==pictochat::View::Naming){chat_naming_bottom(r,y);return;}
        chat_room_bottom(r,y);
    }
    // ---- the room: upper screen ----
    static constexpr float chat_log_scale=.9f;   // message cards on the upper screen
    static int chat_entry_height(const pictochat::Entry& e){return e.notice.empty()?124:34;}
    // What the upper screen depends on. While the pad is being drawn on, or the lobby's brackets breathe,
    // the upper screen is skipped as long as this has not changed (and at least every 15 s, for the clock).
    std::string chat_top_seen;
    // The library's upper screen repaints only when something it shows has
    // changed (and at least every two seconds as a safety net): transitions
    // and browsing animate the lower screen alone, so the top panel's draw and
    // present are usually skipped. An empty signature means "always draw".
    std::string top_seen;Uint32 top_drawn_at=0;uint64_t top_skips=0;
    std::string top_signature(Uint32 now){
        if(settings.page!=Page::Library || chat_open || o.game_menu || probe || card_repair || launching || embedded_menu || (o.boot_startup && !library_ready) || now-top_drawn_at>=2000)return {};
        std::ostringstream k;
        const auto accent=theme.at("accent"),background=theme.at("background");
        k<<home_menu<<'|'<<home_selected<<'|'<<model.selected<<'|'<<model.games.size()<<'|'<<library_ready<<collection_loading<<summary_ready<<summary.generation<<picker_open<<discovery_active()<<'|'
         <<status_stamp()<<'|'<<hardware.battery<<hardware.charging<<(hardware.charging?int((now/420)%4):-1)<<hardware.external_power<<hardware.volume<<hardware.volume_percent<<'|'<<wifi_asset()<<'|'<<artwork_busy()<<(artwork_busy()?loader_phase(now):0)<<'|'
         <<preferences.dark<<preferences.pixel<<preferences.rich_icons<<preferences.accent<<preferences.high_contrast<<preferences.background<<preferences.home_layout<<preferences.view<<preferences.layout<<preferences.language<<'|'
         <<int(accent.r)<<','<<int(accent.g)<<','<<int(accent.b)<<','<<int(background.r)<<','<<int(background.g)<<','<<int(background.b)<<'|'
         <<status<<'|'<<return_writes_pending()<<boot_hardware_ready<<system.volume_pending()<<'|'<<update_status["state"]<<update_status["prepared"]<<'|'<<device_status["wifi_connected"]<<'|'<<(volume_toast && now<volume_toast?now:0)<<'|'
         <<(preferences.animations && cover_motion_at && now-cover_motion_at<110?now:0)<<'|'<<(home_menu && preferences.home_layout==1?std::time(nullptr):0);
        if(!home_menu && !model.games.empty() && !surfaces.empty()){
            const auto& game=model.games[model.selected];auto item=item_folder(model.selected);auto* r=surfaces.front().renderer;
            k<<'|'<<game_title(model.selected)<<'|'<<library_cache.metadata(game).info["developer"]<<'|'<<media.ready(r,item/"box.bmp")<<media.ready(r,game.path,true)<<media.ready(r,item/"icon.bmp")<<'|';
            const auto& stats=library_cache.stats(game);k<<stats.seconds<<','<<stats.sessions;
        }
        if(home_menu && !surfaces.empty()){const auto& last=summary.groups[0].last_game;if(last)k<<'|'<<media.ready(surfaces.front().renderer,last->path,true);}
        return k.str();
    }
    std::string chat_top_signature()const{
        std::ostringstream key;const auto accent=theme.at("accent");
        key<<int(chat.view)<<'|'<<chat.serial<<'|'<<chat.users.size()<<'|'<<chat.online<<chat.helper_up<<chat.helper.running()<<'|'<<chat.scrolled<<'|'<<int(chat_scroll*2)<<chat.unread
           <<'|'<<chat.status<<'|'<<chat.name<<'|'<<edit_buffer<<'|'<<chat.colour<<chat.colour_auto<<int(accent.r)<<','<<int(accent.g)<<','<<int(accent.b)<<preferences.dark<<preferences.pixel<<preferences.rich_icons
           <<'|'<<chat.room<<'|'<<(volume_toast && s.GetTicks()<volume_toast)<<'|'<<s.GetTicks()/15000;
        return key.str();
    }
    double chat_scroll_goal()const{double px=0;int n=0;for(auto it=chat.log.rbegin();it!=chat.log.rend() && n<chat.scrolled;++it,++n)px+=chat_entry_height(*it);return px;}
    double chat_content_height()const{double px=0;for(const auto& e:chat.log)px+=chat_entry_height(e);return px;}
    void chat_top(SDL_Renderer* r,int y){
        using namespace pictochat;
        rect(r,0,y,640,480,"background");pattern(r,y+48,432);status_bar(r,y);
        if(chat.view!=View::Room){chat_lobby_top(r,y);return;}
        const Color tint=chat_tint(),on_tint=tile_ink(tint);
        // Header: the room's letter badge and name on the left; who is here and the signal on the right.
        {const std::string letter(1,char('A'+chat.room));fill_round(r,24,y+56,40,40,11,tint);alpha_round(r,28,y+59,32,12,5,Color{255,255,255},50);raw_label_colour(r,letter,44-label_width(letter,24,false)/2,centred_label_y(letter,24,y+76),24,on_tint,30);}
        {const std::string title=locale.text("Chat Room")+" "+std::string(1,char('A'+chat.room));raw_label(r,title,76,centred_label_y(title,24,y+76),24,"ink",320);}
        const std::string count=std::to_string(chat.people(chat.room))+"/16";
        chat_icon(r,8,610,y+76,chat.online?tint:theme.at("line"));
        raw_label(r,count,586-label_width(count,22,false),centred_label_y(count,22,y+76),22,"ink",70);chat_icon(r,7,566-label_width(count,22,false),y+76,theme.at("muted"));
        // The rail on the left: an arrow above and below a recessed track and a glossy thumb in the pad's colour.
        constexpr int track_top=130,track_height=256,view_top=104,view_height=376;
        const bool more=chat.scrolled+1<int(chat.log.size()),less=chat.scrolled>0;
        for(int i=0;i<2;i++){const bool on=i?less:more;const int by=i?y+track_top+track_height+8:y+track_top-38;if(on)raised_pill(r,16,by,28,26,8,theme.at("panel"));else round_rect(r,16,by,28,26,8,"slot");chat_icon(r,5+i,30,by+13,on?theme.at("ink"):theme.at("line"));}
        sunken_track(r,22,y+track_top,16,track_height,8);
        const double total=std::max<double>(chat_content_height(),view_height),span=total-view_height;
        const int thumb=span<=0?track_height-4:std::clamp(int(track_height*view_height/total),34,track_height-4);
        const double offset=span<=0?0:std::clamp(chat_scroll,0.0,span)/span;   // 0 at the newest message
        {const int ty=y+track_top+2+int(std::lround((track_height-4-thumb)*(1-offset)));alpha_round(r,24,ty+2,12,thumb,6,shadow_colour(),preferences.dark?60:24);glossy_pill(r,24,ty,12,thumb,6,span<=0?mix_colour(theme.at("slot"),tint,.35):tint);}
        {const auto colours=chat.colours_in(chat.room);for(size_t i=0;i<colours.size() && i<6;i++){fill_round(r,16+int(i%2)*16,y+428+int(i/2)*16,12,12,3,ds_colours[size_t(colours[i])].rgb);}}
        chat_top_seen=chat_top_signature();
        // Drop pictures of messages that have left the log.
        {std::set<uint32_t> keep;for(const auto& e:chat.log)keep.insert(e.serial);
         for(auto it=chat_cards.begin();it!=chat_cards.end();){if(!keep.count(it->first.second)){if(it->second)s.DestroyTexture(it->second);it=chat_cards.erase(it);}else ++it;}}
        // The log: newest at the bottom, slid by the eased scroll, clipped to its window.
        SDL_Rect window{56,y+view_top,584,view_height};s.RenderSetClipRect(r,&window);
        const int card_w=int(pictochat::canvas_w*chat_log_scale),card_h=int(pictochat::canvas_h*chat_log_scale),card_x=70;
        double cursor=view_top+view_height+chat_scroll;
        for(int i=int(chat.log.size())-1;i>=0 && cursor>view_top-130;i--){
            const auto& e=chat.log[size_t(i)];const int h=chat_entry_height(e);cursor-=h;
            const int top=int(std::lround(cursor));
            if(top>480)continue;
            if(!e.notice.empty()){
                const int w=label_width(e.notice,16)+28;round_rect(r,348-w/2,y+top+4,w,24,12,"slot");label(r,e.notice,348-w/2+14,centred_label_y(locale.text(e.notice),16,y+top+16),16,"muted",w);continue;
            }
            // A card in the sender's colour with the name on a tab, as on the DS.
            const Color colour=e.mine && chat.colour_auto?chat_tint():ds_colours[size_t(e.colour)].rgb,ink=tile_ink(colour);
            const std::string name=e.mine?locale.text("You"):e.name;const int tab=std::clamp(label_width(name,16,false)+32,80,330);
            alpha_round(r,card_x,y+top+25,card_w+8,card_h+8,10,shadow_colour(),preferences.dark?60:18);
            fill_round(r,card_x,y+top+22,card_w+8,card_h+8,10,colour);
            if(auto* card=chat_card(r,e)){SDL_Rect target{card_x+4,y+top+26,card_w,card_h};s.RenderCopy(r,card,nullptr,&target);}
            fill_round(r,card_x,y+top+2,tab,26,9,colour);raw_label_colour(r,name,card_x+14,centred_label_y(name,16,y+top+14),16,ink,tab-24);
        }
        s.RenderSetClipRect(r,nullptr);
        if(chat.scrolled>0){const std::string text=locale.text(chat.unread?"New messages below":"Older messages");const int w=label_width(text,16,false)+32;alpha_round(r,348-w/2,y+447,w,28,14,shadow_colour(),preferences.dark?60:20);round_rect(r,348-w/2,y+444,w,28,14,chat.unread?"accent":"panel");raw_label_colour(r,text,348-w/2+16,centred_label_y(text,16,y+458),16,chat.unread?theme.at("on_accent"):theme.at("muted"),w);}
    }
    // ---- input ----
    void chat_type(const std::string& piece){
        if(chat.view==pictochat::View::Naming){insert_text(piece);dirty=true;return;}
        if(chat.view!=pictochat::View::Room)return;
        for(size_t i=0;i<piece.size();){size_t n=1;while(i+n<piece.size() && (static_cast<unsigned char>(piece[i+n])&0xc0)==0x80)n++;chat.type(piece.substr(i,n));i+=n;}
        dirty=true;
    }
    void chat_send(){if(chat.send(s.GetTicks()/1000.0))emit_sound(MenuCue::Confirm);else if(!chat.status.empty())emit_sound(MenuCue::Error);dirty=true;}
    bool chat_event(const SDL_Event& e){
        if(e.type==SDL_TEXTINPUT){if(chat.view!=pictochat::View::Lobby)chat_type(e.text.text);return true;}
        if(e.type==SDL_JOYBUTTONDOWN && chat.view==pictochat::View::Room && (joystick_source(e.jbutton.button)==12 || joystick_source(e.jbutton.button)==13)){
            chat_action(joystick_source(e.jbutton.button)==12?15:16);return true;   // L2 / R2: previous / next ink
        }
        // The d-pad and A drive the keyboard, so the shoulder buttons scroll the log: a tap steps, holding repeats.
        if((e.type==SDL_JOYBUTTONDOWN || e.type==SDL_JOYBUTTONUP) && (e.jbutton.button==4 || e.jbutton.button==5)){
            if(e.type==SDL_JOYBUTTONUP){chat_shoulder=0;return false;}
            if(chat.view!=pictochat::View::Room)return false;
            last_activity=s.GetTicks();chat_shoulder=e.jbutton.button==4?1:-1;chat_shoulder_at=chat_repeat_at=s.GetTicks();chat_scroll_step(chat_shoulder);return true;
        }
        if(e.type==SDL_KEYDOWN){
            const auto key=e.key.keysym.sym;
            if(chat.view==pictochat::View::Lobby)return false;
            if(key==SDLK_BACKSPACE){if(chat.view==pictochat::View::Naming)erase_character();else chat.backspace();dirty=true;return true;}
            if(key==SDLK_DELETE && chat.view==pictochat::View::Naming){delete_character();return true;}
            if(key==SDLK_LEFT && chat.view==pictochat::View::Naming){move_edit_cursor(-1);dirty=true;return true;}
            if(key==SDLK_RIGHT && chat.view==pictochat::View::Naming){move_edit_cursor(1);dirty=true;return true;}
            if(key==SDLK_RETURN || key==SDLK_KP_ENTER){if(chat.view==pictochat::View::Naming)chat_finish_name(true);else{chat.type("\n");dirty=true;}return true;}
            if(key==SDLK_ESCAPE){chat_action(3);return true;}
            if((key==SDLK_PAGEUP || key==SDLK_PAGEDOWN) && chat.view==pictochat::View::Room){chat_scroll_step(key==SDLK_PAGEUP?1:-1);return true;}
            if(key>=32 && key<127)return true;   // typed letters arrive as text, not as buttons
        }
        return false;
    }
    void chat_press(int x,int y){
        using namespace pictochat;
        last_activity=s.GetTicks();dirty=true;chat_repeat_at=s.GetTicks();
        if(chat.view==View::Lobby){
            for(int i=0;i<4;i++)if(layout::room(i).has(x,y)){chat_focus=i;key_focus_visible=false;chat_join(i);return;}
            if(layout::profile.has(x,y)){chat_focus=4;chat_enter_naming();return;}
            if(x>=12 && x<172 && y>=432 && y<472)close_pictochat();
            else if(x>=468 && x<628 && y>=432 && y<472){if(chat_focus==4)chat_enter_naming();else chat_join(chat_focus);}
            return;
        }
        if(chat.view==View::Naming){
            if(x>=568 && x<608 && y>=layout::field.y+9 && y<layout::field.y+47 && !edit_buffer.empty()){edit_buffer.clear();edit_cursor=0;return;}
            for(int i=0;i<17;i++)if(layout::chip(i).has(x,y)){chat_set_colour(i);return;}
            if(x>=468 && x<628 && y>=432 && y<472){chat_finish_name(true);return;}
            if(x>=12 && x<172 && y>=432 && y<472){chat_finish_name(false);return;}
            keyboard_touch(x,y);return;
        }
        // In a room.
        if(layout::leave.has(x,y)){chat_leave_room();return;}
        if(keyboard_touch(x,y))return;
        if(layout::pad.has(x,y)){key_focus_visible=false;auto at=chat_pad_point(x,y);chat.begin_stroke(at.first,at.second);return;}
        if(layout::tool(0).has(x,y)){chat.eraser=false;chat.stamp_armed=false;emit_sound(MenuCue::Navigate);return;}
        if(layout::tool(1).has(x,y)){chat.eraser=true;chat.stamp_armed=false;emit_sound(MenuCue::Navigate);return;}
        if(layout::tool(2).has(x,y)){chat.thick=false;return;}
        if(layout::tool(3).has(x,y)){chat.thick=true;return;}
        if(layout::send.has(x,y)){chat_send();return;}
        if(layout::undo.has(x,y)){chat.undo();return;}
        if(layout::clear.has(x,y)){chat.clear_pad();return;}
        for(int i=0;i<=rainbow;i++)if(layout::swatch(i).has(x,y)){chat.ink=i;chat.eraser=false;chat.stamp_armed=false;emit_sound(MenuCue::Navigate);return;}
        if(layout::scroll_up.has(x,y)){chat_scroll_step(1);return;}
        if(layout::scroll_down.has(x,y)){chat_scroll_step(-1);return;}
    }
    void chat_scroll_step(int direction){   // +1 towards older messages
        const int before=chat.scrolled;
        chat.scrolled=std::clamp(chat.scrolled+direction,0,std::max(0,int(chat.log.size())-1));
        if(chat.scrolled==0)chat.unread=false;
        if(before!=chat.scrolled)emit_sound(MenuCue::Navigate);dirty=true;
    }
    void chat_drag(int x,int y){if(chat.drawing){auto at=chat_pad_point(x,y);chat.extend_stroke(at.first,at.second);dirty=true;}}
    void chat_release(){if(chat.drawing){chat.end_stroke();dirty=true;}}
    void chat_action(int code){
        using namespace pictochat;
        last_activity=s.GetTicks();dirty=true;
        if(chat.view==View::Naming){
            if(code==3 || code==7){chat_finish_name(false);return;}
            if(code==4){chat_finish_name(true);return;}
            if(code==2){key_focus_visible=true;keyboard_choose();return;}
            if(code==11){erase_character();return;}
            if(code==12){key_layer=key_layer==0?1:0;return;}
            keyboard_move(code);return;
        }
        if(chat.view==View::Lobby){
            if(code==3 || code==7){close_pictochat();return;}
            key_focus_visible=true;
            if(code==8)chat_focus=std::max(0,chat_focus-1);else if(code==9)chat_focus=std::min(4,chat_focus+1);
            else if(code==2 || code==4)(chat_focus==4?chat_enter_naming():chat_join(chat_focus));
            else if(code==11)chat_enter_naming();
            return;
        }
        switch(code){
        case 3:case 7:chat_leave_room();break;   // B, or the Anbernic button
        case 4:chat_send();break;                // Start
        case 2:key_focus_visible=true;keyboard_choose();break;   // A presses the key under the focus
        case 0:case 1:case 8:case 9:keyboard_move(code);break;
        case 11:chat.undo();break;
        case 12:chat.eraser=!chat.eraser;chat.stamp_armed=false;break;
        case 13:chat_scroll_step(1);break;
        case 14:chat_scroll_step(-1);break;
        case 15:chat.ink=(chat.ink+rainbow)%(rainbow+1);chat.eraser=false;break;
        case 16:chat.ink=(chat.ink+1)%(rainbow+1);chat.eraser=false;break;
        default:break;
        }
    }
    void poll_chat(){
        if(!chat_open)return;
        chat.poll();const Uint32 now=s.GetTicks();
        // A message that arrives slides up from below; while you read older ones the page stays put.
        if(chat.serial!=chat_slid_serial){
            for(auto it=chat.log.rbegin();it!=chat.log.rend() && it->serial>chat_slid_serial;++it)chat_scroll+=chat.scrolled>0?chat_entry_height(*it):-chat_entry_height(*it);
            chat_slid_serial=chat.serial;
        }
        const double dt=std::min(.05,(now-chat_scroll_at)/1000.0);chat_scroll_at=now;
        const double goal=chat_scroll_goal();
        if(!preferences.animations || std::fabs(chat_scroll-goal)<.4)chat_scroll=goal;
        else{chat_scroll+=(goal-chat_scroll)*(1-std::exp(-dt*16));if(now-chat_anim_at>=16){chat_anim_at=now;dirty=true;}}
        if(chat.view==pictochat::View::Room && chat_shoulder && now-chat_shoulder_at>380 && now-chat_repeat_at>130){chat_repeat_at=now;chat_scroll_step(chat_shoulder);}
        // Holding an arrow keeps scrolling.
        if(chat.view==pictochat::View::Room && pointer_down && now-pointer_at>380 && now-chat_repeat_at>130){
            if(chat_down(pictochat::layout::scroll_up)){chat_repeat_at=now;chat_scroll_step(1);}
            else if(chat_down(pictochat::layout::scroll_down)){chat_repeat_at=now;chat_scroll_step(-1);}
        }
        if(chat.view==pictochat::View::Lobby && preferences.animations && now-chat_anim_at>=33){chat_anim_at=now;dirty=true;}   // the selection brackets breathe, at about 30 frames a second
        if(chat.serial!=chat_seen_serial || chat.users.size()!=chat_seen_users || chat.online!=chat_seen_online || chat.helper_up!=chat_seen_up){
            if(chat.serial!=chat_seen_serial && !chat.log.empty() && !chat.log.back().mine && chat.log.back().notice.empty())emit_sound(MenuCue::Enable);
            chat_seen_serial=chat.serial;chat_seen_users=chat.users.size();chat_seen_online=chat.online;chat_seen_up=chat.helper_up;dirty=true;
        }
    }
    void begin_launch_exit(bool handoff,Uint32 now){
        launch_handoff=handoff;
        if(launch_phase==LaunchPhase::Exiting)return;
        launch_exit_opacity=launch_opacity(now);launch_exit_at=now;launch_phase=LaunchPhase::Exiting;dirty=true;
    }
    void cancel_launch(){
        if(!launching || launch_phase==LaunchPhase::Playing || launch_cancelled)return;
        sounds.stop();sound_intent(MenuCue::Back);
        launch_cancelled=true;launch_cancel_at=s.GetTicks();
        if(launch_child>0)kill(launch_child,SIGTERM);
        else begin_launch_exit(false,s.GetTicks());
        launch_handoff=false;
    }
    void finish_launch(bool reopen){
        if(launch_channel>=0)::close(launch_channel);launch_channel=-1;
        launching=false;launch_phase=LaunchPhase::None;
        if(!reopen)configure_sounds();   // a cancelled or preview launch: the music comes back
        if(reopen){
            library_cache.clear();library_summary.request(o.data,all_games);open();configure_sounds();last_activity=s.GetTicks();
            library_view(preferences.view);
            try{if(fs::exists(o.system_preferences))system_preferences=load_system_preferences(o.system_preferences);}
            catch(const std::exception& e){std::cerr<<"Could not reload system settings: "<<e.what()<<std::endl;}
            if(hardware.volume){hardware.volume_percent=confirmed_volume();return_volume=hardware.volume_percent;volume_unsaved=true;}
            if(hardware.brightness){return_brightness=system_preferences.brightness;hardware.brightness_percent=system_preferences.brightness;brightness_unsaved=true;brightness_changed=last_activity;}
            if(hardware.power && system_preferences.power>0)return_power=system_preferences.power;
            return_status_needed=true;begin_return_controls();
        }
        last_activity=s.GetTicks();input_guard_until=last_activity+(reopen?250:100);
        status=launch_cancelled?"Launch cancelled":launch_result==78?"The memory card is read-only":launch_result?"LAUNCH ERROR "+std::to_string(launch_result)+" - SEE LOG":launch_preview?"PREVIEW: "+std::string(core_names[launching_core])+" SELECTED":"BACK HOME";
        if(launch_result==78)check_card(s.GetTicks(),true);
        if(launch_result && !launch_cancelled)emit_sound(MenuCue::Error);
        else if(reopen)emit_sound(MenuCue::Back);
        dirty=true;
    }
    void start_launch_runner(Uint32 now){
        try{save_switcher_library();}catch(const std::exception& error){std::cerr<<"Game switcher snapshot: "<<error.what()<<std::endl;std::error_code ec;fs::remove(o.data/"session/switcher-library.ini",ec);}
        int channel[2];
        if(socketpair(AF_UNIX,SOCK_STREAM,0,channel)<0){launch_result=127;begin_launch_exit(false,now);return;}
        if(fcntl(channel[0],F_SETFD,FD_CLOEXEC)<0 || fcntl(channel[1],F_SETFD,FD_CLOEXEC)<0){::close(channel[0]);::close(channel[1]);launch_result=127;begin_launch_exit(false,now);return;}
#ifdef SO_NOSIGPIPE
        const int suppress=1;setsockopt(channel[0],SOL_SOCKET,SO_NOSIGPIPE,&suppress,sizeof(suppress));
#endif
        std::vector<std::string> args={"python3",(o.app/"runtime/emulators.py").string(),"launch","--app",o.app.string(),
            "--data",o.data.string(),"--core",core_ids[launching_core],"--rom",model.games[model.selected].path.string(),"--display-handoff-fd",std::to_string(channel[1])};
        save_state();launch_child=fork();
        if(launch_child==0){
            if(resume_launch)setenv("RGDS_FORCE_AUTOLOAD","1",1);
            ::close(channel[0]);if(fcntl(channel[1],F_SETFD,0)<0)_exit(127);
            std::vector<char*> argv;for(auto& a:args)argv.push_back(a.data());argv.push_back(nullptr);
            execvp(argv[0],argv.data());_exit(127);
        }
        resume_launch=false;
        ::close(channel[1]);launch_channel=channel[0];launch_phase=LaunchPhase::Preparing;
        if(launch_child<0){launch_result=127;begin_launch_exit(false,now);}
    }
    void poll_launch(Uint32 now){
        if(!launching)return;
        if(launch_child>0){
            int result=0;auto finished=waitpid(launch_child,&result,WNOHANG);
            if(finished==launch_child || (finished<0 && errno!=EINTR)){
                launch_child=-1;launch_result=finished<0?127:WIFEXITED(result)?WEXITSTATUS(result):128+(WIFSIGNALED(result)?WTERMSIG(result):0);
                if(launch_phase==LaunchPhase::Playing){finish_launch(true);return;}
                begin_launch_exit(false,now);
            }else if(launch_cancelled && now-launch_cancel_at>=5000)kill(launch_child,SIGKILL);
        }
        if(launch_phase==LaunchPhase::Playing)return;
        if(launch_preview){
            if(launch_phase!=LaunchPhase::Exiting && now-launch_at>=650)begin_launch_exit(false,now);
        }else if(launch_phase==LaunchPhase::Waiting){
            // Reap helpers and drain pending controls via the ordinary event loop.
            // No sleeps, synchronous worker shutdown or control flush on START.
            if(worker>0){auto done=waitpid(worker,nullptr,WNOHANG);if(done>0 || (done<0 && errno!=EINTR))worker=-1;else if(now-launch_at>=1000)kill(worker,SIGKILL);}
            if(worker<=0 && !system.volume_pending() && !system.brightness_pending() && !volume_unsaved && !brightness_unsaved)start_launch_runner(now);
        }else if(launch_phase==LaunchPhase::Preparing && !launch_cancelled){
            char ready=0;auto received=recv(launch_channel,&ready,1,MSG_DONTWAIT);
            if(received==1 && ready=='R')begin_launch_exit(true,now);
        }
        if(launch_phase==LaunchPhase::Exiting && (!preferences.animations || now-launch_exit_at>=80)){
            if(launch_handoff && launch_child>0){
                // Let the finite launch sparkle finish while input/cancel remains live.
                if(sounds.active() && now-launch_at<1500)return;
                // Present the final fade frame, release SDL, then acknowledge ownership.
                draw(!o.screenshot.empty());close();launch_phase=LaunchPhase::Playing;
                int flags=0;
#ifdef MSG_NOSIGNAL
                flags=MSG_NOSIGNAL;
#endif
                const char released='C';if(send(launch_channel,&released,1,flags)!=1){kill(launch_child,SIGTERM);}
                ::close(launch_channel);launch_channel=-1;
            }else if(launch_child<=0)finish_launch(false);
        }
    }
    void stop_launch(){
        if(launch_channel>=0)::close(launch_channel);launch_channel=-1;
        if(launch_child>0){kill(launch_child,SIGTERM);for(int i=0;i<600;i++){auto done=waitpid(launch_child,nullptr,WNOHANG);if(done==launch_child || (done<0 && errno!=EINTR)){launch_child=-1;break;}usleep(10000);}if(launch_child>0){kill(launch_child,SIGKILL);while(waitpid(launch_child,nullptr,0)<0 && errno==EINTR){}}}
        launch_child=-1;
    }
    void action(int code) {
        if(shutting_down())return;
        SoundAction sound_action(*this,code);
        if(code==0 || code==1 || code==8 || code==9 || code==3)sound_code=code;
        motion_input(code);
        if(launching){if(code==3)cancel_launch();return;}
        if(chat_open){chat_action(code);return;}
        brightness_drag=false;volume_drag=false;
        last_activity=s.GetTicks();
        if(picker_open && picker_title=="Jump to letter" && (code==15 || code==16)){picker_action(3);if(code==16)quick_search();return;}
        if(picker_open){picker_action(code);return;}
        if(settings.page==Page::Keyboard && edit_field==4 && (code==15 || code==16)){finish_edit(false);if(code==15)jump_to_letter();return;}
        if(settings.page==Page::GameSwitcher){
            if(menu_status["state"]=="busy" || !menu_pending_token.empty())return;
            if(code==7)menu_request("resume");
            else if(code==3){settings.page=Page::QuickMenu;settings.focus=0;}
            else if(code==8 || code==9 || code==0 || code==1)settings.focus=std::clamp(settings.focus+(code==8 || code==0?-1:1),0,list_count()-1);
            else if(code==11 || ((code==2 || code==4) && settings.focus==int(switch_games.size()))){settings.page=Page::QuickMenu;settings.focus=0;}
            else if(code==2 || code==4)menu_request(settings.focus==0?"resume":"switch:"+std::to_string(settings.focus));
            dirty=true;return;
        }
        if((code==0 || code==1 || code==2 || code==4) && open_setting_picker())return;
        if(settings.page==Page::ShaderPicker) {
            if(code==3)close_shader_picker();
            else if(code==0 || code==1)focus_shader(settings.focus+(code==0?-1:1));
            else if(code==8 || code==9 || code==13 || code==14)shader_family_step(code==8 || code==13?-1:1);
            else if(code==2 || code==4)choose_shader();else if(code==11 || code==12)change_shader_preview(code);dirty=true;return;
        }
        if(settings.page==Page::MenuSounds){menu_sounds_action(code);return;}
        if(settings.page==Page::Colours){colours_action(code);return;}
        if(settings.page==Page::Motion){
            if(code==3)settings.back();
            else if(code==8 || code==9)settings.focus=std::clamp(settings.focus+(code==8?-1:1),0,3);
            else if(code==0 || code==1 || code==2){
                // Left/right step through the three choices; A moves to the next.
                auto step=[&](int value){return code==0?std::max(0,value-1):code==1?std::min(2,value+1):(value+1)%3;};
                if(settings.focus==2){
                    const int style=step(preferences.view_style==ViewStyle::Push?1:preferences.view_style==ViewStyle::Zoom?2:0);
                    preferences.view_style=style==1?ViewStyle::Push:style==2?ViewStyle::Zoom:ViewStyle::Spring;remember_appearance();dirty=true;return;
                }
                if(settings.focus==3){preferences.view_speed=step(preferences.view_speed);remember_appearance();dirty=true;return;}
                bool& value=settings.focus==0?preferences.animations:preferences.view_animations;
                value=code==2?!value:code==1;
                if(settings.focus==1 && value)preferences.animations=true;
                if(settings.focus==0 && !value)preferences.view_animations=false;
                remember_appearance();
                if(preferences.view_animations)start_view_motion(s.GetTicks());
            }dirty=true;return;
        }
        if(settings.page==Page::Microphone){if(code==3){stop_microphone();settings.page=Page::Sound;settings.focus=1;configure_sounds();dirty=true;}return;}
        if(settings.page==Page::Sound && (code==8 || code==9)){settings.focus=std::clamp(settings.focus+(code==8?-1:1),0,2);dirty=true;return;}
        if(settings.page==Page::Controls){controls_action(code);return;}
        if(settings.page==Page::SaveGallery || settings.page==Page::Discover || settings.page==Page::RomSetup || settings.page==Page::DSControls){
            auto page=settings.page;
            if(code==3){back_to(page==Page::SaveGallery?gallery_return:page==Page::DSControls?ds_return:page==Page::Discover?discover_return:Page::LibraryOptions);}
            else if(code==8 || code==9)settings.focus=std::clamp(settings.focus+(code==8?-1:1),0,list_count()-1);
            else if(code==2 || code==4){
                if(page==Page::SaveGallery)slot_actions();
                else if(page==Page::Discover){if(settings.focus==0)begin_edit(4);else if(settings.focus==1)show_picker("Filter",{"No filter","Unplayed","Played","Missing artwork"},discovery_filter,[this](int n){discovery_filter=n;apply_discovery();});else clear_discovery();}
                else if(page==Page::RomSetup){if(settings.focus==0)show_detail("Add your games",locale.text("Copy your .nds files or DS ZIP archives to Roms/NDS on the games card. Games are not included. Saves stay in userdata.") );else if(settings.focus==1){if(library_ready){setup_scanned=true;rescan_games(true);}}else{fs::create_directories(o.data);std::ofstream(o.data/"setup-complete").put('1');setup_scanned=false;if(setup_first_run){setup_first_run=false;settings.page=Page::Library;home_menu=true;}else back_to(Page::LibraryOptions);}}
                else ds_controls_action();
            }dirty=true;return;
        }
        if(o.game_menu) {
            if(settings.page==Page::Display && code==2){settings.page=Page::QuickMenu;settings.focus=6;dirty=true;return;}
            if(settings.page==Page::ApplyShader){
                if(code==3 || code==7){settings.page=Page::QuickMenu;settings.focus=3;}
                else if(code==8 || code==9)settings.focus=code==8?0:1;
                else if(code==2 || code==4){if(settings.focus==0)menu_request("restart");else{settings.page=Page::QuickMenu;settings.focus=3;}}
                dirty=true;return;
            }
            if(settings.page==Page::QuickMenu) {
                if(code==3 || code==7)menu_request("resume");
                else if(code==11)open_emulator_options();
                else if(code==8 || code==9){const std::array<int,11> up={0,0,0,10,1,2,3,4,5,6,10},down={1,4,5,6,7,8,9,7,8,9,3};settings.focus=(code==8?up:down).at(settings.focus);}
                else if(code==0 || code==1){if(settings.focus==0 || settings.focus==10)settings.focus=code==0?0:10;else{int column=(settings.focus-1)%3;if((code==0 && column>0) || (code==1 && column<2))settings.focus+=code==0?-1:1;}}
                else if(code==2 || code==4)quick_action();dirty=true;return;
            }
            if(settings.page==Page::LeaveGame) {
                if(code==3){settings.page=Page::QuickMenu;settings.focus=0;}
                else if(code==8 || code==9)settings.focus=std::clamp(settings.focus+(code==8?-1:1),0,2);
                else if(code==2){if(settings.focus==0){settings.page=Page::QuickMenu;menu_request("quit");}else menu_request(settings.focus==1?"resume":"force-quit");}dirty=true;return;
            }
            if(settings.page==Page::Graphics){
                if(code==3 || code==7){settings.page=Page::QuickMenu;settings.focus=3;}
                else if(code==8 || code==9){settings.focus=std::clamp(settings.focus+(code==8?-1:1),0,list_count()-1);status.clear();}
                else if(code==2 || code==4 || code==0 || code==1)graphics_action();
                dirty=true;return;
            }
            if(settings.page==Page::EmulatorOptions){
                if(code==3 || code==7){settings.page=Page::QuickMenu;settings.focus=4;}
                else if(code==8 || code==9)settings.focus=std::clamp(settings.focus+(code==8?-1:1),0,list_count()-1);
                else if(code==0 || code==1 || code==2)change_emulator_option(code==0?-1:1);
                dirty=true;return;
            }
            if(code==3 || code==7){settings.page=Page::QuickMenu;settings.focus=0;dirty=true;return;}
        }
        if(settings.page==Page::GameArtwork){
            if(code==3){back_to(artwork_return);if(artwork_return==Page::GameOptions)settings.focus=4;}
            else if(code==8 || code==9)settings.focus=std::clamp(settings.focus+(code==8?-1:1),0,2);
            else if(code==2)game_artwork_action();dirty=true;return;
        }
        if(settings.page==Page::Keyboard) {
            if(code==3 || code==7)finish_edit(false);else if(code==4)finish_edit(true);else if(code==2)keyboard_choose();
            else if(code==11)erase_character();
            else if(code==12)key_layer=key_layer==0?1:0;
            else keyboard_move(code);
            dirty=true;return;
        }
        if(settings.page==Page::Confirm) {
            if(code==3){back_to(confirm_return);}
            else if(code==0 || code==1)settings.focus=code==0?0:1;
            else if(code==2 || code==4){bool accepted=settings.focus==1;settings.page=confirm_return;settings.focus=0;
                if(accepted){if(confirmation=="return-stock")quit=true;else if(confirmation=="startup-rgds")save_startup();else{start_worker("device."+confirmation);if(confirmation=="poweroff" || confirmation=="reboot")begin_shutdown(confirmation);}}}
            dirty=true;return;
        }
        if((code==8 || code==9) && (settings.page==Page::Network || settings.page==Page::Wifi || settings.page==Page::System || settings.page==Page::Maintenance || settings.page==Page::GameOptions)) {
            int maximum=settings.page==Page::System?7:(settings.page==Page::GameOptions || settings.page==Page::Maintenance || settings.page==Page::Network)?list_count()-1:std::max(0,std::atoi(device_status["network_count"].c_str())-1);
            settings.focus=std::clamp(settings.focus+(code==8?-1:1),0,maximum);dirty=true;return;
        }
        if(code==7 && !probe){if(settings.page==Page::Library)settings.open();else settings.page=Page::Library;dirty=true;return;}
        if(!probe && settings.page!=Page::Library && code!=5 && code!=6) {
            if(code==3){bool game=settings.page==Page::GameOptions;settings.back();if(game)library_view(preferences.view);}
            else if(code==8 || code==9){settings.move(code==8?-1:1);if(settings.page==Page::Scraper)settings.focus=std::min(settings.focus,list_count()-1);}
            else if(settings.page==Page::Appearance) {
                if(code==0 || code==1 || code==2)choose_appearance(code);
            }else if(settings.page==Page::Power) {
                if(settings.focus<4 && (code==0 || code==1 || code==2))change_system(settings.focus,code==0?-1:1,code==2);
                else if(settings.focus==4 && code==2)sleep_now();
                else if(settings.focus==5 && code==2 && hardware.poweroff)confirm("poweroff",Page::Power);
            }else if(settings.page==Page::Display) {
                if(code==0 || code==1)change_system(4,code==0?-1:1);else if(code==2){flush_brightness();settings.back();}
            }else if(settings.page==Page::Sound) {
                if(code==0 || code==1)change_volume(hardware.volume_percent+(code==0?-5:5));
                else if(code==2){if(settings.focus==2){settings.page=Page::MenuSounds;settings.focus=0;}else if(settings.focus==1)open_microphone();else toggle_mute();}
            }else if(settings.page==Page::LibraryOptions) {
                if(settings.focus==0 && (code==0 || code==1 || code==2)) {
                    {int core=int(preferences.core);do core=code==2?(core+1)%core_count:std::clamp(core+(code==0?-1:1),0,core_count-1);while(!core_offered(core));preferences.core=unsigned(core);}remember_appearance();
                }else if(settings.focus==1 && code==2)open_scraper();
                else if(settings.focus<4 && settings.focus>=2 && (code==0 || code==1 || code==2)) {
                    bool& value=settings.focus==2?preferences.auto_load:preferences.auto_save;value=code==2?!value:code==1;remember_appearance();
                }
                else if(settings.focus==4 && (code==0 || code==1 || code==2))open_shader_picker(false);
                else if(settings.focus==7 && code==2){open_discover();}
                else if(settings.focus==8 && code==2){settings.page=Page::RomSetup;settings.focus=0;}
                else if(settings.focus==9 && code==2){if(worker<=0){backup_requested=true;start_worker("device.library","{\"action\":\"backup\"}");}}
                else if(settings.focus==5 && (code==0 || code==1 || code==2)){preferences.layout=(preferences.layout+(code==0?2:1))%3;remember_appearance();}
            }else if(settings.page==Page::GameOptions) {
                if(code==0 || code==1 || code==2){if(settings.focus==list_count()-2)open_gallery();else if(settings.focus==list_count()-1)open_ds_controls();else if(settings.focus==4)open_game_artwork();else if(settings.focus==3)open_controls(true,Page::GameOptions);else if(settings.focus==2)open_shader_picker(true);else change_game_option(code==0?-1:1,code==2);}
            }else if(settings.page==Page::System) {
                if(code==2){if(settings.focus==1){settings.page=Page::Timezone;auto found=std::find(timezones.begin(),timezones.end(),system_preferences.timezone);settings.focus=int(found-timezones.begin());}
                    else if(settings.focus==4){settings.page=Page::DeviceInfo;settings.focus=0;start_worker("device.info");}
                    else if(settings.focus==5){settings.page=Page::Maintenance;settings.focus=0;start_worker("device.update-info");}
                    else if(settings.focus==6)confirm("return-stock",Page::System);else if(settings.focus==7){settings.page=Page::Startup;settings.focus=0;start_worker("device.startup-info");}}
            }else if(settings.page==Page::Startup){if(code==0 || code==1 || code==2)change_startup(code==0?-1:1,code==2);
            }else if(settings.page==Page::Network) {
                if(code==2){if(settings.focus==0){if(device_status["wifi"]=="1")confirm("wifi-off",Page::Network);else start_worker("device.wifi-on");}
                    else if(settings.focus==1){settings.page=Page::Wifi;settings.focus=0;start_worker("device.scan");}
                    else if(settings.focus==2)start_worker("device.info");
                    else if(settings.focus==3)show_sharing_picker();
                    else if(settings.focus==4 && device_status["sharing"]=="1"){
                        auto login=fields(device_status["sharing_login"]);
                        if(device_status["sharing_mode"]=="open")
                            show_detail("Sharing details","Mac: Finder > Network > "+(device_status["sharing_name"].empty()?std::string("RG DS"):device_status["sharing_name"])+"\nOr connect to "+device_status["sharing_address"]+"\nWindows: \\\\"+device_status["ip"]+"\nNo password needed: connect as Guest.\nFolders: "+login["shares"]+"\nAnyone on this Wi-Fi can open these folders. Use Password protected on shared networks.");
                        else show_detail("Sharing details",device_status["sharing_address"]+"\nWindows: \\\\"+device_status["ip"]+"\nUsername: "+login["username"]+"\nPassword: "+login["password"]+"\nFolders: "+login["shares"]+"\nLogin changes each time sharing is turned on.");}}
            }else if(settings.page==Page::Wifi) {
                if(code==2)wifi_choose();else if(code==11)start_worker("device.scan");
            }else if(settings.page==Page::Maintenance) {
                if(code==2){bool busy=worker>0 && worker_action.rfind("device.update-",0)==0;
                    if(busy){if(settings.focus==2 && worker_action=="device.update-download")stop_worker();}
                    else if(settings.focus==0)start_worker("device.update-check");
                    else if(settings.focus==1){if(update_status["prepared"]=="1")confirm("update-apply",Page::Maintenance);else if(update_status["state"]=="available" || update_status["state"]=="cancelled" || update_status["state"]=="error")start_worker("device.update-download");}
                    else if(settings.focus==2)show_detail("Release notes",update_status["notes"].empty()?locale.text("No release notes yet."):update_status["notes"]);
                    else if(settings.focus==3){if(device_status["previous"].empty())service_notice="No previous build available.";else confirm("rollback",Page::Maintenance);}
                    else if(settings.focus==6)show_picker("Automatic updates",{"Off","Check automatically","Download automatically"},std::clamp(std::atoi(experience["auto_update"].c_str()),0,2),[this](int n){save_experience("auto_update",n);});
                    else if(settings.focus==4)confirm("clear-cache",Page::Maintenance);else if(update_status["prepared"]=="1")start_worker("device.update-discard");else start_worker("device.diagnostics");}
            }else if(settings.page==Page::Timezone) {
                if(code==2 || code==0 || code==1){if(code!=2)settings.move(code==0?-1:1);save_timezone();}
            }else if(settings.page==Page::Scraper) {
                if(code==2)scraper_action();
            }else if(code==2){if(settings.page==Page::Settings && settings.section==8){if(hardware.poweroff)confirm("poweroff",Page::Settings);}
                else if(settings.page==Page::Settings && settings.section==6)open_controls(false);else if(settings.page==Page::DeviceInfo)open_device_detail();else settings.enter();}
            dirty=true;return;
        }
        if(!home_menu && (code==15 || code==16)){if(code==15)jump_to_letter();else quick_search();return;}
        if(home_menu && preferences.home_layout==1 && (code==0 || code==1 || code==8 || code==9)){home_column_move(code);return;}
        if(!home_menu && (code==13 || code==14)){navigate(code==13?-1:1);return;}
        if(!home_menu && preferences.layout==2 && (code==8 || code==9)){navigate(code==8?-1:1);return;}
        if(code==0)navigate(-1);
        else if(code==1)navigate(1);
        else if(code==2)launch();
        else if(code==3){if(probe)quit=true;else if(!home_menu)return_home();}
        else if(code==4)launch();
        else if(code==5)probe=!probe;
        else if(code==6) {
            try{reload_themes();status="THEME RELOADED";std::cout<<"Theme reloaded: "<<o.theme<<std::endl;}
            catch(const std::exception& e){status="THEME ERROR - RETAINED CURRENT";std::cerr<<e.what()<<std::endl;}
        }
        else if(code==11 && !home_menu)open_game_options();
        else if(code==12 && !home_menu)choose_library_view();
        dirty=true;
    }
    void touch(int x,int y) {
        if(shutting_down())return;
        motion_input(2);
        last_activity=s.GetTicks();
        brightness_drag=false;volume_drag=false;
        touch_x=x;touch_y=y;last_input="TOUCH "+std::to_string(x)+", "+std::to_string(y);dirty=true;
        if(probe)return;
        if(picker_open){picker_touch(x,y);return;}
        if(settings.page==Page::GameSwitcher && y>=424 && x>=200 && x<440){action(11);return;}
        if(settings.page!=Page::Library && x>=24 && x<=76 && y>=8 && y<=72){action(3);return;}
        if(settings.page==Page::ShaderPicker){
            if(y>=24 && y<=66){if(x>=368 && x<=488)change_shader_preview(11);else if(x>=496 && x<=624)change_shader_preview(12);return;}
            for(int f=0;f<shader_family_count;f++)if(point_in(shader_chip(f),x,y)){shader_family_jump(f);return;}
            return;
        }
        if(settings.page!=Page::Library && y>=424){if(x<200)action(3);else if(x>=440)action(settings.page==Page::Keyboard?4:2);return;}
        if(settings.page==Page::Controls && control_capture){controls_action(3);return;}
        if(list_count()>0 || settings.page==Page::Wifi){
            sync_list();if(settings.page==Page::Wifi && x>=476 && y>=24 && y<=66){start_worker("device.scan");return;}if(x>=596 && y>=list_top && y<list_top+list_height){int thumb=std::max(54,list_height*list_height/std::max(1,list_count()*list_stride-16));
                int top=list_top+(list_max_scroll()?list_scroll*(list_height-thumb)/list_max_scroll():0);
                scroll_grab=y>=top && y<top+thumb?y-top:thumb/2;
                scroll_drag=true;scroll_cue_at=s.GetTicks();micro_until=scroll_cue_at+90;scroll_to(y);return;}
            if(x>=24 && x<590 && y>=list_top && y<list_top+list_height){int local=y-list_top+list_scroll,index=local/list_stride;if(local%list_stride<66 && index<list_count()){
                if(settings.page==Page::Settings)settings.section=index;else settings.focus=index;
                if(settings.page==Page::EmulatorOptions){if(!open_setting_picker())change_emulator_option(x<420?-1:1);}
                else if(settings.page==Page::Appearance && (index==0 || index==3) && x>=segment_bounds(index,0).x-4)choose_appearance(x>=segment_bounds(index,1).x?1:0);
                else action(2);
            }}return;
        }
        if(settings.page==Page::Colours){colours_touch(x,y);return;}
        if(settings.page==Page::Motion){
            if(x<28 || x>612 || y<motion_row(0) || y>=motion_row(4) || (y-motion_row(0))%82>=72)return;
            settings.focus=(y-motion_row(0))/82;
            if(settings.focus<2){action(2);return;}
            // A tap on a choice selects it directly.
            for(int i=0;i<3;i++)if(x>=motion_segment_x(i) && x<motion_segment_x(i)+104){
                if(settings.focus==2)preferences.view_style=i==1?ViewStyle::Push:i==2?ViewStyle::Zoom:ViewStyle::Spring;else preferences.view_speed=i;
                remember_appearance();dirty=true;
            }
            return;
        }
        if(settings.page==Page::ApplyShader){if(x>=28 && x<=612 && y>=174 && y<326){int index=(y-174)/86;if((y-174)%86<66){settings.focus=index;action(2);}}return;}
        if(settings.page==Page::QuickMenu) {
            if(x>=28 && x<=612) {
                if(y>=96 && y<=152){if(x>312 && x<328)return;settings.focus=x>=328?10:0;}
                else if(y>=166 && y<402 && (y-166)%82<72 && (x-28)%200<184)settings.focus=1+((y-166)/82)*3+(x-28)/200;else return;
                quick_action();
            }return;
        }
        if(settings.page==Page::LeaveGame){if(x>=28 && x<=612 && y>=210 && y<408){int index=(y-210)/70;if((y-210)%70<58){settings.focus=index;action(2);}}return;}
        if(settings.page!=Page::Library){
            if(settings.page==Page::Keyboard){
                if(!edit_buffer.empty() && point_in({568,103,40,38},x,y)){edit_buffer.clear();edit_cursor=0;search_changed();return;}
                auto rows=keyboard_rows();
                for(size_t row=0;row<rows.size();row++)for(size_t col=0;col<rows[row].size();col++)if(point_in(rows[row][col].bounds,x,y)){key_row=int(row);key_col=int(col);keyboard_choose();return;}
            }else if(settings.page==Page::Display && x>=44 && x<=596 && y>=slider_band().first && y<=slider_band().second){brightness_drag=true;int value=std::clamp(5+int(std::round((x-56)*19.0/528))*5,5,100);change_system(4,value/5-system_preferences.brightness/5);}
            else if(settings.page==Page::Sound){if(x>=44 && x<=596 && y>=slider_band().first && y<=slider_band().second){volume_drag=true;change_volume(std::clamp(int(std::round((x-56)*20.0/528))*5,0,100));}else if(x>=28 && x<=612 && y>=sound_row(0) && y<sound_row(2)+66 && (y-sound_row(0))%74<66){settings.focus=(y-sound_row(0))/74;action(2);}}
            else if(settings.page==Page::Confirm && y>=365 && y<=413 && x>=28 && x<=612){settings.focus=x>=332?1:0;action(2);}dirty=true;return;
        }
        if(home_menu && preferences.home_layout==1){home_column_touch(x,y);return;}
        if(!home_menu){
            auto header=library_header_layout();
            auto inside=[&](SDL_Rect bounds){return x>=bounds.x && x<bounds.x+bounds.w && y>=bounds.y && y<bounds.y+bounds.h;};
            if(inside(header.back))action(3);
            else if(inside(header.view))action(12);
            else if(inside(header.options))action(11);
            else if(discovery_active() && inside(header.clear))clear_discovery();
            else if(inside(header.search))quick_search();
            else if(discovery_active() && model.games.empty() && point_in({80,326,232,48},x,y))quick_search();
            else if(discovery_active() && model.games.empty() && point_in({328,326,232,48},x,y))clear_discovery();
        }
    }
    int cover_hit(int x,int y) {
        int play_width=std::max(92,label_width("Play",16)+48);
        if(y>=383 && y<407 && x>=320-play_width/2 && x<320+play_width/2)return int(model.selected);
        int found=-1;double nearest=10;
        for(int index=std::max(0,int(std::floor(carousel.position))-3);index<std::min(int(navigation_count()),int(std::floor(carousel.position))+4);index++){
            double distance=index-carousel.position,depth=std::min(2.0,std::abs(distance));
            int size=int(std::lround(224-depth*34)),left=int(std::lround(320+distance*carousel.item_spacing))-size/2,top=151+int(depth*18);
            auto bounds=media.fitted_box(surfaces.back().renderer,item_folder(size_t(index))/"box.bmp",{left,top,size,size});
            if(x>=bounds.x-3 && x<bounds.x+bounds.w+3 && y>=bounds.y-3 && y<bounds.y+bounds.h+3 && std::abs(distance)<nearest){found=index;nearest=std::abs(distance);}
        }
        return found;
    }
    LibraryViewport browser_list;
    bool browser_list_touch=false,browser_rail=false,browser_hover=false;
    int browser_scroll_start=0,browser_grab=0;
    void pointer_begin(int x,int y) {
        if(chat_open){
            view_touch_dx=view_touch_dy=0;contact_x=down_x=x;contact_y=down_y=y;pointer_down=true;touch_tail=false;pointer_at=s.GetTicks();
            // As elsewhere, an edge touch waits: a swipe goes Home or Back, a release where it began is a plain tap.
            edge_gesture=gestures_available()?(y>=470?EdgeGesture::Bottom:x<=10?EdgeGesture::Left:EdgeGesture::None):EdgeGesture::None;
            if(edge_gesture==EdgeGesture::None)chat_press(x,y);
            return;
        }
        hold.release(4000);
        if(launching){pointer_source=PointerSource::None;return;}
        // Hit-test the view actually under the finger, then settle it so a
        // held gesture is never chasing moving controls. Preserve its offset.
        motion_touch=true;
        auto point=view_motion.unproject(x,y,view_drawn_at);
        // Pixels belonging to the departing page cannot activate the new page.
        if(preferences.view_style==ViewStyle::Push && view_motion.running && view_previous_ready){
            auto layers=view_motion.push(view_drawn_at);
            if(point.first<0 || point.first>=640 || (!layers.incoming_above && x>=layers.outgoing)){motion_touch=false;pointer_source=PointerSource::None;return;}
        }
        point=motion_hits.unproject(point.first,point.second);
        view_touch_dx=point.first-x;view_touch_dy=point.second-y;
        x=point.first;y=point.second;view_motion.stop();
        contact_x=x;contact_y=y;touch_tail=false;options_ready=false;dirty=true;
        edge_gesture=gestures_available()?(y>=470?EdgeGesture::Bottom:x<=10?EdgeGesture::Left:EdgeGesture::None):EdgeGesture::None;
        pointer_down=true;scroll_drag=false;picker_rail=picker_swipe=false;detail_drag=detail_rail=false;list_touch=list_swipe=tap_pending=false;down_x=x;down_y=y;pressed_game=navigation_selected();pointer_at=s.GetTicks();
        // A touch that begins on a gesture edge belongs to the gesture until it
        // is released: a plain release there still counts as a tap.
        if(edge_gesture!=EdgeGesture::None){tap_pending=true;return;}
        mark_library_press(x,y);
        if(settings.page==Page::BootWarning){boot_touch_candidate=boot_displayed && !boot_touch_wait_release && x>=0 && x<640 && y>=0 && y<480;return;}
        if(o.boot_startup && (!session_initialized || boot_touch_wait_release)){boot_touch_wait_release=true;return;}
        browser_list_touch=browser_rail=false;
        carousel.item_spacing=!home_menu && preferences.layout==1?158:Carousel::spacing;
        if(picker_open && !picker_detail){
            tap_pending=true;
            if(picker_scrollable() && y>=picker_top()+64 && y<picker_top()+64+picker_height()){
                picker_scroll_start=picker_scroll;
                if(picker_max_scroll()>0 && x>=572 && x<604){
                    tap_pending=false;picker_rail=true;int local=y-picker_top()-64,offset=picker_thumb_offset();
                    picker_grab=local>=offset && local<offset+picker_thumb()?local-offset:picker_thumb()/2;
                    scroll_cue_at=s.GetTicks();micro_until=scroll_cue_at+90;seek_picker(y);
                }else if(x>=40 && x<560)picker_swipe=true;
            }return;
        }
        if(!probe && !home_menu && settings.page==Page::Library && preferences.layout==2 && y>=LibraryViewport::top && y<LibraryViewport::top+LibraryViewport::height){
            browser_list.moving=false;
            if(x>=592 && x<628 && navigation_count()){
                scroll_cue_at=s.GetTicks();
                browser_rail=true;browser_grab=y>=browser_list.thumb_top(navigation_count()) && y<browser_list.thumb_top(navigation_count())+browser_list.thumb(navigation_count())?y-browser_list.thumb_top(navigation_count()):browser_list.thumb(navigation_count())/2;
                browser_list.seek(y,browser_grab,navigation_count());model.selected=size_t(browser_list.first());carousel.reset(model.selected);return;
            }
            browser_list_touch=true;browser_scroll_start=browser_list.scroll;tap_pending=true;return;
        }
        if(!probe && home_menu && settings.page==Page::Library && preferences.home_layout==1){tap_pending=true;return;}
        int left_arrow_x=0,right_arrow_x=556;
        if(!probe && settings.page==Page::Library && (home_menu || preferences.layout!=2) && navigation_count() && y>=409 && y<480){if(x>=left_arrow_x && x<left_arrow_x+84){carousel.sync_input(pointer_at);hold.press(4000,-1,pointer_at);navigate(-1);return;}if(x>=right_arrow_x && x<right_arrow_x+84){carousel.sync_input(pointer_at);hold.press(4000,1,pointer_at);navigate(1);return;}}
        bool horizontal_seek=x>=84 && x<556;
        if(!probe && settings.page==Page::Library && navigation_count() && ((y>=LibraryViewport::top && y<LibraryViewport::top+LibraryViewport::height) || (horizontal_seek && y>=409 && y<480))) {
            if(y>=409)carousel.seek_down(x,y,navigation_count(),pointer_at);
            else carousel.down(x,y,false,pointer_at);
            if(carousel.range){auto selected=carousel.drag(x,navigation_count());if(home_menu)home_selected=selected;else {model.selected=selected;browser_list.reveal(selected,navigation_count());}selected_at=s.GetTicks();dirty=true;}return;
        }
        if(picker_open && picker_detail && y>=164 && y<398){
            detail_drag=true;detail_rail=x>=588;gesture_scroll=detail_scroll;if(detail_rail)scroll_detail_rail(y);return;
        }
        if(!probe && settings.page==Page::ShaderPicker && shader_pointer_down(x,y))return;
        if(!probe && settings.page!=Page::Library){
            bool slider=!picker_open && (settings.page==Page::Display || settings.page==Page::Sound) && x>=44 && x<=596 && y>=slider_band().first && y<=slider_band().second;
            bool rail=!picker_open && list_count()>0 && x>=596 && y>=list_top && y<list_top+list_height;
            if(!slider && !rail){tap_pending=true;list_touch=!picker_open && list_count()>0 && y>=list_top && y<list_top+list_height;sync_list();gesture_scroll=list_scroll;return;}
        }
        if(!probe && !home_menu && settings.page==Page::Library && y<LibraryViewport::top){tap_pending=true;return;}
        touch(x,y);
    }
    void pointer_move(int x,int y) {
        if(chat_open){if(pointer_down){contact_x=x;contact_y=y;if(edge_gesture!=EdgeGesture::None)edge_gesture_move(x,y);else chat_drag(x,y);dirty=true;}return;}
        x+=view_touch_dx;y+=view_touch_dy;
        if(!pointer_down)return;
        if(contact_x!=x || contact_y!=y){contact_x=x;contact_y=y;dirty=true;}
        if(edge_gesture_move(x,y))return;
        if(!library_control_active(LibraryPress::LeftArrow,rail_left_bounds()) && !library_control_active(LibraryPress::RightArrow,rail_right_bounds()))hold.release(4000);
        if(settings.page==Page::BootWarning)return;
        if(shader_drag){
            if(std::abs(y-down_y)>12)shader_carousel.moved=true;
            int index=int(shader_carousel.drag(x,shader_choices.size()));if(index!=settings.focus){settings.focus=index;selected_at=s.GetTicks();}
            dirty=true;return;
        }
        if(picker_open && picker_rail){seek_picker(y);return;}
        if(picker_open && picker_swipe){
            if(std::abs(y-down_y)>8){tap_pending=false;scroll_picker(picker_scroll_start+down_y-y);}return;
        }
        if(detail_drag){if(detail_rail)scroll_detail_rail(y);else detail_scroll=std::clamp(gesture_scroll+(down_y-y)/28,0,detail_max_scroll());dirty=true;return;}
        if(browser_rail){browser_list.seek(y,browser_grab,navigation_count());model.selected=size_t(browser_list.first());carousel.reset(model.selected);dirty=true;return;}
        if(browser_list_touch){if(std::abs(y-down_y)>12){
            tap_pending=false;browser_list.scroll=browser_scroll_start+down_y-y;browser_list.clamp(navigation_count());
            if(navigation_count()){model.selected=size_t(browser_list.first());carousel.reset(model.selected);}dirty=true;
        }return;}
        if(list_touch){if(std::abs(y-down_y)>8)list_swipe=true;if(list_swipe){tap_pending=false;list_scroll=std::clamp(gesture_scroll+down_y-y,0,list_max_scroll());dirty=true;}return;}
        if(tap_pending && (std::abs(x-down_x)>12 || std::abs(y-down_y)>12))tap_pending=false;
        if(scroll_drag){scroll_to(y);return;}
        if(carousel.dragging) {
            if(std::abs(y-down_y)>12)carousel.moved=true;
            auto selected=carousel.drag(x,navigation_count());
            if(selected!=navigation_selected()){if(home_menu)home_selected=selected;else {model.selected=selected;browser_list.reveal(selected,navigation_count());}selected_at=s.GetTicks();}
            dirty=true;
        }else if(brightness_drag || volume_drag)touch(std::clamp(x,44,596),std::clamp(y,slider_band().first,slider_band().second));
        else if(probe){touch_x=x;touch_y=y;last_input="Touch "+std::to_string(x)+", "+std::to_string(y);dirty=true;}
    }
    void pointer_end(int x,int y) {
        if(chat_open){if(pointer_down){if(edge_gesture!=EdgeGesture::None){edge_gesture=EdgeGesture::None;chat_press(down_x,down_y);}chat_release();pointer_down=false;pointer_source=PointerSource::None;dirty=true;}return;}
        hold.release(4000);
        x+=view_touch_dx;y+=view_touch_dy;
        if(!pointer_down)return;edge_gesture=EdgeGesture::None;release_at=s.GetTicks();touch_tail=preferences.touch_indicator && preferences.animations;dirty=true;pointer_down=false;pointer_source=PointerSource::None;scroll_drag=false;picker_rail=picker_swipe=false;detail_drag=detail_rail=false;brightness_drag=false;volume_drag=false;
        if(o.boot_startup && boot_touch_wait_release){boot_touch_wait_release=false;boot_touch_candidate=false;return;}
        if(settings.page==Page::BootWarning){bool accept=boot_touch_candidate && !boot_touch_wait_release;boot_touch_candidate=false;boot_touch_wait_release=false;if(accept)continue_boot();return;}
        if(shader_drag){
            shader_drag=false;shader_carousel.dragging=false;
            if(shader_carousel.moved || shader_carousel.range){focus_shader(settings.focus);return;}
            const int index=int(shader_carousel.tapped(x,shader_choices.size()));
            const bool apply=index==settings.focus && std::abs(shader_carousel.position-index)<.08 && y>=128 && y<330;
            focus_shader(index);if(apply)choose_shader();return;
        }
        if(browser_rail){browser_rail=false;return;}
        if(browser_list_touch){browser_list_touch=false;bool tap=tap_pending && std::abs(x-down_x)<=12 && std::abs(y-down_y)<=12;tap_pending=false;
            int hit=browser_list.row_at(y,model.games.size());
            if(tap && x>=24 && x<592 && hit>=0){size_t index=size_t(hit);select_game(index,false);if(index==pressed_game){if(s.GetTicks()-pointer_at>=600)open_game_options();else launch();}}return;
        }
        bool tapped=tap_pending && std::abs(x-down_x)<=12 && std::abs(y-down_y)<=12;
        tap_pending=list_touch=list_swipe=false;
        if(tapped){touch(x,y);brightness_drag=volume_drag=scroll_drag=false;return;}
        if(carousel.dragging) {
            carousel.dragging=false;
            if(carousel.moved || carousel.range){select_navigation(navigation_selected());return;}
            long raw=!home_menu && preferences.layout==1?cover_hit(x,y):long(std::llround(carousel.position+(x-320)/carousel.item_spacing));
            if(raw<0 || raw>=long(navigation_count())){carousel.select(navigation_selected(),s.GetTicks());dirty=true;return;}
            size_t index=size_t(raw);
            bool launch_selected=index==pressed_game && std::abs(carousel.position-index)<0.08 && y>=LibraryViewport::top && y<LibraryViewport::top+LibraryViewport::height;
            select_navigation(index);if(launch_selected){if(!home_menu && s.GetTicks()-pointer_at>=600)open_game_options();else launch();}
        }
    }
    bool mouse_position(Uint32 window,int& x,int& y) {
        (void)x;
        if(o.preview){if(y<480 && !pointer_down)return false;y-=480;return true;}
        return surfaces.size()>1 && window==surfaces[1].id;
    }
    void raw_touch(const TouchPoint& point,Uint32 now){
        SoundAction sound_action(*this,2);
        if(!departure_action.empty())return;
        if(o.boot_startup && boot_touch_wait_release && (point.kind==2 || point.kind==3)){boot_touch_wait_release=false;boot_touch_candidate=false;cancel_navigation();return;}
        if(o.game_menu && point.kind!=1)menu_input="touch "+std::string(point.kind==0?"down":point.kind==2?"up":"cancel")+" at "+std::to_string(point.x)+","+std::to_string(point.y);
        if(point.kind==0){if(pointer_down && pointer_source!=PointerSource::Raw)return;pointer_source=PointerSource::Raw;}
        else if(pointer_source!=PointerSource::Raw)return;
        raw_touch_seen=true;raw_touch_at=now;last_activity=now;auto before=settings.page;
        if(point.kind==0)pointer_begin(point.x,point.y);else if(point.kind==1)pointer_move(point.x,point.y);else if(point.kind==2)pointer_end(point.x,point.y);else{cancel_navigation();brightness_drag=volume_drag=false;carousel.select(navigation_selected(),now);}
        if(before!=settings.page){page_at=now;hold.clear();}dirty=true;
    }
    MainlineControls mainline_controls;
    std::set<SDL_JoystickID> mainline_pads;
    void event(SDL_Event e) {
        if(o.mainline && (e.type==SDL_JOYBUTTONDOWN || e.type==SDL_JOYBUTTONUP) && (o.preview || mainline_pads.count(e.jbutton.which)) && !mainline_controls.normalize(e))return;
        capture_view_for_input(e);
        if(o.game_menu)note_menu_input(e);
        auto before=settings.page;const int before_focus=settings.focus;handle_input(e);
        if(before!=settings.page){settings.remember(before,before_focus);if(before==Page::LibraryOptions)backup_requested=false;page_at=s.GetTicks();hold.clear();dirty=true;}
    }
    void handle_input(const SDL_Event& e) {
        if(card_repair){
            if(e.type==SDL_QUIT)quit=true;
            // The check reboots by itself; only the give-up notice is dismissable.
            if(card_repair==2 && (e.type==SDL_KEYDOWN || e.type==SDL_JOYBUTTONDOWN || e.type==SDL_MOUSEBUTTONDOWN || e.type==SDL_FINGERDOWN)){card_repair=0;card_repair_dismissed=true;dirty=true;}
            return;
        }
        SoundAction sound_action(*this,2);
        if(!departure_action.empty())return;
        if(e.type==SDL_KEYDOWN || e.type==SDL_JOYBUTTONDOWN || e.type==SDL_JOYHATMOTION || e.type==SDL_JOYAXISMOTION || e.type==SDL_MOUSEWHEEL)motion_touch=false;
        if(launching){
            if(e.type==SDL_QUIT)quit=true;
            const char* mapped=std::getenv("RGDS_BUTTON_B");
            if((e.type==SDL_KEYDOWN && (e.key.keysym.sym==SDLK_ESCAPE || e.key.keysym.sym==SDLK_b)) || (e.type==SDL_JOYBUTTONDOWN && e.jbutton.button==(mapped?std::atoi(mapped):1)))action(3);
            return;
        }
        if(!touchscreen.available() && o.boot_startup && boot_touch_wait_release && (e.type==SDL_FINGERUP || e.type==SDL_MOUSEBUTTONUP)){boot_touch_wait_release=false;boot_touch_candidate=false;cancel_navigation();return;}
        if(e.type==SDL_KEYUP && boot_blocked_keys.erase(e.key.keysym.sym))return;
        if(e.type==SDL_JOYBUTTONUP && boot_blocked_buttons.erase({e.jbutton.which,e.jbutton.button}))return;
        if(e.type==SDL_KEYDOWN && boot_blocked_keys.count(e.key.keysym.sym))return;
        if(e.type==SDL_JOYBUTTONDOWN && boot_blocked_buttons.count({e.jbutton.which,e.jbutton.button}))return;
        if(e.type==SDL_JOYHATMOTION && menu_held_hats.count(e.jhat.which)){if(e.jhat.value==SDL_HAT_CENTERED)menu_held_hats.erase(e.jhat.which);return;}
        bool anbernic_key=e.type==SDL_KEYDOWN && (e.key.keysym.sym==SDLK_HOME || e.key.keysym.sym==SDLK_AC_HOME || e.key.keysym.sym==SDLK_AC_BACK || e.key.keysym.sym==SDLK_MENU);
        bool anbernic_button=e.type==SDL_JOYBUTTONDOWN && joystick_source(e.jbutton.button)==control_menu;
        if(chat_open){if(anbernic_key || anbernic_button){chat_action(3);return;}if(chat_event(e))return;}
        if(!o.game_menu && session_initialized && settings.page!=Page::BootWarning && (anbernic_key || anbernic_button)){
            if(!anbernic_key || !e.key.repeat)launcher_search_shortcut();return;
        }
        if(settings.page==Page::Controls && control_capture){
            if(e.type==SDL_WINDOWEVENT && e.window.event==SDL_WINDOWEVENT_FOCUS_LOST){controls_action(3);return;}
            if(e.type==SDL_KEYDOWN && !e.key.repeat && e.key.keysym.sym!=SDLK_VOLUMEUP && e.key.keysym.sym!=SDLK_VOLUMEDOWN){
                if(e.key.keysym.sym==SDLK_ESCAPE || e.key.keysym.sym==SDLK_HOME || e.key.keysym.sym==SDLK_AC_HOME || e.key.keysym.sym==SDLK_MENU){controls_action(3);return;}
                const std::map<SDL_Keycode,int> keys={{SDLK_a,0},{SDLK_b,1},{SDLK_x,2},{SDLK_y,3},{SDLK_l,4},{SDLK_r,5},{SDLK_RETURN,6},{SDLK_TAB,7},{SDLK_UP,8},{SDLK_DOWN,9},{SDLK_LEFT,10},{SDLK_RIGHT,11},{SDLK_q,12},{SDLK_e,13},{SDLK_z,14},{SDLK_c,15},{SDLK_BACKSPACE,16}};
                auto found=keys.find(e.key.keysym.sym);if(found!=keys.end()){capture_control(found->second);boot_blocked_keys.insert(e.key.keysym.sym);}return;
            }
            if(e.type==SDL_JOYBUTTONDOWN){int source=joystick_source(e.jbutton.button);if(source==control_menu)controls_action(3);else if(source>=0){capture_control(source);boot_blocked_buttons.insert({e.jbutton.which,e.jbutton.button});}return;}
            if(e.type==SDL_JOYHATMOTION){int source=hat_source(e.jhat.value);if(source>=0)capture_control(source);return;}
        }
        if(o.boot_startup && !o.boot_warning && !session_initialized){if(e.type==SDL_KEYDOWN){boot_blocked_keys.insert(e.key.keysym.sym);return;}if(e.type==SDL_JOYBUTTONDOWN){boot_blocked_buttons.insert({e.jbutton.which,e.jbutton.button});return;}}
        if(settings.page==Page::BootWarning){
            bool fresh=boot_displayed && (!e.common.timestamp || int32_t(e.common.timestamp-boot_displayed_at)>=0);
            if(e.type==SDL_KEYDOWN){if(e.key.keysym.sym==SDLK_ESCAPE || e.key.keysym.sym==SDLK_b)return;if(e.key.keysym.sym==SDLK_a || e.key.keysym.sym==SDLK_RETURN){boot_blocked_keys.insert(e.key.keysym.sym);if(fresh && !e.key.repeat)continue_boot();}return;}
            if(e.type==SDL_JOYBUTTONDOWN){const char* a=std::getenv("RGDS_BUTTON_A");if(e.jbutton.button==(a?std::atoi(a):0) || e.jbutton.button==7){boot_blocked_buttons.insert({e.jbutton.which,e.jbutton.button});if(fresh)continue_boot();}return;}
        }
        if(s.GetTicks()<input_guard_until && (e.type==SDL_KEYDOWN || e.type==SDL_JOYBUTTONDOWN || e.type==SDL_JOYHATMOTION || e.type==SDL_JOYAXISMOTION || e.type==SDL_FINGERDOWN || e.type==SDL_MOUSEBUTTONDOWN))return;
        const char* menu_external=std::getenv("RGDS_MENU_EXTERNAL");bool owned_menu_keys=embedded_menu && menu_external && std::string(menu_external)=="1";
        if(owned_menu_keys && e.type==SDL_KEYDOWN && (e.key.keysym.sym==SDLK_HOME || e.key.keysym.sym==SDLK_AC_HOME || e.key.keysym.sym==SDLK_AC_BACK || e.key.keysym.sym==SDLK_MENU))return;
        if(owned_menu_keys && (e.type==SDL_JOYBUTTONDOWN || e.type==SDL_JOYBUTTONUP) && e.jbutton.button==8)return;
        if(o.game_menu && !owned_menu_keys && anbernic_key){
            if(!e.key.repeat && menu_status["state"]!="busy" && menu_pending_token.empty()){picker_open=false;cancel_navigation();settings.page=Page::QuickMenu;settings.focus=0;dirty=true;}return;
        }
        if(e.type==SDL_KEYDOWN && (e.key.keysym.sym==SDLK_VOLUMEUP || e.key.keysym.sym==SDLK_VOLUMEDOWN)) {
            if(!volume_keys.available() && !std::getenv("RGDS_VOLUME_EXTERNAL"))change_volume(hardware.volume_percent+(e.key.keysym.sym==SDLK_VOLUMEUP?5:-5));return;
        }
        if(e.type==SDL_KEYDOWN || e.type==SDL_TEXTINPUT || e.type==SDL_JOYBUTTONDOWN || e.type==SDL_JOYHATMOTION || e.type==SDL_MOUSEBUTTONDOWN || e.type==SDL_FINGERDOWN || e.type==SDL_FINGERMOTION)last_activity=s.GetTicks();
        if(e.type==SDL_QUIT){quit=true;return;}
        if(e.type==SDL_RENDER_DEVICE_RESET)throw std::runtime_error("Graphics device reset; restarting RGDS before using stale textures");
        if(e.type==SDL_RENDER_TARGETS_RESET){rounded.clear();media.clear();shader_preview.clear();top_seen.clear();dirty=true;return;}
        if(e.type==SDL_KEYUP){hold.release(e.key.keysym.sym);return;}
        if(e.type==SDL_JOYBUTTONUP){hold.release(1000+e.jbutton.button);return;}
        if(e.type==SDL_WINDOWEVENT){if(e.window.event==SDL_WINDOWEVENT_LEAVE){scroll_hover=browser_hover=false;}if(e.window.event==SDL_WINDOWEVENT_FOCUS_LOST){hold.clear();bool pointer_window=o.preview || embedded_menu || !e.window.windowID || (surfaces.size()>1 && e.window.windowID==surfaces[1].id);if(pointer_source!=PointerSource::Raw && pointer_window){cancel_navigation();brightness_drag=volume_drag=false;carousel.select(navigation_selected(),s.GetTicks());}}dirty=true;return;}
        if(settings.page==Page::Keyboard && e.type==SDL_TEXTINPUT) {
            std::string value=e.text.text;
            insert_text(value);
            dirty=true;return;
        }
        if(e.type==SDL_KEYDOWN && settings.page==Page::Keyboard) {
            switch(e.key.keysym.sym) {
                case SDLK_ESCAPE:finish_edit(false);break;case SDLK_RETURN:finish_edit(true);break;case SDLK_BACKSPACE:erase_character();break;case SDLK_DELETE:delete_character();break;case SDLK_HOME:edit_cursor=0;dirty=true;break;case SDLK_END:edit_cursor=edit_buffer.size();dirty=true;break;case SDLK_TAB:{auto at=keyboard_submit(keyboard_rows());key_row=at.first;key_col=at.second;dirty=true;break;}
                case SDLK_LEFT:action(0);break;case SDLK_RIGHT:action(1);break;case SDLK_UP:action(8);break;case SDLK_DOWN:action(9);break;default:break;
            }return;
        }
        if(e.type==SDL_KEYDOWN && !e.key.repeat) {
            if(!picker_open && horizontal_hold_page() && (e.key.keysym.sym==SDLK_LEFT || e.key.keysym.sym==SDLK_RIGHT))hold.press(e.key.keysym.sym,e.key.keysym.sym==SDLK_LEFT?-1:1,s.GetTicks());
            if(!picker_open && settings.page==Page::Library && !home_menu && (e.key.keysym.sym==SDLK_LEFTBRACKET || e.key.keysym.sym==SDLK_RIGHTBRACKET))hold.press(e.key.keysym.sym,e.key.keysym.sym==SDLK_LEFTBRACKET?-1:1,s.GetTicks());
            // Up/down repeat in every list and picker; +-2 tells the poll it is vertical.
            if(e.key.keysym.sym==SDLK_UP || e.key.keysym.sym==SDLK_DOWN)hold.press(e.key.keysym.sym,e.key.keysym.sym==SDLK_UP?-2:2,s.GetTicks());
            last_input="Key "+std::to_string(e.key.keysym.sym);dirty=true;
            switch(e.key.keysym.sym) {
                case SDLK_LEFT:action(0);break;case SDLK_RIGHT:action(1);break;case SDLK_UP:action(8);break;case SDLK_DOWN:action(9);break;
                case SDLK_RETURN:case SDLK_a:action(2);break;case SDLK_ESCAPE:case SDLK_b:action(3);break;
                case SDLK_TAB:case SDLK_c:if(!o.game_menu && !picker_open){settings.section=2;settings.page=Page::LibraryOptions;settings.focus=0;}break;case SDLK_F1:if(!o.game_menu)action(5);break;case SDLK_AC_HOME:case SDLK_HOME:case SDLK_AC_BACK:case SDLK_MENU:action(7);break;
                case SDLK_SLASH:if(settings.page==Page::Library && !home_menu && !picker_open)quick_search();break;case SDLK_r:action(6);break;case SDLK_s:action(7);break;case SDLK_x:action(11);break;case SDLK_y:if(settings.page==Page::Library || settings.page==Page::ShaderPicker)action(12);break;case SDLK_LEFTBRACKET:if(settings.page==Page::ShaderPicker || (settings.page==Page::Library && !home_menu))action(13);break;case SDLK_RIGHTBRACKET:if(settings.page==Page::ShaderPicker || (settings.page==Page::Library && !home_menu))action(14);break;
                case SDLK_q:if((settings.page==Page::Library && !home_menu) || (settings.page==Page::Keyboard && edit_field==4))action(15);break;case SDLK_e:if((settings.page==Page::Library && !home_menu) || (settings.page==Page::Keyboard && edit_field==4))action(16);break;default:break;
            }
        }else if(e.type==SDL_JOYBUTTONDOWN) {
            if(((settings.page==Page::Library && !home_menu) || (settings.page==Page::Keyboard && edit_field==4)) && (joystick_source(e.jbutton.button)==12 || joystick_source(e.jbutton.button)==13)){action(joystick_source(e.jbutton.button)==12?15:16);return;}

            if(settings.page==Page::ShaderPicker && (e.jbutton.button==4 || e.jbutton.button==5)){action(e.jbutton.button==4?13:14);return;}
            if(!picker_open && horizontal_hold_page() && (e.jbutton.button==4 || e.jbutton.button==5))hold.press(1000+e.jbutton.button,e.jbutton.button==4?-1:1,s.GetTicks());
            const char* a=std::getenv("RGDS_BUTTON_A");const char* b=std::getenv("RGDS_BUTTON_B");const char* select=std::getenv("RGDS_BUTTON_SETTINGS");
            if(e.jbutton.button==(a?std::atoi(a):0))action(2);else if(e.jbutton.button==(b?std::atoi(b):1))action(3);
            else if(e.jbutton.button==4)action(0);else if(e.jbutton.button==5)action(1);else if(e.jbutton.button==(select?std::atoi(select):6))action(7);
            else if(e.jbutton.button==7)action(4);else if(e.jbutton.button==8)action(7);
            else if(e.jbutton.button==3)action(11);
            else if(e.jbutton.button==2 && (settings.page==Page::Library || settings.page==Page::ShaderPicker || settings.page==Page::Keyboard))action(12);
        }else if(e.type==SDL_JOYHATMOTION) {
            hold.release(2000);hold.release(2001);if(horizontal_hold_page() && (e.jhat.value&(SDL_HAT_LEFT|SDL_HAT_RIGHT)))hold.press(2000,e.jhat.value&SDL_HAT_LEFT?-1:1,s.GetTicks());
            else if(!(e.jhat.value&(SDL_HAT_LEFT|SDL_HAT_RIGHT)) && (e.jhat.value&(SDL_HAT_UP|SDL_HAT_DOWN)))hold.press(2001,e.jhat.value&SDL_HAT_UP?-2:2,s.GetTicks());
            if(e.jhat.value&SDL_HAT_LEFT)action(0);else if(e.jhat.value&SDL_HAT_RIGHT)action(1);else if(e.jhat.value&SDL_HAT_UP)action(8);else if(e.jhat.value&SDL_HAT_DOWN)action(9);
        }else if(e.type==SDL_JOYAXISMOTION) {
            if(e.jaxis.axis==0){if(std::abs(int(e.jaxis.value))<10000)hold.release(3000);else if(horizontal_hold_page())hold.press(3000,e.jaxis.value<0?-1:1,s.GetTicks());}
            if(e.jaxis.axis==1){if(std::abs(int(e.jaxis.value))<10000)hold.release(3001);else hold.press(3001,e.jaxis.value<0?-2:2,s.GetTicks());}
            auto now=s.GetTicks();if(std::abs(static_cast<int>(e.jaxis.value))>16000 && now-last_axis>180){
                if(e.jaxis.axis==0)action(e.jaxis.value<0?0:1);else if(e.jaxis.axis==1)action(e.jaxis.value<0?8:9);last_axis=now;
            }
        }else if(e.type==SDL_MOUSEMOTION && !pointer_down){
            int x=e.motion.x,y=e.motion.y;bool in_bottom=mouse_position(e.motion.windowID,x,y);
            bool hover=in_bottom && (picker_open?(picker_scrollable() && picker_max_scroll()>0 && x>=572 && x<604 && y>=picker_top()+64 && y<picker_top()+64+picker_height()):settings.page!=Page::Library && list_max_scroll()>0 && x>=596 && y>=list_top && y<list_top+list_height);
            bool browser=in_bottom && !picker_open && settings.page==Page::Library && !home_menu && preferences.layout==2 && browser_list.maximum(navigation_count())>0 && x>=596 && x<628 && y>=LibraryViewport::top && y<LibraryViewport::top+LibraryViewport::height;
            if(hover!=scroll_hover || browser!=browser_hover){scroll_hover=hover;browser_hover=browser;dirty=true;}
        }else if(e.type==SDL_MOUSEBUTTONDOWN && e.button.which!=SDL_TOUCH_MOUSEID && e.button.button==SDL_BUTTON_LEFT) {
            auto now=s.GetTicks();if(touchscreen.available() || (raw_touch_seen && now-raw_touch_at<250) || (sdl_touch_seen && now-sdl_touch_at<250))return;
            int x=e.button.x,y=e.button.y;if((pointer_source==PointerSource::None || pointer_source==PointerSource::Mouse) && mouse_position(e.button.windowID,x,y)){if(settings.page==Page::BootWarning && e.common.timestamp && int32_t(e.common.timestamp-boot_displayed_at)<0)boot_touch_wait_release=true;pointer_source=PointerSource::Mouse;pointer_begin(x,y);}
        }else if(e.type==SDL_MOUSEMOTION && e.motion.which!=SDL_TOUCH_MOUSEID && pointer_down && pointer_source==PointerSource::Mouse && (e.motion.state&SDL_BUTTON_LMASK)) {
            int x=e.motion.x,y=e.motion.y;if(mouse_position(e.motion.windowID,x,y))pointer_move(x,y);
        }else if(e.type==SDL_MOUSEBUTTONUP && e.button.which!=SDL_TOUCH_MOUSEID) {
            if(pointer_source!=PointerSource::Mouse)return;
            int x=e.button.x,y=e.button.y;
            if(e.button.windowID==0){x=contact_x;y=contact_y;}else if(!mouse_position(e.button.windowID,x,y))return;
            pointer_end(x,y);
        }else if(e.type==SDL_MOUSEWHEEL && !probe){if(picker_open){if(picker_scrollable()){scroll_picker(picker_scroll-e.wheel.y*40);}else if(e.wheel.y)picker_action(e.wheel.y>0?8:9);return;}if(settings.page==Page::Library)navigate(std::clamp(e.wheel.x?e.wheel.x:-e.wheel.y,-3,3));else if(list_count()>0){sync_list();list_scroll=std::clamp(list_scroll-e.wheel.y*40,0,list_max_scroll());dirty=true;}}
        else if(e.type==SDL_FINGERDOWN || e.type==SDL_FINGERMOTION || e.type==SDL_FINGERUP) {
            // Compositor coordinates may span both displays. The calibrated evdev
            // source owns physical touch for its entire lifetime, including idle
            // gaps; SDL must not win a race before the next raw poll.
            auto now=s.GetTicks();if(touchscreen.available() || pointer_source==PointerSource::Raw || (pointer_source==PointerSource::None && raw_touch_seen && now-raw_touch_at<80))return;
            int x=std::clamp(static_cast<int>(e.tfinger.x*640),0,639),y=static_cast<int>(e.tfinger.y*(o.preview?960:480));
            if(o.preview){if(y<480 && !pointer_down)return;y-=480;}
            // KMS associates touch with the focused window, which can be the top
            // panel. Physical touch always belongs to the lower DS screen.
            else if(!embedded_menu && surfaces.size()<2)return;
            y=std::clamp(y,0,479);
            if(e.type==SDL_FINGERDOWN){if(pointer_down)return;if(settings.page==Page::BootWarning && e.common.timestamp && int32_t(e.common.timestamp-boot_displayed_at)<0)boot_touch_wait_release=true;active_finger=e.tfinger.fingerId;pointer_source=PointerSource::SDL;sdl_touch_seen=true;sdl_touch_at=now;pointer_begin(x,y);}
            else if(pointer_source==PointerSource::SDL && e.tfinger.fingerId==active_finger){sdl_touch_at=now;if(e.type==SDL_FINGERUP)pointer_end(x,y);else pointer_move(x,y);}
        }
        if(dirty && probe)std::cout<<last_input<<std::endl;
    }

public:
    App(Options options):o(std::move(options)),theme(default_theme()),
        rounded(s,o.app.empty()?fs::path("assets/fonts"):o.app/"assets/fonts"),media(s),shader_preview(s),system(o.preview,o.app,o.data),probe(o.probe) {
        launch_profile.mark("sdl-library-loaded");
        if(o.themes.empty())o.themes=o.app.empty()?fs::path("themes/default"):o.app/"themes/default";
        if(!o.boot_startup)shaders=load_shaders(o.app.empty()?fs::path("assets/shaders/catalog.tsv"):o.app/"assets/shaders/catalog.tsv");
        if(o.preferences.empty())o.preferences=o.data.empty()?fs::path("build/dev/preferences.ini"):o.data/"preferences.ini";
        if(o.data.empty())o.data=o.preferences.parent_path();
        library_cache.reset(o.data);
        if(o.system_preferences.empty())o.system_preferences=o.preferences.parent_path()/"system.ini";
        if(!o.boot_startup && (!o.game_menu || o.preview)){refresh_system();system_preferences.brightness=hardware.brightness_percent;brightness_applied=hardware.brightness_percent;}
        if(!o.boot_startup && !o.preview && !probe && !o.game_menu && hardware.volume && !system.set("volume",hardware.volume_percent))status="Could not apply volume";
        if(!o.boot_warning)load_system_configuration();
        preferences.dark=dark_theme_file(o.theme);
        try{preferences=load_preferences(o.preferences);}   // reads ui.ini even without the base file
        catch(const std::exception& e){std::cerr<<"Preferences fallback: "<<e.what()<<std::endl;preferences=Preferences{};try{load_ui_preferences(o.preferences.parent_path()/"ui.ini",preferences);}catch(const std::exception&){}}
        locale.load(asset_path("locales"),preferences.language);
        if(!o.mode.empty())preferences.dark=o.mode=="dark";
        if(!o.font.empty())preferences.pixel=o.font=="pixel";
        if(!o.boot_startup || !preferences.pixel)check_fonts(o.boot_startup || o.game_menu);
        try{reload_themes();}catch(const std::exception& e){std::cerr<<"Theme fallback: "<<e.what()<<std::endl;theme=preferences.dark?default_dark_theme():default_theme();status="DEFAULT THEME - CHECK LOG";}
        if(o.boot_startup){settings.page=o.boot_warning?Page::BootWarning:Page::Library;home_menu=true;}
        else{session_initialized=true;load_library(o.game_menu?std::vector<Game>{}:scan(o.roots));}
        if(o.card_repair){card_repair=1;card_repair_at=s.GetTicks();}
        launch_profile.mark("configuration-ready");
    }
    ~App(){stop_microphone();stop_launch();if(o.boot_ready_fd>=0)::close(o.boot_ready_fd);stop_library_scan();stop_worker();std::fill(password.begin(),password.end(),'\0');close();}
    void finish_managed_menu(int result) {
#ifdef __linux__
        const char* driver=s.GetCurrentVideoDriver();
        if(!o.game_menu || o.preview || embedded_menu || !std::getenv("RGDS_SYSTEM_MANAGED") || !driver || std::string(driver)!="wayland")return;
        // The vendor EGL driver waits a full second per window for buffers
        // during destruction. This is a terminating, session-owned helper:
        // finish durable work and reap children, then let process exit close
        // its Wayland connection and release its GPU resources.
        // The parent retains game-input ownership until it reaps this process.
        stop_launch();stop_library_scan();stop_worker();flush_return_controls();
        flush_brightness();flush_volume();system.finish();overlay.close();
        std::cout.flush();std::cerr.flush();_exit(result);
#else
        (void)result;
#endif
    }
    int run() {
        open();configure_sounds();view_motion.seed=ViewMotion::hash(uint32_t(getpid())^uint32_t(std::time(nullptr)));for(auto& panel:surfaces){for(const char* name:{"brand/mark-48.bmp","brand/mark-120.bmp"})media.warm(panel.renderer,asset_path(name));media.warm(panel.renderer,asset_path(preferences.dark?"brand/status-dark.bmp":"brand/status-light.bmp"));media.warm(panel.renderer,asset_path(preferences.dark?"brand/wordmark-dark.bmp":"brand/wordmark-light.bmp"));}const auto began=s.GetTicks();boot_scene_at=began;last_activity=began;int frames=0;
        std::set<int> visited_pages;
        if(!o.boot_startup && !o.game_menu && !o.roots.empty())start_worker("index");
        if(!o.input_script.empty()) {
            std::ifstream file(o.input_script);if(!file)throw std::runtime_error("Cannot read input script");
            std::string line;
            while(std::getline(file,line)) {
                if(line.empty() || line[0]=='#')continue;
                std::istringstream record(line);Input input{};std::string type,key;
                if(!(record>>input.time>>type))throw std::runtime_error("Invalid preview input script");
                if(type=="key" || type=="key-up") {
                    if(!(record>>key))throw std::runtime_error("Input key missing");
                    input.event.type=type=="key"?SDL_KEYDOWN:SDL_KEYUP;input.event.key.keysym.sym=s.GetKeyFromName(key.c_str());
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
                }else if(type=="finger-down" || type=="finger-move" || type=="finger-up"){
                    input.event.type=type=="finger-down"?SDL_FINGERDOWN:type=="finger-move"?SDL_FINGERMOTION:SDL_FINGERUP;
                    input.event.tfinger.touchId=1;input.event.tfinger.fingerId=1;input.event.tfinger.windowID=surfaces[0].id;
                    if(!(record>>input.event.tfinger.x>>input.event.tfinger.y))throw std::runtime_error("Finger coordinates missing");record>>input.event.tfinger.fingerId;
                }else if(type=="joy-down" || type=="joy-up"){
                    input.event.type=type=="joy-down"?SDL_JOYBUTTONDOWN:SDL_JOYBUTTONUP;int button=0;if(!(record>>button) || button<0 || button>255)throw std::runtime_error("Joystick button missing");input.event.jbutton.button=Uint8(button);input.event.jbutton.which=0;input.event.jbutton.state=type=="joy-down"?SDL_PRESSED:SDL_RELEASED;
                }else if(type=="joy-hat"){
                    int value=0;if(!(record>>value) || value<0 || value>15)throw std::runtime_error("Joystick direction missing");input.event.type=SDL_JOYHATMOTION;input.event.jhat.which=0;input.event.jhat.hat=0;input.event.jhat.value=Uint8(value);
                }else if(type=="wheel"){
                    input.event.type=SDL_MOUSEWHEEL;if(!(record>>input.event.wheel.x>>input.event.wheel.y))throw std::runtime_error("Wheel delta missing");
                }else if(type=="focus-lost"){
                    input.event.type=SDL_WINDOWEVENT;input.event.window.event=SDL_WINDOWEVENT_FOCUS_LOST;input.event.window.windowID=surfaces[0].id;
                }else throw std::runtime_error("Unknown preview input type");
                inputs.push_back(input);
                if(type=="click"){input.time++;input.event.type=SDL_MOUSEBUTTONUP;inputs.push_back(input);}
            }
            std::stable_sort(inputs.begin(),inputs.end(),[](const Input& a,const Input& b){return a.time<b.time;});
            // A key the script never releases is a tap, as a real keyboard would send; a key-up later on holds it (and repeats) until then.
            std::vector<Input> taps;
            for(size_t i=0;i<inputs.size();i++)if(inputs[i].event.type==SDL_KEYDOWN){
                bool released=false;
                for(size_t j=i+1;j<inputs.size() && !released;j++)released=inputs[j].event.type==SDL_KEYUP && inputs[j].event.key.keysym.sym==inputs[i].event.key.keysym.sym;
                if(!released){Input up=inputs[i];up.time++;up.event.type=SDL_KEYUP;taps.push_back(up);}
            }
            inputs.insert(inputs.end(),taps.begin(),taps.end());
            std::stable_sort(inputs.begin(),inputs.end(),[](const Input& a,const Input& b){return a.time<b.time;});
        }
        while(!quit && !stopping) {
            update_health();
            if(o.game_menu && !o.preview && !sleep_audio_parked && fs::exists(o.data/"session/game-menu.audio-sleep")){
                sounds.close();sleep_audio_parked=true;
                std::ofstream(o.data/"session/game-menu.audio-sleep.ready").put('1');
            }
            if(sleep_audio_parked && !fs::exists(o.data/"session/game-menu.audio-sleep")){
                sleep_audio_parked=false;restore_sleep_audio();
            }
            if(o.game_menu && !o.preview && fs::exists(o.data/"session/game-menu.wake")){
                fs::remove(o.data/"session/game-menu.wake");reset_after_sleep();sleep_audio_parked=false;restore_sleep_audio();
            }
            // The emulator owns the displays during play; keep only its lifecycle alive.
            if(launch_phase==LaunchPhase::Playing){ui_trace.mark(UiTrace::Playing,rendered_frames,uint32_t(settings.page));poll_launch(0);if(launch_phase==LaunchPhase::Playing)wait_child_exit(launch_child,1000);continue;}
            while(next_input<inputs.size() && s.GetTicks()-began>=inputs[next_input].time)s.PushEvent(&inputs[next_input++].event);
            // Apply releases/back actions before held-button repeat or drawing.
            // A slow screenshot/frame must not repeat a key already released.
            ui_trace.mark(UiTrace::Events,rendered_frames,uint32_t(settings.page));
            SDL_Event queued{};while(s.PollEvent(&queued))event(queued);
            ui_trace.mark(UiTrace::Loop,rendered_frames,uint32_t(settings.page));
            if(!o.cache_report.empty())visited_pages.insert(int(settings.page));
            if(quit || stopping)break;
            auto now=s.GetTicks();const bool muted=hardware.volume && hardware.volume_percent==0;sounds.mute_music(muted);if(muted && sounds.active())sounds.stop();sounds.poll(now);
            if(!departure_action.empty() && int32_t(now-departure_at)>=0){auto request=std::move(departure_action);departure_action.clear();departure_dispatch=true;menu_request(request);departure_dispatch=false;if(quit)break;}
            if(settings.page==Page::Controls && control_capture){
                if(now-capture_started>=10000){control_capture=false;capture_logical=-1;controls_notice="No button detected. Try again when you're ready.";dirty=true;}
                else if(now-capture_clock>=1000){capture_clock=now;dirty=true;}
            }
            if(!o.probe && (session_initialized || o.card_repair))check_card(now);
            if(card_repair){
                if(o.seconds && now-began>=Uint32(o.seconds)*1000)break;
                if(card_repair==1 && preferences.animations && now-feedback_frame_at>=50){feedback_frame_at=now;dirty=true;}
                if(dirty || o.frames){draw(!o.screenshot.empty());frames++;}
                if(o.frames && frames>=o.frames)break;
                SDL_Event wait{};if(wait_event(&wait,50)){event(wait);while(s.PollEvent(&wait))event(wait);}
                continue;
            }
            int volume_delta=volume_keys.poll();if(session_initialized && !launching && volume_delta)change_volume(hardware.volume_percent+volume_delta);
            if(volume_toast && now>=volume_toast){volume_toast=0;dirty=true;}
            for(auto point:touchscreen.poll(now-began))raw_touch(point,now);
            if(!session_initialized){
                if(o.seconds && now-began>=Uint32(o.seconds)*1000)break;
                const bool motion=boot_animating(now);
                if(motion && now-feedback_frame_at>=33){feedback_frame_at=now;dirty=true;}
                if(dirty || o.frames){draw(!o.screenshot.empty());frames++;}
                if(o.frames && frames>=o.frames)break;
                if(o.boot_startup && !o.boot_warning){start_boot_session();continue;}
                SDL_Event wait{};
                if(wait_event(&wait,motion?16:input_wake.available()?250:touchscreen.available()?8:50)){event(wait);while(s.PollEvent(&wait))event(wait);}
                continue;
            }
            poll_library_scan();poll_collection();poll_search_preview(now);if(auto result=library_summary.poll()){summary=std::move(*result);summary_ready=true;dirty=true;}
            if(!o.game_menu && library_ready && !setup_checked){setup_checked=true;if(!o.preview && !fs::exists(o.data/"setup-complete") && fs::exists(o.data/"setup-required")){settings.page=Page::RomSetup;settings.focus=0;setup_first_run=true;dirty=true;}}
            if(!o.game_menu && o.boot_startup && library_ready && boot_hardware_ready && !return_writes_pending() && !system.volume_pending() && !system.brightness_pending() && !resume_checked && settings.page!=Page::BootWarning){resume_checked=true;auto ticket=o.data/"session/resume-launch.ini";auto saved=fields(ticket);std::error_code ec;fs::remove(ticket,ec);if(!saved["rom"].empty()){search_query.clear();discovery_filter=0;library_view(0);for(size_t i=0;i<model.games.size();i++)if(model.games[i].path.string()==saved["rom"]){select_game(i,false);home_menu=false;for(int core=0;core<core_count;core++)if(saved["core"]==core_ids[core])resume_core=core;resume_launch=resume_core>=0;launch();break;}}}
            poll_return_controls();
            if(!launching){int direction=hold.poll(now);
                if(direction==-2 || direction==2){action(direction<0?8:9);last_activity=now;}
                else if(direction && !picker_open && horizontal_hold_page()){if(settings.page==Page::Library){navigate(direction);}else action(direction<0?0:1);last_activity=now;}}
            poll_brightness();
            poll_volume();
            if(auto refreshed=system.poll_status())if(refreshed->success){
                const auto previous=hardware;
                auto brightness=hardware.brightness_percent,volume=hardware.volume_percent;hardware=refreshed->value;
                if(brightness_unsaved || brightness_drag || system.brightness_pending())hardware.brightness_percent=brightness;
                if(o.preview || return_writes_pending() || volume_unsaved || volume_drag || system.volume_pending() || volume_toast>now)hardware.volume_percent=volume;
                if(!volume_unsaved && !system.volume_pending()){volume_applied=hardware.volume_percent;if(volume_applied>0)unmuted_volume=volume_applied;}
                if(o.boot_startup && !boot_hardware_ready){boot_hardware_ready=true;if(hardware.volume){system.request_volume(hardware.volume_percent);volume_unsaved=true;}if(hardware.brightness){system.request_brightness(system_preferences.brightness);brightness_unsaved=true;brightness_changed=now;hardware.brightness_percent=system_preferences.brightness;}if(hardware.power && system_preferences.power>0)system.set("power",system_preferences.power);if(hardware.lid && system_preferences.lid)system.set("lid",1);}
                if(!same_hardware(previous,hardware))dirty=true;
            }
            if(!launching || launch_preview)poll_service(now);
            if(!o.preview && !o.game_menu && !launching && worker<=0 && now-automatic_update_at>60000){automatic_update_at=now;start_worker("device.update-auto");}
            poll_microphone(now);watch_menu(now);
            if(settings.page==Page::Scraper && !service_loaded && worker<=0){start_worker("status");service_loaded=true;}
            if(!o.game_menu && !launching && worker<=0 && (!device_loaded || now-connection_poll>=15000))refresh_network(now);
            poll_launch(now);
            poll_chat();
            if(media.adopt())dirty=true;
            if(launch_phase==LaunchPhase::Playing)continue;
            if(touch_tail && now-release_at>=140){touch_tail=false;dirty=true;}
            bool launch_transition=launching && preferences.animations && (now-launch_at<110 || launch_phase==LaunchPhase::Exiting);
            if(launching && preferences.animations){int head=int((now-launch_at)/75)%8;if(head!=launch_drawn_head){launch_drawn_head=head;dirty=true;}}
            sync_view_motion(now);
            bool view_active=view_motion.active();
            if(view_motion.update(now) && !view_motion.active())dirty=true;
            bool rail_animating=update_rail_feedback(now);
            bool feedback_active=view_active || rail_animating || (preferences.animations && shutting_down()) || (o.game_menu && menu_status["state"]=="busy" && preferences.animations) || (preferences.animations && network_busy() && (settings.page==Page::Network || settings.page==Page::Wifi)) || boot_animating(now) || launch_transition || touch_animating(now) || (preferences.animations && (now<micro_until || (picker_open && now-picker_at<170) || picker_closing()));
            // Stop refreshing a stationary held contact after its cue completes.
            if(feedback_active && now-feedback_frame_at>=16){feedback_frame_at=now;dirty=true;}
            bool ready=options_hold() && now-pointer_at>=600;if(ready!=options_ready){options_ready=ready;dirty=true;}
            bool list_animated=browser_list.update(now);
            const bool shader_moving=settings.page==Page::ShaderPicker && shader_carousel.update(now);
            bool animated=(preferences.animations && (carousel.update(now) || shader_moving || list_animated || now-selected_at<190 || (!view_active && now-page_at<150))) || (settings.page==Page::ShaderPicker && shader_thumbs_pending);
            if(animated)dirty=true;
            if(hardware.charging && !drawn_status_stamp.empty() && int((now/420)%4)!=drawn_charge_phase)dirty=true;
            if(artwork_busy() && !drawn_status_stamp.empty() && loader_phase(now)!=drawn_loader_phase)dirty=true;
            watch_theme(now);
            if(!launching)watch_system(now);
            if(!launching && !animated && !carousel.dragging)watch_library_assets(now);
            if(o.seconds && s.GetTicks()-began>=static_cast<Uint32>(o.seconds)*1000)break;
            if(now-clock_poll>=250){clock_poll=now;if(!drawn_status_stamp.empty() && status_stamp()!=drawn_status_stamp)dirty=true;
                if(home_menu && preferences.home_layout==1 && settings.page==Page::Library && !chat_open && !launching && std::time(nullptr)!=drawn_second){drawn_second=std::time(nullptr);dirty=true;}}
            if(dirty || o.frames){ui_trace.mark(UiTrace::Assets,rendered_frames,uint32_t(settings.page));if(!rendered_frames)launch_profile.mark("assets-start");prepare_render_assets();if(!rendered_frames)launch_profile.mark("assets-ready");draw(!o.screenshot.empty());frames++;if(frames==1 && o.game_menu && !o.preview){system.request_status();system_poll=now;}}
            if(!o.preview && fs::exists(o.data/"session/capture-request")) {
                auto previous=o.screenshot;
                o.screenshot=o.data/"session/ui-snapshot";prepare_render_assets();draw(true);o.screenshot=previous;
                // Acknowledge only after both captures have been saved.
                fs::remove(o.data/"session/capture-request");
            }
            if(o.frames && frames>=o.frames)break;
            prepare_next_cover();
            SDL_Event e{};
            int wait_ms=animated || dirty?1:feedback_active?16:launching && preferences.animations?int(std::min(50u,75u-(now-launch_at)%75)):idle_wait_ms(now);
            if(o.game_menu && (!menu_pending_token.empty() || menu_status["state"]=="busy"))wait_ms=std::min(wait_ms,20);
            if(chat_open)wait_ms=std::min(wait_ms,40);
            if(wait_event(&e,wait_ms)){event(e);while(s.PollEvent(&e))event(e);}
        }
        flush_brightness();flush_volume();save_state();
        if(!o.cache_report.empty()){
            std::ofstream report(o.cache_report);if(!report)throw std::runtime_error("Cannot write cache report");
            report<<"{\"picker_detail\":"<<picker_detail<<",\"detail_lines\":"<<detail_lines.size()<<",\"detail_scroll\":"<<detail_scroll<<",\"picker_open\":"<<picker_open<<",\"picker_focus\":"<<picker_focus<<",\"picker_scroll\":"<<picker_scroll<<",\"list_scroll\":"<<list_scroll<<",\"status_stamp\":"<<std::quoted(drawn_status_stamp)<<',';
            report<<"\"menu_music\":"<<preferences.menu_music<<",\"music_volume\":"<<preferences.music_volume<<",\"music_reads\":"<<sounds.music_reads<<",\"music_playing\":"<<sounds.music_playing()<<",\"music_samples\":"<<sounds.music_samples()<<",\"menu_sounds\":"<<preferences.menu_sounds<<",\"sound_volume\":"<<preferences.sound_volume<<",\"sound_plays\":"<<sounds.plays<<",\"sound_opens\":"<<sounds.opens<<",\"sound_releases\":"<<sounds.releases<<",\"sound_open_now\":"<<sounds.open_now()<<",\"sound_reads\":"<<sounds.reads<<",\"sound_bytes\":"<<sounds.bytes()<<",\"sound_active\":"<<sounds.active()<<",\"sound_counts\":[";for(size_t i=0;i<sounds.counts.size();i++){if(i)report<<',';report<<sounds.counts[i];}report<<"],";
            report<<"\"element_moves\":"<<element_moves<<",\"back_transitions\":"<<back_transitions<<",\"view_energy\":"<<view_motion.energy<<",\"view_variant\":"<<view_motion.variant<<',';
            report<<"\"view_transition_style\":"<<std::quoted(view_style_name(preferences.view_style))<<",\"push_animation_frames\":"<<push_animation_frames<<',';
            report<<"\"view_animations\":"<<preferences.view_animations<<",\"view_transitions\":"<<view_transitions<<",\"view_animation_frames\":"<<view_animation_frames<<",\"view_animation_active\":"<<view_motion.active()<<",\"view_canvas_failed\":"<<view_canvas_failed<<',';
            report<<"\"font_atlases\":"<<rounded.loaded_atlases()<<",\"font_bytes\":"<<rounded.loaded_bytes()<<",\"shader_catalog_loaded\":"<<!shaders.empty()<<',';
            report<<"\"shader_preview_scene\":"<<shader_preview_scene<<",\"shader_preview_mode\":"<<int(shader_preview_mode)<<",\"shader_preview_split\":"<<shader_preview_split<<",\"shader_preview_entries\":"<<shader_preview.count()<<",\"shader_preview_bytes\":"<<shader_preview.bytes()<<",\"shader_preview_reads\":"<<shader_preview.counters.file_reads<<",\"shader_preview_stamp_checks\":"<<shader_preview.counters.stamp_checks<<',';
            report<<"\"controls_game\":"<<controls_per_game<<",\"controls_inherited\":"<<controls_inherit<<",\"controls_capturing\":"<<control_capture<<",\"controls_preset\":"<<std::quoted(edited_controls.preset)<<',';
            report<<"\"system_configuration_loaded\":"<<system_configuration_loaded<<",\"timezone\":"<<std::quoted(system_preferences.timezone)<<',';
            report<<"\"drastic_backend\":"<<preferences.drastic_backend<<",\"game_drastic_backend\":"<<game_preferences.drastic_backend<<",";
            report<<"\"default_core\":"<<preferences.core<<",\"game_core_override\":"<<game_preferences.core<<",\"game_threaded\":"<<game_preferences.threaded<<",\"game_hires\":"<<game_preferences.hires<<",\"game_frameskip\":"<<game_preferences.frameskip<<",\"game_scale\":"<<game_preferences.rgds_scale<<",\"game_filter2d\":"<<game_preferences.rgds_2d<<",\"menu_fast_forward\":"<<(menu_status["fast_forward"]=="1")<<",\"settings_notice\":"<<std::quoted(status)<<",\"picker_title\":"<<std::quoted(picker_title)<<',';
            report<<"\"visited_pages\":[";bool first_page=true;for(int page:visited_pages){if(!first_page)report<<',';report<<page;first_page=false;}report<<"],";
            report<<"\"discovery_active\":"<<discovery_active()<<",\"discovery_description\":"<<std::quoted(discovery_description())<<",\"search_preview_games\":"<<search_matches.size()<<",\"search_preview_loading\":"<<search_preview_loading<<",\"keyboard_row\":"<<key_row<<",\"keyboard_col\":"<<key_col<<",\"keyboard_layer\":"<<key_layer<<",\"keyboard_caps\":"<<key_caps<<",\"edit_cursor\":"<<(edit_field==4?edit_cursor:0)<<',';
            report<<"\"home_menu\":"<<home_menu<<",\"filtered_games\":"<<model.games.size()<<",\"search_query\":"<<std::quoted(search_query)<<",\"discovery_filter\":"<<discovery_filter<<',';
            report<<"\"page\":"<<int(settings.page)<<",\"boot_pending\":"<<(settings.page==Page::BootWarning)<<",\"library_scan_started\":"<<library_scan_started<<",\"library_ready\":"<<library_ready<<",\"frames\":"<<rendered_frames<<",\"metadata_reads\":"<<library_cache.counters.file_reads<<",\"metadata_lookups\":"<<library_cache.counters.lookups<<",\"metadata_hits\":"<<library_cache.counters.hits<<",\"metadata_stamp_checks\":"<<library_cache.counters.stamp_checks<<",\"image_reads\":"<<media.image_reads<<",\"async_loads\":"<<media.async_loads<<",\"top_skips\":"<<top_skips<<",\"banner_reads\":"<<media.banner_reads<<",\"media_lookups\":"<<media.counters.lookups<<",\"media_hits\":"<<media.counters.hits<<",\"media_stamp_checks\":"<<media.counters.stamp_checks<<",\"render_file_reads\":"<<render_file_reads<<",\"render_stamp_checks\":"<<render_stamp_checks<<",\"metadata_entries\":"<<library_cache.size()<<",\"image_entries\":"<<media.image_count()<<",\"banner_entries\":"<<media.banner_count()<<",\"texture_bytes\":"<<media.bytes()<<",\"evictions\":"<<library_cache.counters.evictions+media.counters.evictions<<"}\n";
        }
        // The managed session dispatches a terminal request only with this
        // acknowledgement; ordinary exit or failed cleanup cannot trigger it.
        return system_exit_ack?76:0;
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
    m.move(1);ok=ok && m.selected==0;
    int offered=0;for(int i=0;i<core_count;i++)offered+=core_offered(i);
    for(int i=0;i<offered;i++){m.next_core();ok=ok && core_offered(int(m.core));}ok=ok && m.core==0;
    Model empty;empty.move(-99);ok=ok && empty.selected==0;
    {   // One IMA ADPCM block of rising nibbles must decode to a rising ramp of the declared length.
        std::vector<uint8_t> wav={'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',20,0,0,0,0x11,0,1,0,0,0x7d,0,0,0,0,0,0,8,0,4,0,2,0,9,0,'d','a','t','a',8,0,0,0,0,0,0,0,0x77,0x77,0x77,0x77};
        auto track=decode_ima_adpcm(wav);bool rising=track.samples.size()==9 && track.rate==32000;
        for(size_t i=1;rising && i<track.samples.size();i++)rising=track.samples[i]>track.samples[i-1];
        ok=ok && rising && decode_ima_adpcm({'R','I','F','F'}).empty();
    }
    fs::remove_all(root);
    std::cout<<(ok?"Library, deduplication, navigation and engine selection passed.":"Model test failed.")<<std::endl;
    return ok?0:1;
}
int main(int argc,char** argv) {
    launch_profile.start();
    try {
        if(argc>1 && std::string(argv[1])=="--scan-library"){
            std::vector<fs::path> roots;for(int i=2;i<argc;i++){if(std::string(argv[i])!="--rom-dir" || ++i>=argc)throw std::runtime_error("Invalid library scan arguments");roots.emplace_back(argv[i]);}
            for(const auto& game:scan(roots)){auto path=game.path.string();std::cout.write(path.data(),path.size());std::cout.put('\0');std::cout.write(game.title.data(),game.title.size());std::cout.put('\0');}return std::cout?0:1;
        }
        if(argc>1 && std::string(argv[1])=="--restore-controls"){
            if(argc!=8)throw std::runtime_error("Invalid return-control arguments");setenv("RGDS_SYSTEM_MANAGED","1",1);SystemControls controls(false,fs::path(argv[2]),fs::path(argv[3]));
            const std::array<const char*,4> names={"volume","brightness","power","lid"};const std::array<int,4> minimum={0,5,0,0},maximum={100,100,3,1};int failures=0;
            for(int i=0;i<4;i++){size_t end=0;std::string raw=argv[4+i];int value=std::stoi(raw,&end);if(end!=raw.size() || (value!=-1 && (value<minimum[i] || value>maximum[i])))throw std::runtime_error("Invalid return-control value");if(value>=0 && !controls.set(names[i],value)){failures|=1<<i;std::cerr<<"Could not restore "<<names[i]<<std::endl;}}return failures;
        }
        Options o;
        if(const char* top=std::getenv("RGDS_TOP_DISPLAY"))o.top=top;
        if(const char* bottom=std::getenv("RGDS_BOTTOM_DISPLAY"))o.bottom=bottom;
        if(const char* platform=std::getenv("RGDS_SESSION_PLATFORM"))o.independent=std::string(platform)=="independent";
        if(const char* profile=std::getenv("RGDS_INPUT_PROFILE"))o.mainline=o.independent && std::string(profile)=="mainline-rg-ds";
        o.executable=fs::absolute(argv[0]);
        for(int i=1;i<argc;i++) {
            std::string arg=argv[i];
            auto value=[&](){if(++i>=argc)throw std::runtime_error("Missing value for "+arg);return std::string(argv[i]);};
            if(arg=="--self-test")return self_test();
            if(arg=="--preview")o.preview=true;else if(arg=="--probe")o.probe=true;
            else if(arg=="--game-menu")o.game_menu=true;
            else if(arg=="--game-switcher"){o.game_switcher=true;o.game_menu=true;}
            else if(arg=="--game-core")o.game_core=value();else if(arg=="--game-rom")o.game_rom=value();
            else if(arg=="--allow-launch")o.allow_launch=true;
            else if(arg=="--watch-assets")o.watch_assets=true;
            else if(arg=="--settings")o.settings=true;
            else if(arg=="--home")o.home=true;
            else if(arg=="--library")o.library=true;
            else if(arg=="--boot-warning")o.boot_warning=true;
            else if(arg=="--boot-startup")o.boot_startup=true;
            else if(arg=="--card-repair")o.card_repair=true;
            else if(arg=="--boot-ready-fd"){auto raw=value();size_t end=0;o.boot_ready_fd=std::stoi(raw,&end);if(end!=raw.size() || o.boot_ready_fd<3)throw std::runtime_error("Invalid boot notification descriptor");}
            else if(arg=="--cache-report")o.cache_report=value();
            else if(arg=="--touch-recording")o.touch_recording=value();
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
                    <<"--home | --library (category menu or game-list preview)\n"
                    <<"--boot-warning (optional direct-boot touch-to-continue screen)\n"
                    <<"--boot-startup (direct startup with deferred scan and hardware work)\n"
                    <<"--cache-report PATH (development asset-cache counters)\n"
                    <<"--touch-recording FILE (preview Linux AArch64 evdev capture)\n"
                    <<"--settings [--section theme|brightness|power|library|sound|connectivity|system|controls]\n"
                    <<"--game-menu --game-core drastic|dsperate|melonds|rgds-experimental [--game-rom FILE]\n"
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
        if(!o.preview && !o.touch_recording.empty())throw std::runtime_error("Touch recordings are only available in desktop preview");
        if(o.boot_warning)o.boot_startup=true;
        if(o.boot_ready_fd>=0){
            int kind=0;socklen_t length=sizeof(kind);
            if(!o.boot_startup || getsockopt(o.boot_ready_fd,SOL_SOCKET,SO_TYPE,&kind,&length)!=0 || kind!=SOCK_STREAM)throw std::runtime_error("Boot notification needs a managed startup socket");
            if(fcntl(o.boot_ready_fd,F_SETFD,FD_CLOEXEC)<0 || fcntl(o.boot_ready_fd,F_SETFL,O_NONBLOCK)<0)throw std::runtime_error("Cannot configure boot notification socket");
        }
        if(o.boot_startup && (o.probe || o.game_menu || o.settings))throw std::runtime_error("Boot startup is a home startup option");
        if(o.seconds<0 || o.frames<0)throw std::runtime_error("Frames and seconds must be nonnegative");
        std::signal(SIGTERM,on_signal);std::signal(SIGINT,on_signal);
        launch_profile.mark("arguments-ready");App app(std::move(o));int result=app.run();app.finish_managed_menu(result);return result;
    }catch(const std::exception& e){std::cerr<<"RGDS: "<<e.what()<<std::endl;return 1;}
}
