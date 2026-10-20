#pragma once
#include "launcher/model.hpp"
#include "launcher/shaders.hpp"
#include "launcher/view_motion.hpp"
#include <array>
#include <charconv>

namespace rgds {
enum class Page { Library, Settings, Appearance, Placeholder, Power, Display, LibraryOptions, Scraper, Keyboard, System, Timezone, Sound, QuickMenu, LeaveGame, Network, Wifi, DeviceInfo, Maintenance, Confirm, GameOptions, ShaderPicker, EmulatorOptions, ApplyShader, BootWarning, Startup, Controls, GameArtwork, Statistics, SaveGallery, Discover, RomSetup, DSControls, Microphone, Motion, MenuSounds, Graphics, GameSwitcher, Colours };
struct Preferences { bool dark=false, pixel=false,auto_load=true,auto_save=true,animations=true,view_animations=false,touch_indicator=true,rich_icons=true,menu_sounds=true,high_contrast=false;int accent=-1;ViewStyle view_style=ViewStyle::Spring;int view_speed=1;std::string language="en-US";unsigned core=4;int filter=0,view=0,layout=0,artwork_provider=0,drastic_backend=0,sound_volume=55;int home_layout=1,background=1;bool gestures=false;bool menu_music=true;int music_volume=40; };
struct SystemPreferences {
    int power=0, brightness=60, idle=0;
    bool lid=true,battery_percent=true;
    std::string timezone="Europe/Oslo";
    int date_format=0;
    int time_format=0;   // 0: 24-hour, 1: 12-hour
};
inline const std::array<const char*,2> time_format_names={"24-hour","12-hour"};
inline const std::array<const char*,5> date_format_names={"DD/MM","MM/DD","DD.MM","YYYY-MM-DD","DD-MM-YYYY"};
inline const std::array<const char*,5> date_format_patterns={"%d/%m","%m/%d","%d.%m","%Y-%m-%d","%d-%m-%Y"};
inline const std::array<const char*,17> timezones={"UTC","Europe/Oslo","Europe/London","Europe/Berlin","Europe/Paris","America/New_York","America/Chicago","America/Denver","America/Los_Angeles","America/Sao_Paulo","Asia/Tokyo","Asia/Seoul","Asia/Shanghai","Asia/Kolkata","Asia/Dubai","Australia/Sydney","Pacific/Auckland"};
inline const std::array<const char*,4> power_names={"Stock","Battery saver","Balanced","Performance"};
inline const std::array<int,5> idle_minutes={0,1,2,5,10};
inline std::string idle_name(int index){return index==0?"Never":std::to_string(idle_minutes.at(index))+" min";}
struct Settings {
    Page page=Page::Library;
    int section=0, focus=0;
    // The row you left each page from. Back lands on it again instead of the
    // first row, so Library -> Artwork sources -> Back keeps Artwork sources lit.
    std::array<int,64> remembered{};
    void remember(Page from,int row){if(size_t(from)<remembered.size())remembered[size_t(from)]=std::max(0,row);}
    int recall(Page target)const{return size_t(target)<remembered.size()?remembered[size_t(target)]:0;}
    void open(){page=Page::Settings;focus=0;}
    void back(){if(page==Page::MenuSounds){page=Page::Sound;focus=2;}else if(page==Page::Motion){page=Page::Appearance;focus=6;}else if(page==Page::Colours){page=Page::Appearance;focus=1;}else if(page==Page::Statistics){page=Page::Settings;section=7;focus=0;}else if(page==Page::Timezone){page=Page::System;focus=1;}else if(page==Page::Maintenance){page=Page::System;focus=5;}else if(page==Page::DeviceInfo){page=Page::System;focus=4;}else if(page==Page::Display){page=Page::Appearance;focus=5;}else if(page==Page::Startup){page=Page::System;focus=7;}else{page=page==Page::Settings || page==Page::GameOptions?Page::Library:page==Page::Scraper?Page::LibraryOptions:page==Page::Timezone || page==Page::DeviceInfo || page==Page::Maintenance?Page::System:page==Page::Wifi?Page::Network:Page::Settings;focus=page==Page::Settings?0:recall(page);}}
    void move(int delta){if(page==Page::Settings)section=std::clamp(section+delta,0,8);else if(page==Page::Appearance)focus=std::clamp(focus+delta,0,10);else if(page==Page::LibraryOptions)focus=std::clamp(focus+delta,0,9);else if(page==Page::Power)focus=std::clamp(focus+delta,0,5);else if(page==Page::Scraper)focus=std::clamp(focus+delta,0,2);else if(page==Page::Startup)focus=std::clamp(focus+delta,0,3);else if(page==Page::Timezone)focus=std::clamp(focus+delta,0,int(timezones.size())-1);else if(page==Page::DeviceInfo)focus=std::clamp(focus+delta,0,4);else if(page==Page::System || page==Page::Display)focus=0;}
    void enter(){if(page==Page::Settings){page=section==0?Page::Appearance:section==1?Page::Power:section==2?Page::LibraryOptions:section==3?Page::Sound:section==5?Page::System:section==6?Page::Controls:section==7?Page::Statistics:Page::Network;focus=0;}else if(page==Page::Appearance && focus==5){page=Page::Display;focus=0;}}
};
inline SystemPreferences load_system_preferences(const fs::path& path) {
    SystemPreferences p;std::ifstream file(path);if(!file)throw std::runtime_error("Cannot read system preferences");
    std::map<std::string,std::string> values;std::string line;
    while(std::getline(file,line)) {
        if(!line.empty() && line.back()=='\r')line.pop_back();
        if(line.empty() || line[0]=='#')continue;
        auto separator=line.find('=');if(separator==std::string::npos)throw std::runtime_error("Invalid system preferences");
        if(!values.emplace(line.substr(0,separator),line.substr(separator+1)).second)throw std::runtime_error("Duplicate system preference");
    }
    auto number=[&](const std::string& key,int low,int high){
        auto value=values.at(key);size_t end=0;int n=std::stoi(value,&end);
        if(end!=value.size() || n<low || n>high)throw std::runtime_error("Invalid system preference: "+key);return n;
    };
    if(values.size()<5 || values.size()>9 || values.at("schema")!="1")throw std::runtime_error("Invalid system preferences schema");
    for(auto& pair:values)if(pair.first!="schema" && pair.first!="power" && pair.first!="brightness" && pair.first!="idle" && pair.first!="lid" && pair.first!="battery_percent" && pair.first!="timezone" && pair.first!="date_format" && pair.first!="time_format")throw std::runtime_error("Unknown system preference");
    p.power=number("power",0,3);p.brightness=number("brightness",5,100);p.idle=number("idle",0,4);p.lid=number("lid",0,1);
    if(values.count("battery_percent"))p.battery_percent=number("battery_percent",0,1);
    if(values.count("date_format"))p.date_format=number("date_format",0,int(date_format_names.size())-1);
    if(values.count("time_format"))p.time_format=number("time_format",0,1);
    if(values.count("timezone")){p.timezone=values.at("timezone");if(std::find(timezones.begin(),timezones.end(),p.timezone)==timezones.end())throw std::runtime_error("Invalid timezone");}return p;
}
inline void save_system_preferences(const fs::path& path,SystemPreferences p) {
    if(!path.parent_path().empty())fs::create_directories(path.parent_path());auto pending=path.string()+".tmp";
    std::ofstream file(pending);file<<"schema=1\npower="<<p.power<<"\nbrightness="<<p.brightness<<"\nidle="<<p.idle<<"\nlid="<<p.lid<<'\n';
    file<<"battery_percent="<<p.battery_percent<<"\ntimezone="<<p.timezone<<'\n';
    if(p.date_format)file<<"date_format="<<p.date_format<<'\n';
    if(p.time_format)file<<"time_format="<<p.time_format<<'\n';
    file.close();if(!file)throw std::runtime_error("Cannot save system preferences");fs::rename(pending,path);
}
// The additive UI choices (ui.ini) next to the base file. Kept apart so an
// older release can still read the base file, and so a corrupt base file does
// not discard them either.
inline void load_ui_preferences(const fs::path& ui,Preferences& result) {
    std::string line;
    std::ifstream extra(ui);
    while(std::getline(extra,line)) {
        if(line=="animations=0" || line=="animations=1")result.animations=line.back()=='1';
        else if(line=="view_animations=0" || line=="view_animations=1")result.view_animations=line.back()=='1';
        else if(line=="view_transition_style=spring" || line=="view_transition_style=push" || line=="view_transition_style=zoom")result.view_style=line=="view_transition_style=push"?ViewStyle::Push:line=="view_transition_style=zoom"?ViewStyle::Zoom:ViewStyle::Spring;
        else if(line=="view_transition_speed=snappy" || line=="view_transition_speed=normal" || line=="view_transition_speed=relaxed")result.view_speed=line=="view_transition_speed=snappy"?0:line=="view_transition_speed=relaxed"?2:1;
        else if(line=="menu_sounds=0" || line=="menu_sounds=1")result.menu_sounds=line.back()=='1';
        else if(line.rfind("sound_volume=",0)==0){try{size_t end=0;int value=std::stoi(line.substr(13),&end);if(end==line.size()-13 && value>=0 && value<=100)result.sound_volume=value;}catch(const std::exception&){} }
        else if(line=="touch_indicator=0" || line=="touch_indicator=1")result.touch_indicator=line.back()=='1';
        else if(filter_field(line,result.filter)){}
        else if(line=="view=0" || line=="view=1" || line=="view=2")result.view=line.back()-'0';
        else if(line=="drastic_backend=0" || line=="drastic_backend=1")result.drastic_backend=line.back()-'0';
        else if(line=="icon_style=pixel" || line=="icon_style=rich")result.rich_icons=line=="icon_style=rich";
        else if(line=="high_contrast=0" || line=="high_contrast=1")result.high_contrast=line.back()=='1';
        else if(line.rfind("accent=",0)==0){try{size_t end=0;int value=std::stoi(line.substr(7),&end);if(end==line.size()-7 && value>=-1 && value<16)result.accent=value;}catch(const std::exception&){} }
        else if(line.rfind("language=",0)==0)result.language=line.substr(9);
        else if(line=="layout=0" || line=="layout=1" || line=="layout=2")result.layout=line.back()-'0';
        else if(line=="home_layout=carousel" || line=="home_layout=vertical")result.home_layout=line=="home_layout=vertical";
        else if(line=="background=plain" || line=="background=grid")result.background=line=="background=grid";
        else if(line=="gestures=0" || line=="gestures=1")result.gestures=line.back()=='1';
        else if(line=="menu_music=0" || line=="menu_music=1")result.menu_music=line.back()=='1';
        else if(line.rfind("music_volume=",0)==0){try{size_t end=0;int value=std::stoi(line.substr(13),&end);if(end==line.size()-13 && value>=0 && value<=100)result.music_volume=value;}catch(const std::exception&){} }
        else if(line=="artwork_provider=0" || line=="artwork_provider=1" || line=="artwork_provider=2" || line=="artwork_provider=3")result.artwork_provider=line.back()=='3'?0:line.back()-'0';
    }
}
inline Preferences load_preferences(const fs::path& path) {
    Preferences result;std::string line;
    // The additive ui.ini is read even when the base file is absent: a fresh
    // card may carry only the UI choices (tests and development previews too).
    if(fs::exists(path)){
    std::ifstream file(path);if(!file)throw std::runtime_error("Cannot read preferences");
    bool schema=false,mode=false,font=false;
    while(std::getline(file,line)) {
        if(!line.empty() && line.back()=='\r')line.pop_back();
        if(line=="schema=1")schema=true;
        else if(line=="mode=light" || line=="mode=dark"){mode=true;result.dark=line=="mode=dark";}
        else if(line=="font=rounded" || line=="font=pixel"){font=true;result.pixel=line=="font=pixel";}
        else if(line.rfind("core=",0)==0){
            unsigned value=0;auto parsed=std::from_chars(line.data()+5,line.data()+line.size(),value);
            if(parsed.ec==std::errc{} && parsed.ptr==line.data()+line.size() && value<unsigned(core_count))result.core=unsigned(offered_core(int(value)));
            // A newer release may know another engine. Keep the default engine
            // in that case, without discarding unrelated appearance/UI choices.
        }
        else if(line=="auto_load=0" || line=="auto_load=1")result.auto_load=line.back()=='1';
        else if(line=="auto_save=0" || line=="auto_save=1")result.auto_save=line.back()=='1';
        else if(line=="animations=0" || line=="animations=1")result.animations=line.back()=='1';
        else if(line=="view_animations=0" || line=="view_animations=1")result.view_animations=line.back()=='1';
        else if(line=="view_transition_style=spring" || line=="view_transition_style=push" || line=="view_transition_style=zoom")result.view_style=line=="view_transition_style=push"?ViewStyle::Push:line=="view_transition_style=zoom"?ViewStyle::Zoom:ViewStyle::Spring;
        else if(line=="view_transition_speed=snappy" || line=="view_transition_speed=normal" || line=="view_transition_speed=relaxed")result.view_speed=line=="view_transition_speed=snappy"?0:line=="view_transition_speed=relaxed"?2:1;
        else if(line=="menu_sounds=0" || line=="menu_sounds=1")result.menu_sounds=line.back()=='1';
        else if(line.rfind("sound_volume=",0)==0){try{size_t end=0;int value=std::stoi(line.substr(13),&end);if(end==line.size()-13 && value>=0 && value<=100)result.sound_volume=value;}catch(const std::exception&){} }
        else if(line=="touch_indicator=0" || line=="touch_indicator=1")result.touch_indicator=line.back()=='1';
        else if(filter_field(line,result.filter)){}
        else if(line=="view=0" || line=="view=1" || line=="view=2")result.view=line.back()-'0';
        else if(!line.empty() && line[0]!='#')throw std::runtime_error("Invalid appearance preferences");
    }
    if(!schema || !mode || !font)throw std::runtime_error("Incomplete appearance preferences");
    }
    load_ui_preferences(path.parent_path()/"ui.ini",result);
    return result;
}
inline void save_preferences(const fs::path& path,Preferences p) {
    if(!path.parent_path().empty())fs::create_directories(path.parent_path());auto pending=path.string()+".tmp";
    std::ofstream file(pending);file<<"schema=1\nmode="<<(p.dark?"dark":"light")<<"\nfont="<<(p.pixel?"pixel":"rounded")<<'\n';
    file<<"core="<<p.core<<'\n';
    file<<"auto_load="<<p.auto_load<<"\nauto_save="<<p.auto_save<<'\n';
    file.close();if(!file)throw std::runtime_error("Cannot save appearance preferences");
    fs::rename(pending,path);
    auto ui=path.parent_path()/"ui.ini",draft=fs::path(ui.string()+".pending");std::ofstream extra(draft);
    extra<<"schema=1\nmenu_sounds="<<p.menu_sounds<<"\nsound_volume="<<p.sound_volume<<"\nanimations="<<p.animations<<"\nview_animations="<<p.view_animations<<"\ntouch_indicator="<<p.touch_indicator<<"\nfilter="<<p.filter<<"\nview="<<p.view<<"\nlayout="<<p.layout<<"\nartwork_provider="<<p.artwork_provider<<"\ndrastic_backend="<<p.drastic_backend<<'\n';extra<<"icon_style="<<(p.rich_icons?"rich":"pixel")<<'\n';extra<<"accent="<<p.accent<<'\n';extra<<"high_contrast="<<p.high_contrast<<'\n';extra<<"language="<<p.language<<'\n';extra<<"view_transition_style="<<view_style_name(p.view_style)<<'\n';extra<<"view_transition_speed="<<(p.view_speed==0?"snappy":p.view_speed==2?"relaxed":"normal")<<'\n';
    extra<<"home_layout="<<(p.home_layout==1?"vertical":"carousel")<<"\nbackground="<<(p.background==1?"grid":"plain")<<"\ngestures="<<p.gestures<<'\n';extra<<"menu_music="<<p.menu_music<<"\nmusic_volume="<<p.music_volume<<'\n';extra.close();
    if(!extra)throw std::runtime_error("Cannot save UI preferences");fs::rename(draft,ui);
}
inline bool dark_theme_file(const fs::path& path) {
    std::ifstream file(path);std::string line;
    while(std::getline(file,line)){if(!line.empty() && line.back()=='\r')line.pop_back();if(line=="mode=dark")return true;if(line=="mode=light")return false;}
    return path.stem()=="dark";
}
}
