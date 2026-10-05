#pragma once
#include "launcher/model.hpp"
#include <array>

namespace rgds {
enum class Page { Library, Settings, Appearance, Placeholder, Power, Display, LibraryOptions, Scraper, Keyboard };
struct Preferences { bool dark=false, pixel=false;unsigned core=0; };
struct SystemPreferences {
    int power=0, brightness=60, idle=0;
    bool lid=false;
};
inline const std::array<const char*,4> power_names={"Stock","Battery saver","Balanced","Performance"};
inline const std::array<int,5> idle_minutes={0,1,2,5,10};
inline std::string idle_name(int index){return index==0?"Never":std::to_string(idle_minutes.at(index))+" min";}
struct Settings {
    Page page=Page::Library;
    int section=0, focus=0;
    void open(){page=Page::Settings;focus=0;}
    void back(){if(page==Page::Display){page=Page::Appearance;focus=2;}else{page=page==Page::Settings?Page::Library:page==Page::Scraper?Page::LibraryOptions:Page::Settings;focus=0;}}
    void move(int delta){if(page==Page::Settings)section=std::clamp(section+delta,0,5);else if(page==Page::Appearance || page==Page::Power)focus=std::clamp(focus+delta,0,3);else if(page==Page::LibraryOptions)focus=std::clamp(focus+delta,0,2);else if(page==Page::Scraper)focus=std::clamp(focus+delta,0,4);else if(page==Page::Display)focus=0;}
    void enter(){if(page==Page::Settings){page=section==0?Page::Appearance:section==1?Page::Power:section==2?Page::LibraryOptions:Page::Placeholder;focus=0;}else if(page==Page::Appearance && focus==2){page=Page::Display;focus=0;}}
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
    if(values.size()!=5 || values.at("schema")!="1")throw std::runtime_error("Invalid system preferences schema");
    p.power=number("power",0,3);p.brightness=number("brightness",5,100);p.idle=number("idle",0,4);p.lid=number("lid",0,1);return p;
}
inline void save_system_preferences(const fs::path& path,SystemPreferences p) {
    if(!path.parent_path().empty())fs::create_directories(path.parent_path());auto pending=path.string()+".tmp";
    std::ofstream file(pending);file<<"schema=1\npower="<<p.power<<"\nbrightness="<<p.brightness<<"\nidle="<<p.idle<<"\nlid="<<p.lid<<'\n';
    file.close();if(!file)throw std::runtime_error("Cannot save system preferences");fs::rename(pending,path);
}
inline Preferences load_preferences(const fs::path& path) {
    Preferences result;if(!fs::exists(path))return result;
    std::ifstream file(path);if(!file)throw std::runtime_error("Cannot read preferences");
    std::string line;bool schema=false,mode=false,font=false;
    while(std::getline(file,line)) {
        if(!line.empty() && line.back()=='\r')line.pop_back();
        if(line=="schema=1")schema=true;
        else if(line=="mode=light" || line=="mode=dark"){mode=true;result.dark=line=="mode=dark";}
        else if(line=="font=rounded" || line=="font=pixel"){font=true;result.pixel=line=="font=pixel";}
        else if(line=="core=0" || line=="core=1" || line=="core=2")result.core=static_cast<unsigned>(line.back()-'0');
        else if(!line.empty() && line[0]!='#')throw std::runtime_error("Invalid appearance preferences");
    }
    if(!schema || !mode || !font)throw std::runtime_error("Incomplete appearance preferences");
    return result;
}
inline void save_preferences(const fs::path& path,Preferences p) {
    if(!path.parent_path().empty())fs::create_directories(path.parent_path());auto pending=path.string()+".tmp";
    std::ofstream file(pending);file<<"schema=1\nmode="<<(p.dark?"dark":"light")<<"\nfont="<<(p.pixel?"pixel":"rounded")<<'\n';
    file<<"core="<<p.core<<'\n';
    file.close();if(!file)throw std::runtime_error("Cannot save appearance preferences");
    fs::rename(pending,path);
}
inline bool dark_theme_file(const fs::path& path) {
    std::ifstream file(path);std::string line;
    while(std::getline(file,line)){if(!line.empty() && line.back()=='\r')line.pop_back();if(line=="mode=dark")return true;if(line=="mode=light")return false;}
    return path.stem()=="dark";
}
}
