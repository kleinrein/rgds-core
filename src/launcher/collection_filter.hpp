#pragma once
#include "launcher/library_cache.hpp"
#include <atomic>
#include <future>
#include <memory>
#include <optional>

namespace rgds {
// One cancellable reader at a time. Slow cards never hold the render thread
// while Favourites/Recently played inspect thousands of small records.
class CollectionFilter {
public:
    struct Result {unsigned generation;std::vector<Game> games;};
private:
    std::future<Result> worker;
    std::shared_ptr<std::atomic<bool>> cancelled;
    struct Request {fs::path data;std::vector<Game> games;int view;unsigned generation;std::string query;int filter;};
    std::optional<Request> queued;
    unsigned generation=0;
public:
    ~CollectionFilter(){cancel();}
    void cancel(){++generation;if(cancelled)*cancelled=true;queued.reset();}
    void request(const fs::path& data,const std::vector<Game>& games,int view,const std::string& query="",int filter=0){cancel();queued=Request{data,games,view,generation,query,filter};}
    bool pending()const{return bool(queued) || worker.valid();}
    std::optional<Result> poll(){
        std::optional<Result> result;
        if(worker.valid()){
            if(worker.wait_for(std::chrono::seconds(0))!=std::future_status::ready)return {};
            auto value=worker.get();if(value.generation==generation)result=std::move(value);
        }
        if(queued){
            auto request=std::move(*queued);queued.reset();cancelled=std::make_shared<std::atomic<bool>>(false);
            worker=std::async(std::launch::async,[request=std::move(request),stop=cancelled]() mutable {
                Result result{request.generation,{}};LibraryCache cache(request.data,1);
                std::vector<std::pair<Game,std::string>> recent;
                auto fold=[](std::string value){for(size_t i=0;i<value.size();i++){auto c=static_cast<unsigned char>(value[i]);if(c<128)value[i]=char(std::tolower(c));else if(c==0xc3 && i+1<value.size()){auto d=static_cast<unsigned char>(value[i+1]);if(d>=0x80 && d<=0x9e && d!=0x97)value[++i]=char(d+32);}}return value;};
                auto query=fold(request.query);
                for(auto& game:request.games){
                    if(*stop)return result;
                    if(!query.empty()){auto& entry=cache.metadata(game);auto title=game.title+" "+entry.info["title"]+" "+entry.info["artwork_title"]+" "+entry.banner["title"];if(fold(title).find(query)==std::string::npos)continue;}
                    if(request.filter==1 || request.filter==2){auto stats=cache.stats(game);bool played=stats.sessions || stats.seconds || !stats.last_played.empty();if(played!=(request.filter==2))continue;}
                    if(request.filter==3 && fs::is_regular_file(request.data/"library"/game_key(game.path)/"box.bmp"))continue;
                    if(request.view==0)result.games.push_back(std::move(game));
                    else if(request.view==1){if(cache.options(game).favourite)result.games.push_back(std::move(game));}
                    else{auto date=cache.stats(game).last_played;if(!date.empty())recent.emplace_back(std::move(game),std::move(date));}
                }
                if(request.view==2){std::stable_sort(recent.begin(),recent.end(),[](const auto& a,const auto& b){return a.second>b.second;});for(auto& entry:recent)result.games.push_back(std::move(entry.first));}
                return result;
            });
        }
        return result;
    }
};
}
