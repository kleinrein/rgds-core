#pragma once
#include "launcher/asset_cache.hpp"
#include "launcher/game_options.hpp"

namespace rgds {
class LibraryCache {
public:
    struct Entry {
        fs::path folder;
        std::map<std::string,std::string> info,banner;
        GameOptions options;
        uint64_t seconds=0,sessions=0,used=0;
        std::string last_played,title;
        bool metadata_ready=false,stats_ready=false,options_ready=false,title_ready=false;
        AssetStamp info_stamp,banner_stamp,stats_stamp,options_stamp;
    };
private:
    fs::path data;
    size_t limit;
    uint64_t clock=0;
    std::map<std::string,Entry> entries;
    static std::string identity(const Game& game){return game.demo?"demo:"+game.title:game.path.string();}
    AssetStamp stamp(const fs::path& path){counters.stamp_checks++;return asset_stamp(path);}
    std::map<std::string,std::string> read(const fs::path& path,const AssetStamp& version){if(!version.present)return {};counters.file_reads++;return fields(path);}
    Entry& get(const Game& game) {
        counters.lookups++;auto key=identity(game);auto found=entries.find(key);
        if(found!=entries.end()){counters.hits++;found->second.used=++clock;return found->second;}
        if(entries.size()>=limit){auto oldest=std::min_element(entries.begin(),entries.end(),[](const auto& a,const auto& b){return a.second.used<b.second.used;});entries.erase(oldest);counters.evictions++;}
        Entry value;value.folder=data/"library"/game_key(game.demo?fs::path("demo")/game.title:game.path);value.used=++clock;
        return entries.emplace(std::move(key),std::move(value)).first->second;
    }
public:
    CacheCounters counters;
    explicit LibraryCache(fs::path root={},size_t maximum=128):data(std::move(root)),limit(std::max<size_t>(1,maximum)){}
    void reset(const fs::path& root){data=root;clear();}
    void clear(){counters.invalidations+=entries.size();entries.clear();}
    size_t size() const{return entries.size();}
    fs::path folder(const Game& game){return get(game).folder;}
    Entry& metadata(const Game& game) {
        auto& entry=get(game);if(!entry.metadata_ready){
            entry.info_stamp=stamp(entry.folder/"info.ini");entry.banner_stamp=stamp(entry.folder/"banner.ini");
            entry.info=read(entry.folder/"info.ini",entry.info_stamp);entry.banner=read(entry.folder/"banner.ini",entry.banner_stamp);entry.metadata_ready=true;entry.title_ready=false;
        }return entry;
    }
    Entry& stats(const Game& game,bool refresh=false) {
        auto& entry=get(game);auto path=entry.folder/"stats.ini";
        if(refresh && entry.stats_ready && stamp(path)!=entry.stats_stamp)entry.stats_ready=false;
        if(!entry.stats_ready){entry.stats_stamp=stamp(path);auto values=read(path,entry.stats_stamp);entry.seconds=entry.sessions=0;auto number=[&](const char* key){auto value=values[key];if(value.empty() || value.find_first_not_of("0123456789")!=std::string::npos)return uint64_t(0);try{return std::min<uint64_t>(std::stoull(value),3153600000ULL);}catch(...){return uint64_t(0);}};entry.seconds=number("seconds");entry.sessions=number("sessions");entry.last_played=values["last_played"];entry.stats_ready=true;}
        return entry;
    }
    GameOptions options(const Game& game,bool refresh=false) {
        auto& entry=get(game);auto path=entry.folder/"game.ini";
        if(refresh && entry.options_ready && stamp(path)!=entry.options_stamp)entry.options_ready=false;
        if(!entry.options_ready){entry.options_stamp=stamp(path);entry.options=GameOptions{};if(entry.options_stamp.present){counters.file_reads++;entry.options=load_game_options(path);}entry.options_ready=true;}
        return entry.options;
    }
    void saved_options(const Game& game,GameOptions value){auto& entry=get(game);entry.options=value;entry.options_ready=true;entry.options_stamp=stamp(entry.folder/"game.ini");}
    void invalidate_metadata(){for(auto& pair:entries){pair.second.metadata_ready=false;pair.second.title_ready=false;}counters.invalidations+=entries.size();}
    void invalidate_titles(){for(auto& pair:entries)pair.second.title_ready=false;}
    // Only the selected item is watched while idle; collection scans refresh
    // their lightweight options/stats once per item when a view is entered.
    bool refresh(const Game& game) {
        auto found=entries.find(identity(game));if(found==entries.end())return false;auto& entry=found->second;bool changed=false;
        if(entry.metadata_ready && (stamp(entry.folder/"info.ini")!=entry.info_stamp || stamp(entry.folder/"banner.ini")!=entry.banner_stamp)){entry.metadata_ready=false;entry.title_ready=false;changed=true;}
        if(entry.stats_ready && stamp(entry.folder/"stats.ini")!=entry.stats_stamp){entry.stats_ready=false;changed=true;}
        if(entry.options_ready && stamp(entry.folder/"game.ini")!=entry.options_stamp){entry.options_ready=false;changed=true;}
        if(changed)counters.invalidations++;return changed;
    }
};
}
