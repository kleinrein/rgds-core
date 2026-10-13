#pragma once
#include "launcher/model.hpp"
#include "platform/sdl_dynamic.hpp"
#include <cmath>
#include <initializer_list>
#include <string>

namespace rgds {
enum class SettingsIcon { Display, Power, Cartridge, Volume, Wifi, System, Controller,
    Sun, Moon, Type, Spark, Shader, Heart, Save, Load, Play, Home, Refresh, Back,
    Chevron, Clock, Info, Key, Art, Check, Touch, Battery, Sleep, Trash, Logout, Fast, Sliders, Shutdown, Library, Restart };

inline SettingsIcon settings_icon_for(const std::string& title) {
    if(title=="Network file sharing")return SettingsIcon::Wifi;
    if(title=="Sharing details")return SettingsIcon::Info;
    if(title=="Reset state")return SettingsIcon::Restart;
    if(title=="Save gallery" || title=="Back up all saves")return SettingsIcon::Save;
    if(title=="Blow into microphone" || title=="Real microphone" || title=="Test microphone")return SettingsIcon::Volume;
    if(title=="DS controls" || title=="Close and open DS lid")return SettingsIcon::Controller;
    if(title=="DS language" || title=="Filter" || title=="Automatic updates")return SettingsIcon::Sliders;
    if(title=="Search games" || title=="Search and filters")return SettingsIcon::Library;
    if(title=="Where to put games" || title=="Add your games" || title=="Scan for games")return SettingsIcon::Cartridge;
    if(title=="Library")return SettingsIcon::Library;
    if(title=="Statistics")return SettingsIcon::Clock;
    if(title=="Power off")return SettingsIcon::Shutdown;
    if(title=="Profile" || title=="Shortcut action" || title=="Use global controls")return SettingsIcon::Controller;
    if(title=="Restore defaults")return SettingsIcon::Refresh;
    if(title=="DS rounded" || title=="Pixel classic")return SettingsIcon::Type;
    if(title.find("brightness")!=std::string::npos || title=="Both screens" || title=="Light")return SettingsIcon::Sun;
    if(title=="Dark")return SettingsIcon::Moon;
    if(title.find("Theme")!=std::string::npos || title.find("display")!=std::string::npos)return SettingsIcon::Display;
    if(title.find("Controls")!=std::string::npos || title.find("Button")!=std::string::npos || title.find("shortcut")!=std::string::npos || title.find("Remap")!=std::string::npos)return SettingsIcon::Controller;
    if(title.find("Power")!=std::string::npos || title.find("power")!=std::string::npos)return SettingsIcon::Power;
    if(title.find("Battery")!=std::string::npos)return SettingsIcon::Battery;
    if(title.find("Sleep")!=std::string::npos || title.find("sleep")!=std::string::npos)return SettingsIcon::Sleep;
    if(title=="Preview sound")return SettingsIcon::Play;
    if(title=="Menu sounds" || title=="Effects volume")return SettingsIcon::Volume;
    if(title.find("Sound")!=std::string::npos || title.find("Volume")!=std::string::npos || title.find("Mute")!=std::string::npos || title.find("mute")!=std::string::npos)return SettingsIcon::Volume;
    if(title.find("Shader")!=std::string::npos || title.find("shader")!=std::string::npos)return SettingsIcon::Shader;
    if(title.find("Library")!=std::string::npos || title.find("Emulator")!=std::string::npos || title.find("emulator")!=std::string::npos || title=="Game options")return SettingsIcon::Cartridge;
    if(title.find("Wi-Fi")!=std::string::npos || title.find("network")!=std::string::npos || title.find("Connectivity")!=std::string::npos)return SettingsIcon::Wifi;
    if(title.find("Timezone")!=std::string::npos || title.find("clock")!=std::string::npos)return SettingsIcon::Clock;
    if(title.find("Password")!=std::string::npos || title.find("Sign in")!=std::string::npos)return SettingsIcon::Key;
    if(title.find("Username")!=std::string::npos || title.find("information")!=std::string::npos || title.find("address")!=std::string::npos || title.find("report")!=std::string::npos || title.find("firmware")!=std::string::npos || title.find("Kernel")!=std::string::npos || title.find("build")!=std::string::npos)return SettingsIcon::Info;
    if(title.find("artwork")!=std::string::npos || title.find("Artwork")!=std::string::npos || title.find("ScreenScraper")!=std::string::npos)return SettingsIcon::Art;
    if(title.find("load")!=std::string::npos || title.find("Load")!=std::string::npos)return SettingsIcon::Load;
    if(title.find("save")!=std::string::npos || title.find("Save")!=std::string::npos)return SettingsIcon::Save;
    if(title.find("Favourite")!=std::string::npos)return SettingsIcon::Heart;
    if(title.find("Restart")!=std::string::npos || title.find("Refresh")!=std::string::npos || title.find("previous")!=std::string::npos || title.find("recovery")!=std::string::npos)return SettingsIcon::Refresh;
    if(title.find("Return")!=std::string::npos || title.find("home")!=std::string::npos || title.find("Boot")!=std::string::npos || title=="Startup")return SettingsIcon::Home;
    if(title.find("Resume")!=std::string::npos || title.find("playing")!=std::string::npos)return SettingsIcon::Play;
    if(title.find("Typeface")!=std::string::npos)return SettingsIcon::Type;
    if(title.find("Animations")!=std::string::npos)return SettingsIcon::Spark;
    if(title.find("Touch")!=std::string::npos)return SettingsIcon::Touch;
    if(title.find("Clear")!=std::string::npos || title.find("space")!=std::string::npos)return SettingsIcon::Trash;
    if(title.find("Sign out")!=std::string::npos)return SettingsIcon::Logout;
    if(title.find("Fast")!=std::string::npos)return SettingsIcon::Fast;
    if(title.find("3D")!=std::string::npos || title.find("Frameskip")!=std::string::npos)return SettingsIcon::Sliders;
    return SettingsIcon::System;
}

inline std::string settings_row_hint(const std::string& title) {
    if(title=="Profile")return "Where this layout applies";
    if(title=="Button layout")return "Start with a familiar layout";
    if(title=="Home shortcut")return "Your menu shortcut";
    if(title=="Shortcut action")return "What the shortcut opens";
    if(title=="Use global controls")return "Follow your default button layout";
    if(title=="Restore defaults")return "Return to the original button layout";
    if(title=="Power mode")return "Balance battery life and speed";
    if(title=="Menu idle sleep")return "Rest after a period of inactivity";
    if(title=="Sleep on lid close")return "Close the handheld to sleep";
    if(title=="Battery percentage")return "Show the charge in the status bar";
    if(title=="Default emulator")return "Used by games without an override";
    if(title=="Artwork & ScreenScraper")return "Box art, titles and game details";
    if(title=="Auto-load state")return "Continue where you left off";
    if(title=="Auto-save on exit")return "Keep a checkpoint when returning home";
    if(title=="Default shader")return "Preview and choose your screen style";
    if(title=="Favourite")return "Keep this game in Favourites";
    if(title=="Emulator")return "Choose the core for this cartridge";
    if(title=="Shader")return "A screen style for this cartridge";
    if(title=="Controls")return "Buttons and the game menu shortcut";
    if(title=="Timezone")return "Your local time and date";
    if(title=="Device information")return "Build, storage and device address";
    if(title=="Updates & recovery")return "Manage builds and temporary files";
    if(title=="Return to stock")return "Open the Anbernic menu";
    if(title=="Startup")return "Choose what opens when you power on";
    if(title=="Boot into")return "Your home screen after powering on";
    if(title=="Touch to continue")return "Show the welcome screen at startup";
    if(title=="Available networks")return "Find and connect to a nearby network";
    if(title=="Refresh connection")return "Check the current connection";
    if(title=="Use previous build")return "Restore the last installed RGDS build";
    if(title=="Clear temporary cache")return "Free space while keeping your saves";
    if(title=="Save device report")return "Save a report for troubleshooting";
    if(title=="3D resolution")return "Choose native or a sharper 3D image";
    if(title=="Threaded 3D")return "Use an additional rendering thread";
    if(title=="Frameskip")return "Reduce rendering work when needed";
    if(title=="Save & restart")return "Save your checkpoint and restart";
    return {};
}

inline void draw_settings_icon(Sdl& s,SDL_Renderer* renderer,SettingsIcon icon,int cx,int cy,int size,Color ink) {
    const int weight=std::max(1,size/13);
    auto x=[&](float value){return cx+int(std::round((value-12)*size/24));};
    auto y=[&](float value){return cy+int(std::round((value-12)*size/24));};
    s.SetRenderDrawColor(renderer,ink.r,ink.g,ink.b,255);
    auto line=[&](float ax,float ay,float bx,float by){
        const bool horizontal=std::abs(bx-ax)>=std::abs(by-ay);
        for(int i=0;i<weight;i++){int offset=i-weight/2;s.RenderDrawLine(renderer,x(ax)+(horizontal?0:offset),y(ay)+(horizontal?offset:0),x(bx)+(horizontal?0:offset),y(by)+(horizontal?offset:0));}
    };
    auto path=[&](std::initializer_list<std::pair<float,float>> points){auto a=points.begin();for(auto b=a+1;b!=points.end();++b){line(a->first,a->second,b->first,b->second);a=b;}};
    auto box=[&](float ax,float ay,float bx,float by){path({{ax,ay},{bx,ay},{bx,by},{ax,by},{ax,ay}});};
    auto circle=[&](float ox,float oy,float radius,float start=0,float end=360){for(int i=0;i<40;i++){float a=(start+(end-start)*i/40)*.0174532925f,b=(start+(end-start)*(i+1)/40)*.0174532925f;line(ox+std::cos(a)*radius,oy+std::sin(a)*radius,ox+std::cos(b)*radius,oy+std::sin(b)*radius);}};
    switch(icon){
    case SettingsIcon::Display:box(4,2,20,10);box(4,14,20,22);line(9,18,15,18);break;
    case SettingsIcon::Library:case SettingsIcon::Cartridge:path({{5,3},{17,3},{20,6},{20,22},{4,22},{4,3},{5,3}});box(8,6,16,12);for(int i=7;i<18;i+=3)line(i,18,i,21);break;
    case SettingsIcon::Shutdown:case SettingsIcon::Power:circle(12,13,8,-45,225);line(12,2,12,13);break;
    case SettingsIcon::Volume:path({{3,9},{7,9},{12,5},{12,19},{7,15},{3,15},{3,9}});circle(12,12,6,-50,50);circle(12,12,10,-50,50);break;
    case SettingsIcon::Wifi:circle(12,21,17,235,305);circle(12,21,12,235,305);circle(12,21,7,235,305);circle(12,21,1);break;
    case SettingsIcon::System:box(4,4,20,20);for(int i=8;i<18;i+=4){line(i,1,i,4);line(i,20,i,23);line(1,i,4,i);line(20,i,23,i);}box(8,8,16,16);break;
    case SettingsIcon::Controller:path({{6,6},{18,6},{21,9},{23,19},{20,21},{15,17},{9,17},{4,21},{1,19},{3,9},{6,6}});line(5,11,11,11);line(8,8,8,14);circle(17,10,1);circle(20,13,1);break;
    case SettingsIcon::Sun:circle(12,12,5);for(int i=0;i<8;i++){float a=i*.78539816f;line(12+std::cos(a)*8,12+std::sin(a)*8,12+std::cos(a)*11,12+std::sin(a)*11);}break;
    case SettingsIcon::Moon:circle(12,12,9,20,280);circle(17,7,8,105,195);line(14,3,20,5);line(20,5,22,12);break;
    case SettingsIcon::Type:path({{3,20},{9,4},{15,20}});line(5,14,13,14);line(16,10,22,10);line(19,10,19,20);break;
    case SettingsIcon::Spark:path({{12,2},{15,9},{22,12},{15,15},{12,22},{9,15},{2,12},{9,9},{12,2}});break;
    case SettingsIcon::Shader:box(3,3,21,21);for(int i=8;i<20;i+=5){line(i,3,i,21);line(3,i,21,i);}circle(18,6,3);break;
    case SettingsIcon::Heart:path({{12,21},{3,13},{2,8},{5,4},{9,4},{12,7},{15,4},{19,4},{22,8},{21,13},{12,21}});break;
    case SettingsIcon::Save:path({{3,3},{17,3},{21,7},{21,21},{3,21},{3,3}});box(7,3,16,10);box(7,15,17,21);line(13,5,13,8);break;
    case SettingsIcon::Load:line(12,2,12,15);path({{7,10},{12,15},{17,10}});path({{3,15},{3,21},{21,21},{21,15}});break;
    case SettingsIcon::Play:path({{7,3},{21,12},{7,21},{7,3}});break;
    case SettingsIcon::Home:path({{2,11},{12,2},{22,11}});path({{5,9},{5,22},{19,22},{19,9}});box(10,15,14,22);break;
    case SettingsIcon::Restart:case SettingsIcon::Refresh:circle(12,12,8,-45,230);path({{15,4},{20,4},{20,9}});path({{9,20},{4,20},{4,15}});break;
    case SettingsIcon::Back:path({{10,5},{3,12},{10,19}});line(3,12,22,12);break;
    case SettingsIcon::Chevron:path({{8,4},{16,12},{8,20}});break;
    case SettingsIcon::Clock:circle(12,12,9);path({{12,6},{12,12},{17,15}});break;
    case SettingsIcon::Info:circle(12,12,9);line(12,11,12,18);circle(12,7,1);break;
    case SettingsIcon::Key:circle(8,8,5);path({{12,12},{22,22},{22,17},{19,17},{19,14},{16,14}});break;
    case SettingsIcon::Art:box(3,3,21,21);circle(16,8,2);path({{3,17},{9,10},{15,17},{18,13},{21,17}});break;
    case SettingsIcon::Check:path({{4,12},{9,17},{21,5}});break;
    case SettingsIcon::Touch:path({{9,19},{6,12},{8,11},{11,14},{11,4},{14,4},{14,11},{18,11},{20,13},{18,22},{11,22},{9,19}});circle(12,5,5,190,350);break;
    case SettingsIcon::Battery:box(3,6,20,18);line(23,10,23,14);for(int i=6;i<18;i+=4)line(i,9,i,15);break;
    case SettingsIcon::Sleep:path({{3,5},{9,5},{3,11},{9,11}});path({{12,12},{21,12},{12,21},{21,21}});break;
    case SettingsIcon::Trash:line(3,6,21,6);path({{6,6},{7,22},{17,22},{18,6}});path({{9,6},{9,2},{15,2},{15,6}});line(10,10,10,18);line(14,10,14,18);break;
    case SettingsIcon::Logout:path({{10,3},{3,3},{3,21},{10,21}});line(9,12,22,12);path({{17,7},{22,12},{17,17}});break;
    case SettingsIcon::Fast:path({{2,5},{11,12},{2,19},{2,5}});path({{13,5},{22,12},{13,19},{13,5}});break;
    case SettingsIcon::Sliders:for(int i=6;i<22;i+=6)line(3,i,21,i);circle(8,6,2);circle(17,12,2);circle(10,18,2);break;
    }
}
}
