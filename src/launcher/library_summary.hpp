#pragma once
#include "launcher/collection_filter.hpp"
#include <array>

namespace rgds {
// Read historical records once off the UI thread, never from a draw callback.
class LibrarySummary {
public:
    struct Group {uint64_t games=0,played=0,sessions=0,seconds=0;std::string last_date,last_title;std::optional<Game> last_game;};
    struct Entry {std::string title;uint64_t seconds=0,sessions=0;Game game;};
    struct Result {unsigned generation=0;std::array<Group,3> groups{};std::vector<Entry> most_played;std::vector<Game> recent_games;};
private:
    struct Request {fs::path data;std::vector<Game> games;unsigned generation;};
    std::optional<Request> queued;
    std::future<Result> worker;
    std::shared_ptr<std::atomic<bool>> cancelled;
    unsigned generation=0;
public:
    ~LibrarySummary(){cancel();}
    void cancel(){++generation;if(cancelled)*cancelled=true;queued.reset();}
    void request(const fs::path& data,const std::vector<Game>& games){cancel();queued=Request{data,games,generation};}
    std::optional<Result> poll(){
        std::optional<Result> ready;
        if(worker.valid()){
            if(worker.wait_for(std::chrono::seconds(0))!=std::future_status::ready)return {};
            auto result=worker.get();if(result.generation==generation)ready=std::move(result);
        }
        if(queued){auto request=std::move(*queued);queued.reset();cancelled=std::make_shared<std::atomic<bool>>(false);
            worker=std::async(std::launch::async,[request=std::move(request),stop=cancelled]{
                Result result;result.generation=request.generation;LibraryCache cache(request.data,1);
                std::vector<std::pair<std::string,Game>> recent;
                for(const auto& game:request.games){
                    if(*stop)return result;
                    auto stats=cache.stats(game);bool favourite=cache.options(game).favourite;
                    if(!game.demo && !stats.last_played.empty()){
                        auto at=std::find_if(recent.begin(),recent.end(),[&](const auto& item){return stats.last_played>item.first;});
                        if(at!=recent.end() || recent.size()<8){recent.insert(at,{stats.last_played,game});if(recent.size()>8)recent.pop_back();}
                    }
                    if(stats.seconds || stats.sessions){
                        Entry entry{library_title(game.title),stats.seconds,stats.sessions,game};
                        auto before=[](const Entry& a,const Entry& b){return a.seconds!=b.seconds?a.seconds>b.seconds:a.sessions!=b.sessions?a.sessions>b.sessions:a.title<b.title;};
                        auto at=std::lower_bound(result.most_played.begin(),result.most_played.end(),entry,before);
                        if(at!=result.most_played.end() || result.most_played.size()<5){result.most_played.insert(at,std::move(entry));if(result.most_played.size()>5)result.most_played.pop_back();}
                    }
                    for(size_t i=0;i<3;i++){
                        if((i==1 && !favourite) || (i==2 && stats.last_played.empty()))continue;
                        auto& group=result.groups[i];group.games++;group.seconds+=stats.seconds;group.sessions+=stats.sessions;
                        if(stats.sessions || stats.seconds || !stats.last_played.empty())group.played++;
                        if(stats.last_played>group.last_date){group.last_date=stats.last_played;group.last_game=game;auto& info=cache.metadata(game);group.last_title=info.info["title"];if(group.last_title.empty())group.last_title=library_title(game.title);}
                    }
                }for(auto& entry:recent)result.recent_games.push_back(std::move(entry.second));return result;
            });
        }return ready;
    }
};
}
